#pragma once
#include <semaphore.h>
typedef struct {
    int* buf;
    int n;
    int front;
    int rear;
    sem_t mutex;
    sem_t slots;
    sem_t items;
} pc_t;

void pc_t_init(pc_t* q, int n);
void pc_t_deinit(pc_t* q);
void pc_t_insert(pc_t* q, int item);
int pc_t_remove(pc_t* q);
