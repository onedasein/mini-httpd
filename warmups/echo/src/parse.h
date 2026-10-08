#ifndef ECHO_PARSE_H
#define ECHO_PARSE_H

#define HOST_MAX 64

struct config {
    char host[HOST_MAX];
    int  port;
};

// 解析命令行：argv[1] = <ip>，argv[2] = <port>
// 成功返回 0，失败返回 -1。
// 故意不 exit()：一旦 exit()，这个函数就没法被单元测试
// （一条用例会把整个测试进程带走）。
int parse_args(int argc, char* argv[], struct config* out);

#endif
