#!/usr/bin/env bash
# tests/e2e/run.sh — 端到端验收（M1 阻塞版 → M2 epoll ET 版，同一套脚本）
#
#   bash tests/e2e/run.sh                        # 测 build/debug/mini-httpd
#   bash tests/e2e/run.sh ./build/asan/mini-httpd
#   make e2e / make e2e-asan
#
# 断言从哪来（每条都要能指出出处）：
#   · 状态码与语义 → CONTRACTS.md §1（200/400/403/404/405/413/414/431/505）
#   · 连接语义     → CONTRACTS.md §1b（M2/T2.6 起：1.1 默认 keep-alive、1.0 默认关）
#   · 语法决定     → docs/rfc/解析待定项.md（裸 LF 宽容、HTTP/1→400、小写 get→501…）、docs/adr/002
#   · **改脚本 = 改题目**：本文件是 warmups/httpd/v1/tests/run.sh 那 55 条的移植版，
#     语义断言一条都不许放宽；只有夹具位置（改用临时目录）与 v1 强绑定的字面量做了适配。
#     **M2/T2.6 的两处改动都记在上面 §1b 里**（不是"放宽"）：
#       ① `Connection: close` 那条断言拆成「显式 close」+「默认 keep-alive」两条；
#       ② 逐字节到达那条**显式加** `Connection: close` —— 它本来靠"服务端一律关连接"收尾，
#          现在 1.1 默认保持，不加就会读到超时（题目没变，变的是服务端语义）。
#
# 两条纪律：
#   1. 期望值**写死**，不许拿"另一条命令"当期望（那是自证）。
#   2. 夹具放临时目录，随 $work 一起删 —— 仓库里不留测试素材。
set -u
set -o pipefail                      # 不要 set -e：会中途退出，拿不到汇总

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
BIN=${1:-$ROOT/build/debug/mini-httpd}
BIN=$(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")     # 绝对化，否则 cd 之后就找不到
[ -x "$BIN" ] || { echo "找不到可执行文件: $BIN（先 make debug）" >&2; exit 2; }

TMO=${TMO:-20}
PORT=${PORT:-8080}
if command -v ss >/dev/null 2>&1 && ss -tln 2>/dev/null | grep -q ":${PORT} "; then
    echo "端口 $PORT 已被占用（PORT=xxxx 换一个）" >&2
    exit 2
fi

work=$(mktemp -d) || exit 2
WWW=$work/www                        # 文档根：临时目录（不碰仓库里的 tests/www）
SRV=""
cleanup() {
    [ -n "$SRV" ] && { kill -9 "$SRV" 2>/dev/null; wait "$SRV" 2>/dev/null; }
    rm -rf "$work"
}
trap cleanup EXIT

# ------------------------------- 夹具 -------------------------------
mkdir -p "$WWW/sub" "$WWW/nolist"
printf '<!doctype html>\n<h1>mini-httpd e2e</h1>\n' > "$WWW/index.html"
printf 'body { color: red }\n'            > "$WWW/style.css"
printf 'console.log(1)\n'                 > "$WWW/app.js"
printf '{"k":1}\n'                        > "$WWW/data.json"
printf 'note\n'                           > "$WWW/note.txt"
: > "$WWW/empty.txt"                                                  # 空文件 → Content-Length: 0
printf 'not a known extension\n'          > "$WWW/blob.zzz"
printf '<h1>子目录</h1>\n'                 > "$WWW/sub/index.html"
printf 'inside\n'                         > "$WWW/nolist/a.txt"
printf 'secret\n'                         > "$work/secret.txt"        # 文档根**外**的真实文件（给相对逃逸用）
ln -s /etc/passwd        "$WWW/link-abs"                              # 绝对逃逸 → 403
ln -s "$work/secret.txt" "$WWW/link-rel"                              # 相对逃逸（目标存在）→ 403
ln -s note.txt           "$WWW/link-ok"                               # 根内正常软链 → 200

# ------------------------------- 工具 -------------------------------
srv_start() {
    "$BIN" 127.0.0.1 "$PORT" "$WWW" >"$work/srv.log" 2>&1 &
    SRV=$!
    for _ in $(seq 1 60); do                        # 轮询就绪，不要 sleep 猜
        grep -q 'listening on' "$work/srv.log" 2>/dev/null && return 0
        kill -0 "$SRV" 2>/dev/null || { cat "$work/srv.log"; return 1; }
        sleep 0.05
    done
    return 1
}

pass=0; fail=0
ok()  { printf 'PASS  %-46s %s\n' "$1" "${2:-}"; pass=$((pass+1)); }
bad() { printf 'FAIL  %-46s %s\n' "$1" "${2:-}"; fail=$((fail+1)); }
chk() { if [ "$2" = "$3" ]; then ok "$1" "$2"; else bad "$1" "期望[$3] 实际[$2]"; fi; }

U="http://127.0.0.1:$PORT"
code() { curl -s -o /dev/null -w '%{http_code}' --max-time "$TMO" "$@"; }
hdr()  { curl -s -I --max-time "$TMO" "$@" | tr -d '\r'; }
# 原始报文：printf 是 bash 内建，会按 C 转义解释 \r\n，可控且确定
raw()   { printf "$1" | timeout "$TMO" nc -N 127.0.0.1 "$PORT" | head -c 400 | tr -d '\r' | head -1; }
rawall(){ printf "$1" | timeout "$TMO" nc -N 127.0.0.1 "$PORT"; }

srv_start || { echo "服务端起不来" >&2; exit 2; }
echo "被测程序: $BIN"
echo "文档根   : $WWW（夹具随脚本结束一起删）"
echo "注意：M2 起不再串行 —— 慢客户端拖不死别人（M 组就是这条，M1 的对照证据在 README）"
echo

echo "---- A. 请求行 + 响应（T1.1/T1.2）----"
fds_before=$(ls /proc/"$SRV"/fd 2>/dev/null | wc -l)
chk "GET / → 200" "$(code "$U/")" "200"
if curl -s --max-time "$TMO" "$U/" | grep -q 'mini-httpd e2e'; then
    ok "body 是文档根里的 index.html"
else
    bad "body 是文档根里的 index.html" "$(curl -s --max-time "$TMO" "$U/" | head -1)"
fi
if curl -s -i --max-time "$TMO" "$U/" | grep -q $'\r$'; then
    ok "响应行是 CRLF 结尾"
else
    bad "响应行是 CRLF 结尾" "（可能用了裸 LF）"
fi
chk "响应头有 Server:" "$(hdr "$U/" | grep -ci '^server:')" "1"

echo "---- B. 静态文件 + Content-Type（T1.4）----"
chk "HEAD /index.html → 200" "$(code -I "$U/index.html")" "200"
chk "  Content-Type" "$(hdr "$U/index.html" | grep -i '^content-type' | awk '{print $2}')" "text/html;"
chk "  Content-Length 与真实大小一致" \
    "$(hdr "$U/index.html" | grep -i '^content-length' | awk '{print $2}')" \
    "$(stat -c %s "$WWW/index.html")"
chk ".css"  "$(hdr "$U/style.css" | grep -i '^content-type' | awk '{print $2}')" "text/css;"
chk ".js"   "$(hdr "$U/app.js"    | grep -i '^content-type' | awk '{print $2}')" "application/javascript;"
chk ".json" "$(hdr "$U/data.json" | grep -i '^content-type' | awk '{print $2}')" "application/json"
chk ".txt"  "$(hdr "$U/note.txt"  | grep -i '^content-type' | awk '{print $2}')" "text/plain;"
chk "未知扩展名 → octet-stream" "$(hdr "$U/blob.zzz" | grep -i '^content-type' | awk '{print $2}')" "application/octet-stream"
chk "空文件 Content-Length: 0" "$(hdr "$U/empty.txt" | grep -i '^content-length' | awk '{print $2}')" "0"
chk "目录 /sub/ 补 index.html → 200" "$(code "$U/sub/")" "200"
chk "  /sub/ 内容正确" "$(curl -s --max-time "$TMO" "$U/sub/" | grep -c '子目录')" "1"
chk "query 串被忽略" "$(code "$U/index.html?x=1&y=2")" "200"
chk "404" "$(code "$U/nope")" "404"

echo "---- C. 路径安全（T1.4，必测项）----"
trav=$(curl -s --path-as-is --max-time "$TMO" "$U/../Makefile")
chk "  /../Makefile → 400" "$(code --path-as-is "$U/../Makefile")" "400"
if printf '%s' "$trav" | grep -q 'CC'; then bad "  没泄漏 Makefile 内容" "泄漏了！"; else ok "  没泄漏 Makefile 内容"; fi
chk "  /%2e%2e%2fMakefile → 400" "$(code --path-as-is "$U/%2e%2e%2fMakefile")" "400"
chk "  /..%2fMakefile → 400" "$(code --path-as-is "$U/..%2fMakefile")" "400"
chk "  /%00 → 400" "$(code --path-as-is "$U/%00")" "400"
chk "  符号链接逃逸（绝对）→ 403" "$(code "$U/link-abs")" "403"
chk "  符号链接逃逸（相对）→ 403" "$(code "$U/link-rel")" "403"
chk "  文档根内的正常软链 → 200" "$(code "$U/link-ok")" "200"
chk "  --path-as-is /../ → 400" "$(code --path-as-is "$U/../")" "400"
chk "  目录无 index 不给列表 → 404" "$(code "$U/nolist/")" "404"
chk "  不带斜杠的目录名 → 404" "$(code "$U/nolist")" "404"

echo "---- D. 字段：大小写 / 重复 / 上限（T1.3）----"
chk "hOsT 大小写不敏感" "$(raw 'GET / HTTP/1.1\r\nhOsT: x\r\n\r\n')" "HTTP/1.1 200 OK"
chk "cOnNeCtIoN: CLOSE 大小写不敏感" "$(raw 'GET / HTTP/1.1\r\nHost: x\r\ncOnNeCtIoN: CLOSE\r\n\r\n')" "HTTP/1.1 200 OK"
chk "重复同名 header 允许" "$(raw 'GET / HTTP/1.1\r\nHost: x\r\nX-Dup: 1\r\nX-Dup: 2\r\n\r\n')" "HTTP/1.1 200 OK"
chk "冒号前有空白 → 400" "$(raw 'GET / HTTP/1.1\r\nHost : x\r\n\r\n')" "HTTP/1.1 400 Bad Request"
chk "单行 20000 字节 → 431" \
    "$(raw "GET / HTTP/1.1\r\nHost: x\r\nX: $(head -c 20000 /dev/zero | tr '\0' a)\r\n\r\n")" \
    "HTTP/1.1 431 Request Header Fields Too Large"
chk "URI 2500 字节 → 414" \
    "$(raw "GET /$(head -c 2500 /dev/zero | tr '\0' a) HTTP/1.1\r\nHost: x\r\n\r\n")" \
    "HTTP/1.1 414 URI Too Long"
chk "Content-Length 超 MAX_BODY → 413" \
    "$(raw 'GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 2000000\r\n\r\n')" \
    "HTTP/1.1 413 Payload Too Large"
chk "缺 Host 的 HTTP/1.1 → 400" "$(raw 'GET / HTTP/1.1\r\n\r\n')" "HTTP/1.1 400 Bad Request"
chk "垃圾请求行 → 400" "$(raw 'GARBAGE\r\n\r\n')" "HTTP/1.1 400 Bad Request"
chk "HTTP/1（缺次版本）→ 400" "$(raw 'GET / HTTP/1\r\nHost: x\r\n\r\n')" "HTTP/1.1 400 Bad Request"
chk "HTTP/2.0 → 505" "$(raw 'GET / HTTP/2.0\r\nHost: x\r\n\r\n')" "HTTP/1.1 505 HTTP Version Not Supported"
chk "小写 get（不在已知方法清单）→ 501" "$(raw 'get / HTTP/1.1\r\nHost: x\r\n\r\n')" "HTTP/1.1 501 Not Implemented"
chk "FOOBAR（未知方法）→ 501" "$(raw 'FOOBAR / HTTP/1.1\r\nHost: x\r\n\r\n')" "HTTP/1.1 501 Not Implemented"

# 总 header 量 70KB（很多短行，单行都不超限）→ 431
# 用 nc 当客户端有竞态：服务端在 MAX_HEADER_LEN 处就早退并回 431，此时客户端还在写 → send 可能 EPIPE。
# 所以这里用「容忍 EPIPE 的 python 客户端」，写完再读响应。
cat > "$work/send_raw.py" <<'PYEOF'
import socket, sys
data = sys.stdin.buffer.read()
s = socket.create_connection(('127.0.0.1', int(sys.argv[1])), timeout=10)
try:
    s.sendall(data)
except OSError:
    pass                      # 服务端提前关闭是预期行为，不影响读响应
out = b''
while True:
    try:
        chunk = s.recv(65536)
    except OSError:
        break
    if not chunk:
        break
    out += chunk
sys.stdout.buffer.write(out)
PYEOF
python3 -c "
import sys
sys.stdout.buffer.write(b'GET / HTTP/1.1\r\nHost: x\r\n' + b''.join(b'X-%d: %s\r\n' % (i, b'a'*1000) for i in range(70)) + b'\r\n')" > "$work/huge.req"
chk "header 总量 70KB → 431" \
    "$(python3 "$work/send_raw.py" "$PORT" < "$work/huge.req" | head -c 200 | tr -d '\r' | head -1)" \
    "HTTP/1.1 431 Request Header Fields Too Large"

echo "---- E. 完整写：10MB 大文件（T1.5）----"
BIG=$WWW/big.bin
head -c 10485760 /dev/urandom > "$BIG"
curl -s --max-time 60 -o "$work/got.bin" "$U/big.bin"
if cmp -s "$BIG" "$work/got.bin"; then
    ok "10MB 传输与源文件 cmp 一致" "$(stat -c %s "$work/got.bin") 字节"
else
    bad "10MB 传输与源文件 cmp 一致" "收发不一致"
fi
chk "  大文件 Content-Length" "$(hdr "$U/big.bin" | grep -i '^content-length' | awk '{print $2}')" "10485760"

echo "---- F. 慢速客户端：逼出部分写（T1.5b）----"
slowread=$(python3 - "$PORT" <<'PY'
import socket, sys, time
s = socket.create_connection(('127.0.0.1', int(sys.argv[1])), timeout=60)
s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)   # 收缓冲调小，让服务端的 send 必然写不完
s.sendall(b'GET /big.bin HTTP/1.1\r\nHost: x\r\n\r\n')
buf = b''
while b'\r\n\r\n' not in buf:
    c = s.recv(4096)
    if not c:
        break
    buf += c
if b'\r\n\r\n' in buf:
    head, body = buf.split(b'\r\n\r\n', 1)
    clen = int([l.split(b':')[1] for l in head.split(b'\r\n')
                if l.lower().startswith(b'content-length')][0])
    while len(body) < clen:                # 故意每 256KB 停 10ms，制造背压
        c = s.recv(min(4096, clen - len(body)))
        if not c:
            break
        body += c
        if len(body) % 262144 < 4096:
            time.sleep(0.01)
    print(f"{len(body)}/{clen}")
else:
    print("no-header")
PY
)
chk "  慢速读 10MB 仍是完整响应体" "$slowread" "10485760/10485760"

echo "---- G. 传输中途 RST：服务端不许死（T1.5c）----"
python3 - "$PORT" <<'PY'
import socket, struct, sys, time
s = socket.create_connection(('127.0.0.1', int(sys.argv[1])), timeout=10)
s.sendall(b'GET /big.bin HTTP/1.1\r\nHost: x\r\n\r\n')
s.recv(8192)                                             # 只收一点点
s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
s.close()                                                # SO_LINGER=0 → 直接 RST
time.sleep(0.2)
PY
chk "  RST 之后服务端仍能正常服务" "$(code "$U/index.html")" "200"

echo "---- H. 短读：请求逐字节到达（T1.3d）----"
bytewise=$(python3 - "$PORT" <<'PY'
import socket, sys, time
# 显式 Connection: close：这条用例靠"响应发完服务端就关"收尾（M2 起 1.1 默认保持，不给就会读到超时）
req = b'GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n'
s = socket.create_connection(('127.0.0.1', int(sys.argv[1])), timeout=15)
for ch in req:
    s.sendall(bytes([ch]))                # 每次只发 1 字节，强制服务端多次 read
    time.sleep(0.02)
d = b''
while True:
    c = s.recv(4096)
    if not c:
        break
    d += c
print(d.split(b'\r\n')[0].decode())
PY
)
chk "  逐字节到达也能解析（增量找空行）" "$bytewise" "HTTP/1.1 200 OK"
rm -f "$BIG"

echo "---- I. HTTP 语义细节（T1.6）----"
hdrcount=$(curl -s -i -X HEAD --max-time "$TMO" "$U/index.html" | wc -c)
if [ "$hdrcount" -lt 400 ]; then ok "HEAD 不发 body" "${hdrcount} 字节（只有头）"; else bad "HEAD 不发 body" "${hdrcount} 字节"; fi
chk "HEAD 的 Content-Length 仍是文件大小" \
    "$(hdr "$U/index.html" | grep -i '^content-length' | awk '{print $2}')" \
    "$(stat -c %s "$WWW/index.html")"
chk "PUT → 405" "$(code -X PUT "$U/")" "405"
chk "  405 带 Allow: GET, HEAD" "$(curl -s -I -X PUT --max-time "$TMO" "$U/" | tr -d '\r' | grep -ci '^allow: get, head')" "1"
chk "DELETE → 405" "$(code -X DELETE "$U/")" "405"
chk "HTTP/1.0 也能服务（无 Host）" "$(raw 'GET / HTTP/1.0\r\n\r\n' | head -1)" "HTTP/1.1 200 OK"
chk "  显式 Connection: close → 响应头也是 close" \
    "$(curl -s -I -H 'Connection: close' --max-time "$TMO" "$U/" | tr -d '\r' | grep -ci '^connection: close')" "1"
chk "  默认（1.1 不带 Connection）→ keep-alive" \
    "$(hdr "$U/" | grep -ci '^connection: keep-alive')" "1"
chk "Connection: close 请求" "$(raw 'GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n')" "HTTP/1.1 200 OK"
chk "裸 LF 行尾也被宽容接受" \
    "$(printf 'GET / HTTP/1.1\nHost: x\n\n' | timeout "$TMO" nc -N 127.0.0.1 "$PORT" | head -1 | tr -d '\r')" \
    "HTTP/1.1 200 OK"

echo "---- L. keep-alive / 连接语义（T2.6，规则见 CONTRACTS.md §1b）----"
# 一条连接上连发多个请求，必须各自把 body 读干净（按 Content-Length），否则下一条响应的解析会被 body 串位
cat > "$work/h1.py" <<'PYEOF'
def read_response(s):
    """读一条完整响应（头 + 按 Content-Length 收完 body），返回 (状态码, 头字典)。"""
    buf = b''
    while b'\r\n\r\n' not in buf:
        c = s.recv(4096)
        if not c:
            return None, {}
        buf += c
    head, body = buf.split(b'\r\n\r\n', 1)
    fields = {}
    for line in head.split(b'\r\n')[1:]:
        if b':' in line:
            k, v = line.split(b':', 1)
            fields[k.strip().lower().decode()] = v.strip().decode()
    clen = int(fields.get('content-length', '0'))
    while len(body) < clen:
        c = s.recv(4096)
        if not c:
            break
        body += c
    return head.split(b' ')[1].decode(), fields
PYEOF

kareuse=$(python3 - "$PORT" "$work" <<'PY'
import socket, sys
sys.path.insert(0, sys.argv[2]); import h1
s = socket.create_connection(('127.0.0.1', int(sys.argv[1])), timeout=10)
codes = []
for _ in range(3):                        # 同一条连接上连发 3 个请求
    s.sendall(b'GET / HTTP/1.1\r\nHost: x\r\n\r\n')
    code, _ = h1.read_response(s)
    codes.append(code or '---')
s.close()
print(' '.join(codes))
PY
)
chk "同一连接上连发 3 个请求都 200" "$kareuse" "200 200 200"

kaclose=$(python3 - "$PORT" "$work" <<'PY'
import socket, sys
sys.path.insert(0, sys.argv[2]); import h1
s = socket.create_connection(('127.0.0.1', int(sys.argv[1])), timeout=10)
s.sendall(b'GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n')
code, fields = h1.read_response(s)
s.settimeout(3)
try:                                      # 服务端必须真的关：读完 body 之后应当收到 EOF
    tail = s.recv(100)
except Exception:
    tail = b'x'
if code == '200' and fields.get('connection') == 'close' and tail == b'':
    print('closed')
else:
    print('kept-alive(%s,%s,%r)' % (code, fields.get('connection'), tail))
PY
)
chk "Connection: close 的请求：头是 close 且连接被关" "$kaclose" "closed"

ka10=$(python3 - "$PORT" "$work" <<'PY'
import socket, sys
sys.path.insert(0, sys.argv[2]); import h1
s = socket.create_connection(('127.0.0.1', int(sys.argv[1])), timeout=10)
s.sendall(b'GET / HTTP/1.0\r\n\r\n')      # 1.0 且没要 keep-alive → 处理完就关
code, fields = h1.read_response(s)
s.settimeout(3)
try:
    tail = s.recv(100)
except Exception:
    tail = b'x'
print('closed' if (fields.get('connection') == 'close' and tail == b'') else 'kept-alive')
PY
)
chk "HTTP/1.0 默认短连接（处理完就关）" "$ka10" "closed"

ka10k=$(python3 - "$PORT" "$work" <<'PY'
import socket, sys
sys.path.insert(0, sys.argv[2]); import h1
s = socket.create_connection(('127.0.0.1', int(sys.argv[1])), timeout=10)
codes = []
for _ in range(2):                        # 1.0 + 显式要 keep-alive → 也能保持
    s.sendall(b'GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n')
    code, _ = h1.read_response(s)
    codes.append(code or '---')
s.close()
print(' '.join(codes))
PY
)
chk "HTTP/1.0 + Connection: keep-alive 也保持" "$ka10k" "200 200"

kapipeline=$(python3 - "$PORT" <<'PY'
import socket, sys, time
s = socket.create_connection(('127.0.0.1', int(sys.argv[1])), timeout=10)
# 流水线：两个请求**一次**发出去，服务端要在一次事件里连着处理（残余字节不能丢）
s.sendall(b'GET / HTTP/1.1\r\nHost: x\r\n\r\nGET /style.css HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n')
time.sleep(0.3)
d = b''
while True:
    c = s.recv(65536)
    if not c:
        break
    d += c
print(d.count(b'HTTP/1.1 200'))
PY
)
chk "流水线两个请求 → 两条响应" "$kapipeline" "2"

echo "---- M. 慢客户端拖不死别人（T2.3c 的靶子反过来；ET 版的核心验收）----"
( timeout 8 nc 127.0.0.1 "$PORT" >/dev/null 2>&1 ) &
SLOW=$!
sleep 0.3
chk "慢客户端连上不发数据时，别的客户端仍 200" "$(code "$U/")" "200"
chk "  再来 5 次也全是 200" \
    "$(for _ in 1 2 3 4 5; do code "$U/index.html"; printf ' '; done | tr -s ' ' | sed 's/ $//')" \
    "200 200 200 200 200"
kill "$SLOW" 2>/dev/null; wait "$SLOW" 2>/dev/null
sleep 0.3                                 # 让服务端把那条连接的 EOF 处理掉（J 组要数 fd）

echo "---- J. 收尾：服务端还活着 / 没有泄漏 fd（T3.4 的前身）----"
fds_after=$fds_before
for _ in $(seq 1 30); do                     # 刚跑完最后一条，可能还在 close 上一条连接 → 轮询等回基线
    fds_after=$(ls /proc/"$SRV"/fd 2>/dev/null | wc -l)
    [ "$fds_after" = "$fds_before" ] && break
    sleep 0.1
done
chk "跑完全套后 fd 数量不变（无泄漏）" "$fds_after" "$fds_before"
if kill -0 "$SRV" 2>/dev/null; then ok "服务端仍存活（无越界/无崩溃）"; else bad "服务端仍存活" "它死了"; fi
chk "  服务端日志无崩溃/ASan 报错" \
    "$(grep -ciE 'segmentation|AddressSanitizer|LeakSanitizer|runtime error|Assertion' "$work/srv.log" || true)" "0"

echo "---- K. 优雅退出（LeakSanitizer/valgrind 只在正常退出时才做退出检查）----"
kill -TERM "$SRV" 2>/dev/null
wait "$SRV" 2>/dev/null; rc=$?
SRV=""
chk "SIGTERM → 正常退出 code=0" "$rc" "0"
if grep -q 'shutdown:' "$work/srv.log"; then ok "  日志有 shutdown 记录"; else bad "  日志有 shutdown 记录"; fi

echo "----------------"
printf '共 %d 条，通过 %d，失败 %d\n' "$((pass + fail))" "$pass" "$fail"
[ "$fail" -eq 0 ]
