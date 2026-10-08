#include "parse.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

int parse_args(int argc, char* argv[], struct config* out) {
    if (out == NULL || argv == NULL) {
        return -1;
    }
    if (argc != 3) {        // 先查个数，才敢碰 argv[1] / argv[2]
        return -1;
    }

    const char* host = argv[1];
    const char* port = argv[2];
    if (host == NULL || port == NULL || host[0] == '\0') {
        return -1;
    }
    if (strlen(host) >= HOST_MAX) {     // 用 >= 是为了给结尾 '\0' 留位置
        return -1;
    }

    char* end = NULL;
    errno = 0;
    long v = strtol(port, &end, 10);
    if (errno != 0 || end == port || *end != '\0') {
        return -1;
    }
    if (v < 1 || v > 65535) {
        return -1;
    }

    strcpy(out->host, host);    // 长度上面已经查过，放得下
    out->port = (int)v;
    return 0;
}
