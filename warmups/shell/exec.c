/*
纯进程/fd: fork链、dup2、waitpid
struct job -> 进程
对外接口
void eval_job(struct job* job);
int exec_pipeline(struct job* job);
static void apply_redirects(const struct cmd* cmd);
*/
// eval - Evaluate a command line
#include "shell.h"
#include <stdio.h>           
#include <stdlib.h>          // exit
#include <string.h>          // strcpy
#include <unistd.h>          // fork, execvp
#include <sys/wait.h>        // waitpid

// static void apply_redirects(const struct cmd* cmd);

void eval(char* cmdline) {
    char* argv[MAXARGS];    // Argument list execve()
    char buf[MAXLINE];      // Holds modified command line
    int bg;                 // Should the job run in bg or fg?
    pid_t pid;              // Process id

    strcpy(buf, cmdline);
    bg = parseline(buf, argv);
    if (argv[0] == NULL) return;         // Ignore empty lines
    if (!builtin_command(argv)) {
        if ((pid = fork()) == 0) {      // Child runs user job
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