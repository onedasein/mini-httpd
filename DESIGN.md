# mini-httpd

## 1.1  

这是一个单进程、单线程、非阻塞的HTTP/1.1静态服务器：把文档根下的文件，用HTTP协议正确地、并发地、可压测地发给客户端。 

## 1.2 用途

该项目是后端学习方向的一个探针项目。
第一阶段：用C写一个epoll边沿触发（ET）、全程非阻塞的HTTP/1.1静态文件服务器，
第二阶段：wrk压测+perf定位+优化。体验性能工程、火焰图。
第三阶段：Go小服务+tecd注册+优雅重启。体验分布式协调、工程效率。
其中第一阶段用C写完后，再用rust重写

在项目完成后，可以把自己的博客用所写的服务器托管在局域网。
优化后P99不劣于基线且QPS有可解释变化

## 1.3 判据

| # | 能力 | 判据（命令 + 期望） | 测在哪一层 |
|---|---|---|---|
| 1 | 监听、接受并发连接 | `wrk -t4 -c200 -d30s` → 无崩溃、Non-2xx = 0 | e2e/压测 |
| 2 | 请求行 + header 解析 | `printf 'GET / HTTP/1.1\r\nHost: x\r\n\r\n' \| nc -N …` → `HTTP/1.1 200 OK`；大小写混杂的 header 也认 | 单元 + e2e |
| 3 | 静态文件响应 | `curl -i /index.html` → 200 + `Content-Type`/`Content-Length` 正确 | 单元 + e2e |
| 4 | keep-alive（M2） | `curl -v /a /b` → 出现 `Re-using existing connection` | e2e |
| 5 | 非阻塞 + ET（M2） | 慢客户端占住时，其他客户端仍 200 | e2e |
| 6 | 空闲超时（M3） | `nc` 连上不动 → 5s 内被断开 | e2e |
| 7 | 路径安全 | `curl --path-as-is '/../Makefile'` → 400 且不泄漏内容 | 单元 + e2e |
| 8 | 无泄漏 | 压测前后 `ls /proc/<pid>/fd \| wc -l` 一致；ASan/LSan 全绿 | 体检 |
| 9 | 压测数据 | `results.tsv` 有行；README 有前后对比表 | 文档 |

## 1.4 不做清单

| 不做 | 理由 | 什么时候可能做 |
|---|---|---|
| HTTPS / TLS | 与"事件循环 + 协议"的学习目标无关，且需要另一套库 | 大概率永不 |
| POST / PUT / 请求体 | 静态服务器的核心是"读文件发出去"，body 会引入 chunked/长度边界 | 永不（这是静态服务器） |
| CGI / 动态内容 | 同上 | 永不 |
| 目录列表 | 需要 HTML 生成 + 转义，收益低 | 永不 |
| gzip | 需要 zlib + 内容协商，属于 M4 之后的优化 | 可选 |
| HTTP/2 | 需要帧层 + HPACK，是另一个项目 | 永不 |
| 多线程 / 多进程 | M2 的目标是"单线程事件循环"，多线程会掩盖 epoll 的学习 | 可选（对照实验） |
| Range 请求 | 需要额外的边界处理 | 可选 |

## 2.2 架构确认

通过"数据流"推导架构。
TCP 字节流 → （请求结构：method/target/版本/header）→ （文件：fd + 大小 + 类型）→ 响应字节流

| 模块 | 只做什么 | **不做**什么 | 依赖 | 头文件 |
|---|---|---|---|---|
| `main.c` | I/O 与生命周期：收连接、读、写、关 fd、信号、argv、启动 | 不解析协议；不知道 `\r\n`、不知道 `..` | http.h, file.h | — |
| `http.c` | 字节 ↔ 请求结构；响应头拼装；状态文本；MIME 表 | 不碰 fd/socket/文件；不决定 404 | httpd.h | `http.h` |
| `file.c` | URL 路径 → 根下真实文件；路径安全双保险 | 不懂 HTTP；不拼响应 | httpd.h | `file.h` |
| `httpd.h` | 全项目一份的上限常量与版本串 | 不含类型/函数/依赖 | — | — |
| `tests/unit/` + Unity | 进程内、毫秒级、直接调函数 | 不启动进程 | 被测模块头 | |
| `tests/e2e/` | 起进程、走网络、脚本比对 | 不做纯函数断言 | — | |

main.c 依赖http.h 和 file.h ，http.h 和 file.h共同依赖httpd.h

## 2.1 走查（覆盖度自查）

| 走查 | 怎么走 | 通过标准 |
|---|---|---|
| ① 一条正常请求 | 从字节到字节，逐环节问"这一步谁负责" | 每一环节都有唯一归属，没有"没人管"或"两人都管" |
| ② 错误码清单 | 每个状态码问"谁产生、谁映射成响应" | 产生者只报"哪一类错"，映射只有一处 |
| ③ **未来变更** | 问"M2 换 epoll、加 keep-alive、加空闲超时，各要动哪些文件" | 理想答案：**只动 main.c**；若连 http.c 都要动，说明分层错了 |

## 3.1 自查

用于代码评审
- main.c 里出现\r\n的解析，..的判断，404的判断逻辑
- http.h 里出现struct sockaddr/ int fd/ errno
- file.h 里出现#include "http.h"
出现以上任意一条，说明分层漏掉了。

## 3.3 契约表

C接口规范，契约模板， 契约清单（包含所有函数接口）

- C接口规范

| # | 规则 | 反例 |
|---|---|---|
| 1 | 模块前缀 + **动词优先**：`http_parse_request()`、`file_open_under_root()` | `httpReqParse`、`parseHttpRequest`（驼峰） |
| 2 | **长度紧跟指针**：`(const char *buf, size_t len)` | `(size_t len, const char *buf)` |
| 3 | **出参放最后**：`(in..., out*)` | 出参夹在中间 |
| 4 | 返回码统一：`enum` 0 = 成功；指针返回 `NULL`；`int` 用 `0/-1` | 一会儿 0/1、一会儿 -1/1 |
| 5 | **所有权进名字**：`_new` / `_dup` / `_free` / `_open` / `_close` | `http_parse()` 偷偷 malloc |
| 6 | **const 正确性**：只读入参一律 `const` | `char *buf` 但其实只读 |
| 7 | 头文件**不跨模块 include**；不在头里定义变量或函数体 | `http.h` 里 `#include "file.h"` |
| 8 | 类型命名统一：`http_header` / `static_file` / `enum http_err`（枚举要么全 typedef，要么全 tag） | 一半 typedef 一半 tag |

- 契约模板

```
前置条件  调用者必须保证什么（指针非空？len 范围？初始化？）
输入      每个参数是什么、什么单位、生命周期多长
输出      成功时写入了什么
失败      返回什么值；失败时**有没有留下已分配资源**
所有权    谁 malloc、谁 free、谁 close
是否修改入参   const 保证了什么
```

- 契约清单

先填契约表，再照着写声明

| 函数 | 模块 | 输入 | 输出 | 失败 | 所有权 |
|---|---|---|---|---|---|
| `http_scan_header_end` | http.c | `buf,len` + `scan*` | `1` + `head_len*` | 返回 `0`（数据不够） | 不分配 |
| `http_request_parse` | http.c | `head,head_len`（只读） | 填 `http_request` | `enum http_err` | 成功时 `req->path` 由调用者 free；失败不留垃圾 |
| `http_request_free` | http.c | `req*`（可 NULL） | 清零 | 无 | 释放 `path`，幂等 |
| `http_find_header` | http.c | `req, name` | `const http_header*` | `NULL` | 借用，不分配 |
| `http_status_text` | http.c | `status` | 文本 | 兜底 `"Error"` | 静态串 |
| `http_content_type` | http.c | `path`（NUL 结尾） | MIME 文本 | 兜底 `application/octet-stream` | 静态串 |
| `http_build_response_head` | http.c | `out,cap,status,ctype,body_len,extra` | 写入字节数 | `-1`（放不下，**不要发截断的头**） | 不分配 |
| `file_open_under_root` | file.c | `root_real, url_path, out*` | `FILE_OK` + 已打开 fd | `enum file_status`，**失败不留 fd** | 成功时 fd 由调用者 close |

## 5.2 用例表

三层测试各自完成什么任务

| 层 | 测什么 | 不测什么 | 速度 | 命令 |
|---|---|---|---|---|
| 单元（Unity） | 纯函数：解析、上限、MIME、路径判定 | 网络、进程、并发 | 毫秒 | `make test` |
| 冒烟 | "能不能通电"：能编、能链、框架能跑 | 业务正确性 | 毫秒 | `make test`（含在其中） |
| e2e | 起进程、走网络：状态码、头、字节数、并发形状 | 纯函数细节 | 秒 | `make e2e` |

### 单元测试写法

- 一个被测模块一个 `tests/unit/test_*.c`；`setUp`/`tearDown` 留空；
- **表驱动**：同一类逻辑多组输入输出 → 数组 + 循环，别写 14 个几乎一样的函数；
- 断言只表达"输入 → 期望"；**用例名用 ASCII**（Unity 会把非 ASCII 转义成 `\xNN`）；
- 边界四件套：空 / 刚好等于上限 / 超一个字节 / 非法字符；
- **被测函数不许 `exit()`**。

### 冒烟测试写法

1. `#include "unity.h"`
2. 空的 `void setUp(void) {}` / `void tearDown(void) {}`（Unity 要求这两个符号存在）
3. 1–2 个**表驱动**的用例，断言框架行为本身（例如"同一批输入，期望值等于实际值"）
4. `int main(void) { UNITY_BEGIN(); RUN_TEST(...); return UNITY_END(); }`
5. 注释里写清楚"这是测试链自检，第一个真用例落地后可以删"
**升级版**（等 `http.c` 落地后，把它改成"链接检查"）：
`TEST_ASSERT_NOT_NULL(http_status_text(200));` —— 顺带验证 `LIB_SRC` 的链接纪律（库里混进 `main` 就会 duplicate symbol）。

## 5.4 断言表

e2e 脚本 = **手工验收命令的可重复化 + 断言化**。
第一步永远是：**先手工验一遍，把命令原样抄下来。**

**七步流程**：

1. 手工敲一遍（curl / nc / printf 原始报文），把每条命令与期望结果抄进草稿；
2. 抽 4 类公共件：`srv_start` / `cleanup` / `chk` / 计数器；
3. 一行一条断言：`chk "PUT → 405" "$(code -X PUT $U/)" "405"`；
4. 加清理：`trap cleanup EXIT`（kill 服务 + 删临时目录 + 删写进文档根的测试素材）；
5. 加汇总：失败计数 → 末尾 `exit $fail`（CI/`make e2e` 靠退出码）；
6. **故意造一个失败**，确认脚本真的会红（这一条最常被跳过，也是"测试到底有没有用"的唯一证据）；
7. 提交。

**可照抄的骨架**（断言区已留白）：

```bash
#!/usr/bin/env bash
# tests/e2e/run.sh — 端到端验收：收二进制路径作为 $1，同一套断言在 debug/asan 上双跑
set -u
set -o pipefail                      # 不要 set -e：会中途退出，拿不到汇总

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)   # 注意：数对层级
BIN=${1:-$ROOT/build/debug/mini-httpd}
BIN=$(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")    # 绝对化，否则 cd 之后就找不到
[ -x "$BIN" ] || { echo "找不到可执行文件: $BIN（先 make debug）" >&2; exit 2; }

TMO=${TMO:-20}; PORT=${PORT:-8080}; WWW=$ROOT/tests/www
work=$(mktemp -d); SRV=""
cleanup() {
    [ -n "$SRV" ] && { kill -9 "$SRV" 2>/dev/null; wait "$SRV" 2>/dev/null; }
    rm -rf "$work"
    rm -f "$WWW/big.bin" "$WWW/leak-passwd"          # 测试素材必须清掉，别留在仓库里
}
trap cleanup EXIT

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
ok()  { printf 'PASS  %-40s %s\n' "$1" "${2:-}"; pass=$((pass+1)); }
bad() { printf 'FAIL  %-40s %s\n' "$1" "${2:-}"; fail=$((fail+1)); }
chk() { if [ "$2" = "$3" ]; then ok "$1" "$2"; else bad "$1" "期望[$3] 实际[$2]"; fi; }

U="http://127.0.0.1:$PORT"
code() { curl -s -o /dev/null -w '%{http_code}' --max-time "$TMO" "$@"; }

srv_start || { echo "服务端起不来" >&2; exit 2; }

# ---------------- 断言区：你填（一行一条，期望值写死） ----------------
chk "GET /index.html → 200" "$(code "$U/index.html")" "200"
# chk "PUT → 405"          "$(code -X PUT "$U/")" "405"
# ...

echo; echo "通过 $pass / 失败 $fail"
[ "$fail" -eq 0 ]
```

**断言从哪儿来**：

1. §1.3 能力清单 —— 每条能力 ≥ 1 条断言；
2. 状态码表（200/400/403/404/405/413/414/431/500/501/505）—— 每个码 ≥ 1 条；
3. 上下文里标了「必测项」的必须有（路径穿越、符号链接、10MB 完整性、无 fd 泄漏、SIGTERM 退出码）；
4. 每条断言必须**写死期望值**（不许 `A` 与 `B` 都是同一条命令的两种写法 —— 那是自证）。

**bash 陷阱清单**：

| 坑 | 后果 | 对策 |
|---|---|---|
| `set -e` | 中途退出，计数与汇总全丢 | 只 `set -u` + `set -o pipefail` |
| `BIN` 用相对路径 | `cd` 之后找不到二进制 | 绝对化 |
| `sleep 1` 猜就绪 | 慢机器上假失败，快机器上白等 | 轮询 `grep 'listening on'` |
| 端口被占 | 服务起不来，报一堆无关错 | `PORT` 可覆盖 + 起服务前 `ss -tlnp \| grep :$PORT` |
| `cmd \| head -1` | 管道提前关闭 → `cmd` 吃 SIGPIPE，输出可能丢 | 断言完整输出时别用 `head`；要状态码用 `curl -w '%{http_code}'` |
| 忘了 `--max-time` | 一条断言挂住整个套件 | 所有 curl 都带 `--max-time "$TMO"` |
| `nc` 是 OpenBSD 版 | `nc -l -p 80` 语法错；管道喂数据挂死 | `nc -l <ip> <port>`；管道必须 `-N` |
| 头里带 `\r` | 字符串比较永远不等 | 比较前 `tr -d '\r'` |
| 测试素材留在仓库 | 会被 `git add -A` 卷进提交 | `trap` 里删掉（或先写进 `.git/info/exclude`） |
| 临时文件放 `/tmp` | 本环境跨调用不保留，验证会假通过 | 临时件放 `$(mktemp -d)` 并在 trap 里删 |
| 断言失败只打印"FAIL" | 无法定位 | 打印 `期望[x] 实际[y]` |
| 一个脚本里混单元测试 | 慢、且失败原因不清 | 纯函数放 Unity；脚本只管"起进程 + 网络 + 比对" |
