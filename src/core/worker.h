/* Background job threads (network/API work off the UI thread). */
#ifndef VITAVOX_WORKER_H
#define VITAVOX_WORKER_H
#include <stdbool.h>

typedef void (*JobFn)(void *ud);
typedef struct Worker Worker;

Worker *worker_create(const char *name, int stack_kb);
/* prio as in thread_start (-2 = lowest). */
Worker *worker_create_prio(const char *name, int stack_kb, int prio);
/* Ask the thread to stop after its current job without joining (shutdown with
 * a long, uninterruptible job in flight). The Worker is leaked. */
void worker_abandon(Worker *w);
/* Finishes the job in progress, drops queued ones (their ud is freed), joins. */
void worker_destroy(Worker *w);
/* Like worker_destroy, but abandon the join after timeout_ms. */
void worker_destroy_timeout(Worker *w, int timeout_ms);
/* FIFO. The job owns ud (free it inside fn). Returns false (ud untouched) when full. */
bool worker_post(Worker *w, JobFn fn, void *ud);
int worker_pending(Worker *w);

/* the shared API worker (UI-driven requests), the progress-sync worker and the cover worker */
extern Worker *g_api;
extern Worker *g_sync;
extern Worker *g_covers;
/* 00.36: exact track lengths of a book opened with estimates (keeps g_api free). */
extern Worker *g_refine;
/* 00.37: library folder listing (Refresh); opening a book never waits behind it. */
extern Worker *g_scan;

#endif
