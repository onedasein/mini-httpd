/* tests/unit/test_execute_line.c —— execute_line() 单元测试（mini shell 热身 T0.4）
 *
 * 位置：tests/unit/ 这一层（不是 tests/unit/unity/）。
 *
 * 特性宏必须在所有 include 之前：-std=c11 是严格 ISO 模式，mkstemp / unlink /
 * access / close 这些 POSIX 接口不声明，而 Makefile 开了
 * -Werror=implicit-function-declaration ⇒ 少这一行直接编译失败。
 *
 * execute_line 会 fork/exec，所以这里的「单元测试」只能直接构造 struct command
 * 交给它跑（不经过 shell 二进制）。能验证的是：
 *   - 参数校验（ncmds <= 0 / 空命令）在 fork 之前就挡住；
 *   - 重定向、管道、fd 接线的结果对不对（用临时文件看结果，不污染测试输出）；
 *   - 返回值语义。
 * 真正「起二进制、走 stdin、比对」的那一层属于 tests/e2e/run.sh，不是这里。
 *
 * 用例名和断言消息一律 ASCII：Unity 的 *_MESSAGE 会把非 ASCII 转义成 \xNN。
 */
#define _POSIX_C_SOURCE 200809L

#include "execute_line.h"
#include "parse_line.h"
#include "unity.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Unity 要求这两个符号存在（用不上就留空） */
void setUp(void) {}
void tearDown(void) {}

/* ---------- 小工具 ---------- */

/* 造一个一次性临时文件，把路径写进 dst */
static void make_tmp(char* dst, size_t cap)
{
    int n = snprintf(dst, cap, "/tmp/shell_exec_test_XXXXXX");
    TEST_ASSERT_TRUE_MESSAGE(n > 0 && (size_t)n < cap, "tmp path too long");
    int fd = mkstemp(dst);
    TEST_ASSERT_TRUE_MESSAGE(fd >= 0, "mkstemp failed");
    close(fd);
}

/* 读回整个文件（断言它存在） */
static void read_all(const char* path, char* dst, size_t cap)
{
    FILE* f = fopen(path, "r");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, "output file was not created");
    size_t n = fread(dst, 1, cap - 1, f);
    dst[n] = '\0';
    TEST_ASSERT_EQUAL_INT(0, fclose(f));
}

/* 填一条命令：argv 以 NULL 结尾，outfile 可以是 NULL */
static void make_cmd(struct command* c, char** argv, char* outfile)
{
    int i = 0;
    for (; argv[i] != NULL; i++) c->argv[i] = argv[i];
    c->argv[i] = NULL;
    c->argc = i;
    c->outfile = outfile;
}

/* ---------- 参数校验：不许 fork ---------- */

void test_zero_or_negative_ncmds_is_noop(void)
{
    struct command cmds[1];
    cmds[0].argc = 0;

    /* 提前返回：不算错误，也不该碰 cmds（所以传 NULL 也安全） */
    TEST_ASSERT_EQUAL_INT(0, execute_line(cmds, 0));
    TEST_ASSERT_EQUAL_INT(0, execute_line(NULL, 0));
    TEST_ASSERT_EQUAL_INT(0, execute_line(NULL, -1));
}

void test_empty_command_rejected_before_any_fork(void)
{
    char path[64];
    char* argv[] = {"echo", "should-not-run", NULL};
    struct command cmds[2];

    make_tmp(path, sizeof path);
    unlink(path);            /* 先删掉：万一第一条真的跑了，它会重新出现 */

    make_cmd(&cmds[0], argv, path);   /* 这条带 outfile，跑起来一定会建文件 */
    cmds[1].argc = 0;                 /* 第二条是空命令 */
    cmds[1].argv[0] = NULL;
    cmds[1].outfile = NULL;

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, execute_line(cmds, 2), "empty command must be rejected");
    /* 校验在 fork 之前：前面的命令一次都不许执行 */
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, access(path, F_OK),
        "no child may run when the line is rejected (check happens before fork)");
}

/* ---------- 重定向 ---------- */

void test_redirect_writes_and_truncates(void)
{
    char path[64];
    char got[64];
    char* argv[] = {"echo", "hi", NULL};
    struct command cmds[1];

    make_tmp(path, sizeof path);

    /* 先写点垃圾进去：O_TRUNC 应该把它清掉 */
    FILE* f = fopen(path, "w");
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_TRUE(fputs("junk-junk-junk\n", f) >= 0);
    TEST_ASSERT_EQUAL_INT(0, fclose(f));

    make_cmd(&cmds[0], argv, path);
    TEST_ASSERT_EQUAL_INT(0, execute_line(cmds, 1));

    read_all(path, got, sizeof got);
    TEST_ASSERT_EQUAL_STRING("hi\n", got);      /* 只剩新内容 ⇒ O_TRUNC 生效 */
    unlink(path);
}

/* ---------- 管道（+ 重定向收尾，别把子进程输出喷到测试输出里）---------- */

void test_pipe_connects_two_commands(void)
{
    char path[64];
    char got[64];
    char* a[] = {"echo", "hi", NULL};
    char* b[] = {"wc", "-c", NULL};
    struct command cmds[2];

    make_tmp(path, sizeof path);
    make_cmd(&cmds[0], a, NULL);        /* echo hi → 管道 */
    make_cmd(&cmds[1], b, path);        /* wc -c ← 管道，结果写文件 */

    TEST_ASSERT_EQUAL_INT(0, execute_line(cmds, 2));

    read_all(path, got, sizeof got);
    TEST_ASSERT_EQUAL_STRING("3\n", got);   /* "hi\n" 是 3 字节 */
    unlink(path);
}

/* 验收命令的形状：ls | wc -l > 文件（不经过 shell 二进制，直接用函数跑） */
void test_acceptance_shape_ls_wc_l(void)
{
    char path[64];
    char got[64];
    char* a[] = {"ls", NULL};
    char* b[] = {"wc", "-l", NULL};
    struct command cmds[2];

    make_tmp(path, sizeof path);
    make_cmd(&cmds[0], a, NULL);
    make_cmd(&cmds[1], b, path);

    TEST_ASSERT_EQUAL_INT(0, execute_line(cmds, 2));

    read_all(path, got, sizeof got);
    TEST_ASSERT_TRUE_MESSAGE(strtol(got, NULL, 10) > 0, "ls | wc -l expected at least 1 line");
    unlink(path);
}

/* ">" 覆盖 "|"：同一个命令既有管道又有 outfile 时，outfile 后 dup2 赢 */
void test_outfile_overrides_pipe(void)
{
    char path[64];
    char got[64];
    char* a[] = {"echo", "hi", NULL};
    char* b[] = {"cat", NULL};
    struct command cmds[2];

    make_tmp(path, sizeof path);
    make_cmd(&cmds[0], a, path);        /* 既有 → 管道，又有 > path */
    make_cmd(&cmds[1], b, NULL);        /* cat 只能读到 EOF，什么都不输出 */

    TEST_ASSERT_EQUAL_INT(0, execute_line(cmds, 2));

    read_all(path, got, sizeof got);
    TEST_ASSERT_EQUAL_STRING("hi\n", got);  /* 去了文件，管道里没留下东西 */
    unlink(path);
}

/* ---------- 返回值语义 ---------- */

/* 现状（不是需求）：waitpid 的 status 拿到手就没再看，所以子进程失败也返回 0。
   真 shell 要把 `$?` 传出去；等哪天改成传播退出码，改这条断言。 */
void test_child_exit_status_is_ignored_today(void)
{
    char* argv[] = {"false", NULL};
    struct command cmds[1];

    make_cmd(&cmds[0], argv, NULL);
    TEST_ASSERT_EQUAL_INT(0, execute_line(cmds, 1));
}

/* ---------- 主入口 ---------- */

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_zero_or_negative_ncmds_is_noop);
    RUN_TEST(test_empty_command_rejected_before_any_fork);
    RUN_TEST(test_redirect_writes_and_truncates);
    RUN_TEST(test_pipe_connects_two_commands);
    RUN_TEST(test_acceptance_shape_ls_wc_l);
    RUN_TEST(test_outfile_overrides_pipe);
    RUN_TEST(test_child_exit_status_is_ignored_today);
    return UNITY_END();
}
