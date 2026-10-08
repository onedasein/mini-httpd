/* echo 热身（T0.3）— 最小 TCP echo server
 *
 * 骨架：socket -> setsockopt -> bind -> listen -> accept -> read -> write -> close
 * 结构：单进程、单线程、阻塞 I/O；accept 循环里**一次只伺候一个连接**（串行）
 * 用法：./build/debug/server [ip] [port]      # 都可省，默认 127.0.0.1 8080
 * 验证：bash tests/run.sh                     # 端口由测试脚本给（8800+），不要写死
 *
 * 下一阶段（M2）：把串行 accept 换成 epoll ET + 非阻塞，解决"慢客户端拖死全服"。
 */
#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define PORT     8080
#define BUF_SIZE 4096

/* SIGPIPE 的默认动作是**杀死进程**：对端已经关掉、我们还继续写就会中招。
 * 忽略之后 write 只返回 -1/EPIPE，那只是一个普通 errno。
 * 用 -DKEEP_SIGPIPE_DEFAULT 编译可保留默认处置，用来现场对照"不忽略会怎样"
 * （tests/run.sh ⑤ 会拿这个变体跑 10 轮对端突断）。 */
#ifndef KEEP_SIGPIPE_DEFAULT
static void ignore_sigpipe(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_IGN;
    if (sigemptyset(&sa.sa_mask) < 0) {
        perror("sigemptyset");
        exit(1);
    }
    sa.sa_flags = 0;    /* 故意不加 SA_RESTART：让被信号打断的 read 返回 EINTR */
    if (sigaction(SIGPIPE, &sa, NULL) < 0) {
        perror("sigaction");
        exit(1);
    }
}
#endif

/* 把字节里不可见的部分显形（CR/LF/TAB，其他按 \xNN）。
 * 网络编程里 90% 的怪问题，最后都是"你发的字节和你以为的不一样"。 */
static void dump_bytes(const char *tag, const unsigned char *buf, ssize_t n) {
    printf("%s[%zd]: ", tag, n);
    for (ssize_t i = 0; i < n; i++) {
        unsigned char c = buf[i];
        if (c == '\n')
            printf("\\n");
        else if (c == '\r')
            printf("\\r");
        else if (c == '\t')
            printf("\\t");
        else if (c >= 32 && c < 127)
            putchar(c);
        else
            printf("\\x%02x", c);
    }
    putchar('\n');
    fflush(stdout);     /* stdout 重定向到文件时是全缓冲：不 flush，日志会随进程一起消失 */
}

static int parse_port(const char *s, uint16_t *out) {
    char *end = NULL;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v < 1 || v > 65535) return -1;
    *out = (uint16_t)v;
    return 0;
}

static void describe_peer(const struct sockaddr_in *cli, char *dst, size_t dstlen) {
    char ip[INET_ADDRSTRLEN] = "?";
    if (inet_ntop(AF_INET, &cli->sin_addr, ip, (socklen_t)sizeof(ip)) == NULL) {
        snprintf(ip, sizeof(ip), "?");
    }
    snprintf(dst, dstlen, "%s:%u", ip, (unsigned)ntohs(cli->sin_port));
}

int main(int argc, char **argv) {
    const char *ip = (argc > 1) ? argv[1] : "127.0.0.1";
    uint16_t port = PORT;
    if (argc > 2 && parse_port(argv[2], &port) < 0) {
        fprintf(stderr, "bad port: %s\n", argv[2]);
        return 1;
    }

#ifndef KEEP_SIGPIPE_DEFAULT
    ignore_sigpipe();
#endif

    // 1.socket
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    // 允许端口重用，防止重启时 "Address already in use"。
    // 用 -DNO_REUSEADDR 编译可关掉它，现场对照 EADDRINUSE（tests/run.sh ③）。
#ifndef NO_REUSEADDR
    int opt = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt(SO_REUSEADDR)");
        close(fd);
        return 1;
    }
#endif

    // 2.bind
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
        fprintf(stderr, "bad ip: %s\n", ip);
        close(fd);
        return 1;
    }
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        return 1;
    }

    // 3.listen
    if (listen(fd, SOMAXCONN) < 0) {
        perror("listen");
        close(fd);
        return 1;
    }
    printf("echo server listening on %s:%u\n", ip, (unsigned)port);
    fflush(stdout);

    while (1) {
        // 4.accept（一次只伺候一个连接；其余连接只能在内核 accept 队列里排队）
        struct sockaddr_in cli_addr;
        socklen_t cli_len = sizeof(cli_addr);

        int conn_fd = accept(fd, (struct sockaddr *)&cli_addr, &cli_len);
        if (conn_fd < 0) {
            if (errno == EINTR) continue;         // 被信号打断：重试
            if (errno == ECONNABORTED) continue;  // 连接在队列里被对端 RST：正常现象
            perror("accept");                     // 其他错误（如 EMFILE）也不该让服务器退出
            continue;
        }

        char peer[INET_ADDRSTRLEN + 8];
        describe_peer(&cli_addr, peer, sizeof(peer));
        printf("accept: fd=%d from %s\n", conn_fd, peer);
        fflush(stdout);

        // 收发数据
        char buf[BUF_SIZE];
        ssize_t n;
        while ((n = read(conn_fd, buf, sizeof(buf))) != 0) {
            if (n < 0) {
                if (errno == EINTR) continue;     // 打断的是这一笔 read，重试它（不能跳到 accept）
                perror("read");
                break;                            // 只跳出内层循环，下面统一 close
            }

            // ★ 短读：TCP 是字节流，一次 read 拿到 n < sizeof buf 完全正常
            printf("recv %zd byte(s)\n", n);
            dump_bytes("  <- ", (const unsigned char *)buf, n);

            // ★ 短写：write 契约上允许只写出一部分，所以必须循环写到完
            ssize_t written = 0;
            int write_failed = 0;
            while (written < n) {
                ssize_t w = write(conn_fd, buf + written, (size_t)(n - written));
                if (w < 0) {
                    if (errno == EINTR) continue;
                    if (errno == EPIPE || errno == ECONNRESET) {
                        printf("peer gone (%s), drop this connection\n", strerror(errno));
                    } else {
                        perror("write");
                    }
                    write_failed = 1;
                    break;
                }
                if (w == 0) {                     // 阻塞 fd 上不应出现；出现就当失败处理
                    write_failed = 1;
                    break;
                }
                written += w;
            }
            fflush(stdout);
            if (write_failed) break;              // 跳出内层循环，去 close
        }
        if (n == 0) {
            printf("peer closed\n");
            fflush(stdout);
        }

        // 5.close（fd 是有限资源，必须还回去）
        close(conn_fd);
        printf("connection closed\n");
        fflush(stdout);
    }
}
