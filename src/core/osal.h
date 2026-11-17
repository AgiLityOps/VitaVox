/* Tiny portable threading/time helpers over pthreads (VitaSDK and POSIX hosts). */
#ifndef VITAVOX_OSAL_H
#define VITAVOX_OSAL_H
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

typedef pthread_mutex_t Mutex;
typedef pthread_cond_t Cond;

static inline void mutex_init(Mutex *m) { pthread_mutex_init(m, NULL); }
static inline void mutex_lock(Mutex *m) { pthread_mutex_lock(m); }
static inline void mutex_unlock(Mutex *m) { pthread_mutex_unlock(m); }
static inline void mutex_destroy(Mutex *m) { pthread_mutex_destroy(m); }
static inline void cond_init(Cond *c) { pthread_cond_init(c, NULL); }
static inline void cond_broadcast(Cond *c) { pthread_cond_broadcast(c); }
static inline void cond_destroy(Cond *c) { pthread_cond_destroy(c); }
/* Waits at most ms (spurious wakeups possible: callers re-check). */
void cond_wait_ms(Cond *c, Mutex *m, int ms);

/* Monotonic milliseconds. */
uint64_t now_ms(void);
/* Wall clock, ms since the Unix epoch. */
uint64_t wall_ms(void);
void sleep_ms(int ms);

typedef struct { pthread_t t; bool ok; } Thread;
/* prio: 0 = default; >0 = higher than the caller (audio), -1 = lower (background),
 * -2 = lowest (cover art: must never compete with decoding). */
bool thread_start(Thread *t, void *(*fn)(void *), void *arg, int stack_kb, int prio);
/* Called by the audio decode/output threads themselves: keeps them off the UI
 * core and logs their priority/affinity (no-op on the host). */
void thread_pin_audio(const char *name);
void thread_join(Thread *t);
/* Joins, or detaches after ms if the thread is still running. */
void thread_join_timeout(Thread *t, int ms);

#endif
