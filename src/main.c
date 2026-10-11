/* src/main.c — M1（阻塞式、串行、一次只伺候一个连接）的 I/O 与生命周期
 *
 * 职责边界（「三不」，迷茫的时候用这个自查）：
 *   不找空行     → http_scan_header_end()
 *   不切请求行   → http_request_parse()
 *   不碰路径安全 → file_open_under_root()
 * 本文件只做：收连接、读字节、写字节、关 fd、收信号、管 argv 与启动。
 * 自查法：这里出现 "\r\n" 的解析、出现 ".." 的判断、出现 404 的**判断逻辑**
 *        （只允许出现「错误码 → 状态码」的映射表），就是分层漏了。
 *
 * 为什么这一版「故意难看」（不是 bug，是 M2 的对照组）：
 *   - 一次只处理一条连接、处理完才 accept 下一条 → 慢客户端能拖死整台服务器（T1.7c 要留证）；
 *   - 一律 Connection: close，不在一条连接上处理第二个请求（keep-alive 是 T2.6）；
 *   - **不读请求体**：第一版只支持 GET/HEAD，有 Content-Length 也不读（写完响应直接关）。
 *
 * M2 的换装点：只有第一节那 3 个 IO 函数 + main 的循环。
 *   http.c / file.c 一行都不用改 —— 这就是「main.c 里没有协议知识」换来的东西。
 *
 * 用法：  ./build/debug/mini-httpd [ip] [port] [www-root]
 *         默认 127.0.0.1 8080 tests/www
 *
 * 注：realpath 属于 XSI 选项组，只定义 _POSIX_C_SOURCE 时 glibc 不声明它 → 用 _XOPEN_SOURCE 700。
 */
#define _XOPEN_SOURCE 700

#include "file.h"
#include "http.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* 命令行默认值：三段都可以省。e2e 脚本与 tests/bench.sh 都按这个约定调用，别乱改。 */
#define DEFAULT_IP   "127.0.0.1"
#define DEFAULT_PORT 8080
#define DEFAULT_ROOT "tests/www"

/* 「方法不允许」回哪个码：M1 验收契约（TASKS.md）+ v1 的 55 条断言都要求
 *   405 + "Allow: GET, HEAD"。
 * ⚠ 注意：src/httpd.h 的 MAX_METHOD_LEN 注释、src/http.h 的 HTTP_ERR_METHOD 注释里写的是 501。
 *   两边必须统一（改注释，或改 TASKS.md 契约与 e2e 断言），否则 T1.7a 移植来的 e2e 会有两条红。
 *   想拆得更细也行：method 语法非法/超长 → 400/501；method 合法但不在白名单 → 405。 */
#define STATUS_METHOD_NOT_ALLOWED 405

/* ------------------------------------------------------------------ 全局 */
static const char *g_root_real;              /* 文档根 realpath，启动时算一次 */
static volatile sig_atomic_t g_stop = 0;     /* 信号 handler 只写它（异步信号安全） */

static void on_stop(int sig)
{
    (void)sig;
    g_stop = 1;      /* handler 里不 printf、不 free、不 malloc */
}

/* ------------------------------------------------------------ 底层 IO 包装
 * M2 的换装点：T2.2 只把这三个函数改成「非阻塞 + EAGAIN 语义」，别的都不动。 */
static ssize_t conn_read(int fd, void *buf, size_t n)
{
    for (;;) {
        ssize_t r = recv(fd, buf, n, 0);
        if (r >= 0) return r;                 /* >0 字节数；0 = 对端关闭（EOF） */
        if (errno == EINTR) continue;         /* 被信号打断：重试，不是错误 */
        return -1;                            /* ECONNRESET 等：连接级错误，调用方关掉 */
    }
}

/* 写满为止（write 的返回值可能小于请求长度 —— 大文件必现）。
 * MSG_NOSIGNAL 是第 1 道防线：对端已走时只返回 EPIPE，不发 SIGPIPE。 */
static int write_all(int fd, const void *buf, size_t n)
{
    const char *p = buf;
    size_t off = 0;

    while (off < n) {
        ssize_t w = send(fd, p + off, n - off, MSG_NOSIGNAL);
        if (w > 0) { off += (size_t)w; continue; }
        if (w < 0 && errno == EINTR) continue;
        return -1;    /* EPIPE / ECONNRESET 是常态：关这条连接就行，不是致命错误 */
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

/* 收尾（lingering close）：先 shutdown(SHUT_WR)，再把对端还在路上的字节读掉，最后才 close。
 *
 * 为什么必须做（2026-10-11 实测）：接收队列里还有没读的字节就直接 close，
 * 内核发的是 **RST 而不是 FIN**；而客户端此时往往还在写它那条超大请求 ——
 * 写入失败（EPIPE/ECONNRESET）后，`printf … | nc` 这种测试客户端会直接退出，
 * 连我们已经发出的 431 都读不到（现象：服务端日志记了 431，客户端拿到空响应；
 * 换成会「先读完响应」的客户端就能收到 —— 所以问题在收尾方式，不在响应本身）。
 * 把剩下的字节读干净，close 才会是干净的 FIN。
 *
 * 必须有界：一个只连不发、或无限发送的客户端不能把服务器挂在这里。
 * SO_RCVTIMEO 200ms + 最多 1MB 双重上限。 */
static void conn_finish(int cfd)
{
    struct timeval tv;
    char tmp[4096];
    long long budget = 1024 * 1024;

    (void)shutdown(cfd, SHUT_WR);            /* 明确告诉对端：我不再写了 */
    tv.tv_sec = 0;
    tv.tv_usec = 200 * 1000;
    (void)setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    while (budget > 0) {
        ssize_t r = recv(cfd, tmp, sizeof tmp, 0);
        if (r <= 0) break;                   /* 0 = 对端也关了（干净）；-1 = 超时/出错，走人 */
        budget -= (long long)r;
    }
}

/* ------------------------------------------------------------------ 响应 */

static void send_error(int cfd, int status, const char *extra, int head_only)
{
    char body[256];
    int n = snprintf(body, sizeof body,
                     "<!doctype html>\n<html><head><meta charset=\"utf-8\">"
                     "<title>%d %s</title></head>\n<body><h1>%d %s</h1><hr>"
                     "<p>%s</p></body></html>\n",
                     status, http_status_text(status),
                     status, http_status_text(status), SERVER_VERSION);
    if (n < 0) n = 0;
    if ((size_t)n >= sizeof body) n = (int)(sizeof body - 1);

    char head[1024];
    int hn = http_build_response_head(head, sizeof head, status, "text/html; charset=utf-8",
                                      (long long)n, extra);
    if (hn < 0) return;                        /* 拼不下是 bug，但此刻没法再回 500，只能放弃 */
    if (write_all(cfd, head, (size_t)hn) < 0) return;
    if (!head_only && n > 0) (void)write_all(cfd, body, (size_t)n);
    /* 错误页只说状态码，不回显请求路径/绝对路径 —— 不泄漏服务器信息 */
}

static void send_file(int cfd, const struct static_file *sf, const char *ctype, int head_only)
{
    char head[1024];
    int hn = http_build_response_head(head, sizeof head, 200, ctype, sf->size, NULL);
    if (hn < 0) { send_error(cfd, 500, NULL, head_only); return; }
    if (write_all(cfd, head, (size_t)hn) < 0) return;
    if (head_only) return;                     /* HEAD：只发头，Content-Length 仍是真实大小 */

    char buf[64 * 1024];
    long long left = sf->size;
    while (left > 0) {
        size_t want = (left < (long long)sizeof buf) ? (size_t)left : sizeof buf;
        ssize_t r = file_read(sf->fd, buf, want);
        if (r <= 0) break;                     /* 文件被截短/读错：只能断连（头已发出去了） */
        if (write_all(cfd, buf, (size_t)r) < 0) return;
        left -= (long long)r;
    }
}

/* ------------------------------------------------------------------ 日志 */

/* method/target 是未信任输入，而且是「带长度的借用切片」：
 * 控制字符一律打掉，免得客户端用 \r 或 ANSI 序列伪造日志行、把终端搞花。 */
static void sanitize(char *dst, size_t cap, const char *src, size_t len)
{
    size_t i = 0;

    if (cap == 0) return;
    for (; i < len && i + 1 < cap; i++) {
        unsigned char c = (unsigned char)src[i];
        dst[i] = (c >= 32 && c < 127) ? (char)c : '?';
    }
    dst[i] = '\0';
}

static void log_request(const char *peer, const char *method, size_t method_len,
                        const char *target, size_t target_len, int status, long long bytes)
{
    char m[32];
    char t[256];

    sanitize(m, sizeof m, method ? method : "-", method ? method_len : 1);
    sanitize(t, sizeof t, target ? target : "-", target ? target_len : 1);
    printf("%-21s %-4s %-40s -> %d  %lldB\n", peer, m, t, status, bytes);
    fflush(stdout);      /* 不 flush 的话，管道里 grep 这行会一直等不到 */
}

/* ------------------------------------------------------------ 错误码映射
 * 整个程序只有这两张表：协议层/文件层只报「哪一类错」，由 main.c 决定回哪个码。 */
static int status_from_parse_err(enum http_err e)
{
    switch (e) {
    case HTTP_OK:                   return 200;
    case HTTP_ERR_BAD_REQUEST:      return 400;
    case HTTP_ERR_METHOD:           return STATUS_METHOD_NOT_ALLOWED;   /* 见文件顶部的说明 */
    case HTTP_ERR_URI_TOO_LONG:     return 414;
    case HTTP_ERR_HEADER_TOO_LARGE: return 431;
    case HTTP_ERR_TOO_MANY_HEADERS: return 431;
    case HTTP_ERR_BODY_TOO_LARGE:   return 413;
    case HTTP_ERR_VERSION:          return 505;
    case HTTP_ERR_INTERNAL:         return 500;
    }
    return 400;      /* 解析失败一律算客户端问题：宁可 400，别回 500 把锅甩给自己 */
}

static int status_from_file_err(enum file_status e)
{
    switch (e) {
    case FILE_OK:        return 200;
    case FILE_BAD_PATH:  return 400;
    case FILE_FORBIDDEN: return 403;
    case FILE_NOT_FOUND: return 404;
    case FILE_IO_ERROR:  return 500;
    }
    return 500;
}

/* -------------------------------------------------------------- 一条连接 */

/* 返回：1 = 接收队列里可能还有没读完的字节 → 调用方**先 conn_finish(cfd) 再 close**；
 *       0 = 收干净了，直接 close 就行。 */
static int handle_conn(int cfd, const char *peer, char *rbuf)
{
    http_request req;                 /* 注意：http.h 里是匿名 typedef，没有 struct tag —— 别写 struct http_request */
    struct static_file sf;
    size_t rlen = 0;
    size_t scan = 0;
    size_t head_len = 0;
    int over_limit = 0;
    int drain = 0;
    int status = 500;
    const char *extra = NULL;
    enum http_err pe;
    enum file_status fe;
    ssize_t n;

    memset(&req, 0, sizeof req);
    memset(&sf, 0, sizeof sf);
    sf.fd = -1;

    /* 阶段 1：攒到「空行」为止。
     * 协议知识全在 http_scan_header_end 里，这里只负责「再读一点 / 到上限就别读了」。 */
    for (;;) {
        if (rlen >= RBUF_SIZE) { over_limit = 1; break; }
        n = conn_read(cfd, rbuf + rlen, RBUF_SIZE - rlen);
        if (n <= 0) goto done;                 /* 没发完就断了/出错了：静默关闭，什么都不回 */
        rlen += (size_t)n;
        if (http_scan_header_end(rbuf, rlen, &scan, &head_len)) break;
        if (rlen >= MAX_HEADER_LEN) { over_limit = 1; break; }   /* 提前判 431，别等 rbuf 满 */
    }

    if (over_limit) {
        send_error(cfd, 431, NULL, 0);
        log_request(peer, NULL, 0, NULL, 0, 431, 0);
        drain = 1;                     /* 我们是「读一半就走」的：对端大概率还在写 */
        goto done;
    }

    /* 阶段 2：解析。失败时解析器保证不留已分配内存，所以这里不需要回滚。 */
    pe = http_request_parse(rbuf, head_len, &req);
    if (pe != HTTP_OK) {
        status = status_from_parse_err(pe);
        extra = (status == STATUS_METHOD_NOT_ALLOWED) ? "Allow: GET, HEAD\r\n" : NULL;
        send_error(cfd, status, extra, req.head_only);
        log_request(peer, req.method, req.method_len, req.target, req.target_len, status, 0);
        drain = (req.content_length > 0);   /* 第一版不读 body：声明了 body 就还有没读的字节 */
        goto done;
    }

    /* 阶段 3：映射到文件（路径安全全在 file.c 里，这里一个字都不判）。 */
    fe = file_open_under_root(g_root_real, req.path, &sf);
    if (fe != FILE_OK) {
        status = status_from_file_err(fe);
        send_error(cfd, status, NULL, req.head_only);
        log_request(peer, req.method, req.method_len, req.target, req.target_len, status, 0);
        drain = (req.content_length > 0);
        goto done;
    }

    send_file(cfd, &sf, http_content_type(sf.real), req.head_only);
    log_request(peer, req.method, req.method_len, req.target, req.target_len,
                200, req.head_only ? 0 : sf.size);
    drain = (req.content_length > 0);

done:                                  /* 单一出口：所有分支都从这里出去，fd/free 不会漏 */
    if (sf.fd >= 0) close(sf.fd);      /* sf.fd 归 handle_conn 管；cfd 归 main 管 */
    http_request_free(&req);
    return drain;
}

/* ------------------------------------------------------------------ 启动 */

static void usage(const char *argv0, FILE *out)
{
    fprintf(out,
            "用法: %s [ip] [port] [www-root]\n"
            "  默认: %s %d %s\n"
            "  例:   %s 0.0.0.0 8080 tests/www\n",
            argv0, DEFAULT_IP, DEFAULT_PORT, DEFAULT_ROOT, argv0);
}

/* 解析 argv（三段都可省）。失败只返回错误码，**不 exit** —— 由 main 决定怎么退出。
 * 返回：0 = 成功；1 = 请求帮助（-h/--help）；-1 = 参数非法。 */
static int parse_args(int argc, char **argv, const char **ip, int *port, const char **root)
{
    *ip   = DEFAULT_IP;
    *port = DEFAULT_PORT;
    *root = DEFAULT_ROOT;

    if (argc > 1 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) return 1;
    if (argc > 4) return -1;

    if (argc > 2) {
        char *end = NULL;
        long v;

        errno = 0;
        v = strtol(argv[2], &end, 10);
        /* strtol 会跳过前导空白、接受 '+'/'-'：所以必须查 endptr 与范围，
         * 这样 " 80" 被容忍、而 "80 " / "80abc" / "" / "0" / "65536" 都被挡住。 */
        if (errno != 0 || end == argv[2] || *end != '\0' || v <= 0 || v > 65535) return -1;
        *port = (int)v;
    }
    if (argc > 1) *ip = argv[1];
    if (argc > 3) *root = argv[3];
    return 0;
}

static int make_listener(const char *ip, int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int on = 1;
    struct sockaddr_in addr;

    if (fd < 0) { perror("socket"); return -1; }

    /* SO_REUSEADDR：让 TIME_WAIT 的旧四元组不挡住重启（M0 的 echo 里已实测过它的边界） */
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on) < 0) {
        perror("setsockopt(SO_REUSEADDR)");
        close(fd);
        return -1;
    }

    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
        fprintf(stderr, "非法 ip: %s\n", ip);
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
    return fd;
}

static void describe_peer(int fd, char *out, size_t cap)
{
    struct sockaddr_in sa;
    socklen_t sl = sizeof sa;

    if (getpeername(fd, (struct sockaddr *)&sa, &sl) == 0 && sa.sin_family == AF_INET) {
        char ip[INET_ADDRSTRLEN] = "?";

        if (inet_ntop(AF_INET, &sa.sin_addr, ip, sizeof ip) == NULL) {
            snprintf(ip, sizeof ip, "?");
        }
        snprintf(out, cap, "%s:%u", ip, (unsigned)ntohs(sa.sin_port));
    } else {
        snprintf(out, cap, "?");      /* 取不到对端信息也不能让日志把服务搞崩 */
    }
}

int main(int argc, char **argv)
{
    const char *ip = NULL;
    const char *root = NULL;
    int port = 0;
    int pa;
    char root_real[FILE_MAX_PATH];
    struct stat st;
    struct sigaction sa;
    int lfd;
    char *rbuf;
    char peer[64];

    /* 1) argv */
    pa = parse_args(argc, argv, &ip, &port, &root);
    if (pa == 1) { usage(argv[0], stdout); return 0; }
    if (pa < 0) { usage(argv[0], stderr); return 2; }

    /* 2) 文档根在 bind 之前解析一次，失败就别起服务了 */
    if (realpath(root, root_real) == NULL) {
        fprintf(stderr, "realpath(%s): %s\n", root, strerror(errno));
        return 1;
    }
    if (stat(root_real, &st) < 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "%s 不是目录\n", root_real);
        return 1;
    }
    g_root_real = root_real;

    /* 3) 信号：SIGPIPE 是第 2 道防线（第 1 道是 send 的 MSG_NOSIGNAL） */
    memset(&sa, 0, sizeof sa);
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = SIG_IGN;
    sa.sa_flags = 0;
    (void)sigaction(SIGPIPE, &sa, NULL);

    /* ★ sa_flags = 0：**故意不设 SA_RESTART**。
     * 阻塞在 accept() 里被信号打断时，我们要靠 EINTR 醒过来看一眼 g_stop。
     * 本机实测：带 SA_RESTART 时 accept 会被内核自动重启 —— kill -TERM 之后进程
     * 会一直卡住，直到下一个连接到来才退出（T1.7e 那条「SIGTERM → exit 0」就会挂）。
     * （顺带：glibc 的 signal() 是 BSD 语义、默认带 SA_RESTART，所以这里不能用 signal()。） */
    sa.sa_handler = on_stop;
    sa.sa_flags = 0;
    (void)sigaction(SIGINT, &sa, NULL);
    (void)sigaction(SIGTERM, &sa, NULL);

    /* 4) 监听 */
    lfd = make_listener(ip, port);
    if (lfd < 0) return 1;

    /* e2e 脚本靠这句话判断服务是否就绪：措辞别改，而且要 flush（管道是全缓冲的） */
    printf("listening on %s:%d  (www-root=%s)\n", ip, port, g_root_real);
    fflush(stdout);

    /* 5) 读缓冲：串行模型的一个好处就是可以复用一份（M2 才改成每连接一份 struct conn） */
    rbuf = malloc(RBUF_SIZE);
    if (rbuf == NULL) {
        fprintf(stderr, "malloc(%d) 失败\n", (int)RBUF_SIZE);
        close(lfd);
        return 1;
    }

    /* 6) 串行 accept 循环：一次一条连接，处理完才收下一条（M2 的对照组） */
    while (!g_stop) {
        int cfd = accept(lfd, NULL, NULL);

        if (cfd < 0) {
            if (errno == EINTR || errno == ECONNABORTED) continue;   /* EINTR：该看 g_stop 了 */
            if (errno == EMFILE || errno == ENFILE) {
                /* fd 耗尽：记日志 + 短暂退避，**绝不退出** —— 退出等于把整个服务丢了 */
                struct timespec ts;

                fprintf(stderr, "accept: %s（fd 耗尽，退避 100ms 后继续）\n", strerror(errno));
                ts.tv_sec = 0;
                ts.tv_nsec = 100L * 1000 * 1000;
                (void)nanosleep(&ts, NULL);
                continue;
            }
            fprintf(stderr, "accept: %s\n", strerror(errno));
            continue;                          /* 连接级错误不该杀服务器 */
        }

        describe_peer(cfd, peer, sizeof peer);
        if (handle_conn(cfd, peer, rbuf) != 0) {
            conn_finish(cfd);                  /* 收尾：别让对端还在写的字节把响应顶掉（见 conn_finish） */
        }
        close(cfd);                            /* cfd 的生命周期只归 main 管 */
    }

    printf("shutdown: 收到信号，关闭监听 fd 并正常退出\n");
    fflush(stdout);
    close(lfd);
    free(rbuf);
    return 0;
}
