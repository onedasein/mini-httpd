/* main.c — v1（M1）阻塞式、串行、一次只伺候一个连接的 HTTP/1.1 静态服务器
 *
 * 用法：  ./build/debug/httpd_v1 [ip] [port] [www-root]     （默认 127.0.0.1 8080 www）
 *
 * 为什么这一版「故意难看」：
 *   - 一次只处理一条连接，处理完才 accept 下一条 → 慢客户端能拖死整台服务器。
 *     这不是 bug，是 M2（epoll ET reactor）的对照组，T2.2 要拿它复现「全服卡死」。
 *   - 一律回 Connection: close，不在一条连接上处理第二个请求（keep-alive 是 T2.6）。
 *
 * 又是为什么 fd 全都设成非阻塞：
 *   - 「阻塞」指的是**架构**（一次一条连接），不是指「允许 syscall 挂住进程」。
 *     从第一天起就用 O_NONBLOCK + poll 等就绪，read/write 都要能接住 EAGAIN，
 *     这样 M2 换 epoll 时只是换掉「等就绪」的那一行，IO 包装层不用重写。
 *   - DSH 交接稿 §5 的原话：「哪怕 v1 是阻塞式模型，也建议 fd 非阻塞 + 自己处理 EAGAIN」。
 *
 * 注：realpath 属于 XSI 选项组，需要 _XOPEN_SOURCE 700（含 POSIX.1-2008）。
 */
#define _XOPEN_SOURCE 700

#include "file.h"
#include "http.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static const char *g_root_real;      /* www 根的 realpath，启动时算一次 */

/* ---------------------------------------------------------- 底层 IO 包装 */

static int set_nonblocking(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) return -1;
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

/* 收到 SIGINT/SIGTERM 就置标志、跳出 accept 循环、正常 return。
 * 为什么要这个：进程若是被 kill -9 掉的，LeakSanitizer/valgrind 根本没机会做退出检查，
 * 「无泄漏」就只能靠数 fd。优雅退出之后，泄漏检查才是真结论（handler 里只写标志，不 printf）。 */
static volatile sig_atomic_t g_stop = 0;

static void on_stop(int sig)
{
    (void)sig;
    g_stop = 1;
}

/* 等 fd 就绪。v1 的 HTTPD_IDLE_MS 是 -1 = 无限等，也就是「阻塞」的那一半。 */
static int wait_fd(int fd, short events)
{
    struct pollfd p;
    p.fd = fd;
    p.events = events;
    p.revents = 0;
    for (;;) {
        int r = poll(&p, 1, HTTPD_IDLE_MS);
        if (r > 0) return 0;
        if (r == 0) return -1;                 /* 超时（v1 不会发生） */
        if (errno == EINTR) {
            if (g_stop) return -1;             /* 被信号打断：要退出了就别再等 */
            continue;                          /* 否则重试，不是错误 */
        }
        return -1;
    }
}

/* 非阻塞 recv：>0 字节数；0 = 对端 close（EOF）；-1 = 真错误 */
static ssize_t conn_read(int fd, void *buf, size_t n)
{
    for (;;) {
        ssize_t r = recv(fd, buf, n, 0);
        if (r >= 0) return r;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (wait_fd(fd, POLLIN) < 0) return -1;
            continue;
        }
        return -1;                             /* ECONNRESET 等：连接级错误，交给调用方关掉 */
    }
}

/* 写满为止。MSG_NOSIGNAL 让「对端已走」只返回 EPIPE 而不发 SIGPIPE；
 * 进程级还额外 SIG_IGN 兜底（两道防线，见 M0 的 SIGPIPE 结论）。 */
static int write_all(int fd, const void *buf, size_t n)
{
    const char *p = buf;
    size_t off = 0;
    while (off < n) {
        ssize_t w = send(fd, p + off, n - off, MSG_NOSIGNAL);
        if (w > 0) { off += (size_t)w; continue; }
        if (w < 0 && errno == EINTR) continue;
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (wait_fd(fd, POLLOUT) < 0) return -1;
            continue;
        }
        return -1;      /* EPIPE / ECONNRESET / ETIMEDOUT：正常现象，关这条连接就行 */
    }
    return 0;
}

/* 普通文件用 read(2)，别用 recv(2)（对普通文件 recv 会 ENOTSOCK） */
static ssize_t file_read(int fd, void *buf, size_t n)
{
    for (;;) {
        ssize_t r = read(fd, buf, n);
        if (r >= 0) return r;
        if (errno == EINTR) continue;
        return -1;
    }
}

/* ---------------------------------------------------------------- 响应 */

static void send_error(int cfd, int status, const char *extra, int head_only)
{
    char body[256];
    int n = snprintf(body, sizeof body,
                     "<!doctype html>\n<html><head><meta charset=\"utf-8\">"
                     "<title>%d %s</title></head>\n<body><h1>%d %s</h1><hr>"
                     "<p>%s</p></body></html>\n",
                     status, http_status_text(status),
                     status, http_status_text(status), HTTPD_SERVER_NAME);
    if (n < 0) n = 0;
    if ((size_t)n >= sizeof body) n = (int)(sizeof body - 1);

    char hdr[1024];
    int hn = http_build_header(hdr, sizeof hdr, status, "text/html; charset=utf-8",
                               (long long)n, extra);
    if (hn < 0) return;
    if (write_all(cfd, hdr, (size_t)hn) < 0) return;
    if (!head_only && n > 0) write_all(cfd, body, (size_t)n);
    /* 错误页只说状态码，不回显请求路径/绝对路径 —— 不泄漏服务器信息（§8） */
}

static void send_file(int cfd, const struct static_file *sf, const char *ctype, int head_only)
{
    char hdr[1024];
    int hn = http_build_header(hdr, sizeof hdr, 200, ctype, sf->size, NULL);
    if (hn < 0) { send_error(cfd, 500, NULL, head_only); return; }
    if (write_all(cfd, hdr, (size_t)hn) < 0) return;
    if (head_only) return;                     /* T1.6：HEAD 只发头，不发体 */

    char buf[64 * 1024];
    long long left = sf->size;
    while (left > 0) {
        size_t want = (left < (long long)sizeof buf) ? (size_t)left : sizeof buf;
        ssize_t r = file_read(sf->fd, buf, want);
        if (r <= 0) break;                     /* 文件被截短了，只能断连 */
        if (write_all(cfd, buf, (size_t)r) < 0) return;
        left -= (long long)r;
    }
}

/* ---------------------------------------------------------------- 日志 */

static void sanitize(char *dst, size_t cap, const char *src)
{
    size_t i = 0;
    for (; src[i] != '\0' && i + 1 < cap; i++) {
        unsigned char c = (unsigned char)src[i];
        dst[i] = (c >= 32 && c < 127) ? (char)c : '?';   /* 防日志伪造：控制字符一律打掉 */
    }
    dst[i] = '\0';
}

static void log_request(const char *peer, const char *method, const char *target,
                        int status, long long bytes)
{
    char m[32], t[256];
    sanitize(m, sizeof m, method ? method : "-");
    sanitize(t, sizeof t, target ? target : "-");
    printf("%-21s %-4s %-40s -> %d  %lldB\n", peer, m, t, status, bytes);
    fflush(stdout);
}

/* ------------------------------------------------------------ 一条连接 */

static int parse_status_to_http(enum http_parse_status st)
{
    switch (st) {
    case HTTP_PARSE_OK:                 return 200;
    case HTTP_PARSE_BAD_REQUEST:        return 400;
    case HTTP_PARSE_URI_TOO_LONG:       return 414;
    case HTTP_PARSE_HEADER_TOO_LARGE:   return 431;
    case HTTP_PARSE_METHOD_NOT_ALLOWED: return 405;
    case HTTP_PARSE_VERSION_UNSUPPORTED:return 505;
    case HTTP_PARSE_PAYLOAD_TOO_LARGE:  return 413;
    }
    return 400;
}

static void handle_conn(int cfd, const char *peer)
{
    char *rbuf = malloc(HTTPD_RBUF_SIZE);
    if (!rbuf) { send_error(cfd, 500, NULL, 0); return; }

    struct http_request req;
    memset(&req, 0, sizeof req);

    struct static_file sf;
    memset(&sf, 0, sizeof sf);
    sf.fd = -1;

    size_t rlen = 0, scan = 0, hdr_end = 0;
    int over_limit = 0;

    /* 1) 攒到「空行」为止；攒到上限还没攒到 → 431 */
    for (;;) {
        if (rlen >= HTTPD_RBUF_SIZE) { over_limit = 1; break; }
        ssize_t n = conn_read(cfd, rbuf + rlen, HTTPD_RBUF_SIZE - rlen);
        if (n <= 0) goto done;                 /* 请求没发完就断了/出错了：静默关闭 */
        rlen += (size_t)n;
        hdr_end = http_scan_header_end(rbuf, rlen, &scan);
        if (hdr_end > 0) break;
        if (rlen >= HTTPD_MAX_HEADER) { over_limit = 1; break; }
    }

    if (over_limit) {
        send_error(cfd, 431, NULL, 0);
        log_request(peer, "-", "-", 431, 0);
        goto done;
    }

    /* 2) 解析 */
    enum http_parse_status ps = http_parse_request(rbuf, hdr_end, &req);
    if (ps != HTTP_PARSE_OK) {
        int st = parse_status_to_http(ps);
        const char *extra = (st == 405) ? "Allow: GET, HEAD\r\n" : NULL;
        send_error(cfd, st, extra, req.head_only);
        log_request(peer, req.method, req.target, st, 0);
        goto done;
    }

    /* 3) 映射到文件（路径安全在 file.c 里） */
    enum file_status fs = file_open_under_root(g_root_real, req.path, &sf);
    int st;
    switch (fs) {
    case FILE_OK:        st = 200; break;
    case FILE_BAD_PATH:  st = 400; break;
    case FILE_FORBIDDEN: st = 403; break;
    case FILE_NOT_FOUND: st = 404; break;
    default:             st = 500; break;
    }

    if (st != 200) {
        send_error(cfd, st, NULL, req.head_only);
        log_request(peer, req.method, req.target, st, 0);
        goto done;
    }

    /* 4) 200：响应头 + 文件体 */
    send_file(cfd, &sf, http_content_type(sf.real), req.head_only);
    log_request(peer, req.method, req.target, 200, req.head_only ? 0 : sf.size);
    if (!req.keep_alive) { /* v1 一律按 close 处理，这里只是把语义写清楚 */ }

done:
    if (sf.fd >= 0) close(sf.fd);
    http_request_free(&req);
    free(rbuf);
}

/* ------------------------------------------------------------------ 启动 */

static int make_listener(const char *ip, const char *port_str)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }

    int on = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on) < 0) {
        perror("setsockopt(SO_REUSEADDR)");
        close(fd);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    long port = strtol(port_str, NULL, 10);
    if (port <= 0 || port > 65535) { fprintf(stderr, "bad port: %s\n", port_str); close(fd); return -1; }
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
        fprintf(stderr, "bad ip: %s\n", ip);
        close(fd);
        return -1;
    }

    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }
    if (listen(fd, 128) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }
    if (set_nonblocking(fd) < 0) {             /* 监听 fd 也非阻塞 */
        perror("fcntl(O_NONBLOCK)");
        close(fd);
        return -1;
    }
    printf("listening on %s:%s  (www-root=%s)\n", ip, port_str, g_root_real);
    fflush(stdout);
    return fd;
}

static void describe_peer(int fd, char *out, size_t cap)
{
    struct sockaddr_in sa;
    socklen_t sl = sizeof sa;
    if (getpeername(fd, (struct sockaddr *)&sa, &sl) == 0 && sa.sin_family == AF_INET) {
        char ip[INET_ADDRSTRLEN] = "?";
        inet_ntop(AF_INET, &sa.sin_addr, ip, sizeof ip);
        snprintf(out, cap, "%s:%u", ip, (unsigned)ntohs(sa.sin_port));
    } else {
        snprintf(out, cap, "?");
    }
}

int main(int argc, char **argv)
{
    const char *ip   = (argc > 1) ? argv[1] : "127.0.0.1";
    const char *port = (argc > 2) ? argv[2] : "8080";
    const char *root = (argc > 3) ? argv[3] : "www";

    /* 客户端半路断开是常态：write 触发的 SIGPIPE 默认动作是杀死整个进程（§8 必做项）。
     * 这里再忽略一次是「第二道防线」——第一道是 send(MSG_NOSIGNAL)。 */
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, on_stop);
    signal(SIGTERM, on_stop);

    char root_real[HTTPD_MAX_PATH];
    if (!realpath(root, root_real)) {
        fprintf(stderr, "realpath(%s): %s\n", root, strerror(errno));
        return 1;
    }
    struct stat st;
    if (stat(root_real, &st) < 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "%s 不是目录\n", root_real);
        return 1;
    }
    g_root_real = root_real;

    int lfd = make_listener(ip, port);
    if (lfd < 0) return 1;

    while (!g_stop) {
        if (wait_fd(lfd, POLLIN) < 0) continue;     /* EINTR 重试；收到信号时返回 -1，循环条件退出 */

        int cfd = accept(lfd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR || errno == ECONNABORTED) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            if (errno == EMFILE || errno == ENFILE) {
                /* 致命级：fd 耗尽。记日志 + 短暂退避，**绝不退出**（退了就把服务全丢了） */
                fprintf(stderr, "accept: %s（fd 耗尽，退避 100ms 后继续）\n", strerror(errno));
                struct timespec ts;
                ts.tv_sec = 0;
                ts.tv_nsec = 100 * 1000 * 1000L;
                nanosleep(&ts, NULL);
                continue;
            }
            fprintf(stderr, "accept: %s\n", strerror(errno));
            continue;                               /* 连接级错误不该杀服务器 */
        }

        if (set_nonblocking(cfd) < 0) { close(cfd); continue; }

        char peer[64];
        describe_peer(cfd, peer, sizeof peer);
        handle_conn(cfd, peer);
        close(cfd);
    }

    printf("shutdown: 收到信号，关闭监听 fd 并正常退出\n");
    close(lfd);
    return 0;
}
