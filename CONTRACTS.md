# CONTRACTS.md — 契约真源

> **顺序纪律（2026-10-13 定）**：**契约表 → 测试用例表 → 实现**。
> 任何状态码/失败语义的改动，都先改本文（+ `docs/adr/`），再改 `src/*.h`，最后改实现与测试。
> 数字（`MAX_*`）的真源是 `src/httpd.h`（**代码是数字的唯一真源**）；本文只写名字与含义，不抄数字。

---

## 0. 已知方法清单

只影响状态码405/501的分流
- 已知方法清单：
{ACL, BASELINE-CONTROL, BIND, CHECKIN, CHECKOUT, CONNECT, COPY, DELETE, GET, HEAD, LABEL, LINK, LOCK, MERGE, MKACTIVITY, MKCALENDAR, MKCOL, MKREDIRECTREF, MKWORKSPACE, MOVE, OPTIONS, ORDERPATCH, PATCH, POST, PRI, PROPFIND, PROPPATCH, PUT, QUERY, REBIND, REPORT, SEARCH, TRACE, UNBIND, UNCHECKOUT, UNLINK, UNLOCK, UPDATE, UPDATEREDIRECTREF, VERSION-CONTROL}

- 白名单：
{GET, HEAD}

| 码 | 触发条件 | 谁产生 | 出处|
|---|---|---|---|
| 405 | method在已知方法清单里但是不在白名单中(除了{GET, HEAD}外的所有合法方法) | http.c| RFC 9110 §15.5.6 |
| 501 | method不在已知清单中(`get`、`Foo`、超长token) | http.c | RFC 9110 §15.6.2 |
## 1. 状态码表（语义真源）

| 码 | 触发条件 | 谁产生 | 出处 |
|---|---|---|---|
| 200 | 文件存在（目录请求补 `index.html`） | `file.c` → `main.c` | RFC 9110 §15.3.1 |
| 400 | 请求行类：HTTP/1.1缺`Host`; 三段之间连续多个空格；版本字段为空；行尾只有`\r`没有`\n` | `http.c` | RFC 9112 §2.2  §2.3 |
| 400 | targe与解码类:target非origi-form`(http://host/x、*)`；非法`%XX`、`%00`、 反斜杠、解码出控制字符；`..`穿越； | file.c | - |
| 400 | 字段类：版本字段语法不合；请求行语法错；重复`Content-Length`且值不同 | `http.c` | RFC 9112 §3 §6.3 |
| 403 | `realpath` 后越出文档根；`EACCES` | `file.c` | RFC 9110 §15.5.4 |
| 404 | 文件不存在；目录没有 `index.html` | `file.c` | RFC 9110 §15.5.5 |
| 405 | 方法合法但不在白名单（PUT / DELETE / …），响应必须带Allow: GET, HEAD | `http.c` 报 `HTTP_ERR_METHOD` → `main.c` 映射 | RFC 9110 §15.5.6、**ADR-002** |
| 413 | `Content-Length > MAX_BODY`（第一版不收 body） | `http.c` | RFC 9110 §15.5.14 |
| 414 | `request-target > MAX_PATH_LEN` | `http.c` | RFC 9110 §15.5.16 |
| 431 | 单行 > `MAX_LINE_LEN`；head 总量 > `MAX_HEADER_LEN`；字段数 > `MAX_HEADERS` | `main.c`（早退）+ `http.c` | RFC 9110 §15.5.23 |
| 500 | 内部分配失败（`HTTP_ERR_INTERNAL`） | `http.c` → `main.c` | RFC 9110 §15.6.1 |
| 501 | method为小写`get`; method超长(> `MAX_METHOD_LEN`)| http.c | RFC 9110 §15.6.2 |
| 503 | **连接表拿不到槽位**（fd ≥ `CONN_MAX_FD`，或连接对象 `calloc` 失败）→ 回 503 后关掉（M2 T2.1b） | `main.c` | RFC 9110 §15.6.4 |
| 505 | 版本形如 `HTTP/x.y` 但不受支持 | `http.c` | RFC 9110 §15.6.6 |
| **静默关闭**（一个字节都不回） | 请求没发完就 EOF / 读写出错 | `main.c` | — |

**不报错误码的情形**：`Content-Length:0`与没有该header的情况，正常处理，无错误码

---

## 1b. 连接语义（keep-alive 真源，M2/T2.6 起）

| 请求 | 响应里的 `Connection:` | 处理完连接怎么办 |
|---|---|---|
| HTTP/1.1，没写 `Connection` | `keep-alive` | **留着**接下一个请求 |
| HTTP/1.1，`Connection: close` | `close` | 响应写完就关 |
| HTTP/1.0，没写 `Connection` | `close` | 响应写完就关（1.0 默认短连接） |
| HTTP/1.0，`Connection: keep-alive` | `keep-alive` | 留着 |

**例外（强制关，即使客户端想留）**：
- 请求声明了 `Content-Length > 0`（第一版不读 body）→ 没被消费的 body 会被当成下一个请求，只能关；
- 解析失败的那些分支（400/405/413/414/431/501/505）→ 我们无法确定请求边界，一律关；
- 收尾（DRAIN）与连接表收缩等异常路径。

**实现位置**：判断在 `http.c`（`http_request_wants_close()`，协议知识不许漏进 `main.c`），
落地在 `main.c`（`serve_request` 里定 `conn.keep_alive`，写响应时交给 `http_build_response_head`）。

**已知简化**：`Connection` 的值按**整体**比较，不解析逗号列表（`close, foo` 这种会被当成没写）。
两个方向的误判都无害：1.1 少关一次，客户端自己会关；1.0 多关一次，客户端重连就是。
出处：RFC 9110 §9.3（`Connection`）、RFC 9112 §9.3、RFC 7230 §6.3。

> **M1 的旧语义是「一律 `Connection: close`」**（当时是刻意简化，e2e 有一条断言钉着它）。
> T2.6 起改成上表；e2e 那条断言随之拆成「显式 close / 默认 keep-alive」两条，`v1` 的其余断言不动。

---

## 2. 函数契约清单

每个函数必须能回答 6 件事：**前置条件 / 输入 / 输出 / 失败 / 所有权 / 是否修改入参**。

| 函数 | 模块 | 前置 | 输入 | 输出 | 失败 | 所有权 |
|---|---|---|---|---|---|---|
| `http_scan_header_end` | http.c | `*scan` 由调用方保存（首次传 0） | `buf,len`（累计字节）、`scan*` | 返回 `1` + `head_len*` | 返回 `0` = 数据不够 | 不分配 |
| `http_request_parse` | http.c | `len` 正好是 head 长度 | `head,len`（**只读**）、`req*` | `HTTP_OK` + 填满 `req` | `http_err`；**不留已分配内存** | 成功时 `req->path` 由调用方 `free`；其余字段借用 `head`（生命周期 ≤ 缓冲） |
| `http_request_free` | http.c | — | `req*`（可 NULL） | `*req` 清零 | 无 | 释放 `path`；**幂等**，调用方不要再 `free(req->path)` |
| `http_find_header` | http.c | `req` 已解析 | `req,name`（NUL 结尾） | `const http_header*` | `NULL` | 借用，不分配 |
| `http_status_text` | http.c | — | `status` | 状态文本 | 表里没有 → 兜底串（如 `"Error"`） | 静态串 |
| `http_content_type` | http.c | `path` NUL 结尾 | `path` | MIME 文本 | 不认识的扩展名 → `application/octet-stream` | 静态串 |
| `http_build_response_head` | http.c | `cap > 0` | `out,cap,status,ctype,body_len,keep_alive,extra` | 写入字节数 | 放不下 → `-1`（**绝不许发截断的头**） | 不分配 |
| `http_request_wants_close` | http.c | `req` 已解析 | `req`（**只读**） | `1` = 处理完就关 | 无（纯判断，见 §1b） | 不分配 |
| `file_open_under_root` | file.c | `root_real` 是启动时算好的文档根 realpath | `root_real,url_path,out*` | `FILE_OK` + `out` 填好（fd 已打开） | `enum file_status`；**不留已打开的 fd** | 成功时 fd 由调用方 `close` |

### M2/T2.3b 新增（连接状态机与 IO；细节写在 `src/conn.h`，决策见 `docs/adr/004`）

| 函数 | 模块 | 前置 | 输入 | 输出 | 失败 | 所有权 |
|---|---|---|---|---|---|---|
| `conn_set_wbuf` | conn.c | `c` 有效 | `buf,len` | `wbuf` 装好、`woff=0` | `-1` = 内存不足（调用方转 `CONN_CLOSE`） | **拷贝**进去；旧的 `wbuf` 被释放 |
| `conn_next_step` | conn.c | — | `c`（**只读**） | 下一步（`conn_step`） | 无（纯函数） | 不改任何状态 |
| `conn_step_events` | conn.c | — | `step` | `EPOLLIN` / `EPOLLOUT` / `0` | 无 | 无 |
| `io_sendfile` | conn.c | `out_fd` 是 socket、`in_fd` 是普通文件 | `off*`、`count` | `IO_OK` + `*n`；`*off` 由内核推进 | `IO_WOULD_BLOCK` / `IO_CLOSED`（文件提前到头）/ `IO_ERROR` | 不动所有权 |
| `conn_next_fd` | conn.c | — | `fd`（`-1` = 从头开始） | 下一个已占用 fd；没有 → `-1` | 无 | 无（遍历途中可安全 `close_conn`） |
| `conn_now_ms` | conn.c | — | — | 单调毫秒（`CLOCK_MONOTONIC`） | 时钟失败 → `0` | 无 |
| `conn_reset_for_next_request` | conn.c | 当前请求的响应已写完 | `c` | `wbuf`/`file_fd`/`rscan`/`keep_alive` 清干净，回到 `CONN_READ_REQ` | 无 | **关掉 `file_fd`、释放 `wbuf`**（每请求一份的资源） |

**状态机不变式（T2.3b + T2.6）**：一条连接恒处于 `CONN_READ_REQ / CONN_WRITE / CONN_DRAIN / CONN_CLOSE` 之一；
"下一步干什么、该关注什么事件"**只能**由 `conn_next_step()` + `conn_step_events()` 推出来
（**不许另外存一份 interest 副本** —— 存的是"当前注册的掩码"，只用于"没变就不 `MOD`"，方向仍由这两个函数决定）。
`CONN_WRITE` 内部顺序固定：先把 `wbuf` 写完 → 再 `sendfile` 文件体 → 然后**四选一**：
`drain_left > 0` → `DRAIN`（先 `shutdown(SHUT_WR)`，200ms 截止）；`keep_alive` → `NEXT_REQ`（清状态接下一个）；
否则 → `CLOSE`。连接对象**拥有** `fd` 与 `file_fd`（`conn_free` 一起关）。
**ET 口径（T2.4）**：每一步都要**做到 `IO_WOULD_BLOCK` 为止**才算"做不动了"，
否则边沿触发不会再通知 → 连接会挂住。注册 `EPOLLOUT` 只在真的写不动之后（写完立刻摘掉）。

**借用 vs 拥有**（最容易出事的一条）：`http.h` 里除 `path` 之外**所有字段都是借用**（指向读缓冲的切片，
**非 NUL 结尾**）：比较必须"先比长度、再 `strncasecmp`/`memcmp`"，**不许 `strcmp`**。

---

## 3. 错误码 → 状态码映射（只在 `main.c` 实现一次）

| `http_err` | 状态码 | 备注 |
|---|---|---|
| `HTTP_OK` | 继续处理 | — |
| `HTTP_ERR_BAD_REQUEST` | 400 | |
| `HTTP_ERR_METHOD` | **405** | 带 `Allow: GET, HEAD`（ADR-002） |
| `HTTP_ERR_URI_TOO_LONG` | 414 | |
| `HTTP_ERR_VERSION` | 505 | |
| `HTTP_ERR_HEADER_TOO_LARGE` | 431 | |
| `HTTP_ERR_TOO_MANY_HEADERS` | 431 | |
| `HTTP_ERR_BODY_TOO_LARGE` | 413 | |
| `HTTP_ERR_INTERNAL` | 500 | |
| `HTTP_ERR_NOT_IMPLEMENTED` | 501 |
| （未列举的值） | 400 | 解析失败算客户端问题，别回 500 |

| `file_status` | 状态码 |
|---|---|
| `FILE_OK` | 200 |
| `FILE_BAD_PATH` | 400 |
| `FILE_FORBIDDEN` | 403 |
| `FILE_NOT_FOUND` | 404 |
| `FILE_IO_ERROR` | 500 |

轮询超限（`main.c` 在解析之前就判）：`rlen >= RBUF_SIZE` 或 `rlen >= MAX_HEADER_LEN` → **431**，且必须走收尾（lingering close）。

---

## 4. 不许违反的纪律（可用 grep 自查）

```bash
grep -rn '\bexit(\|\babort(' src/ | grep -v main.c    # 必须为空：只有 main 能决定进程生死
grep -rn 'strcpy\|strcat\|sprintf' src/               # 必须为空：只用 snprintf + 长度检查
grep -rn 'fd\|sockaddr\|errno' src/http.h             # 必须为空：协议层不碰 IO
grep -n '#include "http.h"' src/file.h                # 必须为空：file 模块不认识协议
grep -n 'io_wait\|poll(' src/main.c                   # 必须为空：T2.3b 起等待统一交给 epoll_wait
grep -n 'close(' src/main.c   # 只允许：reply_then_close / accept 与启动的失败兜底 / file fd 的退还 / 退出路径
```

- 写一律 `send(..., MSG_NOSIGNAL)` + 进程级 `SIGPIPE → SIG_IGN`（两道防线）。
- 被测函数**失败返回错误码、不 `exit()`**；失败路径**不留已分配内存、不留已打开的 fd**。
- 上限**先检查再解析**（`MAX_*` 都在 `src/httpd.h`）。
- 事件循环里**不许阻塞着等某一条连接**：一次事件只做一段 IO，剩下的交给下一次 `epoll_wait`。
- 关连接只走 `close_conn()`：**先 `epoll_ctl(DEL)` 再 `conn_free`**（顺序反了就是 fd 复用踩已释放对象）。
