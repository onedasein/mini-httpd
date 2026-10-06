#!/usr/bin/env bash
# tests/run.sh — warmups/httpd/v1（M1 阻塞式 HTTP）验收测试
#   bash tests/run.sh                          # 测 build/debug/httpd_v1
#   bash tests/run.sh ./build/asan/httpd_v1    # 用 ASan 版跑同一套
#
# 对应 TASKS.md：T1.2 请求行 / T1.3 header 与上限 / T1.4 静态文件与路径安全 /
#                T1.5 完整写（大文件） / T1.6 HTTP 语义细节
set -u
set -o pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BIN=${1:-$ROOT/build/debug/httpd_v1}
TMO=${TMO:-20}
PORT=${PORT:-8080}

BIN=$(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")
[ -x "$BIN" ] || { echo "找不到可执行文件: $BIN （先 make debug）" >&2; exit 2; }

work=$(mktemp -d) || exit 2
SRV=""
cleanup() { [ -n "$SRV" ] && { kill -9 "$SRV" 2>/dev/null; wait "$SRV" 2>/dev/null; }; rm -rf "$work"; }
trap cleanup EXIT

# 临时素材直接放进 www/，退出时删掉（别把 10MB 文件提交进仓库）
WWW=$ROOT/www
BIG=$WWW/big.bin
LINK_ABS=$WWW/leak-passwd
LINK_REL=$WWW/leak-makefile
LINK_OK=$WWW/alias-note.txt
extra_cleanup() { rm -f "$BIG" "$LINK_ABS" "$LINK_REL" "$LINK_OK"; }
trap 'cleanup; extra_cleanup' EXIT

pass=0; fail=0
ok()  { printf 'PASS  %-42s %s\n' "$1" "${2:-}"; pass=$((pass+1)); }
bad() { printf 'FAIL  %-42s %s\n' "$1" "${2:-}"; fail=$((fail+1)); }
chk() { if [ "$2" = "$3" ]; then ok "$1" "$2"; else bad "$1" "期望[$3] 实际[$2]"; fi; }

srv_start() {
    "$BIN" 127.0.0.1 "$PORT" "$WWW" >"$work/srv.log" 2>&1 &
    SRV=$!
    for _ in $(seq 1 60); do
        grep -q 'listening on' "$work/srv.log" 2>/dev/null && return 0
        kill -0 "$SRV" 2>/dev/null || { cat "$work/srv.log"; return 1; }
        sleep 0.05
    done
    return 1
}

U=http://127.0.0.1:$PORT
code()  { curl -s -o /dev/null -w '%{http_code}' --max-time "$TMO" "$@"; }
hdr()   { curl -s -I --max-time "$TMO" "$@" | tr -d '\r'; }
# 原始报文：printf 是 bash 内建，会按 C 转义解释 \r\n，可控且确定
raw()   { printf "$1" | timeout "$TMO" nc -N 127.0.0.1 "$PORT" | head -c 400 | tr -d '\r' | head -1; }
rawall(){ printf "$1" | timeout "$TMO" nc -N 127.0.0.1 "$PORT"; }

srv_start || { echo "服务端起不来" >&2; exit 2; }
echo "被测程序: $BIN"
echo "注意：v1 故意串行 —— 一个慢客户端就能拖死全服（这是 M2 的对照组，T2.2）"
echo

echo "---- T1.1/T1.2 请求行解析 + 响应 ----"
fds_before=$(ls /proc/"$SRV"/fd 2>/dev/null | wc -l)
chk "GET / → 200" "$(code $U/)" "200"
body=$(curl -s --max-time "$TMO" $U/)
if printf '%s' "$body" | grep -q 'mini-httpd v1'; then ok "body 是 www/index.html"; else bad "body 是 www/index.html" "$(printf '%s' "$body" | head -1)"; fi
if curl -s -i --max-time "$TMO" $U/ | grep -q $'\r$'; then ok "响应行是 CRLF 结尾"; else bad "响应行是 CRLF 结尾" "（可能用了裸 LF）"; fi

echo "---- T1.4 静态文件 + Content-Type ----"
chk "HEAD /index.html → 200" "$(code -I $U/index.html)" "200"
chk "  Content-Type" "$(hdr $U/index.html | grep -i '^content-type' | awk '{print $2}')" "text/html;"
chk "  Content-Length 与真实大小一致" \
    "$(hdr $U/index.html | grep -i '^content-length' | awk '{print $2}')" \
    "$(stat -c %s "$WWW/index.html")"
chk ".css" "$(hdr $U/style.css | grep -i '^content-type' | awk '{print $2}')" "text/css;"
chk ".js"  "$(hdr $U/app.js    | grep -i '^content-type' | awk '{print $2}')" "application/javascript;"
chk ".json" "$(hdr $U/data.json | grep -i '^content-type' | awk '{print $2}')" "application/json"
chk ".txt" "$(hdr $U/note.txt | grep -i '^content-type' | awk '{print $2}')" "text/plain;"
printf 'not a known extension\n' > "$WWW/blob.zzz"
chk "未知扩展名 → octet-stream" "$(hdr $U/blob.zzz | grep -i '^content-type' | awk '{print $2}')" "application/octet-stream"
rm -f "$WWW/blob.zzz"
chk "空文件 Content-Length: 0" "$(hdr $U/empty.txt | grep -i '^content-length' | awk '{print $2}')" "0"
chk "目录 /sub/ 补 index.html" "$(code $U/sub/)" "200"
chk "  /sub/ 内容正确" "$(curl -s --max-time "$TMO" $U/sub/ | grep -c '子目录')" "1"
chk "query 串被忽略" "$(code "$U/index.html?x=1&y=2")" "200"
chk "404" "$(code $U/nope)" "404"

echo "---- T1.4 路径安全（必测项）----"
trav=$(curl -s --path-as-is --max-time "$TMO" "$U/../Makefile")
chk "  /../Makefile → 400" "$(code --path-as-is "$U/../Makefile")" "400"
if printf '%s' "$trav" | grep -q 'CC'; then bad "  没泄漏 Makefile 内容" "泄漏了！"; else ok "  没泄漏 Makefile 内容"; fi
chk "  /%2e%2e%2fMakefile → 400" "$(code --path-as-is "$U/%2e%2e%2fMakefile")" "400"
chk "  /..%2fMakefile → 400" "$(code --path-as-is "$U/..%2fMakefile")" "400"
chk "  /%00 → 400" "$(code --path-as-is "$U/%00")" "400"
ln -sf /etc/passwd "$LINK_ABS"
ln -sf ../Makefile  "$LINK_REL"
ln -sf note.txt     "$LINK_OK"
chk "  符号链接逃逸(绝对) → 403" "$(code $U/$(basename $LINK_ABS))" "403"
chk "  符号链接逃逸(相对) → 403" "$(code $U/$(basename $LINK_REL))" "403"
chk "  www 内的正常符号链接 → 200" "$(code $U/$(basename $LINK_OK))" "200"
chk "  --path-as-is /../ → 400" "$(code --path-as-is "$U/../")" "400"
mkdir -p "$WWW/nolist"
printf 'inside\n' > "$WWW/nolist/a.txt"
chk "  目录无 index 不给列表 → 404" "$(code $U/nolist/)" "404"
chk "  不带斜杠的目录名 → 404" "$(code $U/nolist)" "404"
rm -rf "$WWW/nolist"

echo "---- T1.3 header 大小写不敏感 / 重复 / 上限 ----"
chk "hOsT 大小写不敏感" "$(raw 'GET / HTTP/1.1\r\nhOsT: x\r\n\r\n')" "HTTP/1.1 200 OK"
chk "cOnNeCtIoN: CLOSE 大小写不敏感" "$(raw 'GET / HTTP/1.1\r\nHost: x\r\ncOnNeCtIoN: CLOSE\r\n\r\n')" "HTTP/1.1 200 OK"
chk "重复同名 header 允许" "$(raw 'GET / HTTP/1.1\r\nHost: x\r\nX-Dup: 1\r\nX-Dup: 2\r\n\r\n')" "HTTP/1.1 200 OK"
chk "单行 20000 字节 → 431" "$(raw "GET / HTTP/1.1\r\nHost: x\r\nX: $(head -c 20000 /dev/zero | tr '\0' a)\r\n\r\n")" "HTTP/1.1 431 Request Header Fields Too Large"
chk "URI 2500 字节 → 414" "$(raw "GET /$(head -c 2500 /dev/zero | tr '\0' a) HTTP/1.1\r\nHost: x\r\n\r\n")" "HTTP/1.1 414 URI Too Long"
chk "缺 Host 的 HTTP/1.1 → 400" "$(raw 'GET / HTTP/1.1\r\n\r\n')" "HTTP/1.1 400 Bad Request"
chk "垃圾请求行 → 400" "$(raw 'GARBAGE\r\n\r\n')" "HTTP/1.1 400 Bad Request"
chk "HTTP/2.0 → 505" "$(raw 'GET / HTTP/2.0\r\nHost: x\r\n\r\n')" "HTTP/1.1 505 HTTP Version Not Supported"
# 总 header 量 70KB（很多短行，单行都不超限）→ 431
# 注意：服务端在 64KB 处就会回 431 并关连接，此时客户端可能还在写 → send 会 EPIPE。
#       用 nc 当客户端会因这个竞态偶发收不到响应，所以这里用「容忍 EPIPE 的 python 客户端」。
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
huge=$(python3 "$work/send_raw.py" "$PORT" < "$work/huge.req" | head -c 200 | tr -d '\r' | head -1)
chk "header 总量 70KB → 431" "$huge" "HTTP/1.1 431 Request Header Fields Too Large"

echo "---- T1.5 完整写：10MB 大文件 ----"
head -c 10485760 /dev/urandom > "$BIG"
curl -s --max-time 60 -o "$work/got.bin" "$U/big.bin"
if cmp -s "$BIG" "$work/got.bin"; then ok "10MB 传输与源文件 cmp 一致" "$(stat -c %s "$work/got.bin") 字节"
else bad "10MB 传输与源文件 cmp 一致" "收发不一致"; fi
chk "  大文件 Content-Length" "$(hdr $U/big.bin | grep -i '^content-length' | awk '{print $2}')" "10485760"

echo "---- T1.5b 慢速客户端：逼出部分写 / EAGAIN / POLLOUT 路径 ----"
slowread=$(python3 - "$PORT" <<'PY'
import socket, sys, time
s = socket.create_connection(('127.0.0.1', int(sys.argv[1])), timeout=60)
s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)   # 收缓冲调小，让服务端的 send 必然写不完
s.sendall(b'GET /big.bin HTTP/1.1\r\nHost: x\r\n\r\n')
buf = b''
while b'\r\n\r\n' not in buf:
    c = s.recv(4096)
    if not c: break
    buf += c
if b'\r\n\r\n' in buf:
    head, body = buf.split(b'\r\n\r\n', 1)
    clen = int([l.split(b':')[1] for l in head.split(b'\r\n')
                if l.lower().startswith(b'content-length')][0])
    while len(body) < clen:                # 故意每 256KB 停 10ms，制造背压
        c = s.recv(min(4096, clen - len(body)))
        if not c: break
        body += c
        if len(body) % 262144 < 4096:
            time.sleep(0.01)
    print(f"{len(body)}/{clen}")
else:
    print("no-header")
PY
)
chk "  慢速读 10MB 仍是完整响应体" "$slowread" "10485760/10485760"

echo "---- T1.5c 传输中途 RST：服务端不许死 ----"
python3 - "$PORT" <<'PY'
import socket, struct, sys, time
s = socket.create_connection(('127.0.0.1', int(sys.argv[1])), timeout=10)
s.sendall(b'GET /big.bin HTTP/1.1\r\nHost: x\r\n\r\n')
s.recv(8192)                                             # 只收一点点
s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
s.close()                                                # SO_LINGER=0 → 直接 RST
time.sleep(0.2)
PY
chk "  RST 之后服务端仍能正常服务" "$(code $U/index.html)" "200"

echo "---- T1.4b 短读：请求被拆成单个字节逐个到达 ----"
bytewise=$(python3 - "$PORT" <<'PY'
import socket, sys, time
req = b'GET / HTTP/1.1\r\nHost: x\r\n\r\n'
s = socket.create_connection(('127.0.0.1', int(sys.argv[1])), timeout=15)
for ch in req:
    s.sendall(bytes([ch]))                # 每次只发 1 字节，强制服务端多次 read
    time.sleep(0.02)
d = b''
while True:
    c = s.recv(4096)
    if not c: break
    d += c
print(d.split(b'\r\n')[0].decode())
PY
)
chk "  逐字节到达也能解析（增量找空行）" "$bytewise" "HTTP/1.1 200 OK"
rm -f "$BIG"

echo "---- T1.6 HTTP 语义细节 ----"
hdrcount=$(curl -s -i -X HEAD --max-time "$TMO" $U/index.html | wc -c)
if [ "$hdrcount" -lt 400 ]; then ok "HEAD 不发 body" "${hdrcount} 字节（只有头）"; else bad "HEAD 不发 body" "${hdrcount} 字节"; fi
chk "HEAD 的 Content-Length 仍是文件大小" \
    "$(hdr $U/index.html | grep -i '^content-length' | awk '{print $2}')" "$(stat -c %s "$WWW/index.html")"
chk "PUT → 405" "$(code -X PUT $U/)" "405"
chk "  405 带 Allow: GET, HEAD" "$(curl -s -I -X PUT --max-time "$TMO" $U/ | tr -d '\r' | grep -ci '^allow: get, head')" "1"
chk "DELETE → 405" "$(code -X DELETE $U/)" "405"
chk "HTTP/1.0 默认短连接 → Connection: close" "$(raw 'GET / HTTP/1.0\r\n\r\n' | head -1)" "HTTP/1.1 200 OK"
chk "  响应头里有 Connection: close" "$(hdr $U/ | grep -ci '^connection: close')" "1"
chk "Connection: close" "$(raw 'GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n')" "HTTP/1.1 200 OK"
chk "裸 LF 行尾也被宽容接受" "$(printf 'GET / HTTP/1.1\nHost: x\n\n' | timeout "$TMO" nc -N 127.0.0.1 "$PORT" | head -1 | tr -d '\r')" "HTTP/1.1 200 OK"

echo "---- 收尾：全程没有打死服务端 / 没有泄漏 fd ----"
# 刚跑完最后一条用例时，服务端可能还在 close 上一条连接 → 轮询等它回到基线。
# 真泄漏不会自己收敛，所以这样等既去掉竞态又不减弱断言。
fds_after=$fds_before
for _ in $(seq 1 30); do
    fds_after=$(ls /proc/"$SRV"/fd 2>/dev/null | wc -l)
    [ "$fds_after" = "$fds_before" ] && break
    sleep 0.1
done
chk "跑完全套后 fd 数量不变（无泄漏）" "$fds_after" "$fds_before"
if kill -0 "$SRV" 2>/dev/null; then ok "服务端仍存活（无越界/无崩溃）"; else bad "服务端仍存活" "它死了"; fi
errs=$(grep -ciE 'segmentation|AddressSanitizer|LeakSanitizer|runtime error|Assertion' "$work/srv.log" || true)
chk "  服务端日志无崩溃/ASan 报错" "$errs" "0"

echo "---- 优雅退出（LeakSanitizer/valgrind 只有在正常退出时才做退出检查）----"
kill -TERM "$SRV" 2>/dev/null
wait "$SRV" 2>/dev/null; rc=$?
SRV=""
chk "  SIGTERM → 正常退出 code=0" "$rc" "0"
if grep -q 'shutdown:' "$work/srv.log"; then ok "  日志有 shutdown 记录"; else bad "  日志有 shutdown 记录"; fi

echo "----------------"
printf '共 %d 条，通过 %d，失败 %d\n' "$((pass+fail))" "$pass" "$fail"
[ "$fail" -eq 0 ]
