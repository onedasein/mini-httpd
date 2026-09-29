// 纯字符串：分词、拆管道、摘重定向
// string -> struct job
// 对外接口 int parse_job(char *line, struct job *job)
#include "shell.h"
#include <stddef.h> // size_t exit
#include <string.h> // str* strcmp
int parseline(char* buf, char** argv) {
    char* delim;                       // Points to first space delimiter
    int argc;                          // Number of args
    int bg;

    size_t len = strlen(buf);
    if (len > 0 && buf[len-1] == '\n') buf[len-1] = ' ';        // Replace trailing '\n' with space
    while (*buf && (*buf == ' ')) buf++;

    // Build the argv list
    argc = 0;
    while ((delim = strchr(buf, ' '))) {
        argv[argc++] = buf;
        *delim = '\0';
        buf = delim + 1;
        while (*buf && (*buf == ' ')) // Ignore spaces
            buf++;
    }
    argv[argc] = NULL;

    if (argc == 0)                  // Ignore blank line
        return 1;

    if (!strcmp(argv[argc-1], "&")) {
        argv[--argc] = NULL;
        bg = 1;
    } else
        bg = 0;
    return bg;
}