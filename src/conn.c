/* src/conn.c — 连接表（T2.1b）、残余搬移（T2.1c）、非阻塞 IO 包装（T2.2a）、状态推导（T2.3b）
 *
 * 设计取舍记在 docs/adr/003-连接表与conn模块.md：
 *   · fd → conn 用「按 fd 索引的指针数组」，连接对象每连接 calloc 一次（M4 再考虑固定池）；
 *   · 连接对象**拥有 fd**（conn_free 负责 close）—— 所以 T2.5 引入 epoll 时，
 *     epoll_ctl(DEL) 必须由调用方在 conn_free 之前做。
 *
 * 注：sendfile 是 Linux/glibc 的扩展，声明在 _DEFAULT_SOURCE 下（_POSIX_C_SOURCE 不够）。
 */
#define _DEFAULT_SOURCE

#include "conn.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <sys/epoll.h>

/* fd → conn 的按 fd 索引表（静态 4096 个指针 = 32KB） */
static conn *g_by_fd[CONN_MAX_FD];

/* ============================ 连接表 ============================ */

conn *conn_lookup(int fd) {
    if (fd < 0 || fd >= CONN_MAX_FD) return NULL;
    return g_by_fd[fd];
}

conn *conn_alloc(int fd) {
    conn *c;

    if (fd < 0 || fd >= CONN_MAX_FD) return NULL;
    if (g_by_fd[fd] != NULL) return NULL; /* 同一个 fd 建两次 = 串号，直接拒绝 */
    c = calloc(1, sizeof *c);
    if (c == NULL) return NULL;
    c->fd = fd;
    c->phase = CONN_READ_REQ; /* calloc 出来本来就是 0，写出来是为了「状态机从哪起步」一眼可见 */
    c->interest = 0;          /* 0 = 还没注册进 epoll（on_accept 里 ADD 之后才写上真实掩码） */
    c->file_fd = -1;          /* -1 不能靠 calloc：0 是合法 fd，忘了置就会误关 stdin */
    c->last_active = time(NULL);
    g_by_fd[fd] = c;
    return c;
}

void conn_free(conn *c) {
    if (c == NULL) return;
    /* 槽位校验：只有"还指着自己"时才清，防止把别人的槽位清掉（串号的第二道防线） */
    if (c->fd >= 0 && c->fd < CONN_MAX_FD && g_by_fd[c->fd] == c) g_by_fd[c->fd] = NULL;
    if (c->file_fd >= 0) close(c->file_fd); /* 文件体也归连接对象所有（T2.3b 起） */
    if (c->fd >= 0) close(c->fd);           /* 连接对象拥有 fd（T2.5 要在这之前 epoll_ctl(DEL)） */
    c->fd = -1;
    c->file_fd = -1;
    free(c->wbuf);
    free(c);
}

int conn_next_fd(int fd) {
    int i;

    for (i = fd + 1; i < CONN_MAX_FD; i++) {
        if (g_by_fd[i] != NULL) return i;
    }
    return -1;
}

long long conn_now_ms(void) {
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (long long)ts.tv_sec * 1000 + (long long)(ts.tv_nsec / 1000000);
}

void conn_compact(conn *c) {
    if (c->roff == 0) return; /* 没有已消费的前缀：不用动 */
    if (c->roff >= c->rlen) { /* 全消费完了：直接归零，省一次 memmove */
        c->rlen = 0;
        c->roff = 0;
        return;
    }
    memmove(c->rbuf, c->rbuf + c->roff, c->rlen - c->roff);
    c->rlen -= c->roff;
    c->roff = 0;
}

/* ==================== 状态推导与待写缓冲（T2.3b） ==================== */

int conn_set_wbuf(conn *c, const void *buf, size_t len) {
    char *p = NULL;

    if (len > 0) {
        p = malloc(len);
        if (p == NULL) return -1;
        memcpy(p, buf, len);
    }
    free(c->wbuf); /* 旧的一定要放掉：这一步的字节已经取代它了 */
    c->wbuf = p;
    c->wlen = len;
    c->woff = 0;
    return 0;
}

conn_step conn_next_step(const conn *c) {
    if (c->phase == CONN_READ_REQ) return CONN_STEP_READ_REQ;
    if (c->phase == CONN_DRAIN) return CONN_STEP_DRAIN;
    if (c->phase == CONN_CLOSE) return CONN_STEP_CLOSE;
    /* 剩下 CONN_WRITE：按"先写完头、再发文件体、最后决定收尾/下一个请求"的顺序细分成几步 */
    if (c->woff < c->wlen) return CONN_STEP_WRITE_HEAD;
    if (c->file_left > 0) return CONN_STEP_SEND_FILE;
    if (c->drain_left > 0) return CONN_STEP_DRAIN; /* 有没读完的残留：先收尾（收尾完必然要关） */
    return c->keep_alive ? CONN_STEP_NEXT_REQ : CONN_STEP_CLOSE;
}

uint32_t conn_step_events(conn_step s) {
    switch (s) {
        case CONN_STEP_READ_REQ:
        case CONN_STEP_DRAIN:
        case CONN_STEP_NEXT_REQ:
            return EPOLLIN;
        case CONN_STEP_WRITE_HEAD:
        case CONN_STEP_SEND_FILE:
            return EPOLLOUT;
        case CONN_STEP_CLOSE:
            return 0; /* 不用关注任何事件：调用方马上就会收摊 */
    }
    return 0; /* 枚举只到这几种；真写漏了一种，这里返回 0 会立刻暴露成"事件循环不动了" */
}

void conn_reset_for_next_request(conn *c) {
    if (c->file_fd >= 0) { /* 上一个请求的文件体读游标已经没用了，fd 现在就得还 */
        close(c->file_fd);
        c->file_fd = -1;
    }
    free(c->wbuf);
    c->wbuf = NULL;
    c->wlen = 0;
    c->woff = 0;
    c->file_off = 0;
    c->file_left = 0;
    c->drain_left = 0;
    c->drain_deadline_ms = 0;
    c->keep_alive = 0; /* 下一个请求重新判断 */
    c->rscan = 0;      /* 残余字节已经被 conn_compact 挪到 rbuf[0]，从头扫 */
    c->roff = 0;
    c->phase = CONN_READ_REQ;
}

/* ============================ 非阻塞 IO ============================ */

int conn_set_nonblocking(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);

    if (fl < 0) return -1;
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

io_result io_read(int fd, void *buf, size_t cap, size_t *n) {
    *n = 0;
    if (cap == 0) return IO_OK; /* 读 0 字节是无操作，别把它误判成"对端关了" */
    for (;;) {
        ssize_t r = recv(fd, buf, cap, 0);

        if (r > 0) {
            *n = (size_t)r;
            return IO_OK;
        }
        if (r == 0) return IO_CLOSED;
        if (errno == EINTR) continue; /* 被信号打断：重试，不是错误 */
        if (errno == EAGAIN || errno == EWOULDBLOCK) return IO_WOULD_BLOCK;
        return IO_ERROR; /* ECONNRESET / EBADF… */
    }
}

io_result io_write(int fd, const void *buf, size_t len, size_t *n) {
    *n = 0;
    if (len == 0) return IO_OK; /* 同 io_read：0 字节不是错误 */
    for (;;) {
        ssize_t w = send(fd, buf, len, MSG_NOSIGNAL); /* 第 1 道防线：对端走了只回 EPIPE */

        if (w > 0) {
            *n = (size_t)w;
            return IO_OK;
        }
        if (w == 0) return IO_OK; /* 理论上不会发生，防御一下 */
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return IO_WOULD_BLOCK;
        return IO_ERROR; /* EPIPE / ECONNRESET：常态，关掉这条连接就行 */
    }
}

io_result io_read_file(int fd, void *buf, size_t cap, size_t *n) {
    *n = 0;
    if (cap == 0) return IO_OK;
    for (;;) {
        ssize_t r = read(fd, buf, cap); /* 普通文件用 read(2)：对普通文件 recv 会 ENOTSOCK */

        if (r > 0) {
            *n = (size_t)r;
            return IO_OK;
        }
        if (r == 0) return IO_CLOSED; /* 文件被截短了 */
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return IO_WOULD_BLOCK;
        return IO_ERROR;
    }
}

io_result io_sendfile(int out_fd, int in_fd, off_t *off, size_t count, size_t *n) {
    *n = 0;
    if (count == 0) return IO_OK;
    for (;;) {
        ssize_t s = sendfile(out_fd, in_fd, off, count); /* off 由内核推进，下次接着发 */

        if (s > 0) {
            *n = (size_t)s;
            return IO_OK;
        }
        if (s == 0) return IO_CLOSED; /* 文件读到头/被截短：body 已经发不全了，只能断连 */
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return IO_WOULD_BLOCK;
        return IO_ERROR; /* EPIPE / ECONNRESET / EINVAL… */
    }
}

int io_wait(int fd, short events, int timeout_ms) {
    struct pollfd p;

    p.fd = fd;
    p.events = events;
    p.revents = 0;
    for (;;) {
        int r = poll(&p, 1, timeout_ms);

        if (r > 0) return 1; /* 就绪（可能同时带 POLLHUP/POLLERR，交给读写去发现） */
        if (r == 0) return 0; /* 超时 */
        if (errno == EINTR) continue; /* 信号：重试；要不要退出由调用方看 g_stop */
        return -1; /* 真出错（fd 坏了） */
    }
}
