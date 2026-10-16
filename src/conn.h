/* src/conn.h — 一条连接的状态（M2 T2.1）与非阻塞 IO 包装（M2 T2.2a）
 *
 * 为什么单独一个模块：T2.1b / T2.1c / T2.2a 的验收都要**单元测试**（用 socketpair 造 fd，
 * 不需要真网络），而测试只链接 LIB_SRC（`src/` 下的 .c 去掉 `main.c`）——
 * 所以连接表与 IO 包装不能写在 `main.c` 里，否则测试碰不到它们。
 *
 * 缓冲语义（读用 roff/rlen，写用 woff/wlen）：
 *   rbuf[0 .. rlen)    已经读进来的字节；rbuf[roff .. rlen) 是**还没被解析消费**的部分
 *   wbuf[0 .. wlen)    待写的字节；      wbuf[woff .. wlen) 是**还没发出去**的部分
 *   一个请求处理完 → roff 前移到「已消费」处 → conn_compact() 把残余挪到前面（keep-alive 的前置）
 */
#pragma once

#include <poll.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <time.h>

#include "httpd.h"

/* IO 的四种结果：把「EAGAIN 不是错误」表达进类型，而不是让调用方去记 errno。 */
typedef enum {
    IO_OK = 0,      /* 成功：*n 是本次读/写的字节数（可能 < cap/len，短读短写都正常） */
    IO_WOULD_BLOCK, /* 现在没有数据 / 写不进去 → 稍后再来（ET 下就等下一次事件） */
    IO_CLOSED,      /* 对端关闭（read 返回 0） */
    IO_ERROR        /* 真错误（ECONNRESET / EBADF…）→ 关掉这条连接 */
} io_result;

/* fd → conn 表的容量：超过它的 fd 拿不到槽位（M2 的验收只到 `wrk -c200`，4096 绰绰有余）。
 * 选「按 fd 索引的指针数组」而不是哈希表：fd 是小整数，查表 O(1)、没有哈希/冲突代码；
 * 代价是一张 4096 指针的静态表（32KB）——拿不到槽位时由调用方回 503。 */
#define CONN_MAX_FD 4096

/* 连接处在状态机的哪一步（T2.3b）。
 * 在「一次事件做一段 IO」的循环里，函数会return回到事件循环，所以位置必须存在对象上。 */
typedef enum {
    CONN_READ_REQ = 0, /* 攒请求头（一个字节都还没解析） */
    CONN_WRITE,        /* 写响应：先 wbuf，再 sendfile 文件体（两步由 conn_next_step 细分） */
    CONN_DRAIN,        /* 响应已发完，正在把对端残留字节读干（有截止时间），之后才 close */
    CONN_CLOSE         /* 该收摊了：调用方先 epoll_ctl(DEL)，再 conn_free */
} conn_phase;

/* 下一步该干什么 —— 由 conn_next_step() 从上面这些字段**推**出来（不额外存一份状态，免得两处打架）。
 * 为什么要有这层：事件循环里"该关注什么事件"若散在 IO 分支里，很容易写成
 * "有数据要写却只关注 EPOLLIN"，然后在 epoll_wait 里死等。判断只能有一份。 */
typedef enum {
    CONN_STEP_READ_REQ = 0, /* 还没攒够请求头 → 关注 EPOLLIN */
    CONN_STEP_WRITE_HEAD,   /* wbuf 还没写完（响应头 / 错误页）→ 关注 EPOLLOUT */
    CONN_STEP_SEND_FILE,    /* 头写完了，文件体还剩 → 关注 EPOLLOUT */
    CONN_STEP_DRAIN,        /* 响应发完、先把对端残留读干 → 关注 EPOLLIN */
    CONN_STEP_NEXT_REQ,     /* 响应发完、连接留着（T2.6 keep-alive）→ 清状态、回去读下一个 → 关注 EPOLLIN */
    CONN_STEP_CLOSE         /* 没别的可做了 → 收摊 */
} conn_step;

typedef struct conn {
    int fd;               /* 谁改：conn_alloc() 时置为 fd；conn_free() 里 close 之后置 -1 */
    char rbuf[RBUF_SIZE]; /* 谁改：io_read() 往 rbuf + rlen 追加；conn_compact() 里整体前移 */
    size_t rlen;          /* 读缓冲里已有的字节数（每成功读一次 += got） */
    size_t rscan;         /* 增量找空行的扫描进度（http_scan_header_end 的 *scan）。
                           * 谁改：扫描函数推进它；**必须跨事件轮次留着** —— 每次从 0 重扫的话，
                           * 一个逐字节发满 8KB 头的连接会把 CPU 放大成 O(n²)（慢速攻击的放大器）。
                           * keep-alive（T2.6）开始读下一个请求时要把它归零。 */
    size_t roff;          /* 已经消费掉的偏移（解析完一个请求后 = head_len；由处理函数前移） */
    char *wbuf;           /* 待写缓冲（响应头/小响应体）；NULL = 当前没有待写数据 */
    size_t wlen;          /* 待写总长度（填 wbuf 时定下，之后不变） */
    size_t woff;          /* 已经写出去的字节数（每成功写一次 += sent；woff == wlen 表示写完了） */
    time_t last_active;   /* 最后一次读/写成功的时间（M3 T3.1 的空闲超时用它） */

    /* ---- 跨事件轮次的响应状态（T2.3b）----
     * 这些以前是 handle_conn() 的局部变量：函数一口气跑到底，栈上存着就行。
     * 改成事件循环后一次事件只做一段 IO，所以它们必须住在连接对象里。 */
    conn_phase phase;            /* 谁改：main.c 的状态机；conn_alloc 置 CONN_READ_REQ */
    uint32_t interest;           /* 当前在 epoll 里注册的事件掩码（0 = 还没注册）。
                                  * 只用来「没变就不 epoll_ctl(MOD)」——**方向仍由 conn_next_step/conn_step_events 决定**，
                                  * 这里存的只是"已经注册了什么"，不是第二份真相。 */
    int keep_alive;              /* 这条连接上的当前请求处理完是否继续保持（T2.6：serve_request 按 §1b 定） */
    int file_fd;                 /* 要发的文件体；-1 = 没有（错误页 / HEAD）。**归连接对象所有**：conn_free 负责 close */
    off_t file_off;              /* sendfile 的读游标（内核会更新它），下一次接着发 */
    long long file_left;         /* 文件体还剩多少字节没发（0 = 发完） */
    long long drain_left;        /* > 0 = 响应发完后要把对端残留读干，这是剩余预算（字节）；0 = 直接关 */
    long long drain_deadline_ms; /* DRAIN 的截止时刻（CLOCK_MONOTONIC 毫秒）：不能无限等对端 */
} conn;

/* ---------------- 连接表（T2.1b） ----------------
 * conn_alloc 返回 NULL = 拿不到槽位（fd 越界 / 同 fd 重复建 / calloc 失败）→ 调用方回 503 后关掉 fd。
 * conn_free 负责 close(fd) + close(file_fd) + free(wbuf) + 归还槽位；对 NULL 安全（连接对象拥有这些 fd）。
 * ⚠ T2.5 起：epoll_ctl(EPOLL_CTL_DEL) 必须在 conn_free() **之前**调用（否则 close 之后 fd 复用会踩到已释放的连接）。 */
conn *conn_alloc(int fd);
conn *conn_lookup(int fd); /* 没有则返回 NULL */
void conn_free(conn *c);

/* 活连接遍历（T2.3b 的超时扫描用）：返回「大于 fd 的最小已占用 fd」，没有则 -1；传 -1 表示从头开始。
 * 为什么不是"上一个连接指针"式迭代器：扫描里会关连接，指针一旦先 free 再取 next 就是 use-after-free。 */
int conn_next_fd(int fd);

/* 单调毫秒（CLOCK_MONOTONIC）：DRAIN 的截止时间用它，M3 的空闲超时也用它。
 * 不用 time(NULL)：它会被系统时间/校时跳变影响，而且秒的粒度表达不了 200ms。 */
long long conn_now_ms(void);

/* 残余字节前移（T2.1c）：把 rbuf[roff..rlen) 搬到 rbuf[0..)，roff 归零。roff == 0 时不动。 */
void conn_compact(conn *c);

/* ---------------- 非阻塞与 IO 包装（T2.2a） ---------------- */

/* 设为非阻塞。T2.3b 起**监听 fd 也用**：事件循环里 accept 一旦阻塞，整个循环就冻住了
 * （LT 下一次还会通知，所以不需要 accept 循环 —— 那是 T2.4a 配 EPOLLET 才必须的）。 */
int conn_set_nonblocking(int fd);

/* 非阻塞 IO：内部重试 EINTR；EAGAIN/EWOULDBLOCK → IO_WOULD_BLOCK；read == 0 → IO_CLOSED。
 * io_write 一律带 MSG_NOSIGNAL（SIGPIPE 的第 1 道防线）。三个函数都要求 n != NULL。
 * io_sendfile 同套语义（Linux 专用）：把普通文件直接推进 socket，省掉用户态缓冲与一次拷贝；
 *   *off 由内核推进，返回 IO_CLOSED = 文件读到头/被截短（客户端拿到的 body 会短，只能断连）。 */
io_result io_read(int fd, void *buf, size_t cap, size_t *n);
io_result io_write(int fd, const void *buf, size_t len, size_t *n);
io_result io_read_file(int fd, void *buf, size_t cap, size_t *n); /* 普通文件用 read(2)，不是 recv */
io_result io_sendfile(int out_fd, int in_fd, off_t *off, size_t count, size_t *n);

/* 等 fd 就绪：**1 = 就绪；0 = 超时；-1 = 出错**（三种情况必须能分开 —— 调用方要区分
 * "再等一会儿看 g_stop" 和 "这个 fd 坏了，收摊"）。
 * T2.3b 起主循环不再用它（等待统一交给 epoll_wait）——留在库里服务单元测试与将来的一次性等待。 */
int io_wait(int fd, short events, int timeout_ms);

/* ---------------- 事件循环要用的三件事（T2.3b） ---------------- */

/* 装载「这一步要写的字节」到 wbuf（替换旧的，旧的 free 掉）。返回 -1 = 内存不足。
 * 只做内存搬运，不碰 fd —— 所以能在单元测试里直接断言。 */
int conn_set_wbuf(conn *c, const void *buf, size_t len);

/* 下一步该干什么（纯函数：只看字段，不碰 fd、不改状态）。
 *   CONN_WRITE 里细分：wbuf 没写完 → WRITE_HEAD；写完了文件体还剩 → SEND_FILE；
 *   都干完了 → drain_left > 0 ? DRAIN : (keep_alive ? NEXT_REQ : CLOSE)。 */
conn_step conn_next_step(const conn *c);

/* 这一步该关注什么事件（EPOLLIN / EPOLLOUT；CLOSE 返回 0）。
 * 与 conn_next_step 配对使用：**该关注什么是推出来的，不是记出来的**。
 * 注意：这里**不含** EPOLLET —— 那是注册策略（在 main.c），不是"该关注什么"的一部分。 */
uint32_t conn_step_events(conn_step s);

/* 一个请求处理完、连接要留着接下一个（T2.6 keep-alive）：把"每请求一份"的状态清干净。
 *   · 关掉 file_fd、释放 wbuf（连接级拥有的东西里，属于这个请求的那部分）
 *   · rscan/roff 归零（rbuf 里的残余字节已经被 conn_compact 挪到前面了）
 *   · keep_alive/drain 清掉，phase 回到 CONN_READ_REQ
 * 不碰 fd / rbuf 里的字节 —— 那些是连接级的。 */
void conn_reset_for_next_request(conn *c);
