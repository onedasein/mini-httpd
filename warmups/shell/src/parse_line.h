#pragma once
#define MAX_ARGS 128
#define MAX_CMDS 32
struct command {
    char* argv[MAX_ARGS];
    int argc;
    char* outfile;
};

int parse_line(char* line, struct command cmds[], int* ncmds);
