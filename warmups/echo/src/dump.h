#ifndef ECHO_DUMP_H
#define ECHO_DUMP_H

#include <stddef.h>

// 纯函数：把字节里不可见的部分显形（CR/LF/TAB，其他按 \xNN），可打印字符原样。
// 网络编程里 90% 的怪问题，最后都是"你发的字节和你以为的不一样"。
// 最多写 cap 字节（含结尾 '\0'），语义同 snprintf：
// 返回「本该写入的字符数」（不含 '\0'），返回值 >= cap 表示被截断。
// 没有 I/O、没有全局状态 —— 所以它能被单元测试直接喂输入、断言输出。
size_t format_bytes(char* out, size_t cap, const unsigned char* buf, size_t n);

// 薄壳：把 format_bytes 的结果连同 tag 和长度打到 fd。
// 把 I/O 挤到边缘，好测的核心逻辑留在上面那个纯函数里。
void dump_bytes(int fd, const char* tag, const unsigned char* buf, size_t n);

#endif
