# CONTRACTS.md — 契约真源

> **顺序纪律（2026-10-13 定）**：**契约表 → 测试用例表 → 实现**。
> 任何状态码/失败语义的改动，都先改本文（+ `docs/adr/`），再改 `src/*.h`，最后改实现与测试。
> 数字（`MAX_*`）的真源是 `src/httpd.h`（**代码是数字的唯一真源**）；本文只写名字与含义，不抄数字。

---

## 1. 状态码表（语义真源）

| 码 | 触发条件 | 谁产生 | 出处 |
|---|---|---|---|
| 200 | 文件存在（目录请求补 `index.html`） | `file.c` → `main.c` | RFC 9110 §15.3.1 |
| 400 | 请求行语法错；HTTP/1.1 缺 `Host`；`..` 穿越；`%00` / 反斜杠；版本字段语法不合;三段之间多个连续空格(`GET  /  HTTP/1.1`);用\t当分隔符；HTTP/1（缺次版本）/版本字段带尾随空格；版本字段为空；target非origin-form`(http://host/x、*)`；行尾只有`\r`没有`\n`；重复`Content-Length`且值不同 | `http.c` / `file.c` | RFC 9112 §2.2、§3.2 §3 §2.3 § 6.3 |
| 403 | `realpath` 后越出文档根；`EACCES` | `file.c` | RFC 9110 §15.5.4 |
| 404 | 文件不存在；目录没有 `index.html` | `file.c` | RFC 9110 §15.5.5 |
| 405 | 方法合法但不在白名单（PUT / DELETE / …），响应必须带Allow: GET, HEAD | `http.c` 报 `HTTP_ERR_METHOD` → `main.c` 映射 | RFC 9110 §15.5.6、**ADR-002** |
| 413 | `Content-Length > MAX_BODY`（第一版不收 body） | `http.c` | RFC 9110 §15.5.14 |
| 414 | `request-target > MAX_PATH_LEN` | `http.c` | RFC 9110 §15.5.16 |
| 431 | 单行 > `MAX_LINE_LEN`；head 总量 > `MAX_HEADER_LEN`；字段数 > `MAX_HEADERS` | `main.c`（早退）+ `http.c` | RFC 9110 §15.5.23 |
| 500 | 内部分配失败（`HTTP_ERR_INTERNAL`） | `http.c` → `main.c` | RFC 9110 §15.6.1 |
| 501 | method为小写`get`; method超长(> `MAX_METHOD_LEN`)| http.c | RFC 9110 §9.1 |
| 505 | 版本形如 `HTTP/x.y` 但不受支持 | `http.c` | RFC 9110 §15.6.6 |

| **静默关闭**（一个字节都不回） | 请求没发完就 EOF / 读写出错 | `main.c` | — |

`Content-Length:0`与没有该header的情况，正常处理，无错误码

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
| `http_build_response_head` | http.c | `cap > 0` | `out,cap,status,ctype,body_len,extra` | 写入字节数 | 放不下 → `-1`（**绝不许发截断的头**） | 不分配 |
| `file_open_under_root` | file.c | `root_real` 是启动时算好的文档根 realpath | `root_real,url_path,out*` | `FILE_OK` + `out` 填好（fd 已打开） | `enum file_status`；**不留已打开的 fd** | 成功时 fd 由调用方 `close` |

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
```

- 写一律 `send(..., MSG_NOSIGNAL)` + 进程级 `SIGPIPE → SIG_IGN`（两道防线）。
- 被测函数**失败返回错误码、不 `exit()`**；失败路径**不留已分配内存、不留已打开的 fd**。
- 上限**先检查再解析**（`MAX_*` 都在 `src/httpd.h`）。
