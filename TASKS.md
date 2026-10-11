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
| **M1** | 阻塞式 HTTP：**在仓库根 `src/` 亲手重做**（warmups/httpd/v1 降为对照物） | 10h → 15–17h（含根骨架与测试脚手架） | 周 3 上半 |
| **M2** | epoll 非阻塞 reactor（核心）：**同一份根 `src/` 原地改造**，`git tag m1-blocking` 作对照基线 | 14h | 周 3 下半 – 周 4 |
| **M2+** | Rust 对照实现（非路线图里程碑，见下节） | 1.5–2 天 | —（校准「C→Rust 替代」判断） |
| **M3** | 健壮性与对抗测试 | 9h | 周 4 末 |
| **M4** | 压测与第一轮优化 | 10h | 周 5–6 |
| **M5** | 交付 | 4h | 周 5–6 |

---

## 2. 测试分层约定（2026-10-08 起）

每个项目目录（各 `warmups/*/` 与将来的根 `src/`）内部统一三个入口，命名一致：

| 目标 | 含义 | 速度 |
|---|---|---|
| `make test` | **单元测试**：一个被测模块一个 `tests/unit/test_*.c`，文件里多个测试函数；进程内直接调用被测函数并断言 | 毫秒级 |
| `make e2e` | **端到端**：起完整二进制、走网络，`tests/e2e/run.sh` 负责调度与比对 | 秒级 |
| `make check` | `test` + `e2e`（提交前 / CI 用） | — |

每个目标都有 `-asan` 变体（`test-asan` / `e2e-asan`），发布前双跑。
**根目录当前没有 Makefile**（2026-10-09 被置空成 0 字节）：根的三入口、`-asan` 变体、Unity vendor 都由 **M1-T1.0b / T1.0c** 建立
（早先那套「根 Makefile 只做往 `WARMUPS` 列表转发」的写法已随置空作废，别再照着恢复）。

落地要点（踩过才知道）：

- 被测逻辑必须从 `main` 拆出来、编成**不含 main** 的对象：
  `LIB_SRC := $(filter-out src/main.c, $(wildcard src/*.c))`，否则链接测试程序会 duplicate symbol；
- 被测函数**失败要返回错误码，不要 `exit()`**，否则一条用例会把整个测试进程带走；
- 「脚本 + 传参 + 比对」属于 E2E，不是单元测试；两者互补，不是替代；
- 第三方框架 vendor 进 `tests/unit/unity/`（版本记在 `tests/unit/unity/.VERSION`），
  **单独用 `-w` 编译**，别套本仓库的严格告警；测试文件加 `-Wno-missing-prototypes`（否则 `setUp`/`tearDown` 刷屏）；
- 用例名用 ASCII —— Unity 的 `TEST_ASSERT_*_MESSAGE` 会把非 ASCII 转义成 `\xNN`，中文会变乱码，中文写注释；
- 空套件不许报「全部通过」：`make test` 里先挡住「一个 `test_*.c` 都没找到」的情况。

**执行标记**（M1 起每个步骤都带）：`[手写]` = 你的学习点，必须自己写（用例设计与断言、解析器/状态机、路径安全、并发取舍）；
`[可委托]` = 机械/样板动作，直接点名让 AI 做（目录、Makefile 骨架、vendor Unity、脚本搬运、文档整理）。
**默认值**：步骤里没标 `[可委托]` 的，都是你自己写。

**分层实况（2026-10-11 复核）**：`warmups/echo`、`warmups/pc`（含 `cv/` 条件变量版）、`warmups/shell` 三层齐（`test` / `e2e` / `check`，pc 另加 `tsan`）；
`warmups/httpd/v1` 与 `warmups/concurrent_learn` 仍是单一脚本回归 / 无测试目标。
**根目录尚未分层**：根 `Makefile` 2026-10-09 被置空（0 字节）、根 `src/` 为空 —— 根的分层是 **M1 的交付物**（T1.0b / T1.0c）。

---

## M0 · 环境与热身（周 1–2，8h）— ✅ 2026-10-11 复核：全部完成

> 产物在 `warmups/`：`echo`（T0.3）、`shell`（T0.4）、`pc` + `pc/cv` + `concurrent_learn`（T0.5）。
> T0.2 里那条「根构建编不动」的旧账**并入 M1-T1.0b** 一起结清（根 `Makefile` 现在是空的）。

- [x] **T0.1 工具链自检与补齐**（1h）
  - 已有：`gcc clang make gdb valgrind nc curl wrk ab cmake`
  - **缺**：`strace`、`perf` → `sudo apt install strace`；perf 在 WSL2 装不了，见 README「WSL2 注意」
  - ✅ `strace -c true` 有输出；`wrk --version` 正常
- [x] **T0.2 仓库与构建跑通**（1h）
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
- [x] **T0.4 热身 ①：mini shell**（2h，路线图 C 第 4 阶段验收）
  - 支持管道 `|` 与重定向 `>`，用 `fork` + `execvp` + `waitpid`
  - ✅ 在你自己写的 shell 里 `ls | wc -l > out.txt` 结果正确
  - 学到：`dup2`、fd 表、`waitpid` 与僵尸进程
  - 2026-10-07：STAGE=1 套件（10 条）已全绿（`make test`）。修掉两个真 bug：① `main.c` 用 `fgets` 后又判 `feof`，「末行没有换行符」的命令会被整个丢掉（`fgets` 命中 EOF 时**照样返回已读到的那一行**）；② `parse.c` 的分词依赖"每个 token 后面都有分隔符"，末 token 没有分隔符就被吞掉（`echo tail` 变成零参 `echo`）。管道/重定向（STAGE=2）仍未实现
- [x] **T0.5 热身 ②：生产者-消费者队列**（2h，路线图 C 第 5 阶段验收）
  - `pthread` + 互斥锁 + 条件变量，固定大小环形缓冲
  - ✅ `make tsan` 编译运行无数据竞争告警
  - 学到：为什么条件变量必须在 `while` 里判条件、`pthread_cond_signal` vs `broadcast`

---

## M1 · 阻塞式 HTTP（周 3 上半）— **在仓库根 `src/` 重做**

> **为什么重做**：`warmups/httpd/v1`（885 行、55/55、2026-10-06）是照外部交接稿落的第一版 —— 它现在**降级为参考/对照物**（可以读，别照抄）。
> 这一遍要求：解析器、响应拼装、路径安全、写循环**全部自己写**，同时把**根 `Makefile` + 根分层测试**立起来。
> 立起来之后 M2 才能在同一份 `src/` 上原地换事件循环，用 `git tag` 做前后对照。
> 预计 **15–17h**（比原估 10h 多出：根 Makefile、vendor Unity、把 v1 的 55 条 e2e 移植到根）。

### M1 验收契约（先钉死，再动手）

| 项 | 定死为 | 谁在依赖它 |
|---|---|---|
| 二进制 | `build/{debug,release,asan}/mini-httpd` | `tests/bench.sh` 写死了 `build/release/mini-httpd` |
| argv | `mini-httpd <ip> <port> <www_root>`，三段都可省（默认 `127.0.0.1 8080 tests/www`） | e2e 脚本、M2+ 的 Rust 对照（同一套黑盒） |
| 启动信号 | stdout 先打一行含 `listening on`，再进 accept | e2e 脚本靠这句话同步，**别改措辞** |
| 文档根 | `tests/www/`（2026-10-09 从根 `www/` 移过来的；**别再往根 `www/` 写**） | e2e / bench 的 `URL_PATH=/index.html` |
| 连接语义 | v1 一律 `Connection: close`（keep-alive 是 T2.6） | v1 的 55 条断言 |
| 计数 | `Content-Length` 必须等于**真实字节数**（HEAD 也是） | T1.5 / T1.6 |

**状态码表**（每行都要有 e2e 断言，不许「大概能对上」）：

| 码 | 触发条件 |
|---|---|
| 200 / 404 | 文件在（目录补 `index.html`）/ 文件不在、目录没有 `index.html` |
| 400 | 请求行语法错、`..` 穿越、`%00`、HTTP/1.1 缺 `Host`、版本字段非法 |
| 403 | `realpath` 结果越出文档根、EACCES |
| 405 + `Allow: GET, HEAD` | PUT / DELETE / 任何白名单外方法 |
| 413 | `Content-Length` > 1MB（第一版不收 body） |
| 414 | request-target > 2KB |
| 431 | 单行 > 8KB / 头部总量 > 64KB / 字段数 > 64 |
| 505 | 版本不是 1.0 / 1.1 |

**上限常量**（写进 `src/httpd.h`，**先有上限、再有解析**）：`MAX_LINE 8KB`、`MAX_HEADER 64KB`、`MAX_URI 2KB`、`MAX_HEADERS 64`、`RBUF = MAX_HEADER + MAX_LINE`、`MAX_PATH 4KB`、`MAX_BODY 1MB`。

**全局禁令**：`grep -rn 'strcpy\|strcat\|sprintf' src/` **必须为空**；被测函数**失败返回错误码、不 `exit()`**；
写一律 `MSG_NOSIGNAL` + 进程级 `SIG_IGN` 两道防线。

---

### 阶段 A · 地基（约 2h）

- [x] **T1.0a 目录定型 + 清掉歧义**（20min｜[可委托]）— ✅ 2026-10-11 完成
  - 建 `src/`、`tests/unit/`、`tests/e2e/`；处置根目录那个空 `v1/` 和 `warmups/httpd/v2/`（只有 Makefile 骨架）——删掉，或在 README 里一句话标注「已被根 `src/` 取代」
  - 顺手修 `README.md` 的目录树：现在写的根 `bench.sh`、根 `www/`、`docs/env.md`、`docs/bench/` **磁盘上都不存在**（实际是 `tests/bench.sh`、`tests/www/`）
  - ✅ 实际做法：空 `v1/`（0 个条目，未跟踪）**已 `rmdir` 删除**；`warmups/httpd/v2/` 不删，改在 `warmups/httpd/README.md` 顶部标注「v1 降为对照物、v2 已被根 `src/` 取代」；`README.md` 目录树按磁盘重写（含 `warmups/`、`tests/e2e/`、`tests/unit/`），构建/运行示例改成三段 argv
- [x] **T1.0b 根 Makefile 立起来**（40min｜[可委托]）— ✅ 2026-10-11 完成
  - 三个产物目标 `debug`/`release`/`asan` 各自独立产物目录；`LIB_SRC := $(filter-out src/main.c, $(wildcard src/*.c))`（不然单测链接时 duplicate symbol）
  - `guard`：源文件还没落地时给一句人话，别让链接器报 `undefined reference to main`
  - 严格告警集照抄：`-std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wmissing-prototypes -Wstrict-prototypes -Werror=implicit-function-declaration`
  - ✅ 验收：`make debug` / `release` / `asan` 各出 `build/<profile>/mini-httpd`，零告警（grep 诊断为空的实测）
  - ✅ 附带：`src/main.c` 是**一次性占位**（只打印一行 scaffold 自检，**故意不打印 `listening on`**，免得 e2e 误判服务已起）——它在 T1.2e 会被真正的 listener 整个替换，你可以直接覆盖它
- [x] **T1.0c vendor Unity + 空套件防护**（30min｜[可委托]）— ✅ 2026-10-11 完成
  - Unity 从 `warmups/echo/tests/unit/unity/`（已核对的 **v2.7.0**）拷进根 `tests/unit/unity/`，版本写进 `.VERSION`；第三方源码单独 `-w` 编，测试文件加 `-Wno-missing-prototypes`
  - `make test` / `test-asan` / `e2e` / `e2e-asan` / `check` 目标建好；**空套件必须报错退出**（「一个 `test_*.c` 都没找到」是最危险的假绿）
  - ✅ 拷贝后逐文件 sha256 与 `warmups/echo/...`（v2.7.0）一致；`make test` / `make test-asan` 各 2 PASS / 0 FAIL
  - ✅ 反向验证：把 `test_smoke.c` 挪走后 `make test` 退出码非 0 并打印「空套件不许报全部通过」；`make e2e` 在 `tests/e2e/run.sh` 还没写时也明确失败
  - 📌 `tests/unit/test_smoke.c` 是**测试链自检**（不测业务逻辑，只证明 Makefile + Unity 通了），T1.2b 第一个真用例落地后可以直接删
- [ ] **T1.1 观察真实报文**（30min）
  - `nc -l 8080` 占端口 + `curl -v` 打过去，用 `od -c` / `strace -e trace=network -f` 把**内核侧真实字节**看清楚：每行结尾是 `\r\n`，请求头以空行结束
  - 验收：能把自己的请求报文逐字节打印出来（含 `\r` 不可见字符），并写下请求行三段 `METHOD SP TARGET SP VERSION` 各自的分隔符位置

### 阶段 B · 请求行与响应（约 3h）

- [ ] **T1.2a 定接口**（30min｜[手写]）
  - 交付 `src/httpd.h`（上限常量 + 版本串）、`src/http.h`：`struct request` 字段、错误码枚举、`http_parse_request_line()` / `http_find_header()` / `http_build_response_head()` 原型
  - 关键：接口先定，测试才有东西可调；返回错误码不 `exit()`
  - 验收：`make debug` 过（实现可以先 `return -1`）
- [ ] **T1.2b 用例表先行：请求行**（30min｜[手写]）
  - 交付 `tests/unit/test_request_line.c`：表驱动（一行一个输入 → 期望错误码/期望字段），至少覆盖 `GET / HTTP/1.1`、多空格、缺 version、`GARBAGE\r\n`、裸 LF
  - 关键：**这张用例表是 M2+ Rust 对照要原样翻译的那张**，字段名与错误码别随手改
  - 验收：`make test` 里能看到断言失败（红），不是编译错误
- [ ] **T1.2c 实现请求行解析**（40min）
  - 只做解析，不做 I/O：在固定 `char buf[]` 上工作，全程长度 + `memchr`，不用 `strtok`/`strcpy`
  - 验收：`make test` 绿；`make test-asan` 也绿
- [ ] **T1.2d 响应拼装**（40min）
  - `http_build_response_head()`：状态行 + `Content-Type` / `Content-Length` / `Connection: close` / `Server`；**用 `snprintf` 并检查截断**（返回「本该写入的长度」要和 cap 比）
  - 验收：单测断言「响应头以 `\r\n\r\n` 结束」「Content-Length 等于给定 body 长度」
- [ ] **T1.2e listener + 串行 accept 主循环**（40min）
  - `socket → setsockopt(SO_REUSEADDR) → bind → listen(128) → accept → read → write → close`；`make_listener()` 失败返错码；启动打印 `listening on <ip>:<port>  (www-root=...)`
  - 验收：`./build/debug/mini-httpd 127.0.0.1 8080 tests/www &` 后 `curl -i http://127.0.0.1:8080/` 能同时看到状态行与 `tests/www/index.html` 的 body

### 阶段 C · header 解析与上限（约 3h）

- [ ] **T1.3a 行切分器 `next_line`**（40min｜[手写]）★ 已知事故点
  - CRLF 为主、裸 LF 宽容；**吃掉 `\r` 之后必须把行尾指针回退一格**，否则空行被算成「长度 1 的行」，所有正常请求全判 400（v1 的 B1 就是这么来的，见 `warmups/httpd/docs/03-审查报告.md`）
  - 验收：单测覆盖 `"A\r\n\r\n"`、`"A\n\n"`、`"A\r\nB"`（末行无换行）三种，断言每次返回的行长度与剩余偏移
- [ ] **T1.3b header 结构 + 大小写不敏感**（40min）
  - 按 `name: value` 切；名字比较用 `strncasecmp`；**同名 header 允许重复**（不覆盖、不报错）
  - 验收：`hOsT:` / `cOnNeCtIoN: CLOSE` 能被认出；重复 `X-Dup` 两条都在
- [ ] **T1.3c 上限防护**（40min）
  - 单行 > 8KB 或总量 > 64KB → **431**；字段数 > 64 → 431；URI > 2KB → **414**；`Content-Length` > 1MB → **413**
  - 关键：`RBUF = MAX_HEADER + MAX_LINE`，保证「先攒满再判错」过程永不越界
  - 验收：单测里塞 20000 字节单行、70KB 总量，断言错误码；`make test-asan` 不得越界报警
- [ ] **T1.3d 增量解析（短读现场）**（40min）
  - TCP 是字节流：一次 `read` 可能只到半个请求行。用 `roff/rlen` 记「读到哪、解析到哪」，`read` 返回后重新找 `\r\n\r\n`
  - 验收：e2e 里「请求逐字节到达（每字节间隔 20ms）」仍 200；单测断言「喂半个请求 → 需要更多数据」而不是报 400
- [ ] **T1.3e 三态回归 + ASan**（20min）
  - 跑 `test` + `test-asan`，把 400/431/414 三种拒绝路径都覆盖到；确认没有 `strcpy/strcat/sprintf`（grep 一次）
  - 验收：两套全绿 + grep 输出为空

### 阶段 D · 静态文件与路径安全（约 3h）

- [ ] **T1.4a MIME 表 + 文件打开**（40min）
  - `file_map_path()`：文档根 + URL path → 真实路径；`open` + `fstat`；`Content-Type` 小表（html/css/js/png/jpg/txt/json，未知 → `application/octet-stream`）；**目录补 `index.html`**，没有就 404（不给目录列表）
  - 验收：单测断言扩展名 → MIME 的映射表；e2e `HEAD /index.html` 的 `Content-Type`/`Content-Length` 正确
- [ ] **T1.4b URL 解码**（30min）
  - 只解 `%XX`；**`%00` 与解码后出现控制字符一律 400**；不信 `%2e%2e%2f` 这类「编码后的穿越」
  - 验收：`/%2e%2e%2fMakefile` → 400；`/%00` → 400
- [ ] **T1.4c 路径归一化 + realpath 双保险**（40min｜[手写]）★ 必测项
  - 第一层：自己折叠 `.` / `..`（越出根就 400）；第二层：`realpath()` 结果必须仍以文档根 realpath 为前缀，否则 403
  - 坑：`realpath` 在严格 `-std=c11` 下要 `_XOPEN_SOURCE 700` + `<stdlib.h>`，否则隐式声明被 `-Werror` 挡住
  - 验收：`curl --path-as-is 'http://127.0.0.1:8080/../Makefile'` → 400 **且不泄漏内容**；绝对/相对符号链接逃逸 → 403；www 内正常软链 → 200
- [ ] **T1.4d 错误码映射收口**（40min）
  - 把 `ENOENT → 404`、`EACCES → 403`、坏语法 → 400 收进一个映射函数；400 响应体不含路径等内部信息
  - 验收：单测表驱动喂 `errno` → 状态码；e2e 的 404/403/400 组全绿
- [ ] **T1.4e 安全 e2e 组**（30min）
  - 在 `tests/e2e/run.sh` 里补：穿越三种写法、`%00`、符号链接三条、目录无 index、query 串被忽略
  - 关键：临时素材（10MB 文件、符号链接）跑完必须删掉，别提交进仓库；临时目录一建好先写进 `.git/info/exclude`
  - 验收：这一组单独跑全绿

### 阶段 E · 完整写与背压（约 1.5h）

- [ ] **T1.5a 写满循环**（40min）
  - `send(MSG_NOSIGNAL)` 循环到写完；处理短写；`SIG_IGN(SIGPIPE)` 兜底；`EINTR` 重试
  - 验收：单测里用一个 `socketpair` 把它逼到短写（对端 `SO_RCVBUF` 压到 4096 且不读），断言「最终全部写出」
- [ ] **T1.5b 10MB 完整传输 + 背压**（40min｜[可委托] 造数据）
  - 造 10MB 文件，客户端 `curl -o out.bin` 后 `cmp` 一致；再把客户端 `SO_RCVBUF=4096` + 间歇停读，逼出「部分写」路径
  - 验收：`cmp` 无差异；慢读场景仍能收满 `10485760/10485760`
- [ ] **T1.5c 客户端半路 RST**（20min）
  - 客户端发一半就 RST，服务端不得退出、不得泄漏 fd；RST 之后仍能正常服务下一个请求
  - 验收：e2e 里断言「RST 之后 `/index.html` 仍 200」

### 阶段 F · HTTP 语义细节（约 1h）

- [ ] **T1.6a HEAD**（20min）：不发 body，但 `Content-Length` 仍是文件大小（`curl -I` 只应收到头）
- [ ] **T1.6b 方法白名单**（20min）：`PUT`/`DELETE`/其它 → **405 + `Allow: GET, HEAD`**
- [ ] **T1.6c 版本与连接语义**（30min）：`HTTP/2.0` → 505；HTTP/1.1 缺 `Host` → 400；HTTP/1.0 默认短连接；任何响应都带 `Connection: close`；裸 LF 行尾宽容接受
  - 验收：这四条各有一条 e2e 断言（`raw()` 直接灌原始报文，别只靠 `curl`）

### 阶段 G · 收口与对照（约 3.5h）

- [ ] **T1.7a 把 v1 的 55 条验收脚本移植到根**（40min｜[可委托]）
  - 源：`warmups/httpd/v1/tests/run.sh`（**改脚本 = 改题目**，语义断言一条都不许放宽）；适配点：根 `./build/debug/mini-httpd`、文档根 `tests/www`、`BIN` 绝对化、`set -o pipefail`、PASS/FAIL 计数
  - 验收：`make e2e` 报出总条数（≥55）且失败为 0
- [ ] **T1.7b 三跑体检**（40min）
  - `make check`、`make e2e-asan`（`ASAN_OPTIONS=detect_leaks=1`）、`valgrind --leak-check=full --error-exitcode=1 ./build/debug/mini-httpd`
  - 判据：全绿；valgrind `ERROR SUMMARY: 0 errors` + `All heap blocks were freed`；判「零告警」要 grep `\.c:[0-9]+:[0-9]+: (warning|error)`，别 grep 整个命令（`-Werror=` 会把命令行也算进去）
- [ ] **T1.7c 复现 M2 的靶子**（30min）
  - `nc 127.0.0.1 8080` 连上不发数据 → 第二个客户端应被拖死；用 `ss -tnp | grep 8080`、`cat /proc/<pid>/wchan` 取证（v1 实测停在 `do_sys_poll`，慢客户端一走 ~2ms 恢复）
  - 验收：把现象与证据写进 `README.md`「M1 已知缺陷（故意保留）」——这是 M2 前后对照的基线
- [ ] **T1.7d 与 v1 的设计差异对照**（40min｜[手写]）
  - 逐个模块对比「我这次怎么写的 vs `warmups/httpd/v1` 怎么写的」：缓冲/行切分/路径安全/错误码/写循环；差异处写清「为什么这样更好」
  - 交付 `docs/04-M1总结（根版）.md`：跑通的输出、卡住的地方（现象 → 根因 → 出处）、重写里改掉的
- [ ] **T1.7e 自查清单**（30min）
  - `grep -rn 'strcpy\|strcat\|sprintf' src/` 空；`ls /proc/<pid>/fd | wc -l` 跑完全套后不变；`-h`/无参/坏 argv 不崩；`SIGTERM` → 正常退出 code=0
- [ ] **T1.7f 提交与打基线 tag**（20min）
  - 提交根版 M1，`git tag m1-blocking` 并 push（tag 是 M2 对照的锚点）；确认 `git status` 干净、没有把 `build/`、临时素材、10MB 测试文件卷进提交
  - 回填本文件 §5 进度表

---

## M2 · epoll 非阻塞 reactor（周 3 下半 – 周 4，14h）★ 核心

> **落点**：**同一份根 `src/` 原地改造**（`http.c` / `file.c` 的协议逻辑基本不动，`main.c` 换成事件循环 + 连接状态机）。
> 对照基线是 `git tag m1-blocking`：改造前后跑**同一套** `tests/e2e/run.sh`，差异才谈得上归因。
> 契约不变（argv / `listening on` / `tests/www`），只新增能力：非阻塞、ET、keep-alive。
> 预计 14h；每个阶段结束时 `make check` 必须仍是绿的（改造期不允许长期红）。

### 阶段 A · 连接抽象（约 3h）

- [ ] **T2.1a `struct conn` 与缓冲语义**（40min｜[手写]）
  - `struct conn { int fd; char rbuf[RBUF]; size_t rlen, roff; char *wbuf; size_t wlen, woff; time_t last_active; }`
  - 想清楚：**读用 `roff/rlen`，写用 `woff/wlen`**；`rlen==roff` 才 compact；写出多少 `woff` 加多少
  - 验收：头文件里每个字段写一行注释说明「谁在什么时刻改它」
- [ ] **T2.1b fd → conn 映射**（40min）
  - 定死一种：`struct conn *conns[FD_MAX]` 按 fd 索引（简单）或 fd 池 + 空闲链表；`accept` 拿不到槽位就 503 后关
  - 验收：单测用 `socketpair()` 造 fd（不需要真网络）验证「建/取/销毁」不串号；`make test-asan`
- [ ] **T2.1c 残余字节搬移**（40min）
  - 一个请求处理完后，缓冲区里可能已经躺着**下一个请求的开头**（pipelining）：`memmove` 到前面，`rlen -= consumed`
  - 验收：单测喂「两个请求连在一个 `read` 里」，断言第二个请求被正确识别（keep-alive 的前置）
- [ ] **T2.2a 非阻塞 + IO 包装**（40min）
  - `fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK)`；所有 IO 统一成「`-1 && errno==EAGAIN` → 稍后再来」；`EINTR` **一律重试**（不是错误）
  - 验收：单测/日志里能看到 `EAGAIN` 是正常路径（返回码区分「读到了」「没有了」「对端关了」「真错了」）
- [ ] **T2.2b 留下对照组证据**（30min）
  - 用改造**前**的 `m1-blocking` 二进制再复现一次「慢客户端拖死全服」，把两份证据（改造前卡死 / 改造后不卡）并排放进 README
  - 验收：README 里有一段带命令的对照记录

### 阶段 B · 事件循环（约 5h）

- [ ] **T2.3a epoll 骨架**（30min）
  - `epoll_create1(0)` → `epoll_ctl(ADD, listener, EPOLLIN)` → `epoll_wait` 循环；先只处理 listener，连接读一次就关
  - 验收：`strace -c ./build/debug/mini-httpd` 里能看到 `epoll_wait`
- [ ] **T2.3b LT 版接通**（40min）
  - 水平触发下把 accept/read/write 接进来：一次事件处理一条连接的一段数据即可，不追求性能，只求「能跑」
  - 验收：`curl` 200；`make check` 绿
- [ ] **T2.3c LT 就已经不卡了**（20min）
  - 慢客户端连上不发数据时，其它客户端应照常 200（对照 T2.2b 的证据）
- [ ] **T2.4a listener 切 ET**（40min）
  - listener 加 `EPOLLET`；**`accept` 必须循环到 `EAGAIN`**，否则连接堆在队列里（`ss -tln` 的 `Recv-Q` 会一直涨）
  - 验收：`ss -tln 'sport = :8080'` 压测后 `Recv-Q` 回到 0
- [ ] **T2.4b 连接 fd 切 ET**（40min）
  - conn fd 加 `EPOLLET`；**`read` 必须循环到 `EAGAIN`**（ET 只在状态变化时通知一次）
  - 验收：大 body / 逐字节到达的 e2e 用例仍绿
- [ ] **T2.4c 写不完注册 EPOLLOUT**（40min）
  - 只在「写不完」时 `epoll_ctl(MOD, EPOLLIN|EPOLLOUT|EPOLLET)`，**写完立刻摘掉**；`EPOLLOUT` 常驻 = 事件循环空转 CPU 100%
  - 验收：背压 e2e（客户端 `SO_RCVBUF=4096`）能收满 10MB；压测时 `top` 里 CPU 不空转
- [ ] **T2.4d ET 验收**（30min）
  - `wrk -t2 -c100 -d10s http://127.0.0.1:8080/index.html` → Non-2xx = 0 且无超时；顺手记一张 `strace -c`
  - 验收：数据与 `strace -c` 表一起进 `docs/bench/`（M4 的起点）

### 阶段 C · 生命周期与 keep-alive（约 4.5h）

- [ ] **T2.5a 关闭路径封装**（40min）
  - `close_conn()`：**先 `epoll_ctl(DEL)` 再 `close`**（顺序反了就是 use-after-free）；同时清 `conns[fd]` 槽位、释放 `wbuf`
  - 验收：ASan 下反复连断 100 次无报错
- [ ] **T2.5b 对端半关闭**（40min）
  - `read` 返回 0 / `EPOLLRDHUP` → 收完就关；`EPOLLHUP`/`EPOLLERR` 也要处理，别只认 `EPOLLIN`
  - 验收：`curl` 正常请求后连接被服务端关闭（`Connection: close` 语义）；半关闭（`shutdown(SHUT_WR)`）场景不卡死
- [ ] **T2.5c errno 分类**（40min）
  - `EPIPE` / `ECONNRESET` 是正常现象 → 关连接继续；`EINTR` 重试；只对真错误打印并清理
  - 验收：压测中途 `kill -9` 一批客户端，服务端不退出、日志无致命错误
- [ ] **T2.5d 压测 + 无泄漏**（40min）
  - ASan 版跑 `wrk -t4 -c200 -d30s`（全程不崩）；压测前后 `ls /proc/<pid>/fd | wc -l` 一致
  - 验收：两套数据都记进 `docs/`（一条命令 + 输出）
- [ ] **T2.6a keep-alive 解析循环**（40min）
  - 同一条连接上循环「解析 → 处理 → 消费缓冲 → 处理残余」，`EAGAIN` 才回到 `epoll_wait`
  - 验收：`curl -v http://127.0.0.1:8080/a http://127.0.0.1:8080/b` 出现 `Re-using existing connection`
- [ ] **T2.6b 何时主动关**（20min）：`Connection: close` 与 HTTP/1.0（无 `Connection: keep-alive`）→ 处理完就关；1.1 默认保持
- [ ] **T2.6c e2e 增补 keep-alive 组**（30min）
  - 新增断言：同连接两个请求都 200、`Connection: close` 后连接真的关、1.0 默认关；**v1 那 55 条一条都不许退化成 fail**
  - 验收：`make check` 绿，条数 ≥ 之前 + 新增
- [ ] **T2.6d 收口**（40min｜[手写]）
  - 写 `docs/05-M2总结（根版）.md`：与 `m1-blocking` 的 diff 里哪些是「必要的状态机改造」、哪些是「epoll 特有」；`git tag m2-epoll`
  - 验收：tag + 文档 + 差异清单都在，M2+ 才有参照物

---

## M2+ · Rust 对照实现（M2 完成后插入，1.5–2 天）

> **目的**：同一份需求、同一套黑盒验收，换语言再实现一次，把「Rust 到底省了我什么、多要了我什么」变成第一手结论，
> 用来校准「C 能不能被 Rust 替代」这个判断（三个方向答案不同，见产出笔记第 7 条）。
> **前置**：根 `src/` 的 M2（epoll ET 版，`git tag m2-epoll`）完成 —— 没有 C 版作参照物，就没有「对照」可言。
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

## M3 · 健壮性（9h）

> M2 落地后按上面的粒度继续展开（每步 30–40min）；下面已经是可直接执行的小步。

- [ ] **T3.1a 空闲超时：把时间带进事件循环**（30min）
  - `epoll_wait` 用超时参数（比如 1s），每次事件更新 `conn->last_active`
- [ ] **T3.1b 扫描并踢连接**（30min）
  - 先线性扫（可选最小堆）；超过 5s 没动静 → `close_conn()`
  - 验收：`nc 127.0.0.1 8080` 连上不动，5s 内被断开；`make check` 仍绿
- [ ] **T3.2a 对抗脚本化**（40min）
  - 慢速 loris（每 2s 发 1 字节）、只发一半就 RST、边收边断，做成 `tests/e2e/` 里可重复的用例
- [ ] **T3.2b 攻击后的体检**（30min）
  - 每种攻击后进程仍在、`grep VmRSS /proc/<pid>/status` 不涨、fd 数回升到基线
- [ ] **T3.3a SIGPIPE 两道防线各验一次**（30min）
  - 关掉 `SIG_IGN` 用 `MSG_NOSIGNAL` 验证；关掉 `MSG_NOSIGNAL` 用 `SIG_IGN` 验证（机制钉死，别只留一道）
- [ ] **T3.3b 客户端批量暴死**（30min）：压测中途 `kill -9` 一批客户端，服务端不退出
- [ ] **T3.4a 三件套体检**（40min）：`make asan` + `valgrind --leak-check=full`，全绿
- [ ] **T3.4b fd 泄漏复核 + 记录**（30min）：压测前后 fd 数一致，写进 `docs/`

---

## M4 · 压测与第一轮优化（10h）

> **纪律**：只比「同机 + 同编译目标 + 同参数」；参数中途不许改；看 **P99**，不看平均值。
> T4.2 之后的优化步骤依赖实测热点 —— 那两步到时候再按需展开，别提前写死。

- [ ] **T4.0 修 `tests/bench.sh`**（20min｜[可委托]）★ 2026-10-11 实测已坏
  - 2026-10-09 把 `bench.sh` 从根移到 `tests/` 后：脚本 `cd` 到 `tests/`，于是 `make release` 找不到 Makefile、`BIN=build/release/mini-httpd` 也指到了 `tests/build/...`、`docs/bench` 会建在 `tests/` 下
  - 修法：脚本里显式 `ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)`，`cd "$ROOT"`，`BIN="$ROOT/build/release/mini-httpd"`
  - 验收：`bash tests/bench.sh -q smoke` 能自己编、起服务、跑出一次结果并落到 `docs/bench/results.tsv`
- [ ] **T4.1a 环境信息脚本化**（30min）：内核 / CPU / 提交号 / 编译目标随每次压测一起落盘
- [ ] **T4.1b 跑基线**（30min）：`bash tests/bench.sh baseline`（`make release` → 起服务 → 预热 → `wrk -t4 -c100 -d30s --latency`），填进 `docs/bench.md` 环境表 + 结果总表
- [ ] **T4.2a 系统调用分布**（40min）：`bash tests/bench.sh -s <标签>` 抓 `strace -c`（WSL2 没有 `perf`）
- [ ] **T4.2b 函数热点**（40min）：`valgrind --tool=callgrind` + `callgrind_annotate` 看热点
- [ ] **T4.2c 写下 top3 热点**（20min）：每条都要带命令输出作证据，别写猜测
- [ ] **T4.3 优化一：少一次拷贝**（40min）：`sendfile(2)` 或 `writev` 把响应头与文件体一次发出
  - 验收：`strace -c` 里 `read`/`write` 次数显著下降，QPS/P99 有可解释变化
- [ ] **T4.4 优化二：减少 malloc / syscall**（40min）：连接对象固定池、响应头栈上拼装
- [ ] **T4.5a 再跑一轮并填总表**（40min）：`docs/bench/results.tsv` + 各自的 `<日期>-<标签>.txt`（syscall 对比 + 每轮假设/结论）
- [ ] **T4.5b 结论搬进 README**（20min）：前后对比表 + 「WSL2 / 同机自测 / 仅作前后对比」的免责说明

---

## M5 · 交付（4h）

- [ ] **T5.1a README 架构与用法**（40min）：架构 ASCII 图、构建/运行（`make debug|release|asan`、argv 三段）、目录树与磁盘一致
- [ ] **T5.1b 能力/不做清单 + 数据 + 坑**（40min）：能力清单（对应 §0 的 9 条判据）、bench 表、踩坑记录（含 M1 故意的串行缺陷）
- [ ] **T5.2 200 字感受**（30min）：路线图要求「哪里最爽 / 最烦」，直接回答三个方向各自被印证/证伪了什么
- [ ] **T5.3 推 GitHub**（30min）：`origin` 已配好（`git@github.com:onedasein/mini-httpd.git`），确认无凭据/大文件 → `git push -u origin main --tags`
- [ ] **T5.4 回填路线图进度**（30min）：勾 `~/blog/_typst/roadmap/07-progress.typ` 的探针 A，然后 `bash tools/build-roadmap.sh && git commit -am "progress: 探针 A 完成" && git push`

---

## 2. 分周执行建议

| 周 | 主线（本文件） | 底座（同时进行） | 周末产出 |
|---|---|---|---|
| 1–2 | M0 全部 | CSAPP 1–2 章，`docs/env.md` | 环境笔记 + echo/server + shell + pc（✅ 已完成） |
| 3 | **M1-T1.0 ~ T1.4**（根骨架 + 解析 + 文件 + 安全） | CSAPP 第 3 章 + Bomb Lab | 根 `src/` 能正确返回静态文件，穿越/符号链接全挡住 |
| 3 末–4 | **M1-T1.5 ~ T1.7 → M2 全部** | CSAPP 第 3 / 6 章 | `m1-blocking` 与 `m2-epoll` 两个 tag；keep-alive 通过，ASan 全绿 |
| 5 | M4-T4.0 ~ T4.3 | Cache Lab | 基准数据（`results.tsv`）+ 第一轮优化 |
| 6 | M4-T4.4 / T4.5 + M5 | Data Lab | repo + README + 前后对比表 |

> 弹性规则：某一周只完成了主线的一半，**先保「当周有一个可运行产物 + 一条可复现的验收命令」**，别为了赶周次跳过 ASan 与 e2e。

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
| M0 | 8h | ✅ 完成 | 2026-10-06：echo（T0.3）。2026-10-07：echo 重写 `server.c`。2026-10-08：echo 分层（Unity 单元 + e2e）。2026-10-09：shell 分层（`make test` 10 条，修掉 `fgets` 末行与分词末 token 两个真 bug）。2026-10-10/11：pc 信号量版 + `pc/cv` 条件变量版（TSan 走 `setarch -R` 绕 ASLR 假阳性）+ `concurrent_learn`（CSAPP 第 12 章例子）。**T0.2 的根构建旧账并入 M1-T1.0b**（根 `Makefile` 现为 0 字节） |
| M1 | 原 10h / 重做 15–17h | 🚧 地基已完（T1.0a–c） | 2026-10-06 的 `warmups/httpd/v1`（885 行、`tests/run.sh` 55/55、debug+ASan、valgrind 0 error）**降级为对照物**；2026-10-11 决定在根 `src/` 亲手重做，验收契约与状态码表见 M1 开头。**2026-10-11 阶段 A 完成**：根 `Makefile` 重建（debug/release/asan + test/e2e/check，零告警）、Unity v2.7.0 vendor 进 `tests/unit/unity/`（sha256 与 warmups/echo 一致）、空套件与缺失 e2e 脚本都会明确失败、`src/main.c` 只留一次性占位、README 目录树与磁盘对齐、空 `v1/` 删除、`warmups/httpd/{v1,v2}` 标注为对照物/已取代。**下一步：T1.1（观察真实报文）→ T1.2a（定接口）** |
| M2 | 14h | 未开始 | 落点 = 根 `src/` 原地改造；对照基线 = `git tag m1-blocking`；靶子（慢客户端拖死全服）已在 v1 上复现并留证 |
| M2+ | 1.5–2 天 | 未开始 | 前置：根 `src/` 的 M2 完成（`m2-epoll`）；任务是 TR.1–TR.9；本机尚未安装 Rust，TR.1 是纯前置 |
| M3 | 9h | 未开始 | 步骤已展开到 30–40min 粒度 |
| M4 | 10h | 未开始 | 先修 `tests/bench.sh`（T4.0）；T4.2 之后的优化步骤等实测热点出来再细化 |
| M5 | 4h | 未开始 | README 目录树必须先与磁盘对齐（T1.0a 已含） |
