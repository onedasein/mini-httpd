/* tests/unit/test_headers.c — 字段行（header）用例表
 *
 * 结构和 test_request_line.c 一样：**按类别一个测试函数、每个里面一个简单循环**
 *（Unity 的断言失败是 longjmp，会跳出整个测试函数，所以一个大循环只能报第一条失败）。
 *
 *   test_headers_case            大小写混杂（hOsT / cOnTeNt-LeNgTh）+ Host 存在性
 *   test_headers_repeat          重复同名 header（允许，两条都在、顺序保持）
 *   test_headers_cl              Content-Length：缺失/0/空白/非法/溢出/重复/上下界
 *   test_headers_limit           字段数上限、单行上限
 *   test_headers_empty           空值（合法）vs 空名（400）
 *   test_headers_empty_pre_colon 冒号前空白 / 名字里有 TAB → 400
 *   test_headers_onlycr          字段行里出现裸 CR → 400
 *
 * 每条期望值的出处：
 *   · 状态码与语义 → CONTRACTS.md §1（400/413/431）、§2（函数契约）
 *   · 语法决定     → docs/rfc/解析待定项.md #4 #5 #10 #11 #12、docs/adr/002
 *   · 上限数字     → src/httpd.h（唯一真源；本文件用宏，不写死数字）
 *
 * 三条纪律（同 test_request_line.c）：
 *   1. 表驱动一行一条；循环用 sizeof 算条数。
 *   2. name/value 是**非 NUL 结尾**的借用切片 → 只能"先比长度再 memcmp"（assert_slice）。
 *   3. 顺序：断言失败会跳过后面的语句 ——
 *      · 比标量（错误码 / content_length）时：**先把值取出来、先 free，再断言**；
 *      · 要比 req.headers[i] 的切片时不能先 free（free 会把 req 清零）→ free 放最后。
 *
 * 注：这里 include 的是 "http.h" —— **不要** include "http.c"：
 *     Makefile 会把 tests/unit/test_*.c 和 src/ 下的 .c 一起链，重复定义会报 multiple definition。
 */
#include "unity.h"
#include "http.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/* ---------------------- 小助手（与 test_request_line.c 里相同） ---------------------- */

/* 错误码 → 名字：失败信息里显示名字才看得懂。
 * 故意不写 default：以后往 http_err 里加值，-Wswitch 会提醒你同步这里。 */
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

static void assert_err(const char *label, http_err want, http_err got) {
    if (want != got) {
        char msg[200];
        snprintf(msg, sizeof msg, "%s: want %s, got %s", label, err_name(want), err_name(got));
        TEST_FAIL_MESSAGE(msg);
    }
}

/* 借用切片 vs 字面量：先比长度再 memcmp（切片没有 NUL 结尾）。 */
static void assert_slice(const char *label, const char *what, const char *got, size_t got_len, const char *want) {
    if (got == NULL || got_len != strlen(want) || memcmp(got, want, got_len) != 0) {
        char msg[200];
        snprintf(msg, sizeof msg, "%s: %s want \"%s\", got \"%.*s\" (len=%zu)", label, what, want, (int)got_len,
                 got != NULL ? got : "", got_len);
        TEST_FAIL_MESSAGE(msg);
    }
}

/* ==================== 大小写混杂 + Host 存在性 ==================== */
static void test_headers_case(void) {
    // clang-format off
    struct { const char *head; http_err want; } cases[] = {
        { "GET / HTTP/1.1\r\nhOsT: x\r\n\r\n",             HTTP_OK },              /* 名字大小写不敏感 → 满足 Host 要求 */
        { "GET / HTTP/1.1\r\nHOST: x\r\n\r\n",             HTTP_OK },
        { "GET / HTTP/1.1\r\nX-A: 1\r\nHost: x\r\n\r\n",   HTTP_OK },              /* Host 不在第一条也算 */
        { "GET / HTTP/1.1\r\nX-A: 1\r\n\r\n",              HTTP_ERR_BAD_REQUEST }, /* 没有 Host → 400（RFC 9112 §3.2） */
        { "GET / HTTP/1.0\r\nX-A: 1\r\n\r\n",              HTTP_OK },              /* 1.0 不要求 Host */
        { "GET / HTTP/1.1\r\ncOnTeNt-LeNgTh: 5\r\nHost: x\r\n\r\n", HTTP_OK },     /* 名字大小写不敏感 → 认出 CL */
    };
    // clang-format on

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        http_request req;
        http_err got;

        memset(&req, 0, sizeof req);
        got = http_request_parse(cases[i].head, strlen(cases[i].head), &req);
        http_request_free(&req); /* ★ 先释放再断言 */
        assert_err(cases[i].head, cases[i].want, got);
    }

    /* 大小写：名字原样存下来（"hOsT" 就是 "hOsT"），但匹配时不敏感 */
    {
        static const char *head = "GET / HTTP/1.1\r\nX-A: 1\r\nhOsT: y\r\n\r\n";
        http_request req;
        http_err got;

        memset(&req, 0, sizeof req);
        got = http_request_parse(head, strlen(head), &req);
        assert_err("hOsT 原样存", HTTP_OK, got);
        TEST_ASSERT_EQUAL_INT_MESSAGE(2, (int)req.nheaders, "hOsT 原样存");
        assert_slice("hOsT 原样存", "name", req.headers[1].name, req.headers[1].name_len, "hOsT");
        assert_slice("hOsT 原样存", "value", req.headers[1].value, req.headers[1].value_len, "y");
        http_request_free(&req);
    }
}

/* ==================== 重复同名 header（允许） ==================== */
static void test_headers_repeat(void) {
    // clang-format off
    struct { const char *label, *head, *want_name, *want_value; int idx; int want_n; } cases[] = {
        { "重复同名 第1条", "GET / HTTP/1.1\r\nHost: x\r\nX-Dup: 1\r\nX-Dup: 2\r\n\r\n", "X-Dup", "1", 1, 3 },
        { "重复同名 第2条", "GET / HTTP/1.1\r\nHost: x\r\nX-Dup: 1\r\nX-Dup: 2\r\n\r\n", "X-Dup", "2", 2, 3 },
        /* 重复 Host 也要照存（RFC 9112 §3.2 要求"多个 Host → 400"，但那条还没进决策表，
         * 现在实现会放过 —— 见文件末尾 TODO；这里只断言"存下来了 + 计数对"） */
        { "重复 Host 照存", "GET / HTTP/1.1\r\nHost: x\r\nHost: y\r\n\r\n",             "Host",  "y", 1, 2 },
    };
    // clang-format on

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        http_request req;
        http_err got;

        memset(&req, 0, sizeof req);
        got = http_request_parse(cases[i].head, strlen(cases[i].head), &req);
        assert_err(cases[i].label, HTTP_OK, got);

        /* 先断言条数：漏存/多存时这一步就 longjmp，后面的 headers[idx] 不会被越界读 */
        TEST_ASSERT_EQUAL_INT_MESSAGE(cases[i].want_n, (int)req.nheaders, cases[i].label);
        assert_slice(cases[i].label, "name", req.headers[cases[i].idx].name, req.headers[cases[i].idx].name_len,
                     cases[i].want_name);
        assert_slice(cases[i].label, "value", req.headers[cases[i].idx].value, req.headers[cases[i].idx].value_len,
                     cases[i].want_value);

        http_request_free(&req); /* 比切片，只能最后 free */
    }
}

/* ==================== Content-Length：语义 ==================== */
static void test_headers_cl(void) {
    // clang-format off
    struct { const char *label, *head; http_err want_err; size_t want_cl; } cases[] = {
        /* #11：没有该 header 与 Content-Length: 0 不区分，都记 0 */
        { "没有 CL",          "GET / HTTP/1.1\r\nHost: x\r\n\r\n",                                            HTTP_OK,               0 },
        { "CL: 0",            "GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n",                        HTTP_OK,               0 },
        { "CL: 5",            "GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\n",                        HTTP_OK,               5 },
        { "值两端空白",       "GET / HTTP/1.1\r\nHost: x\r\nContent-Length:   7  \r\n\r\n",                    HTTP_OK,               7 },
        { "重复且同值",       "GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nContent-Length: 5\r\n\r\n",   HTTP_OK,               5 },
        /* ---- 非法（RFC 9112 §6.3；解析待定项 #12）---- */
        { "重复且异值",       "GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\n",   HTTP_ERR_BAD_REQUEST,  0 },
        { "空值",             "GET / HTTP/1.1\r\nHost: x\r\nContent-Length:\r\n\r\n",                          HTTP_ERR_BAD_REQUEST,  0 },
        { "带非数字",         "GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 12abc\r\n\r\n",                    HTTP_ERR_BAD_REQUEST,  0 },
        { "负数",             "GET / HTTP/1.1\r\nHost: x\r\nContent-Length: -1\r\n\r\n",                       HTTP_ERR_BAD_REQUEST,  0 },
        { "溢出",             "GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 99999999999999999999\r\n\r\n",     HTTP_ERR_BAD_REQUEST,  0 },
    };
    // clang-format on

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        http_request req;
        http_err got;
        size_t cl;

        memset(&req, 0, sizeof req);
        got = http_request_parse(cases[i].head, strlen(cases[i].head), &req);
        cl = req.content_length; /* ★ 先取值：下面的 free 会把 req 清零 */
        http_request_free(&req);

        assert_err(cases[i].label, cases[i].want_err, got);
        if (cases[i].want_err == HTTP_OK) {
            TEST_ASSERT_EQUAL_INT_MESSAGE((int)cases[i].want_cl, (int)cl, cases[i].label);
        }
    }
}

/* ==================== 上限：字段数 / 单行 / CL 上下界 ==================== */
static void test_headers_limit(void) {
    char buf[16 * 1024]; /* 最大的一例：单行 MAX_LINE_LEN 个 'a' */
    size_t n;
    http_request req;
    http_err got;

    /* 恰好 MAX_HEADERS 个字段 → OK（边界是闭的），顺手验 nheaders 计数 */
    n = (size_t)sprintf(buf, "GET / HTTP/1.1\r\nHost: x\r\n");
    for (int i = 0; (size_t)i + 1 < (size_t)MAX_HEADERS; i++) n += (size_t)sprintf(buf + n, "X-N: v\r\n");
    sprintf(buf + n, "\r\n");
    got = http_request_parse(buf, strlen(buf), &req);
    assert_err("字段数 = MAX_HEADERS", HTTP_OK, got);
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)MAX_HEADERS, (int)req.nheaders, "nheaders = MAX_HEADERS");
    http_request_free(&req);

    /* 多一个字段 → 431/TOO_MANY_HEADERS */
    n = (size_t)sprintf(buf, "GET / HTTP/1.1\r\nHost: x\r\n");
    for (int i = 0; (size_t)i < (size_t)MAX_HEADERS; i++) n += (size_t)sprintf(buf + n, "X-N: v\r\n");
    sprintf(buf + n, "\r\n");
    got = http_request_parse(buf, strlen(buf), &req);
    http_request_free(&req);
    assert_err("字段数 = MAX_HEADERS+1", HTTP_ERR_TOO_MANY_HEADERS, got);

    /* 单个字段行 > MAX_LINE_LEN → 431/HEADER_TOO_LARGE
     * （注意不是 TOO_MANY：整个 head 还没到 MAX_HEADER_LEN，先撞上的是单行上限） */
    n = (size_t)sprintf(buf, "GET / HTTP/1.1\r\nHost: x\r\nX-Big: ");
    for (int i = 0; i < MAX_LINE_LEN; i++) buf[n++] = 'a';
    sprintf(buf + n, "\r\n\r\n");
    got = http_request_parse(buf, strlen(buf), &req);
    http_request_free(&req);
    assert_err("单行 > MAX_LINE_LEN", HTTP_ERR_HEADER_TOO_LARGE, got);

    /* CL 的上下界：数字用宏拼，别写死（数字真源是 src/httpd.h） */
    sprintf(buf, "GET / HTTP/1.1\r\nHost: x\r\nContent-Length: %d\r\n\r\n", (int)MAX_BODY);
    got = http_request_parse(buf, strlen(buf), &req);
    assert_err("CL = MAX_BODY", HTTP_OK, got);
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)MAX_BODY, (int)req.content_length, "cl = MAX_BODY");
    http_request_free(&req);

    sprintf(buf, "GET / HTTP/1.1\r\nHost: x\r\nContent-Length: %d\r\n\r\n", (int)MAX_BODY + 1);
    got = http_request_parse(buf, strlen(buf), &req);
    http_request_free(&req);
    assert_err("CL = MAX_BODY+1", HTTP_ERR_BODY_TOO_LARGE, got);
}

/* ==================== 空值（合法）vs 空名（400） ==================== */
static void test_headers_empty(void) {
    // clang-format off
    struct { const char *label, *head, *want_name, *want_value; http_err want; int idx; int want_n; } cases[] = {
        /* "X-Empty:" 是合法的空值：name 有，value 长度 0 */
        { "空值合法", "GET / HTTP/1.1\r\nHost: x\r\nX-Empty:\r\n\r\n", "X-Empty", "", HTTP_OK, 1, 2 },
        /* 空名非法：冒号在行首 */
        { "空名非法", "GET / HTTP/1.1\r\n:x\r\nHost: x\r\n\r\n",       NULL,      "", HTTP_ERR_BAD_REQUEST, 0, 0 },
    };
    // clang-format on

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        http_request req;
        http_err got;

        memset(&req, 0, sizeof req);
        got = http_request_parse(cases[i].head, strlen(cases[i].head), &req);
        assert_err(cases[i].label, cases[i].want, got);

        if (cases[i].want == HTTP_OK) {
            TEST_ASSERT_EQUAL_INT_MESSAGE(cases[i].want_n, (int)req.nheaders, cases[i].label);
            assert_slice(cases[i].label, "name", req.headers[cases[i].idx].name, req.headers[cases[i].idx].name_len,
                         cases[i].want_name);
            assert_slice(cases[i].label, "value", req.headers[cases[i].idx].value, req.headers[cases[i].idx].value_len,
                         cases[i].want_value);
        }
        http_request_free(&req);
    }
}

/* ==================== 冒号前空白 / 名字里有 TAB → 400 ==================== */
static void test_headers_empty_pre_colon(void) {
    // clang-format off
    struct { const char *head; http_err want; } cases[] = {
        { "GET / HTTP/1.1\r\nHost : x\r\n\r\n",         HTTP_ERR_BAD_REQUEST }, /* 冒号前有空格 */
        { "GET / HTTP/1.1\r\nHost\t: x\r\n\r\n",        HTTP_ERR_BAD_REQUEST }, /* 冒号前有 TAB */
        { "GET / HTTP/1.1\r\nHo st: x\r\n\r\n",         HTTP_ERR_BAD_REQUEST }, /* 名字中间有空格 */
        { "GET / HTTP/1.1\r\nHo\tst: x\r\n\r\n",        HTTP_ERR_BAD_REQUEST }, /* 名字中间有 TAB */
        { "GET / HTTP/1.1\r\n y: z\r\nHost: x\r\n\r\n", HTTP_ERR_BAD_REQUEST }, /* 行首空白（obs-fold 雏形） */
    };
    // clang-format on

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        http_request req;
        http_err got;

        memset(&req, 0, sizeof req);
        got = http_request_parse(cases[i].head, strlen(cases[i].head), &req);
        http_request_free(&req); /* ★ 先释放再断言 */
        assert_err(cases[i].head, cases[i].want, got);
    }
}

/* ==================== 字段行里的裸 CR → 400 ==================== */
static void test_headers_onlycr(void) {
    // clang-format off
    struct { const char *head; http_err want; } cases[] = {
        { "GET / HTTP/1.1\r\nHost: x\rX: y\r\n\r\n",   HTTP_ERR_BAD_REQUEST }, /* 值里夹了裸 CR */
        { "GET / HTTP/1.1\r\nHost: x\r\r\n\r\n",       HTTP_ERR_BAD_REQUEST }, /* 行尾多了一个 CR */
        { "GET / HTTP/1.1\r\nHost:\rx\r\n\r\n",        HTTP_ERR_BAD_REQUEST }, /* 值开头就是裸 CR */
        { "GET / HTTP/1.1\r\nHost: x\r\n\r\n",         HTTP_OK },              /* 对照：正常 CRLF */
        { "GET / HTTP/1.1\nHost: x\n\n",               HTTP_OK },              /* 对照组：裸 LF 也接受 */
    };
    // clang-format on

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        http_request req;
        http_err got;

        memset(&req, 0, sizeof req);
        got = http_request_parse(cases[i].head, strlen(cases[i].head), &req);
        http_request_free(&req);
        assert_err(cases[i].head, cases[i].want, got);
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_headers_case);
    RUN_TEST(test_headers_repeat);
    RUN_TEST(test_headers_cl);
    RUN_TEST(test_headers_limit);
    RUN_TEST(test_headers_empty);
    RUN_TEST(test_headers_empty_pre_colon);
    RUN_TEST(test_headers_onlycr);
    return UNITY_END();
}

/* TODO(§2 定稿后补) —— 下面这些**还没有断言**，因为决策表里还没定：
 * [ ] RFC 9112 §3.2 还要求：**多个 Host 字段 → 400**、**Host 值为空/非法 → 400**
 *     （现在的实现会放过 —— test_headers_repeat 里那条"重复 Host 照存"就是在给这件事留位置）
 * [ ] 字段值里出现 NUL / 控制字符 → 400？（§2）
 * [ ] 字段值里的非 ASCII（UTF-8）直通还是拒绝？（§2）
 * [ ] Connection 的语义（keep-alive / close）—— 字段会被存下来，用到它是 T2.6
 * [ ] 整体上限（head > MAX_HEADER_LEN、URI > MAX_PATH_LEN）→ 归 test_limits.c
 */
