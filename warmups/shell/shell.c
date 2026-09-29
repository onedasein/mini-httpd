#include "shell.h"
#include <stdio.h>
#include <stdlib.h>
#define MAXLINE 1024
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
