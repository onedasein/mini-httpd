/* tests/unit/test_parse.c —— parse_line() 单元测试（mini shell 热身 T0.4）
 *
 * 位置：tests/unit/ 这一层（不是 tests/unit/unity/）——
 *   Makefile 用 TEST_SRC := $(wildcard tests/unit/test_*.c) 发现用例，unity/ 只装框架。
 * 用例名和断言消息一律 ASCII：Unity 的 *_MESSAGE 会把非 ASCII 转义成 \xNN；中文写注释。
 *
 * 本文件钉住的契约（parse_line 的规范）：
 *   1. 按空白（空格 / tab / 换行）切 token，连续空白算一个；
 *   2. "|" 结束当前命令、开始下一条（必须左右有空白，见 test_split_table 的现状项）；
 *   3. ">" 把「下一个 token」记为当前命令的 outfile（不要求它写在末尾）；
 *   4. 每条命令的 argv 以 NULL 结尾；argc 不含这个 NULL；
 *   5. argv 有 MAX_ARGS 格，最后一格留给 NULL ⇒ 每条命令最多 MAX_ARGS-1 个参数；
 *   6. 失败返回 1，且不修改 *ncmds（调用方必须先看返回值再决定要不要执行）；
 *   7. line 被就地改写 —— argv[] 指向 line 内部，所以 line 必须可写、且活得比 cmds[] 久。
 */
#include "parse_line.h"
#include "unity.h"

#include <stdio.h>
#include <string.h>

/* 与 parse_line.c 里的命令数上限保持一致。
   建议以后提到 parse_line.h 里只定义一次，别在三个文件里各写一个 32。 */
#ifndef MAX_CMDS
#define MAX_CMDS 32
#endif

/* Unity 要求这两个符号存在（用不上就留空） */
void setUp(void) {}
void tearDown(void) {}

/* ---------- 小工具 ---------- */

/* strtok_r 会就地写 '\0'，所以输入必须先拷进可写缓冲区。
   千万别直接传字符串字面量 —— 字面量在 .rodata，改写它 = 段错误。 */
static void copy_in(char* dst, size_t cap, const char* src)
{
    size_t n = strlen(src);
    TEST_ASSERT_TRUE_MESSAGE(n + 1 <= cap, "test input longer than the buffer");
    memcpy(dst, src, n + 1);
}

/* 断言一条命令的完整形状：argc / argv[] / NULL 结尾 / outfile 归属 */
static void expect_cmd(const char* label, const struct command* c, int want_argc,
                       const char* const* want_argv, const char* want_outfile)
{
    TEST_ASSERT_EQUAL_INT_MESSAGE(want_argc, c->argc, label);
    for (int i = 0; i < want_argc; i++)
        TEST_ASSERT_EQUAL_STRING_MESSAGE(want_argv[i], c->argv[i], label);
    TEST_ASSERT_NULL_MESSAGE(c->argv[c->argc], "argv must be NULL-terminated (execvp needs it)");
    if (want_outfile != NULL)
        TEST_ASSERT_EQUAL_STRING_MESSAGE(want_outfile, c->outfile, label);
    else
        TEST_ASSERT_NULL_MESSAGE(c->outfile, label);
}

/* ---------- 验收命令：ls | wc -l > out.txt ---------- */

void test_acceptance_pipe_and_redirect(void)
{
    char buf[128];
    struct command cmds[4];
    int ncmds = -1;
    const char* cmd0[] = {"ls"};
    const char* cmd1[] = {"wc", "-l"};

    copy_in(buf, sizeof buf, "ls | wc -l > out.txt\n");
    TEST_ASSERT_EQUAL_INT(0, parse_line(buf, cmds, &ncmds));

    TEST_ASSERT_EQUAL_INT_MESSAGE(2, ncmds, "a pipe must split the line into two commands");
    /* outfile 属于「它前面那条命令」：> out.txt 写在 wc -l 后面，重定向的是 wc 的 stdout */
    expect_cmd("cmds[0]", &cmds[0], 1, cmd0, NULL);
    expect_cmd("cmds[1]", &cmds[1], 2, cmd1, "out.txt");
}

/* ---------- 切词：空白与参数个数（表驱动）---------- */

typedef struct {
    const char* name;
    const char* input;
    int         want_ncmds;
    int         want_argc0;
    const char* want_argv0[4];
} SplitCase;

void test_split_table(void)
{
    static const SplitCase cases[] = {
        /* name                  input                 ncmds argc0 argv0 */
        {"single arg",          "ls\n",                1, 1, {"ls", NULL}},
        {"three args",          "ls -l /tmp\n",        1, 3, {"ls", "-l", "/tmp", NULL}},
        {"no trailing NL",      "ls -l",               1, 2, {"ls", "-l", NULL}},
        {"leading spaces",      "   ls\n",             1, 1, {"ls", NULL}},
        {"trailing spaces",     "ls -l   \n",          1, 2, {"ls", "-l", NULL}},
        {"tab separated",       "ls\t-l\t/tmp\n",      1, 3, {"ls", "-l", "/tmp", NULL}},
        {"mixed whitespace",    "  ls \t -l  \n",      1, 2, {"ls", "-l", NULL}},
        {"spaces around pipe",  "ls    |    wc\n",     2, 1, {"ls", NULL}},
        /* 现状（不是需求）：分隔符只有 " \t\n"，所以字面量按空白切，
           "ls|wc" 不会被拆开；CRLF 的 \r 也会留在参数尾巴上。 */
        {"no space around pipe","ls|wc\n",             1, 1, {"ls|wc", NULL}},
        {"crlf not stripped",   "ls\r\n",              1, 1, {"ls\r", NULL}},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const SplitCase* tc = &cases[i];
        char buf[128];
        struct command cmds[4];
        int ncmds = -1;

        copy_in(buf, sizeof buf, tc->input);
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, parse_line(buf, cmds, &ncmds), tc->name);
        TEST_ASSERT_EQUAL_INT_MESSAGE(tc->want_ncmds, ncmds, tc->name);
        expect_cmd(tc->name, &cmds[0], tc->want_argc0, tc->want_argv0, NULL);
    }
}

/* ---------- 管道：一条输入切成多条命令 ---------- */

void test_pipe_chain(void)
{
    char buf[128];
    struct command cmds[4];
    int ncmds = -1;
    const char* a[] = {"ls", "-l"};
    const char* b[] = {"grep", "foo"};
    const char* c[] = {"wc", "-l"};

    copy_in(buf, sizeof buf, "ls -l | grep foo | wc -l\n");
    TEST_ASSERT_EQUAL_INT(0, parse_line(buf, cmds, &ncmds));

    TEST_ASSERT_EQUAL_INT(3, ncmds);
    expect_cmd("cmds[0]", &cmds[0], 2, a, NULL);
    expect_cmd("cmds[1]", &cmds[1], 2, b, NULL);
    expect_cmd("cmds[2]", &cmds[2], 2, c, NULL);
}

/* ---------- 重定向：outfile 归属与位置 ---------- */

void test_redirect_variants(void)
{
    {
        /* 写在末尾 */
        char buf[64];
        struct command cmds[2];
        int ncmds = -1;
        const char* want[] = {"ls", "-l"};
        copy_in(buf, sizeof buf, "ls -l > out.txt\n");
        TEST_ASSERT_EQUAL_INT(0, parse_line(buf, cmds, &ncmds));
        expect_cmd("redirect at end", &cmds[0], 2, want, "out.txt");
    }
    {
        /* 写在前面：> 只是「把下一个 token 记成 outfile」，不要求它在末尾 */
        char buf[64];
        struct command cmds[2];
        int ncmds = -1;
        const char* want[] = {"ls"};
        copy_in(buf, sizeof buf, "> out.txt ls\n");
        TEST_ASSERT_EQUAL_INT(0, parse_line(buf, cmds, &ncmds));
        expect_cmd("redirect at front", &cmds[0], 1, want, "out.txt");
    }
    {
        /* outfile 只属于当前那条命令，管道之后的不受影响 */
        char buf[64];
        struct command cmds[2];
        int ncmds = -1;
        const char* c0[] = {"ls"};
        const char* c1[] = {"wc"};
        copy_in(buf, sizeof buf, "ls > out.txt | wc\n");
        TEST_ASSERT_EQUAL_INT(0, parse_line(buf, cmds, &ncmds));
        TEST_ASSERT_EQUAL_INT(2, ncmds);
        expect_cmd("before pipe", &cmds[0], 1, c0, "out.txt");
        expect_cmd("after pipe", &cmds[1], 1, c1, NULL);
    }
    {
        /* 现状（不是需求）：同一个命令写两个 > 时，后一个覆盖前一个（只留一个 outfile） */
        char buf[64];
        struct command cmds[2];
        int ncmds = -1;
        const char* want[] = {"ls"};
        copy_in(buf, sizeof buf, "ls > a.txt > b.txt\n");
        TEST_ASSERT_EQUAL_INT(0, parse_line(buf, cmds, &ncmds));
        expect_cmd("last redirect wins", &cmds[0], 1, want, "b.txt");
    }
}

/* ---------- 语法错误：> 后面没有文件名 ---------- */

void test_redirect_without_filename_is_error(void)
{
    static const char* bad[] = {"ls >", "ls >\n", "ls >   ", "ls >\t\n"};

    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        char buf[64];
        struct command cmds[2];
        int ncmds = 42;      /* 故意写成非零：失败时它必须原样不动 */

        copy_in(buf, sizeof buf, bad[i]);
        TEST_ASSERT_EQUAL_INT_MESSAGE(1, parse_line(buf, cmds, &ncmds), bad[i]);
        TEST_ASSERT_EQUAL_INT_MESSAGE(42, ncmds,
            "on failure *ncmds must stay untouched (caller uses it to decide)");
    }
}

/* ---------- 退化输入：空行 / 只留一侧的管道 ---------- */

void test_empty_input_yields_one_empty_command(void)
{
    static const char* blank[] = {"", "\n", "   \n", "\t\n"};

    for (size_t i = 0; i < sizeof(blank) / sizeof(blank[0]); i++) {
        char buf[64];
        struct command cmds[2];
        int ncmds = -1;

        copy_in(buf, sizeof buf, blank[i]);
        TEST_ASSERT_EQUAL_INT(0, parse_line(buf, cmds, &ncmds));
        /* 现状：空输入不是 0 条命令，而是 1 条「argc == 0」的空命令。
           约定：execute_line 必须跳过 argc == 0 的命令，否则 execvp(NULL) 会炸。
           若以后改成 ncmds = 0，改这两行并同步 execute_line。 */
        TEST_ASSERT_EQUAL_INT(1, ncmds);
        TEST_ASSERT_EQUAL_INT(0, cmds[0].argc);
        TEST_ASSERT_NULL(cmds[0].argv[0]);
    }
}

void test_leading_and_trailing_pipe(void)
{
    {
        char buf[64];
        struct command cmds[3];
        int ncmds = -1;
        const char* want[] = {"ls"};
        copy_in(buf, sizeof buf, "| ls\n");
        TEST_ASSERT_EQUAL_INT(0, parse_line(buf, cmds, &ncmds));
        /* 现状：前导 | 也照样切，于是多出一条空命令（cmds[0].argc == 0） */
        TEST_ASSERT_EQUAL_INT(2, ncmds);
        TEST_ASSERT_EQUAL_INT(0, cmds[0].argc);
        expect_cmd("after leading pipe", &cmds[1], 1, want, NULL);
    }
    {
        char buf[64];
        struct command cmds[3];
        int ncmds = -1;
        const char* want[] = {"ls"};
        copy_in(buf, sizeof buf, "ls |\n");
        TEST_ASSERT_EQUAL_INT(0, parse_line(buf, cmds, &ncmds));
        /* 现状：尾随 | 同样多出一条空命令 —— 这两种其实都该算语法错误 */
        TEST_ASSERT_EQUAL_INT(2, ncmds);
        expect_cmd("before trailing pipe", &cmds[0], 1, want, NULL);
        TEST_ASSERT_EQUAL_INT(0, cmds[1].argc);
    }
    {
        /* 连续两个管道（中间必须有空白，否则 "||" 只是一个普通 token） */
        char buf[64];
        struct command cmds[4];
        int ncmds = -1;
        const char* c0[] = {"ls"};
        const char* c2[] = {"wc"};
        copy_in(buf, sizeof buf, "ls | | wc\n");
        TEST_ASSERT_EQUAL_INT(0, parse_line(buf, cmds, &ncmds));
        TEST_ASSERT_EQUAL_INT(3, ncmds);
        expect_cmd("cmds[0]", &cmds[0], 1, c0, NULL);
        TEST_ASSERT_EQUAL_INT(0, cmds[1].argc);
        expect_cmd("cmds[2]", &cmds[2], 1, c2, NULL);
    }
}

/* ---------- 边界：MAX_ARGS ---------- */

/* 造一条「n 个单字符参数」的输入 */
static void build_args(char* dst, size_t cap, int n)
{
    size_t off = 0;
    for (int i = 0; i < n; i++) {
        int wrote = snprintf(dst + off, cap - off, i ? " a" : "a");
        TEST_ASSERT_TRUE_MESSAGE(wrote > 0 && (size_t)wrote < cap - off, "test input too long");
        off += (size_t)wrote;
    }
}

void test_max_args_boundary(void)
{
    {
        /* 127 个参数：塞得下（argv[0..126] + NULL 在 argv[127]） */
        char buf[512];
        struct command cmds[2];
        int ncmds = -1;
        build_args(buf, sizeof buf, MAX_ARGS - 1);
        TEST_ASSERT_EQUAL_INT(0, parse_line(buf, cmds, &ncmds));
        TEST_ASSERT_EQUAL_INT(1, ncmds);
        TEST_ASSERT_EQUAL_INT(MAX_ARGS - 1, cmds[0].argc);
        TEST_ASSERT_EQUAL_STRING("a", cmds[0].argv[0]);
        TEST_ASSERT_NULL_MESSAGE(cmds[0].argv[MAX_ARGS - 1], "NULL goes after the last argument");
    }
    {
        /* 128 个参数：argv 总共只有 MAX_ARGS 格，最后那格要留给 NULL，
           所以第 MAX_ARGS 个参数必须被拒绝。当前实现只在 argc >= MAX_ARGS + 1
           时才拒绝，于是第 128 个 token 会让收尾的 argv[argc] = NULL 写到 argc 字段上。 */
        char buf[512];
        struct command cmds[2];
        int ncmds = -1;
        build_args(buf, sizeof buf, MAX_ARGS);
        TEST_ASSERT_EQUAL_INT_MESSAGE(1, parse_line(buf, cmds, &ncmds),
            "MAX_ARGS slots must include the NULL terminator => at most MAX_ARGS-1 args");
    }
}

/* ---------- 边界：命令数上限，且必须「先检查再写」 ---------- */

/* 用哨兵包住 cmds[]：越界写必然踩到它。
   这样 debug 和 ASan 两种跑法都能稳定抓到（裸数组在 debug 下踩栈往往悄无声息）。 */
typedef struct {
    struct command cmds[MAX_CMDS];
    unsigned char  canary[2 * sizeof(struct command)];
} CmdGuard;

static const unsigned char CANARY_BYTE = 0xAAu;

static void guard_reset(CmdGuard* g)
{
    memset(g->cmds, 0, sizeof g->cmds);
    memset(g->canary, CANARY_BYTE, sizeof g->canary);
}

/* 返回第一处被踩坏的下标；没被踩则返回 -1 */
static int guard_damage(const CmdGuard* g)
{
    for (size_t i = 0; i < sizeof g->canary; i++) {
        if (g->canary[i] != CANARY_BYTE) return (int)i;
    }
    return -1;
}

/* 造一条「n 条命令」的输入：a | a | a ... */
static void build_cmds(char* dst, size_t cap, int n_cmds)
{
    size_t off = 0;
    for (int i = 0; i < n_cmds; i++) {
        int wrote = snprintf(dst + off, cap - off, i ? " | a" : "a");
        TEST_ASSERT_TRUE_MESSAGE(wrote > 0 && (size_t)wrote < cap - off, "test input too long");
        off += (size_t)wrote;
    }
}

/* 边界内侧：MAX_CMDS 条命令正好塞满 cmds[0 .. MAX_CMDS-1]，必须成功。
   这条同时回答「管道那里要预留一格还是两格」：
   循环外那次 n++ 只是把「最后一个下标」换算成「命令数」，它不占新槽位，
   所以 32 条命令是合法的，不能提前报错。 */
void test_exactly_max_cmds_is_allowed(void)
{
    static CmdGuard guard;
    char buf[1024];
    int ncmds = -1;

    guard_reset(&guard);
    build_cmds(buf, sizeof buf, MAX_CMDS);

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, parse_line(buf, guard.cmds, &ncmds),
        "exactly MAX_CMDS commands must fit in cmds[MAX_CMDS]");
    TEST_ASSERT_EQUAL_INT(MAX_CMDS, ncmds);
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, guard_damage(&guard), "wrote past cmds[MAX_CMDS]");
}

/* 边界外侧：再多就必须在「写之前」拒绝，而且不许碰界外内存 */
void test_too_many_commands_must_not_overrun(void)
{
    static CmdGuard guard;
    char buf[1024];
    int ncmds = -1;

    guard_reset(&guard);
    build_cmds(buf, sizeof buf, MAX_CMDS + 2);

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, parse_line(buf, guard.cmds, &ncmds),
        "more than MAX_CMDS commands must fail");
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, guard_damage(&guard),
        "out-of-bounds write: bytes past cmds[MAX_CMDS] were clobbered"
        " (the limit check must run before the write)");
}

/* ---------- 契约：line 被就地改写 ---------- */

void test_line_is_rewritten_in_place(void)
{
    char buf[64];
    struct command cmds[2];
    int ncmds = -1;

    copy_in(buf, sizeof buf, "echo hi\n");
    TEST_ASSERT_EQUAL_INT(0, parse_line(buf, cmds, &ncmds));

    /* argv[] 指向 line 内部（strtok_r 把分隔符写成了 '\0'）：
       所以 line 必须是可写的，而且要比 cmds[] 活得久。 */
    TEST_ASSERT_EQUAL_PTR_MESSAGE(buf, cmds[0].argv[0], "argv[0] points into the line buffer");
    TEST_ASSERT_EQUAL_STRING("echo", buf);
    TEST_ASSERT_EQUAL_STRING("hi", buf + 5);
}

/* ---------- 主入口 ---------- */

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_acceptance_pipe_and_redirect);
    RUN_TEST(test_split_table);
    RUN_TEST(test_pipe_chain);
    RUN_TEST(test_redirect_variants);
    RUN_TEST(test_redirect_without_filename_is_error);
    RUN_TEST(test_empty_input_yields_one_empty_command);
    RUN_TEST(test_leading_and_trailing_pipe);
    RUN_TEST(test_max_args_boundary);
    RUN_TEST(test_exactly_max_cmds_is_allowed);
    RUN_TEST(test_too_many_commands_must_not_overrun);
    RUN_TEST(test_line_is_rewritten_in_place);
    return UNITY_END();
}
