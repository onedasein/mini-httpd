#define _POSIX_C_SOURCE 200809L
#include "stdio.h"
#include "parse_line.h"
#include "string.h"
int parse_line(char* line, struct command cmds[], int* ncmds) {
    int n = 0;
    struct command* cur = &cmds[0];
    cur->argc = 0;
    cur->outfile = NULL;

    char* save = NULL;
    char* tok = strtok_r(line, " \t\n", &save);

    while (tok != NULL) {
        if (strcmp(tok, "|") == 0) {
            // 当前命令结束，开始下一个
            cur->argv[cur->argc] = NULL;
            if ((n + 1) >= MAX_CMDS) {
                fprintf(stderr, "too many commands\n");
                return 1;
            }
            n++;
            cur = &cmds[n];
            cur->argc = 0;
            cur->outfile = NULL;
        } else if (strcmp(tok, ">") == 0) {
            // 下一个token是输出文件
            tok = strtok_r(NULL, " \t\n", &save);
            if (tok == NULL) {
                fprintf(stderr, "syntax error: expected file after >\n");
                return 1;
            }
            cur->outfile = tok;
        } else {
            // 一般情况
            if (cur->argc >= MAX_ARGS - 1) {
                fprintf(stderr, "too many arguments\n");
                return 1;
            }
            cur->argv[cur->argc++] = tok;
        }
        tok = strtok_r(NULL, " \t\n", &save);
    }
    cur->argv[cur->argc] = NULL;
    n++;
    *ncmds = n;
    return 0;
}
