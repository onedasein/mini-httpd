#pragma once
/*
 * 条件变量版生产者-消费者队列（T0.5 的正式验收目标）
 *
 * 这一份是「接口」，字段你可以按自己的设计改，但**下面 6 个函数签名不要动**：
 * main.c 与两份测试都依赖它们。
 *
 * 与信号量版（../src/pc.h）的关键差别：
 *   1. 谓词（空/满/已关闭）必须由你在**持锁前提下**用 while 重新判断 —— 信号量把它藏进计数器了
 *   2. 停机从「毒丸值」换成 close()：置 closed + 唤醒**所有**等待者
 *   3. 因此 insert/remove 都能返回失败，不用再靠 -1 这类哨兵值和合法数据撞车
 *   4. init 能返回错误，n <= 0 终于可以被拒绝
 */
#include <pthread.h>

typedef struct {
    int* buf;
    int  n;         /* 容量 */
    int  front;     /* 下一个出队位置（环形索引） */
    int  rear;      /* 下一个入队位置（环形索引） */
    int  count;     /* 当前元素个数 —— 所有谓词的基础 */
    int  closed;    /* 关闭标志：插入一律拒绝；取出把已有元素排空后才返回失败 */
    pthread_mutex_t lock;
    pthread_cond_t  not_empty;  /* 队列非空（消费者等这个） */
    pthread_cond_t  not_full;   /* 队列未满（生产者等这个） */
} pc_t;

/* 全部失败返回 -1、成功返回 0；被测函数里绝不要 exit() */
int  pc_t_init(pc_t* q, int n);         /* -1：n <= 0 或初始化失败 */
void pc_t_destroy(pc_t* q);             /* 契约：所有线程都退出后才调用 */
int  pc_t_close(pc_t* q);               /* 幂等；必须唤醒所有等待者（含阻塞的生产者） */
int  pc_t_insert(pc_t* q, int item);    /* 满则阻塞；-1：已关闭 */
int  pc_t_remove(pc_t* q, int* out);    /* 空则阻塞；-1：已关闭且已排空 */
int  pc_t_size(pc_t* q);                /* 只为测试/断言用；必须加锁读 count */
