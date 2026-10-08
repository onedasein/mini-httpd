#include "dump.h"

#include <stdio.h>
#include <unistd.h>

// 往 out 里追加一个字符，同时记「本该写入多少」。
// 写不下时只计数不落笔，于是返回值仍然等于 snprintf 语义的"需要长度"。
static void put_char(char* out, size_t cap, size_t* k, char c) {
    if (*k + 1 < cap) {     // 永远给结尾 '\0' 留一个位置
        out[*k] = c;
    }
    (*k)++;
}

static void put_escape(char* out, size_t cap, size_t* k, char c) {
    put_char(out, cap, k, '\\');
    put_char(out, cap, k, c);
}

static void put_hex(char* out, size_t cap, size_t* k, unsigned char c) {
    static const char HEX[] = "0123456789abcdef";
    put_char(out, cap, k, '\\');
    put_char(out, cap, k, 'x');
    put_char(out, cap, k, HEX[(c >> 4) & 0x0f]);
    put_char(out, cap, k, HEX[c & 0x0f]);
}

size_t format_bytes(char* out, size_t cap, const unsigned char* buf, size_t n) {
    size_t k = 0;

    if (cap > 0) {
        out[0] = '\0';
    }

    for (size_t i = 0; i < n; i++) {
        unsigned char c = buf[i];
        if (c == '\n') {
            put_escape(out, cap, &k, 'n');
        } else if (c == '\r') {
            put_escape(out, cap, &k, 'r');
        } else if (c == '\t') {
            put_escape(out, cap, &k, 't');
        } else if (c >= 32 && c < 127) {
            put_char(out, cap, &k, (char)c);
        } else {
            put_hex(out, cap, &k, c);
        }
    }

    if (cap > 0) {
        out[(k < cap) ? k : (cap - 1)] = '\0';
    }
    return k;
}

void dump_bytes(int fd, const char* tag, const unsigned char* buf, size_t n) {
    char body[512];
    format_bytes(body, sizeof(body), buf, n);   // 日志而已，截断了也不影响回显

    char line[600];
    int r = snprintf(line, sizeof(line), "%s[%zu]: %s\n", tag, n, body);
    if (r < 0) {
        return;
    }

    size_t len = (size_t)r;
    if (len >= sizeof(line)) {      // snprintf 返回的是「本该写入」的长度
        len = sizeof(line) - 1;
    }
    (void)!write(fd, line, len);
}
