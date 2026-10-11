/*
 * cv 版实现 —— 【这份留给你写】
 *
 * 需要实现 pc.h 里的 6 个函数：
 *   pc_t_init / pc_t_destroy / pc_t_close / pc_t_insert / pc_t_remove / pc_t_size
 *
 * 现在这个文件里没有任何函数定义，所以 `make` 会在链接期报
 *   undefined reference to `pc_t_init'
 * —— 那是**预期状态**，不是 Makefile 坏了。
 *
 * ── 变异开关（只有 `make mutation-check` 会打开，正常运行不要开）──────────
 * Makefile 会分别用 -DPC_BUG_IF / -DPC_BUG_SIGNAL 编译这个文件两次，
 * 目的是验证「用例真的抓得住这两个经典 bug」。所以实现必须让这两个开关真的改变行为：
 *
 *   PC_BUG_IF      把 insert/remove 里等条件的 while 换成 if（经典错误）
 *                  形如：
 *                      #ifdef PC_BUG_IF
 *                          if (q->count == 0) pthread_cond_wait(&q->not_empty, &q->lock);
 *                      #else
 *                          while (q->count == 0) pthread_cond_wait(&q->not_empty, &q->lock);
 *                      #endif
 *
 *   PC_BUG_SIGNAL  把 close() 里的 broadcast 换成 signal（经典错误）
 *
 * 判据是「变体必须红」：如果开了开关反而全绿，说明用例无效。
 * ────────────────────────────────────────────────────────────────────────
 */
#include "pc.h"
#include <stdlib.h>
#include <stdio.h>
int pc_t_init(pc_t* q, int n) {
    if (n <= 0) return -1;
    q->buf = (int*)calloc((size_t)n, sizeof(int));
    if (q->buf == NULL) {
        fprintf(stderr, "calloc 失败\n");
        free(q->buf);
        return -1;
    }
    q->n = n;
    q->front = q->rear = q->count = q->closed = 0;
    if (pthread_mutex_init(&q->lock, NULL) != 0) {
        free(q->buf);
        q->buf = NULL;
        return -1;
    }
    if (pthread_cond_init(&q->not_empty, NULL) != 0) {
        pthread_mutex_destroy(&q->lock);
        free(q->buf);
        q->buf = NULL;
        return -1;
    }
    if (pthread_cond_init(&q->not_full, NULL) != 0) {
        pthread_mutex_destroy(&q->lock);
        pthread_cond_destroy(&q->not_empty);
        free(q->buf);
        q->buf = NULL;
        return -1;
    }
    return 0;
}       /* -1：n <= 0 或初始化失败 */
void pc_t_destroy(pc_t* q) {
    if (q == NULL) return;

    pthread_cond_destroy(&q->not_empty);
    pthread_cond_destroy(&q->not_full);
    pthread_mutex_destroy(&q->lock);
    free(q->buf);
    q->buf = NULL;
    q->n = q->front = q->rear = q->count = q->closed = 0;
}             /* 契约：所有线程都退出后才调用 */
int pc_t_close(pc_t* q) {
    if (q == NULL) return -1;
    if (pthread_mutex_lock(&q->lock) != 0) {
        return -1;
    }
    q->closed = 1;
    pthread_cond_broadcast(&q->not_empty);
    pthread_cond_broadcast(&q->not_full);
    pthread_mutex_unlock(&q->lock);
    return 0;
}               /* 幂等；必须唤醒所有等待者（含阻塞的生产者） */
int pc_t_insert(pc_t* q, int item) {
    pthread_mutex_lock(&q->lock);
    while (q->count == q->n && !q->closed) {
        pthread_cond_wait(&q->not_full, &q->lock);
    }
    if (q->closed) {
        pthread_mutex_unlock(&q->lock);
        return -1;
    }
    q->buf[q->rear] = item;
    q->rear = (q->rear + 1) % q->n;
    q->count++;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->lock);
    return 0;
}   /* 满则阻塞；-1：已关闭 */
int pc_t_remove(pc_t* q, int* out) {
    pthread_mutex_lock(&q->lock);
    while (q->count == 0 && !q->closed) {
        pthread_cond_wait(&q->not_empty, &q->lock);
    }
    if (q->count == 0 && q->closed) {
        pthread_mutex_unlock(&q->lock);
        return -1;
    }
    *out = q->buf[q->front];
    q->front = (q->front + 1) % q->n;
    q->count--;
    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->lock);
    return 0;
}   /* 空则阻塞；-1：已关闭且已排空 */
int pc_t_size(pc_t* q) {
    pthread_mutex_lock(&q->lock);
    int count = q->count;
    pthread_mutex_unlock(&q->lock);
    return count;
}               /* 只为测试/断言用；必须加锁读 count */
