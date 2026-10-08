// echo 热身（T0.3）— 最小 TCP echo server
#define _POSIX_C_SOURCE 200809L

#include "dump.h"
#include "parse.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define BUF_SIZE 4096

int main(int argc, char* argv[]) {
    struct config cfg;
    if (parse_args(argc, argv, &cfg) < 0) {
        fprintf(stderr, "用法: %s <ip> <port>\n", argv[0]);
        exit(1);
    }

    // 1.socket
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        exit(1);
    }

    // 允许端口复用
    int opt = 1;
    if ((setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) < 0) {
        perror("setsockopt");
        exit(1);
    }

    // 2.bind
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)cfg.port);
    if (inet_pton(AF_INET, cfg.host, &addr.sin_addr) != 1) {
        fprintf(stderr, "ip 不合法: %s\n", cfg.host);
        exit(1);
    }
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        exit(1);
    }

    // 3.listen
    if (listen(fd, SOMAXCONN) < 0) {
        perror("listen");
        exit(1);
    }
    printf("listening on %s:%d\n", cfg.host, cfg.port);
    fflush(stdout);     // stdout 重定向到文件时是全缓冲：不 flush，日志会随进程一起消失

    while (1) {
        // 4.accept
        socklen_t addrlen = sizeof(addr);
        int conn_fd = accept(fd, (struct sockaddr*)&addr, &addrlen);
        if (conn_fd < 0) {
            perror("accept");
            continue;
        }

        // accept 返回的 conn_fd 已经是「已连接」套接字，不能再 connect

        // 收发数据
        char buf[BUF_SIZE];
        ssize_t n;

        while ((n = read(conn_fd, buf, sizeof(buf))) != 0) {
            if (n > 0) {
                // 日志：收到的字节到底长什么样（短读现场也留证）
                dump_bytes(STDOUT_FILENO, "  <- ", (const unsigned char*)buf, (size_t)n);

                // 短写：write 契约上允许只写出一部分，所以必须循环写到完
                ssize_t written = 0;
                while (written < n) {
                    ssize_t w = write(conn_fd, buf + written, (size_t)(n - written));
                    if (w < 0) {
                        perror("write");
                        break; // 跳过此次回显
                    }
                    written += w;
                }
            } else if (n < 0) {
                perror("read");
                if (errno == EINTR) // 如果系统调用被信号打断，则重试。否则应当关闭连接。
                    continue;
                else
                    break;
            }
        }

        // 5.close
        close(conn_fd);
    }
    close(fd);
}
