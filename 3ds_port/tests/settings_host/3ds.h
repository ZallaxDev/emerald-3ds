/* Host synchronization double for the production settings worker. */
#ifndef SETTINGS_HOST_3DS_H
#define SETTINGS_HOST_3DS_H
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
typedef int32_t s32;
typedef int LightLock;
typedef struct { pthread_mutex_t mutex; pthread_cond_t cond; bool signalled; } LightEvent;
typedef struct { pthread_t thread; void (*fn)(void *); void *arg; } *Thread;
#define RESET_ONESHOT 0
#define CUR_THREAD_HANDLE 0
#define U64_MAX UINT64_MAX
static pthread_mutex_t testLock = PTHREAD_MUTEX_INITIALIZER;
static void LightLock_Lock(LightLock *p) { (void)p; pthread_mutex_lock(&testLock); }
static void LightLock_Unlock(LightLock *p) { (void)p; pthread_mutex_unlock(&testLock); }
static void LightEvent_Init(LightEvent *e, int mode) {
    (void)mode; pthread_mutex_init(&e->mutex, NULL); pthread_cond_init(&e->cond, NULL); e->signalled = false;
}
static void LightEvent_Wait(LightEvent *e) {
    pthread_mutex_lock(&e->mutex);
    while (!e->signalled) pthread_cond_wait(&e->cond, &e->mutex);
    e->signalled = false; pthread_mutex_unlock(&e->mutex);
}
static void LightEvent_Signal(LightEvent *e) {
    pthread_mutex_lock(&e->mutex); e->signalled = true;
    pthread_cond_signal(&e->cond); pthread_mutex_unlock(&e->mutex);
}
static void svcGetThreadPriority(s32 *p, int handle) { (void)handle; *p = 0x30; }
/* SHADOW QUALITY's default: an Old 3DS on the host. */
#define R_FAILED(r) ((r) < 0)
static s32 APT_CheckNew3DS(bool *out) { *out = false; return 0; }
static void *hostThread(void *p) { Thread t = p; t->fn(t->arg); return NULL; }
static Thread threadCreate(void (*fn)(void *), void *arg, size_t stack, int priority, int cpu, bool detached) {
    (void)stack; (void)priority; (void)cpu;
    if (detached) abort();
    Thread t = malloc(sizeof(*t)); t->fn = fn; t->arg = arg;
    if (pthread_create(&t->thread, NULL, hostThread, t) != 0) { free(t); return NULL; }
    return t;
}
static void threadJoin(Thread t, uint64_t timeout) { (void)timeout; pthread_join(t->thread, NULL); }
static void threadFree(Thread t) { free(t); }
#endif
