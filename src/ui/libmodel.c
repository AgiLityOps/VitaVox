#include "libmodel.h"

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "app.h"
#include "covers.h"
#include "libroot.h"
#include "libcache.h"
#include "catbuild.h"
#include "progress.h"
#include "quit.h"
#include "timeline.h"
#include "config.h"
#include "dbg.h"
#include "scanner.h"
#include "osal.h"
#include "strutil.h"
#include "worker.h"

#define MAX_CONT 24
#define MAX_RECENT 24

typedef struct {
    int rc;
    char root[320];
    BookSummary *books;
    int n;
    char ids[MAX_CONT][40];
    int nids;
} Result;

static struct {
    LibState state;
    bool loading;
    char error[360];
    ScannerStats scan;
    BookSummary *books;
    int n;
    int *sorted[SORT_COUNT];
    int cont[MAX_CONT], ncont;
    int recent[MAX_RECENT], nrecent;
    Group *groups[2];
    int ngroups[2];
    char cont_ids[MAX_CONT][40];
    int ncont_ids;
} L;

static char g_root[320];        /* normalised library folder */
static char g_cache_path[400];  /* its catalog: <data>/libraries/<folder>-<hash>.json */
static char g_libs_dir[300];    /* <data>/libraries */
static char g_legacy_path[300]; /* <data>/library.json (before 00.33: one catalog for any folder) */
static LibraryCache g_catalog;
static LibraryCache *g_pending_catalog;
static Mutex g_lock;
static Result *g_result;
static volatile int g_done, g_total;
static volatile unsigned g_gen;

static void free_views(void);
static void build_views(void);
static void apply_progress_one(BookSummary *b);

#define MAX_META 64
static BookSummary g_meta[MAX_META];
static int g_nmeta;

static void on_progress(int done, int total) { g_done = done; g_total = total; }

/* Lazily generated thumbnails reported by the covers worker; applied on the UI
 * thread and written back to library.json (debounced, saved on a worker). */
#define MAX_THUMB_UPD 32
typedef struct { char id[40]; char src[320]; char thumb[320]; } ThumbUpd;
static ThumbUpd g_tupd[MAX_THUMB_UPD];
static int g_ntupd;
static bool g_catalog_dirty;
static uint64_t g_dirty_since;

void lib_post_thumb(const char *id, const char *src, const char *thumb) {
    if (!id || !id[0] || !thumb || !thumb[0]) return;
    mutex_lock(&g_lock);
    if (g_ntupd < MAX_THUMB_UPD) {
        ThumbUpd *u = &g_tupd[g_ntupd++];
        str_copy(u->id, sizeof u->id, id);
        str_copy(u->src, sizeof u->src, src ? src : "");
        str_copy(u->thumb, sizeof u->thumb, thumb);
    }
    mutex_unlock(&g_lock);
}

typedef struct { LibraryCache cat; char path[320]; } SaveJob;

static void save_job(void *ud) {
    SaveJob *j = ud;
    uint64_t t0 = now_ms();
    int rc = libcache_save(j->path, &j->cat);
    dbg("lib: library.json saved with lazy thumbs rc=%d (%d books, %llu ms)", rc, j->cat.n,
        (unsigned long long)(now_ms() - t0));
    libcache_free(&j->cat);
    free(j);
}

static void apply_thumb_updates(void) {
    ThumbUpd upd[MAX_THUMB_UPD];
    mutex_lock(&g_lock);
    int n = g_ntupd;
    if (n) memcpy(upd, g_tupd, sizeof(ThumbUpd) * (size_t)n);
    g_ntupd = 0;
    mutex_unlock(&g_lock);
    for (int i = 0; i < n; i++) {
        int k = lib_find(L.books, L.n, upd[i].id);
        if (k >= 0 && !strcmp(L.books[k].cover_path, upd[i].src))
            str_copy(L.books[k].thumb_path, sizeof L.books[k].thumb_path, upd[i].thumb);
        int c = libcache_find(&g_catalog, upd[i].id);
        if (c >= 0 && !strcmp(g_catalog.books[c].sum.cover_path, upd[i].src)
            && strcmp(g_catalog.books[c].sum.thumb_path, upd[i].thumb)) {
            str_copy(g_catalog.books[c].sum.thumb_path, sizeof g_catalog.books[c].sum.thumb_path, upd[i].thumb);
            if (!g_catalog_dirty) g_dirty_since = now_ms();
            g_catalog_dirty = true;
        }
    }
    /* Save 3 s after the last change, and never while a Refresh is running or
     * the catalog is a first scan's partial one (the scan writes it itself). */
    if (g_catalog_dirty && !L.loading && !g_catalog.partial && g_cache_path[0] && g_catalog.n > 0
        && now_ms() - g_dirty_since > 3000 && worker_pending(g_covers) == 0) {
        SaveJob *j = calloc(1, sizeof *j);
        if (j && libcache_clone(&g_catalog, &j->cat) == 0) {
            str_copy(j->path, sizeof j->path, g_cache_path);
            if (worker_post(g_covers, save_job, j)) j = NULL;
            else libcache_free(&j->cat);
        }
        free(j);
        g_catalog_dirty = false;
    }
}

static void fill_cont_ids(Result *r) {
    r->nids = 0;
    for (int pass = 0; pass < r->n && r->nids < MAX_CONT; pass++) {
        int best = -1;
        for (int i = 0; i < r->n; i++) {
            const BookSummary *b = &r->books[i];
            if (b->finished || b->current_time <= 0) continue;
            int listed = 0;
            for (int j = 0; j < r->nids; j++) if (!strcmp(r->ids[j], b->id)) { listed = 1; break; }
            if (listed) continue;
            if (best < 0 || b->progress_updated > r->books[best].progress_updated) best = i;
        }
        if (best < 0) break;
        str_copy(r->ids[r->nids++], sizeof r->ids[0], r->books[best].id);
    }
}

static void publish_result(Result *r) {
    mutex_lock(&g_lock);
    if (g_result) { free(g_result->books); free(g_result); }
    g_result = r;
    mutex_unlock(&g_lock);
}

typedef struct {
    unsigned gen;
    int n;
    BookSummary *books;
    char root[320], cache[400];  /* folder scanned and its catalog file */
    LibraryCache *resume;        /* 00.36: partial catalog of an interrupted first scan */
} EnrichJob;

typedef struct {
    unsigned gen;
    char root[320], cache[400];
} LoadJob;

/* Hands a finished catalog to the UI thread. */
static void post_catalog(LibraryCache *cat) {
    mutex_lock(&g_lock);
    if (g_pending_catalog) { libcache_free(g_pending_catalog); free(g_pending_catalog); }
    g_pending_catalog = malloc(sizeof *g_pending_catalog);
    if (g_pending_catalog) { *g_pending_catalog = *cat; memset(cat, 0, sizeof *cat); }
    mutex_unlock(&g_lock);
}

static void apply_progress_one(BookSummary *b) {
    BookProgress p;
    if (!b) return;
    int pr = progress_load_for(b->id, b->duration, &p);
    if (pr == -2) { /* saved for a different book with this path (another folder): not this one's */
        b->current_time = 0;
        b->finished = false;
        b->progress = 0;
        b->progress_updated = 0;
        return;
    }
    if (pr != 0) return;
    b->current_time = p.position;
    b->finished = p.finished;
    b->progress_updated = p.updated_ms;
    if (b->duration > 0 && b->current_time > 0)
        b->progress = (float)(b->current_time / b->duration);
    if (b->finished) b->progress = 1.0f;
}

static int enrich_cancelled(void *ud) {
    const EnrichJob *job = ud;
    return job->gen != g_gen || app_quit_active();
}

static void enrich_on_book(BookSummary *b, int changed, void *ud) {
    const EnrichJob *job = ud;
    if (!changed || job->gen != g_gen) return;
    apply_progress_one(b);
    mutex_lock(&g_lock);
    if (g_nmeta < MAX_META) g_meta[g_nmeta++] = *b;
    mutex_unlock(&g_lock);
}

/* 00.37: cover and title of a many-file book as soon as its first file is read
 * (duration 0 = pending: the UI keeps "length pending" until on_book). */
static void enrich_on_early(BookSummary *b, void *ud) {
    const EnrichJob *job = ud;
    if (job->gen != g_gen) return;
    mutex_lock(&g_lock);
    if (g_nmeta < MAX_META) g_meta[g_nmeta++] = *b;
    mutex_unlock(&g_lock);
}

/* 00.36: books enriched so far are on the card (partial catalog); the UI's copy
 * lets a book that is already done open from it. */
static void enrich_on_partial(const LibraryCache *cat, void *ud) {
    const EnrichJob *job = ud;
    if (job->gen != g_gen) return;
    LibraryCache copy;
    if (libcache_clone(cat, &copy) == 0) post_catalog(&copy);
    libcache_free(&copy);
}

static void enrich_job(void *ud) {
    EnrichJob *job = ud;
    if (job->gen != g_gen || app_quit_active()) {
        dbg("lib: enrich of a replaced scan skipped");
        free(job->books);
        free(job);
        return;
    }
    if (job->cache[0]) {
        /* An interrupted first scan (or a build stopped by Refresh) left a partial
         * catalog: finish it, not redo it. Loaded here, on the enrich worker,
         * after the stopped build has saved what it had. */
        LibraryCache *pc = calloc(1, sizeof *pc);
        if (pc && libcache_load(job->cache, pc) == 0 && pc->partial && !strcmp(pc->root, job->root)) {
            job->resume = pc;
        } else if (pc) {
            libcache_free(pc);
            free(pc);
        }
    }
    dbg("lib: enrich start n=%d%s", job->n, job->resume ? " (resuming an interrupted first scan)" : "");
    CatBuildOpts o;
    memset(&o, 0, sizeof o);
    o.root = job->root;
    o.cache_path = job->cache;
    o.resume = job->resume;
    o.save_every_ms = CATBUILD_SAVE_EVERY_MS;
    o.cancel = enrich_cancelled;
    o.on_book = enrich_on_book;
    o.on_early = enrich_on_early;
    o.on_partial = enrich_on_partial;
    o.ud = job;
    LibraryCache cat;
    CatBuildStats st;
    int rc = catbuild_run(job->books, job->n, &o, &cat, &st);
    if (rc == 0) {
        if (job->gen == g_gen) post_catalog(&cat);
    } else {
        dbg("lib: enrich cancelled");
    }
    libcache_free(&cat);
    dbg("lib: enrich done");
    if (job->resume) { libcache_free(job->resume); free(job->resume); }
    free(job->books);
    free(job);
}

static void load_job(void *ud) {
    LoadJob *lj = ud;
    unsigned gen = lj->gen;
    Result *r = calloc(1, sizeof *r);
    if (!r) { free(lj); return; }
    (void)on_progress;
    if (app_quit_active() || gen != g_gen) { free(r); free(lj); return; }
    str_copy(r->root, sizeof r->root, lj->root);
    r->rc = scanner_scan(lj->root, &r->books, &r->n);
    if (r->rc == 0) fill_cont_ids(r);
    if (gen != g_gen) {
        dbg("lib: scan result discarded (stale)");
        free(r->books);
        free(r);
        free(lj);
        return;
    }
    if (r->rc == 0 && r->n == 0) {
        /* An empty folder is remembered as empty (not as the last non-empty list). */
        LibraryCache cat;
        memset(&cat, 0, sizeof cat);
        cat.version = LIBCACHE_VERSION;
        str_copy(cat.root, sizeof cat.root, lj->root);
        cat.saved_at = wall_ms();
        if (lj->cache[0]) libcache_save(lj->cache, &cat);
        post_catalog(&cat);
        dbg("lib: '%s' is empty (saved as empty)", lj->root);
    }
    EnrichJob *job = NULL;
    if (r->rc == 0 && r->n > 0) {
        job = calloc(1, sizeof *job);
        if (job) {
            job->gen = gen;
            job->n = r->n;
            str_copy(job->root, sizeof job->root, lj->root);
            str_copy(job->cache, sizeof job->cache, lj->cache);
            job->books = malloc(sizeof(BookSummary) * (size_t)r->n);
            if (job->books) memcpy(job->books, r->books, sizeof(BookSummary) * (size_t)r->n);
            else { free(job); job = NULL; }
        }
    }
    free(lj);
    /* Publish the filename-based list immediately so the UI leaves the
     * refresh state even if a later FFmpeg probe is slow. */
    publish_result(r);
    Worker *w = g_sync ? g_sync : g_api;
    if (job && !worker_post(w, enrich_job, job)) {
        dbg("lib: enrich not posted, running inline");
        enrich_job(job);
    }
}

void lib_set_root(const char *path) {
    char norm[320];
    if (libroot_normalize(path && path[0] ? path : SCANNER_DEFAULT_ROOT, norm, sizeof norm) == LR_EMPTY)
        str_copy(norm, sizeof norm, SCANNER_DEFAULT_ROOT);
    str_copy(g_root, sizeof g_root, norm);
    if (g_libs_dir[0]) libroot_catalog_path(g_libs_dir, g_root, g_cache_path, sizeof g_cache_path);
}

void lib_set_data_dir(const char *dir) {
    snprintf(g_libs_dir, sizeof g_libs_dir, "%s/libraries", dir);
    snprintf(g_legacy_path, sizeof g_legacy_path, "%s/library.json", dir);
    mkdir(g_libs_dir, 0777);
    if (g_root[0]) libroot_catalog_path(g_libs_dir, g_root, g_cache_path, sizeof g_cache_path);
}

const char *lib_root(void) { return g_root[0] ? g_root : SCANNER_DEFAULT_ROOT; }

const char *lib_cover_path(const char *id) {
    int i = lib_find(L.books, L.n, id);
    return i >= 0 ? L.books[i].cover_path : "";
}

void lib_init(void) { mutex_init(&g_lock); if (!g_root[0]) str_copy(g_root, sizeof g_root, SCANNER_DEFAULT_ROOT); }


static void adopt_catalog(LibraryCache *c) {
    BookSummary *sums = NULL;
    if (c->n > 0) {
        sums = malloc(sizeof(BookSummary) * (size_t)c->n);
        if (!sums) return;
        for (int i = 0; i < c->n; i++) {
            sums[i] = c->books[i].sum;
            apply_progress_one(&sums[i]);
        }
    }
    free_views();
    free(L.books);
    L.books = sums;
    L.n = c->n;
    libcache_free(&g_catalog);
    g_catalog = *c;
    memset(c, 0, sizeof *c);
    L.error[0] = 0;
    L.state = L.n ? LIB_READY : LIB_EMPTY;
    L.loading = false;
    build_views();
}

/* Empties the in-memory library and drops every pending result of the folder
 * that was shown (scans, enrich, thumbnails). Nothing on the card is deleted. */
static void reset_library(void) {
    g_gen++;
    mutex_lock(&g_lock);
    if (g_result) { free(g_result->books); free(g_result); g_result = NULL; }
    if (g_pending_catalog) { libcache_free(g_pending_catalog); free(g_pending_catalog); g_pending_catalog = NULL; }
    g_nmeta = 0;
    g_ntupd = 0;
    mutex_unlock(&g_lock);
    g_catalog_dirty = false;
    free_views();
    free(L.books);
    memset(&L, 0, sizeof L);
    libcache_free(&g_catalog);
}

/* Shows this folder's saved catalog, if it has one (an empty one counts). */
static bool load_saved_catalog(void) {
    LibraryCache c;
    memset(&c, 0, sizeof c);
    if (!g_cache_path[0] || libcache_load(g_cache_path, &c) != 0) { libcache_free(&c); return false; }
    adopt_catalog(&c);
    return true;
}

static void set_missing_folder(void) {
    L.state = LIB_ERROR;
    L.loading = false;
    snprintf(L.error, sizeof L.error, "Can't open %s", g_root);
    str_copy(L.scan.root, sizeof L.scan.root, g_root);
    str_copy(L.scan.error, sizeof L.scan.error, L.error);
}

void lib_startup(void) {
    /* 00.33: one catalog per folder; the old single library.json becomes its folder's. */
    if (g_legacy_path[0]) libroot_migrate_legacy(g_legacy_path, g_libs_dir, g_root);
    LibRootStatus st = libroot_check(g_root);
    if (st != LR_OK) {
        /* Never fall back to another folder's books. */
        set_missing_folder();
        dbg("lib: library folder '%s' is not available (%d) - empty library", g_root, (int)st);
        return;
    }
    if (load_saved_catalog()) {
        if (g_catalog.partial) {
            dbg("lib: %s's catalog is partial (%d books, first scan was interrupted) - finishing the scan", g_root, L.n);
            lib_refresh();
            return;
        }
        dbg("lib: started from %s's catalog (%d books) - Refresh in Settings to rescan", g_root, L.n);
        return;
    }
    dbg("lib: no catalog for '%s', scanning once", g_root);
    lib_refresh();
}

void lib_switch_root(const char *path) {
    reset_library();
    lib_set_root(path);
    dbg("lib: switched to '%s' (catalog %s)", g_root, g_cache_path);
    /* Coming back to a folder shows its saved list at once; the rescan follows. */
    if (!load_saved_catalog()) L.state = LIB_LOADING;
}

int lib_copy_detail(const char *id, BookDetail *out) {
    return libcache_detail(&g_catalog, id, out);
}

static void free_views(void) {
    for (int i = 0; i < SORT_COUNT; i++) { free(L.sorted[i]); L.sorted[i] = NULL; }
    for (int k = 0; k < 2; k++) { groups_free(L.groups[k], L.ngroups[k]); L.groups[k] = NULL; L.ngroups[k] = 0; }
}

void lib_clear(void) {
    g_gen++;
    free_views();
    free(L.books);
    memset(&L, 0, sizeof L);
    libcache_free(&g_catalog);
    mutex_lock(&g_lock);
    if (g_pending_catalog) { libcache_free(g_pending_catalog); free(g_pending_catalog); g_pending_catalog = NULL; }
    mutex_unlock(&g_lock);
}

void lib_refresh(void) {
    unsigned gen = ++g_gen;
    LoadJob *lj = calloc(1, sizeof *lj);
    if (!lj) return;
    lj->gen = gen;
    str_copy(lj->root, sizeof lj->root, g_root);
    str_copy(lj->cache, sizeof lj->cache, g_cache_path);
    L.loading = true;
    g_done = g_total = 0;
    if (L.state != LIB_READY) L.state = LIB_LOADING;
    /* Always post a new scan. A previous job blocked in FFmpeg is cancelled
     * via g_gen once it returns; the listing job itself does not probe. */
    /* 00.37: on its own worker: opening a book (g_api) never queues behind a
     * rescan's folder listing. */
    Worker *w = g_scan ? g_scan : g_api;
    if (!worker_post(w, load_job, lj)) { free(lj); L.loading = false; }
}

static void build_continue(void) {
    L.ncont = 0;
    for (int i = 0; i < L.ncont_ids && L.ncont < MAX_CONT; i++) {
        int k = lib_find(L.books, L.n, L.cont_ids[i]);
        if (k >= 0 && !L.books[k].finished) L.cont[L.ncont++] = k;
    }
    /* anything else with progress, most recently updated first */
    for (int pass = 0; pass < L.n && L.ncont < MAX_CONT; pass++) {
        int best = -1;
        for (int i = 0; i < L.n; i++) {
            const BookSummary *b = &L.books[i];
            if (b->finished || b->current_time <= 0) continue;
            bool listed = false;
            for (int j = 0; j < L.ncont; j++) if (L.cont[j] == i) { listed = true; break; }
            if (listed) continue;
            if (best < 0 || b->progress_updated > L.books[best].progress_updated) best = i;
        }
        if (best < 0) break;
        L.cont[L.ncont++] = best;
    }
}

static void build_views(void) {
    free_views();
    int *idx = malloc(sizeof(int) * (L.n ? L.n : 1));
    if (idx) {
        for (int i = 0; i < L.n; i++) idx[i] = i;
        lib_sort(L.books, idx, L.n, SORT_RECENT);
        L.nrecent = L.n < MAX_RECENT ? L.n : MAX_RECENT;
        memcpy(L.recent, idx, sizeof(int) * L.nrecent);
        free(idx);
    }
    for (int k = 0; k < 2; k++) {
        int n = groups_build(L.books, L.n, (GroupKind)k, &L.groups[k]);
        L.ngroups[k] = n > 0 ? n : 0;
    }
    build_continue();
}

static void apply_meta_updates(void) {
    BookSummary upd[MAX_META];
    int n;
    mutex_lock(&g_lock);
    n = g_nmeta;
    if (n) memcpy(upd, g_meta, sizeof(BookSummary) * (size_t)n);
    g_nmeta = 0;
    mutex_unlock(&g_lock);
    if (!n || !L.books) return;
    int changed = 0;
    for (int i = 0; i < n; i++) {
        int k = lib_find(L.books, L.n, upd[i].id);
        if (k < 0) continue;
        BookSummary *b = &L.books[k];
        if (upd[i].title[0]) str_copy(b->title, sizeof b->title, upd[i].title);
        if (upd[i].author[0]) str_copy(b->author, sizeof b->author, upd[i].author);
        if (upd[i].duration > 0) b->duration = upd[i].duration;
        if (upd[i].cover_path[0] && strcmp(b->cover_path, upd[i].cover_path)) {
            str_copy(b->cover_path, sizeof b->cover_path, upd[i].cover_path);
            covers_invalidate(b->id);
        }
        if (upd[i].thumb_path[0] && strcmp(b->thumb_path, upd[i].thumb_path)) {
            str_copy(b->thumb_path, sizeof b->thumb_path, upd[i].thumb_path);
            covers_invalidate(b->id);
        }
        if (b->duration > 0 && b->current_time > 0)
            b->progress = (float)(b->current_time / b->duration);
        if (b->finished) b->progress = 1.0f;
        changed = 1;
    }
    if (changed) build_views();
}

void lib_tick(void) {
    mutex_lock(&g_lock);
    Result *r = g_result;
    g_result = NULL;
    mutex_unlock(&g_lock);
    if (r && strcmp(r->root, g_root)) {
        /* finished just as the library folder changed: not this folder's */
        dbg("lib: dropping scan of '%s' (folder is now '%s')", r->root, g_root);
        free(r->books);
        free(r);
        r = NULL;
    }
    if (r) {
        L.loading = false;
        L.scan = *scanner_last_stats();
        if (r->rc != 0) {
            str_copy(L.error, sizeof L.error, L.scan.error[0] ? L.scan.error : "Couldn't read the library folder");
            if (L.n > 0) {
                /* A failed rescan of the folder being shown (the list is always this
                 * folder's: switching folders empties it first): keep it, say so. */
                L.state = LIB_READY;
                char t[200];
                snprintf(t, sizeof t, "Couldn't read %.150s - showing the last list", r->root);
                app_toast(t);
            } else {
                L.state = LIB_ERROR;
            }
            dbg("lib: scan of '%s' failed: %s (%d books kept)", r->root, L.error, L.n);
            free(r->books);
            free(r);
        } else {
            /* The filename scan has no thumbs yet; keep the ones we know until
             * enrich reports, so covers do not blink or regenerate. */
            for (int i = 0; i < r->n; i++) {
                BookSummary *nb = &r->books[i];
                int k = lib_find(L.books, L.n, nb->id);
                if (k < 0) continue;
                const BookSummary *ob = &L.books[k];
                if (!nb->cover_path[0] && ob->cover_path[0])
                    str_copy(nb->cover_path, sizeof nb->cover_path, ob->cover_path);
                if (!nb->thumb_path[0] && ob->thumb_path[0] && !strcmp(nb->cover_path, ob->cover_path))
                    str_copy(nb->thumb_path, sizeof nb->thumb_path, ob->thumb_path);
                /* 00.37: a Refresh during the first scan keeps what is known
                 * (lengths, authors) until enrich reports again. */
                if (nb->duration <= 0 && ob->duration > 0 && !strcmp(nb->path, ob->path)) {
                    nb->duration = ob->duration;
                    if (nb->current_time > 0) nb->progress = (float)(nb->current_time / nb->duration);
                    if (nb->finished) nb->progress = 1.0f;
                }
                if (!nb->author[0] && ob->author[0]) str_copy(nb->author, sizeof nb->author, ob->author);
            }
            free(L.books);
            L.books = r->books;
            L.n = r->n;
            memcpy(L.cont_ids, r->ids, sizeof r->ids);
            L.ncont_ids = r->nids;
            L.error[0] = 0;
            L.state = L.n ? LIB_READY : LIB_EMPTY;
            build_views();
            free(r);
        }
    }
    apply_meta_updates();
    apply_thumb_updates();
    LibraryCache *pc = NULL;
    mutex_lock(&g_lock);
    pc = g_pending_catalog;
    g_pending_catalog = NULL;
    mutex_unlock(&g_lock);
    if (pc && strcmp(pc->root, g_root)) {
        dbg("lib: dropping catalog of '%s' (folder is now '%s')", pc->root, g_root);
        libcache_free(pc);
        free(pc);
        pc = NULL;
    }
    if (pc) {
        libcache_free(&g_catalog);
        g_catalog = *pc;
        free(pc);
        g_catalog_dirty = false;
        dbg("lib: catalog ready (%d books%s)", g_catalog.n,
            g_catalog.partial ? ", partial: scan still running" : ", persistent cache updated");
    }
}

LibState lib_state(void) { return L.state; }
bool lib_loading(void) { return L.loading; }
const char *lib_error(void) { return L.error; }
void lib_progress(int *done, int *total) { *done = g_done; *total = g_total; }
int lib_count(void) { return L.n; }
const BookSummary *lib_book(int i) { return i >= 0 && i < L.n ? &L.books[i] : NULL; }
int lib_find_id(const char *id) { return lib_find(L.books, L.n, id); }

const int *lib_sorted(SortMode m, int *n) {
    *n = L.n;
    if (!L.sorted[m] && L.n) {
        L.sorted[m] = malloc(sizeof(int) * L.n);
        if (!L.sorted[m]) { *n = 0; return NULL; }
        for (int i = 0; i < L.n; i++) L.sorted[m][i] = i;
        lib_sort(L.books, L.sorted[m], L.n, m);
    }
    return L.sorted[m];
}

const int *lib_continue(int *n) { *n = L.ncont; return L.cont; }
const int *lib_recent(int *n) { *n = L.nrecent; return L.recent; }
const Group *lib_groups(GroupKind k, int *n) { *n = L.ngroups[k]; return L.groups[k]; }

int lib_search(const char *q, int *out, int max) {
    int n;
    const int *idx = lib_sorted(SORT_TITLE, &n);
    if (!idx || !q || !q[0]) return 0;
    int *tmp = malloc(sizeof(int) * n);
    if (!tmp) return 0;
    int k = lib_filter(L.books, idx, n, q, tmp);
    if (k > max) k = max;
    memcpy(out, tmp, sizeof(int) * k);
    free(tmp);
    return k;
}

const ScannerStats *lib_scan_stats(void) { return &L.scan; }

void lib_note_progress(const char *id, double t, double duration) {
    int i = lib_find(L.books, L.n, id);
    if (i < 0) return;
    BookSummary *b = &L.books[i];
    b->current_time = t;
    if (duration > 0) b->progress = (float)(t / duration);
    b->finished = duration > 0 && t >= duration - 1;
    b->progress_updated = wall_ms();
    /* move it to the front of Continue */
    int j = 0;
    while (j < L.ncont_ids && strcmp(L.cont_ids[j], id)) j++;
    if (j == L.ncont_ids && L.ncont_ids < MAX_CONT) L.ncont_ids++;
    if (j >= MAX_CONT) j = MAX_CONT - 1;
    memmove(L.cont_ids[1], L.cont_ids[0], sizeof L.cont_ids[0] * j);
    str_copy(L.cont_ids[0], sizeof L.cont_ids[0], id);
    build_continue();
}
