#pragma once
#define MAX_ARGS 128
struct command {
    char* argv[MAX_ARGS];
    int argc;
    char* outfile;
};

int parse_line(char* line, struct command cmds[], int* ncmds);
