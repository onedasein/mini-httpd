/* echo_server.c — M0/T0.3 最小 TCP echo server
 *
 * 教学顺序：socket -> setsockopt -> bind -> listen -> accept -> read -> write -> close
 * 编译：  make debug      运行： ./build/debug/echo_server 8080
 * 验证：  printf 'hi\n' | nc 127.0.0.1 8080
 *         nc 127.0.0.1 8080   # 交互：手打一行回车一次；Ctrl-D 断开
 *
 * 下一阶段（M1）会在同一骨架上长成阻塞式 HTTP；M2 再换成 epoll ET 非阻塞。
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
#include <sys/types.h>
#include <unistd.h>

#define MAX_CONN 8            /* backlog：内核 accept 队列长度（不是连接数上限） */
#define BUF_SIZE 4096         /* ★ 必须 >= 2，否则短读的 while 逻辑没有意义 */

static int make_listener(const char *ip, const char *port_str)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }

    /* ① 反面教材：把这段注释掉再跑一次，重启会看到 bind: Address already in use
     *    原因：TIME_WAIT 的连接还占着 (ip, port) 四元组 */
#if 1
    int on = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on) < 0) {
        perror("setsockopt(SO_REUSEADDR)");
        close(fd);
        return -1;
    }
#endif

    /* ② 反面教材：正常版本应打开它（或用 send(MSG_NOSIGNAL)）
     *    不打开时：nc 断开后我们再 write，会收到 SIGPIPE，默认动作是**杀死整个进程** */
#if 0
    signal(SIGPIPE, SIG_IGN);
#endif

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)atoi(port_str));   /* host -> network 字节序 */
    /* 127.0.0.1 只监听本机回环；INADDR_ANY 则监听所有网卡 */
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
        fprintf(stderr, "bad ip: %s\n", ip);
        close(fd);
        return -1;
    }

    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("bind"); close(fd); return -1;
    }
    if (listen(fd, MAX_CONN) < 0) {
        perror("listen"); close(fd); return -1;
    }

    char port_s[16];
    inet_ntop(AF_INET, &addr.sin_addr, port_s, sizeof port_s);   /* 只为打印 */
    printf("listening on %s:%s\n", ip, port_str);
    fflush(stdout);
    return fd;
}

/* 打印一行"可读不可见字符"的字节：把 CR/LF/ESC 之类显形。
 * 网络编程里 90% 的诡异问题，最后都是"你发的字节和你以为的不一样"。 */
static void dump_bytes(const char *tag, const unsigned char *buf, ssize_t n)
{
    printf("%s[%zd]: ", tag, n);
    for (ssize_t i = 0; i < n; i++) {
        unsigned char c = buf[i];
        if (c == '\n')      printf("\\n");
        else if (c == '\r') printf("\\r");
        else if (c == '\t') printf("\\t");
        else if (c >= 32 && c < 127) putchar(c);
        else printf("\\x%02x", c);
    }
    putchar('\n');
    fflush(stdout);
}

static void handle_conn(int cfd)
{
    unsigned char buf[BUF_SIZE];

    for (;;) {
        ssize_t n = read(cfd, buf, sizeof buf);

        if (n == 0) {                       /* 对方 close() -> EOF，不是错误 */
            printf("peer closed\n"); break;
        }
        if (n < 0) {
            if (errno == EINTR) continue;   /* 被信号打断 -> 必须重试 */
            perror("read"); break;
        }

        /* ★ 短读演示：TCP 是字节流，一次 read 拿到 n < sizeof buf 完全正常。
         *   想要"读满 N 字节"必须自己循环——这里故意不循环，所以只要客户端
         *   一次发超过 4096 字节，你就能亲眼看到它被切成好几次。 */
        printf("recv %zd byte(s)\n", n);
        dump_bytes("  <- ", buf, n);

        /* ★ 短写：write 可能只写出去一部分。阻塞 fd 上通常不会，但契约上允许。
         *   所以哪怕 echo 也要写成"写满为止"的循环。 */
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = write(cfd, buf + off, (size_t)(n - off));
            if (w < 0) {
                if (errno == EINTR) continue;
                /* 对方已经走了：这是正常现象，不是致命错误 */
                if (errno == EPIPE || errno == ECONNRESET) {
                    printf("peer gone (%s), drop this connection\n", strerror(errno));
                } else {
                    perror("write");
                }
                goto out;
            }
            off += w;
        }
    }

out:
    close(cfd);                             /* fd 是有限资源，必须还回去 */
    printf("connection closed\n");
    fflush(stdout);
}

int main(int argc, char **argv)
{
    const char *ip   = (argc > 1) ? argv[1] : "127.0.0.1";
    const char *port = (argc > 2) ? argv[2] : "8080";

    int lfd = make_listener(ip, port);
    if (lfd < 0) return 1;

    for (;;) {
        /* accept 返回**新 fd**；失败通常是"被信号打断"或"连接在队列里被 RST"，
         * 两者都不应该让服务器退出 */
        int cfd = accept(lfd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            if (errno == ECONNABORTED) continue;
            perror("accept");
            break;
        }
        printf("accept: fd=%d\n", cfd);
        fflush(stdout);

        /* ★ 一次只伺候一个连接（串行）。
         *   感受一下：用 `nc 127.0.0.1 8080` 连上之后**什么都不发**，
         *   另一个终端再连就完全没反应 —— 这就是 M2 要解决的"整个服务器卡死"。
         *   下一版本改成 fork()，再下一版本改成 epoll 事件循环。 */
        handle_conn(cfd);
    }

    close(lfd);
    return 0;
}
