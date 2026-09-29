#pragma once

int parseline(char* buf, char** argv);
int builtin_command(char** argv);
void eval(char* cmdline);

