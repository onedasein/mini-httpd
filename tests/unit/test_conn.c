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

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_conn_table);
    RUN_TEST(test_conn_compact);
    RUN_TEST(test_io_read_states);
    RUN_TEST(test_io_write_states);
    return UNITY_END();
}
