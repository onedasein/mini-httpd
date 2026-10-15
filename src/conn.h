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

typedef struct conn {
    int fd;               /* 谁改：conn_alloc() 时置为 fd；conn_free() 里 close 之后置 -1 */
    char rbuf[RBUF_SIZE]; /* 谁改：io_read() 往 rbuf + rlen 追加；conn_compact() 里整体前移 */
    size_t rlen;          /* 读缓冲里已有的字节数（每成功读一次 += got） */
    size_t roff;          /* 已经消费掉的偏移（解析完一个请求后 = head_len；由处理函数前移） */
    char *wbuf;           /* 待写缓冲（响应头/小响应体）；NULL = 当前没有待写数据 */
    size_t wlen;          /* 待写总长度（填 wbuf 时定下，之后不变） */
    size_t woff;          /* 已经写出去的字节数（每成功写一次 += sent；woff == wlen 表示写完了） */
    time_t last_active;   /* 最后一次读/写成功的时间（M3 T3.1 的空闲超时用它） */
} conn;

/* ---------------- 连接表（T2.1b） ----------------
 * conn_alloc 返回 NULL = 拿不到槽位（fd 越界 / 同 fd 重复建 / calloc 失败）→ 调用方回 503 后关掉 fd。
 * conn_free 负责 close(fd) + 归还槽位与内存；对 NULL 安全。
 * ⚠ T2.5 起：epoll_ctl(EPOLL_CTL_DEL) 必须在 conn_free() **之前**调用（否则 close 之后 fd 复用会踩到已释放的连接）。 */
conn *conn_alloc(int fd);
conn *conn_lookup(int fd); /* 没有则返回 NULL */
void conn_free(conn *c);

/* 残余字节前移（T2.1c）：把 rbuf[roff..rlen) 搬到 rbuf[0..)，roff 归零。roff == 0 时不动。 */
void conn_compact(conn *c);

/* ---------------- 非阻塞与 IO 包装（T2.2a） ---------------- */

/* 设为非阻塞。**监听 fd 在阶段 A 保持阻塞**：串行 accept 还想靠它排队等待。 */
int conn_set_nonblocking(int fd);

/* 非阻塞 IO：内部重试 EINTR；EAGAIN/EWOULDBLOCK → IO_WOULD_BLOCK；read == 0 → IO_CLOSED。
 * io_write 一律带 MSG_NOSIGNAL（SIGPIPE 的第 1 道防线）。三个函数都要求 n != NULL。 */
io_result io_read(int fd, void *buf, size_t cap, size_t *n);
io_result io_write(int fd, const void *buf, size_t len, size_t *n);
io_result io_read_file(int fd, void *buf, size_t cap, size_t *n); /* 普通文件用 read(2)，不是 recv */

/* 等 fd 就绪：**1 = 就绪；0 = 超时；-1 = 出错**（三种情况必须能分开 —— 调用方要区分
 * "再等一会儿看 g_stop" 和 "这个 fd 坏了，收摊"）。
 * 这是**阶段 A 的临时等待点**：T2.3 会把它换成 `epoll_wait`（同一个位置、同一个套路）。 */
int io_wait(int fd, short events, int timeout_ms);
