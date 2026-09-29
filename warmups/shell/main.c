// fgets && call eval_job
#include "shell.h"
#include <stdio.h>
#include <stdlib.h>
int main() {
    char cmdline[MAXLINE]; // Command line

    while (1) {
        // Read
        printf(">");
        fgets(cmdline, MAXLINE, stdin);
        if (feof(stdin)) exit(0);

        // evaluate
        eval(cmdline);
    }
}
