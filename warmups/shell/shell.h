#pragma once
#include <sys/types.h>

#define MAXLINE 1024
#define MAXARGS 128
#define MAXCMDS 16          /* 一条命令行最多 16 段管道 */

struct cmd {
    char *argv[MAXARGS];    /* NULL 结尾，直接喂 execvp */
    int   argc;
    char *infile;           /* 指向 job->line 内部；NULL = 无 */
    char *outfile;          /* NULL = 无 */
    int   append;           /* 1 = >> ，0 = > */
};

struct job {
    char  line[MAXLINE];    /* 原始行副本，错误信息/打印用 */
    struct cmd cmds[MAXCMDS];
    int   ncmds;            /* 1 = 无管道 */
    int   bg;
    pid_t pids[MAXCMDS];
    pid_t pgid;             /* 全部子进程同一个进程组，作业控制要用 */
};
// new interface
int parse_job(char* line, struct job* job); // 0 = 成功，-1 = 失败, 1 = 空行
void eval_job(struct job* job);
int exec_pipeline(struct job* job);
int run_builtin(struct cmd* cmd);      // 1 = 已处理， don't fork

// old function prototypes
int parseline(char* buf, char** argv);
int builtin_command(char** argv);
void eval(char* cmdline);

