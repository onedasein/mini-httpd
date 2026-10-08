#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#define PORT     8080
#define BUF_SIZE 4096
int main(int argc, char* argv[]) {
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

    // bind
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        exit(1);
    }

    // listen
    if (listen(fd, SOMAXCONN) < 0) {
        perror("listen");
        exit(1);
    }

    while (1) {
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
                ssize_t written = 0;
                while (written < n) {
                    ssize_t w = write(conn_fd, buf + written, (size_t)(n - written));
                    if (w < 0) {
                        perror("write");
                        continue;
                    }
                    written += w;
                }
            } else if (n < 0) {
                perror("read");
                continue;
            }
        }
        close(conn_fd);
    }
    close(fd);
}
