# mini-httpd · 任务分解

## 0. 一句话定义 + 能力清单

**用 C 写一个单进程、单线程、epoll 边沿触发（ET）、全程非阻塞的 HTTP/1.1 静态文件服务器。**

| # | 能力 | 判据 |
|---|---|---|
| 1 | 监听端口、接受并发连接 | `wrk -t4 -c200 -d30s` 无崩溃、无 5xx |
| 2 | HTTP/1.1 请求解析 | 请求行 + headers；header 名大小写不敏感；超长请求被拒 |
| 3 | 静态文件响应 | `Content-Length` / `Content-Type` / `404` / `400` 正确 |
| 4 | **keep-alive** | 同一条 TCP 连接上连续处理 ≥2 个请求 |
| 5 | 非阻塞 + ET | 每次事件循环里的 read/write 都必须读到/写到 `EAGAIN` 才收手 |
| 6 | 空闲超时 | 连上不发数据，5s 内被服务端断开 |
| 7 | 安全 | `..` 路径穿越被挡；客户端突然断开不会让进程退出（`SIGPIPE`） |
| 8 | 无泄漏 | ASan/UBSan 全绿；压测前后 `/proc/<pid>/fd` 数量一致 |
| 9 | 压测数据 | `wrk --latency` 的 QPS/P50/P99 + 优化前后对比表 |

**第一版明确不做**：HTTPS、POST/PUT、CGI、动态内容、目录列表、gzip、HTTP/2、多线程/多进程、Range 请求。
（这些留给「优化」阶段，暂时不扩散。）

---

## 1. 里程碑总览

| 里程碑 | 内容 | 预计 | 对应路线图 |
|---|---|---|---|
| **M0** | 环境 + C 五阶段热身 | 8h | 周 1–2（环境搭建、C 第 1–5 阶段） |
| **M1** | 阻塞式 HTTP，跑通正确性 | 10h | 周 3 上半 |
| **M2** | epoll 非阻塞 reactor（核心） | 14h | 周 3 下半 – 周 4 |
| **M2+** | Rust 对照实现（非路线图里程碑，见下节） | 1.5–2 天 | —（校准「C→Rust 替代」判断） |
| **M3** | 健壮性与对抗测试 | 9h | 周 4 末 |
| **M4** | 压测与第一轮优化 | 10h | 周 5–6 |
| **M5** | 交付 | 4h | 周 5–6 |

---

## 2. 测试分层约定（2026-10-08 起）

每个 warmup 目录内部统一三个入口，命名一致，后续 httpd/v1、shell 也照这个来：

| 目标 | 含义 | 速度 |
|---|---|---|
| `make test` | **单元测试**：一个被测模块一个 `tests/unit/test_*.c`，文件里多个测试函数；进程内直接调用被测函数并断言 | 毫秒级 |
| `make e2e` | **端到端**：起完整二进制、走网络，`tests/e2e/run.sh` 负责调度与比对 | 秒级 |
| `make check` | `test` + `e2e`（提交前 / CI 用） | — |

每个目标都有 `-asan` 变体（`test-asan` / `e2e-asan`），发布前双跑。根 `Makefile` 里
`make test` / `make e2e` / `make check` / `make test-asan` / `make e2e-asan` 只做转发。

落地要点（踩过才知道）：

- 被测逻辑必须从 `main` 拆出来、编成**不含 main** 的对象：
  `LIB_SRC := $(filter-out src/main.c, $(wildcard src/*.c))`，否则链接测试程序会 duplicate symbol；
- 被测函数**失败要返回错误码，不要 `exit()`**，否则一条用例会把整个测试进程带走；
- 「脚本 + 传参 + 比对」属于 E2E，不是单元测试；两者互补，不是替代；
- 第三方框架 vendor 进 `tests/unit/unity/`（版本记在 `tests/unit/unity/.VERSION`），
  **单独用 `-w` 编译**，别套本仓库的严格告警；测试文件加 `-Wno-missing-prototypes`（否则 `setUp`/`tearDown` 刷屏）；
- 用例名用 ASCII —— Unity 的 `TEST_ASSERT_*_MESSAGE` 会把非 ASCII 转义成 `\xNN`，中文会变乱码，中文写注释；
- 空套件不许报「全部通过」：`make test` 里先挡住「一个 `test_*.c` 都没找到」的情况。

**已分层**：`warmups/echo`。**尚未分层**：`warmups/echo_pre`、`warmups/httpd/v1`、`warmups/shell`
（它们的 `test` 目前仍是脚本回归，列入根 `WARMUPS` 但不在 `WARMUPS_E2E`）。

---

## M0 · 环境与热身（周 1–2，8h）

- [ ] **T0.1 工具链自检与补齐**（1h）
  - 已有：`gcc clang make gdb valgrind nc curl wrk ab cmake`
  - **缺**：`strace`、`perf` → `sudo apt install strace`；perf 在 WSL2 装不了，见 README「WSL2 注意」
  - ✅ `strace -c true` 有输出；`wrk --version` 正常
- [ ] **T0.2 仓库与构建跑通**（1h）
  - 骨架已就绪：`make debug && ./build/debug/mini-httpd`
  - ✅ 三个目标（debug / release / asan）都能编出二进制；`git log` 有一次提交
  - 学到：Makefile 的变量、`.PHONY`、为什么 release 和 debug 要分开编
  - ⚠ **2026-10-06 复核未通过**：`src/` 与 `lib/` 目前是空目录，git 也没有跟踪任何 `src/*.c`，
    根 `make debug` 直接报 `cc: fatal error: no input files`；`build/*/mini-httpd` 是 9/23–9/28 的陈旧产物。
    本项要等 M1 源码落地后重新验收。
- [x] **T0.3 最小 TCP echo server**（2h）— 2026-10-06 完成：`warmups/echo/`（代码 + 概念图 + `README.md` 坑列表 + `tests/run.sh` 回归全绿；2026-10-07 重写为 `server.c`，补齐 argv/<ip> <port> 与 `dump_bytes` 日志后 11 条全绿，debug + ASan 双跑）
  - ★ **2026-10-08 重构为分层结构**：`server.c` 拆成 `src/{main,parse,dump}.{c,h}` —— `parse_args` 解析 `<ip> <port>` 且**失败返回 -1、不 `exit`**；`format_bytes` 是纯函数（snprintf 式截断语义）+ `dump_bytes` 是 I/O 薄壳。测试分两层：`tests/unit/`（Unity v2.7.0 vendored，**6 个测试函数 / 32 行表驱动用例**）与 `tests/e2e/run.sh`（原脚本挪位，顺手修了 `ROOT` 少算一级的路径）。单元与 E2E 各 debug+ASan 双跑，**四条全绿**；目标名与落地要点见上面「测试分层约定」。
  - ⚠ 上面「11 条全绿」记的是 2026-10-07 那版 `tests/run.sh`；当前脚本只留 1 条回显断言，11 条那版仅存在于 git 历史 `8dfb04e`。
  - 学到：`$(filter-out src/main.c, ...)` 出库，测试才能直接链接被测模块（库里有 main 就 duplicate symbol）；`strtol` 会跳过前导空白、接受 `+`/`-`，所以 `" 80"` 被容忍而 `"80 "` 被 `*end != '\0'` 挡住；`snprintf` 的返回值是「本该写入的长度」，截断判断必须拿它跟 `cap` 比而不是看写了多少；空套件报「全部通过」是最危险的假绿。
  - blocking socket：`socket → bind → listen → accept → read → write`，一次只伺候一个连接
  - ✅ `printf 'hi\n' | nc -N 127.0.0.1 8080` 能回显（本机 `nc` 是 OpenBSD 版，**必须 `-N`**：否则 stdin EOF 后两边互等，管道永久挂死，实测 `timeout 2` 收尾 exit=124）
  - ✅ 10000 字节被读成 4096 / 4096 / 1808 三次（短读现场），回显与源文件 `cmp` 完全一致
  - 学到：`sockaddr_in`、`htons`、`SO_REUSEADDR`
  - ★★ **修正**「不加 `SO_REUSEADDR` 重启就 `EADDRINUSE`」：这个 echo 上**复现不出来**。常规会话结束后 TIME_WAIT 落在**客户端**临时端口；要复现必须让服务端当主动关闭方，而且 `SO_REUSEADDR` 只对「旧 socket 自己也带该选项」的 TIME_WAIT 放行（accepted socket 会从 listener 继承）。双向对照见 `warmups/echo/tests/run.sh` ③
  - ★ **修正**「不忽略 SIGPIPE 会被信号干掉」：本 echo 在 `SIG_DFL` 下 **0/20 被杀** —— 它「一次 read 一次 write」，EOF 之后最多只剩一次 write（成功），碰不到第二次 write 的 EPIPE。机制用最小复现钉死（无 `SIG_IGN` → exit 141；`SIG_IGN` → 存活），**M1/M2 仍必须做 `MSG_NOSIGNAL` / `SIG_IGN`**（HTTP 由服务端自己决定何时写 body，那时才会中招）
- [ ] **T0.4 热身 ①：mini shell**（2h，路线图 C 第 4 阶段验收）
  - 支持管道 `|` 与重定向 `>`，用 `fork` + `execvp` + `waitpid`
  - ✅ 在你自己写的 shell 里 `ls | wc -l > out.txt` 结果正确
  - 学到：`dup2`、fd 表、`waitpid` 与僵尸进程
  - 2026-10-07：STAGE=1 套件（10 条）已全绿（`make test`）。修掉两个真 bug：① `main.c` 用 `fgets` 后又判 `feof`，「末行没有换行符」的命令会被整个丢掉（`fgets` 命中 EOF 时**照样返回已读到的那一行**）；② `parse.c` 的分词依赖"每个 token 后面都有分隔符"，末 token 没有分隔符就被吞掉（`echo tail` 变成零参 `echo`）。管道/重定向（STAGE=2）仍未实现
- [ ] **T0.5 热身 ②：生产者-消费者队列**（2h，路线图 C 第 5 阶段验收）
  - `pthread` + 互斥锁 + 条件变量，固定大小环形缓冲
  - ✅ `make tsan` 编译运行无数据竞争告警
  - 学到：为什么条件变量必须在 `while` 里判条件、`pthread_cond_signal` vs `broadcast`

---

## M1 · 阻塞式 HTTP：先把「协议」做对（周 3 上半，10h）

> 这一阶段**故意不用 epoll**——先把 HTTP 语义、缓冲、错误码做对，后面换成事件循环时才有对照组。
>
> **2026-10-06 完成**：`warmups/httpd/v1/`（885 行：`main.c` / `http.c,h` / `file.c,h` / `httpd.h`）
> `bash tests/run.sh` → **55/55**（debug 与 ASan+UBSan+LeakSanitizer 双跑），valgrind `0 errors` + `All heap blocks were freed`。
> 审查报告 `warmups/httpd/docs/03-审查报告.md`，总结 `04-M1总结.md`，速查 `02-man与RFC速查.md`。

- [x] **T1.1 观察真实报文**（0.5h）— strace 抓到内核侧真实字节：请求 `recvfrom(...) = 92`（`GET / HTTP/1.1\r\nHost: …\r\n\r\n`）、响应头 `sendto(..., MSG_NOSIGNAL) = 137`
  - `nc -l 8080` 占住端口，用 `curl -v` 打过去，把原始字节看清楚（每行结尾是 `\r\n`）
  - ✅ 能把自己的请求报文原样打印出来（含不可见字符，用 `od -c` 或 `%q` 打印）
- [x] **T1.2 请求行解析 + 固定响应**（2h）— `GET /` → 200，body 是 `www/index.html`（408B），响应行 CRLF 结尾
  - 解析 `METHOD SP PATH SP VERSION CRLF`，返回固定 body
  - ✅ `curl -i http://127.0.0.1:8080/` 同时看到状态行与 body
- [x] **T1.3 headers 解析成结构 + 上限防护**（2h）— 单行 20000B → **431**；总量 70KB → **431**；URI 2500B → **414**；`Content-Length: 9999999` → 413；`hOsT` / `cOnNeCtIoN: CLOSE` 大小写不敏感；重复同名 header 允许
  - 大小写不敏感（`Connection` / `connection`）、允许多个同名 header、**单行 ≤ 8KB、总量 ≤ 64KB**，超了就 431/400
  - ✅ 20000 字节的单行 header 不崩、不越界（ASan 版跑同一套全绿）
  - 学到：为什么不能对二进制/未信任输入用 `strcpy`/`strcat`（用长度 + `memchr`）——全仓库 `grep` 无一处 `strcpy/strcat/sprintf`
  - ★ 附带踩坑：`next_line` 吃掉 `\r` 却没回退行尾指针 → **所有 CRLF 请求被误判 400、裸 LF 反而 200**。见 `03-审查报告.md` B1
- [x] **T1.4 静态文件服务 + 路径安全**（3h）— 路径穿越 `--path-as-is /../Makefile` → **400 且不泄漏内容**；`%2e%2e%2f`、`..%2f`、`%00` → 400；指向 `/etc/passwd`、`../Makefile` 的符号链接 → **403**；www 内正常软链 → 200；目录无 index → 404
  - 把 URL path 映射到 `./www` 下的文件；`Content-Type` 用一张小表（html/css/js/png/jpg/txt/json）
  - **必须挡住** `..`（先做前缀归一化或用 `realpath()` 校验结果仍以 `www/` 开头）——两条都做了（`file.c` 双保险）
  - ✅ `curl --path-as-is 'http://127.0.0.1:8080/../Makefile'` 返回 400/404 **而不是** Makefile 内容
  - ✅ `curl -I http://127.0.0.1:8080/index.html` 的 `Content-Type`/`Content-Length` 正确
- [x] **T1.5 完整写 + 部分写**（1.5h）— 10MB `cmp` 一致；把客户端 `SO_RCVBUF` 压到 4096 并间歇停读，逼出「部分写/EAGAIN/POLLOUT」路径，仍是 `10485760/10485760`
  - `write()` 返回值可能小于请求长度，必须循环写完
  - ✅ `curl -o out.bin http://127.0.0.1:8080/big.bin` 后 `cmp` 与源文件一致（自己造一个 10MB 文件）
- [x] **T1.6 HTTP 语义细节**（1h）— `HEAD` 只发 137 字节头（Content-Length 仍为 408）；`PUT`/`DELETE` → 405 + `Allow: GET, HEAD`；缺 `Host` 的 1.1 → 400；`HTTP/2.0` → 505；裸 LF 行尾宽容接受
  - `HEAD` 不返回 body；非法方法 → 405；HTTP/1.0 默认短连接；`Connection: close` 立即断开
  - ✅ 四条各用 `curl -I/-X` 手工验一遍
  - 注：keep-alive 按计划留给 **T2.6**（v1 一律 `Connection: close`）

---

## M2 · epoll 非阻塞 reactor（周 3 下半 – 周 4，14h）★ 核心

- [ ] **T2.1 连接抽象与缓冲区**（2h）
  - `struct conn { int fd; char rbuf[8192]; size_t rlen, roff; char *wbuf; size_t wlen, woff; time_t last; }`
  - 想清楚「读用 roff/rlen、写用 woff/wlen」的语义，以及一个连接处理完一个请求后怎么把残余字节搬到前面（`memmove`）
- [ ] **T2.2 非阻塞改造**（2h）
  - `fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK)`
  - 所有 IO 包装成「返回 -1 且 `errno == EAGAIN` 时表示稍后再来」；`EINTR` 一律重试
  - ✅ 故意不改造完成时用一个慢客户端（`nc` 连上不发数据）就能**复现整台服务器卡死**——把这个对比记进 README
- [ ] **T2.3 epoll 水平触发（LT）版事件循环**（2h）
  - `epoll_create1` → `epoll_ctl(ADD)` → `epoll_wait` 循环；先不追求性能，求「能跑」
  - ✅ `strace -c ./build/debug/mini-httpd` 里能看到 `epoll_wait`
- [ ] **T2.4 切到边沿触发 ET**（3h）
  - 所有 fd 加 `EPOLLET`；**accept 要循环到 `EAGAIN`**；**read 要循环到 `EAGAIN`**；写不完时注册 `EPOLLOUT`
  - ✅ `wrk -t2 -c100 -d10s http://127.0.0.1:8080/index.html` 结果里 Non-2xx = 0 且无超时
  - 学到：ET 的本质是「只在状态变化时通知一次」，所以必须自己耗尽数据
- [ ] **T2.5 连接生命周期正确性**（3h）
  - 对端关闭（read 返回 0 / `EPOLLRDHUP`）→ 关闭；**关闭前先 `epoll_ctl(DEL)`**（否则拿到已释放连接 → use-after-free）
  - ✅ `wrk -t4 -c200 -d30s` 全程无崩溃；ASan 版跑同样压测无报错
- [ ] **T2.6 keep-alive**（2h）
  - 同一连接上循环解析并处理多个请求；`Connection: close` 或 HTTP/1.0 时主动关闭
  - ✅ `curl -v http://127.0.0.1:8080/a http://127.0.0.1:8080/b` 出现 `Re-using existing connection`

---

## M2+ · Rust 对照实现（M2 完成后插入，1.5–2 天）

> **目的**：同一份需求、同一套黑盒验收，换语言再实现一次，把「Rust 到底省了我什么、多要了我什么」变成第一手结论，
> 用来校准「C 能不能被 Rust 替代」这个判断（三个方向答案不同，见产出笔记第 7 条）。
> **前置**：M2（`warmups/httpd/v2/`）完成 —— 没有 C 版作参照物，就没有「对照」可言。
> **编号**：`TR.x` = Rust 对照任务，插在 M2 之后，不占用 M3/M4 的编号。

**硬禁令**（违反即本项失败）：

| 禁令 | 理由 |
|---|---|
| 不用 `tokio` / `async-std` / `axum` / `hyper` | 那测的是框架，不是语言 |
| 不用 `mio` | mio 也是 epoll 之上的抽象层；要对照就自己碰 `libc` |
| 不改 HTTP 语义、不换架构 | 语义一改，基准就没了 |
| 不做 TLS / HTTP2 / 多线程 / 零拷贝 | 性能优化留给 M4；本项只做语言对照 |

允许的依赖：`libc`（必须）、`nix`（可选薄封装）、`log`+`env_logger`（可选）。理想状态只有 `libc`。

- [ ] **TR.1 装工具链并配镜像**（1h）
  - **本机实测**：`static.rust-lang.org` 一个 HEAD 就要 **8.0s**（不可用）；`rsproxy.cn` 0.14s、
    `mirrors.tuna.tsinghua.edu.cn/rustup/` 0.036s → **必须走镜像**
  - 装之前先 `unset http_proxy https_proxy HTTP_PROXY HTTPS_PROXY all_proxy ALL_PROXY`
    （本机代理变量指向 WSL 自己的 loopback，真代理在 Windows 侧，不摘掉会误判「网络不通」）
  - `export RUSTUP_DIST_SERVER=https://mirrors.tuna.tsinghua.edu.cn/rustup`（备选 `https://rsproxy.cn`）；
    `~/.cargo/config.toml` 里把 crates-io 换成 `sparse+https://rsproxy.cn/index/`
  - ✅ `cargo --version` 有输出；`cargo new /tmp/x && cd /tmp/x && cargo build` **1 分钟内**完成
    （拉不动就是镜像没配好）
  - ⚠ 这套命令只实测了**端点可达性**，整条链没验证过；真装的时候先跑一遍再往下走

- [ ] **TR.2 目录与接口契约**（0.5h）
  - `warmups/httpd/rust/`（与 `v1`/`v2` 平级，**不碰 v2**）；模块拆 `main / sys / conn / http / file`；
    `www` 指向 `../v1/www`
  - ★ **接口契约（硬要求）**：二进制必须接受同样的 `argv`：`<ip> <port> <www_root>` ——
    否则复用不了 `v1/tests/run.sh`，就得重写题目，对照失效
  - `sys.rs` 是全仓库唯一允许 `unsafe` 的模块：crate 级 `#![deny(unsafe_code)]` + 该模块 `#[allow(unsafe_code)]`

- [ ] **TR.3 六个骨架设计决策（自己拍板，全部记进产出笔记）**（2h）

| # | 决策点 | C 版 | Rust 逼你面对什么 |
|---|---|---|---|
| 1 | fd 所有权 | 裸 `int`，靠约定 close | `OwnedFd`；而 `epoll_ctl(DEL)` 必须在 `Drop` **之前** → 让 `Conn::drop` 自己 DEL（要拿到 epfd），还是手写 `close_conn()`？**两次尝试都记下来** |
| 2 | fd → Conn 映射 | 数组按 fd 索引 | `HashMap<RawFd,Conn>` / `Vec<Option<Conn>>` / slab；`epoll_event.data` 里放**索引**，别放裸指针（否则 unsafe 扩散到全局） |
| 3 | 缓冲区 | `char rbuf[8192]` + `roff/rlen` | 建议照抄语义以便对照；用 `Vec<u8>` 会立刻撞上「切片借用 vs 连接可变借用」——**本项最大的收获点** |
| 4 | 错误模型 | `ret == -1 && errno == EAGAIN` | 自定义 `enum Io { Done(usize), WouldBlock, Closed, Fatal(io::Error) }`，把「EAGAIN 是正常路径不是错误」表达进类型 |
| 5 | 不许 panic | 「失败返回错误码，不 `exit`」 | `#![deny(clippy::unwrap_used, clippy::expect_used, clippy::panic, clippy::indexing_slicing)]`，顺带把 T1.3/T1.4 的上限防护逼成显式 |
| 6 | unsafe 边界 | 无处不 unsafe | `MaybeUninit` 只出现在 syscall 封装里；验收见 TR.8 |

- [ ] **TR.4 LT 版事件循环**（3h）
  - `epoll_create1` → `epoll_ctl(ADD)` → `epoll_wait` 循环；同 C 版 T2.3，先求「能跑」
  - ✅ `strace -c ./target/release/mini-httpd-rs` 里能看到 `epoll_wait`
  - 学到：Rust 里没有隐式的 errno 全局，`EAGAIN`/`EINTR` 必须被显式建模

- [ ] **TR.5 切到 ET**（3h）
  - 所有 fd 加 `EPOLLET`；**accept 循环到 `EAGAIN`**；**read 循环到 `EAGAIN`**；写不完时注册 `EPOLLOUT`
  - ✅ `wrk -t2 -c100 -d10s http://127.0.0.1:8080/index.html` → Non-2xx = 0 且无超时
  - 学到：ET 的本质是「只在状态变化时通知一次」（同 C 版 T2.4）；Rust 额外要处理 `libc::epoll_event` 的 packed 布局

- [ ] **TR.6 连接生命周期 + keep-alive**（3h）
  - ✅ **黑盒语义对齐（本项最重要的一条）**：
    `cd warmups/httpd/v1 && PORT=8090 bash tests/run.sh ../rust/target/release/mini-httpd-rs`
    → **55/55 通过，且不许改这个脚本**（改脚本＝改题目）
  - ✅ 之后开 keep-alive 再跑一遍，把与 v1 语义（一律 `Connection: close`）的差异**逐条记录**：
    分清「语义等价」与「新加能力」
  - ✅ `wrk -t4 -c200 -d30s` 全程不崩；压测前后 `ls /proc/<pid>/fd | wc -l` 一致（无 fd 泄漏）

- [ ] **TR.7 健壮性对齐（M3 口径）**（2h）
  - 慢速 loris（每 2s 发 1 字节）、只发一半就 RST、空闲 5s 超时
  - ✅ 每种攻击后进程仍在、`grep VmRSS /proc/<pid>/status` 不涨
  - ✅ 非阻塞证明：`nc` 连上不发数据，**其他**客户端仍能拿到 200

- [ ] **TR.8 工程纪律收口**（1.5h，等价于 `-Werror` + ASan 那套）
  - ✅ `cargo clippy --all-targets -- -D warnings` 干净；`cargo fmt --check` 干净；`cargo build --release` 0 warning
  - ✅ `cargo test` 全绿 —— **把 T1.3/T1.4 的用例表原样翻译过来**（同一张用例表两种语言，是最漂亮的对照材料）
  - ✅ `grep -rn unsafe src/ | grep -v '^src/sys.rs'` → 输出为空

- [ ] **TR.9 压测对照 + 产出笔记**（2h）
  - 参考项（**明确不是验收项**）：
    `SRV_CMD=warmups/httpd/rust/target/release/mini-httpd-rs bash tests/bench.sh rust-对照`；
    同机同参数 C/Rust 各一遍，按 T4.5 的纪律记进 `docs/bench/results.tsv`
  - ⚠ **预期 Rust 版不会更快**（甚至略慢）：瓶颈在 syscall 与缓冲区拷贝，两边是同一批 syscall。
    别把「Rust 应该更快」当验收；出现大幅差异先怀疑自己多写了一次拷贝
  - 产出 `warmups/httpd/docs/05-Rust对照.md`，必须回答 7 条：
    ① 行数对比（`find src -name '*.rs' | xargs wc -l` vs `wc -l ../v2/*.c`）
    ② unsafe 行数与出现场合
    ③ 资源生命周期（C 靠约定 vs Rust 靠类型，各自容易漏什么）
    ④ 错误处理形态（errno / `Result` / `Io` 枚举）
    ⑤ 构建与测试体验（Makefile+Unity+vendor vs cargo test/clippy/fmt）
    ⑥ **卡住点 top3**（尤其借用检查器逼你改设计的地方）
    ⑦ 结论：对三个方向 Rust 各能替代多少（200 字，可直接进博客与 `07-progress.typ`）

**本项特有的坑**：

- 压测必须用 `--release`：debug 下路径校验/解析会拖垮 QPS，别据此得出「Rust 慢」的结论
- `libc::EPOLLIN` 是 `c_int`，和 `u32` 混用会天天 `as` → 包一个 `Events(u32)` newtype
- 判 `EAGAIN` 用 `ErrorKind::WouldBlock`，但 **EINTR 是 `ErrorKind::Interrupted`，必须单独重试**
- 越界在 C 里是 UB、在 Rust 里是 panic；「不许 panic」意味着每个 `get()` 的 `None` 都要显式处理
- `libc::epoll_event` 是 `#[repr(C, packed)]`：只填 `data.u64`，别碰 `data.ptr`

---

## M3 · 健壮性

- [ ] **T3.1 空闲超时**（2h）
  - `epoll_wait` 的超时 + 每连接 `last_active`，扫到超过 5s 的就踢（先线性扫，之后可选最小堆）
  - ✅ `nc 127.0.0.1 8080` 连上不动，5s 内被断开
- [ ] **T3.2 对抗性测试**（3h）
  - 超长 header、慢速 loris（脚本每 2s 发 1 字节）、只发一半就 RST、边收边断
  - ✅ 每种攻击后进程仍在、`VmRSS` 不涨（`grep VmRSS /proc/<pid>/status`）
- [ ] **T3.3 `SIGPIPE` 与 errno 分类**（1.5h）
  - `signal(SIGPIPE, SIG_IGN)`（或发送时用 `MSG_NOSIGNAL`）；`EPIPE`/`ECONNRESET` 是正常现象，不要当致命错误
  - ✅ 压测中途 `kill -9` 一批客户端，服务端不退出
- [ ] **T3.4 三件套体检**（2.5h）
  - `make asan` + `valgrind --leak-check=full ./build/debug/mini-httpd`
  - ✅ 全绿；压测前后 `ls /proc/<pid>/fd | wc -l` 一致（无 fd 泄漏）

---

## M4 · 压测与第一轮优化

- [ ] **T4.1 建立基准并记录环境**（2h）
  - 直接跑 **`bash bench.sh baseline`**：脚本会自动 `make release` → 起服务 → 预热 → 跑
    `wrk -t4 -c100 -d30s --latency` → 记录内核/CPU/提交号 → 汇总追加到 `docs/bench/results.tsv`
  - 把那一行填进 `docs/bench.md` 的环境表与结果总表
- [ ] **T4.2 找瓶颈**（3h）
  - WSL2 上 `perf` 不可用 → 用 **`strace -c`** 看系统调用分布、`valgrind --tool=callgrind` 看函数热点
  - 省事做法：`bash bench.sh -s <标签>` 会在压测的同时抓一张 `strace -c` 表
  - ✅ 写下 top3 热点 + 证据（截图或命令输出）
- [ ] **T4.3 优化一：`sendfile(2)` 或 `writev`**（2h）
  - 把响应头与文件体一次发出，省掉「读进用户态再写出」的一次拷贝
  - ✅ `strace -c` 里 `read`/`write` 次数显著下降
- [ ] **T4.4 优化二：减少 malloc/系统调用**（2h）
  - 连接对象用固定池或 `realloc` 一次到位；响应头用栈上缓冲拼装
  - ✅ 压测再跑一次，QPS 或 P99 有可解释的变化
- [ ] **T4.5 优化前后对比表**（1h）
  - 数据都在 `docs/bench/results.tsv`（一行一次）与各自的 `<日期>-<标签>.txt` 原始输出里
  - 按 `docs/bench.md` 的模板填「结果总表 + syscall 对比 + 每轮假设/结论」，再把结论表搬进 README
  - **纪律**：只比同机同编译目标、参数不许中途改、看 P99 不看平均值

---

## M5 · 交付（4h）

- [ ] **T5.1 README 完稿**（2h）：架构 ASCII 图、构建/运行、能力与不做清单、bench 表、踩坑记录
- [ ] **T5.2 200 字感受**（0.5h）：路线图明确要求的「哪里最爽 / 最烦」
- [ ] **T5.3 推上 GitHub**（1h）：
  ```bash
  # 先在 GitHub 网页建空仓库 mini-httpd（不要勾 README）
  git remote add origin git@github.com:onedasein/mini-httpd.git
  git push -u origin main
  ```
- [ ] **T5.4 回填路线图进度**（0.5h）：勾 `~/blog/_typst/roadmap/07-progress.typ` 里的探针 A 项，然后
  `cd ~/blog && bash tools/build-roadmap.sh && git commit -am "progress: 探针 A 完成" && git push`

---

## 2. 分周执行建议

| 周 | 主线（本文件） | 底座（同时进行） | 周末产出 |
|---|---|---|---|
| 1–2 | M0 全部 | CSAPP 1–2 章，`docs/env.md` | 环境笔记 + echo server |
| 3 | M1 全部 + M2-T2.1~T2.3 | CSAPP 第 3 章 + Bomb Lab | 阻塞版能返回文件，epoll 骨架跑起来 |
| 4 | M2-T2.4~T2.6 + M3 全部 | CSAPP 第 6 章 | keep-alive 通过，ASan 全绿 |
| 5 | M4-T4.1~T4.3 | Cache Lab | 基准数据 + 第一轮优化 |
| 6 | M4-T4.4~T4.5 + M5 | Data Lab | repo + README + 对比表 |

---

## 3. 卡住时的求助路径

1. `man 7 epoll`（ET vs LT 的权威说明）、`man 2 sendfile`、`man 2 accept`
2. 书：《UNIX 网络编程 卷1》第 6 章（IO 复用）｜《Linux 高性能服务器编程》第 8–9 章｜CSAPP 第 10、12 章
3. 搜索词（英文）：`epoll ET EAGAIN loop`、`EPOLLOUT partial write`、`accept EAGAIN ET`、`SIGPIPE MSG_NOSIGNAL`
4. 对照实现（**读思路别抄**）：redis 的 `ae_epoll.c`（200 行，最好读）→ nginx 的 `ngx_epoll_module.c` → muduo

---

## 4. 一定会踩的坑

- ET + 不循环 read → 连接永远卡住（最经典）
- `EPOLLOUT` 一直注册着 → 事件循环空转、CPU 100%（只在写不完时注册，写完立刻摘）
- `close()` 之后没 `epoll_ctl(DEL)` → use-after-free（ASan 会抓，但很吓人）
- 忘了 `O_NONBLOCK` → 一个慢客户端拖死全服
- 不处理 `EINTR` → 一个信号就让 read 报错
- ET 下 `accept` 只调一次 → 连接堆积在队列里
- `write` 部分写没循环 → 大文件响应被截断
- 路径穿越 `..` 没挡 → 泄漏任意文件（**必测项**）
- 忘了 `SIGPIPE` → 客户端一断，服务端被信号杀死
- 用 `strcat/sprintf` 拼响应头 → 缓冲区溢出（用 `snprintf` + 长度检查）

---

## 5. 进度记录

| 里程碑 | 计划 | 实际 | 备注（卡在哪、怎么解决） |
|---|---|---|---|
| M0 | 8h | 进行中 | 2026-10-06：T0.3 完成（`warmups/echo`：echo + 概念图 + 回归全绿，4 个坑都有可复现命令）；T0.2 复核未通过（`src/` 为空、根构建编不动）；T0.4/T0.5 产物在 `warmups/` 但本次未复核。2026-10-07：echo 重写为 `server.c`（argv + dump_bytes + `-DNO_REUSEADDR`/`-DKEEP_SIGPIPE_DEFAULT` 开关），11 条回归 debug/ASan 双绿；`make test`/`make test-asan` 目标已加到 echo、httpd/v1、shell 与根 Makefile |
| M1 | 10h | v1 完成 | 2026-10-06：`warmups/httpd/v1`（885 行）落地，`tests/run.sh` **55/55**（debug + ASan/LSan），valgrind 0 error / 0 leak；修掉 1 个真 bug（CRLF 解析让所有正常请求 400）；keep-alive 按计划留给 T2.6；M2 的靶子（慢客户端拖死全服）已复现并留证 |
| M2 | 14h | | |
| M2+ | 1.5–2 天 | 未开始 | 前置：M2（`warmups/httpd/v2/`）完成，否则无参照物；任务与验收点见「M2+ · Rust 对照实现」；本机尚未安装 Rust，TR.1 是纯前置 |
| M3 | 9h | | |
| M4 | 10h | | |
| M5 | 4h | | |
