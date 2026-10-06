/* http.h — HTTP/1.1 请求解析与响应头拼装（与 socket 无关，可单独测） */
#ifndef MINI_HTTPD_HTTP_H
#define MINI_HTTPD_HTTP_H

#include <stddef.h>

#include "httpd.h"

/* header 名/值就地指向读缓冲内部（解析时就地把 ':' 改成 '\0'），不额外分配 */
struct http_header {
    char *name;
    char *value;
};

struct http_request {
    char *method;        /* "GET" / "HEAD" */
    char *target;        /* 原始 request-target，含 query */
    char *version;       /* "HTTP/1.1" / "HTTP/1.0" */
    char *path;          /* URL 解码 + 去 query 之后的路径（malloc，需 http_request_free） */
    struct http_header headers[HTTPD_MAX_HEADERS];
    size_t nheaders;
    int head_only;
    int keep_alive;      /* 记录客户端意愿；v1 一律回 Connection: close（keep-alive 是 T2.6） */
};

enum http_parse_status {
    HTTP_PARSE_OK = 0,
    HTTP_PARSE_BAD_REQUEST,            /* 400 */
    HTTP_PARSE_URI_TOO_LONG,           /* 414 */
    HTTP_PARSE_HEADER_TOO_LARGE,       /* 431 */
    HTTP_PARSE_METHOD_NOT_ALLOWED,     /* 405 */
    HTTP_PARSE_VERSION_UNSUPPORTED,    /* 505 */
    HTTP_PARSE_PAYLOAD_TOO_LARGE       /* 413 */
};

/* 增量查找 header 结束（空行之后的偏移）。*scan 保存扫描进度（调用方初始化为 0），
 * 找不到返回 0 并推进 *scan —— 这样每次 read 只需重扫最后 3 个字节，不必整个重来。
 * 同时接受 CRLF 和裸 LF（概念图 §7 的宽容实现）。 */
size_t http_scan_header_end(const char *buf, size_t len, size_t *scan);

/* 就地解析 buf[0..len)（len 必须是 header 结束位置）。会修改 buf。 */
enum http_parse_status http_parse_request(char *buf, size_t len, struct http_request *req);

void http_request_free(struct http_request *req);

/* 大小写不敏感地取 header（概念图 §7：header 名大小写不敏感） */
const struct http_header *http_find_header(const struct http_request *req, const char *name);

const char *http_status_text(int status);

/* 按扩展名给 Content-Type（小表；不认识就给 application/octet-stream） */
const char *http_content_type(const char *path);

/* 拼响应头；extra 可塞一行额外 header（如 "Allow: GET, HEAD\r\n"）。
 * 返回写入长度，缓冲区不够返回 -1。 */
int http_build_header(char *out, size_t cap, int status, const char *ctype,
                      long long body_len, const char *extra);

/* %XX 解码（只解一次）。遇到非法 %、编码出的 NUL、反斜杠、或 query/fragment 就截断/拒绝：
 * 成功返回 malloc 的字符串，失败返回 NULL。 */
char *http_url_decode(const char *s, size_t len);

#endif
