#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <sys/wait.h>
#include "parse_line.h"
#include <fcntl.h>
#include "execute_line.h"
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
int execute_line(const struct command cmds[], int ncmds) {
    if (ncmds <= 0) return 0;

    // 检查空命令
    for (int i = 0; i < ncmds; i++) {
        if (cmds[i].argc == 0) {
            fprintf(stderr, "syntax error: empty command.\n");
            return 1;
        }
    }

    int pipes[MAX_CMDS - 1][2];
    if (ncmds > MAX_CMDS) {
        fprintf(stderr, "too many commands.\n");
        return 1;
    }
    // 1. create ncmds - 1  pipe
    for (int i = 0; i < ncmds - 1; i++) {
        if (pipe(pipes[i]) < 0) {
            perror("pipe");
            return 1;
        }
    }

    pid_t pids[MAX_CMDS];
    // 2. fork
    for (int i = 0; i < ncmds; i++) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            return 1;
        }

        if (pid == 0) {
            // child process
            if (i > 0) {
                // not the first one
                // accept last read
                if (dup2(pipes[i - 1][0], STDIN_FILENO) < 0) {
                    perror("dup2");
                    _exit(1);
                }
            }

            if (i < ncmds - 1) {
                // not the last one
                // lead to next write
                if (dup2(pipes[i][1], STDOUT_FILENO) < 0) {
                    perror("dup2");
                    _exit(1);
                }
            }

            // output > will cover pipe |
            if (cmds[i].outfile != NULL) {
                int fd = open(cmds[i].outfile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (fd < 0) {
                    perror("open");
                    _exit(1);
                }
                if (dup2(fd, STDOUT_FILENO) < 0) {
                    perror("dup2");
                    _exit(1);
                }
                close(fd);
            }
            // close all pipe
            for (int j = 0; j < ncmds - 1; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }

            execvp(cmds[i].argv[0], cmds[i].argv);
            perror("execvp");
            _exit(127);
        }
        // father process
        pids[i] = pid;
    }

    for (int i = 0; i < ncmds - 1; i++) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }

    for (int i = 0; i < ncmds; i++) {
        int status;
        while (waitpid(pids[i], &status, 0) < 0) {
            if (errno == EINTR) continue;
            perror("waitpid");
            return 1;
        }
    }
    return 0;
}
