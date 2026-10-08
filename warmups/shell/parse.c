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
    // ★ 别依赖"末尾一定有分隔符"：末行没换行符时，最后一个 token 后面什么都没有，
    //   老写法（只认 strchr 找到的空格）会把它整段吞掉（`echo tail` 变成零参 echo）。
    argc = 0;
    while (*buf && argc < MAXARGS - 1) {    // 留一格给 argv[argc] = NULL
        while (*buf == ' ') buf++;          // 跳过连续空格
        if (!*buf) break;
        argv[argc++] = buf;
        delim = strchr(buf, ' ');
        if (!delim) break;                  // 最后一个 token：没有分隔符，到此为止
        *delim = '\0';
        buf = delim + 1;
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