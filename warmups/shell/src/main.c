#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <stdio.h>
#include <stddef.h>
#include "parse_line.h"
#include "execute_line.h"
#define MAX_CMDS 32
int main(int argc, char* argv[]) {
    struct command cmds[MAX_CMDS];
    int ncmds;

    char* line = NULL;
    size_t cap = 0;

    while (1) {
        printf(">");
        fflush(stdout);

        ssize_t n = getline(&line, &cap, stdin);
        if (n < 0) break;

        parse_line(line, cmds, &ncmds);

        execute_line(cmds, ncmds);
    }

    free(line);
}
