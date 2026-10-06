# 02 · man / RFC 速查（M1 边写边查的实际结论）

只记**这次真的踩到、查过、验证过**的条目。每条都给出「怎么自己验一遍」。
（这一版的答案会过时，命令不会。）

---

## 1. `_POSIX_C_SOURCE` 不够用的时候

| 想用的东西 | 只定义 `_POSIX_C_SOURCE 200809L` | 需要什么 |
|---|---|---|
| `realpath(3)` | **不声明**（XSI 选项组） | `_XOPEN_SOURCE 700`，且记得 `#include <stdlib.h>` |
| `strcasecmp(3)` | 可以（`strings.h`） | — |
| `poll(2)` / `nanosleep(2)` / `MSG_NOSIGNAL` | 可以 | — |
| `strdup(3)`、`memmem(3)` | `strdup` 可以；`memmem` 要 `_GNU_SOURCE` | 本项目没用 `memmem`，自己写扫描 |

```bash
# 一条命令验证某个符号在当前 feature macro 下有没有声明
printf '#define _XOPEN_SOURCE 700\n#include <stdlib.h>\nint main(void){char b[4096];return realpath("/",b)==0;}\n' > /tmp/rp.c
gcc -std=c11 -Werror=implicit-function-declaration -o /dev/null /tmp/rp.c && echo OK
grep -n -B6 'realpath' /usr/include/stdlib.h     # 看 glibc 的门槛条件
```
**坑**：`implicit declaration of function` 报的是「没声明」，但根因常常是**漏 include**，不是 feature macro。
B2 就是两个原因叠在一起 —— 先确认头文件在不在，再去调 feature macro。

## 2. `poll(2)` 与信号：`SA_RESTART` 救不了它

- `signal()` 在 glibc 上是 **BSD 语义**（自动带 `SA_RESTART`），但 `man 7 signal` 明确列出
  **`poll`/`ppoll`/`select` 属于「永不因 SA_RESTART 而重启」的接口** → 被信号打断就是 `-1/EINTR`。
- 所以 `wait_fd()` 里必须区分两种 `EINTR`：**要退出**（`g_stop`）还是**只是被别的信号碰了一下**（重试）。
- `HTTPD_IDLE_MS = -1` 表示无限等 —— 这就是 v1「阻塞」那一半的实现（慢客户端能拖死全服）。

```bash
man 7 signal | grep -A4 'never restarted'
```

## 3. `send(2)` 的 `MSG_NOSIGNAL` 与 `SIGPIPE` 的两道防线

- 写一个「对端已经关掉」的 TCP socket，内核会发 `SIGPIPE`，**默认动作 = 杀死整个进程**。
- 两道防线（都做）：`send(..., MSG_NOSIGNAL)`（只影响这一次调用）+ `signal(SIGPIPE, SIG_IGN)`（进程级兜底）。
- 触发条件是**第二次** write：第一次写得出去（或直接成功），对端内核回 RST；第二次才 `EPIPE`。
  所以「一次读一次写、读 0 就收尾」的 echo 结构**碰不到**它（0/20 实测），而 HTTP 服务器由自己决定何时写 body，必然要防。

```bash
# 在 DSH 沙箱里做 SIGPIPE 实验前，必须先把处置恢复成默认！否则结论会整个反过来
grep SigIgn /proc/self/status        # 本机实测 0000000001001000 → 含 0x1000 = SIGPIPE 被忽略
python3 -c 'import signal,os,sys; signal.signal(signal.SIGPIPE,signal.SIG_DFL); os.execv(sys.argv[1],sys.argv[1:])' ./yourprog
```

## 4. `SO_REUSEADDR`：它到底放行什么

- 作用：允许 `bind()` 到一个**还有 TIME_WAIT 连接**占着的本地端口。不加它，重启服务会 `EADDRINUSE`。
- 三个实测细节（都不是教科书会写清楚的）：
  1. 常规「客户端先关」的会话结束后，TIME_WAIT 落在**客户端临时端口**，服务端端口没被占 → **不加 `SO_REUSEADDR` 也能立刻重启**。
     要复现 `EADDRINUSE`，得让**服务端当主动关闭方**（例如连接存续时 `kill -9` 服务端）。
  2. Linux 只在**旧 socket 自己也带 `SO_REUSEADDR`** 时才放行冲突。
  3. 正常情况下没这个问题：**accepted socket 会从 listener 继承 `SO_REUSEADDR`**。

```bash
ss -tan | grep 8080      # 看 TIME-WAIT 的「本地地址」列在谁身上，结论立刻分明
```

## 5. `nc`：OpenBSD 版不会因为 stdin EOF 就关连接

`printf 'hi\n' | nc 127.0.0.1 8080` 会打印出 `hi` 然后**永远挂着**（两端都在等对方先关）。

```bash
timeout 2 bash -c "printf 'hi\n' | nc 127.0.0.1 8080"; echo $?   # 124 ← 挂死
timeout 5 bash -c "printf 'hi\n' | nc -N 127.0.0.1 8080"; echo $? # 0
```
`-N` = stdin EOF 后 shutdown 写方向。**别用 timeout 掩盖挂死**，那会连真 bug 一起盖掉。
另外：`nc -l` 配 `od -c` 是看客户端真实字节最省事的办法（curl 默认的请求到底长什么样）。

## 6. RFC / 状态码（M1 实际用到的）

| 场景 | 码 | 依据 |
|---|---|---|
| HTTP/1.1 请求缺 `Host` | 400 | RFC 9112 §3.2（服务器 MUST 回 400） |
| 单个 header 字段行过长（>8KB） | 431 | RFC 9110 §15.5.23 的语义 |
| header 总量过大（>64KB） | 431 | 同上 |
| request-target 过长（>2KB） | 414 | RFC 9110 §15.5.16 |
| 请求体声明过大（>1MB，本版不支持 body） | 413 | RFC 9110 §15.5.14 |
| 方法不在白名单 | 405 + `Allow` | RFC 9110 §15.5.6（必须给 `Allow`） |
| 版本不认识但形如 `HTTP/x.y` | 505 | RFC 9110 §15.6.6 |

行尾：标准是 CRLF；宽容实现也接受裸 LF —— 本项目两种都收（`http_scan_header_end`）。
`RFC 9112` 取代了老的 `7230`（报文语法），`RFC 9110` 是语义。

## 7. 调试手法备忘

```bash
# 1) 把「网络层」和「解析层」切开：直接给解析器喂字节，别隔着 socket 猜
#    （见 tests/run.sh 里的 send_raw.py；M1 就是这么把 B1 从 12 个 return 里揪出来的）
# 2) 调试器打点：每个 return 一行断点，看最后停在哪 —— 比 printf 快，也不改代码
gdb -batch -q -x .scratch/gdb.cmds --args ./probe
# 3) 服务端侧的线上真实字节
strace -f -e trace=recvfrom,sendto -s 300 -o /tmp/tr.txt ./httpd_v1 ...
# 4) 优雅退出之后做泄漏检查（被 kill -9 的进程，LeakSanitizer/valgrind 根本不做退出检查）
kill -TERM <pid>
```
