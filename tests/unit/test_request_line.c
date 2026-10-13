/* tests/unit/test_request_line.c — 请求行语义的用例表
 *
 * TODO(T1.2b)：用例表先写在 docs/ 或 DESIGN 里，再落到这里。
 * 顺序：契约（CONTRACTS.md §1 + docs/rfc/解析待定项.md 决策完）→ 用例表 → 本文件 → 实现。
 * 表驱动骨架与切片比较 helper 见 DESIGN.md §5.2；每轮记得 http_request_free(&req)。
 */
#include "unity.h"
#include <stddef.h>

void setUp(void) {}
void tearDown(void) {}

static void test_request_line(void) {
    /* TODO(T1.2b)：这里换成表驱动断言，别留空壳 —— 零断言的用例不算测试 */
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_request_line);
    return UNITY_END();
}
