/*
 * T0.5 单元测试 —— 生产者-消费者队列（CSAPP sbuf 风格：sem_t 信号量版）
 *
 * 分工：
 *   - 装配（Unity 钩子 / main / 下面 3 条机械基线用例）是铺好的
 *   - 真正的用例矩阵由你自己设计，维度见文件末尾的 TODO
 *
 * 注意：信号量版没有 close()/broadcast，停机只能靠毒丸值（POISON）。
 */
#include "unity.h"
#include "pc.h"

#include <pthread.h>

#define POISON (-1)

void setUp(void) {}
void tearDown(void) {}

/* ---------- 机械基线：证明装配与断言可用 ---------- */

static void test_init_deinit_smoke(void) {
    pc_t q;
    pc_t_init(&q, 1);                   /* cap=1 是边界 */
    pc_t_insert(&q, 42);
    TEST_ASSERT_EQUAL_INT(42, pc_t_remove(&q));
    pc_t_deinit(&q);
}

static void test_single_thread_fifo(void) {
    pc_t q;
    pc_t_init(&q, 4);
    pc_t_insert(&q, 11);
    pc_t_insert(&q, 22);
    pc_t_insert(&q, 33);
    TEST_ASSERT_EQUAL_INT(11, pc_t_remove(&q));
    TEST_ASSERT_EQUAL_INT(22, pc_t_remove(&q));
    TEST_ASSERT_EQUAL_INT(33, pc_t_remove(&q));
    pc_t_deinit(&q);
}

static void test_wraparound(void) {
    pc_t q;
    pc_t_init(&q, 2);                   /* 环形索引必须回绕 */
    for (int i = 1; i <= 7; i++) {
        pc_t_insert(&q, i);
        TEST_ASSERT_EQUAL_INT(i, pc_t_remove(&q));
    }
    pc_t_deinit(&q);
}

/* ---------- 并发冒烟：证明 TSan 通路是通的（矩阵留给你） ---------- */

#define N_ITEMS 20000

static pc_t g_q;

static void* one_producer(void* arg) {
    (void)arg;
    for (int i = 0; i < N_ITEMS; i++) {
        pc_t_insert(&g_q, 1);
    }
    return NULL;
}

static void* one_consumer(void* arg) {
    long* sum = (long*)arg;
    int   v;
    while ((v = pc_t_remove(&g_q)) != POISON) {
        *sum += (long)v;
    }
    return NULL;
}

static void test_one_producer_one_consumer(void) {
    long      sum = 0;
    pthread_t tp;
    pthread_t tc;

    pc_t_init(&g_q, 64);
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&tc, NULL, one_consumer, &sum));
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&tp, NULL, one_producer, NULL));
    TEST_ASSERT_EQUAL_INT(0, pthread_join(tp, NULL));
    pc_t_insert(&g_q, POISON);          /* 1 个消费者 → 1 个毒丸 */
    TEST_ASSERT_EQUAL_INT(0, pthread_join(tc, NULL));
    TEST_ASSERT_EQUAL_INT(N_ITEMS, (int)sum);
    pc_t_deinit(&g_q);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_init_deinit_smoke);
    RUN_TEST(test_single_thread_fifo);
    RUN_TEST(test_wraparound);
    RUN_TEST(test_one_producer_one_consumer);
    return UNITY_END();
}

/* ============================================================================
 * TODO(你)：把用例矩阵补全（每加一条就多一行 RUN_TEST）
 *
 * 并发形状：P/C = 1/1、1/4、4/1、4/4；cap = 1 与 1024
 *
 * 边界与停机：
 *   - cap=1：生产者与消费者被迫交替，交错最狠
 *   - items=0：只有毒丸，消费者必须能退出（不能挂死）
 *   - 毒丸数 == 消费者数（少了有人永久阻塞；多了会被当成数据）
 *   - 队列满 → 生产者阻塞；消费者取走一个后必须被唤醒
 *   - 队列空 → 消费者阻塞；生产者放入一个后必须被唤醒
 *   - 不变量：consumed_count == P*items，consumed_sum == items*P*(P+1)/2
 *   - 越界：pc_t_init(&q, 0) 会除零崩溃 —— 这是 void 签名拒绝不了非法入参的直接后果，
 *     main.c 里用退出码 2 兜住了；若想让队列自己兜，就得给它返回值（见 README 第 4 条）
 *
 * 提醒：任何一条用例挂死 = make test 挂死。Makefile 已给每条测试套了 timeout 20s，
 *       看到「超时」就是死锁，不是断言失败。
 * ========================================================================== */
