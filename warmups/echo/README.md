# warmups/echo — 最小 TCP echo server（M0 / T0.3）

**一个进程、一条连接、阻塞 I/O。** 目的是把 `socket → setsockopt → bind → listen → accept → read → write → close`
这条骨架和字节序/短读/短写的手感练出来，为 M1（阻塞 HTTP）和 M2（epoll ET）当底座。

## 构建与运行

```bash
make debug                       # -O0 -g，开发用
make release                     # -O2
make asan                        # ASan + UBSan 体检
./build/debug/echo_server 127.0.0.1 8080    # 参数：<ip> <port>（都可省，默认 127.0.0.1 8080）
```

三个目标各自产出独立二进制（`build/debug|release|asan/echo_server`），互不覆盖。

## 验证

```bash
bash tests/run.sh                # 全部回归（含 4 个坑的现场复现）
bash tests/run.sh ./build/release/echo_server
```

手工跑：

```bash
printf 'hi\n' | nc -N 127.0.0.1 8080        # ★ 必须有 -N，理由见坑 ①
head -c 10000 /dev/urandom | nc -N 127.0.0.1 8080 >/dev/null   # 看短读：日志会出现 4096/4096/1808
nc 127.0.0.1 8080                            # 交互：打一行回车一次，Ctrl-D 断开
```

服务端日志里 `dump_bytes` 会把不可见字符显形，例如 `  <- [3]: hi\n`。

## 实测结论（2026-10-06，本机 WSL2 / gcc 13.3 / OpenBSD nc）

| 项 | 命令 | 结果 |
|---|---|---|
| 基本回显 | `printf 'hi\n' \| nc -N …` | 收到 `hi`；日志 `recv 3 byte(s)` + `<- [3]: hi\n` |
| 短读 | `nc -N < 10000 字节` | 读成 3 次：4096 / 4096 / 1808；回显与源文件 `cmp` 完全一致 |
| 构建 | `make debug/release/asan` | 三个目标均零警告（`-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wmissing-prototypes -Wstrict-prototypes`） |
| SO_REUSEADDR | 见坑 ② | 常规会话后重启**不报错**；只有制造出落在服务端端口上的 TIME_WAIT 才复现 |
| SIGPIPE | 见坑 ③ | 本服务端在 SIG_DFL 下 **0/20 被杀** —— 结构上碰不到 EPIPE |

## 坑列表

### ① `nc` 不会因为 stdin EOF 就关连接 → 必须 `-N`

本机 `nc` 是 **OpenBSD netcat**（`/usr/bin/nc.openbsd`）。`printf 'hi\n' | nc 127.0.0.1 8080`
会打印出 `hi` 之后**永远挂着**（实测 `timeout 2` 收尾时 exit=124），因为两边都在等对方先关。

```bash
timeout 2 bash -c "printf 'hi\n' | nc 127.0.0.1 8080"; echo $?   # 124
timeout 5 bash -c "printf 'hi\n' | nc -N 127.0.0.1 8080"; echo $? # 0
```

`-N` = stdin EOF 后 shutdown 写方向，服务端才读得到 EOF。**别用「加 timeout」掩盖它**，那会把真 bug 一起盖掉。

### ② `SO_REUSEADDR`：要复现 `EADDRINUSE` 得让服务端当主动关闭方

`man 7 tcp` 说它「允许 bind 到 TIME_WAIT 状态的地址」，但真正的现场是这样的（三轮实测）：

1. **常规 `nc` 会话结束后重启 —— 不会报错。** TIME_WAIT 落在**客户端**的临时端口上
   （`TIME-WAIT 127.0.0.1:46681 → 127.0.0.1:8081`），服务端的监听端口根本没被占。
   所以「注释掉 setsockopt 再重启就一定 EADDRINUSE」的说法在这个 echo 上**不成立**。
2. 要制造落在**服务端端口**上的 TIME_WAIT，必须让服务端成为主动关闭方：
   连上一条连接后 `kill -9` 服务端（内核替它发 FIN）→ 客户端随后也关 → 服务端端口进入 TIME_WAIT。
3. 还有一层：**`SO_REUSEADDR` 只对「旧 socket 自己也带 SO_REUSEADDR」的 TIME_WAIT 放行。**
   旧 TIME_WAIT 若是从一个不带该选项的进程来的，连新进程带 `SO_REUSEADDR` 也照样 `EADDRINUSE`。
   正常情况下没这个问题 —— **accepted socket 会从 listener 继承 `SO_REUSEADDR`**，
   所以自己的服务器反复重启总是能起来。

正确复现（`tests/run.sh` 里已脚本化，双向对照）：

| 旧 TIME_WAIT 的来源 | 重启【不带】SO_REUSEADDR | 重启【带】SO_REUSEADDR |
|---|---|---|
| 不带该选项的进程 | `bind: Address already in use`（exit 1） | `bind: Address already in use`（exit 1） |
| 带该选项的进程（= 真实场景） | `bind: Address already in use`（exit 1） | `listening on 127.0.0.1:8083`（OK） |

### ③ SIGPIPE：机制是真的，但这个 echo server 结构上杀不掉

交接稿说「不打开 `SIG_IGN`，客户端断开后服务端会被信号干掉」。**实测：杀不掉（0/20）。**

原因在代码形状：服务端**只在 `read > 0` 之后才 `write`**，且 `read == 0` 立刻收尾；
而 EOF 总是排在所有数据之后，所以「对端已关」之后最多只剩**一次** write —— 而这一笔是成功的
（TCP 允许往半关连接写）。真正会 `EPIPE → SIGPIPE` 的是**第二次** write，本服务端走不到。
实际命中的是 `ECONNRESET`（对端 close 时若接收缓冲里还有没读走的回显数据，内核会回 RST），
它只是普通 errno，代码里已经有 `peer gone (…)` 分支处理：

```
recv 4096 byte(s) ...            # 若干轮正常回显
peer gone (Connection reset by peer), drop this connection
connection closed
```

机制本身用最小复现钉死（`tests/run.sh` 会编出来跑）：

```
对端 clean close 之后：  write#0 -> 10 (ok)
                        write#1 -> -1 (Broken pipe)     ← 这里发 SIGPIPE
                                   默认动作 = 杀死进程 → exit 141
                        signal(SIGPIPE, SIG_IGN) → 只返回 EPIPE，活到最后 exit 0
```

**结论（写给 M1/M2）**：echo 躲过去是因为它「一次读一次写」；HTTP 服务器要由自己决定
什么时候写 body（尤其大文件 + 慢客户端），那时「写完一次、连接已被对端关掉、再写第二次」是常态，
**`signal(SIGPIPE, SIG_IGN)` 或 `send(..., MSG_NOSIGNAL)` 仍然必做**，只是没法在这个 warmup 里演出来。

### ④ 沙箱陷阱：DSH 的 bash 会把 SIGPIPE 设成 `SIG_IGN` 并被子进程继承

本机 DSH bash 工具启动的进程，`/proc/self/status` 里 `SigIgn = 0x…1001000` —— 含 `0x1000` 即
**信号 13 = SIGPIPE 被忽略**，且 `fork/exec` 会继承。不做处理直接做 SIGPIPE 实验，**必然**得到
「进程存活」的假结论（我第一轮 30/30「存活」就是这么来的）。复现前必须显式恢复默认处置：

```bash
python3 -c 'import signal,os,sys; signal.signal(signal.SIGPIPE, signal.SIG_DFL); os.execv(sys.argv[1], sys.argv[1:])' \
        ./build/debug/echo_server 127.0.0.1 8080
# 验证：SigIgn 变成 0x…1000000（只剩 SIGXFSZ）
```

`bash` 自己的 `trap - PIPE` **不能**恢复被继承的 `SIG_IGN`，必须靠程序显式 `signal()` 重置。

## 下一阶段

M1（阻塞式 HTTP）在 `../httpd/v1/`，M2（epoll ET）在 `../httpd/v2/`；
每个版本各自一个目录 + 各自 Makefile，**别共用**（`SRC = $(wildcard *.c)` 会把两个 `main` 一起编，重复符号）。
概念坐标系见 `docs/01-网络编程概念图.md`。
