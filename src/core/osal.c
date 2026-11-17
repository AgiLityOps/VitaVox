#if defined(__linux__) && !defined(__vita__)
#define _GNU_SOURCE
#endif
#include "osal.h"

#include <errno.h>
#include <sys/time.h>
#include <time.h>

void cond_wait_ms(Cond *c, Mutex *m, int ms) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct timespec ts;
    long long ns = (long long)tv.tv_usec * 1000 + (long long)ms * 1000000LL;
    ts.tv_sec = tv.tv_sec + (time_t)(ns / 1000000000LL);
    ts.tv_nsec = (long)(ns % 1000000000LL);
    pthread_cond_timedwait(c, m, &ts);
}

uint64_t now_ms(void) {
#ifdef CLOCK_MONOTONIC
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
    return wall_ms();
}

uint64_t wall_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

void sleep_ms(int ms) {
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {}
}

#ifdef __vita__
#include <psp2/kernel/threadmgr.h>
#include <stdlib.h>
typedef struct { void *(*fn)(void *); void *arg; int prio; } Trampoline;
static void *trampoline(void *p) {
    Trampoline t = *(Trampoline *)p;
    free(p);
    /* relative to the default user priority (0x10000100); lower runs first */
    /* 2 audio output, 1 decode, -1 background, -2 lowest (covers, thumbs, saves) */
    int off = t.prio >= 2 ? -30 : t.prio > 0 ? -20 : t.prio <= -2 ? 40 : 20;
    sceKernelChangeThreadPriority(0, 0x10000100 + off);
    return t.fn(t.arg);
}
#endif

bool thread_start(Thread *t, void *(*fn)(void *), void *arg, int stack_kb, int prio) {
    pthread_attr_t a;
    pthread_attr_init(&a);
    if (stack_kb > 0) pthread_attr_setstacksize(&a, (size_t)stack_kb * 1024);
#ifdef __vita__
    if (prio) {
        Trampoline *tr = malloc(sizeof *tr);
        if (tr) {
            tr->fn = fn;
            tr->arg = arg;
            tr->prio = prio;
            fn = trampoline;
            arg = tr;
        }
    }
#else
    (void)prio;
#endif
    t->ok = pthread_create(&t->t, &a, fn, arg) == 0;
    pthread_attr_destroy(&a);
    return t->ok;
}

void thread_join(Thread *t) {
    if (t->ok) pthread_join(t->t, NULL);
    t->ok = false;
}

void thread_join_timeout(Thread *t, int ms) {
    if (!t || !t->ok) return;
#if defined(__linux__) && !defined(__vita__)
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) == 0) {
        ts.tv_sec += ms / 1000;
        ts.tv_nsec += (long)(ms % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        if (pthread_timedjoin_np(t->t, NULL, &ts) == 0) { t->ok = false; return; }
        pthread_detach(t->t);
        t->ok = false;
        return;
    }
#endif
#ifdef __vita__
    /* newlib pthread_join is built on sceKernelWaitThreadEnd; we cannot
     * reliably timed-join, so callers must make the thread exit quickly.
     * A shutdown watchdog in main() calls sceKernelExitProcess if we stick. */
    (void)ms;
#endif
    thread_join(t);
}

#ifdef __vita__
#include <psp2/kernel/cpu.h>
#include "dbg.h"
/* Audio threads run on user cores 1-2 only: the UI/main thread and the GPU
 * submission live on core 0, and the lower-priority workers that may also be
 * scheduled on 1-2 are always preempted by them. */
void thread_pin_audio(const char *name) {
    int rc = sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_1 | SCE_KERNEL_CPU_MASK_USER_2);
    dbg("thread %s: priority 0x%x, affinity 0x%x (set rc=%d)", name ? name : "?",
        (unsigned)sceKernelGetThreadCurrentPriority(), (unsigned)sceKernelGetThreadCpuAffinityMask(0), rc);
}
#else
void thread_pin_audio(const char *name) { (void)name; }
#endif
