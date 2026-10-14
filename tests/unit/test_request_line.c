/* tests/unit/test_request_line.c — 请求行用例表（T1.2b）
 *
 * 分三个测试函数，每个里面一个简单循环 —— 这是刻意的：
 *   Unity 的断言失败是 longjmp，会**跳出整个测试函数**（这份 vendored Unity 没有 TEST_PROTECT），
 *   所以一个大循环只能报第一条失败；按类别拆成几个函数，就能一次看到每一类的结果。
 *
 *   ① test_request_line_errors   只比错误码（400 / 505 类）
 *   ② test_request_line_methods  只比错误码（405 / 501 类）
 *   ③ test_request_line_fields   比解析出来的字段（借用切片，需要 assert_slice）
 *
 * 每条期望值的出处：
 *   · 状态码与语义   → CONTRACTS.md §0（方法 405/501 分流）、§1（状态码表）
 *   · 语法边界决定   → docs/rfc/解析待定项.md #2 #3 #4 #5 #6 …、docs/adr/
 *
 * 三条纪律：
 *   1. 表驱动，一行一条；循环用 sizeof 算条数，别写死 4（加了行会悄悄不被测）。
 *   2. method/target/version 是**非 NUL 结尾**的借用切片 → 只能"先比长度再 memcmp"。
 *   3. 顺序要小心（断言失败会跳过后面的语句）：
 *      · 只比错误码时：**先 http_request_free 再断言**（成功那条会 malloc path）；
 *      · 比字段时不能先 free（free 会把 req 清零）→ free 放最后，失败时会多报一次泄漏（见注释）。
 *
 * 关于 assert_err()：它只是把 Unity 的数字输出（Expected 1 Was 0）换成错误码名字。
 * 想用纯 Unity 宏也行，把 assert_err(...) 换成：
 *     TEST_ASSERT_EQUAL_INT_MESSAGE(cases[i].want, got, cases[i].head);
 *
 * 现在跑起来应该是**红的**（src/http.c 还是 `return HTTP_OK;` 的占位）——这正是 T1.2b 的验收。
 */
#include "unity.h"
#include "http.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/* ---------------------------------- 小助手 ---------------------------------- */

/* 错误码 → 名字：失败信息里显示名字才看得懂。
 * 故意不写 default：以后往 http_err 里加值，-Wswitch 会提醒你同步这里（本仓库零告警）。 */
static const char *err_name(http_err e) {
    switch (e) {
        case HTTP_OK:
            return "HTTP_OK";
        case HTTP_ERR_BAD_REQUEST:
            return "HTTP_ERR_BAD_REQUEST";
        case HTTP_ERR_METHOD:
            return "HTTP_ERR_METHOD";
        case HTTP_ERR_URI_TOO_LONG:
            return "HTTP_ERR_URI_TOO_LONG";
        case HTTP_ERR_VERSION:
            return "HTTP_ERR_VERSION";
        case HTTP_ERR_HEADER_TOO_LARGE:
            return "HTTP_ERR_HEADER_TOO_LARGE";
        case HTTP_ERR_TOO_MANY_HEADERS:
            return "HTTP_ERR_TOO_MANY_HEADERS";
        case HTTP_ERR_BODY_TOO_LARGE:
            return "HTTP_ERR_BODY_TOO_LARGE";
        case HTTP_ERR_INTERNAL:
            return "HTTP_ERR_INTERNAL";
        case HTTP_ERR_NOT_IMPLEMENTED:
            return "HTTP_ERR_NOT_IMPLEMENTED";
    }
    return "?(未知 http_err)";
}

/* 比错误码；失败时把输入、want、got 一起打出来（表格里这条是哪一行一目了然）。 */
static void assert_err(const char *label, http_err want, http_err got) {
    if (want != got) {
        char msg[200];
        snprintf(msg, sizeof msg, "%s: want %s, got %s", label, err_name(want), err_name(got));
        TEST_FAIL_MESSAGE(msg); /* longjmp 回 Unity —— 只在测试函数里用（本项目单线程） */
    }
}

/* 借用切片 vs 字面量：切片没有 NUL 结尾，所以**先比长度**再 memcmp。 */
static void assert_slice(const char *label, const char *what, const char *got, size_t got_len, const char *want) {
    if (got == NULL || got_len != strlen(want) || memcmp(got, want, got_len) != 0) {
        char msg[200];
        snprintf(
            msg,
            sizeof msg,
            "%s: %s want \"%s\", got \"%.*s\" (len=%zu)",
            label,
            what,
            want,
            (int)got_len,
            got != NULL ? got : "",
            got_len
        );
        TEST_FAIL_MESSAGE(msg);
    }
}

/* ============================ ① 错误码：200 / 400 / 414 / 431 / 505 类 ============================ */
static void test_request_line_errors(void) {
    struct {
        const char *head;
        http_err want;
    } cases[] = {        /* 1.1 缺 Host → 400（RFC 9112 §3.2；CONTRACTS §1 的"报文级"行） */
                 {"GET / HTTP/1.1\r\n\r\n", HTTP_ERR_BAD_REQUEST},
        /* 请求行切不出三段（只有两段）→ 400 */
                 {"GET /\r\nHost: x\r\n\r\n", HTTP_ERR_BAD_REQUEST},
        /* 三段之间连续多个空格 → 400（解析待定项 #4） */
                 {"GET  / HTTP/1.1\r\nHost: x\r\n\r\n", HTTP_ERR_BAD_REQUEST},
        /* 用 TAB 当分隔符 → 400（#5） */
                 {"GET /\tHTTP/1.1\r\nHost: x\r\n\r\n", HTTP_ERR_BAD_REQUEST},
        /* 行尾裸LF -> OK*/
                 {"GET / HTTP/1.1\nHost: x\n\n", HTTP_OK},
        /* head内出现裸CR -> 400 */
                 {"GET / HTTP/1.1\rHost: x\r\r", HTTP_ERR_BAD_REQUEST},
        /* 版本轴补全HTTP/1.0 -> OK*/
                 {"GET / HTTP/1.0\r\nHost: x\r\n\r\n", HTTP_OK},
        /* 缺次版本 -> 400 */
                 {"GET / HTTP/1\r\nHost: x\r\n\r\n", HTTP_ERR_BAD_REQUEST},
        /* 版本为空 -> 400 */
                 {"GET /\r\nHost: x\r\n\r\n", HTTP_ERR_BAD_REQUEST},
        /* 构建超长请求暂时没有思路，先搁置 */
        /* 请求行 = MAX_LINE_LEN / + 1 -> 431*/
        /* target = MAX_PATH_LEN / + 1 -> 414*/
        /* 版本形如 HTTP/x.y 但不受支持 → 505（CONTRACTS §1） */
                 {"GET / HTTP/2.0\r\nHost: x\r\n\r\n", HTTP_ERR_VERSION}
    };

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        http_request req;
        http_err got;

        memset(&req, 0, sizeof req); /* 实现没写完时未填字段是垃圾；清零让失败确定，而不是读野指针 */
        got = http_request_parse(cases[i].head, strlen(cases[i].head), &req);
        http_request_free(&req); /* ★ 先释放：断言失败会 longjmp 跳过后面，漏掉这条 malloc 的 path */
        assert_err(cases[i].head, cases[i].want, got);
    }
}

/* ============================ ② 错误码：405 / 501 类 ============================ */
static void test_request_line_methods(void) {
    // clang-format off
    struct { const char *head; http_err want; } cases[] = {
        /* 已知方法但不在白名单 {GET, HEAD} → HTTP_ERR_METHOD，
         * 由 main.c 映射成 405 + "Allow: GET, HEAD"（CONTRACTS §0/§3、ADR-002） */
        { "PUT / HTTP/1.1\r\nHost: x\r\n\r\n",     HTTP_ERR_METHOD },
        { "DELETE / HTTP/1.1\r\nHost: x\r\n\r\n",  HTTP_ERR_METHOD },
        /* 小写 get：方法名大小写敏感 → 不在"已知方法清单"里 → 501（#2、RFC 9110 §9.1/§15.6.2） */
        { "get / HTTP/1.1\r\nHost: x\r\n\r\n",     HTTP_ERR_NOT_IMPLEMENTED },
        /* 已知清单之外的合法 token → 501 */
        { "FOOBAR / HTTP/1.1\r\nHost: x\r\n\r\n",  HTTP_ERR_NOT_IMPLEMENTED },
        /* method 超长（> MAX_METHOD_LEN = 16）→ 501（#3） */
        { "VERYLONGMETHODNAME123 / HTTP/1.1\r\nHost: x\r\n\r\n", HTTP_ERR_NOT_IMPLEMENTED },
    };
    // clang-format on

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        http_request req;
        http_err got;

        memset(&req, 0, sizeof req);
        got = http_request_parse(cases[i].head, strlen(cases[i].head), &req);
        http_request_free(&req); /* ★ 同 ①：先释放再断言 */
        assert_err(cases[i].head, cases[i].want, got);
    }
}

/* ============================ ③ 字段解析（借用切片） ============================ */
static void test_request_line_fields(void) {
    // clang-format off
    struct { const char *name, *head, *method, *target, *version; int head_only; } cases[] = {
        { "GET ok",     "GET / HTTP/1.1\r\nHost: x\r\n\r\n",
          "GET",  "/",            "HTTP/1.1", 0 },
        /* HEAD 要顺带断言 head_only（main.c 靠它决定不发 body） */
        { "HEAD",       "HEAD /index.html HTTP/1.1\r\nHost: x\r\n\r\n",
          "HEAD", "/index.html",  "HTTP/1.1", 1 },
        /* target 保留原始形态（含 query）—— path 的解码/去 query 规则还没定，
         * 所以这里先不断言 path（docs/rfc/解析待定项.md §3 定了再补） */
        { "with query", "GET /a/b?x=1&y=2 HTTP/1.1\r\nHost: x\r\n\r\n",
          "GET",  "/a/b?x=1&y=2", "HTTP/1.1", 0 },
    };
    // clang-format on

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        http_request req;

        memset(&req, 0, sizeof req);
        assert_err(cases[i].head, HTTP_OK, http_request_parse(cases[i].head, strlen(cases[i].head), &req));

        assert_slice(cases[i].name, "method", req.method, req.method_len, cases[i].method);
        assert_slice(cases[i].name, "target", req.target, req.target_len, cases[i].target);
        assert_slice(cases[i].name, "version", req.version, req.version_len, cases[i].version);
        TEST_ASSERT_EQUAL_INT_MESSAGE(cases[i].head_only, req.head_only, cases[i].name);

        /* 这里 free 只能在断言之后（free 会把 req 清零，字段断言就读不到了）。
         * 代价：断言失败 longjmp 会跳过这一行 → 失败时 make test-asan 会**额外**报一条泄漏。
         * 想彻底避免：先把比较结果和消息攒进局部变量，释放之后再统一断言一次。 */
        http_request_free(&req);
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_request_line_errors);
    RUN_TEST(test_request_line_methods);
    RUN_TEST(test_request_line_fields);
    return UNITY_END();
}

/* TODO(T1.2b) 还没覆盖的轴 —— 每条一行，期望值都要写出处：
 * [ ] 行尾轴：裸 LF 也 OK（扫描器已定）；head 内出现裸 CR → 400（解析待定项 #10）
 * [ ] 版本轴补全：HTTP/1.0 → OK；HTTP/1（缺次版本）→ 400；版本尾随空格 / 版本为空 → 400（#6 #7 #8）
 * [ ] 长度轴：请求行 = MAX_LINE_LEN / +1 → 431；target = MAX_PATH_LEN / +1 → 414（CONTRACTS §1 + src/httpd.h）
 * [ ] path 断言：等 §3（query 截断、%XX 解码）定了再补
 * [ ] 字段轴不在这里：大小写混杂、重复同名、Content-Length 非法/重复… 归 test_headers.c
 */
