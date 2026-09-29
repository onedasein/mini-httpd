#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#define MAXARGS 128
#define MAXLINE 1024
// eval - Evaluate a command line
void eval(char* cmdline) {
    char* argv[MAXARGS]; // Argument list execve()
    char buf[MAXLINE];   // Holds modified command line
    int bg;              // Should the job run in bg or fg?
    pid_t pid;           // Process id

    strcpy(buf, cmdline);
    bg = parseline(buf, argv);
    if (argv[0] == NULL) return;         // Ignore empty lines
    if (!builtin_command(argv)) {
        if ((pid = fork()) == 0) { // Child runs user job
            if (execve(argv[0], argv, environ) < 0) {
                printf("%s: Command not found.\n", argv[0]);
            }
        }

        // Parent waits for foreground job to terminate
        if (!bg) {
            int status;
            if (waitpid(pid, &status, 0) < 0) unix_error("waitfg: waitpid error");
        } else
            printf("%d %s", pid, cmdline);
    }
    return;
}

int parseline(char* buf, char** argv) {
    char* delim;                       // Points to first space delimiter
    int argc;                          // Number of args

    buf[strlen(buf) - 1] = ' ';        // Replace trailing '\n' with space
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

    if ((bg = (*argv[argc - 1] == '&')) != 0) argv[--argc] = NULL;

    return bg;
}

int builtin_command(char** argv) {
    if (!strcmp(argv[0], "quit"))    // quit command
        exit(0);
    if (!strcmp(argv[0], "&"))      // Ignore singleton &
        return 1;
    return 0;                       // Not a builtin command
}
