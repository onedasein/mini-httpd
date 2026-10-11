/* src/http.h — HTTP 请求的解析结果与解析器接口。
 * 只依赖 httpd.h 中的常量；不涉及 socket、fd、IO 细节。 */
#pragma once
#include <stddef.h>
#include "httpd.h"

/* ---------- 数据结构 ---------- */

/* 单个 header 字段。
 * name/value 都是指向解析器读缓冲（rbuf）内部的指针，并未另行 malloc。
 * 生命周期 ≤ 读缓冲：rbuf 被复用或释放后，这些指针立即失效。 */
typedef struct {
    const char *name;       /* 借用：指向 rbuf 内部，非 NUL 结尾 */
    size_t name_len;
    const char *value;      /* 借用：指向 rbuf 内部，非 NUL 结尾 */
    size_t value_len;
} http_header;

/* 解析结果。
 * 所有 "借用" 字段都指向解析器读缓冲，生命周期 ≤ 读缓冲；
 * v1 中唯一 malloc 出来的字段是 path（调用方需负责 free）。 */
typedef struct {
    /* 借用：指向 rbuf 内部 */
    const char *method;                    /* 例："GET"，非 NUL 结尾 */
    size_t method_len;
    const char *target;                    /* 例："/index.html?x=1"，原始 request-target，非 NUL 结尾 */
    size_t target_len;                     /* ← 2026-10-11 补：日志/排障要看原始请求行；path 已解码去 query，回不去 */
    const char *version;                   /* 例："HTTP/1.1"，非 NUL 结尾 */
    size_t version_len;

    /* 借用：header 数组本体与每个元素都指向 rbuf 内部 */
    http_header headers[MAX_HEADERS];
    size_t nheaders;

    /* 拥有：解析器内部 malloc 出来的、以 '\0' 结尾的字符串。
     * 调用方必须通过 http_request_free() 释放（或自行 free(req->path)）。 */
    char *path;

    /* 解析后的 Content-Length；没有该 header 时为 0。 */
    size_t content_length;

    /* ← 2026-10-11 补：method 是 "HEAD" 时置 1。
     * 没有它，handler 就得自己拿 method/method_len 去比字符串 ——
     * 而 method 不是 NUL 结尾的切片，那是协议知识漏进 main.c 的口子。 */
    int head_only;
} http_request;

/* ---------- 错误码 ---------- */
/* 每个值对应一个 HTTP 状态码（见注释）。 */
enum http_err {
    HTTP_OK = 0,  /* 200 OK              —— 解析成功 */
    HTTP_ERR_BAD_REQUEST,       /* 400 Bad Request */
    HTTP_ERR_METHOD,            /* 501 Not Implemented */
    HTTP_ERR_URI_TOO_LONG,      /* 414 URI Too Long */
    HTTP_ERR_VERSION,           /* 505 HTTP Version Not Supported */
    HTTP_ERR_HEADER_TOO_LARGE,  /* 431 Request Header Fields Too Large */
    HTTP_ERR_TOO_MANY_HEADERS,  /* 431 Request Header Fields Too Large */
    HTTP_ERR_BODY_TOO_LARGE,    /* 413 Payload Too Large */
    HTTP_ERR_INTERNAL,          /* 500 Internal Server Error */
};

/* ---------- 函数原型 ---------- */

/* 把 rbuf 中前 len 个字节的原始 HTTP 请求解析为 *req。
 *
 * 输入：
 *   rbuf, len  — 读缓冲及有效字节数。只读；本函数不会修改 rbuf。
 *   req        — 输出参数，会被完全覆写；调用前无需初始化。
 *
 * 返回：
 *   HTTP_OK 表示成功；否则返回 enum http_err，调用方据此回对应状态码。
 *
 * 是否修改入参：
 *   rbuf 不被修改；*req 会被写入。
 *
 * 谁负责 free：
 *   成功时 req->path 由本函数 malloc，调用方负责释放（可用
 *   http_request_free()）。
 *   失败时本函数保证不留下任何已分配内存，调用方无需清理。
 *   其余字段指向 rbuf，生命周期 ≤ rbuf，不应 free。 */
enum http_err http_request_parse(const char *rbuf, size_t len, http_request *req);

/* 释放 http_request_parse 分配的资源（v1 中仅 path）。
 *
 * 输入：req —— 可以为 NULL；非 NULL 时会被重置为 0。
 * 返回：无。
 * 是否修改入参：会把 *req 清零（保证重复调用安全）。
 * 谁负责 free：本函数负责释放内部资源；调用方不要再 free(req->path)。 */
void http_request_free(http_request *req);

/* ---------- main.c 需要的其余接口（2026-10-11 补，实现都在 http.c 里，由你写）----------
 *
 * 原来漏了两类东西：①「请求头到哪结束」的扫描器；② 响应侧（状态文本 / MIME / 拼响应头）。
 * 主循环拿不到 ① 就只能自己 memmem（协议知识漏进 main.c，而且漏判裸 LF、每次重扫 O(n²)）；
 * 拿不到 ② 就得在 main.c 里手写响应头（更糟）。 */

/* 增量寻找 head 边界（就是那个空行）。
 *   buf, len   — 目前已经攒到的全部字节
 *   *scan      — 扫描进度：调用方第一次传 0，之后原样带回（函数会推进它）
 *   *head_len  — 找到时写出 head 总长（= 空行结束之后的第一个字节偏移）
 * 返回：1 = 找到；0 = 还没找到（数据不够，请继续 read）。
 * 关键不变式：返回 0 之后再喂新数据，只能从 *scan 往回退 3 字节重扫
 *            —— "\r\n\r\n" 可能刚好被一次 read 切断。必须同时认 CRLF 与裸 LF。 */
int http_scan_header_end(const char *buf, size_t len, size_t *scan, size_t *head_len);

/* 按名字找 header（大小写不敏感、按长度比较）。找不到返回 NULL。
 * 注意：**不要**写 strcasecmp(req->headers[i].name, "Host") —— 切片没有 NUL 结尾，
 *       它会一路比到 rbuf 里别的字节去。先比 name_len 再用 strncasecmp。 */
const http_header *http_find_header(const http_request *req, const char *name);

/* 状态码 → 状态文本（表里没有就兜底，比如 "Error"）。 */
const char *http_status_text(int status);

/* 文件路径 → Content-Type（只看扩展名；不认识就给 application/octet-stream）。
 * path 必须是 NUL 结尾的（传 req->path 或 static_file.real，别传裸切片）。 */
const char *http_content_type(const char *path);

/* 一次性拼出响应头：状态行 + Server/Content-Type/Content-Length/Connection: close + extra + 空行。
 *   extra — 可塞额外一行，如 "Allow: GET, HEAD\r\n"；没有就传 NULL
 * 返回：写入的字节数；缓冲区放不下返回 -1（调用方回 500，别把截断的头发出去）。 */
int http_build_response_head(char *out, size_t cap, int status, const char *ctype,
                             long long body_len, const char *extra);
