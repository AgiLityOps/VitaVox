#include "dbg.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "osal.h"
#include "quit.h"

#define QUEUE_BYTES (256 * 1024)
#define LINE_MAX_BYTES 1024

static Mutex g_flock = PTHREAD_MUTEX_INITIALIZER; /* file state below */
static char g_path[256];
static FILE *g_f;
static size_t g_size;          /* bytes in the current debug.log */
static size_t g_limit = DBG_MAX_BYTES;
static int g_rotations;

static Mutex g_qlock = PTHREAD_MUTEX_INITIALIZER; /* queue below */
static Cond g_qcv = PTHREAD_COND_INITIALIZER;
static char *g_q;
static size_t g_qlen;
static unsigned g_dropped, g_dropped_noted;
static bool g_async;
static volatile bool g_quit;
static Thread g_th;

void dbg_old_path(const char *path, char *out, size_t n) {
    if (!out || !n) return;
    out[0] = 0;
    if (!path) return;
    size_t len = strlen(path);
    if (len > 4 && !strcmp(path + len - 4, ".log"))
        snprintf(out, n, "%.*s.old.log", (int)(len - 4), path);
    else
        snprintf(out, n, "%s.old", path);
}

static size_t file_size(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && st.st_size > 0 ? (size_t)st.st_size : 0;
}

static void open_log(const char *mode) {
    g_f = g_path[0] ? fopen(g_path, mode) : NULL;
    if (g_f) setvbuf(g_f, NULL, _IOFBF, 16 * 1024);
}

static void stamp(char *buf, size_t n) {
    snprintf(buf, n, "[%llu] ", (unsigned long long)(now_ms() % 100000000ULL));
}

/* g_flock held. debug.log -> debug.old.log, fresh debug.log with a first line. */
static void rotate(const char *why) {
    if (!g_path[0]) return;
    size_t was = g_size;
    if (g_f) { fclose(g_f); g_f = NULL; }
    char old[280];
    dbg_old_path(g_path, old, sizeof old);
    remove(old);
    rename(g_path, old);
    remove(g_path); /* if the rename failed, still never more than two files */
    open_log("w");
    g_size = 0;
    g_rotations++;
    if (g_f) {
        char line[400];
        stamp(line, sizeof line);
        size_t k = strlen(line);
        snprintf(line + k, sizeof line - k, "log: rotated (%s): previous %zu bytes are in %s\n", why, was, old);
        fputs(line, g_f);
        fflush(g_f);
        g_size = strlen(line);
    }
}

/* g_flock held. Writes complete lines, rotating before one that would pass the cap. */
static void write_lines(const char *p, size_t n) {
    if (!g_path[0]) { fwrite(p, 1, n, stderr); return; }
    if (!g_f) open_log("a");
    while (n > 0) {
        const char *nl = memchr(p, '\n', n);
        size_t k = nl ? (size_t)(nl - p) + 1 : n;
        if (g_size + k > g_limit && g_size > 0) rotate("size limit");
        if (g_f) fwrite(p, 1, k, g_f);
        g_size += k;
        p += k;
        n -= k;
    }
    if (g_f) fflush(g_f);
}

void dbg_set_limit(size_t bytes) {
    mutex_lock(&g_flock);
    g_limit = bytes ? bytes : DBG_MAX_BYTES;
    mutex_unlock(&g_flock);
}

int dbg_rotations(void) { return g_rotations; }
unsigned dbg_dropped(void) { return g_dropped; }

void dbg_init(const char *path) {
    mutex_lock(&g_flock);
    if (g_f) { fclose(g_f); g_f = NULL; }
    snprintf(g_path, sizeof g_path, "%s", path ? path : "");
    g_size = 0;
    if (g_path[0]) {
        /* pre-00.32 rotation target: never keep a third file */
        char legacy[280];
        snprintf(legacy, sizeof legacy, "%s.old", g_path);
        char old[280];
        dbg_old_path(g_path, old, sizeof old);
        if (strcmp(legacy, old)) remove(legacy);
        g_size = file_size(g_path);
        if (g_size >= g_limit) rotate("over the limit at startup");
        else open_log("a");
    }
    mutex_unlock(&g_flock);
}

/* Take everything queued and write it (logger thread, shutdown, quit path). */
static void drain_queue(void) {
    mutex_lock(&g_qlock);
    char *batch = NULL;
    size_t n = g_qlen;
    if (n) {
        batch = malloc(n);
        if (batch) memcpy(batch, g_q, n);
        g_qlen = 0;
    }
    mutex_unlock(&g_qlock);
    unsigned dropped = g_dropped;
    if (batch) {
        mutex_lock(&g_flock);
        write_lines(batch, n);
        if (dropped != g_dropped_noted) {
            char note[160];
            stamp(note, sizeof note);
            size_t k = strlen(note);
            snprintf(note + k, sizeof note - k, "log: %u lines dropped (queue full)\n", dropped - g_dropped_noted);
            write_lines(note, strlen(note));
            g_dropped_noted = dropped;
        }
        mutex_unlock(&g_flock);
        free(batch);
    }
}

static void *logger(void *arg) {
    (void)arg;
    while (!g_quit) {
        mutex_lock(&g_qlock);
        if (!g_qlen && !g_quit) cond_wait_ms(&g_qcv, &g_qlock, 250);
        mutex_unlock(&g_qlock);
        drain_queue();
    }
    drain_queue();
    return NULL;
}

void dbg_start_async(void) {
    if (g_async) return;
    mutex_lock(&g_qlock);
    if (!g_q) g_q = malloc(QUEUE_BYTES);
    mutex_unlock(&g_qlock);
    if (!g_q) return;
    g_quit = false;
    if (!thread_start(&g_th, logger, NULL, 32, -2)) return;
    g_async = true;
}

void dbg_stop_async(void) {
    if (!g_async) return;
    g_quit = true;
    mutex_lock(&g_qlock);
    cond_broadcast(&g_qcv);
    mutex_unlock(&g_qlock);
    thread_join(&g_th);
    g_async = false;
    drain_queue();
}

void dbg(const char *fmt, ...) {
    char line[LINE_MAX_BYTES];
    stamp(line, sizeof line);
    size_t k = strlen(line);
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(line + k, sizeof line - k - 1, fmt, ap);
    va_end(ap);
    if (w < 0) w = 0;
    k += (size_t)w < sizeof line - k - 1 ? (size_t)w : sizeof line - k - 2;
    line[k++] = '\n';

    if (g_async && !app_quit_active()) {
        /* Never blocks on the card: copy into the queue (dropped if full). */
        mutex_lock(&g_qlock);
        if (g_qlen + k <= QUEUE_BYTES) {
            memcpy(g_q + g_qlen, line, k);
            g_qlen += k;
            cond_broadcast(&g_qcv);
        } else {
            g_dropped++;
        }
        mutex_unlock(&g_qlock);
        return;
    }
    if (g_async) drain_queue(); /* quitting: keep order, then write directly */
    mutex_lock(&g_flock);
    write_lines(line, k);
    mutex_unlock(&g_flock);
}
