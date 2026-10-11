/* tests/unit/test_smoke.c — 测试链自检（T1.0c）
 *
 * 它**不测业务逻辑**：只证明「根 Makefile + vendored Unity + 空套件防护」这条链是通的，
 * 顺便给后面的 test_*.c 一个可照抄的文件骨架（表驱动写法见 warmups/echo/tests/unit/test_parse.c）。
 * M1 第一个真正的用例落地后（T1.2b），这个文件可以直接删。
 *
 * 用例名一律 ASCII：Unity 的 TEST_ASSERT_*_MESSAGE 会把非 ASCII 转义成 \xNN，中文会变乱码。
 */
#include "unity.h"

#include <stddef.h>

/* Unity 要求这两个符号存在（用不上就留空） */
void setUp(void) {}
void tearDown(void) {}

/* 表驱动：同一类逻辑、多组输入输出 —— 数组 + 循环，别写一堆几乎一样的函数 */
static void test_harness_is_wired(void)
{
    static const int cases[][2] = {
        {0, 0}, {1, 1}, {-1, -1}, {65535, 65535},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        TEST_ASSERT_EQUAL_INT(cases[i][1], cases[i][0]);
    }
}

static void test_assert_message_is_usable(void)
{
    /* 断言带说明文字时也应当是纯 ASCII 输出（中文写注释，别写进 message） */
    TEST_ASSERT_EQUAL_INT_MESSAGE(200, 200, "GET / should be 200");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_harness_is_wired);
    RUN_TEST(test_assert_message_is_usable);
    return UNITY_END();
}
