# mini-httpd

用 C 从零写一个 **单进程、单线程、epoll 边沿触发（ET）、全程非阻塞**的 HTTP/1.1 静态文件服务器。

> **完整任务分解见 [`TASKS.md`](TASKS.md)**（M0–M5，每个任务都有预计时间和可执行的完成判据）。

## 目标

1. 监听端口、接受并发连接 → `wrk -t4 -c200 -d30s` 无崩溃
2. HTTP/1.1 请求解析（header 名大小写不敏感、超长拒绝）
3. 静态文件响应（`Content-Length` / `Content-Type` / 404 / 400）
4. **keep-alive**（同一 TCP 连接上处理多个请求）
5. 非阻塞 + ET（每次事件都读到/写到 `EAGAIN` 才收手）
6. 空闲连接 5s 超时断开
7. 安全：挡住 `..` 路径穿越；客户端突断不会让进程退出
8. 无泄漏：ASan/UBSan 全绿，fd 数在压测前后一致
9. 压测数据：QPS / P50 / P99 + 优化前后对比表

**第一版不涉及**：HTTPS、POST/PUT、CGI、动态内容、目录列表、gzip、HTTP/2、多线程/多进程、Range。

## 构建与运行

```bash
make debug      # -O0 -g，开发用（gdb 友好）
make release    # -O2，压测必须用这个
make asan       # AddressSanitizer + UBSan，体检用
make test       # 单元测试（tests/unit/*.c，Unity）
make e2e        # 端到端（tests/e2e/run.sh 起进程、走网络）
make check      # test + e2e（提交前用）
make run        # 编译并启动（ARGS 默认 127.0.0.1 8080 tests/www）
make clean
```

```bash
./build/debug/mini-httpd 127.0.0.1 8080 tests/www &     # 三段 argv：<ip> <port> <www_root>
curl -i http://127.0.0.1:8080/index.html
wrk -t4 -c100 -d30s --latency http://127.0.0.1:8080/index.html
```

启动成功时 stdout 会先打一行含 `listening on` 的信息，e2e 脚本靠这句话同步 —— **别改这句措辞**。

## 目录结构

```
mini-httpd/
├── README.md         # 本文件：项目是什么、怎么编怎么跑
├── DESIGN.md         # 设计真源：要写什么 / 架构 / 实现顺序 / 测试策略
├── CONTRACTS.md      # 契约真源：状态码表 / 函数契约 / 错误码映射 / 纪律（先改它，再改代码）
├── TASKS.md          # 执行计划：M0–M5，每一步都有预计时间与验收点
├── Makefile          # 根构建：debug / release / asan + test / e2e / check
├── src/              # 服务器源码（M1 阻塞版 → M2 epoll 版，同一份原地演进）
│   ├── httpd.h       # 上限常量与版本串（数字的唯一真源）
│   ├── http.h/.c     # 协议：字节 ↔ 请求结构、响应头拼装
│   ├── file.h/.c     # URL 路径 → 根下真实文件（含路径安全）
│   ├── conn.h/.c     # 连接对象 + fd 索引连接表 + io_result 四态 IO + 状态机推导（见 docs/adr/003、004）
│   └── main.c        # I/O 与生命周期：收连接 / 读写 / 关 fd / 信号 / argv
├── tests/
│   ├── www/          # 被测的静态文件根目录（argv 第三段，默认就指这里）
│   ├── unit/         # 单元测试 test_*.c；Unity vendored 在 unit/unity/（v2.7.0）
│   ├── e2e/          # 端到端脚本 run.sh（M1-T1.7a）
│   └── bench.sh      # 压测脚本：固定参数、自动起服务、结果落盘（见 M4）
├── docs/             # 参考资料（非真源）—— 入口见 docs/README.md 的文档地图
│   ├── README.md     # 文档地图：谁是真源、什么该查哪里
│   ├── rfc/          # 报文语法与状态码出处、系统调用实测结论（写解析器时查它）
│   ├── adr/          # 决策记录（为什么这么定）
│   ├── method/       # 设计方法论模板
│   ├── reports/      # 阶段性报告（M1/M2 总结、Rust 对照）
│   ├── archive/      # 只读存档：v1 审查报告、warmup 概念图、2026-10-13 重构指令
│   ├── bench.md      # 压测与优化记录（T4.x）
│   └── bench/        # 每次压测的原始输出 + results.tsv（跑 bench 时生成）；M2 的 ET 验收数据在这里
├── warmups/          # M0 的热身产物（echo / shell / pc / concurrent_learn）
│                     #   warmups/httpd/v1 = M1 的参考对照物，不再演进
└── build/            # 编译产物（已 gitignore）
```

**文档地图（先看这个）**：[`docs/README.md`](docs/README.md) —— 明确写了"同一件事只能有一个真源"，
以及"写代码时该查哪一份"。改动顺序：`CONTRACTS.md` → `docs/adr/` → `src/*.h` → 实现 → 测试。

## M1 的串行缺陷（历史证据：M2 已经修掉）

M1 是**阻塞式、串行**的：一次只伺候一条连接，处理完才 `accept` 下一条。
这不是 bug，是 M2（epoll）的**对照组**（`TASKS.md` 的 T2.2b / T1.7c）。
**M2 完成之后这个缺陷已经不存在**（下面的三路对照表里 `200` 那一列）；
这一节保留原文，是因为"靶子长什么样、怎么取证"本身是要留档的。

实测取证（2026-10-14，单独实验；**不在 e2e 套件里**，因为"被拖死"是预期行为，写进套件就成了假失败）：

```bash
./build/debug/mini-httpd 127.0.0.1 18097 <docroot> &                  # 服务端
nc 127.0.0.1 18097                                                    # 慢客户端：连上不发数据
curl -m 2 -o /dev/null -w '%{http_code}\n' http://127.0.0.1:18097/    # 另一个客户端 → 000（超时）
```

| 观测 | 结果 | 说明 |
|---|---|---|
| 第二个客户端的 curl | `000`（超时） | 被拖死：服务端还卡在读慢客户端 |
| 服务端阻塞在哪 | `/proc/<pid>/wchan` = `wait_woken` | 停在 `recv()` 上，不是在等新连接 |
| `ss -tnp` 慢客户端那条 | `ESTAB … users:(("mini-httpd",pid=…,fd=4))` | 真被 `accept` 了，fd 一直被握着 |
| `ss -tnp` 第二个客户端那条 | `CLOSE-WAIT  Recv-Q=79` 且**没有 `users:(…)`** | 它只躺在内核 accept 队列里 —— **`connect()` 成功 ≠ 服务器接受了你**；curl 超时后自己关了 |
| 慢客户端退出之后 | 下一次 `curl` → `200` | 串行模型下立刻恢复 |

→ M2 的验收就是把这个实验**反过来**：慢客户端占住时，其他客户端仍能拿到 200。
**2026-10-14 已经反过来**了（T2.3c，LT 版）—— 见下面第三节的表。

## M2 阶段 A：改造前后对照（T2.2b）

M2 的第一刀只做两件事：**连接 fd 全改非阻塞** + **把 IO 收敛成 `io_result` 四态**。
它**故意不动** accept 结构 —— 监听 fd 仍是阻塞的，主循环仍是一次一条。
所以这一刀**治不好**上面那个"被拖死"，这是预期内的：不卡人是 T2.3/T2.4 的验收。
阶段 A 的产出是**等待点只剩一个**：`io_wait()`（内部 `poll`）。T2.3 把那一个点换成 `epoll_wait`，慢客户端才不再拖死别人。

取证方法（2026-10-14，两侧同一夹具、同一手法；`m1base` 落在 gitignore 的 `build/` 下，不污染仓库）：

```bash
git archive m1-blocking | tar -x -C build/tmp/m1base && make -C build/tmp/m1base debug
# 对两侧各跑一遍：起服务 → nc 连上不发数据 → 第二个客户端 curl -m 2 → 看 wchan → 放开慢客户端 → 再 curl
```

| 观测 | `m1-blocking`（纯阻塞 `recv`） | 阶段 A 后（非阻塞 + `poll` 等待） |
|---|---|---|
| 慢客户端占住时的第二个客户端 | `000`（超时） | `000`（**仍然超时**，符合预期） |
| 服务端卡在哪（`/proc/<pid>/wchan`） | `wait_woken`（停在 `recv()`） | `do_sys_poll`（停在 `io_wait()` 的 `poll`） |
| 慢客户端放开之后 | 立刻 `200` | 立刻 `200` |
| IO 层可分辨的返回 | 「字节数 / -1」两态 | `IO_OK` / `IO_WOULD_BLOCK` / `IO_CLOSED` / `IO_ERROR` 四态 |
| `SIGTERM` 退出码 | `0` | `0`（`wait_readable` 每 200ms 看一次 `g_stop`，不会僵在 `poll` 里） |

**怎么读这张表**：第 1 行没变，说明阶段 A 没顺手治串行（也不该治）；第 2 行的 `wchan` 从 `recv` 变成 `poll`，
才是这一刀真正的形状 —— 等待从「散落在各处的阻塞系统调用」变成了「一个显式的等待点」，
而"等什么、等到没有"由 `io_result` 表达。T2.3 只需替换那一个点。

体检（当时的提交）：`make e2e-asan` → 61/61；新代码走 valgrind → `ERROR SUMMARY: 0 errors` + `All heap blocks were freed`。
（M2 收口时 e2e 已增至 **69** 条：新增 keep-alive 与慢客户端两组。）

## M2 阶段 B/C：事件循环（LT → ET）与 keep-alive

### 1) 慢客户端这个靶子，三路对照（T2.3/T2.4）

```bash
git archive m1-blocking | tar -x -C build/tmp/m1base && make -C build/tmp/m1base debug   # 阻塞基线
git archive <LT 版的那个提交> | tar -x -C build/tmp/stageA && make -C build/tmp/stageA debug
# 三侧各跑一遍：起服务 → nc 连上不发数据 → curl -m 2 → 看 wchan → 放开慢客户端 → 再 curl
```

| 观测 | `m1-blocking` | 阶段 A（非阻塞 + `poll`） | **M2（epoll ET 事件循环）** |
|---|---|---|---|
| 慢客户端占住时的第二个客户端 | `000`（超时） | `000`（超时） | **`200`** |
| 服务端停在哪（`/proc/<pid>/wchan`） | `wait_woken`（`recv`） | `do_sys_poll`（`poll`） | **`do_epoll_wait`** |
| 20 条慢 loris 同时挂着 | 全服卡死 | 全服卡死 | **正常请求 10/10** |
| 空闲 2 秒的 CPU | — | — | **0 jiffy**（没空转） |
| `SIGTERM` 退出码 | `0` | `0` | `0` |

**为什么前两行是 `000`（"改了非阻塞怎么还卡"）**：非阻塞 + `poll` 只改变了「等的时候在干什么」，
没改变「只有一条连接在动」—— 主循环还是在 `handle_conn()` 里把这一条处理到底。
**谁有数据、谁能写，只有内核知道**；不问内核（`epoll_wait`），就只能死等一个人。

### 2) 性能：`TCP_NODELAY` 那一行值 18 倍（T2.4d）

`wrk -t2 -c100 -d10s http://127.0.0.1:8080/index.html`（release，同机）：

| | Requests/sec | Latency(avg) |
|---|---|---|
| 不设 `TCP_NODELAY` | 2,233 | 44.50ms |
| 设了 `TCP_NODELAY` | **48,338** | **2.04ms** |

同一次压测：Non-2xx = 0、超时 = 0、压测后 `ss -tln` 的 `Recv-Q = 0`。
ASan 版 `wrk -t4 -c200 -d30s`：133,975 请求不崩、fd `6 → 6`、ASan 报错 0。
**原始输出与 `strace -c` 表（含"这张表怎么读"）**：`docs/bench/M2-ET验收.md`。

### 3) 连接语义变了（T2.6）

M1 是「一律 `Connection: close`」；M2 起按 `CONTRACTS.md` **§1b**：
HTTP/1.1 默认 `keep-alive`、HTTP/1.0 默认关、显式 `Connection` 覆盖默认；
声明了 `Content-Length > 0`（我们不读 body）或解析失败的分支**一律关**。
`curl -v a b` 现在能看到 `Re-using existing connection`。

决策与取舍（为什么 ET 要"做到 EAGAIN"、为什么 `EPOLLOUT` 只在写不动后挂、为什么 `sendfile`、
为什么 `rscan` 要跨事件保存、为什么 `TCP_NODELAY`）：`docs/adr/004`（LT）、`docs/adr/005`（ET + keep-alive）。
`strace -c` 侧证（10 秒压测）：`epoll_wait` **仅 15 次**、`accept` 102 次（keep-alive 的直接证据）、
`poll` **0 次**；`readlink` 占 37.8% 的时间（每请求一次 `realpath` —— M4 的第一个优化点）。

## WSL2 注意事项

- **`perf` 装不上也用不了**：内核是 `6.6.87.2-microsoft-standard-WSL2`，Ubuntu 仓库里没有对应的
  `linux-tools-<版本>` 包，而且 WSL2 是虚拟机、拿不到 PMU 硬件计数器。
  → 优化阶段改用 **`strace -c`**（系统调用分布）和 `valgrind --tool=callgrind`（函数热点）。所做的优化里有一半是「减少 syscall / 减少拷贝」。
- **压测数字只做「前后相对比较」**：WSL2 的网络栈是虚拟化的，QPS 绝对值不能和外网机器比。
  README 里的数据请注明「WSL2 / 同机自测 / 仅作优化前后对比」。
- `SO_REUSEPORT`、`io_uring` 等在 WSL2 上表现和裸机不同。
