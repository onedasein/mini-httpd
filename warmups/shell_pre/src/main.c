// fgets && call eval_job
#include "shell.h"
#include <stdio.h>
#include <stdlib.h>
int main() {
    char cmdline[MAXLINE]; // Command line

    while (1) {
        // Read
        printf(">");
        // 只看 fgets 的返回值：命中 EOF 时它会把"已经读到的那一行"照样返回，
        // 而此刻 feof 已经为真 —— 先判 feof 会把「末行没有换行符」的命令丢掉。
        if (fgets(cmdline, MAXLINE, stdin) == NULL) break;

        // evaluate
        eval(cmdline);
    }
    return 0;
}
