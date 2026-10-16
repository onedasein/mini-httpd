# M2 ET 验收数据（T2.4d / T2.5d）

- 日期：2026-10-14
- 机器：WSL2（6 vCPU，内核 6.6.87.2-microsoft-standard-WSL2）
- 纪律：**同机自测，只做"改前/改后"的相对比较**，绝对值不能和外网机器比（README「WSL2 注意事项」）
- 被测：`src/` 的 M2 版（`epoll` 边沿触发 + keep-alive），对照基线 `git tag m1-blocking`
- 文档根：`tests/www/`，请求 `/index.html`（40 字节）

## 1. T2.4d：`wrk -t2 -c100 -d10s`（release 产物）

```
Running 10s test @ http://127.0.0.1:8080/index.html
  2 threads and 100 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency     2.04ms    0.86ms  14.23ms   65.91%
    Req/Sec    24.36k     4.34k   33.73k    65.00%
  484801 requests in 10.03s, 151.65MB read
Requests/sec:  48338.74
Transfer/sec:     15.12MB
```

**验收点**：`Non-2xx` 与 `Socket errors` 两行**没有出现**（wrk 有错才会打印）→ 0 非 2xx、0 超时。
`ss -tln` 压测后 `Recv-Q = 0`（T2.4a：ET 下 accept 循环到 `EAGAIN`，队列没堆连接）。

## 2. T2.4c / T2.4d 的对照：**TCP_NODELAY 那一刀（18 倍）**

同一台机、同一参数，唯一区别是 `on_accept` 里有没有 `setsockopt(TCP_NODELAY)`：

| | Requests/sec | Latency (avg) |
|---|---|---|
| 没设 `TCP_NODELAY` | **2,233** | **44.50ms** |
| 设了 `TCP_NODELAY` | **48,338** | **2.04ms** |

**44ms 是哪来的**：我们发一条响应是"先 `send` 响应头、再 `sendfile` 文件体"两次写。
Nagle 的规则是"还有没被确认的数据时，后面的小段先攒着" → 头出去以后，那几十字节的 body
要等客户端的**延迟 ACK（约 40ms）**才动。这正是"HTTP 服务器为什么都设 `TCP_NODELAY`"的现场版。
（`strace -c` 里 `setsockopt` 只出现在 accept 时，102 次 = 102 条连接；keep-alive 下**每请求不重连**。）

## 3. T2.5d：ASan 产物跑 `wrk -t4 -c200 -d30s`（正确性压测）

```
Running 30s test @ http://127.0.0.1:8080/index.html
  4 threads and 200 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency    59.69ms   97.37ms   1.00s    97.25%
    Req/Sec     1.12k    87.49     1.45k    88.87%
  133975 requests in 31.04s, 41.93MB read
Requests/sec:   4316.56
```

- fd 数：压测前 `6` → 压测后 `6`（**无泄漏**）
- ASan/LSan 报错：`0`；进程全程存活；`SIGTERM` → exit 0
- 4,316 req/s ≈ release 的 1/11 —— 这是 **ASan 的固定开销**（每条内存访问都插桩），不是服务器的锅；
  压测数字一律用 release，ASan 只用来证明"没踩内存"

## 4. `strace -c`（release，压测期间）

```
% time     seconds  usecs/call     calls    errors syscall
 37.80    0.865022          31     27200     27200 readlink
 11.33    0.259175          57      4533           sendfile
  9.11    0.208510          45      4533           sendto
  8.43    0.192956          35      5429       886 recvfrom
  7.79    0.178175          38      4638           close
  6.99    0.160004          35      4535           openat
  6.13    0.140357          30      4634           getpeername
  5.72    0.130848          28      4536           fstat
  5.68    0.129897          28      4535           write
  0.32    0.007408          36       203           epoll_ctl
  0.29    0.006748          33       204           fcntl
  0.18    0.004098          40       102         1 accept
  0.14    0.003254          31       102           setsockopt
  0.04    0.000834          55        15           epoll_wait
```

**这张表怎么读**（也是 M4 的起点）：

1. **`readlink` 27200 次、占 37.8% 的时间** —— `file_open_under_root()` 每个请求都走一遍
   `realpath()`（内部就是 `readlink` + `lstat`）。这是**当前最大的一个优化点**：同一个路径反复
   `realpath` 完全没必要，M4 做路径缓存（或改成"逐段校验 + 只在必要时 realpath"）。
   附带地：`openat`/`fstat`/`close` 各 4500+ 次，也就是"每请求 open 一次文件"（没做 fd/内容缓存）。
2. **`epoll_wait` 只有 15 次**（10 秒里！）—— 这就是 ET + keep-alive 的形状：
   一次 `epoll_wait` 拿回一批就绪连接，每条连接在自己的事件里把"读请求 → 写响应 → 接下一个请求"
   连着做完。LT 版做不到这样（每个事件只做一段 IO），`m1-blocking` 更是每次都要阻塞等待。
3. **`accept` 只有 102 次** —— 100 条连接 + 备用；**每请求不重连**（keep-alive 的直接证据）。
4. `recvfrom` 有 886 个 `errors` 且 `epoll_wait` 很少：ET 下"读到 `EAGAIN`"是**正常路径**
   （`EAGAIN` 由 `io_result` 表达成 `IO_WOULD_BLOCK`，不是错误），这也解释了为什么
   `CONTRACTS.md` 要把"EAGAIN 不是错误"写进类型里。
5. 注意这是**被 `strace` 拖慢之后**的调用次数（否则 10 秒跑的请求数远不止 4500）；
   看比例、看形状，不看绝对值。

## 5. 复现命令

```bash
make release asan
./build/release/mini-httpd 127.0.0.1 8080 tests/www &
wrk -t2 -c100 -d10s http://127.0.0.1:8080/index.html      # T2.4d
ss -tln | awk 'NR==1 || /:8080/'                           # T2.4a：压测后 Recv-Q 应为 0

./build/asan/mini-httpd 127.0.0.1 8080 tests/www &         # T2.5d
ls /proc/<pid>/fd | wc -l                                  # 压测前后应一致
wrk -t4 -c200 -d30s http://127.0.0.1:8080/index.html

strace -f -c -o strace.txt ./build/release/mini-httpd 127.0.0.1 8080 tests/www &
wrk -t2 -c100 -d10s http://127.0.0.1:8080/index.html
```
