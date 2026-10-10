/*
 * pc 热身（T0.5）— 生产者-消费者演示 / e2e 的被测对象
 *
 * 用法: ./pc [producers] [consumers] [items_each] [cap]     默认 4 4 20000 1024
 *
 * 语义:
 *   - 生产者 p（0-based）把值 (p+1) 压入 items_each 次
 *     → 所以总量 count = P*items，总和 sum = items*P*(P+1)/2（两个不变量都可算出来）
 *   - 消费者循环取出并累加，直到读到毒丸 POISON(-1)
 *   - 所有生产者 join 之后，主线程压入 consumers 个毒丸
 *     （信号量版没有 close()/broadcast：阻塞在 sem_wait(&items) 的人叫不醒，
 *      停机只能靠毒丸，而且毒丸数必须 == 消费者数）
 *   - 最后校验上面两个不变量：TSan 只保证「没有竞争访问」，
 *     保证不了「元素没丢/没重复」，这条才是真验收
 *
 * 退出码: 0 = 不变量成立 | 1 = 不变量被破坏 | 2 = 参数非法 | 3 = 系统调用失败
 */
#include "pc.h"

#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#define POISON (-1)

struct prod_arg {
    pc_t* q;
    int   id;       /* 0-based，压入的值是 id + 1 */
    long  items;
};

struct cons_arg {
    pc_t*     q;
    long long count;    /* 只被本线程写，join 之后才有人读 → 不用加锁 */
    long long sum;
};

static void* producer(void* arg) {
    struct prod_arg* a = (struct prod_arg*)arg;
    for (long i = 0; i < a->items; i++) {
        pc_t_insert(a->q, a->id + 1);
    }
    return NULL;
}

static void* consumer(void* arg) {
    struct cons_arg* a = (struct cons_arg*)arg;
    int               v;
    while ((v = pc_t_remove(a->q)) != POISON) {
        a->count++;
        a->sum += (long long)v;
    }
    return NULL;
}

/* 解析一个 >= lo 的十进制整数；失败返回 -1（负数/非法字符/越界都算失败） */
static long parse_long(const char* s, long lo) {
    char* end = NULL;
    long  v   = strtol(s, &end, 10);
    if (end == s || *end != '\0' || v < lo || v == LONG_MAX) {
        return -1;
    }
    return v;
}

int main(int argc, char** argv) {
    long P     = 4;
    long C     = 4;
    long items = 20000;
    long cap   = 1024;

    if (argc > 5) {
        fprintf(stderr, "用法: %s [producers] [consumers] [items_each] [cap]\n", argv[0]);
        return 2;
    }
    if (argc > 1) { P     = parse_long(argv[1], 1); }
    if (argc > 2) { C     = parse_long(argv[2], 1); }
    if (argc > 3) { items = parse_long(argv[3], 0); }
    if (argc > 4) { cap   = parse_long(argv[4], 1); }
    /* cap >= 1 是硬要求：pc_t_init 是 void 没法拒绝，n=0 时 pc.c 的 % q->n 会直接除零 */
    if (P < 1 || C < 1 || items < 0 || cap < 1 || cap > (long)INT_MAX) {
        fprintf(stderr,
                "参数非法（producers>=1, consumers>=1, items_each>=0, 1<=cap<=%d）\n",
                INT_MAX);
        return 2;
    }

    struct prod_arg* ps  = (struct prod_arg*)malloc((size_t)P * sizeof(*ps));
    struct cons_arg* cs  = (struct cons_arg*)malloc((size_t)C * sizeof(*cs));
    pthread_t*       tps = (pthread_t*)malloc((size_t)P * sizeof(*tps));
    pthread_t*       tcs = (pthread_t*)malloc((size_t)C * sizeof(*tcs));
    if (ps == NULL || cs == NULL || tps == NULL || tcs == NULL) {
        fprintf(stderr, "malloc 失败\n");
        free(ps); free(cs); free(tps); free(tcs);
        return 3;
    }

    pc_t q;
    pc_t_init(&q, (int)cap);

    for (long i = 0; i < P; i++) {
        ps[i].q     = &q;
        ps[i].id    = (int)i;
        ps[i].items = items;
        if (pthread_create(&tps[i], NULL, producer, &ps[i]) != 0) {
            fprintf(stderr, "pthread_create(producer %ld) 失败\n", i);
            return 3;
        }
    }
    for (long i = 0; i < C; i++) {
        cs[i].q     = &q;
        cs[i].count = 0;
        cs[i].sum   = 0;
        if (pthread_create(&tcs[i], NULL, consumer, &cs[i]) != 0) {
            fprintf(stderr, "pthread_create(consumer %ld) 失败\n", i);
            return 3;
        }
    }

    for (long i = 0; i < P; i++) {
        pthread_join(tps[i], NULL);
    }
    for (long i = 0; i < C; i++) {          /* 停机：毒丸数 == 消费者数 */
        pc_t_insert(&q, POISON);
    }
    for (long i = 0; i < C; i++) {
        pthread_join(tcs[i], NULL);
    }

    long long got_count = 0;
    long long got_sum   = 0;
    for (long i = 0; i < C; i++) {
        got_count += cs[i].count;
        got_sum   += cs[i].sum;
    }
    const long long want_count = (long long)P * items;
    const long long want_sum   = (long long)items * P * (P + 1) / 2;

    pc_t_deinit(&q);
    free(ps); free(cs); free(tps); free(tcs);

    printf("producers=%ld consumers=%ld items_each=%ld cap=%ld\n", P, C, items, cap);
    printf("consumed_count=%lld consumed_sum=%lld\n", got_count, got_sum);
    printf("expected_count=%lld expected_sum=%lld\n", want_count, want_sum);

    if (got_count == want_count && got_sum == want_sum) {
        printf("OK\n");
        return 0;
    }
    printf("FAIL 不变量被破坏\n");
    return 1;
}
