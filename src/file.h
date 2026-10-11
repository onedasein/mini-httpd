/* src/file.h — URL 路径 → 文档根下的真实文件（路径安全是这里唯一的重点）
 *
 * 依赖方向：file.h → httpd.h（只有常量），**不依赖 http.h** ——
 * 「解析器怎么了」和「文件怎么了」是两件事，各报各的错误码，由 main.c 映射成状态码。
 */
#pragma once

#include <stddef.h>

#include "httpd.h"

/* realpath 的目标缓冲（glibc 的 PATH_MAX 也是 4096）。
 * 归在 file 模块里，因为它只服务「拼路径 + realpath」这一件事。 */
#define FILE_MAX_PATH 4096

enum file_status {
    FILE_OK = 0,       /* 200 */
    FILE_BAD_PATH,     /* 400：路径本身非法（".." 段、反斜杠、不以 '/' 开头） */
    FILE_FORBIDDEN,    /* 403：realpath 之后逃出了文档根，或 EACCES */
    FILE_NOT_FOUND,    /* 404：ENOENT / 目录（第一版明确不做目录列表） */
    FILE_IO_ERROR      /* 500：其它 open/fstat 失败 */
};

/* 一次成功解析的结果。fd 已打开，调用方负责 close。
 * 同时给了 tag 和 typedef：`static_file` 与 `struct static_file` 两种写法都能用
 * （http.h 那边是匿名 typedef，只能写 `http_request`）。 */
typedef struct static_file {
    int fd;                      /* -1 表示未打开（调用方初始化时置 -1） */
    long long size;              /* fstat 出来的大小 —— HTTP 的 Content-Length */
    char real[FILE_MAX_PATH];    /* realpath 结果；Content-Type 靠它的扩展名判断 */
} static_file;

/* root_real：启动时算好的文档根 realpath（只算一次，之后只读）。
 * url_path ：req->path（已 URL 解码、已去 query，NUL 结尾）。
 *
 * 成功：返回 FILE_OK，out->fd 已打开（调用方负责 close）。
 * 失败：返回对应错误码，且**不得留下已打开的 fd**（这条是「压测前后 fd 数一致」的前提）。
 *
 * 双保险（只做一条都不够）：
 *   ① 逐段拒绝 ".."、拒绝反斜杠 —— 快速失败，且不会误杀 a..b 这种合法文件名；
 *   ② realpath() 解开符号链接后，结果必须仍以 root_real 为前缀，
 *      且边界要么结束、要么正好是 '/'（否则 /var/wwwroot-evil 能骗过纯前缀比较）。 */
enum file_status file_open_under_root(const char *root_real, const char *url_path,
                                      struct static_file *out);
