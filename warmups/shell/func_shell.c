#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#define MAXARGS 128
#define MAXLINE 1024

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

int builtin_command(char** argv) {
    if (!strcmp(argv[0], "quit"))    // quit command
        exit(0);
    if (!strcmp(argv[0], "&"))      // Ignore singleton &
        return 1;
    return 0;                       // Not a builtin command
}
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
            if (execvp(argv[0], argv) < 0) {
                printf("%s: Command not found.\n", argv[0]);
                exit(1);
            }
        }

        // Parent waits for foreground job to terminate
        if (!bg) {
            int status;
            if (waitpid(pid, &status, 0) < 0) perror("waitfg: waitpid error");
        } else
            printf("%d %s", (int)pid, cmdline);
    }
    return;
}


