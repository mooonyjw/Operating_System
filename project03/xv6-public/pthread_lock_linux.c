#include <stdio.h>
#include <pthread.h>

int shared_resource = 0;

#define NUM_ITERS 1000
#define NUM_THREADS 1000

void lock();
void unlock();

volatile int lock_flag = 0;

void lock()
{
  int expected = 1;
  while(expected != 0 ){
    __asm__ __volatile__(
      "xchg %0, %1"
      : "=r" (expected), "+m" (lock_flag)
      : "0" (expected)
      : "memory"
    );
  }
}

void unlock()
{
  __asm__ __volatile__(
    "" ::: "memory"
  );
  lock_flag = 0;
}

void* thread_func(void* arg) {
    int tid = *(int*)arg;
    
    lock();
    
        for(int i = 0; i < NUM_ITERS; i++)    shared_resource++;
    
    unlock();
    
    pthread_exit(NULL);
}

int main() {
    pthread_t threads[NUM_THREADS];
    int tids[NUM_THREADS];
    
    for (int i = 0; i < NUM_THREADS; i++) {
        tids[i] = i;
        pthread_create(&threads[i], NULL, thread_func, &tids[i]);
    }
    
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    printf("shared: %d\n", shared_resource);
    
    return 0;
}
