/* src/http.c — 字节 → 请求结构：请求行 + 字段行 + 上限 + 收尾检查 + path
 *
 * 职责边界（对照 docs/rfc/RFC与系统调用速查.md §8.4）：
 *   **找 head 边界不是这里的事** —— 那是 http_scan_header_end() 的活；
 *   本函数收到的 len 就是 head 总长（含结尾那个空行），所以内部**不再判一次边界**。
 *
 * 规则出处（每条都能指到一份文档）：
 *   · 请求行三段 / 单个 SP 分隔 / 裸 CR        → RFC 9112 §2.2、§3；docs/rfc/解析待定项.md #4 #5 #10
 *   · method 分流（已知? 白名单?）             → CONTRACTS.md §0（改那里就要改这里的 KNOWN_METHODS）
 *   · 1.1 必须有 Host                          → RFC 9112 §3.2；CONTRACTS.md §1
 *   · 版本：形状对但不支持→505，形状不对→400   → RFC 9110 §15.6.6；解析待定项 #6 #7 #8
 *   · 上限（431/414/413）                      → CONTRACTS.md §1；数字真源 src/httpd.h
 *   · Content-Length 重复且值不同→400          → RFC 9112 §6.3；解析待定项 #12
 *   · path 是唯一 malloc，调用方 free          → CONTRACTS.md §2
 *
 * ⚠ 三处**暂定**规则（先让代码能跑，等对应决策定稿再复核；改动都集中在本文件）：
 *   1. path 的 `?`/`#` 截断 + %XX 解码 + 拒绝 %00/反斜杠 —— 按 v1 语义写的，§3（target 与解码）还没定稿；
 *      v1 的已知缺口：解码出的控制字符（%0a 之类）没挡 —— 这里同样没挡，标了 TODO。
 *   2. 字段值两端 OWS 会被裁掉 —— §2（字段语法）还没定稿。
 *   3. method 只判"在不在已知清单里"；不校验它是不是合法 token（`G@T` 这类会落到 501，严格该是 400）。
 */
#define _POSIX_C_SOURCE 200809L /* strncasecmp 需要 POSIX 2008（和 v1 一样） */

#include "http.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ============================ 小工具（全部 static） ============================ */

static int is_digit(char c) {
    return c >= '0' && c <= '9';
}

/* 切片 vs 字面量：切片**非 NUL 结尾**，所以先比长度再 memcmp（大小写敏感：method / version）。 */
static int slice_eq(const char *s, size_t n, const char *lit) {
    return n == strlen(lit) && memcmp(s, lit, n) == 0;
}

/* 同上，但大小写不敏感：给**字段名**用（Host / hOsT / CONTENT-LENGTH 都要认）。 */
static int slice_eq_ci(const char *s, size_t n, const char *lit) {
    return n == strlen(lit) && strncasecmp(s, lit, n) == 0;
}

/* 已知方法清单 —— 抄自 CONTRACTS.md §0（只影响 405/501 的分流）。
 * 在清单里但不在白名单 {GET, HEAD} → HTTP_ERR_METHOD(405)；不在清单里 → HTTP_ERR_NOT_IMPLEMENTED(501)。 */
// clang-format off
static const char *KNOWN_METHODS[] = {
    "ACL", "BASELINE-CONTROL", "BIND", "CHECKIN", "CHECKOUT", "CONNECT", "COPY", "DELETE", "GET",
    "HEAD", "LABEL", "LINK", "LOCK", "MERGE", "MKACTIVITY", "MKCALENDAR", "MKCOL", "MKREDIRECTREF",
    "MKWORKSPACE", "MOVE", "OPTIONS", "ORDERPATCH", "PATCH", "POST", "PRI", "PROPFIND", "PROPPATCH",
    "PUT", "QUERY", "REBIND", "REPORT", "SEARCH", "TRACE", "UNBIND", "UNCHECKOUT", "UNLINK", "UNLOCK",
    "UPDATE", "UPDATEREDIRECTREF", "VERSION-CONTROL",
};
// clang-format on
// 返回1表示是已知方法，返回0表示不是已知方法
static int is_known_method(const char *m, size_t n) {
    for (size_t i = 0; i < sizeof KNOWN_METHODS / sizeof KNOWN_METHODS[0]; i++) {
        if (slice_eq(m, n, KNOWN_METHODS[i])) return 1;
    }
    return 0;
}

/* 取一行（不含行尾 CRLF）。成功 0，并把 *pp 推到下一行开头；失败 -1（调用方回 400）。
 *   · 行尾 CRLF 与裸 LF 都接受
 *   · 行内容里出现**裸 CR**（不是紧跟 \n 的那个）→ 失败（RFC 9112 禁止；解析待定项 #10） */
static int next_line(const char **pp, const char *end, const char **line, size_t *line_len) {
    const char *p = *pp;
    const char *nl;
    size_t n;

    if (p >= end) return -1;
    nl = memchr(p, '\n', (size_t)(end - p));
    if (nl == NULL) return -1;
    n = (size_t)(nl - p);
    if (n > 0 && p[n - 1] == '\r') n--; /* 吃掉 CRLF 的 CR */
    if (memchr(p, '\r', n) != NULL) return -1;

    *line = p;
    *line_len = n;
    *pp = nl + 1;
    return 0;
}

/* 有界十进制解析（**不能用 strtoull**：切片没有 NUL 结尾，它会读越界）。
 * 空串 / 非数字 / 溢出 → -1（调用方回 400）。 */
static int parse_content_length(const char *v, size_t n, size_t *out) {
    size_t acc = 0;

    if (n == 0) return -1;
    for (size_t i = 0; i < n; i++) {
        unsigned d;
        if (!is_digit(v[i])) return -1;
        d = (unsigned)(v[i] - '0');
        if (acc > (SIZE_MAX - d) / 10) return -1; /* 溢出是非法输入，不是回绕 */
        acc = acc * 10 + d;
    }
    *out = acc;
    return 0;
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* target → NUL 结尾的 path（唯一 malloc 的地方）。
 *   · '?' 或 '#' 之后丢弃（query / fragment 不参与文件映射）
 *   · %XX 解码；非法 %XX、解码出的 NUL、反斜杠 → 失败（返回 NULL → 400）
 * ⚠ 暂定：规则按 v1，等 docs/rfc/解析待定项.md §3 定稿后复核这一处。 */
static char *url_to_path(const char *target, size_t target_len) {
    char *out = malloc(target_len + 1);
    size_t o = 0;

    if (out == NULL) return NULL;
    for (size_t i = 0; i < target_len; i++) {
        char c = target[i];

        if (c == '?' || c == '#') break;
        if (c == '%') {
            int hi;
            int lo;
            if (i + 2 >= target_len) {
                free(out);
                return NULL;
            }
            hi = hex_val(target[i + 1]);
            lo = hex_val(target[i + 2]);
            if (hi < 0 || lo < 0) {
                free(out);
                return NULL;
            }
            c = (char)((hi << 4) | lo);
            i += 2;
            if (c == '\0') { /* %00：会截断 C 字符串 → 拒绝 */
                free(out);
                return NULL;
            }
            /* TODO(§3)：解码出的控制字符（< 0x20）也该拒；v1 漏了，这一版先照旧 */
        }
        if (c == '\\') { /* 某些平台/代理把它当分隔符 → 拒绝 */
            free(out);
            return NULL;
        }
        out[o++] = c;
    }
    out[o] = '\0';
    return out;
}

/* ============================ 请求行 ============================ */

static http_err parse_request_line(http_request *req, const char *line, size_t llen) {
    const char *sp1;
    const char *sp2;

    if (llen == 0) return HTTP_ERR_BAD_REQUEST;
    if (llen > (size_t)MAX_LINE_LEN) return HTTP_ERR_HEADER_TOO_LARGE;

    /* 三段之间只能是**单个 SP**：#4 连续空格、#5 TAB 都被下面两条挡掉 */
    sp1 = memchr(line, ' ', llen);
    if (sp1 == NULL) return HTTP_ERR_BAD_REQUEST;
    sp2 = memchr(sp1 + 1, ' ', (size_t)((line + llen) - (sp1 + 1)));
    if (sp2 == NULL) return HTTP_ERR_BAD_REQUEST;

    req->method = line;
    req->method_len = (size_t)(sp1 - line);
    req->target = sp1 + 1;
    req->target_len = (size_t)(sp2 - (sp1 + 1));
    req->version = sp2 + 1;
    req->version_len = (size_t)((line + llen) - (sp2 + 1));
    if (req->method_len == 0 || req->target_len == 0 || req->version_len == 0) return HTTP_ERR_BAD_REQUEST;

    /* method：#3 超长 → 501；不在已知清单（含小写 get）→ 501（#2）；已知但不在白名单 → 405 */
    if (req->method_len > (size_t)MAX_METHOD_LEN) return HTTP_ERR_NOT_IMPLEMENTED;
    if (!is_known_method(req->method, req->method_len)) return HTTP_ERR_NOT_IMPLEMENTED;
    if (!slice_eq(req->method, req->method_len, "GET") && !slice_eq(req->method, req->method_len, "HEAD")) {
        return HTTP_ERR_METHOD;
    }
    req->head_only = slice_eq(req->method, req->method_len, "HEAD");

    /* version：先看形状，再看支不支持（#6 HTTP/1、#7 尾随空格、#8 空 → 400） */
    if (req->version_len > (size_t)MAX_VERSION_LEN) return HTTP_ERR_VERSION;
    if (req->version_len == 8 && memcmp(req->version, "HTTP/", 5) == 0 && is_digit(req->version[5]) &&
        req->version[6] == '.' && is_digit(req->version[7])) {
        if (slice_eq(req->version, req->version_len, "HTTP/1.1") ||
            slice_eq(req->version, req->version_len, "HTTP/1.0")) {
            return HTTP_OK;
        }
        return HTTP_ERR_VERSION; /* 形如 HTTP/x.y 但不受支持 → 505 */
    }
    return HTTP_ERR_BAD_REQUEST;
}

/* ============================ 入口 ============================ */

http_err http_request_parse(const char *head, size_t len, http_request *req) {
    const char *end;
    const char *p;
    const char *line = NULL;
    size_t line_len = 0;
    int saw_host = 0;
    int have_content_length = 0;
    size_t content_length = 0;
    http_err err;

    memset(req, 0, sizeof *req); /* 契约：req 会被完全覆写 */
    if (len > (size_t)MAX_HEADER_LEN) return HTTP_ERR_HEADER_TOO_LARGE;

    end = head + len;
    p = head;

    /* ---- 1. 请求行 ---- */
    if (next_line(&p, end, &line, &line_len) != 0) return HTTP_ERR_BAD_REQUEST;
    err = parse_request_line(req, line, line_len);
    if (err != HTTP_OK) return err;

    /* ---- 2. target：第一版只支持 origin-form（以 '/' 开头），长度看上限 ---- */
    if (req->target[0] != '/') return HTTP_ERR_BAD_REQUEST; /* 绝对 URI / "*" → 400（#9） */
    if (req->target_len > (size_t)MAX_PATH_LEN) return HTTP_ERR_URI_TOO_LONG;

    /* ---- 3. 字段行：一直读到空行为止 ---- */
    for (;;) {
        const char *colon;
        const char *name;
        const char *value;
        size_t name_len;
        size_t value_len;

        if (next_line(&p, end, &line, &line_len) != 0) return HTTP_ERR_BAD_REQUEST;
        if (line_len == 0) break; /* 空行 = head 结束 */
        if (line_len > (size_t)MAX_LINE_LEN) return HTTP_ERR_HEADER_TOO_LARGE;

        colon = memchr(line, ':', line_len);
        if (colon == NULL || colon == line) return HTTP_ERR_BAD_REQUEST;
        name = line;
        name_len = (size_t)(colon - line);
        value = colon + 1;
        value_len = (size_t)((line + line_len) - value);
        for (size_t i = 0; i < name_len; i++) { /* 名字里不许有空白（含冒号前的空白） */
            if (name[i] == ' ' || name[i] == '\t') return HTTP_ERR_BAD_REQUEST;
        }
        while (value_len > 0 && (*value == ' ' || *value == '\t')) { /* 去前导 OWS（暂定，§2） */
            value++;
            value_len--;
        }
        while (value_len > 0 && (value[value_len - 1] == ' ' || value[value_len - 1] == '\t')) {
            value_len--; /* 去尾随 OWS（暂定，§2） */
        }

        if (req->nheaders >= (size_t)MAX_HEADERS) return HTTP_ERR_TOO_MANY_HEADERS;
        req->headers[req->nheaders].name = name;
        req->headers[req->nheaders].name_len = name_len;
        req->headers[req->nheaders].value = value;
        req->headers[req->nheaders].value_len = value_len;
        req->nheaders++;

        if (slice_eq_ci(name, name_len, "Host")) {
            saw_host = 1;
        } else if (slice_eq_ci(name, name_len, "Content-Length")) {
            size_t cl;
            if (parse_content_length(value, value_len, &cl) != 0) return HTTP_ERR_BAD_REQUEST;
            if (have_content_length && cl != content_length) return HTTP_ERR_BAD_REQUEST; /* #12 */
            content_length = cl;
            have_content_length = 1;
        }
    }

    /* ---- 4. 收尾检查 ---- */
    /* RFC 9112 §3.2：HTTP/1.1 请求没有 Host 必须回 400（1.0 不要求） */
    if (slice_eq(req->version, req->version_len, "HTTP/1.1") && !saw_host) return HTTP_ERR_BAD_REQUEST;
    if (content_length > (size_t)MAX_BODY) return HTTP_ERR_BODY_TOO_LARGE; /* 413 */
    req->content_length = content_length; /* #11：没有该 header 与 Content-Length: 0 不区分，都记 0 */

    /* ---- 5. path（唯一 malloc；调用方负责 http_request_free） ---- */
    req->path = url_to_path(req->target, req->target_len);
    if (req->path == NULL) return HTTP_ERR_BAD_REQUEST;

    return HTTP_OK;
}

void http_request_free(http_request *req) {
    if (req == NULL) return;
    free(req->path);
    memset(req, 0, sizeof *req); /* 幂等：重复调用安全（契约 §2） */
}
// 返回1表示已找到，返回0表示还未找到
/*
 *   \n\n
 *   \n\r\n
 *   \r\n\n
 *   \r\n\r\n
 */
int http_scan_header_end(const char *buf, size_t len, size_t *scan, size_t *head_len) {
    size_t i = *scan;
    while (i < len) {
        if (buf[i] != '\n') {
            i++;
            continue;
        }

        if (i + 1 >= len) {
            *scan = (len >= 3) ? len - 3 : 0;
            return 0;
        }

        if (buf[i + 1] == '\n') {
            *head_len = i + 2;
            *scan = i + 2;
            return 1;
        }

        if (buf[i + 1] == '\r') {
            if (i + 2 >= len) {
                *scan = (len >= 3) ? len - 3 : 0;
                return 0;
            }
            if (buf[i + 2] == '\n') {
                *head_len = i + 3;
                *scan = i + 3;
                return 1;
            }
        }
        i++;
    }
    *scan = (len >= 3) ? len - 3 : 0;
    return 0;
}

/* ============================ 请求侧：按名字找字段 ============================ */

/* 大小写不敏感、按长度比较。name 是 NUL 结尾的字面量（调用方传 "Host" 这种）。
 * 注意：比较只能用 slice_eq_ci —— **不能** strcasecmp(req->headers[i].name, ...)，
 * 因为切片没有 NUL 结尾，它会一路比到读缓冲里别的字节去。 */
const http_header *http_find_header(const http_request *req, const char *name) {
    for (size_t i = 0; i < req->nheaders; i++) {
        if (slice_eq_ci(req->headers[i].name, req->headers[i].name_len, name)) return &req->headers[i];
    }
    return NULL;
}

/* ============================ 响应侧 ============================ */

/* 状态码 → 状态文本。集合与 CONTRACTS.md §1 一致；表里没有的给兜底串。
 * **绝不能返回 NULL**：调用方会直接把它 snprintf 进状态行。 */
const char *http_status_text(int status) {
    switch (status) {
        case 200:
            return "OK";
        case 400:
            return "Bad Request";
        case 403:
            return "Forbidden";
        case 404:
            return "Not Found";
        case 405:
            return "Method Not Allowed";
        case 413:
            return "Payload Too Large";
        case 414:
            return "URI Too Long";
        case 431:
            return "Request Header Fields Too Large";
        case 500:
            return "Internal Server Error";
        case 501:
            return "Not Implemented";
        case 503:
            return "Service Unavailable"; /* M2 T2.1b：连接表拿不到槽位时回它 */
        case 505:
            return "HTTP Version Not Supported";
        default:
            return "Error";
    }
}

/* 扩展名 → Content-Type。表与 v1 保持一致 —— T1.7a 要移植 v1 的断言，而它们是**精确匹配**：
 *   .css / .js / .txt 取第一段要带 ';'（=> 必须带 charset），.json 必须**不带** charset。 */
// clang-format off
static const struct { const char *ext; const char *type; } MIME_TYPES[] = {
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
// clang-format on

const char *http_content_type(const char *path) {
    const char *dot = strrchr(path, '.');
    const char *slash = strrchr(path, '/');

    /* 只认"最后一个 '/' 之后"的点："/a.b/c" 里的 .b 不是扩展名
     *（v1 直接 strrchr('.')，这些例子上结果一样，但按语义写更站得住） */
    if (dot == NULL || (slash != NULL && dot < slash)) return "application/octet-stream";
    for (size_t i = 0; i < sizeof MIME_TYPES / sizeof MIME_TYPES[0]; i++) {
        if (strcasecmp(dot, MIME_TYPES[i].ext) == 0) return MIME_TYPES[i].type;
    }
    return "application/octet-stream"; /* 不认识就给二进制流，绝不猜 */
}

/* 一次性拼出响应头：状态行 + Server/Content-Type/Content-Length/Connection: close + extra + 空行。
 *   extra — 可塞额外一行（如 "Allow: GET, HEAD\r\n"），没有就传 NULL
 * 返回写入的字节数；放不下返回 -1 —— 调用方据此回 500，**绝不要发截断的头**。
 * 坑：snprintf 的返回值是"本该写入的长度"，所以要拿它跟 cap 比，而不是看实际写了多少。 */
int http_build_response_head(char *out, size_t cap, int status, const char *ctype, long long body_len,
                             const char *extra) {
    int n;

    if (ctype == NULL) ctype = "application/octet-stream"; /* 防御：上游漏给类型时别打出 "(null)" */
    n = snprintf(out, cap,
                 "HTTP/1.1 %d %s\r\n"
                 "Server: %s\r\n"
                 "Content-Type: %s\r\n"
                 "Content-Length: %lld\r\n"
                 "Connection: close\r\n"
                 "%s"
                 "\r\n",
                 status, http_status_text(status), SERVER_VERSION, ctype, body_len, extra != NULL ? extra : "");
    if (n < 0 || (size_t)n >= cap) return -1;
    return n;
}
