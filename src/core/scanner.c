#include "scanner.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#ifdef __vita__
#include <psp2/io/dirent.h>
#include <psp2/io/stat.h>
#else
#include <dirent.h>
#endif

#include "dbg.h"
#include "osal.h"
#include "image.h"
#include "pathutil.h"
#include "probecache.h"
#include "bookcache.h"
#include "quit.h"
#include "tagart.h"
#include "progress.h"
#include "stream.h"
#include "strutil.h"
#include "mp3info.h"

static char g_cover_dir[256];
static ScannerStats g_stats;
static volatile int g_scan_cancel;

void scanner_cancel(int on) { g_scan_cancel = on ? 1 : 0; }
static int scan_stop(void) { return g_scan_cancel || app_quit_active(); }
int scanner_stopping(void) { return scan_stop(); }
static Mutex g_meta = PTHREAD_MUTEX_INITIALIZER;
static Cond g_meta_cv;
static int g_meta_ready;
static char g_busy[16][320];
static int g_nbusy;

static void meta_init(void) {
    if (g_meta_ready) return;
    cond_init(&g_meta_cv);
    g_meta_ready = 1;
}

static int path_is_busy(const char *path) {
    for (int i = 0; i < g_nbusy; i++)
        if (!strcmp(g_busy[i], path)) return 1;
    return 0;
}

/* Wait until no other thread is probing this path, then mark it busy.
 * Does not serialize probes of different files (play of book A must not
 * wait on enrich of book B). */
static void meta_begin(const char *path) {
    meta_init();
    mutex_lock(&g_meta);
    while (path_is_busy(path) && !scan_stop())
        cond_wait_ms(&g_meta_cv, &g_meta, 200);
    if (g_nbusy < 16) str_copy(g_busy[g_nbusy++], sizeof g_busy[0], path);
    mutex_unlock(&g_meta);
}

static void meta_end(const char *path) {
    mutex_lock(&g_meta);
    for (int i = 0; i < g_nbusy; i++) {
        if (!strcmp(g_busy[i], path)) {
            if (i < g_nbusy - 1)
                memmove(g_busy[i], g_busy[g_nbusy - 1], sizeof g_busy[0]);
            g_nbusy--;
            break;
        }
    }
    cond_broadcast(&g_meta_cv);
    mutex_unlock(&g_meta);
}

static void meta_wait_idle(const char *path) {
    meta_init();
    mutex_lock(&g_meta);
    while (path_is_busy(path) && !scan_stop())
        cond_wait_ms(&g_meta_cv, &g_meta, 200);
    mutex_unlock(&g_meta);
}

/* 00.36: opening a book whose details are not ready takes priority over the
 * background enrich: while a priority load runs, enrich and background detail
 * loads wait between files (an in-flight probe finishes first, ~0.2 s). */
static volatile int g_prio;
static uint64_t g_prio_waited_ms;

void scanner_priority_begin(void) {
    meta_init();
    mutex_lock(&g_meta);
    g_prio++;
    mutex_unlock(&g_meta);
}

void scanner_priority_end(void) {
    mutex_lock(&g_meta);
    if (g_prio > 0) g_prio--;
    cond_broadcast(&g_meta_cv);
    mutex_unlock(&g_meta);
}

int scanner_priority_active(void) { return g_prio > 0; }
uint64_t scanner_priority_waited_ms(void) { return g_prio_waited_ms; }

/* Background work: wait while a priority load runs. 1 if it waited. */
static int prio_yield(const char *what) {
    if (!g_prio) return 0;
    meta_init();
    uint64_t t0 = now_ms();
    dbg("scanner: %s paused for the book being opened", what ? what : "background scan");
    mutex_lock(&g_meta);
    while (g_prio > 0 && !scan_stop()) cond_wait_ms(&g_meta_cv, &g_meta, 50);
    mutex_unlock(&g_meta);
    uint64_t dt = now_ms() - t0;
    g_prio_waited_ms += dt;
    dbg("scanner: %s resumed after %llu ms", what ? what : "background scan", (unsigned long long)dt);
    return 1;
}

void scanner_yield(void) { prio_yield("enrich"); }

void scanner_set_cover_dir(const char *dir) {
    str_copy(g_cover_dir, sizeof g_cover_dir, dir ? dir : "");
    if (g_cover_dir[0]) mkdir(g_cover_dir, 0777);
}

const ScannerStats *scanner_last_stats(void) { return &g_stats; }

bool scanner_is_audio(const char *name) {
    return path_is_audio(name);
}

static bool is_dot(const char *name) {
    return !name || !name[0] || name[0] == '.' || !strcmp(name, ".") || !strcmp(name, "..");
}

static void strip_trailing_slash(char *p) {
    size_t n = strlen(p);
    while (n > 1 && (p[n - 1] == '/' || p[n - 1] == '\\')) {
        /* keep a slash right after a device colon: "ux0:/" */
        if (n >= 2 && p[n - 2] == ':') break;
        p[--n] = 0;
    }
}

static int is_dir_path(const char *path) {
#ifdef __vita__
    SceIoStat st;
    memset(&st, 0, sizeof st);
    if (sceIoGetstat(path, &st) < 0) return 0;
    return SCE_S_ISDIR(st.st_mode);
#else
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

static uint64_t mtime_ms(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return (uint64_t)st.st_mtime * 1000ULL;
}

static uint64_t file_size(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return (uint64_t)st.st_size;
}

/* Tests: emulate the Vita's per-file I/O cost (open/stat on an SD2Vita). */
static volatile int g_test_io_ms;
static volatile int g_ff_probes;
static volatile int g_mp3_hdr, g_mp3_fallback;
int scanner_mp3_header_reads(void) { return g_mp3_hdr; }
int scanner_mp3_fallbacks(void) { return g_mp3_fallback; }
void scanner_test_set_io_delay(int ms) { g_test_io_ms = ms; }
int scanner_ffmpeg_probes(void) { return g_ff_probes; }
static void test_io_cost(void) { if (g_test_io_ms > 0) sleep_ms(g_test_io_ms); }

/* Size and mtime with one stat (the probe cache key). */
static void file_stat2(const char *path, uint64_t *size, uint64_t *mt) {
    test_io_cost();
    struct stat st;
    if (stat(path, &st) != 0) { *size = 0; *mt = 0; return; }
    *size = (uint64_t)st.st_size;
    *mt = (uint64_t)st.st_mtime * 1000ULL;
}

static const char *base_name(const char *path) {
    const char *s = path, *last = path;
    for (; *s; s++) if (*s == '/' || *s == '\\') last = s + 1;
    return last;
}

static void strip_ext(char *name) {
    char *dot = NULL;
    for (char *p = name; *p; p++) if (*p == '.') dot = p;
    if (dot && dot != name) *dot = 0;
}

static void id_from_path(const char *rel, char *out, size_t n) {
    uint64_t h = 14695981039346656037ULL;
    for (const unsigned char *p = (const unsigned char *)rel; *p; p++) {
        h ^= *p;
        h *= 1099511628211ULL;
    }
    snprintf(out, n, "%016llx", (unsigned long long)h);
}

typedef struct { char name[256]; char path[320]; int is_dir; uint64_t size; } DirEnt;

static int ent_cmp(const void *a, const void *b) {
    const DirEnt *x = a, *y = b;
    if (x->is_dir != y->is_dir) return y->is_dir - x->is_dir;
    return str_natcasecmp(x->name, y->name);
}

#ifdef __vita__
/* Try the given path, then ux0:foo -> ux0:/foo, then a trailing slash. */
static SceUID dopen_variants(const char *path, char *used, size_t n) {
    str_copy(used, n, path);
    SceUID fd = sceIoDopen(used);
    if (fd >= 0) return fd;
    dbg("scanner: sceIoDopen('%s') failed %08x", used, (unsigned)fd);

    const char *col = strchr(path, ':');
    if (col && col[1] && col[1] != '/' && col[1] != '\\') {
        char alt[320];
        snprintf(alt, sizeof alt, "%.*s:/%s", (int)(col - path), path, col + 1);
        fd = sceIoDopen(alt);
        if (fd >= 0) {
            str_copy(used, n, alt);
            dbg("scanner: opened variant '%s'", used);
            return fd;
        }
        dbg("scanner: sceIoDopen('%s') failed %08x", alt, (unsigned)fd);
    }

    char slashed[320];
    snprintf(slashed, sizeof slashed, "%s/", path);
    fd = sceIoDopen(slashed);
    if (fd >= 0) {
        str_copy(used, n, path); /* join without a doubled slash */
        dbg("scanner: opened with trailing slash '%s/'", path);
        return fd;
    }
    dbg("scanner: sceIoDopen('%s/') failed %08x", path, (unsigned)fd);
    return fd;
}
#endif

static int list_dir_ex(const char *path, DirEnt **out, int *n, int tally) {
    *out = NULL;
    *n = 0;
    char opened[320];
    str_copy(opened, sizeof opened, path);

#ifdef __vita__
    SceUID fd = dopen_variants(path, opened, sizeof opened);
    if (fd < 0) {
        snprintf(g_stats.error, sizeof g_stats.error, "Can't open %s (%08x)", path, (unsigned)fd);
        return -1;
    }
    int cap = 0;
    for (;;) {
        if (scan_stop()) { sceIoDclose(fd); return -1; }
        SceIoDirent de;
        memset(&de, 0, sizeof de);
        int r = sceIoDread(fd, &de);
        if (r <= 0) {
            if (r < 0) dbg("scanner: sceIoDread('%s') %08x", opened, (unsigned)r);
            break;
        }
        if (tally) g_stats.entries_seen++;
        if (is_dot(de.d_name)) {
            if (tally) g_stats.skipped++;
            dbg("scanner: skip dotfile '%s' in '%s'", de.d_name, opened);
            continue;
        }
        if (*n == cap) {
            cap = cap ? cap * 2 : 16;
            DirEnt *neo = realloc(*out, sizeof(DirEnt) * cap);
            if (!neo) { free(*out); *out = NULL; sceIoDclose(fd); return -1; }
            *out = neo;
        }
        DirEnt *e = &(*out)[(*n)++];
        memset(e, 0, sizeof *e);
        str_copy(e->name, sizeof e->name, de.d_name);
        path_join(e->path, sizeof e->path, opened, de.d_name);
        e->is_dir = SCE_S_ISDIR(de.d_stat.st_mode);
        e->size = e->is_dir ? 0 : (uint64_t)de.d_stat.st_size;
        if ((g_stats.entries_seen % 50) == 1)
            dbg("scanner: entry %d '%s' dir=%d", g_stats.entries_seen, e->path, e->is_dir);
    }
    sceIoDclose(fd);
#else
    DIR *d = opendir(path);
    if (!d) {
        snprintf(g_stats.error, sizeof g_stats.error, "Can't open %s (%s)", path, strerror(errno));
        dbg("scanner: opendir('%s') failed: %s", path, strerror(errno));
        return -1;
    }
    int cap = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (scan_stop()) { closedir(d); return -1; }
        if (tally) g_stats.entries_seen++;
        if (is_dot(de->d_name)) {
            if (tally) g_stats.skipped++;
            dbg("scanner: skip dotfile '%s' in '%s'", de->d_name, path);
            continue;
        }
        if (*n == cap) {
            cap = cap ? cap * 2 : 16;
            DirEnt *neo = realloc(*out, sizeof(DirEnt) * cap);
            if (!neo) { free(*out); *out = NULL; closedir(d); return -1; }
            *out = neo;
        }
        DirEnt *e = &(*out)[(*n)++];
        memset(e, 0, sizeof *e);
        str_copy(e->name, sizeof e->name, de->d_name);
        path_join(e->path, sizeof e->path, path, de->d_name);
        struct stat st;
        if (stat(e->path, &st) == 0) {
            e->is_dir = S_ISDIR(st.st_mode);
            e->size = e->is_dir ? 0 : (uint64_t)st.st_size;
        }
        if ((g_stats.entries_seen % 50) == 1)
            dbg("scanner: entry %d '%s' dir=%d", g_stats.entries_seen, e->path, e->is_dir);
    }
    closedir(d);
#endif
    if (*n) qsort(*out, *n, sizeof **out, ent_cmp);
    return 0;
}

static int list_dir(const char *path, DirEnt **out, int *n) {
    return list_dir_ex(path, out, n, 1);
}

static int ent_cmp_path(const void *a, const void *b) {
    const DirEnt *x = a, *y = b;
    return str_natcasecmp(x->path, y->path);
}

static int dir_has_audio(const char *path, int depth) {
    if (depth < 0 || !path || !path[0]) return 0;
    DirEnt *ents = NULL;
    int n = 0;
    if (list_dir_ex(path, &ents, &n, 0) != 0) return 0;
    int yes = 0;
    for (int i = 0; i < n; i++) {
        if (!ents[i].is_dir && path_is_audio(ents[i].name)) { yes = 1; break; }
        if (ents[i].is_dir && dir_has_audio(ents[i].path, depth - 1)) { yes = 1; break; }
    }
    free(ents);
    return yes;
}

/* Collect audio files under dir, recursively, naturally sorted by full path
 * so Disc 1/01... stays before Disc 2/01... */
static int collect_audio(const char *dir, DirEnt **out, int *n) {
    DirEnt *ents = NULL;
    int ne = 0;
    if (list_dir(dir, &ents, &ne) != 0) return -1;
    int cap = *n;
    for (int i = 0; i < ne; i++) {
        if (ents[i].is_dir) {
            collect_audio(ents[i].path, out, n);
            cap = *n;
            continue;
        }
        if (!path_is_audio(ents[i].name)) {
            g_stats.skipped++;
            if (!path_is_image(ents[i].name) && !path_is_sidecar(ents[i].name))
                dbg("scanner: skip file '%s': not a supported audio type", ents[i].path);
            continue;
        }
        g_stats.audio_files++;
        /* Each log line is a file open/append/close on the Vita (~10+ ms): only the first few. */
        if (*n < 3) dbg("scanner: accept audio '%s'", ents[i].path);
        else if (*n == 3) dbg("scanner: accept audio ... (further files in '%s' not logged)", dir);
        if (*n == cap) {
            cap = cap ? cap * 2 : 8;
            DirEnt *neo = realloc(*out, sizeof(DirEnt) * cap);
            if (!neo) { free(ents); return -1; }
            *out = neo;
        }
        (*out)[(*n)++] = ents[i];
    }
    free(ents);
    if (*n) qsort(*out, *n, sizeof **out, ent_cmp_path);
    return 0;
}

static void stem_lower(const char *name, char *dst, size_t n) {
    const char *ext = path_ext(name);
    size_t len = ext && ext[0] ? (size_t)(ext - name) : strlen(name ? name : "");
    size_t i = 0;
    for (; i + 1 < n && i < len; i++) {
        char c = name[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        dst[i] = c;
    }
    if (n) dst[i] = 0;
}

/* 0 cover, 1 folder, 2 front, 3 other named-ish (album/art), 4 any image. */
static int cover_rank(const char *name) {
    if (!path_is_image(name)) return 99;
    char stem[128];
    stem_lower(name, stem, sizeof stem);
    if (!strcmp(stem, "cover")) return 0;
    if (!strcmp(stem, "folder")) return 1;
    if (!strcmp(stem, "front")) return 2;
    if (strstr(stem, "cover") || strstr(stem, "folder") || strstr(stem, "front")
        || strstr(stem, "album") || !strcmp(stem, "art"))
        return 3;
    return 4;
}

/* Prefer cover/folder/front (any case, jpg/jpeg/png), else a single image
 * in the folder, else the naturally first image if several exist. */
static int pick_cover_in_dir(const char *dir, char *out, size_t n) {
    DirEnt *ents = NULL;
    int ne = 0;
    if (list_dir_ex(dir, &ents, &ne, 0) != 0) return 0;
    int best = 99, nimg = 0;
    const char *best_path = NULL;
    for (int i = 0; i < ne; i++) {
        if (ents[i].is_dir) continue;
        int r = cover_rank(ents[i].name);
        if (r >= 99) continue;
        nimg++;
        if (r < best || (r == best && best_path && str_natcasecmp(ents[i].name, base_name(best_path)) < 0)) {
            best = r;
            best_path = ents[i].path;
        } else if (r < best || !best_path) {
            best = r;
            best_path = ents[i].path;
        }
    }
    int ok = 0;
    if (best_path && (best <= 3 || nimg >= 1)) {
        str_copy(out, n, best_path);
        ok = 1;
    }
    free(ents);
    return ok;
}

static void find_folder_cover(const char *dir, char *out, size_t n) {
    out[0] = 0;
    if (!dir || !dir[0]) return;
    if (pick_cover_in_dir(dir, out, n)) {
        dbg("scanner: folder cover '%s'", out);
        return;
    }
    DirEnt *ents = NULL;
    int ne = 0;
    if (list_dir_ex(dir, &ents, &ne, 0) != 0) return;
    /* Disc subfolders first, then any other child folder. */
    for (int pass = 0; pass < 2 && !out[0]; pass++) {
        for (int i = 0; i < ne; i++) {
            if (!ents[i].is_dir) continue;
            int disc = path_is_disc_folder(ents[i].name);
            if ((pass == 0 && !disc) || (pass == 1 && disc)) continue;
            if (pick_cover_in_dir(ents[i].path, out, n)) {
                dbg("scanner: folder cover (from '%s') '%s'", ents[i].name, out);
                break;
            }
        }
    }
    free(ents);
}

static int file_exists_path(const char *path) {
    if (!path || !path[0]) return 0;
#ifdef __vita__
    SceIoStat st;
    memset(&st, 0, sizeof st);
    return sceIoGetstat(path, &st) >= 0;
#else
    struct stat st;
    return stat(path, &st) == 0;
#endif
}

/* Save attached_pic (or already-decoded bytes) and return the written path. */
static int write_cover(const char *id, const uint8_t *data, size_t len, char *out, size_t n) {
    if (out && n) out[0] = 0;
    if (!g_cover_dir[0] || !id || !id[0] || !data || len < 8) return -1;
    int rc = img_save_cover(g_cover_dir, id, data, len, out, n);
    dbg("scanner: cover save id=%s rc=%d path='%s' src=%u",
        id, rc, (out && out[0]) ? out : "", (unsigned)len);
    return rc;
}

static int cache_complete(const ProbeCacheEnt *e) {
    if (!e || e->ver < PROBE_CACHE_VER || e->duration <= 0) return 0;
    /* 00.36 header reads were not checked (a VBR file without a Xing header
     * would carry a CBR estimate): redo them with the 00.37 check (~4-12 KB). */
    if (e->dur_only && e->hdr == PROBE_HDR_NONE) return 0;
    if (e->has_cover && (!e->cover_path[0] || !file_exists_path(e->cover_path))) return 0;
    return 1;
}

static void apply_progress(BookSummary *b) {
    BookProgress p;
    if (progress_load_for(b->id, b->duration, &p) != 0) return; /* none, or another book's (length check) */
    b->current_time = p.position;
    b->finished = p.finished;
    b->progress_updated = p.updated_ms;
    if (b->duration > 0) b->progress = (float)(p.position / b->duration);
    if (p.finished) b->progress = 1.0f;
}

static int apply_cached_meta(BookSummary *b, const char *audio_path, int first) {
    ProbeCacheEnt e;
    uint64_t sz = file_size(audio_path), mt = mtime_ms(audio_path);
    if (probecache_get(audio_path, sz, mt, &e) != 0) return -1;
    /* 00.38: a first file needs its tags read by 00.38 (the description). */
    if (!cache_complete(&e) || (first && (e.dur_only || e.tags < PROBE_TAGS_VER))) {
        /* One line per file is too slow for a 354-part book on the Vita. */
        if (first)
            dbg("scanner: cache stale/incomplete '%s' ver=%d tags=%d has_cover=%d path='%s'",
                audio_path, e.ver, e.tags, e.has_cover, e.cover_path);
        /* Still surface title/author from the v0.2.2 record while we re-probe. */
        if (first) {
            if (e.title[0] && !is_dir_path(b->path)) str_copy(b->title, sizeof b->title, e.title);
            if (e.artist[0]) str_copy(b->author, sizeof b->author, e.artist);
        }
        probecache_ent_free(&e);
        return -1;
    }
    if (first) {
        if (e.title[0] && !is_dir_path(b->path)) str_copy(b->title, sizeof b->title, e.title);
        if (e.artist[0]) str_copy(b->author, sizeof b->author, e.artist);
        if (e.cover_path[0] && !b->cover_path[0])
            str_copy(b->cover_path, sizeof b->cover_path, e.cover_path);
    }
    if (e.duration > 0) b->duration += e.duration;
    if (first)
        dbg("scanner: cache hit '%s' duration=%.1f cover='%s' chapters=%d",
            audio_path, e.duration, e.cover_path, e.nchapters);
    probecache_ent_free(&e);
    return 0;
}

/* first: the book's first file, whose record also keeps the description. */
static void fill_cache_from_info(ProbeCacheEnt *e, const char *audio_path, const MediaInfo *m,
                                 const char *cover_path, int first) {
    memset(e, 0, sizeof *e);
    str_copy(e->path, sizeof e->path, audio_path);
    e->size = file_size(audio_path);
    e->mtime_ms = mtime_ms(audio_path);
    str_copy(e->title, sizeof e->title, m->title);
    str_copy(e->artist, sizeof e->artist, m->artist);
    str_copy(e->album, sizeof e->album, m->album);
    e->duration = m->duration;
    e->has_cover = m->has_cover || (cover_path && cover_path[0]);
    if (cover_path) str_copy(e->cover_path, sizeof e->cover_path, cover_path);
    e->ver = PROBE_CACHE_VER;
    e->tags = first ? PROBE_TAGS_VER : 0;
    e->description = first ? m->description : NULL; /* borrowed, like the chapters */
    e->nchapters = m->nchapters;
    e->chapters = m->chapters; /* borrowed for put(); caller still owns */
}

static int is_mp3(const char *path);

/* 00.37: an MP3 part's length from its checked header (~4 KB, or ~12 KB for a
 * CBR file without a header), stored as a duration-only probe-cache record.
 * -1 when the header cannot be trusted: the caller probes with FFmpeg. */
static int mp3_checked_cache(const char *path, uint64_t size, uint64_t mt, double *dur, int log) {
    Mp3Info mi;
    int why = 0;
    test_io_cost();
    if (mp3_fast_info_checked(path, &mi, &why) != 0) {
        g_mp3_fallback++;
        dbg("scanner: mp3 header of '%s' not usable (%s) - full FFmpeg probe", path, mp3_why_str(why));
        return -1;
    }
    *dur = mi.duration;
    ProbeCacheEnt ne;
    memset(&ne, 0, sizeof ne);
    str_copy(ne.path, sizeof ne.path, path);
    ne.size = size;
    ne.mtime_ms = mt;
    ne.duration = mi.duration;
    ne.ver = PROBE_CACHE_VER;
    ne.dur_only = 1;
    ne.hdr = mi.exact ? PROBE_HDR_EXACT : PROBE_HDR_CBR;
    if (probecache_put(&ne) != 0) dbg("scanner: cache put failed for '%s'", path);
    g_mp3_hdr++;
    if (log)
        dbg("scanner: mp3 header '%s' duration=%.2f (%s)", path, mi.duration,
            mi.exact ? "frame count" : "constant bitrate, checked");
    return 0;
}

/* multi: the book has several files. Its MP3 parts after the first are timed
 * from their header (fallback: FFmpeg); the first file is probed for tags/art. */
static int probe_and_cache_ex(BookSummary *b, const char *audio_path, int first, int multi, int log) {
    meta_wait_idle(audio_path);
    if (apply_cached_meta(b, audio_path, first) == 0) return 0;
    meta_begin(audio_path);
    /* Recheck after winning the per-path lock — another thread may have finished. */
    if (apply_cached_meta(b, audio_path, first) == 0) { meta_end(audio_path); return 0; }
    if (!first && multi && is_mp3(audio_path)) {
        double d = 0;
        uint64_t sz = file_size(audio_path), mt = mtime_ms(audio_path);
        if (mp3_checked_cache(audio_path, sz, mt, &d, log) == 0) {
            b->duration += d;
            meta_end(audio_path);
            return 0;
        }
        if (scan_stop()) { meta_end(audio_path); return -1; }
    }
    /* Folder cover already on the book: do not open the audio file for art. */
    char cover[320];
    cover[0] = 0;
    if (first && !b->cover_path[0]) {
        uint8_t *art = NULL;
        size_t art_len = 0;
        if (tagart_extract(audio_path, &art, &art_len) == 0 && art) {
            write_cover(b->id, art, art_len, cover, sizeof cover);
            free(art);
            if (cover[0]) str_copy(b->cover_path, sizeof b->cover_path, cover);
            dbg("scanner: tagart cover for '%s' -> '%s'", audio_path, b->cover_path);
        }
    }
    if (scan_stop()) { meta_end(audio_path); return -1; }
    MediaInfo m;
    test_io_cost();
    g_ff_probes++;
    dbg("scanner: enrich probe '%s'", audio_path);
    if (stream_probe_ex(audio_path, &m, STREAM_PROBE_QUICK, STREAM_PROBE_TIMEOUT_MS) != 0) {
        if (scan_stop()) dbg("scanner: probe cancelled for '%s'", audio_path);
        else dbg("scanner: probe failed/timed out for '%s' (keeping filename title)", audio_path);
        meta_end(audio_path);
        return -1;
    }
    if (first && !b->cover_path[0] && !cover[0] && m.cover && m.cover_len)
        write_cover(b->id, m.cover, m.cover_len, cover, sizeof cover);
    if (first) {
        if (m.title[0] && !is_dir_path(b->path)) str_copy(b->title, sizeof b->title, m.title);
        if (m.artist[0]) str_copy(b->author, sizeof b->author, m.artist);
        if (cover[0] && !b->cover_path[0]) str_copy(b->cover_path, sizeof b->cover_path, cover);
    }
    if (m.duration > 0) b->duration += m.duration;
    ProbeCacheEnt e;
    fill_cache_from_info(&e, audio_path, &m, cover[0] ? cover : b->cover_path, first);
    if (first)
        dbg("scanner: description for '%s': %u bytes%s", b->path, m.description ? (unsigned)strlen(m.description) : 0u,
            m.description ? "" : " (no comment/description tag in the first file)");
    if (probecache_put(&e) != 0) dbg("scanner: cache put failed for '%s'", audio_path);
    media_info_free(&m);
    meta_end(audio_path);
    return 0;
}

static int probe_and_cache(BookSummary *b, const char *audio_path, int first) {
    return probe_and_cache_ex(b, audio_path, first, 0, 0);
}

static void title_from_file(const char *path, char *out, size_t n) {
    str_copy(out, n, base_name(path));
    strip_ext(out);
}

static void rel_id(const char *lib_root, const char *path, char *out, size_t n) {
    const char *rel = path;
    size_t lr = strlen(lib_root);
    if (strncmp(path, lib_root, lr) == 0) {
        rel = path + lr;
        while (*rel == '/' || *rel == '\\') rel++;
    }
    id_from_path(rel, out, n);
}

static int add_book(BookSummary **books, int *n, int *cap, const BookSummary *b) {
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 8;
        BookSummary *neo = realloc(*books, sizeof(BookSummary) * *cap);
        if (!neo) return -1;
        *books = neo;
    }
    (*books)[(*n)++] = *b;
    g_stats.books++;
    return 0;
}

static void make_book_from_file(const char *lib_root, const DirEnt *e, BookSummary *b) {
    memset(b, 0, sizeof *b);
    rel_id(lib_root, e->path, b->id, sizeof b->id);
    title_from_file(e->path, b->title, sizeof b->title);
    str_copy(b->path, sizeof b->path, e->path);
    b->added_at = mtime_ms(e->path);
    apply_cached_meta(b, e->path, 1);
    if (!b->title[0]) title_from_file(e->path, b->title, sizeof b->title);
    apply_progress(b);
    dbg("scanner: book from file '%s' title='%s' duration=%.1f", e->path, b->title, b->duration);
}

static void make_book_from_folder(const char *lib_root, const DirEnt *folder, BookSummary *b) {
    memset(b, 0, sizeof *b);
    rel_id(lib_root, folder->path, b->id, sizeof b->id);
    str_copy(b->title, sizeof b->title, folder->name);
    str_copy(b->path, sizeof b->path, folder->path);
    b->added_at = mtime_ms(folder->path);
    find_folder_cover(folder->path, b->cover_path, sizeof b->cover_path);
    DirEnt *files = NULL;
    int nf = 0;
    collect_audio(folder->path, &files, &nf);
    int hits = 0;
    for (int i = 0; i < nf; i++) {
        if (apply_cached_meta(b, files[i].path, i == 0) == 0) hits++;
    }
    /* 00.37: a partly known length is not the book's length: pending (0) until
     * every part is timed. */
    if (hits < nf) b->duration = 0;
    if (!b->title[0]) str_copy(b->title, sizeof b->title, folder->name);
    free(files);
    apply_progress(b);
    dbg("scanner: book from folder '%s' title='%s' tracks=%d duration=%.1f",
        folder->path, b->title, nf, b->duration);
}

/* Walk a directory: loose audio files are books; a folder with audio (directly
 * or nested) is a book; a folder with only subfolders is a container and we
 * descend so Author/Book/ becomes a book. */
static int scan_level(const char *lib_root, const char *dir, BookSummary **books, int *n, int *cap) {
    DirEnt *ents = NULL;
    int ne = 0;
    if (list_dir(dir, &ents, &ne) != 0) return -1;
    for (int i = 0; i < ne; i++) {
        if (scan_stop()) { free(ents); return -1; }
        BookSummary b;
        if (ents[i].is_dir) {
            DirEnt *kids = NULL;
            int nk = 0, direct = 0, subdirs = 0;
            if (list_dir(ents[i].path, &kids, &nk) != 0) {
                g_stats.skipped++;
                dbg("scanner: skip dir '%s': cannot list", ents[i].path);
                continue;
            }
            int discish = 0, other_audio_sub = 0;
            for (int k = 0; k < nk; k++) {
                if (kids[k].is_dir) {
                    subdirs++;
                    if (dir_has_audio(kids[k].path, 3)) {
                        if (path_is_disc_folder(kids[k].name)) discish++;
                        else other_audio_sub++;
                    }
                } else if (path_is_audio(kids[k].name)) {
                    direct++;
                }
            }
            free(kids);
            /* Voice-style multi-disc: Book/CD1 + Book/CD2 (or Disc 1/Disc 2)
             * is one book, even though the parent folder has no direct audio. */
            if (direct == 0 && discish > 0 && other_audio_sub == 0) {
                dbg("scanner: multi-disc book '%s' (%d disc folders)", ents[i].path, discish);
                make_book_from_folder(lib_root, &ents[i], &b);
                if (add_book(books, n, cap, &b) != 0) { free(ents); return -1; }
                continue;
            }
            if (direct == 0 && subdirs > 0) {
                dbg("scanner: descend into container '%s'", ents[i].path);
                if (scan_level(lib_root, ents[i].path, books, n, cap) != 0) {
                    free(ents);
                    return -1;
                }
                continue;
            }
            if (direct == 0) {
                g_stats.skipped++;
                dbg("scanner: skip dir '%s': no audio files", ents[i].path);
                continue;
            }
            make_book_from_folder(lib_root, &ents[i], &b);
            if (add_book(books, n, cap, &b) != 0) { free(ents); return -1; }
        } else if (path_is_audio(ents[i].name)) {
            g_stats.audio_files++;
            dbg("scanner: accept root audio '%s'", ents[i].path);
            make_book_from_file(lib_root, &ents[i], &b);
            if (add_book(books, n, cap, &b) != 0) { free(ents); return -1; }
        } else {
            g_stats.skipped++;
            if (!path_is_image(ents[i].name) && !path_is_sidecar(ents[i].name))
                dbg("scanner: skip file '%s': not a supported audio type", ents[i].path);
        }
    }
    free(ents);
    return 0;
}

int scanner_scan(const char *root, BookSummary **books, int *n) {
    *books = NULL;
    *n = 0;
    memset(&g_stats, 0, sizeof g_stats);
    if (!root || !root[0]) {
        str_copy(g_stats.error, sizeof g_stats.error, "No library folder set");
        return -1;
    }
    char root_buf[320];
    str_copy(root_buf, sizeof root_buf, root);
    strip_trailing_slash(root_buf);
    str_copy(g_stats.root, sizeof g_stats.root, root_buf);
    if (app_quit_active()) {
        str_copy(g_stats.error, sizeof g_stats.error, "Cancelled");
        return -1;
    }
    dbg("scanner: start root='%s'", root_buf);

    int cap = 0;
    int rc = scan_level(root_buf, root_buf, books, n, &cap);
    if (rc != 0 && *n == 0) {
        dbg("scanner: failed to read '%s'", root_buf);
        return -1;
    }
    dbg("scanner: done root='%s' entries=%d audio=%d books=%d skipped=%d",
        root_buf, g_stats.entries_seen, g_stats.audio_files, g_stats.books, g_stats.skipped);
    return 0;
}


/* Menu thumbnail (256 px PNG) for every cover, folder images included, so
 * menus never decode a multi-MB original. Keyed by path+size+mtime, so an
 * unchanged image is a stat, not a decode. */
static void ensure_thumb(BookSummary *b) {
    if (b->cover_path[0] && g_cover_dir[0] && !scan_stop()) {
        int made = 0;
        if (img_thumb_ensure(g_cover_dir, b->cover_path, b->thumb_path, sizeof b->thumb_path, &made) != 0)
            b->thumb_path[0] = 0;
    } else if (!b->cover_path[0]) {
        b->thumb_path[0] = 0;
    }
}

static int enrich_stop(const EnrichOpts *o) {
    return scan_stop() || (o && o->cancel && o->cancel(o->ud));
}

int scanner_enrich_ex(BookSummary *b, const EnrichOpts *o) {
    if (!b || !b->path[0]) return -1;
    double before = b->duration;
    char title0[160], author0[96], cover0[320], thumb0[320];
    str_copy(thumb0, sizeof thumb0, b->thumb_path);
    str_copy(title0, sizeof title0, b->title);
    str_copy(author0, sizeof author0, b->author);
    str_copy(cover0, sizeof cover0, b->cover_path);
    int cancelled = 0;
    if (is_dir_path(b->path)) {
        if (!b->cover_path[0])
            find_folder_cover(b->path, b->cover_path, sizeof b->cover_path);
        DirEnt *files = NULL;
        int nf = 0;
        collect_audio(b->path, &files, &nf);
        b->duration = 0;
        uint64_t t0 = now_ms();
        int hdr0 = g_mp3_hdr, fb0 = g_mp3_fallback, ff0 = g_ff_probes;
        for (int i = 0; i < nf; i++) {
            prio_yield("enrich");
            if (enrich_stop(o)) { cancelled = 1; break; }
            probe_and_cache_ex(b, files[i].path, i == 0, nf > 1, i < 3);
            if (i == 0 && nf > 1 && o && o->on_early) {
                /* 00.37: title, author and cover now; the length when all parts are timed. */
                if (!b->title[0]) str_copy(b->title, sizeof b->title, title0);
                ensure_thumb(b);
                BookSummary early = *b;
                early.duration = 0;
                o->on_early(&early, o->ud);
                dbg("scanner: early cover for '%s' cover='%s' thumb='%s' (%d tracks to time)", b->path,
                    b->cover_path, b->thumb_path, nf - 1);
            }
        }
        if (nf > 1 && g_mp3_hdr + g_mp3_fallback > hdr0 + fb0)
            dbg("scanner: timed '%s': %d tracks, %d mp3 headers, %d fallbacks, %d FFmpeg probes in %llu ms", b->path,
                nf, g_mp3_hdr - hdr0, g_mp3_fallback - fb0, g_ff_probes - ff0, (unsigned long long)(now_ms() - t0));
        free(files);
        if (!b->title[0]) str_copy(b->title, sizeof b->title, title0);
    } else {
        /* Keep filename title unless the probe supplies one. */
        b->duration = 0;
        prio_yield("enrich");
        if (probe_and_cache(b, b->path, 1) != 0)
            b->duration = before;
        if (!b->title[0]) str_copy(b->title, sizeof b->title, title0);
    }
    if (cancelled || enrich_stop(o)) {
        dbg("scanner: enrich of '%s' stopped (Refresh, folder change or exit); finished parts stay in the probe cache",
            b->path);
        return SCANNER_CANCELLED;
    }
    apply_progress(b);
    ensure_thumb(b);
    int changed = strcmp(b->title, title0) || strcmp(b->author, author0)
        || b->duration != before || (b->cover_path[0] && strcmp(b->cover_path, cover0))
        || strcmp(b->thumb_path, thumb0);
    dbg("scanner: enrich '%s' title='%s' duration=%.1f cover='%s' thumb='%s' changed=%d",
        b->path, b->title, b->duration, b->cover_path, b->thumb_path, changed);
    return changed ? 0 : 1;
}

int scanner_enrich(BookSummary *b) {
    int rc = scanner_enrich_ex(b, NULL);
    return rc == SCANNER_CANCELLED ? 1 : rc;
}

/* ---- book detail (tracks, durations, chapters) ------------------------------ */


static void add_chapters(Chapter **ch, int *nch, int *cap, const Chapter *src, int n, double offset) {
    if (n <= 0 || !src) return;
    int need = *nch + n;
    if (need > *cap) {
        int nc = need + 8;
        Chapter *neo = realloc(*ch, sizeof(Chapter) * nc);
        if (!neo) return;
        *ch = neo;
        *cap = nc;
    }
    for (int c = 0; c < n; c++) {
        (*ch)[*nch] = src[c];
        (*ch)[*nch].start += offset;
        (*ch)[*nch].end += offset;
        (*nch)++;
    }
}

static int is_mp3(const char *path) {
    const char *e = path_ext(path);
    if (!e || strlen(e) != 4 || e[0] != '.') return 0;
    return (e[1] | 0x20) == 'm' && (e[2] | 0x20) == 'p' && e[3] == '3';
}

/* The cached record for a file, if complete. Fills duration/author/chapters. */
static int detail_from_cache(const char *path, uint64_t size, uint64_t mt, BookDetail *out, Track *t,
                             Chapter **ch, int *nch, int *cap, double offset, int first) {
    ProbeCacheEnt e;
    if (probecache_get(path, size, mt, &e) != 0) return -1;
    if (!cache_complete(&e)) { probecache_ent_free(&e); return -1; }
    /* 00.38: the first file's record must carry the description read. */
    if (first && (e.dur_only || e.tags < PROBE_TAGS_VER)) { probecache_ent_free(&e); return -1; }
    if (first && e.description && !out->description) {
        out->description = e.description;
        e.description = NULL;
    }
    t->duration = e.duration;
    if (!out->author[0] && e.artist[0]) str_copy(out->author, sizeof out->author, e.artist);
    if (e.nchapters > 0 && e.chapters) add_chapters(ch, nch, cap, e.chapters, e.nchapters, offset);
    probecache_ent_free(&e);
    return 0;
}

int scanner_load_detail_opts(const BookSummary *b, BookDetail *out, const DetailOpts *o) {
    memset(out, 0, sizeof *out);
    if (!b) return -1;
    DetailOpts def;
    memset(&def, 0, sizeof def);
    def.start = -1;
    if (!o) o = &def;
    uint64_t t0 = now_ms();
    str_copy(out->id, sizeof out->id, b->id);
    str_copy(out->title, sizeof out->title, b->title);
    str_copy(out->author, sizeof out->author, b->author);
    out->duration = b->duration;

    DirEnt *files = NULL;
    int nf = 0;
    if (is_dir_path(b->path)) {
        collect_audio(b->path, &files, &nf);
    } else {
        files = calloc(1, sizeof *files);
        if (!files) return -1;
        str_copy(files[0].path, sizeof files[0].path, b->path);
        str_copy(files[0].name, sizeof files[0].name, base_name(b->path));
        nf = 1;
    }
    if (nf <= 0) { free(files); return scan_stop() ? SCANNER_CANCELLED : -1; }

    out->tracks = calloc(nf, sizeof(Track));
    if (!out->tracks) { free(files); return -1; }
    out->ntracks = nf;
    uint64_t t_list = now_ms();
    int hits = 0, fast = 0, probed = 0, estimated = 0;
    double offset = 0;
    int nch = 0, ch_cap = 0;
    Chapter *ch = NULL;
    /* Many MP3 parts: time each from its header (~4 KB read) when opening. */
    int fast_mp3 = o->priority && nf >= 2;
    double ex_dur = 0, ex_bytes = 0; /* exact tracks, for estimating the rest */
    int *est = NULL;

    for (int i = 0; i < nf; i++) {
        Track *t = &out->tracks[i];
        t->index = i;
        str_copy(t->path, sizeof t->path, files[i].path);
        str_copy(t->ext, sizeof t->ext, path_ext(files[i].path));
        title_from_file(files[i].path, t->title, sizeof t->title);
        t->start_offset = offset;
        if (scan_stop() || (o->cancel && o->cancel(o->ud))) {
            dbg("scanner: detail load of %s cancelled at track %d/%d", b->id, i, nf);
            free(files);
            free(ch);
            free(est);
            book_detail_free(out);
            return SCANNER_CANCELLED;
        }
        /* Opening: once the start track is timed and the budget is spent, the
         * rest are estimated from their size (no per-file I/O) and refined
         * after playback has started. */
        if (o->priority && o->start >= 0 && i > 0 && offset > o->start && ex_bytes > 0
            && now_ms() - t_list >= (uint64_t)o->budget_ms && files[i].size > 0) {
            t->size = files[i].size;
            if (!est) est = calloc(nf, sizeof *est);
            if (est) est[i] = 1;
            t->duration = (double)t->size * (ex_dur / ex_bytes);
            estimated++;
            offset += t->duration;
            continue;
        }
        if (!o->priority && prio_yield("book detail load")) {
            if (scan_stop() || (o->cancel && o->cancel(o->ud))) { i--; continue; }
        }
        uint64_t size = 0, mt = 0;
        file_stat2(files[i].path, &size, &mt);
        t->size = size;
        int got = 0;
        /* Wait only if this same file is being probed (play of one book must
         * not block on enrich of another). */
        meta_wait_idle(files[i].path);
        if (detail_from_cache(files[i].path, size, mt, out, t, &ch, &nch, &ch_cap, offset, i == 0) == 0) {
            if (i < 3) dbg("scanner: detail cache hit '%s' duration=%.1f", files[i].path, t->duration);
            hits++;
            got = 1;
        }
        if (!got) {
            meta_begin(files[i].path);
            /* Recheck cache after taking the path lock. */
            if (detail_from_cache(files[i].path, size, mt, out, t, &ch, &nch, &ch_cap, offset, i == 0) == 0) {
                hits++;
                got = 1;
            }
            /* 00.38: never for the first file: its tags hold the book's description. */
            if (!got && fast_mp3 && i > 0 && is_mp3(files[i].path)) {
                /* 00.37: the same checked header read as the background scan. */
                double d = 0;
                if (mp3_checked_cache(files[i].path, size, mt, &d, fast < 3) == 0) {
                    t->duration = d;
                    fast++;
                    got = 1;
                }
            }
            if (!got && !scan_stop()) {
                /* Quick probe only — never find_stream_info for a warm mov/m4b. */
                MediaInfo m;
                dbg("scanner: detail quick probe '%s'", files[i].path);
                g_ff_probes++;
                if (stream_probe_ex(files[i].path, &m, STREAM_PROBE_QUICK, STREAM_PROBE_TIMEOUT_MS) == 0) {
                    t->duration = m.duration;
                    if (!out->author[0] && m.artist[0]) str_copy(out->author, sizeof out->author, m.artist);
                    char cover[320];
                    cover[0] = 0;
                    if (i == 0 && m.cover && m.cover_len)
                        write_cover(out->id, m.cover, m.cover_len, cover, sizeof cover);
                    add_chapters(&ch, &nch, &ch_cap, m.chapters, m.nchapters, offset);
                    ProbeCacheEnt ne;
                    fill_cache_from_info(&ne, files[i].path, &m, cover, i == 0);
                    ne.size = size;
                    ne.mtime_ms = mt;
                    probecache_put(&ne);
                    if (i == 0 && m.description && !out->description) {
                        out->description = m.description; /* only the first file's tags describe the book */
                        m.description = NULL;
                    }
                    media_info_free(&m);
                    probed++;
                    got = 1;
                } else if (scan_stop()) {
                    dbg("scanner: detail probe cancelled for '%s'", files[i].path);
                } else {
                    dbg("scanner: detail probe failed for '%s'", files[i].path);
                }
            }
            meta_end(files[i].path);
            if (!got && (scan_stop() || (o->cancel && o->cancel(o->ud)))) { i--; continue; } /* -> cancelled above */
        }
        if (t->duration <= 0) t->duration = 0;
        if (t->duration > 0 && t->size > 0) { ex_dur += t->duration; ex_bytes += (double)t->size; }
        offset += t->duration;
    }
    if (offset > 0 || out->duration <= 0) out->duration = offset;
    out->provisional = estimated;

    if (nch > 0) {
        /* Embedded chapters (with estimates: those of the timed part, until refined). */
        out->chapters = ch;
        out->nchapters = nch;
    } else {
        free(ch);
        out->chapters = calloc(nf, sizeof(Chapter));
        if (out->chapters) {
            out->nchapters = nf;
            for (int i = 0; i < nf; i++) {
                out->chapters[i].start = out->tracks[i].start_offset;
                out->chapters[i].end = out->tracks[i].start_offset + out->tracks[i].duration;
                str_copy(out->chapters[i].title, sizeof out->chapters[i].title, out->tracks[i].title);
            }
        }
    }
    free(files);
    free(est);
    dbg("scanner: detail '%s' %d tracks (%d probe-cache hits, %d mp3 headers, %d probes, %d estimated) %d chapters: "
        "list %llu ms, tracks %llu ms%s",
        b->id, nf, hits, fast, probed, estimated, out->nchapters, (unsigned long long)(t_list - t0),
        (unsigned long long)(now_ms() - t_list), o->priority ? " (priority)" : "");
    return 0;
}

int scanner_load_detail(const BookSummary *b, BookDetail *out) {
    return scanner_load_detail_opts(b, out, NULL);
}

static int open_detail(const BookSummary *b, BookDetail *out, double start, int (*cancel)(void *), void *ud) {
    uint64_t t0 = now_ms();
    if (bookcache_load(b, out) == 0) return 0;
    DetailOpts o;
    memset(&o, 0, sizeof o);
    o.priority = 1;
    o.start = start < 0 ? 0 : start;
    o.budget_ms = SCANNER_OPEN_BUDGET_MS;
    o.cancel = cancel;
    o.ud = ud;
    scanner_priority_begin();
    int rc = scanner_load_detail_opts(b, out, &o);
    scanner_priority_end();
    int saved = rc == 0 && !out->provisional && scanner_save_detail(b, out) == 0;
    dbg("scanner: open detail %s via %s load rc=%d in %llu ms (book file %s)", b ? b->id : "?",
        rc == 0 && out->provisional ? "quick" : "full", rc, (unsigned long long)(now_ms() - t0),
        saved ? "saved for next time" : rc == 0 && out->provisional ? "after the durations are refined" : "not saved");
    return rc;
}

int scanner_open_detail(const BookSummary *b, BookDetail *out) {
    return open_detail(b, out, 0, NULL, NULL);
}

int scanner_open_detail_at(const BookSummary *b, BookDetail *out, double start, int (*cancel)(void *), void *ud) {
    return open_detail(b, out, start, cancel, ud);
}

int scanner_refine_detail(const BookSummary *b, BookDetail *out, int (*cancel)(void *), void *ud) {
    uint64_t t0 = now_ms();
    DetailOpts o;
    memset(&o, 0, sizeof o);
    o.priority = 1;
    o.start = -1;
    o.cancel = cancel;
    o.ud = ud;
    scanner_priority_begin();
    int rc = scanner_load_detail_opts(b, out, &o);
    scanner_priority_end();
    int saved = rc == 0 && scanner_save_detail(b, out) == 0;
    dbg("scanner: refined %s rc=%d in %llu ms (book file %s)", b ? b->id : "?", rc,
        (unsigned long long)(now_ms() - t0), saved ? "saved" : "not saved");
    return rc;
}

int scanner_save_detail(const BookSummary *b, const BookDetail *d) {
    if (!b || !d || d->ntracks <= 0) return -1;
    if (d->provisional) {
        dbg("scanner: book file for %s not saved: %d track lengths are estimates", b->id, d->provisional);
        return -1;
    }
    /* Never freeze an incomplete detail (a track whose probe failed / timed out). */
    for (int i = 0; i < d->ntracks; i++)
        if (d->tracks[i].duration <= 0) {
            dbg("scanner: book file for %s not saved: track %d has no duration", b->id, i);
            bookcache_remove(b->id);
            return -1;
        }
    return bookcache_save(b, d);
}
