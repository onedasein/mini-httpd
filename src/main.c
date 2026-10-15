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

#include "conn.h"
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
#define DEFAULT_IP                    "127.0.0.1"
#define DEFAULT_PORT                  8080
#define DEFAULT_ROOT                  "tests/www"

/* 「方法不允许」回哪个码：M1 验收契约（TASKS.md）+ v1 的 55 条断言都要求
 *   405 + "Allow: GET, HEAD"。
 *   method 语法非法 → 400；
 *   method 在已知名单但不在白名单 → 405。
 *   method 超长或小写-> 501;
 */
#define STATUS_METHOD_SYNTAX_ERROR    400
#define STATUS_METHOD_NOT_ALLOWED     405
#define STATUS_METHOD_NOT_IMPLEMENTED 501

/* 非阻塞下「等一会儿」的轮询粒度（ms）。三处用它：
 *   ① wait_readable / write_all 等就绪时顺手检查 g_stop（不然 SIGTERM 正好落在 poll 里就退不出去）；
 *   ② conn_finish 的收尾上界（200ms 内没数据就走）；③ M3 的空闲超时也照这个路子做。 */
#define WAIT_TICK_MS 200

/* 连接对象（struct conn）与 IO 包装住在 src/conn.h / conn.c —— 抽成模块是因为
 * T2.1b/T2.1c/T2.2a 的验收都要单元测试，而测试只链接 LIB_SRC（不含 main.c）。 */
/* ------------------------------------------------------------------ 全局 */
static const char *g_root_real;              /* 文档根 realpath，启动时算一次 */
static volatile sig_atomic_t g_stop = 0;     /* 信号 handler 只写它（异步信号安全） */

static void on_stop(int sig) {
    (void)sig;
    g_stop = 1;      /* handler 里不 printf、不 free、不 malloc */
}

/* ------------------------------------------------------------ 底层 IO 包装
 * 真正的非阻塞语义在 src/conn.c 的 io_read / io_write / io_read_file 里（T2.2a）。
 * 这里只放这一层的两个**组合器**：写满为止、等可以读。 */

/* 写满为止（write 会短写 —— 大文件必现）。非阻塞下写不进去就 poll 等 POLLOUT。
 * MSG_NOSIGNAL 在 io_write 里（第 1 道防线），所以对端已走只会得到 IO_ERROR，不会吃到 SIGPIPE。 */
static int write_all(int fd, const void *buf, size_t n) {
    const char *p = buf;
    size_t off = 0;

    while (off < n) {
        size_t sent = 0;
        io_result r = io_write(fd, p + off, n - off, &sent);

        if (r == IO_OK) {
            off += sent;
            continue;
        }
        if (r == IO_WOULD_BLOCK) {
            if (io_wait(fd, POLLOUT, WAIT_TICK_MS) < 0 || g_stop) return -1;
            continue;
        }
        return -1; /* IO_CLOSED / IO_ERROR：EPIPE、ECONNRESET 是常态，关这条连接就行 */
    }
    return 0;
}

/* 等"可以读了"。返回 0 = 就绪；-1 = 该收摊了（收到信号，或 poll 报错）。
 * 用 WAIT_TICK_MS 的短超时轮询 g_stop —— **不能无限等**，否则 SIGTERM 正好落在这里就退不出去。
 * （T2.3 起这个函数退休：等待统一由 epoll_wait 负责，超时用来检查 g_stop / 空闲超时。） */
static int wait_readable(int fd) {
    while (!g_stop) {
        int r = io_wait(fd, POLLIN, WAIT_TICK_MS);

        if (r == 1) return 0; /* 就绪 */
        if (r < 0) return -1; /* poll 出错（fd 坏了）：收摊 */
    }
    return -1;
}

/* 收尾（lingering close）：先 shutdown(SHUT_WR)，再把对端还在路上的字节读掉，最后才 close。
 *
 * 为什么必须做（2026-10-11 实测）：接收队列里还有没读的字节就直接 close，内核发的是 **RST 而不是 FIN**；
 * 客户端此时往往还在写它那条超大请求 —— 写入失败（EPIPE/ECONNRESET）后，`printf … | nc` 这种
 * 测试客户端会直接退出，连我们已经发出的 431 都读不到。把剩下的字节读干净，close 才是干净的 FIN。
 *
 * 必须有界：200ms 没新数据就走 + 总量最多 1MB。
 * 注：fd 现在**是非阻塞**的，所以旧的 SO_RCVTIMEO 已经不起作用了 —— 超时改由 io_wait 的 poll 超时承担。 */
static void conn_finish(int cfd) {
    char tmp[4096];
    long long budget = 1024 * 1024;

    (void)shutdown(cfd, SHUT_WR); /* 明确告诉对端：我不再写了 */
    while (budget > 0) {
        size_t got = 0;
        io_result r = io_read(cfd, tmp, sizeof tmp, &got);

        if (r == IO_OK) {
            budget -= (long long)got;
            continue;
        }
        if (r == IO_WOULD_BLOCK) {
            if (io_wait(cfd, POLLIN, WAIT_TICK_MS) != 1) break; /* 200ms 没数据：走人 */
            continue;
        }
        break; /* IO_CLOSED（对端也关了，干净）或 IO_ERROR */
    }
}

/* ------------------------------------------------------------------ 响应 */

static void send_error(int cfd, int status, const char *extra, int head_only) {
    char body[256];
    int n = snprintf(
        body,
        sizeof body,
        "<!doctype html>\n<html><head><meta charset=\"utf-8\">"
        "<title>%d %s</title></head>\n<body><h1>%d %s</h1><hr>"
        "<p>%s</p></body></html>\n",
        status,
        http_status_text(status),
        status,
        http_status_text(status),
        SERVER_VERSION
    );
    if (n < 0) n = 0;
    if ((size_t)n >= sizeof body) n = (int)(sizeof body - 1);

    char head[1024];
    int hn = http_build_response_head(head, sizeof head, status, "text/html; charset=utf-8", (long long)n, extra);
    if (hn < 0) return;                        /* 拼不下是 bug，但此刻没法再回 500，只能放弃 */
    if (write_all(cfd, head, (size_t)hn) < 0) return;
    if (!head_only && n > 0) (void)write_all(cfd, body, (size_t)n);
    /* 错误页只说状态码，不回显请求路径/绝对路径 —— 不泄漏服务器信息 */
}

static void send_file(int cfd, const static_file *sf, const char *ctype, int head_only) {
    char head[1024];
    int hn = http_build_response_head(head, sizeof head, 200, ctype, sf->size, NULL);
    if (hn < 0) {
        send_error(cfd, 500, NULL, head_only);
        return;
    }
    if (write_all(cfd, head, (size_t)hn) < 0) return;
    if (head_only) return;                     /* HEAD：只发头，Content-Length 仍是真实大小 */

    char buf[64 * 1024];
    long long left = sf->size;
    while (left > 0) {
        size_t want = (left < (long long)sizeof buf) ? (size_t)left : sizeof buf;
        size_t got = 0;

        if (io_read_file(sf->fd, buf, want, &got) != IO_OK || got == 0) {
            break;                             /* 文件被截短/读错：只能断连（头已发出去了） */
        }
        if (write_all(cfd, buf, got) < 0) return;
        left -= (long long)got;
    }
}

/* ------------------------------------------------------------------ 日志 */

/* method/target 是未信任输入，而且是「带长度的借用切片」：
 * 控制字符一律打掉，免得客户端用 \r 或 ANSI 序列伪造日志行、把终端搞花。 */
static void sanitize(char *dst, size_t cap, const char *src, size_t len) {
    size_t i = 0;

    if (cap == 0) return;
    for (; i < len && i + 1 < cap; i++) {
        unsigned char c = (unsigned char)src[i];
        dst[i] = (c >= 32 && c < 127) ? (char)c : '?';
    }
    dst[i] = '\0';
}

static void log_request(
    const char *peer, const char *method, size_t method_len, const char *target, size_t target_len, int status,
    long long bytes
) {
    char m[32];
    char t[256];

    sanitize(m, sizeof m, method ? method : "-", method ? method_len : 1);
    sanitize(t, sizeof t, target ? target : "-", target ? target_len : 1);
    printf("%-21s %-4s %-40s -> %d  %lldB\n", peer, m, t, status, bytes);
    fflush(stdout);      /* 不 flush 的话，管道里 grep 这行会一直等不到 */
}

/* ------------------------------------------------------------ 错误码映射
 * 整个程序只有这两张表：协议层/文件层只报「哪一类错」，由 main.c 决定回哪个码。 */
static int status_from_parse_err(http_err e) {
    switch (e) {
        case HTTP_OK:
            return 200;
        case HTTP_ERR_BAD_REQUEST:
            return 400;
        case HTTP_ERR_METHOD:
            return STATUS_METHOD_NOT_ALLOWED;   /* 见文件顶部的说明 */
        case HTTP_ERR_URI_TOO_LONG:
            return 414;
        case HTTP_ERR_HEADER_TOO_LARGE:
            return 431;
        case HTTP_ERR_TOO_MANY_HEADERS:
            return 431;
        case HTTP_ERR_BODY_TOO_LARGE:
            return 413;
        case HTTP_ERR_VERSION:
            return 505;
        case HTTP_ERR_INTERNAL:
            return 500;
        case HTTP_ERR_NOT_IMPLEMENTED:
            return 501;
    }
    return 400;      /* 解析失败一律算客户端问题：宁可 400，别回 500 把锅甩给自己 */
}

static int status_from_file_err(file_status e) {
    switch (e) {
        case FILE_OK:
            return 200;
        case FILE_BAD_PATH:
            return 400;
        case FILE_FORBIDDEN:
            return 403;
        case FILE_NOT_FOUND:
            return 404;
        case FILE_IO_ERROR:
            return 500;
    }
    return 500;
}

/* -------------------------------------------------------------- 一条连接 */

/* 返回：1 = 接收队列里可能还有没读完的字节 → 调用方**先 conn_finish(c->fd) 再 conn_free**；
 *       0 = 收干净了，直接 conn_free 就行。 */
static int handle_conn(conn *c, const char *peer) {
    http_request req; /* 注意：http.h 里是匿名 typedef，没有 struct tag —— 别写 struct http_request */
    static_file sf;
    size_t scan = 0;
    size_t head_len = 0;
    int over_limit = 0;
    int drain = 0;
    int status = 500;
    const char *extra = NULL;
    http_err pe;
    file_status fe;

    memset(&req, 0, sizeof req);
    memset(&sf, 0, sizeof sf);
    sf.fd = -1;

    /* 阶段 1：攒到「空行」为止。
     * 协议知识全在 http_scan_header_end 里，这里只负责「再读一点 / 到上限就别读了」。 */
    for (;;) {
        size_t got = 0;
        io_result ir;

        if (c->rlen >= RBUF_SIZE) {
            over_limit = 1;
            break;
        }
        ir = io_read(c->fd, c->rbuf + c->rlen, RBUF_SIZE - c->rlen, &got);
        if (ir == IO_WOULD_BLOCK) {
            if (wait_readable(c->fd) != 0) goto done; /* 收到信号 / poll 出错：静默收摊 */
            continue;
        }
        if (ir != IO_OK || got == 0) goto done; /* 没发完就断了/出错了：静默关闭，什么都不回 */
        c->rlen += got;
        if (http_scan_header_end(c->rbuf, c->rlen, &scan, &head_len)) break;
        if (c->rlen >= MAX_HEADER_LEN) {
            over_limit = 1;
            break;
        } /* 提前判 431，别等 rbuf 满 */
    }

    if (over_limit) {
        send_error(c->fd, 431, NULL, 0);
        log_request(peer, NULL, 0, NULL, 0, 431, 0);
        drain = 1; /* 我们是「读一半就走」的：对端大概率还在写 */
        goto done;
    }

    /* 阶段 2：解析。失败时解析器保证不留已分配内存，所以这里不需要回滚。 */
    pe = http_request_parse(c->rbuf, head_len, &req);
    if (pe != HTTP_OK) {
        status = status_from_parse_err(pe);
        extra = (status == STATUS_METHOD_NOT_ALLOWED) ? "Allow: GET, HEAD\r\n" : NULL;
        send_error(c->fd, status, extra, req.head_only);
        log_request(peer, req.method, req.method_len, req.target, req.target_len, status, 0);
        drain =
            (req.content_length > 0
            ); /* 第一版不读 body：声明了 body
                * 就还有没读的字节；如果客户端声明CL:0或没有CL却仍然多发了字节，我们不drain->close发RST->客户端可能读不到响应。M2的keep-alive会自然修掉。
                */

        goto done;
    }

    /* 阶段 3：映射到文件（路径安全全在 file.c 里，这里一个字都不判）。 */
    fe = file_open_under_root(g_root_real, req.path, &sf);
    if (fe != FILE_OK) {
        status = status_from_file_err(fe);
        send_error(c->fd, status, NULL, req.head_only);
        log_request(peer, req.method, req.method_len, req.target, req.target_len, status, 0);
        drain = (req.content_length > 0);
        goto done;
    }

    send_file(c->fd, &sf, http_content_type(sf.real), req.head_only);
    log_request(peer, req.method, req.method_len, req.target, req.target_len, 200, req.head_only ? 0 : sf.size);
    drain = (req.content_length > 0);

done: /* 单一出口：所有分支都从这里出去，fd/free 不会漏 */
    /* T2.1c：把这个请求消费掉（roff 前移到 head_len），再把残余字节挪到前面。
     * M1 只处理一条连接上的一个请求，所以 compact 之后也没有"下一个请求"；但流程与 M2/T2.6 一致。 */
    if (head_len > 0 && head_len <= c->rlen) {
        c->roff = head_len;
        conn_compact(c);
    }
    if (sf.fd >= 0) close(sf.fd); /* sf.fd 归 handle_conn 管；c->fd 归 conn_free 管 */
    http_request_free(&req);
    return drain;
}

/* ------------------------------------------------------------------ 启动 */

static void usage(const char *argv0, FILE *out) {
    fprintf(
        out,
        "用法: %s [ip] [port] [www-root]\n"
        "  默认: %s %d %s\n"
        "  例:   %s 0.0.0.0 8080 tests/www\n",
        argv0,
        DEFAULT_IP,
        DEFAULT_PORT,
        DEFAULT_ROOT,
        argv0
    );
}

/* 解析 argv（三段都可省）。失败只返回错误码，**不 exit** —— 由 main 决定怎么退出。
 * 返回：0 = 成功；1 = 请求帮助（-h/--help）；-1 = 参数非法。 */
static int parse_args(int argc, char **argv, const char **ip, int *port, const char **root) {
    *ip = DEFAULT_IP;
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

static int make_listener(const char *ip, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int on = 1;
    struct sockaddr_in addr;

    if (fd < 0) {
        perror("socket");
        return -1;
    }

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
// 打印日志
static void describe_peer(int fd, char *out, size_t cap) {
    struct sockaddr_in sa;
    socklen_t sl = sizeof sa;

    if (getpeername(fd, (struct sockaddr *)&sa, &sl) == 0 && sa.sin_family == AF_INET) {
        char ip[INET_ADDRSTRLEN] = "?";

        if (inet_ntop(AF_INET, &sa.sin_addr, ip, sizeof ip) == NULL) {
            snprintf(ip, sizeof ip, "?");
        }
        snprintf(out, cap, "%s:%u", ip, (unsigned)ntohs(sa.sin_port));
    } else {
        snprintf(out, cap, "?"); /* 取不到对端信息也不能让日志把服务搞崩 */
    }
}

int main(int argc, char **argv) {
    const char *ip = NULL;
    const char *root = NULL;
    int port = 0;
    int pa;
    char root_real[FILE_MAX_PATH];
    struct stat st;
    struct sigaction sa;
    int lfd;
    char peer[64];

    /* 1) argv */
    pa = parse_args(argc, argv, &ip, &port, &root);
    if (pa == 1) {
        usage(argv[0], stdout);
        return 0;
    }
    if (pa < 0) {
        usage(argv[0], stderr);
        return 2;
    }

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

    /* 5) 读缓冲现在**每连接一份**（内嵌在 `struct conn` 里），不再有全局 rbuf */

    /* 6) 串行 accept 循环（M2 的对照组）。阶段 A 起的变化：
     *    · 连接 fd 是**非阻塞**的：IO 层能区分「读到了 / 没有了 / 对端关了 / 真错了」；
     *    · 等就绪集中到 wait_readable() / io_wait() 一处 —— T2.3 换成 epoll_wait 就成了事件循环；
     *    · 监听 fd 仍保持阻塞：accept 还想在这里排队等待（T2.3 才改）。 */
    while (!g_stop) {
        int cfd = accept(lfd, NULL, NULL);

        if (cfd < 0) {
            if (errno == EINTR || errno == ECONNABORTED) continue; /* EINTR：该看 g_stop 了 */
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
            continue; /* 连接级错误不该杀服务器 */
        }

        describe_peer(cfd, peer, sizeof peer);
        if (conn_set_nonblocking(cfd) < 0) {
            fprintf(stderr, "fcntl(O_NONBLOCK): %s\n", strerror(errno));
            close(cfd);
            continue;
        }
        conn *c = conn_alloc(cfd);
        if (c == NULL) {
            /* T2.1b：拿不到连接槽位（fd 超过表容量 / 内存不足）→ 回 503 再关 */
            log_request(peer, NULL, 0, NULL, 0, 503, 0);
            send_error(cfd, 503, NULL, 0);
            close(cfd);
            continue;
        }
        if (handle_conn(c, peer) != 0) {
            conn_finish(c->fd); /* 收尾：别让对端还在写的字节把响应顶掉（见 conn_finish） */
        }
        conn_free(c); /* 关 fd + 归还槽位：fd 的生命周期归 conn 管 */
    }

    printf("shutdown: 收到信号，关闭监听 fd 并正常退出\n");
    fflush(stdout);
    close(lfd);
    return 0;
}
