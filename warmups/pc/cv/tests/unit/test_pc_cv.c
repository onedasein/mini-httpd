#define _POSIX_C_SOURCE 200809L
/*
 * T0.5 单元测试 —— 条件变量版生产者-消费者队列
 *
 * 覆盖的维度：
 *   单线程语义   init 拒绝非法 cap / init-destroy / FIFO / 环形回绕
 *   阻塞与唤醒   满队列 insert 阻塞 → 取走一个后被唤醒
 *                空队列 remove 阻塞 → 放入一个后被唤醒
 *   close 协议   close 唤醒所有等待者（基线 5）/ 排空后再失败 /
 *                唤醒阻塞的生产者 / 幂等 / 关闭后 insert 被拒且内容不变
 *   destroy      排空销毁后同一块内存能重新 init 再用
 *   顺序         1P1C 下必须严格 FIFO（sum 对不代表顺序对）
 *   并发形状     P/C = 1/1、1/4、4/1、4/4，cap = 1 与 1024，
 *                每个形状校验 count / sum / 已排空 / 无消费者提前退出
 *   变异验证     while-if（基线 7：假唤醒注入，确定性）
 *                broadcast-signal（基线 5：多个等待者只醒一个 → join 挂死）
 *
 * ⚠ 基线 5/6/7 不要删：make mutation-check 靠它们抓住那两个经典 bug。
 * ⚠ 所有用例都跑在 Makefile 的 `timeout 20` 之下 —— 看到「超时」就是死锁，不是断言失败。
 */
#include "unity.h"
#include "pc.h"

#include <pthread.h>
#include <time.h>

void setUp(void) {}
void tearDown(void) {}

static void sleep_ms(long ms) {
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

/* ---------- 并发用例共用的全局状态 ---------- */
/* 「线程是不是已经退出/是不是提前退出」这类观察量，统一用这把锁保护，避免 TSan 噪音 */
static pc_t            g_q;
static pthread_mutex_t g_flag_lock = PTHREAD_MUTEX_INITIALIZER;
static int             g_closing;      /* 主线程在 close() 之前置 1 */
static int             g_premature;    /* 在 close 之前就返回失败的消费者个数 */

/* ==========================================================================
 * 一、单线程语义
 * ======================================================================== */

static void test_init_rejects_zero_cap(void) {
    pc_t q;
    TEST_ASSERT_EQUAL_INT(-1, pc_t_init(&q, 0));    /* 这一版终于能拒绝非法入参 */
    TEST_ASSERT_EQUAL_INT(-1, pc_t_init(&q, -1));
}

static void test_init_destroy_smoke(void) {
    pc_t q;
    TEST_ASSERT_EQUAL_INT(0, pc_t_init(&q, 1));
    TEST_ASSERT_EQUAL_INT(0, pc_t_insert(&q, 42));
    int v = 0;
    TEST_ASSERT_EQUAL_INT(0, pc_t_remove(&q, &v));
    TEST_ASSERT_EQUAL_INT(42, v);
    TEST_ASSERT_EQUAL_INT(0, pc_t_size(&q));
    pc_t_destroy(&q);
}

static void test_single_thread_fifo(void) {
    pc_t q;
    TEST_ASSERT_EQUAL_INT(0, pc_t_init(&q, 4));
    TEST_ASSERT_EQUAL_INT(0, pc_t_insert(&q, 11));
    TEST_ASSERT_EQUAL_INT(0, pc_t_insert(&q, 22));
    TEST_ASSERT_EQUAL_INT(0, pc_t_insert(&q, 33));
    TEST_ASSERT_EQUAL_INT(3, pc_t_size(&q));
    int v = 0;
    TEST_ASSERT_EQUAL_INT(0, pc_t_remove(&q, &v));
    TEST_ASSERT_EQUAL_INT(11, v);
    TEST_ASSERT_EQUAL_INT(0, pc_t_remove(&q, &v));
    TEST_ASSERT_EQUAL_INT(22, v);
    TEST_ASSERT_EQUAL_INT(0, pc_t_remove(&q, &v));
    TEST_ASSERT_EQUAL_INT(33, v);
    TEST_ASSERT_EQUAL_INT(0, pc_t_size(&q));
    pc_t_destroy(&q);
}

static void test_wraparound(void) {
    pc_t q;
    TEST_ASSERT_EQUAL_INT(0, pc_t_init(&q, 2));     /* 环形索引必须回绕 */
    for (int i = 1; i <= 7; i++) {
        int v = 0;
        TEST_ASSERT_EQUAL_INT(0, pc_t_insert(&q, i));
        TEST_ASSERT_EQUAL_INT(1, pc_t_size(&q));
        TEST_ASSERT_EQUAL_INT(0, pc_t_remove(&q, &v));
        TEST_ASSERT_EQUAL_INT(i, v);
    }
    pc_t_destroy(&q);
}

/* ==========================================================================
 * 二、阻塞与唤醒
 * ======================================================================== */

struct blocker {
    pc_t* q;
    int   item;     /* insert 用 */
    int   out;      /* remove 用 */
    int   rc;
    int   done;
};

static int blocker_done(struct blocker* b) {
    pthread_mutex_lock(&g_flag_lock);
    int d = b->done;
    pthread_mutex_unlock(&g_flag_lock);
    return d;
}

static void blocker_mark_done(struct blocker* b) {
    pthread_mutex_lock(&g_flag_lock);
    b->done = 1;
    pthread_mutex_unlock(&g_flag_lock);
}

static void* insert_blocker(void* arg) {
    struct blocker* b = (struct blocker*)arg;
    b->rc = pc_t_insert(b->q, b->item);
    blocker_mark_done(b);
    return NULL;
}

static void* remove_blocker(void* arg) {
    struct blocker* b = (struct blocker*)arg;
    b->rc = pc_t_remove(b->q, &b->out);
    blocker_mark_done(b);
    return NULL;
}

/* 队列满 → 生产者必须阻塞；消费者取走一个后才被唤醒 */
static void test_insert_blocks_when_full(void) {
    pc_t            q;
    struct blocker  b;
    pthread_t       t;
    int             v = 0;

    TEST_ASSERT_EQUAL_INT(0, pc_t_init(&q, 1));
    TEST_ASSERT_EQUAL_INT(0, pc_t_insert(&q, 1));       /* cap=1 → 已满 */

    b.q = &q; b.item = 2; b.rc = 123; b.done = 0;
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&t, NULL, insert_blocker, &b));
    sleep_ms(50);
    TEST_ASSERT_FALSE(blocker_done(&b));                /* 必须还阻塞着 */
    TEST_ASSERT_EQUAL_INT(1, pc_t_size(&q));

    TEST_ASSERT_EQUAL_INT(0, pc_t_remove(&q, &v));      /* 腾出一个空位 → 唤醒生产者 */
    TEST_ASSERT_EQUAL_INT(1, v);
    TEST_ASSERT_EQUAL_INT(0, pthread_join(t, NULL));
    TEST_ASSERT_EQUAL_INT(0, b.rc);

    TEST_ASSERT_EQUAL_INT(0, pc_t_remove(&q, &v));      /* FIFO：后进去的排后面 */
    TEST_ASSERT_EQUAL_INT(2, v);
    pc_t_destroy(&q);
}

/* 队列空 → 消费者必须阻塞；生产者放入一个后才被唤醒 */
static void test_remove_blocks_when_empty(void) {
    pc_t           q;
    struct blocker b;
    pthread_t      t;

    TEST_ASSERT_EQUAL_INT(0, pc_t_init(&q, 2));

    b.q = &q; b.out = -1; b.rc = 123; b.done = 0;
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&t, NULL, remove_blocker, &b));
    sleep_ms(50);
    TEST_ASSERT_FALSE(blocker_done(&b));                /* 必须还阻塞着 */

    TEST_ASSERT_EQUAL_INT(0, pc_t_insert(&q, 99));
    TEST_ASSERT_EQUAL_INT(0, pthread_join(t, NULL));
    TEST_ASSERT_EQUAL_INT(0, b.rc);
    TEST_ASSERT_EQUAL_INT(99, b.out);
    pc_t_destroy(&q);
}

/* ==========================================================================
 * 三、close 协议
 * ======================================================================== */

/* close 必须唤醒**所有**等待者（PC_BUG_SIGNAL 的确定性抓手）
 * 注意：工作线程里**不能**调 Unity 断言 —— 失败时它会跨线程 longjmp（UB）。
 *       所以结果只记进结构体，回到主线程再断言。 */
enum { WAITERS = 4 };

struct idle_arg {
    pc_t* q;
    int   rc;
};

static void* idle_consumer(void* arg) {
    struct idle_arg* a = (struct idle_arg*)arg;
    int              v = 0;
    a->rc = pc_t_remove(a->q, &v);      /* 空队列 → 阻塞在 not_empty，直到 close */
    return NULL;
}

static void test_close_wakes_all_waiters(void) {
    pc_t             q;
    pthread_t        tc[WAITERS];
    struct idle_arg  a[WAITERS];

    TEST_ASSERT_EQUAL_INT(0, pc_t_init(&q, 1));
    for (int i = 0; i < WAITERS; i++) {
        a[i].q  = &q;
        a[i].rc = 123;
        TEST_ASSERT_EQUAL_INT(0, pthread_create(&tc[i], NULL, idle_consumer, &a[i]));
    }
    sleep_ms(50);                   /* 让 4 个消费者都进入 cond_wait（睡眠换确定性） */
    TEST_ASSERT_EQUAL_INT(0, pc_t_close(&q));
    for (int i = 0; i < WAITERS; i++) {
        TEST_ASSERT_EQUAL_INT(0, pthread_join(tc[i], NULL));   /* 少唤醒一个 → 挂死 → 超时 */
        TEST_ASSERT_EQUAL_INT(-1, a[i].rc);                    /* 被 close 唤醒 → 返回失败 */
    }
    pc_t_destroy(&q);
}

/* close 时队列非空 → 必须先把已有元素排空，之后才返回失败
   （后半段「已关闭且空时立刻返回」也在这里：它若阻塞，整个二进制会撞上 timeout 20s）*/
static void test_close_drains_before_failing(void) {
    pc_t q;
    int  v = 0;

    TEST_ASSERT_EQUAL_INT(0, pc_t_init(&q, 4));
    TEST_ASSERT_EQUAL_INT(0, pc_t_insert(&q, 11));
    TEST_ASSERT_EQUAL_INT(0, pc_t_insert(&q, 22));
    TEST_ASSERT_EQUAL_INT(0, pc_t_insert(&q, 33));
    TEST_ASSERT_EQUAL_INT(0, pc_t_close(&q));

    TEST_ASSERT_EQUAL_INT(0, pc_t_remove(&q, &v));      /* 关掉了也不能丢数据 */
    TEST_ASSERT_EQUAL_INT(11, v);
    TEST_ASSERT_EQUAL_INT(0, pc_t_remove(&q, &v));
    TEST_ASSERT_EQUAL_INT(22, v);
    TEST_ASSERT_EQUAL_INT(0, pc_t_remove(&q, &v));
    TEST_ASSERT_EQUAL_INT(33, v);

    TEST_ASSERT_EQUAL_INT(-1, pc_t_remove(&q, &v));     /* 排空后立刻失败（不能阻塞） */
    TEST_ASSERT_EQUAL_INT(0, pc_t_size(&q));
    pc_t_destroy(&q);
}

/* 阻塞在满队列上的生产者，必须被 close 唤醒 —— 只在消费者侧唤醒的实现会在这儿挂死 */
static void test_close_unblocks_blocked_producer(void) {
    pc_t           q;
    struct blocker b;
    pthread_t      t;

    TEST_ASSERT_EQUAL_INT(0, pc_t_init(&q, 1));
    TEST_ASSERT_EQUAL_INT(0, pc_t_insert(&q, 1));       /* 填满，且没有消费者 */

    b.q = &q; b.item = 2; b.rc = 123; b.done = 0;
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&t, NULL, insert_blocker, &b));
    sleep_ms(50);
    TEST_ASSERT_FALSE(blocker_done(&b));

    TEST_ASSERT_EQUAL_INT(0, pc_t_close(&q));           /* 无消费者可唤醒 → 只能靠 close 唤醒生产者 */
    TEST_ASSERT_EQUAL_INT(0, pthread_join(t, NULL));
    TEST_ASSERT_EQUAL_INT(-1, b.rc);                    /* 已关闭 → 拒绝插入 */
    TEST_ASSERT_EQUAL_INT(1, pc_t_size(&q));            /* 队列内容不变 */
    pc_t_destroy(&q);
}

static void test_close_is_idempotent(void) {
    pc_t q;
    TEST_ASSERT_EQUAL_INT(0, pc_t_init(&q, 2));
    TEST_ASSERT_EQUAL_INT(0, pc_t_close(&q));
    TEST_ASSERT_EQUAL_INT(0, pc_t_close(&q));
    TEST_ASSERT_EQUAL_INT(0, pc_t_close(&q));
    pc_t_destroy(&q);
}

static void test_insert_after_close_fails(void) {
    pc_t q;
    int  v = 0;

    TEST_ASSERT_EQUAL_INT(0, pc_t_init(&q, 2));
    TEST_ASSERT_EQUAL_INT(0, pc_t_insert(&q, 5));
    TEST_ASSERT_EQUAL_INT(0, pc_t_close(&q));

    TEST_ASSERT_EQUAL_INT(-1, pc_t_insert(&q, 6));      /* 关闭后一律拒绝 */
    TEST_ASSERT_EQUAL_INT(1, pc_t_size(&q));            /* 而且不能偷偷塞进去 */
    TEST_ASSERT_EQUAL_INT(0, pc_t_remove(&q, &v));
    TEST_ASSERT_EQUAL_INT(5, v);
    TEST_ASSERT_EQUAL_INT(-1, pc_t_remove(&q, &v));
    pc_t_destroy(&q);
}

/* ==========================================================================
 * 四、destroy 后同一块内存能重新 init 再用（忘重置 closed/count 就会被抓到）
 * ======================================================================== */

static void test_destroy_then_reuse(void) {
    pc_t q;
    int  v = 0;

    TEST_ASSERT_EQUAL_INT(0, pc_t_init(&q, 2));
    TEST_ASSERT_EQUAL_INT(0, pc_t_insert(&q, 1));
    TEST_ASSERT_EQUAL_INT(0, pc_t_remove(&q, &v));
    TEST_ASSERT_EQUAL_INT(0, pc_t_close(&q));
    pc_t_destroy(&q);

    TEST_ASSERT_EQUAL_INT(0, pc_t_init(&q, 2));         /* 复用同一块内存 */
    TEST_ASSERT_EQUAL_INT(0, pc_t_insert(&q, 7));
    TEST_ASSERT_EQUAL_INT(0, pc_t_remove(&q, &v));
    TEST_ASSERT_EQUAL_INT(7, v);
    TEST_ASSERT_EQUAL_INT(0, pc_t_close(&q));
    pc_t_destroy(&q);
}

/* ==========================================================================
 * 五、顺序：1P1C 下必须严格 FIFO（sum 对不代表顺序对）
 * ======================================================================== */

#define FIFO_N 1000

struct fifo_arg {
    pc_t* q;
    int   n;
    int   ok;
    int   got;
};

static void* fifo_producer(void* arg) {
    struct fifo_arg* a = (struct fifo_arg*)arg;
    for (int i = 1; i <= a->n; i++) {
        if (pc_t_insert(a->q, i) != 0) {
            break;
        }
    }
    return NULL;
}

static void* fifo_consumer(void* arg) {
    struct fifo_arg* a = (struct fifo_arg*)arg;
    int v    = 0;
    int prev = 0;
    int ok   = 1;

    while (pc_t_remove(a->q, &v) == 0) {
        if (v != prev + 1) {
            ok = 0;                 /* 乱序 / 重复 / 丢件 */
        }
        prev = v;
        a->got++;
    }
    a->ok = ok;
    return NULL;
}

static void test_fifo_order_1p1c(void) {
    pc_t             q;
    pthread_t        tp;
    pthread_t        tc;
    struct fifo_arg  ap;
    struct fifo_arg  ac;

    TEST_ASSERT_EQUAL_INT(0, pc_t_init(&q, 4));         /* 小 cap 逼出阻塞/唤醒 */

    ap.q = &q; ap.n = FIFO_N; ap.ok = 0; ap.got = 0;
    ac.q = &q; ac.n = FIFO_N; ac.ok = 0; ac.got = 0;

    TEST_ASSERT_EQUAL_INT(0, pthread_create(&tc, NULL, fifo_consumer, &ac));
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&tp, NULL, fifo_producer, &ap));
    TEST_ASSERT_EQUAL_INT(0, pthread_join(tp, NULL));
    TEST_ASSERT_EQUAL_INT(0, pc_t_close(&q));
    TEST_ASSERT_EQUAL_INT(0, pthread_join(tc, NULL));

    TEST_ASSERT_EQUAL_INT(FIFO_N, ac.got);
    TEST_ASSERT_TRUE(ac.ok);
    pc_t_destroy(&q);
}

/* ==========================================================================
 * 六、并发形状矩阵
 * ======================================================================== */

#define SHAPE_MAX_P 4
#define SHAPE_MAX_C 8

struct shape_arg {
    pc_t* q;
    int   id;
    int   items;
};

static struct shape_arg g_pargs[SHAPE_MAX_P];
static struct shape_arg g_cargs[SHAPE_MAX_C];
static long             g_ccount[SHAPE_MAX_C];
static long             g_csum[SHAPE_MAX_C];

static void* shape_producer(void* arg) {
    struct shape_arg* a = (struct shape_arg*)arg;
    for (int i = 0; i < a->items; i++) {
        if (pc_t_insert(a->q, a->id + 1) != 0) {    /* 压 id+1：值不同才能抓"串值" */
            break;
        }
    }
    return NULL;
}

static void* shape_consumer(void* arg) {
    struct shape_arg* a = (struct shape_arg*)arg;
    long s = 0;
    int  v = 0;
    long n = 0;

    while (pc_t_remove(a->q, &v) == 0) {
        s += (long)v;
        n++;
    }
    g_ccount[a->id] = n;        /* 每个消费者只写自己的槽位 → 不用加锁 */

    pthread_mutex_lock(&g_flag_lock);
    if (!g_closing) {
        g_premature++;          /* while 写成 if 的典型后果：线程池悄悄缩水 */
    }
    pthread_mutex_unlock(&g_flag_lock);

    g_csum[a->id] = s;
    return NULL;
}

static void run_shape(int P, int C, int items, int cap) {
    pthread_t tp[SHAPE_MAX_P];
    pthread_t tc[SHAPE_MAX_C];
    long      total_count = 0;
    long      total_sum   = 0;

    TEST_ASSERT_TRUE(P >= 1 && P <= SHAPE_MAX_P);
    TEST_ASSERT_TRUE(C >= 1 && C <= SHAPE_MAX_C);

    pthread_mutex_lock(&g_flag_lock);
    g_closing   = 0;
    g_premature = 0;
    pthread_mutex_unlock(&g_flag_lock);
    for (int i = 0; i < SHAPE_MAX_C; i++) {
        g_ccount[i] = 0;
        g_csum[i]   = 0;
    }

    TEST_ASSERT_EQUAL_INT(0, pc_t_init(&g_q, cap));
    for (int i = 0; i < P; i++) {
        g_pargs[i].q     = &g_q;
        g_pargs[i].id    = i;
        g_pargs[i].items = items;
        TEST_ASSERT_EQUAL_INT(0, pthread_create(&tp[i], NULL, shape_producer, &g_pargs[i]));
    }
    for (int i = 0; i < C; i++) {
        g_cargs[i].q     = &g_q;
        g_cargs[i].id    = i;
        g_cargs[i].items = 0;
        TEST_ASSERT_EQUAL_INT(0, pthread_create(&tc[i], NULL, shape_consumer, &g_cargs[i]));
    }

    for (int i = 0; i < P; i++) {
        TEST_ASSERT_EQUAL_INT(0, pthread_join(tp[i], NULL));
    }
    pthread_mutex_lock(&g_flag_lock);
    g_closing = 1;
    pthread_mutex_unlock(&g_flag_lock);
    TEST_ASSERT_EQUAL_INT(0, pc_t_close(&g_q));
    for (int i = 0; i < C; i++) {
        TEST_ASSERT_EQUAL_INT(0, pthread_join(tc[i], NULL));
    }

    for (int i = 0; i < C; i++) {
        total_count += g_ccount[i];
        total_sum   += g_csum[i];
    }

    TEST_ASSERT_EQUAL_INT64((long long)P * items, (long long)total_count);
    TEST_ASSERT_EQUAL_INT64((long long)items * P * (P + 1) / 2, (long long)total_sum);
    TEST_ASSERT_EQUAL_INT(0, pc_t_size(&g_q));      /* 关闭后必须已排空 */
    TEST_ASSERT_EQUAL_INT(0, g_premature);          /* 没有消费者提前退出 */
    pc_t_destroy(&g_q);
}

static void test_shape_1p1c(void) { run_shape(1, 1, 20000, 1024); }
static void test_shape_1p4c(void) { run_shape(1, 4, 20000, 1024); }
static void test_shape_4p1c(void) { run_shape(4, 1, 20000, 1024); }
static void test_shape_4p4c(void) { run_shape(4, 4, 20000, 1024); }
static void test_shape_4p4c_cap1(void) { run_shape(4, 4, 20000, 1); }
static void test_shape_2p8c_cap1(void) { run_shape(2, 8, 5000, 1); }

/* ==========================================================================
 * 七、并发冒烟：cap=1 下 1P/8C，边跑边盯"提前退出"
 * ======================================================================== */

#define SMOKE_N 50000
#define SMOKE_C 8

static void* smoke_producer(void* arg) {
    (void)arg;
    for (int i = 0; i < SMOKE_N; i++) {
        if (pc_t_insert(&g_q, 1) != 0) {
            break;
        }
    }
    return NULL;
}

static void* smoke_consumer(void* arg) {
    long* sum = (long*)arg;
    int   v   = 0;
    while (pc_t_remove(&g_q, &v) == 0) {
        *sum += (long)v;
    }
    pthread_mutex_lock(&g_flag_lock);
    if (!g_closing) {
        g_premature++;
    }
    pthread_mutex_unlock(&g_flag_lock);
    return NULL;
}

static void test_concurrent_smoke(void) {
    pthread_t tp;
    pthread_t tc[SMOKE_C];
    long      s[SMOKE_C];
    long      total = 0;

    for (int i = 0; i < SMOKE_C; i++) {
        s[i] = 0;
    }
    pthread_mutex_lock(&g_flag_lock);
    g_closing   = 0;
    g_premature = 0;
    pthread_mutex_unlock(&g_flag_lock);

    TEST_ASSERT_EQUAL_INT(0, pc_t_init(&g_q, 1));       /* cap=1：最狠的交错 */
    for (int i = 0; i < SMOKE_C; i++) {
        TEST_ASSERT_EQUAL_INT(0, pthread_create(&tc[i], NULL, smoke_consumer, &s[i]));
    }
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&tp, NULL, smoke_producer, NULL));

    TEST_ASSERT_EQUAL_INT(0, pthread_join(tp, NULL));
    pthread_mutex_lock(&g_flag_lock);
    g_closing = 1;
    pthread_mutex_unlock(&g_flag_lock);
    TEST_ASSERT_EQUAL_INT(0, pc_t_close(&g_q));
    for (int i = 0; i < SMOKE_C; i++) {
        TEST_ASSERT_EQUAL_INT(0, pthread_join(tc[i], NULL));
        total += s[i];
    }

    TEST_ASSERT_EQUAL_INT(SMOKE_N, (int)total);
    TEST_ASSERT_EQUAL_INT(0, pc_t_size(&g_q));
    TEST_ASSERT_EQUAL_INT(0, g_premature);
    pc_t_destroy(&g_q);
}

/* ==========================================================================
 * 八、假唤醒注入：while 不能写成 if（确定性抓手）
 * ======================================================================== */

struct victim_arg {
    pc_t* q;
    int   rc;
    int   value;
};

static void* spurious_victim(void* arg) {
    struct victim_arg* a = (struct victim_arg*)arg;
    a->value = -1;
    a->rc    = pc_t_remove(a->q, &a->value);
    return NULL;
}

static void test_spurious_wakeup_needs_while(void) {
    pc_t              q;
    struct victim_arg a;
    pthread_t         tv;

    TEST_ASSERT_EQUAL_INT(0, pc_t_init(&q, 4));
    a.q     = &q;
    a.rc    = 123;
    a.value = -1;
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&tv, NULL, spurious_victim, &a));
    sleep_ms(50);                       /* 让它进入 cond_wait */

    /* 假唤醒风暴：连发多次，确保至少一次落在"它正在 wait"的窗口里。
       while 版醒来发现谓词仍为假 → 继续等（假唤醒是合法的，无害）；
       if  版会直接往下走，在「未关闭的空队列」上返回失败 */
    for (int i = 0; i < 20; i++) {
        TEST_ASSERT_EQUAL_INT(0, pthread_mutex_lock(&q.lock));
        TEST_ASSERT_EQUAL_INT(0, pthread_cond_signal(&q.not_empty));
        TEST_ASSERT_EQUAL_INT(0, pthread_mutex_unlock(&q.lock));
        sleep_ms(5);
    }
    sleep_ms(20);

    TEST_ASSERT_EQUAL_INT(0, pc_t_insert(&q, 7));   /* 这一次才是"真"唤醒 */
    TEST_ASSERT_EQUAL_INT(0, pthread_join(tv, NULL));

    TEST_ASSERT_EQUAL_INT(0, a.rc);                 /* if 版在这儿红：rc == -1 */
    TEST_ASSERT_EQUAL_INT(7, a.value);
    pc_t_destroy(&q);
}

/* ==========================================================================
 * main
 * ======================================================================== */

int main(void) {
    UNITY_BEGIN();

    /* 单线程语义 */
    RUN_TEST(test_init_rejects_zero_cap);
    RUN_TEST(test_init_destroy_smoke);
    RUN_TEST(test_single_thread_fifo);
    RUN_TEST(test_wraparound);

    /* 阻塞与唤醒 */
    RUN_TEST(test_insert_blocks_when_full);
    RUN_TEST(test_remove_blocks_when_empty);

    /* close 协议 */
    RUN_TEST(test_close_wakes_all_waiters);
    RUN_TEST(test_close_drains_before_failing);
    RUN_TEST(test_close_unblocks_blocked_producer);
    RUN_TEST(test_close_is_idempotent);
    RUN_TEST(test_insert_after_close_fails);

    /* 生命周期与顺序 */
    RUN_TEST(test_destroy_then_reuse);
    RUN_TEST(test_fifo_order_1p1c);

    /* 并发形状矩阵 */
    RUN_TEST(test_shape_1p1c);
    RUN_TEST(test_shape_1p4c);
    RUN_TEST(test_shape_4p1c);
    RUN_TEST(test_shape_4p4c);
    RUN_TEST(test_shape_4p4c_cap1);
    RUN_TEST(test_shape_2p8c_cap1);

    /* 变异验证依赖的两条 */
    RUN_TEST(test_concurrent_smoke);
    RUN_TEST(test_spurious_wakeup_needs_while);

    return UNITY_END();
}

/* ============================================================================
 * 还没覆盖、可以继续加的维度（留给你按需要补）：
 *   - 生产者多于消费者时 close 与"排空"的交错（P > C 且 items 很大）
 *   - insert/remove 被信号打断（EINTR）时的行为
 *   - pc_t_size 在并发下的边界：始终 0 <= size <= cap
 *   - destroy 时有等待者属 UB —— 这条没法测，只能写进 README 的契约
 * ========================================================================== */
