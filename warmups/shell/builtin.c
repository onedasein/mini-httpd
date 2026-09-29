// 内建命令: quit/cd... (argv)
// argv ->  副作用
// 对外接口
// int run_builtin(struct cmd* cmd);      // 1 = 已处理， don't fork    
#include "shell.h"
#include <string.h>
#include <stdlib.h> // exit
int builtin_command(char** argv) {
    if (!strcmp(argv[0], "quit"))    // quit command
        exit(0);
    if (!strcmp(argv[0], "&"))      // Ignore singleton &
        return 1;
    return 0;                       // Not a builtin command
}