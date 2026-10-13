# 01 · 网络编程概念图（M0 热身）

> 这份图是坐标系：后面 M1（阻塞 HTTP）、M2（epoll ET 非阻塞）里每个名词都能在图上找到位置。
> 读法：先看 §1 分层 → §2 socket 生命周期 → §4 短读短写 → §10 路线图（概念 ↔ TASKS.md 编号）。

## 1. 分层：一次 curl 背后谁在干活

```
┌─────────────────────────────────────────────────────────┐
│ 你的程序（userspace）  printf / read / write / sendfile  │
├─────────────────────────────────────────────────────────┤
│ 标准库缓冲（stdio: FILE*）  ← 网络编程通常故意绕开它      │
├─────────────────────────────────────────────────────────┤
│ 系统调用边界  read(2) write(2) accept(2) ...             │
├─────────────────────────────────────────────────────────┤
│ 内核 socket 层：struct sock 两个队列                     │
│   rcvbuf ← 已被 TCP 确认、等 read 来取                   │
│   sndbuf → 已交给内核、还没被对方 ACK                    │
├─────────────────────────────────────────────────────────┤
│ TCP：分段/重组、序号、ACK、重传、滑动窗口、拥塞控制       │
│   ★ 这里是「字节流」，没有消息边界（见 §4）              │
├─────────────────────────────────────────────────────────┤
│ IP：路由、分片                                           │
├─────────────────────────────────────────────────────────┤
│ 链路层：以太网 / WSL2 虚拟网卡（veth + NAT）             │
└─────────────────────────────────────────────────────────┘
```

三个关键认知：
1. `write()` 成功 **只代表数据进了内核 sndbuf**，不代表对方收到。
2. `read()` 只能拿到内核已收到的部分，可能比期望的少 —— 这就是短读。
3. syscall 之上还有一层 stdio 缓冲；网络编程要精确控制「什么时候真的发了」，所以直接用 `read/write`。

## 2. socket API 生命周期（服务器标准骨架）

```
服务端                                     客户端
socket()   拿一个 fd
   │
 setsockopt(SO_REUSEADDR)   ← 不加它，重启会 EADDRINUSE
   │
 bind()     绑到 (IP, port)
   │
 listen()   变成监听 socket，内核开始收握手
   │          内部两个队列：SYN 队列（半连接）、accept 队列（已完成握手）
   │
 accept() ─► 返回一个**新的 fd**（已连接 socket）      connect() ─┐
   │            监听 fd 继续听；新 fd 只代表这一个连接             │
   ├── read()/recv()  ◄─────── 字节流 ─────── write()/send() ────┤
   ├── write()/send() ──────── 字节流 ──────► read()/recv()  ────┤
   ├── close()（发 FIN）                            close() ─────┘
   └─ 回到 accept()（循环）
```

三个反直觉点：

| 现象 | 原因 |
|---|---|
| `listen()` 后还没 `accept()` 就能连上 | 三次握手由**内核**完成，`accept()` 只是从队列里取一个 |
| `accept()` 的 fd 和监听 fd 不是一回事 | 监听 fd 是「门」，连接 fd 是「人」；关连接 fd 不影响继续 accept |
| 客户端 `connect()` 成功时服务端还没 `accept()` | 握手已完成，只是躺在 accept 队列里（队列满 → 丢包或 RST） |

## 3. 地址与字节序

```c
struct sockaddr_in a;
a.sin_family = AF_INET;
a.sin_port   = htons(8080);                       /* 网络序 = 大端 */
inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);     /* 不要用已过时的 inet_addr() */
```

- x86 是小端，协议规定大端 → 端口/地址必须 `htons/htonl` 转换，反向用 `ntohs/ntohl`。
- `struct sockaddr` 是通用类型，要 `(struct sockaddr *)&a` 强转；长度参数是**值-结果型**（调用前 `len = sizeof(a)`）。
- 要同时支持 IPv4/IPv6 → `getaddrinfo()` + `bind()` 循环（本热身先只用 IPv4）。

## 4. ★ TCP 是字节流：短读、短写、粘包

内核对 TCP 的抽象是**一根水管**，不是一包一包的消息：

```
发送方 write("GET / HTTP/1.1\r\nHost: x\r\n\r\n")   一次调用
接收方 read(8192) 可能拿到：
   [GET / HTTP/1.1\r\nHost: x\r\n\r\n]       一次拿全（最常见）
   [GET / HTTP/1.1\r\nHo]                    短读：只拿到一半
   [GET / HTTP/1.1\r\n...\r\n\r\nGET /fav]   粘包：两个请求粘在一起
   [-1, errno=EINTR]                         被信号打断，不是错误
```

五条必须刻进肌肉记忆的规则：

1. **短读**：一次 `read()` 不能假定读完。HTTP 要靠解析找 `\r\n\r\n` 判断「这条请求完没完」。
2. **短写**：一次 `write()` 不能假定写完。`ssize_t` 是有符号的；必须
   `while (left) { n = write(fd, p, left); if (n > 0) { p += n; left -= n; } }`。
3. **`read() == 0`** 不是错误，是 **EOF**（对方 `close()`）。
4. **`EINTR`** 被信号打断，**必须重试**。
5. **`EAGAIN`/`EWOULDBLOCK`**（非阻塞）不是错误，是「稍后再来」。

阻塞 vs 非阻塞返回值语义：

| 情况 | 阻塞 fd | 非阻塞 fd |
|---|---|---|
| 没有数据可读 | 挂住（进程睡眠） | -1 / `EAGAIN` |
| 发送缓冲区满 | 挂住 | -1 / `EAGAIN`，或**部分写** |
| 对方关闭 | 返回 0 | 返回 0 |
| 对方 RST | -1 / `ECONNRESET` | -1 / `ECONNRESET` |
| 信号打断 | -1 / `EINTR` | -1 / `EINTR` |

## 5. 五种 I/O 模型

```
阻塞 I/O      read ────────────────────►[等数据][拷数据]   ← 全程占着线程
非阻塞 I/O    read ─►EAGAIN ─►EAGAIN ──►[拷数据]           ← 轮询，烧 CPU
I/O 复用      epoll_wait 等「任一 fd 就绪」─►read           ← 本项目 M2 的选择
信号驱动      SIGIO 通知 ─►read
异步 I/O      aio_read 立刻返回，内核全干完再通知（Linux 真异步是 io_uring）
```

判定「是不是真异步」的口径：**数据从内核拷到用户态这一步谁做的？**
epoll 只是告诉你哪个 fd 就绪，拷贝还得你自己 `read` → 所以它是**同步 I/O + 多路复用**。

### LT vs ET（`man 7 epoll` 的权威定义）

```
水平触发 LT（默认）: 缓冲区里还有数据，每次 epoll_wait 都通知
                     → 可以每次只 read 一次，写起来接近阻塞版
边沿触发 ET（EPOLLET）: 只在「状态变化」那一刻通知一次
                     → 必须循环 read/write 直到 EAGAIN，否则剩余数据永远没人通知
                     → accept 也必须循环到 EAGAIN，否则连接烂在 accept 队列
                     → 只有「写不完」时才注册 EPOLLOUT，写完立刻摘掉，否则事件循环空转 CPU 100%
```

ET 三条铁律就是 TASKS.md §4 那张坑表的来源。

## 6. 并发模型：怎么同时伺候多个客户端

```
① 串行（M1 阻塞版）        accept → 处理到结束 → 下一个。简单正确，但一个慢客户端拖死全服
② 每连接一进程 fork()      隔离最好，开销大（Apache prefork / PHP-FPM 早期）
③ 每连接一线程 / 线程池    模型直观，要加锁；C10K 下线程栈（默认 8MB）和切换是瓶颈
④ 单进程单线程 + 事件循环 ★ react，无锁无切换，但每个事件里的代码绝不许阻塞
⑤ 多 reactor（每核一个 epoll）SO_REUSEPORT / 多线程分工，nginx、redis 的现代形态
```

**④ 的门槛是「连接 = 状态」**：阻塞写时代码可以很直白；一旦非阻塞，
「这个连接写到哪了」「请求收到一半了」必须显式存进 `struct conn`（T2.1）。
这正是 M1 要**先用阻塞版把 HTTP 语义做对**、再换 reactor 的原因 —— 否则同时在调协议和状态机两个 bug。

## 7. HTTP/1.1 在字节流之上做了什么

TCP 给字节流，HTTP 负责在其上**划出消息边界**：

```
请求： GET /index.html HTTP/1.1 CRLF      ← 方法 SP 目标 SP 版本
       Host: 127.0.0.1:8080 CRLF          ← HTTP/1.1 必须带 Host
       Connection: keep-alive CRLF
       CRLF                               ← 空行 = header 结束
       [body]

响应： HTTP/1.1 200 OK CRLF
       Content-Length: 1234 CRLF          ← ★ 响应边界靠它，不能省
       Content-Type: text/html CRLF
       CRLF
       [body 正好 1234 字节]
```

| 概念 | 要点 |
|---|---|
| 划界方式 | `Content-Length` / `Transfer-Encoding: chunked` / **连接关闭**（HTTP/1.0 老办法） |
| 划界失败 | 粘包 → 把下一个响应当成这个响应的一部分 → 客户端直接报协议错 |
| keep-alive | 一条 TCP 上处理多个请求；复用时要处理缓冲区里的**残余字节**（T2.6 的 `memmove`） |
| 谁关连接 | HTTP/1.0 默认短连接；HTTP/1.1 默认长连接，除非 `Connection: close` |
| 状态码 | 200 / 400 语法错 / 404 / 405 方法不许 / 413 太大 / 414 URI 太长 / 431 header 太大 / 500 |
| 大小写 | **header 名大小写不敏感**（`Content-Length` == `content-length`） |
| 行尾 | 标准是 `CRLF`；宽容实现也收裸 `\n` |

> 卡住时查：`RFC 9112`（报文语法，取代老的 7230）、`RFC 9110`（语义）、`man 7 http`。

## 8. 安全：能跑 vs 能用

| 类别 | 威胁 | 防御 |
|---|---|---|
| 路径 | `/../../etc/passwd`、`%2e%2e` 编码绕过、符号链接逃逸 | 拒绝含 `..` 的段；**且** `realpath()` 后校验结果仍在 www 根之下（两条都做才稳） |
| 缓冲区 | 超长 header / URI → 栈溢出、越界读写 | 全部用长度受限 `snprintf`；先查长度再拷贝；单行 ≤ 8KB、总量 ≤ 64KB |
| 资源耗尽 | slow loris、连接打满 | 空闲超时、每连接固定缓冲、连接数上限 |
| 信号 | 客户端突断 → `write` 触发 `SIGPIPE` → **整个进程被杀** | `signal(SIGPIPE, SIG_IGN)` 或 `send(..., MSG_NOSIGNAL)`（**必做**） |
| 信息泄漏 | 错误页回显绝对路径、目录列表 | 错误页只给状态码 + 简短说明 |
| 逻辑 | `HEAD` 返回 body、PUT/DELETE 未拦 | 方法白名单；`HEAD` 只发头 |
| 崩溃 | 客户端半路 RST | `EPIPE`/`ECONNRESET`/`ETIMEDOUT` 是**正常现象**，关掉这个连接继续服务 |

## 9. 工具箱

| 工具 | 回答什么 |
|---|---|
| `man 2 read` / `man 7 socket` | 精确返回值与 errno 语义 |
| `man 7 epoll` | LT/ET 的权威说明 |
| `man 7 tcp` | `SO_REUSEADDR` / `SO_LINGER` / `TCP_NODELAY` |
| `printf 'hi\n' \| nc 127.0.0.1 8080` | **看原始字节**，验证 CRLF 和报文 |
| `curl -v -i --path-as-is` | 真实请求/响应头；`--path-as-is` 才能发不规范的 `..` |
| `strace -f -e trace=network,read,write ./srv` | 谁返回了 EAGAIN、谁短写了 |
| `tcpdump -i lo -nn -X port 8080` | 线上真实字节（含重传、RST） |
| `gdb`（`handle SIGPIPE nostop`） | 崩在哪一行 |
| `valgrind --leak-check=full` / `-fsanitize=address,undefined` | 越界、泄漏、use-after-free |
| `wrk -t4 -c200 -d30s --latency` | 并发正确性与性能（M4） |

## 10. 坐标：概念 ↔ 任务

```
M0 T0.3 最小 TCP echo     → §2 socket 生命周期、§3 字节序、阻塞 I/O    [warmups/echo]
M1 T1.1~T1.6 阻塞 HTTP    → §7 HTTP 划界、§4 短读短写、§8 路径安全     [warmups/httpd/v1]
M2 T2.1~T2.6 epoll reactor→ §5 LT/ET、§6 并发④、§4 EAGAIN             [warmups/httpd/v2]
M3 T3.1~T3.4 健壮性       → §8 全表、§4 errno 分类
M4 T4.x 压测优化          → §1 的「减少拷贝 / 减少 syscall」、sendfile
```

> 一句话：你在用户态拿着一个 fd，往内核的字节流里读写；所有复杂度来自三件事 ——
> **字节流没有边界**（§4）、**等数据的方式有五种**（§5）、**同时伺候多个人有五种组织方式**（§6）。
> HTTP 是在第一件事上盖的一层规矩（§7），安全是任何一个环节都不能偷懒的底线（§8）。
