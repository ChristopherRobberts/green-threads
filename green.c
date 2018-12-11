#include <stdlib.h>
#include <stdio.h>
#include <ucontext.h>
#include <assert.h>
#include <signal.h>
#include <sys/time.h>

#include "green.h"

#define FALSE 0
#define TRUE 1
#define PERIOD 1

#define STACK_SIZE 4096

static sigset_t block;
static ucontext_t main_cntx = {0};
static struct green_thread main_green = {&main_cntx, NULL, NULL, NULL, NULL, FALSE};
static struct green_thread *running = &main_green;
static void init () __attribute__ ((constructor));

struct green_cond_t cond;
struct green_mutex_t lock;
int counter = 0;

static struct green_thread *queue = NULL;

void timer_handler(int);

int flag = 0;

void enqueue(struct green_thread *node) {
	if(queue==NULL) {
		queue = node;
	} else if(queue->next == NULL){
		queue->next = node;
	} else {
		struct green_thread *temp = queue;

		while(temp->next != NULL){
			temp = temp->next;
        }

		temp->next = node;
    }
}

struct green_thread* dequeue(){
	struct green_thread* next = queue;
	queue = next->next;
	next->next = NULL;
	return next;
}

void init() {
    getcontext(&main_cntx);

    sigemptyset(&block);
    sigaddset(&block, SIGVTALRM);

    struct sigaction act = {0};
    struct timeval interval;
    struct itimerval period;

    act.sa_handler = timer_handler;
    assert(sigaction(SIGVTALRM, &act, NULL) == 0);

    interval.tv_sec = 0;
    interval.tv_usec = PERIOD;
    period.it_interval = interval;
    period.it_value = interval;
    setitimer(ITIMER_VIRTUAL, &period, NULL);
}

void timer_handler(int sig) {
    sigprocmask(SIG_BLOCK, &block, NULL);

    struct green_thread *susp = running;
    enqueue(susp);
    struct green_thread *next = dequeue();
    running = next;
    swapcontext(susp->context, next->context);

    sigprocmask(SIG_UNBLOCK, &block, NULL);
}

int green_mutex_init(struct green_mutex_t *mutex) {
    mutex->taken = FALSE;
    mutex->susp = NULL;
}

int green_mutex_lock(struct green_mutex_t *mutex) {
    sigprocmask(SIG_BLOCK, &block, NULL);

    struct green_thread *susp = running;
    while(mutex->taken) {
        if (mutex->susp == NULL) {
            mutex->susp = susp;
        } else {
            struct green_thread *tmp = mutex->susp;
            while (tmp->next != NULL) {
                tmp = tmp->next;
            }

            tmp->next = susp;
        }

        struct green_thread *next = dequeue();
        running = next;
        swapcontext(susp->context, next->context);
    }

    mutex->taken = TRUE;
    sigprocmask(SIG_UNBLOCK, &block, NULL);
    return 0;
}

int green_mutex_unlock(struct green_mutex_t *mutex) {
    sigprocmask(SIG_BLOCK, &block, NULL);
    enqueue(mutex->susp);
    mutex->susp = NULL;
    mutex->taken = FALSE;
    sigprocmask(SIG_UNBLOCK, &block, NULL);
    return 0;
}

void green_cond_init(struct green_cond_t *condition) {
    struct green_cond_t *cond = {NULL};
    condition = cond;
}
void green_cond_wait(struct green_cond_t *condition) {
    sigprocmask(SIG_BLOCK, &block, NULL);

    struct green_thread *susp = running;
    if (condition->suspended == NULL) {
        condition->suspended = susp;
    } else {
        struct green_thread *curr = condition->suspended;
        while(curr->next != NULL) {
            curr = curr->next;
        }
        curr->next = susp;
    }

    struct green_thread *next = dequeue();
    running = next;
    swapcontext(susp->context, next->context);
    sigprocmask(SIG_UNBLOCK, &block, NULL);
}

int green_cond_wait_atomic(struct green_cond_t *condition, struct green_mutex_t *mutex) {
    sigprocmask(SIG_BLOCK, &block, NULL);

    struct green_thread *susp = running;
    if (condition->suspended == NULL) {
        condition->suspended = susp;
    } else {
        struct green_thread *curr = condition->suspended;
        while(curr->next != NULL) {
            curr = curr->next;
        }
        curr->next = susp;
    }

    if (mutex != NULL) {
        enqueue(mutex->susp);
        mutex->susp = NULL;
        mutex->taken = FALSE;
    }

    struct green_thread *next = dequeue();
    running = next;
    swapcontext(susp->context, next->context);

    if (mutex != NULL) {
        while(mutex->taken) {
            if (mutex->susp == NULL) {
                mutex->susp = susp;
            } else {
                struct green_thread *tmp = mutex->susp;
                while (tmp->next != NULL) {
                    tmp = tmp->next;
                }

                tmp->next = susp;
            }
        }
        mutex->taken = TRUE;
    }
    sigprocmask(SIG_UNBLOCK, &block, NULL);
    return 0;
}

void green_cond_signal(struct green_cond_t *condition) {
    sigprocmask(SIG_BLOCK, &block, NULL);

    struct green_thread *next;
    if (condition->suspended != NULL) {
        next = condition->suspended;
        condition->suspended = condition->suspended->next;
        next->next = NULL;
    } else {
        next = NULL;
    }
    enqueue(next);

    sigprocmask(SIG_UNBLOCK, &block, NULL);
}

void green_thread_funct() {
    sigprocmask(SIG_UNBLOCK, &block, NULL);

    struct green_thread *this = running;
    (*this->fun)(this->arg);

    sigprocmask(SIG_BLOCK, &block, NULL);

    enqueue(this->join);
    free(this->context);
    this->zombie = TRUE;
    struct green_thread *next = dequeue();

    running = next;
    setcontext(next->context);
}

int green_create(struct green_thread *new, void*(*fun)(void*), void*arg) {
    ucontext_t *cntx = (ucontext_t *)malloc(sizeof(ucontext_t));
    getcontext(cntx);

    void *stack = malloc(STACK_SIZE);

    cntx->uc_stack.ss_sp = stack;
    cntx->uc_stack.ss_size = STACK_SIZE;


    makecontext(cntx, green_thread_funct, 0);

    new->context = cntx;
    new->fun = fun;
    new->arg = arg;
    new->next = NULL;
    new->join = NULL;
    new->zombie = FALSE;
    sigprocmask(SIG_BLOCK, &block, NULL);
    enqueue(new);
    sigprocmask(SIG_UNBLOCK, &block, NULL);
    return 0;
}

int green_yield() {
    sigprocmask(SIG_BLOCK, &block, NULL);
    struct green_thread *susp = running;
    enqueue(susp);
    struct green_thread *next = dequeue();
    running = next;
    swapcontext(susp->context, next->context);
    sigprocmask(SIG_UNBLOCK, &block, NULL);
    return 0;
}

int green_join(struct green_thread *thread) {
    if (thread->zombie) {
        return 0;
    }

    struct green_thread *susp = running;
    sigprocmask(SIG_BLOCK, &block, NULL);

    if (thread->join != NULL) {
        struct green_thread *tmp = thread->join;
        while(tmp->next != NULL) {
            tmp = tmp->next;
        }

        tmp->next = susp;
    } else {
        thread->join = susp;
    }

    struct green_thread *next = dequeue();
    running = next;
    swapcontext(susp->context, next->context);
    sigprocmask(SIG_UNBLOCK, &block, NULL);
    return 0;
}

void *test_condition(void *arg) {
    int id = *(int*)arg;
    int loop = 40000;

    while(loop > 0) {
        if (flag == id) {
            counter++;
            printf("thread %d: %d\t counter: %d\n", id, loop, counter);
            loop--;
            flag = (id + 1) % 4;
            green_cond_signal(&cond);
        } else {
            green_cond_signal(&cond);
            green_cond_wait(&cond);
        }
    }
}

void *test_mutex(void *arg) {
    int i = 400000;
    //green_mutex_lock(&lock);
    while(i > 0) {
        green_mutex_lock(&lock);
        printf("thread %d: counter: %d\n", *(int*)arg, counter);
        counter++;
        i--;
        green_mutex_unlock(&lock);
    }
    printf("counter: %d\n", counter);
}

void *test_atomic_lock(void *arg) {
    green_mutex_lock(&lock);
    while(1) {
        printf("thread: %d\n", *(int*)arg);
        if (flag == 0) {
            flag = 1;
            green_cond_signal(&cond);
            green_mutex_unlock(&lock);
            break;
        } else {
            printf("asd\n");
            green_mutex_unlock(&lock);
            green_cond_wait_atomic(&cond, &lock);
        }
    }
}

int main() {
    //green_cond_init(&cond);
    green_mutex_init(&lock);
    int a0 = 0, a1 = 1, a2 = 2, a3 = 3;
    struct green_thread g0, g1, g2, g3;
    green_create(&g0, test_atomic_lock, &a0);
    green_create(&g1, test_atomic_lock, &a1);
    green_create(&g2, test_atomic_lock, &a2);
    green_create(&g3, test_atomic_lock, &a3);

    green_join(&g0);
    green_join(&g1);
    green_join(&g2);
    green_join(&g3);

    return 0;
}
