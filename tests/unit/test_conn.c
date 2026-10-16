/* tests/unit/test_conn.c — 连接表 / 残余搬移 / 非阻塞 IO 包装（M2 T2.1b、T2.1c、T2.2a）
 *
 * 和别的测试文件同一套写法：**按类别一个测试函数、每个里面一个简单循环**。
 * 这里**不需要真网络**：socketpair() 就能造出一对 fd，正是这三个任务要测的东西。
 *
 *   test_conn_table      fd → conn：建/取/销毁不串号；拿不到槽位返回 NULL；free 会关 fd
 *   test_conn_compact    残余字节前移：roff == 0 / 部分消费 / 全消费 / roff > rlen 四种
 *   test_io_read_states  非阻塞读的四种返回：IO_OK / IO_WOULD_BLOCK / IO_CLOSED / IO_ERROR
 *   test_io_write_states 非阻塞写 + io_wait 的超时与就绪
 */
#include "unity.h"
#include "conn.h"

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

void setUp(void) {}
void tearDown(void) {}

/* 一个用例一个断言太散，这里统一走"条件 + 说明"（失败信息里带 where） */
static void ck(const char *what, int ok) {
    if (!ok) {
        char msg[160];
        snprintf(msg, sizeof msg, "不满足：%s", what);
        TEST_FAIL_MESSAGE(msg);
    }
}

static int make_pair(int sv[2]) {
    return socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
}

/* ==================== fd → conn（T2.1b） ==================== */
static void test_conn_table(void) {
    int sv[2];
    struct stat st;
    conn *a;

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, make_pair(sv), "socketpair");

    a = conn_alloc(sv[0]);
    ck("alloc 返回非 NULL", a != NULL);
    ck("lookup 拿到同一个对象", a != NULL && conn_lookup(sv[0]) == a);
    ck("没建过的 fd → NULL", conn_lookup(sv[1]) == NULL);
    ck("同一个 fd 建两次 → 拒绝（不串号）", conn_alloc(sv[0]) == NULL);
    ck("fd 越界 → NULL", conn_alloc(CONN_MAX_FD) == NULL);
    ck("负数 fd → NULL", conn_alloc(-1) == NULL);

    conn_free(a);
    ck("free 之后 lookup 为 NULL", conn_lookup(sv[0]) == NULL);
    errno = 0;
    ck("free 关掉了 fd（EBADF）", fstat(sv[0], &st) == -1 && errno == EBADF);
    conn_free(NULL); /* 对 NULL 安全 */

    close(sv[1]);
}

/* ==================== 残余搬移（T2.1c） ==================== */
static void test_conn_compact(void) {
    // clang-format off
    struct { size_t rlen, roff, want_rlen; } cases[] = {
        { 10,  0, 10 },   /* 没消费：不动 */
        { 10,  4,  6 },   /* 消费了 4 个："余下 6 个"要挪到前面 */
        { 10, 10,  0 },   /* 全消费：直接归零 */
        { 10, 11,  0 },   /* roff > rlen（不该发生，但要稳） */
        {  0,  0,  0 },   /* 空缓冲 */
    };
    // clang-format on

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        conn c;
        char want[RBUF_SIZE];
        char label[96];
        size_t want_len = 0;

        memset(&c, 0, sizeof c);
        c.fd = -1;
        for (size_t k = 0; k < cases[i].rlen; k++) c.rbuf[k] = (char)('A' + (k % 26));
        c.rlen = cases[i].rlen;
        c.roff = cases[i].roff;

        /* 先算好"compact 之后前 want_rlen 个字节应当是什么" */
        if (cases[i].roff < cases[i].rlen) {
            want_len = cases[i].rlen - cases[i].roff;
            memcpy(want, c.rbuf + cases[i].roff, want_len);
        }

        conn_compact(&c);
        snprintf(label, sizeof label, "compact #%zu 后的 rlen/roff", i);
        TEST_ASSERT_EQUAL_INT_MESSAGE((int)cases[i].want_rlen, (int)c.rlen, label);
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, (int)c.roff, label);
        if (want_len > 0) {
            TEST_ASSERT_EQUAL_INT_MESSAGE(0, memcmp(c.rbuf, want, want_len), "compact 搬移的字节内容");
        }
    }
}

/* ==================== 非阻塞读的四种返回（T2.2a） ==================== */
static void test_io_read_states(void) {
    int sv[2];
    char buf[64];
    size_t got = 0;

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, make_pair(sv), "socketpair");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, conn_set_nonblocking(sv[0]), "set_nonblocking");

    ck("没有数据 → IO_WOULD_BLOCK", io_read(sv[0], buf, sizeof buf, &got) == IO_WOULD_BLOCK);
    ck("  （WOULD_BLOCK 时 got 归零）", got == 0);

    TEST_ASSERT_EQUAL_INT_MESSAGE(5, (int)write(sv[1], "hello", 5), "对端写 5 字节");
    ck("有数据 → IO_OK 且拿到 5 字节", io_read(sv[0], buf, sizeof buf, &got) == IO_OK && got == 5);
    ck("  内容正确", got == 5 && memcmp(buf, "hello", 5) == 0);
    ck("再读一次 → 又没有了", io_read(sv[0], buf, sizeof buf, &got) == IO_WOULD_BLOCK);

    TEST_ASSERT_EQUAL_INT_MESSAGE(6, (int)write(sv[1], "abcdef", 6), "对端写 6 字节");
    ck("cap 只给 3 → 短读，拿到 3 字节", io_read(sv[0], buf, 3, &got) == IO_OK && got == 3);
    ck("  剩下的还在缓冲里", io_read(sv[0], buf, sizeof buf, &got) == IO_OK && got == 3);

    close(sv[1]);
    ck("对端关闭 → IO_CLOSED", io_read(sv[0], buf, sizeof buf, &got) == IO_CLOSED);
    ck("坏 fd → IO_ERROR", io_read(-1, buf, sizeof buf, &got) == IO_ERROR);
    ck("cap == 0 不算关闭", io_read(sv[0], buf, 0, &got) == IO_OK);

    close(sv[0]);
}

/* ==================== 非阻塞写 + io_wait ==================== */
static void test_io_write_states(void) {
    int sv[2];
    char buf[64];
    size_t n = 0;

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, make_pair(sv), "socketpair");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, conn_set_nonblocking(sv[0]), "set_nonblocking");

    ck("写 2 字节 → IO_OK", io_write(sv[0], "ab", 2, &n) == IO_OK && n == 2);
    ck("  对端能读到", read(sv[1], buf, sizeof buf) == 2 && memcmp(buf, "ab", 2) == 0);
    ck("写 0 字节不是错误", io_write(sv[0], "x", 0, &n) == IO_OK && n == 0);
    ck("坏 fd → IO_ERROR", io_write(-1, "x", 1, &n) == IO_ERROR);

    ck("io_wait 超时 → 0", io_wait(sv[0], POLLIN, 30) == 0);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, (int)write(sv[1], "z", 1), "对端写 1 字节");
    ck("io_wait 就绪 → 1", io_wait(sv[0], POLLIN, 100) == 1);

    close(sv[1]);
    close(sv[0]);
}

/* ==================== 状态推导：下一步 + 该关注什么事件（T2.3b） ====================
 * 这张表就是事件循环的"不变式"：**该关注什么事件是推出来的**。
 * 写错一行的后果很典型：有数据要写却只关注 EPOLLIN → 在 epoll_wait 里死等（连接挂住）。 */
static void test_conn_next_step(void) {
    static const struct {
        conn_phase phase;
        size_t woff;
        size_t wlen;
        long long file_left;
        long long drain_left;
        int keep_alive;
        conn_step step;
        uint32_t events;
    } cases[] = {
        /* phase, woff, wlen, file_left, drain_left, keep_alive, 期望的下一步, 期望关注的事件 */
        {CONN_READ_REQ, 0, 0, 0, 0, 0, CONN_STEP_READ_REQ, EPOLLIN},
        {CONN_DRAIN, 0, 0, 0, 0, 0, CONN_STEP_DRAIN, EPOLLIN},
        {CONN_CLOSE, 0, 0, 0, 0, 0, CONN_STEP_CLOSE, 0},
        {CONN_WRITE, 0, 100, 0, 0, 0, CONN_STEP_WRITE_HEAD, EPOLLOUT},        /* 头一个字都没写出去 */
        {CONN_WRITE, 40, 100, 500, 0, 0, CONN_STEP_WRITE_HEAD, EPOLLOUT},     /* 头没写完：先不管文件体 */
        {CONN_WRITE, 100, 100, 500, 0, 0, CONN_STEP_SEND_FILE, EPOLLOUT},     /* 头写完 → 发文件体 */
        {CONN_WRITE, 100, 100, 500, 1024, 0, CONN_STEP_SEND_FILE, EPOLLOUT},  /* 文件体优先于收尾 */
        {CONN_WRITE, 100, 100, 0, 0, 0, CONN_STEP_CLOSE, 0},                  /* 都干完、不保持 → 关 */
        {CONN_WRITE, 100, 100, 0, 0, 1, CONN_STEP_NEXT_REQ, EPOLLIN},         /* 都干完、要保持 → 接下一个 */
        {CONN_WRITE, 100, 100, 0, 1024, 1, CONN_STEP_DRAIN, EPOLLIN},         /* 要收尾就必须关，保持让位 */
        {CONN_WRITE, 100, 100, 0, 1024, 0, CONN_STEP_DRAIN, EPOLLIN},         /* 都干完、要收尾 → 读干残留 */
        {CONN_WRITE, 0, 0, 0, 0, 1, CONN_STEP_NEXT_REQ, EPOLLIN},             /* 空 wbuf、无文件、保持 */
        {CONN_WRITE, 0, 0, 0, 0, 0, CONN_STEP_CLOSE, 0},                      /* 空 wbuf、无文件（防御） */
    };
    size_t i;

    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        conn c;
        char what[220];
        conn_step got;

        memset(&c, 0, sizeof c);
        c.phase = cases[i].phase;
        c.woff = cases[i].woff;
        c.wlen = cases[i].wlen;
        c.file_left = cases[i].file_left;
        c.drain_left = cases[i].drain_left;
        c.keep_alive = cases[i].keep_alive;
        got = conn_next_step(&c);
        snprintf(
            what,
            sizeof what,
            "表第 %zu 行（phase=%d woff=%zu wlen=%zu file_left=%lld drain_left=%lld keep_alive=%d）：想要 step=%d events=%u，实际 step=%d events=%u",
            i,
            (int)cases[i].phase,
            cases[i].woff,
            cases[i].wlen,
            cases[i].file_left,
            cases[i].drain_left,
            cases[i].keep_alive,
            (int)cases[i].step,
            cases[i].events,
            (int)got,
            conn_step_events(got)
        );
        ck(what, got == cases[i].step && conn_step_events(got) == cases[i].events);
    }
}

/* ==================== 活连接遍历（T2.3b 的超时扫描用） ==================== */
static void test_conn_iter(void) {
    int sv[3][2];
    conn *c[3];
    int got[8];
    int n;
    size_t i;

    for (i = 0; i < 3; i++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, make_pair(sv[i]), "socketpair");
        c[i] = conn_alloc(sv[i][0]);
        ck("alloc 成功", c[i] != NULL);
    }

    n = 0;
    for (int fd = conn_next_fd(-1); fd >= 0; fd = conn_next_fd(fd)) {
        if (n < (int)(sizeof got / sizeof got[0])) got[n] = fd;
        n++;
    }
    ck("三条都遍历到了", n == 3);
    ck("  按 fd 升序", n == 3 && got[0] < got[1] && got[1] < got[2]);
    ck("  吐出来的正是建过的 fd", n == 3 && (got[0] == sv[0][0] || got[0] == sv[1][0] || got[0] == sv[2][0]));

    /* 扫描里边走边关的连接不能被漏掉、也不能重复吐（T2.5a 的收尾顺序靠这个） */
    conn_free(c[1]);
    n = 0;
    for (int fd = conn_next_fd(-1); fd >= 0; fd = conn_next_fd(fd)) {
        ck("  已关掉的 fd 不再出现", fd != sv[1][0]);
        n++;
    }
    ck("关掉一条后只剩两条", n == 2);

    conn_free(c[0]);
    conn_free(c[2]);
    ck("全关掉后遍历立即结束", conn_next_fd(-1) == -1);

    close(sv[0][1]);
    close(sv[1][1]);
    close(sv[2][1]);
}

/* ==================== wbuf 装载 + fd 所有权（T2.3b） ==================== */
static void test_conn_wbuf_and_owned_fds(void) {
    int sv[2];
    int pr[2]; /* 拿 pipe 的读端当"文件 fd"的替身：fstat 能用，全是 fd 语义 */
    struct stat st;
    conn *c;

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, make_pair(sv), "socketpair");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, pipe(pr), "pipe");
    c = conn_alloc(sv[0]);
    ck("alloc 成功", c != NULL);
    ck("新连接的 file_fd 是 -1（不能靠 calloc 的 0 —— 0 是合法 fd）", c->file_fd == -1);
    ck("单调毫秒可用", conn_now_ms() > 0);

    ck("装 wbuf → 长度与写游标就位", conn_set_wbuf(c, "abc", 3) == 0 && c->wlen == 3 && c->woff == 0);
    ck("  内容正确", c->wbuf != NULL && memcmp(c->wbuf, "abc", 3) == 0);
    ck("换一份 → 新的生效", conn_set_wbuf(c, "de", 2) == 0 && c->wlen == 2 && memcmp(c->wbuf, "de", 2) == 0);
    ck("装 0 字节 → wbuf 归 NULL（不留空 malloc）", conn_set_wbuf(c, "", 0) == 0 && c->wbuf == NULL);
    ck("  写游标也归零", c->wlen == 0 && c->woff == 0);

    /* 连接对象拥有 fd：conn_free 得把连接 fd 和文件 fd 一起关掉（否则就是漏 fd） */
    c->file_fd = pr[0];
    conn_free(c);
    errno = 0;
    ck("free 关掉了连接 fd", fstat(sv[0], &st) == -1 && errno == EBADF);
    errno = 0;
    ck("free 也关掉了 file_fd", fstat(pr[0], &st) == -1 && errno == EBADF);

    close(sv[1]);
    close(pr[1]);
}

/* ==================== keep-alive 复位（T2.6） ====================
 * 一个请求处理完、连接要留着接下一个：**每请求一份**的资源必须清干净、连接级的东西必须留着。
 * 漏清的两种典型后果：① file_fd 不关 → fd 泄漏；② rscan 不清 → 下一个请求从错误的位置扫。 */
static void test_conn_reset_for_next_request(void) {
    int sv[2];
    int pr[2];
    struct stat st;
    conn *c;

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, make_pair(sv), "socketpair");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, pipe(pr), "pipe");
    c = conn_alloc(sv[0]);
    ck("alloc 成功", c != NULL);

    /* 造一个"刚处理完一个请求"的现场 */
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, conn_set_wbuf(c, "HTTP/1.1 200 OK\r\n\r\n", 19), "装 wbuf");
    c->woff = 7;
    c->file_fd = pr[0];
    c->file_off = 4096;
    c->file_left = 123;
    c->drain_left = 999;
    c->drain_deadline_ms = 12345;
    c->keep_alive = 1;
    c->rscan = 42;
    c->roff = 42;
    c->rlen = 100; /* 残余字节：复位**不能**动它（那是下一个请求的开头） */
    c->phase = CONN_WRITE;

    conn_reset_for_next_request(c);

    ck("wbuf 释放", c->wbuf == NULL && c->wlen == 0 && c->woff == 0);
    ck("file 状态清干净", c->file_fd == -1 && c->file_left == 0 && c->file_off == 0);
    ck("drain 状态清干净", c->drain_left == 0 && c->drain_deadline_ms == 0);
    ck("keep_alive 归零（下一个请求重新判断）", c->keep_alive == 0);
    ck("扫描游标归零（残余已 compact 到 rbuf[0]）", c->rscan == 0 && c->roff == 0);
    ck("回到读请求", c->phase == CONN_READ_REQ);
    ck("残余字节不动（下一个请求就靠它）", c->rlen == 100);
    errno = 0;
    ck("file_fd 被关掉（不漏 fd）", fstat(pr[0], &st) == -1 && errno == EBADF);
    errno = 0;
    ck("连接 fd 还在（没被误关）", fstat(sv[0], &st) == 0);

    conn_free(c); /* 这时才关连接 fd */
    errno = 0;
    ck("conn_free 才关连接 fd", fstat(sv[0], &st) == -1 && errno == EBADF);
    close(sv[1]);
    close(pr[1]);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_conn_table);
    RUN_TEST(test_conn_compact);
    RUN_TEST(test_io_read_states);
    RUN_TEST(test_io_write_states);
    RUN_TEST(test_conn_next_step);
    RUN_TEST(test_conn_iter);
    RUN_TEST(test_conn_wbuf_and_owned_fds);
    RUN_TEST(test_conn_reset_for_next_request);
    return UNITY_END();
}
