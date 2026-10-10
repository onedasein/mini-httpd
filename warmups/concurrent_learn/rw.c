#include <semaphore.h>
int readcnt;
sem_t mutex, w;

void reader(void) {
    while (1) {
        sem_wait(&mutex);
        readcnt++;
        if (readcnt == 1) {
            sem_wait(&w);
        }
        sem_post(&mutex);

        sem_wait(&mutex);
        readcnt--;
        if (readcnt == 0) sem_post(&w);
        sem_post(&mutex);
    }
}

void writer(void) {
    while (1) {
        sem_wait(&w);

        sem_post(&w);
    }
}
