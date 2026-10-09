#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <stdio.h>
#include <stddef.h>
#include "parse_line.h"
#include "execute_line.h"
int main(int argc, char* argv[]) {
    struct command cmds[MAX_CMDS];
    int ncmds = 0;

    char* line = NULL;
    size_t cap = 0;

    while (1) {
        printf(">");
        fflush(stdout);

        ssize_t n = getline(&line, &cap, stdin);
        if (n < 0) break;

        if (parse_line(line, cmds, &ncmds) != 0) {
            fprintf(stderr, "parse failed, line ignored.\n");
            continue;
        }

        if (execute_line(cmds, ncmds) != 0) {
            fprintf(stderr, "execute failed, try again.\n");
            continue;
        }
    }

    free(line);
}
