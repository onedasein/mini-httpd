#pragma once

void eval(char* cmdline);
int parseline(char* buf, char** argv);
int builtin_command(char** argv);
