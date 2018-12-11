#include <ucontext.h>

struct green_thread {
    ucontext_t *context;
    void *(*fun)(void*);
    void *arg;
    struct green_thread *next;
    struct green_thread *join;
    int zombie;
};

struct green_cond_t {
    struct green_thread *suspended;
};

struct green_mutex_t {
    volatile int taken;
    struct green_thread *susp;
};

int green_create(struct green_thread *thread, void *(*fun)(void*), void *arg);
int green_yield();
int green_join(struct green_thread *thread);

void green_cond_init(struct green_cond_t* cond);
void green_cond_wait(struct green_cond_t* cond);
void green_cond_signal(struct green_cond_t* cond);

int green_mutex_init(struct green_mutex_t *mutex);
int green_mutex_lock(struct green_mutex_t *mutex);
int green_mutex_unlock(struct green_mutex_t *mutex);
