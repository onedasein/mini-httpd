# warmups/httpd — HTTP 服务器两级热身

> **⚠ 2026-10-11 起：本目录降级为「参考 / 对照物」**
>
> 正式实现已移到**仓库根 `src/`**（`TASKS.md` 的 M1 = 阻塞版、M2 = epoll 版，**同一份源码原地演进**，
> 用 `git tag m1-blocking` / `m2-epoll` 做前后对照）。
> - `v1/`：**只作参考与对照物**（885 行、`tests/run.sh` 55/55，2026-10-06），别再往里加功能；
> - `v2/`：只剩一个 Makefile 骨架，**已被根 `src/` 取代，不要再往里写代码**。
> - 本目录的 `docs/`（`02-man与RFC速查` / `03-审查报告` / `04-M1总结`）仍然有效，继续当资料读。

M1（阻塞版）与 M2（epoll ET 非阻塞版）**最初**的落地目录。**两个版本分别一个目录、分别一个 Makefile。**

## 为什么必须分开

本仓库的 warmup Makefile 用 `SRC = $(wildcard *.c)` 收集源文件。若把 v1/v2 的两份 `main.c`
放进同一个目录，`make` 会把它们一起编进去，直接报重复符号；即使侥幸编过，也是在测一个
既不是 v1 也不是 v2 的东西。

```
warmups/httpd/
├── v1/     M1 阻塞式：main.c + http.c/h + file.c + httpd.h   → 依次一个连接
├── v2/     M2 epoll ET 非阻塞：同一功能，改成事件循环 + struct conn
└── docs/   02-man与RFC速查.md / 03-审查报告.md / 04-M1总结.md
```

## 版本定位

| | v1（M1） | v2（M2） |
|---|---|---|
| I/O 模型 | 阻塞，串行 accept | epoll 边沿触发，全程非阻塞 |
| 连接状态 | 局部变量，处理完即释放 | 显式 `struct conn`（`roff/rlen`、`woff/wlen`、`last_active`） |
| 目的 | **先把 HTTP 语义做对** | 换事件循环，对照 v1 找状态机 bug |
| 明文不做 | keep-alive 可只做最简 | 空闲超时（M3 T3.1） |

v1 故意不优雅：它就是 M2 的对照组。如果在 v1 里顺手把非阻塞/连接池都做上，
M2 就没有可比对象了。

## 构建

```bash
cd v1 && make debug      # → v1/build/debug/httpd_v1
cd v2 && make debug      # → v2/build/debug/httpd_v2
```

三个目标（debug / release / asan）与仓库根 Makefile 一致，各自产出独立二进制。
源文件还没落地前，Makefile 会报一句明确提示而不是让链接器去报 `undefined reference to main`。
