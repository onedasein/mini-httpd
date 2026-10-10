#include <stdlib.h>
#include "sbuf.h"
void sbuf_init(sbuf_t *sp, int n) {
    sp->buf = calloc(n, sizeof(int));
    sp->n = n;
    sp->front = sp->rear = 0;
    sem_init(&sp->mutex, 0, 1);
    sem_init(&sp->slots, 0, n);
    sem_init(&sp->items, 0, 0);
}
void sbuf_deinit(sbuf_t *sp) {
    free(sp->buf);
}
void sbuf_insert(sbuf_t *sp, int item) {
    sem_wait(&sp->slots); // wait for available slots
    sem_wait(&sp->mutex); // lock the buffer
    sp->buf[(++sp->rear) % (sp->n)] = item; // insert the item
    sem_post(&sp->mutex); // unlock the buffer
    sem_post(&sp->items); // announce available item
}
int sbuf_remove(sbuf_t *sp) {
    int item;
    sem_wait(&sp->items);
    sem_wait(&sp->mutex);
    item = sp->buf[(++sp->front) % (sp->n)];
    sem_post(&sp->mutex);
    sem_post(&sp->slots);
    return item;
}
