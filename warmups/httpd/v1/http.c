/* http.c — 请求解析 + 响应头拼装
 *
 * 三条纪律（对应 TASKS.md §4 的坑表）：
 *   1. 一切按长度处理，绝不 strcpy/strcat/sprintf；也不假设「一行一定以 \0 结尾」——
 *      先按长度切出来，再自己补 \0。
 *   2. 先检查上限再解析，绝不「先拷贝再看超没超」。
 *   3. 只认 GET/HEAD；其余方法在解析层就返回 405，别让它进到文件层。
 */
#define _POSIX_C_SOURCE 200809L

#include "http.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ------------------------------------------------------------------ 解码 */

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

char *http_url_decode(const char *s, size_t len)
{
    char *out = malloc(len + 1);
    if (!out) return NULL;

    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (c == '?' || c == '#') break;              /* query / fragment 不参与路径映射 */
        if (c == '%') {
            if (i + 2 >= len) { free(out); return NULL; }
            int hi = hexval(s[i + 1]);
            int lo = hexval(s[i + 2]);
            if (hi < 0 || lo < 0) { free(out); return NULL; }
            c = (char)((hi << 4) | lo);
            i += 2;
            if (c == '\0') { free(out); return NULL; } /* %00：编码出来的 NUL，拒绝 */
        }
        if (c == '\\') { free(out); return NULL; }     /* 反斜杠：拒绝（§5 的约定） */
        out[o++] = c;
    }
    out[o] = '\0';
    return out;
}

/* -------------------------------------------------------------- 响应头 */

const char *http_status_text(int status)
{
    switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 413: return "Payload Too Large";
    case 414: return "URI Too Long";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 505: return "HTTP Version Not Supported";
    default:  return "Error";
    }
}

int http_build_header(char *out, size_t cap, int status, const char *ctype,
                      long long body_len, const char *extra)
{
    int n = snprintf(out, cap,
                     "HTTP/1.1 %d %s\r\n"
                     "Server: %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %lld\r\n"
                     "Connection: close\r\n"
                     "%s"
                     "\r\n",
                     status, http_status_text(status), HTTPD_SERVER_NAME,
                     ctype, body_len, extra ? extra : "");
    if (n < 0 || (size_t)n >= cap) return -1;   /* 截断了就是 bug，宁可回 500 */
    return n;
}

struct mime_entry {
    const char *ext;
    const char *type;
};

static const struct mime_entry MIMES[] = {
    { ".html", "text/html; charset=utf-8" },
    { ".htm",  "text/html; charset=utf-8" },
    { ".css",  "text/css; charset=utf-8" },
    { ".js",   "application/javascript; charset=utf-8" },
    { ".json", "application/json" },
    { ".txt",  "text/plain; charset=utf-8" },
    { ".png",  "image/png" },
    { ".jpg",  "image/jpeg" },
    { ".jpeg", "image/jpeg" },
    { ".gif",  "image/gif" },
    { ".svg",  "image/svg+xml" },
    { ".ico",  "image/x-icon" },
    { ".pdf",  "application/pdf" },
};

const char *http_content_type(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    for (size_t i = 0; i < sizeof MIMES / sizeof MIMES[0]; i++) {
        if (strcasecmp(dot, MIMES[i].ext) == 0) return MIMES[i].type;
    }
    return "application/octet-stream";   /* 默认二进制流，绝不猜 */
}

/* ---------------------------------------------------------------- 扫描 */

size_t http_scan_header_end(const char *buf, size_t len, size_t *scan)
{
    size_t i = (*scan > 3) ? *scan - 3 : 0;   /* 回退 3 字节，接住被 read 切断的 \r\n\r\n */
    for (; i + 1 < len; i++) {
        if (buf[i] != '\n') continue;
        if (buf[i + 1] == '\n') { *scan = i + 2; return i + 2; }
        if (buf[i + 1] == '\r' && i + 2 < len && buf[i + 2] == '\n') {
            *scan = i + 3;
            return i + 3;
        }
    }
    *scan = (len > 3) ? len - 3 : 0;
    return 0;
}

/* ----------------------------------------------------------------- 解析 */

/* 取一行并就地截断（吃 CRLF 或裸 LF）。返回行长；没有完整行返回 (size_t)-1。
 * 注意：每个 \r 和 \n 都被写成 '\0'，所以行内容绝不会有残留的 CR/LF。 */
static size_t next_line(char **pp, char *end)
{
    char *p = *pp;
    if (p >= end) return (size_t)-1;
    char *nl = memchr(p, '\n', (size_t)(end - p));
    if (!nl) return (size_t)-1;
    char *eol = nl;
    if (eol > p && eol[-1] == '\r') {
        eol[-1] = '\0';          /* 吃掉 \r */
        eol--;                   /* ★ 必须回退，否则行尾的 \r 会算进行长：
                                  *   空行 "\r\n" 会被误判成「长度 1 的行」→ 后续报 400 */
    }
    *nl = '\0';
    *pp = nl + 1;
    return (size_t)(eol - p);
}

static enum http_parse_status parse_content_length(const char *value, long long *out)
{
    if (value[0] == '\0') return HTTP_PARSE_BAD_REQUEST;
    for (const char *q = value; *q; q++) {
        if (*q < '0' || *q > '9') return HTTP_PARSE_BAD_REQUEST;
    }
    errno = 0;
    char *endp = NULL;
    long long v = strtoll(value, &endp, 10);
    if (errno != 0 || endp == value || *endp != '\0' || v < 0) return HTTP_PARSE_BAD_REQUEST;
    *out = v;
    return HTTP_PARSE_OK;
}

enum http_parse_status http_parse_request(char *buf, size_t len, struct http_request *req)
{
    memset(req, 0, sizeof *req);

    if (len > HTTPD_MAX_HEADER) return HTTP_PARSE_HEADER_TOO_LARGE;

    char *p = buf;
    char *end = buf + len;

    /* ---- 请求行：METHOD SP request-target SP HTTP-version CRLF ---- */
    char *line = p;
    size_t llen = next_line(&p, end);
    if (llen == (size_t)-1 || llen == 0) return HTTP_PARSE_BAD_REQUEST;
    if (llen > HTTPD_MAX_LINE) return HTTP_PARSE_HEADER_TOO_LARGE;

    char *sp1 = memchr(line, ' ', llen);
    if (!sp1) return HTTP_PARSE_BAD_REQUEST;
    char *sp2 = NULL;
    if (sp1 + 1 < line + llen) {
        sp2 = memchr(sp1 + 1, ' ', (size_t)(line + llen - (sp1 + 1)));
    }
    if (!sp2) return HTTP_PARSE_BAD_REQUEST;
    *sp1 = '\0';
    *sp2 = '\0';

    req->method  = line;
    req->target  = sp1 + 1;
    req->version = sp2 + 1;
    if (req->method[0] == '\0' || req->target[0] == '\0' || req->version[0] == '\0') {
        return HTTP_PARSE_BAD_REQUEST;
    }

    /* 方法：先要求是纯字母 token，再判白名单（§5：GET/HEAD + 405 + Allow） */
    for (const char *q = req->method; *q; q++) {
        if (!((*q >= 'A' && *q <= 'Z') || (*q >= 'a' && *q <= 'z'))) return HTTP_PARSE_BAD_REQUEST;
    }
    if (strcmp(req->method, "GET") != 0 && strcmp(req->method, "HEAD") != 0) {
        return HTTP_PARSE_METHOD_NOT_ALLOWED;
    }
    req->head_only = (strcmp(req->method, "HEAD") == 0);

    /* 版本：只支持 1.0 / 1.1；是 HTTP/ 开头但不认识 → 505，否则 400 */
    if (strcmp(req->version, "HTTP/1.1") == 0) {
        req->keep_alive = 1;                       /* HTTP/1.1 默认长连接 */
    } else if (strcmp(req->version, "HTTP/1.0") == 0) {
        req->keep_alive = 0;                       /* HTTP/1.0 默认短连接 */
    } else if (strncmp(req->version, "HTTP/", 5) == 0) {
        return HTTP_PARSE_VERSION_UNSUPPORTED;
    } else {
        return HTTP_PARSE_BAD_REQUEST;
    }

    /* request-target：第一版只支持 origin-form（以 / 开头），不做代理形式的绝对 URI */
    size_t tlen = strlen(req->target);
    if (req->target[0] != '/') return HTTP_PARSE_BAD_REQUEST;
    if (tlen > HTTPD_MAX_URI) return HTTP_PARSE_URI_TOO_LONG;

    /* ---- header 字段 ---- */
    int saw_host = 0;
    long long content_length = -1;

    for (;;) {
        char *hstart = p;
        size_t hlen = next_line(&p, end);
        if (hlen == (size_t)-1) return HTTP_PARSE_BAD_REQUEST;
        if (hlen == 0) break;                      /* 空行 = header 结束 */
        if (hlen > HTTPD_MAX_LINE) return HTTP_PARSE_HEADER_TOO_LARGE;

        char *colon = memchr(hstart, ':', hlen);
        if (!colon || colon == hstart) return HTTP_PARSE_BAD_REQUEST;
        *colon = '\0';

        char *name  = hstart;
        char *value = colon + 1;
        char *vend  = hstart + hlen;
        while (value < vend && (*value == ' ' || *value == '\t')) value++;
        while (vend > value && (vend[-1] == ' ' || vend[-1] == '\t')) *--vend = '\0';

        for (char *q = name; *q; q++) {            /* header 名里不许有空白 */
            if (*q == ' ' || *q == '\t') return HTTP_PARSE_BAD_REQUEST;
        }
        if (req->nheaders >= HTTPD_MAX_HEADERS) return HTTP_PARSE_HEADER_TOO_LARGE;

        req->headers[req->nheaders].name  = name;
        req->headers[req->nheaders].value = value;
        req->nheaders++;

        if (strcasecmp(name, "Host") == 0) {
            saw_host = 1;
        } else if (strcasecmp(name, "Connection") == 0) {
            if (strcasecmp(value, "close") == 0) {
                req->keep_alive = 0;
            } else if (strcasecmp(value, "keep-alive") == 0 &&
                       strcmp(req->version, "HTTP/1.0") == 0) {
                req->keep_alive = 1;
            }
        } else if (strcasecmp(name, "Content-Length") == 0) {
            enum http_parse_status st = parse_content_length(value, &content_length);
            if (st != HTTP_PARSE_OK) return st;
        }
    }

    /* RFC 9112：HTTP/1.1 请求缺 Host 必须回 400 */
    if (strcmp(req->version, "HTTP/1.1") == 0 && !saw_host) return HTTP_PARSE_BAD_REQUEST;
    if (content_length > HTTPD_MAX_BODY) return HTTP_PARSE_PAYLOAD_TOO_LARGE;

    /* 路径：解码一次；解码失败（非法 %、%00、反斜杠）一律 400 */
    req->path = http_url_decode(req->target, tlen);
    if (!req->path) return HTTP_PARSE_BAD_REQUEST;

    return HTTP_PARSE_OK;
}

const struct http_header *http_find_header(const struct http_request *req, const char *name)
{
    for (size_t i = 0; i < req->nheaders; i++) {
        if (strcasecmp(req->headers[i].name, name) == 0) return &req->headers[i];
    }
    return NULL;
}

void http_request_free(struct http_request *req)
{
    free(req->path);
    req->path = NULL;
}
