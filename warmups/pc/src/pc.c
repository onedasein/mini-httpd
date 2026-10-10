#include "pc.h"
#include <stdlib.h>

void pc_t_init(pc_t* q, int n) {
    q->buf = (int*)calloc((size_t)n, sizeof(int));
    q->n = n;
    q->front = q->rear = 0;
    sem_init(&q->mutex, 0, 1);
    sem_init(&q->slots, 0, (unsigned)n);
    sem_init(&q->items, 0, 0);
}
void pc_t_deinit(pc_t* q) {
    free(q->buf);
}
void pc_t_insert(pc_t* q, int item) {
    sem_wait(&q->slots);
    sem_wait(&q->mutex);
    q->buf[(++q->rear) % (q->n)] = item;
    sem_post(&q->mutex);
    sem_post(&q->items);
}
int pc_t_remove(pc_t* q) {
    int item;
    sem_wait(&q->items);
    sem_wait(&q->mutex);
    item = q->buf[(++q->front) % (q->n)];
    sem_post(&q->mutex);
    sem_post(&q->slots);
    return item;
}
