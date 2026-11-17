#include "bookcache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef __vita__
#include <psp2/io/stat.h>
#endif

#include "dbg.h"
#include "osal.h"
#include "strutil.h"

#define MAGIC "VABK"
#define MAX_FILE (16 * 1024 * 1024)
#define MAX_TRACKS 20000
#define MAX_CHAPTERS 50000

static char g_dir[256];

void bookcache_init(const char *dir) {
    str_copy(g_dir, sizeof g_dir, dir ? dir : "");
    if (g_dir[0]) mkdir(g_dir, 0777);
}

static int file_name(const char *id, char *out, size_t n) {
    if (!g_dir[0] || !id || !id[0]) return -1;
    for (const char *p = id; *p; p++)
        if (*p == '/' || *p == '\\' || *p == ':') return -1;
    snprintf(out, n, "%s/%s.bin", g_dir, id);
    return 0;
}

static uint64_t stat_stamp(const char *path, uint64_t *size_out) {
    if (size_out) *size_out = 0;
    if (!path || !path[0]) return 0;
#ifdef __vita__
    SceIoStat st;
    memset(&st, 0, sizeof st);
    if (sceIoGetstat(path, &st) < 0) return 0;
    const SceDateTime *t = &st.st_mtime;
    uint64_t v = ((uint64_t)t->year << 40) | ((uint64_t)t->month << 36) | ((uint64_t)t->day << 31)
               | ((uint64_t)t->hour << 26) | ((uint64_t)t->minute << 20) | ((uint64_t)t->second << 14);
    v ^= (uint64_t)t->microsecond;
    if (size_out) *size_out = SCE_S_ISDIR(st.st_mode) ? 0 : (uint64_t)st.st_size;
    if (!SCE_S_ISDIR(st.st_mode)) v = v * 1099511628211ULL ^ (uint64_t)st.st_size;
    return v ? v : 1;
#else
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    uint64_t v = (uint64_t)st.st_mtime * 1000000000ULL;
#if defined(__linux__)
    v += (uint64_t)st.st_mtim.tv_nsec;
#endif
    if (size_out) *size_out = S_ISDIR(st.st_mode) ? 0 : (uint64_t)st.st_size;
    if (!S_ISDIR(st.st_mode)) v = v * 1099511628211ULL ^ (uint64_t)st.st_size;
    return v ? v : 1;
#endif
}

uint64_t bookcache_stamp(const char *path) { return stat_stamp(path, NULL); }

/* ---- tiny writer / reader --------------------------------------------------------- */

typedef struct { uint8_t *p; size_t n, cap; int err; } W;

static void w_put(W *w, const void *d, size_t n) {
    if (w->err) return;
    if (w->n + n > w->cap) {
        size_t c = w->cap ? w->cap * 2 : 16384;
        while (c < w->n + n) c *= 2;
        uint8_t *q = realloc(w->p, c);
        if (!q) { w->err = 1; return; }
        w->p = q;
        w->cap = c;
    }
    memcpy(w->p + w->n, d, n);
    w->n += n;
}
static void w_u32(W *w, uint32_t v) { w_put(w, &v, 4); }
static void w_u64(W *w, uint64_t v) { w_put(w, &v, 8); }
static void w_f64(W *w, double v) { w_put(w, &v, 8); }
static void w_str(W *w, const char *s) {
    size_t n = s ? strlen(s) : 0;
    if (n > 65535) n = 65535;
    uint16_t l = (uint16_t)n;
    w_put(w, &l, 2);
    if (n) w_put(w, s, n);
}

typedef struct { const uint8_t *p; size_t n, o; int err; } R;

static void r_get(R *r, void *d, size_t n) {
    if (r->err || r->o + n > r->n) { r->err = 1; memset(d, 0, n); return; }
    memcpy(d, r->p + r->o, n);
    r->o += n;
}
static uint32_t r_u32(R *r) { uint32_t v; r_get(r, &v, 4); return v; }
static uint64_t r_u64(R *r) { uint64_t v; r_get(r, &v, 8); return v; }
static double r_f64(R *r) { double v; r_get(r, &v, 8); return v; }
static void r_str(R *r, char *out, size_t cap) {
    uint16_t l = 0;
    r_get(r, &l, 2);
    if (r->err || r->o + l > r->n) { r->err = 1; if (cap) out[0] = 0; return; }
    size_t k = l < cap ? l : cap - 1;
    memcpy(out, r->p + r->o, k);
    out[k] = 0;
    r->o += l;
}

/* malloc'd copy of a string field; NULL when empty or on error. */
static char *r_str_dup(R *r) {
    uint16_t l = 0;
    r_get(r, &l, 2);
    if (r->err || r->o + l > r->n) { r->err = 1; return NULL; }
    char *s = NULL;
    if (l) {
        s = malloc((size_t)l + 1);
        if (!s) { r->err = 1; return NULL; }
        memcpy(s, r->p + r->o, l);
        s[l] = 0;
    }
    r->o += l;
    return s;
}

int bookcache_save(const BookSummary *b, const BookDetail *d) {
    char path[320], tmp[330];
    if (!b || !d || d->ntracks <= 0 || file_name(b->id, path, sizeof path) != 0) return -1;
    uint64_t t0 = now_ms();
    W w = {0};
    w_put(&w, MAGIC, 4);
    w_u32(&w, BOOKCACHE_VER);
    w_u64(&w, stat_stamp(b->path, NULL));
    w_str(&w, b->id);
    w_str(&w, b->path);
    w_str(&w, d->title);
    w_str(&w, d->author);
    w_str(&w, d->narrator);
    w_str(&w, d->description); /* <= DESC_MAX_BYTES, well under the 64 KB string limit */
    w_f64(&w, d->duration);
    w_u32(&w, (uint32_t)d->ntracks);
    for (int i = 0; i < d->ntracks; i++) {
        const Track *t = &d->tracks[i];
        w_str(&w, t->path);
        w_str(&w, t->ext);
        w_str(&w, t->title);
        w_str(&w, t->codec);
        w_f64(&w, t->start_offset);
        w_f64(&w, t->duration);
        w_u64(&w, t->size);
    }
    w_u32(&w, (uint32_t)(d->chapters ? d->nchapters : 0));
    for (int i = 0; d->chapters && i < d->nchapters; i++) {
        w_f64(&w, d->chapters[i].start);
        w_f64(&w, d->chapters[i].end);
        w_str(&w, d->chapters[i].title);
    }
    w_put(&w, MAGIC, 4); /* trailer: detects truncated files */
    if (w.err) { free(w.p); return -1; }
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    int ok = f && fwrite(w.p, 1, w.n, f) == w.n;
    if (f && fclose(f) != 0) ok = 0;
    if (ok) {
        remove(path);
        ok = rename(tmp, path) == 0;
    }
    if (!ok) remove(tmp);
    dbg("bookcache: save %s '%s' %d tracks %d chapters desc %u B, %u B in %llu ms%s", b->id, path, d->ntracks,
        d->nchapters, d->description ? (unsigned)strlen(d->description) : 0u, (unsigned)w.n, (unsigned long long)(now_ms() - t0), ok ? "" : " FAILED");
    free(w.p);
    return ok ? 0 : -1;
}

void bookcache_remove(const char *id) {
    char path[320];
    if (file_name(id, path, sizeof path) == 0) remove(path);
}

int bookcache_load(const BookSummary *b, BookDetail *out) {
    memset(out, 0, sizeof *out);
    char path[320];
    if (!b || file_name(b->id, path, sizeof path) != 0) return -1;
    uint64_t t0 = now_ms();
    FILE *f = fopen(path, "rb");
    if (!f) {
        dbg("bookcache: miss %s (no %s)", b->id, path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 16 || sz > MAX_FILE) { fclose(f); dbg("bookcache: bad size %ld for %s", sz, b->id); return -1; }
    uint8_t *buf = malloc((size_t)sz);
    if (!buf) { fclose(f); return -1; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    uint64_t t_read = now_ms();
    if (got != (size_t)sz) { free(buf); return -1; }
    R r = {buf, (size_t)sz, 0, 0};
    char magic[4], id[40], bpath[320];
    const char *why = NULL;
    r_get(&r, magic, 4);
    uint32_t ver = r_u32(&r);
    uint64_t stamp = r_u64(&r);
    r_str(&r, id, sizeof id);
    r_str(&r, bpath, sizeof bpath);
    uint64_t now_stamp = stat_stamp(b->path, NULL);
    if (r.err || memcmp(magic, MAGIC, 4)) why = "corrupt header";
    else if (ver != BOOKCACHE_VER) why = "old version";
    else if (strcmp(id, b->id)) why = "id mismatch";
    else if (strcmp(bpath, b->path)) why = "book path changed";
    else if (!now_stamp || stamp != now_stamp) why = "folder/file changed";
    if (why) {
        free(buf);
        dbg("bookcache: stale %s (%s) -> full load", b->id, why);
        remove(path);
        return -2;
    }
    str_copy(out->id, sizeof out->id, b->id);
    r_str(&r, out->title, sizeof out->title);
    r_str(&r, out->author, sizeof out->author);
    r_str(&r, out->narrator, sizeof out->narrator);
    out->description = r_str_dup(&r);
    out->duration = r_f64(&r);
    uint32_t nt = r_u32(&r);
    if (r.err || nt == 0 || nt > MAX_TRACKS) { r.err = 1; nt = 0; }
    if (nt) out->tracks = calloc(nt, sizeof(Track));
    if (nt && !out->tracks) r.err = 1;
    for (uint32_t i = 0; !r.err && i < nt; i++) {
        Track *t = &out->tracks[i];
        t->index = (int)i;
        r_str(&r, t->path, sizeof t->path);
        r_str(&r, t->ext, sizeof t->ext);
        r_str(&r, t->title, sizeof t->title);
        r_str(&r, t->codec, sizeof t->codec);
        t->start_offset = r_f64(&r);
        t->duration = r_f64(&r);
        t->size = r_u64(&r);
    }
    out->ntracks = r.err ? 0 : (int)nt;
    uint32_t nc = r.err ? 0 : r_u32(&r);
    if (nc > MAX_CHAPTERS) r.err = 1;
    if (!r.err && nc) {
        out->chapters = calloc(nc, sizeof(Chapter));
        if (!out->chapters) r.err = 1;
        for (uint32_t i = 0; !r.err && i < nc; i++) {
            out->chapters[i].start = r_f64(&r);
            out->chapters[i].end = r_f64(&r);
            r_str(&r, out->chapters[i].title, sizeof out->chapters[i].title);
        }
        out->nchapters = (int)nc;
    }
    char trail[4];
    r_get(&r, trail, 4);
    if (!r.err && memcmp(trail, MAGIC, 4)) r.err = 1;
    free(buf);
    if (r.err) {
        book_detail_free(out);
        memset(out, 0, sizeof *out);
        dbg("bookcache: corrupt %s -> full load", b->id);
        remove(path);
        return -2;
    }
    /* Cheap spot check: first and last track still there with the same size. */
    uint64_t s0 = 0, s1 = 0;
    stat_stamp(out->tracks[0].path, &s0);
    stat_stamp(out->tracks[out->ntracks - 1].path, &s1);
    if (s0 != out->tracks[0].size || s1 != out->tracks[out->ntracks - 1].size) {
        dbg("bookcache: stale %s (track files changed) -> full load", b->id);
        book_detail_free(out);
        memset(out, 0, sizeof *out);
        remove(path);
        return -2;
    }
    dbg("bookcache: hit %s %d tracks %d chapters (%ld B): read %llu ms, total %llu ms", b->id, out->ntracks,
        out->nchapters, sz, (unsigned long long)(t_read - t0), (unsigned long long)(now_ms() - t0));
    return 0;
}
