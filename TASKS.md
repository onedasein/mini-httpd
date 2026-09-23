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
| **M3** | 健壮性与对抗测试 | 9h | 周 4 末 |
| **M4** | 压测与第一轮优化 | 10h | 周 5–6 |
| **M5** | 交付 | 4h | 周 5–6 |

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
- [ ] **T0.3 最小 TCP echo server**（2h）
  - blocking socket：`socket → bind → listen → accept → read → write`，一次只伺候一个连接
  - ✅ `printf 'hi\n' | nc 127.0.0.1 8080` 能回显
  - 学到：`sockaddr_in`、`htons`、`SO_REUSEADDR`（不加它重启会 `EADDRINUSE`）
- [ ] **T0.4 热身 ①：mini shell**（2h，路线图 C 第 4 阶段验收）
  - 支持管道 `|` 与重定向 `>`，用 `fork` + `execvp` + `waitpid`
  - ✅ 在你自己写的 shell 里 `ls | wc -l > out.txt` 结果正确
  - 学到：`dup2`、fd 表、`waitpid` 与僵尸进程
- [ ] **T0.5 热身 ②：生产者-消费者队列**（2h，路线图 C 第 5 阶段验收）
  - `pthread` + 互斥锁 + 条件变量，固定大小环形缓冲
  - ✅ `make tsan` 编译运行无数据竞争告警
  - 学到：为什么条件变量必须在 `while` 里判条件、`pthread_cond_signal` vs `broadcast`

---

## M1 · 阻塞式 HTTP：先把「协议」做对（周 3 上半，10h）

> 这一阶段**故意不用 epoll**——先把 HTTP 语义、缓冲、错误码做对，后面换成事件循环时才有对照组。

- [ ] **T1.1 观察真实报文**（0.5h）
  - `nc -l 8080` 占住端口，用 `curl -v` 打过去，把原始字节看清楚（每行结尾是 `\r\n`）
  - ✅ 能把自己的请求报文原样打印出来（含不可见字符，用 `od -c` 或 `%q` 打印）
- [ ] **T1.2 请求行解析 + 固定响应**（2h）
  - 解析 `METHOD SP PATH SP VERSION CRLF`，返回固定 body
  - ✅ `curl -i http://127.0.0.1:8080/` 同时看到状态行与 body
- [ ] **T1.3 headers 解析成结构 + 上限防护**（2h）
  - 大小写不敏感（`Connection` / `connection`）、允许多个同名 header、**单行 ≤ 8KB、总量 ≤ 64KB**，超了就 431/400
  - ✅ `printf 'GET / HTTP/1.1\r\nX: %s\r\n\r\n' "$(head -c 20000 /dev/zero | tr '\0' a)" | nc 127.0.0.1 8080` 不会崩、不越界
  - 学到：为什么不能对二进制/未信任输入用 `strcpy`/`strcat`（用长度 + `memchr`）
- [ ] **T1.4 静态文件服务 + 路径安全**（3h）
  - 把 URL path 映射到 `./www` 下的文件；`Content-Type` 用一张小表（html/css/js/png/jpg/txt/json）
  - **必须挡住** `..`（先做前缀归一化或用 `realpath()` 校验结果仍以 `www/` 开头）
  - ✅ `curl --path-as-is 'http://127.0.0.1:8080/../Makefile'` 返回 400/404 **而不是** Makefile 内容
  - ✅ `curl -I http://127.0.0.1:8080/index.html` 的 `Content-Type`/`Content-Length` 正确
- [ ] **T1.5 完整写 + 部分写**（1.5h）
  - `write()` 返回值可能小于请求长度，必须循环写完
  - ✅ `curl -o out.bin http://127.0.0.1:8080/big.bin` 后 `cmp` 与源文件一致（自己造一个 10MB 文件）
- [ ] **T1.6 HTTP 语义细节**（1h）
  - `HEAD` 不返回 body；非法方法 → 405；HTTP/1.0 默认短连接；`Connection: close` 立即断开
  - ✅ 四条各用 `curl -I/-X` 手工验一遍

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
| M0 | 8h | | |
| M1 | 10h | | |
| M2 | 14h | | |
| M3 | 9h | | |
| M4 | 10h | | |
| M5 | 4h | | |
