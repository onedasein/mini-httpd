/* src/main.c — I/O 与生命周期（M2 T2.4/T2.6：epoll **边沿触发**的事件循环 + keep-alive）
 *
 * 职责边界（「三不」，迷茫的时候用这个自查）：
 *   不找空行     → http_scan_header_end()
 *   不切请求行   → http_request_parse()
 *   不碰路径安全 → file_open_under_root()
 * 本文件只做：收连接、读字节、写字节、关 fd、收信号、管 argv 与启动、推状态机。
 * 自查法：这里出现 "\r\n" 的解析、出现 ".." 的判断、出现 404 的**判断逻辑**
 *        （只允许出现「错误码 → 状态码」的映射表），就是分层漏了。
 *
 * 这一版（阶段 B）：
 *   - **一个 epoll、边沿触发（EPOLLET）、每次事件把当前这一步做到 EAGAIN 为止**：
 *     慢客户端不再拖死全服（它只是"下次事件再说"），而一次事件能把"读请求→写响应→读下一个"走完；
 *   - 一条连接 = 连接表里的一个 conn 对象 + 它自己的缓冲（见 src/conn.h）；
 *   - 响应**先装进 conn 的 wbuf**，再由 EPOLLOUT 事件往外写；文件体走 sendfile（零拷贝）；
 *   - keep-alive（T2.6）：HTTP/1.1 默认保持，1.0 默认关（规则真源 CONTRACTS.md §1b）；
 *   - **仍不读请求体**：声明了 Content-Length 的请求处理完就走 DRAIN 收尾后关闭（不能保持）。
 *
 * 对照基线：`git tag m1-blocking`（阻塞串行版）。改事件循环时 http.c / file.c 一行都不用动
 *   —— 这就是「main.c 里没有协议知识」换来的东西。
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
#include <netinet/tcp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/epoll.h>
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

/* 事件循环的唤醒粒度（ms）。两处用它：
 *   ① epoll_wait 的超时（信号与 epoll_wait 的竞态兜底：最坏 200ms 也能退出）；
 *   ② DRAIN 的截止时间（对端不发也不关，到点我们收）。M3 的空闲超时也照这个路子做。 */
#define WAIT_TICK_MS                  200
#define MAX_EVENTS                    64
#define DRAIN_MAX_BYTES               (1024 * 1024) /* 收尾预算：对端残留字节最多读掉 1MB（M1 定的） */
#define SENDFILE_CHUNK                (1024 * 1024) /* 一次 sendfile 最多推 1MB：别让一次调用吃掉整个文件 */
#define RESP_MAX                      (4 * 1024)    /* 错误页（头 + 小 body）的拼装缓冲 */
/* 连接对象（struct conn）与 IO 包装住在 src/conn.h / conn.c —— 抽成模块是因为
 * T2.1b/T2.1c/T2.2a 的验收都要单元测试，而测试只链接 LIB_SRC（不含 main.c）。 */
/* ------------------------------------------------------------------ 全局 */
static const char *g_root_real;              /* 文档根 realpath，启动时算一次 */
static volatile sig_atomic_t g_stop = 0;     /* 信号 handler 只写它（异步信号安全） */

static void on_stop(int sig) {
    (void)sig;
    g_stop = 1;      /* handler 里不 printf、不 free、不 malloc */
}

/* ------------------------------------------------------------ 事件循环（T2.3b）
 * 形状：**一个 epoll、全部 fd 带 EPOLLET、一次事件把当前步骤做到 IO_WOULD_BLOCK 为止**。
 * 真正的非阻塞语义在 src/conn.c 的 io_* 里（T2.2a），"下一步该干什么"在 conn_next_step（T2.3b）。
 *
 * ET 的两条硬要求（T2.4，都是踩过才知道疼的地方）：
 *   ① 每次事件必须把数据读/写干（循环到 EAGAIN），否则内核"只在状态变化时通知一次"，
 *      剩下的数据就再也没人叫你了 —— 连接挂住，且没有报错；
 *   ② accept 同理：监听 fd 带 EPOLLET 之后必须把队列 accept 到 EAGAIN。
 *
 * 为什么阶段 A 之后仍然阻塞（这是他问的那个"不清楚"）：
 *   阶段 A 只改了**两件事**：fd 变非阻塞、把等待收敛到 conn.c 的一个包装里（T2.2a）。
 *   但主循环还是"一条连接在 handle_conn 里跑到底"：那条慢连接读不到数据时，
 *   我们仍然在原地 poll 等它（等 200ms 再看一次 g_stop），全服务器陪着它停。
 *   非阻塞只改变了"等的时候在干什么"，改不了"同时只有一条连接在动"——
 *   因为**谁有数据、谁能写，只有内核知道**；不问内核（epoll_wait），就只能死等一个人。
 *   事件循环正是那个"把所有连接一起交给内核、谁就绪谁被返回"的机制。 */
static int g_epfd = -1; /* 事件循环的 epoll fd（处理函数要用，所以放文件作用域） */

/* 关一条连接：**先 epoll_ctl(DEL) 再 conn_free**（conn_free 里会 close(fd)；顺序反了，
 * fd 被下一次 accept 复用后，epoll 里就留下一个指向已释放连接的事件）。 */
static void close_conn(conn *c) {
    if (c == NULL) return;
    if (g_epfd >= 0 && c->fd >= 0) (void)epoll_ctl(g_epfd, EPOLL_CTL_DEL, c->fd, NULL);
    conn_free(c); /* 关 fd + 关 file_fd + free wbuf + 归还槽位 */
}

/* 所有连接都带 EPOLLET（T2.4）：只在状态变化时通知一次，所以每一步都必须做到 EAGAIN 为止。
 * EPOLLRDHUP 没注册：对端半关闭（FIN）一定让 socket 变成"可读的 EOF"→ EPOLLIN 照来，
 * 写侧的半关闭也不该打断我们发响应（`printf … | nc -N` 就是这种客户端）。 */
#define CONN_EV_FLAGS EPOLLET

/* 只有在"该关注什么"变化时才 epoll_ctl(MOD)：掩码没变说明状态没变，内核还会照旧通知。
 * 这条同时是 T2.4c 的纪律：EPOLLOUT **只在真的写不动之后**才挂上（见 conn_pump），写完立刻摘掉。 */
static int arm_interest(conn *c, conn_step s) {
    uint32_t want = conn_step_events(s) | CONN_EV_FLAGS;
    struct epoll_event ev;

    if (want == c->interest) return 0; /* 没变：省一次系统调用 */
    ev.events = want;
    ev.data.fd = c->fd;
    if (epoll_ctl(g_epfd, EPOLL_CTL_MOD, c->fd, &ev) < 0) return -1;
    c->interest = want;
    return 0;
}

/* 超时扫描：DRAIN 有截止时间，到点就关（对端不发也不关的话，fd 会一直挂在表里）。
 * 每 200ms 扫一次很便宜：没有连接时就是一次「4096 个空指针」的比较（conn_next_fd 一趟）。
 * 用 fd 序号遍历而不是连接指针：扫描里会关连接，先拿到下一个 fd 再动手，不会 use-after-free。 */
static void sweep_deadlines(void) {
    long long now = conn_now_ms();
    int fd = -1;

    while ((fd = conn_next_fd(fd)) >= 0) {
        conn *c = conn_lookup(fd);

        if (c != NULL && c->phase == CONN_DRAIN && now >= c->drain_deadline_ms) close_conn(c);
    }
}

/* ------------------------------------------------------------------ 响应 */

/* 错误页（头 + 小 body）拼进 buf；返回总长度，拼不下返回 0（宁可什么都不发，也不发半截）。
 * keep_alive 只是"响应头里写 Connection: 什么"；**解析失败的分支一律传 0** ——
 * 请求边界都不确定，凭什么把连接留着接下一个（见 CONTRACTS.md §1b 的例外）。 */
static size_t build_error(char *buf, size_t cap, int status, const char *extra, int head_only, int keep_alive) {
    char body[512];
    int bn;
    int hn;
    size_t total;

    bn = snprintf(
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
    if (bn < 0) bn = 0;
    if ((size_t)bn >= sizeof body) bn = (int)(sizeof body - 1);

    hn = http_build_response_head(
        buf, cap, status, "text/html; charset=utf-8", (long long)bn, keep_alive, extra
    );
    if (hn < 0) return 0; /* 拼不下是 bug，但此刻没法再回 500，只能放弃 */
    total = (size_t)hn;
    if (!head_only && bn > 0) {
        if (total + (size_t)bn > cap) return 0;
        memcpy(buf + total, body, (size_t)bn);
        total += (size_t)bn;
    }
    return total;
    /* 错误页只说状态码，不回显请求路径/绝对路径 —— 不泄漏服务器信息 */
}

/* 503 兜底：fd 表都满了，连 conn 都建不出来 —— 只能尽力直接写一屏（fd 是非阻塞的，写不完就算）。
 * 这里**不能等**：一等等，整个事件循环就陪着一条注定要关的连接停住了。 */
static void reply_then_close(int fd, int status) {
    char resp[RESP_MAX];
    size_t len = build_error(resp, sizeof resp, status, NULL, 0, 0); /* 兜底响应：一律关 */
    size_t off = 0;

    while (off < len) {
        size_t n = 0;

        if (io_write(fd, resp + off, len - off, &n) != IO_OK || n == 0) break;
        off += n;
    }
    close(fd);
}

/* 把响应装进连接（不再直接写 fd）：真正往外写是主循环的 EPOLLOUT 事件干的活。 */
static void answer_error(conn *c, int status, const char *extra, int head_only) {
    char resp[RESP_MAX];
    size_t len = build_error(resp, sizeof resp, status, extra, head_only, 0);

    if (conn_set_wbuf(c, resp, len) < 0) {
        c->phase = CONN_CLOSE; /* 连错误页都装不下（内存不足）：只能关 */
        return;
    }
    c->phase = CONN_WRITE;
}

/* 文件响应：头进 wbuf，**body 交给 sendfile**（内核直接推给 socket），文件 fd 的所有权转给连接对象。
 * 为什么不把文件读进内存再写：要么每连接常驻一大块缓冲（4096 连接 × 64KB = 256MB），
 * 要么状态里再多一份"半截 body 缓冲"。sendfile 把这两样都省了。 */
static void answer_file(conn *c, static_file *sf, const char *ctype, int head_only) {
    char head[1024];
    int hn = http_build_response_head(head, sizeof head, 200, ctype, sf->size, c->keep_alive, NULL);

    if (hn < 0) {
        close(sf->fd); /* 头拼不下：这个 fd 当场还掉，再回 500（否则就是漏 fd） */
        answer_error(c, 500, NULL, head_only);
        return;
    }
    /* 先接管 fd、再装 wbuf：万一下面装失败，phase 转 CLOSE，conn_free 会把 file_fd 关掉（不漏 fd） */
    c->file_fd = sf->fd;
    c->file_off = 0;
    c->file_left = head_only ? 0 : sf->size; /* HEAD：Content-Length 照旧是真实大小，一个字节 body 都不发 */
    if (conn_set_wbuf(c, head, (size_t)hn) < 0) {
        c->phase = CONN_CLOSE;
        return;
    }
    c->phase = CONN_WRITE;
}

/* ------------------------------------------------------------------ 日志 */

/* 把对端地址做成 "ip:port"（取不到就给 "?"，绝不让日志把服务搞崩）。
 * 放这里是因为事件循环里好几处都要打日志，而"谁在连"属于日志信息。 */
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
        snprintf(out, cap, "?");
    }
}

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

/* -------------------------------------------------------------- 一条连接
 * 每一步都**做到 io 说"做不动了"（IO_WOULD_BLOCK）为止** —— 这是 ET（T2.4）的硬要求：
 * 边沿触发只在状态变化时通知一次，你要是半路回事件循环，剩下的数据就再也没人叫你了。
 * 返回值统一是"这一步一共搬了多少字节"：0 = 一点没动（用于判断"是不是在做无用功"）。 */

/* 请求头攒够空行了：解析 → 定位文件 → 把响应装进连接。全是纯逻辑，一个字节都不阻塞。 */
static void serve_request(conn *c, size_t head_len) {
    http_request req;
    static_file sf;
    char peer[64];
    int status;
    const char *extra = NULL;
    http_err pe;
    file_status fe;

    memset(&req, 0, sizeof req);
    memset(&sf, 0, sizeof sf);
    sf.fd = -1;
    describe_peer(c->fd, peer, sizeof peer);
    c->keep_alive = 0; /* 默认关：只有"解析成功 + 没有未消费的 body + 客户端想留"三条都成立才保持 */

    /* 解析失败时解析器保证不留已分配内存，所以这里不需要回滚。 */
    pe = http_request_parse(c->rbuf, head_len, &req);
    if (pe != HTTP_OK) {
        status = status_from_parse_err(pe);
        extra = (status == STATUS_METHOD_NOT_ALLOWED) ? "Allow: GET, HEAD\r\n" : NULL;
        answer_error(c, status, extra, req.head_only);
        log_request(peer, req.method, req.method_len, req.target, req.target_len, status, 0);
        goto decided;
    }

    /* 映射到文件（路径安全全在 file.c 里，这里一个字都不判）。 */
    fe = file_open_under_root(g_root_real, req.path, &sf);
    if (fe != FILE_OK) {
        status = status_from_file_err(fe);
        answer_error(c, status, NULL, req.head_only);
        log_request(peer, req.method, req.method_len, req.target, req.target_len, status, 0);
        goto decided;
    }

    /* 走到这里两条前提都成立：解析成功、且没有未消费的 body（body 分支在下面统一处理）。
     * 能不能保持，交给协议层判断（规则见 CONTRACTS.md §1b）。 */
    c->keep_alive = !http_request_wants_close(&req);
    answer_file(c, &sf, http_content_type(sf.real), req.head_only);
    log_request(peer, req.method, req.method_len, req.target, req.target_len, 200, req.head_only ? 0 : sf.size);

decided:
    /* 第一版不读请求体：客户端声明了 body 就还有没读的字节。
     * 直接 close 会让内核回 RST，客户端连刚发出的响应都可能读不到 —— 所以要走 DRAIN 收尾；
     * 而且这份 body 没被消费掉，**绝不能** keep-alive（那些字节会被当成下一个请求解析）。 */
    if (req.content_length > 0) {
        c->drain_left = DRAIN_MAX_BYTES;
        c->keep_alive = 0;
    }
    http_request_free(&req);

    /* T2.1c + T2.6：请求字节被消费掉（roff 前移），把**残余字节**（流水线里的下一个请求）挪到 rbuf[0]。
     * keep-alive 时下一个请求就从这里接着解析；不再有"处理完就关"那条限制。 */
    c->roff = head_len;
    conn_compact(c);
}

/* 步骤 ①：攒请求头。返回值 = 这一步读了几个字节（0 = 现在读不到）。 */
static long long step_read_req(conn *c) {
    long long total = 0;

    for (;;) {
        size_t got = 0;
        size_t head_len = 0;
        io_result r;

        /* 缓冲满了还没见到空行：再读也没地方放，直接判 431（不是先读再看） */
        if (c->rlen >= RBUF_SIZE || c->rlen >= MAX_HEADER_LEN) break;

        /* 先扫一遍已经攒下的字节：keep-alive 的下一个请求可能**已经在缓冲里**了
         * （客户端流水线发了两个请求 / 上一个 recv 顺手读进来），这时不该再等内核。 */
        if (http_scan_header_end(c->rbuf, c->rlen, &c->rscan, &head_len)) {
            serve_request(c, head_len);
            return total + 1;
        }
        r = io_read(c->fd, c->rbuf + c->rlen, RBUF_SIZE - c->rlen, &got);
        if (r == IO_CLOSED || r == IO_ERROR) { /* 没发完就断了 / 出错了：静默关闭，什么都不回 */
            c->phase = CONN_CLOSE;
            return total;
        }
        if (r == IO_WOULD_BLOCK) return total; /* ← ET 的收工条件：读到 EAGAIN 才回事件循环 */
        c->rlen += got;
        total += (long long)got;
        c->last_active = time(NULL);
    }

    /* 到这里只有一种原因：缓冲/上限满了还没见到空行 → 431（读一半就走，所以待会儿要收尾） */
    {
        char peer[64];

        describe_peer(c->fd, peer, sizeof peer);
        answer_error(c, 431, NULL, 0);
        log_request(peer, NULL, 0, NULL, 0, 431, 0);
        c->drain_left = DRAIN_MAX_BYTES;
    }
    return total + 1;
}

/* 步骤 ②：写响应 —— 先写完 wbuf（响应头/错误页），再用 sendfile 发文件体。
 * 返回值 = 这一步写了几个字节（0 = 现在写不进去，可以挂 EPOLLOUT 了）。 */
static long long step_write(conn *c) {
    long long total = 0;

    for (;;) {
        size_t n = 0;
        io_result r;

        if (c->woff < c->wlen) {
            r = io_write(c->fd, c->wbuf + c->woff, c->wlen - c->woff, &n);
            if (r == IO_OK && n > 0) {
                c->woff += n;
                total += (long long)n;
                c->last_active = time(NULL);
                continue; /* ET：继续写，直到内核说写不动 */
            }
            if (r == IO_WOULD_BLOCK) return total;
            c->phase = CONN_CLOSE; /* EPIPE / ECONNRESET：对端走了，这是常态 */
            return total;
        }

        if (c->file_left > 0) {
            size_t want = (c->file_left > (long long)SENDFILE_CHUNK) ? SENDFILE_CHUNK : (size_t)c->file_left;

            r = io_sendfile(c->fd, c->file_fd, &c->file_off, want, &n);
            if (r == IO_OK && n > 0) {
                c->file_left -= (long long)n;
                total += (long long)n;
                c->last_active = time(NULL);
                continue;
            }
            if (r == IO_WOULD_BLOCK) return total;
            c->phase = CONN_CLOSE; /* 文件被截短（IO_CLOSED）或对端走了：头已经发出去了，只能断 */
            return total;
        }

        return total + 1; /* 没有要写的了：DRAIN / NEXT_REQ / CLOSE 由 conn_next_step 决定 */
    }
}

/* 步骤 ③：收尾 —— 把对端残留的字节读干（读进栈上缓冲丢掉）。返回值 = 读掉几个字节。
 *
 * 为什么值得一步（2026-10-11 实测）：接收队列里还有没读的字节就直接 close，内核发的是 **RST 而不是 FIN**；
 * 客户端此时往往还在写它那条超大请求 —— 写入失败（EPIPE/ECONNRESET）后，`printf … | nc` 这种客户端
 * 会直接退出，连我们已经发出的 431 都读不到。把剩下的读干净，close 才是干净的 FIN。
 *
 * 与 M1 的区别：M1 在 handle_conn 里 poll 等 200ms —— 整个服务器陪着一条连接停。
 * 现在只登记一个截止时刻，等不到就由 sweep_deadlines() 收掉，谁都不耽误。 */
static long long step_drain(conn *c) {
    char tmp[4096];
    long long total = 0;

    for (;;) {
        size_t got = 0;
        io_result r = io_read(c->fd, tmp, sizeof tmp, &got);

        if (r == IO_OK) {
            c->drain_left -= (long long)got;
            total += (long long)got;
            c->last_active = time(NULL);
            if (c->drain_left <= 0) c->phase = CONN_CLOSE; /* 预算用完了：收 */
            if (c->phase == CONN_CLOSE) return total;
            continue; /* ET：继续读到 EAGAIN 或者预算用尽 */
        }
        if (r == IO_WOULD_BLOCK) return total; /* 等下一次 EPOLLIN（或等到截止时间） */
        c->drain_left = 0;                     /* IO_CLOSED:对端也关了；IO_ERROR:再留着没意义 */
        c->phase = CONN_CLOSE;
        return total;
    }
}

/* 响应写完、连接要留着接下一个请求：把"每请求一份"的状态清掉（T2.6）。
 * 清完 phase 回到 READ_REQ，循环接着解析 —— rbuf 里的残余字节就是下一个请求的开头。 */
static void enter_next_request(conn *c) {
    conn_reset_for_next_request(c);
}

/* 响应写完、但要先把对端残留读干（DRAIN）：shutdown(SHUT_WR) 明确告诉它"我不再写了"，
 * 再登记 200ms 的截止时间 —— 对端既不发也不关，到点由 sweep_deadlines() 收掉。 */
static void enter_drain(conn *c) {
    (void)shutdown(c->fd, SHUT_WR);
    c->phase = CONN_DRAIN;
    c->drain_deadline_ms = conn_now_ms() + WAIT_TICK_MS;
}

/* 跑当前这一步（返回值 = 搬了多少字节；0 = 做不动了）。 */
static long long run_step(conn *c, conn_step s) {
    switch (s) {
        case CONN_STEP_READ_REQ:
            return step_read_req(c);
        case CONN_STEP_WRITE_HEAD:
        case CONN_STEP_SEND_FILE:
            return step_write(c);
        case CONN_STEP_DRAIN:
            return step_drain(c);
        case CONN_STEP_NEXT_REQ:
            enter_next_request(c);
            return 1; /* 状态推进了：接着跑下一个请求的解析 */
        case CONN_STEP_CLOSE:
            return 0;
    }
    return 0;
}

/* 一条连接的推进：**一直做到做不动为止**（ET 的要求），然后按"下一步"登记关注的事件。
 *
 * 有了这个循环，平常一次事件就能把"读请求 → 写响应 → 接着读下一个请求"全部走完，
 * 不需要在中间回 epoll_wait 等 EPOLLOUT（那也是 T2.4c 说的"写完立刻摘掉 EPOLLOUT"）。
 *
 * hard = 这次事件带了 EPOLLERR/EPOLLHUP。 */
static void conn_pump(conn *c, int hard) {
    long long moved_total = 0;

    for (;;) {
        conn_step s = conn_next_step(c);
        long long moved;

        if (s == CONN_STEP_DRAIN && c->phase == CONN_WRITE) enter_drain(c); /* 先转阶段，再跑 */
        s = conn_next_step(c);
        if (s == CONN_STEP_CLOSE) {
            close_conn(c);
            return;
        }

        moved = run_step(c, s);
        moved_total += moved;
        if (moved == 0) break; /* 做不动了（IO_WOULD_BLOCK）：去登记事件，等下一次 */
    }

    if (c->phase == CONN_CLOSE) {
        close_conn(c);
        return;
    }
    /* 硬错误（HUP/ERR）+ 整趟一点没动 = 再关注下去就是空转（HUP 会被反复报，CPU 100%）。
     * 注意判据是**整趟**是否推进过，不是最后一步 —— 半关闭的客户端也可能带这类标志，
     * 而它的请求该被正常处理（`printf … | nc -N`）。 */
    if (hard && moved_total == 0) {
        close_conn(c);
        return;
    }
    if (arm_interest(c, conn_next_step(c)) < 0) close_conn(c); /* fd 已经坏了：收摊 */
}

/* 一条连接的一次事件。 */
static void on_conn_event(int fd, uint32_t ev) {
    conn *c = conn_lookup(fd);

    if (c == NULL) return; /* 本次批次里已经关掉的连接留下的陈旧事件 */
    /* HUP/ERR **不当成"立刻关"**：半关闭也会带这类标志 —— 对端 shutdown(SHUT_WR) 之后还在读我们的响应
     * （e2e 里的 `printf … | nc -N` 就是），此时响应完全发得出去。
     * 所以照常推进，read/write 的返回值才是权威（同一件事只有一个真源）；
     * 真的一点都推不动（conn_pump 里那一步返回 0）才收摊。 */
    conn_pump(c, (ev & (EPOLLERR | EPOLLHUP)) != 0);
}

/* 新连接。T2.4a：监听 fd 带 EPOLLET，所以**必须把队列 accept 到 EAGAIN** ——
 * 边沿触发只通知一次，你要是只 accept 一个就走，剩下的连接会一直躺在队列里（`ss -tln` 的 Recv-Q 涨）。
 * 一次事件收多条也顺带把"每条连接一次 epoll_ctl(ADD)"摊薄了。 */
static void on_accept(int lfd) {
    for (;;) {
        struct epoll_event ev;
        char peer[64];
        conn *c;
        int cfd = accept(lfd, NULL, NULL);

        if (cfd < 0) {
            if (errno == EINTR) {
                if (g_stop) return;
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) return; /* 队列空了：ET 的收工条件 */
            if (errno == ECONNABORTED) continue;                 /* 连接在队列里就断了：下一个 */
            if (errno == EMFILE || errno == ENFILE) {
                /* fd 耗尽：记日志 + 直接停手（**不 sleep**：睡了就更没机会回收 fd，等于自锁）。
                 * ET 下队列没排空就不再通知，但下一次新连接会带来新的边沿，那时把队列一起排空 —— 自愈。 */
                fprintf(stderr, "accept: %s（fd 耗尽，暂时停止 accept）\n", strerror(errno));
                return;
            }
            fprintf(stderr, "accept: %s\n", strerror(errno));
            continue; /* 连接级错误不该杀服务器 */
        }

        describe_peer(cfd, peer, sizeof peer);
        /* TCP_NODELAY：关掉 Nagle。
         * 为什么非关不可（2026-10-14 实测）：我们发一条响应是"先 send 响应头、再 sendfile 文件体"两次写，
         * 而 Nagle 的规则是"还有没被确认的数据时，后面的小段先攒着"—— 于是头出去以后，
         * 那几十字节的 body 要等客户端的延迟 ACK（约 40ms）才动。实测：没关时 wrk p50 就是 44ms、
         * 2233 req/s；关掉后同机同参数 0.x ms、几万 req/s（数字记在 docs/bench/M2-ET验收.md）。
         * 这是"HTTP 服务器为什么都设 TCP_NODELAY"的第一手答案。 */
        {
            int on = 1;

            if (setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on) < 0) {
                fprintf(stderr, "setsockopt(TCP_NODELAY): %s\n", strerror(errno));
            }
        }
        if (conn_set_nonblocking(cfd) < 0) {
            fprintf(stderr, "fcntl(O_NONBLOCK): %s\n", strerror(errno));
            close(cfd);
            continue;
        }
        c = conn_alloc(cfd);
        if (c == NULL) {
            /* T2.1b：拿不到连接槽位（fd 超过表容量 / 内存不足）→ 尽力回 503 再关 */
            log_request(peer, NULL, 0, NULL, 0, 503, 0);
            reply_then_close(cfd, 503);
            continue;
        }

        /* 新连接是 CONN_READ_REQ → EPOLLIN（关注什么是推出来的，不是写死的）＋ EPOLLET。
         * 记到 c->interest 里：之后 arm_interest() 就靠它判断"要不要 MOD"。 */
        ev.events = conn_step_events(conn_next_step(c)) | CONN_EV_FLAGS;
        ev.data.fd = cfd;
        if (epoll_ctl(g_epfd, EPOLL_CTL_ADD, cfd, &ev) < 0) {
            perror("epoll_ctl(ADD conn)");
            close_conn(c); /* 已经在连接表里了，走正规收摊路径 */
            continue;
        }
        c->interest = ev.events;
    }
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

int main(int argc, char **argv) {
    const char *ip = NULL;
    const char *root = NULL;
    int port = 0;
    int pa;
    char root_real[FILE_MAX_PATH];
    struct stat st;
    struct sigaction sa;
    int lfd;

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

    /* 4) 监听 fd：**非阻塞**（事件循环里 accept 一旦阻塞，整个循环就冻住了）
     * ＋ 注册进 epoll（关注 EPOLLIN：有新连接）。 */
    lfd = make_listener(ip, port);
    if (lfd < 0) return 1;
    if (conn_set_nonblocking(lfd) < 0) {
        fprintf(stderr, "fcntl(O_NONBLOCK) listener: %s\n", strerror(errno));
        close(lfd);
        return 1;
    }

    g_epfd = epoll_create1(EPOLL_CLOEXEC);
    if (g_epfd < 0) {
        perror("epoll_create1");
        close(lfd);
        return 1;
    }
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = lfd;
    if (epoll_ctl(g_epfd, EPOLL_CTL_ADD, lfd, &ev) < 0) {
        perror("epoll_ctl(ADD lfd)");
        close(g_epfd);
        close(lfd);
        return 1;
    }

    struct epoll_event events[MAX_EVENTS];

    /* e2e 脚本靠这句话判断服务是否就绪：措辞别改，而且要 flush（管道是全缓冲的） */
    printf("listening on %s:%d  (www-root=%s)\n", ip, port, g_root_real);
    fflush(stdout);

    /* 5) 读缓冲现在**每连接一份**（内嵌在 `struct conn` 里），不再有全局 rbuf */

    /* 6) 事件循环（T2.4，边沿触发）：listener 与所有连接 fd 都在同一个 epoll 里，
     *    每个事件把当前步骤做到 EAGAIN（见 on_accept / on_conn_event / conn_pump）
     *    —— **没有人再阻塞着等某一条连接**，也没有人半路撒手（ET 下撒手 = 挂住）。
     *
     *    超时固定 WAIT_TICK_MS，两个用处：
     *      ① 信号与 epoll_wait 之间的竞态兜底：信号若在检查 g_stop 之后、进入 epoll_wait 之前到达，
     *         没有超时就会一直等下去（最坏也只晚 200ms 退出）；
     *      ② DRAIN 的截止时间靠这个 tick 落地（见 sweep_deadlines）。
     *    "信号立刻叫醒事件循环"的正解是 eventfd / self-pipe（handler 里 write 一个字节），M3 再做。 */
    while (!g_stop) {
        int n = epoll_wait(g_epfd, events, MAX_EVENTS, WAIT_TICK_MS);

        if (n < 0) {
            if (errno == EINTR) continue; /* 收到信号：回 while 看 g_stop */
            perror("epoll_wait");
            break;
        }
        for (int i = 0; i < n; i++) {
            if (events[i].data.fd == lfd) on_accept(lfd);
            else on_conn_event(events[i].data.fd, events[i].events);
        }
        sweep_deadlines(); /* 到点的 DRAIN 该收了（n == 0 的超时醒来也走这里） */
    }

    printf("shutdown: 收到信号，关闭监听 fd 并正常退出\n");
    fflush(stdout);
    /* 退出前把还挂着的连接收干净（否则 valgrind 的 "All heap blocks were freed" 不成立） */
    for (int fd = conn_next_fd(-1); fd >= 0; fd = conn_next_fd(fd)) close_conn(conn_lookup(fd));
    close(g_epfd);
    close(lfd);
    return 0;
}
