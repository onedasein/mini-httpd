#pragma once
#include <pthread.h>
#include <stddef.h>

typedef struct {
    int *buf;
    size_t cap, head, tail, count;
    int closed;
    pthread_mutex_t lock;
    pthread_cond_t not_empty, not_full;
} pc_queue;

int pc_queue_init(pc_queue *q, size_t cap);   /* 0 / -1：cap==0 直接拒 */
int pc_queue_close(pc_queue *q);              /* 幂等；broadcast 唤醒所有人 */
void pc_queue_destroy(pc_queue *q); /* 只释放，不负责唤醒（契约：调用时不能有等待者） */
int pc_queue_push(pc_queue *q, int v); /* 满则阻塞；已关闭 → -1 */
int pc_queue_pop(pc_queue *q, int *out); /* 空则阻塞；已关闭且排空 → -1 */
int pc_queue_try_pop(pc_queue *q, int *out); /* 非阻塞：空 → -1 */
