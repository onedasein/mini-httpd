#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
void* thread(void* vargp);

int main() {
    pthread_t tid;
    pthread_create(&tid, NULL, thread, NULL);
    pthread_join(tid, NULL);
    exit(0);
}
void* thread(void* vargp) { // thread routine
    printf("Hello, world!\n");
    return NULL;
}
