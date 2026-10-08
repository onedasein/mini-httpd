#include "parse.h"
#include "unity.h"

#include <string.h>

/* Unity 要求这两个符号存在（用不上就留空） */
void setUp(void) {}
void tearDown(void) {}

/* ---------- 表驱动（参数化测试）----------
   同一类逻辑、多组输入输出 —— 用数组 + 循环，别写 14 个几乎一样的函数。
   用例名一律用 ASCII：Unity 的 *_MESSAGE 会把非 ASCII 转义成 \xNN，中文会变乱码。 */
typedef struct {
    const char* name;
    const char* host;
    const char* port;
    int         want_rc;
    int         want_port;
} ParseCase;

void test_parse_port_table(void) {
    static const ParseCase cases[] = {
        {"plain port",     "127.0.0.1", "8800",   0, 8800},
        {"lower bound",    "127.0.0.1", "1",      0, 1},
        {"upper bound",    "127.0.0.1", "65535",  0, 65535},
        {"leading plus",   "127.0.0.1", "+80",    0, 80},
        {"leading zeros",  "127.0.0.1", "0080",   0, 80},
        {"leading space",  "127.0.0.1", " 80",    0, 80},   /* strtol 会跳过前导空白 */
        {"zero",           "127.0.0.1", "0",     -1, 0},
        {"too big",        "127.0.0.1", "65536", -1, 0},
        {"negative",       "127.0.0.1", "-1",    -1, 0},
        {"not a number",   "127.0.0.1", "abc",   -1, 0},
        {"trailing junk",  "127.0.0.1", "80abc", -1, 0},
        {"trailing space", "127.0.0.1", "80 ",   -1, 0},
        {"empty port",     "127.0.0.1", "",      -1, 0},
        {"huge number",    "127.0.0.1", "99999999999999999999", -1, 0},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const ParseCase* tc = &cases[i];
        struct config c;
        memset(&c, 0, sizeof(c));

        char* argv[] = {(char*)"server", (char*)tc->host, (char*)tc->port, NULL};

        int rc = parse_args(3, argv, &c);

        TEST_ASSERT_EQUAL_INT_MESSAGE(tc->want_rc, rc, tc->name);
        if (rc == 0) {
            TEST_ASSERT_EQUAL_INT_MESSAGE(tc->want_port, c.port, tc->name);
            TEST_ASSERT_EQUAL_STRING_MESSAGE(tc->host, c.host, tc->name);
        }
    }
}

/* ---------- 一个行为一个函数（名字即文档）---------- */
void test_parse_rejects_wrong_argc(void) {
    struct config c;
    char* one[]  = {(char*)"server", NULL};
    char* two[]  = {(char*)"server", (char*)"127.0.0.1", NULL};
    char* four[] = {(char*)"server", (char*)"127.0.0.1", (char*)"80", (char*)"x", NULL};

    TEST_ASSERT_EQUAL_INT(-1, parse_args(1, one, &c));
    TEST_ASSERT_EQUAL_INT(-1, parse_args(2, two, &c));
    TEST_ASSERT_EQUAL_INT(-1, parse_args(4, four, &c));
}

void test_parse_rejects_null_out(void) {
    char* argv[] = {(char*)"server", (char*)"127.0.0.1", (char*)"80", NULL};
    TEST_ASSERT_EQUAL_INT(-1, parse_args(3, argv, NULL));
}

/* host 长度的两个边界：HOST_MAX-1 放得下，HOST_MAX 放不下（要给 '\0' 留位置） */
void test_parse_accepts_max_host(void) {
    struct config c;
    char host[HOST_MAX];
    memset(host, 'a', HOST_MAX - 1);
    host[HOST_MAX - 1] = '\0';

    char* argv[] = {(char*)"server", host, (char*)"80", NULL};
    TEST_ASSERT_EQUAL_INT(0, parse_args(3, argv, &c));
    TEST_ASSERT_EQUAL_STRING(host, c.host);
}

void test_parse_rejects_overlong_host(void) {
    struct config c;
    char host[HOST_MAX + 1];
    memset(host, 'a', HOST_MAX);
    host[HOST_MAX] = '\0';

    char* argv[] = {(char*)"server", host, (char*)"80", NULL};
    TEST_ASSERT_EQUAL_INT(-1, parse_args(3, argv, &c));
}

/* 一个测试文件一个 main：编成独立可执行，跑完自己返回 0/1 */
int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_parse_port_table);
    RUN_TEST(test_parse_rejects_wrong_argc);
    RUN_TEST(test_parse_rejects_null_out);
    RUN_TEST(test_parse_accepts_max_host);
    RUN_TEST(test_parse_rejects_overlong_host);
    return UNITY_END();
}
