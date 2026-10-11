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
