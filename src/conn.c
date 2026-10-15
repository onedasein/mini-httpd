/* src/conn.c — 连接表（T2.1b）、残余搬移（T2.1c）、非阻塞 IO 包装（T2.2a）
 *
 * 设计取舍记在 docs/adr/003-连接表与conn模块.md：
 *   · fd → conn 用「按 fd 索引的指针数组」，连接对象每连接 calloc 一次（M4 再考虑固定池）；
 *   · 连接对象**拥有 fd**（conn_free 负责 close）—— 所以 T2.5 引入 epoll 时，
 *     epoll_ctl(DEL) 必须由调用方在 conn_free 之前做。
 */
#include "conn.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

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
    c->last_active = time(NULL);
    g_by_fd[fd] = c;
    return c;
}

void conn_free(conn *c) {
    if (c == NULL) return;
    /* 槽位校验：只有"还指着自己"时才清，防止把别人的槽位清掉（串号的第二道防线） */
    if (c->fd >= 0 && c->fd < CONN_MAX_FD && g_by_fd[c->fd] == c) g_by_fd[c->fd] = NULL;
    if (c->fd >= 0) close(c->fd); /* 连接对象拥有 fd（T2.5 要在这之前 epoll_ctl(DEL)） */
    c->fd = -1;
    free(c->wbuf);
    free(c);
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

int io_wait(int fd, short events, int timeout_ms) {
    struct pollfd p;

    p.fd = fd;
    p.events = events;
    p.revents = 0;
    for (;;) {
        int r = poll(&p, 1, timeout_ms);

        if (r > 0) return 1;          /* 就绪（可能同时带 POLLHUP/POLLERR，交给读写去发现） */
        if (r == 0) return 0;         /* 超时 */
        if (errno == EINTR) continue; /* 信号：重试；要不要退出由调用方看 g_stop */
        return -1;                    /* 真出错（fd 坏了） */
    }
}
