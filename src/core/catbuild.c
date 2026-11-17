#include "catbuild.h"

#include <stdlib.h>
#include <string.h>

#include "dbg.h"
#include "osal.h"
#include "quit.h"
#include "scanner.h"
#include "strutil.h"

static int stopped(const CatBuildOpts *o) {
    return scanner_stopping() || (o->cancel && o->cancel(o->ud));
}

/* A book from the partial catalog can be taken as is when it is the same
 * folder/file and was fully done (tracks with lengths). */
static int reusable(const LibraryCache *r, const BookSummary *b) {
    if (!r) return -1;
    int i = libcache_find(r, b->id);
    if (i < 0) return -1;
    const CachedBook *c = &r->books[i];
    if (strcmp(c->sum.path, b->path) || c->ntracks <= 0 || c->sum.duration <= 0) return -1;
    for (int k = 0; k < c->ntracks; k++)
        if (c->tracks[k].duration <= 0) return -1;
    return i;
}

static int copy_cached(const CachedBook *src, CachedBook *dst) {
    *dst = *src;
    dst->tracks = NULL;
    dst->chapters = NULL;
    dst->ntracks = dst->nchapters = 0;
    if (src->ntracks > 0) {
        dst->tracks = malloc(sizeof(Track) * (size_t)src->ntracks);
        if (!dst->tracks) return -1;
        memcpy(dst->tracks, src->tracks, sizeof(Track) * (size_t)src->ntracks);
        dst->ntracks = src->ntracks;
    }
    if (src->nchapters > 0) {
        dst->chapters = malloc(sizeof(Chapter) * (size_t)src->nchapters);
        if (dst->chapters) {
            memcpy(dst->chapters, src->chapters, sizeof(Chapter) * (size_t)src->nchapters);
            dst->nchapters = src->nchapters;
        }
    }
    return 0;
}

typedef struct {
    const CatBuildOpts *o;
    LibraryCache *cat;
    int k;
    int saving;
    int pending;           /* cat->books[k] holds the early summary of the book being timed */
    uint64_t *last_save;
    CatBuildStats *st;
} BuildCtx;

static void save_partial(BuildCtx *c, int with_pending) {
    int n0 = c->cat->n;
    c->cat->n = c->k + (with_pending && c->pending ? 1 : 0);
    c->cat->partial = 1;
    c->cat->saved_at = wall_ms();
    if (libcache_save(c->o->cache_path, c->cat) == 0) {
        c->st->partial_saves++;
        if (c->o->on_partial) c->o->on_partial(c->cat, c->o->ud);
    }
    c->cat->n = n0;
    *c->last_save = now_ms();
}

static void ctx_early(const BookSummary *b, void *ud) {
    BuildCtx *c = ud;
    BookSummary copy = *b;
    if (c->o->on_early) c->o->on_early(&copy, c->o->ud);
    CachedBook *cb = &c->cat->books[c->k];
    memset(cb, 0, sizeof *cb);
    cb->sum = *b;
    cb->sum.duration = 0; /* pending */
    c->pending = 1;
    if (c->saving && now_ms() - *c->last_save >= (uint64_t)c->o->save_every_ms) save_partial(c, 1);
}

static int ctx_cancel(void *ud) {
    BuildCtx *c = ud;
    return stopped(c->o);
}

int catbuild_run(BookSummary *books, int n, const CatBuildOpts *o, LibraryCache *out, CatBuildStats *st) {
    memset(out, 0, sizeof *out);
    CatBuildStats dummy;
    if (!st) st = &dummy;
    memset(st, 0, sizeof *st);
    if (!o || n <= 0) return -1;
    LibraryCache cat;
    memset(&cat, 0, sizeof cat);
    cat.version = LIBCACHE_VERSION;
    str_copy(cat.root, sizeof cat.root, o->root ? o->root : "");
    cat.books = calloc((size_t)n, sizeof(CachedBook));
    if (!cat.books) return -1;
    int saving = o->cache_path && o->cache_path[0];
    uint64_t last_save = now_ms();
    int k = 0, cancelled = 0;
    if (o->resume) dbg("lib: resuming the first scan: %d of %d books already in the partial catalog", o->resume->n, n);
    BuildCtx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.o = o;
    ctx.cat = &cat;
    ctx.saving = saving;
    ctx.last_save = &last_save;
    ctx.st = st;
    EnrichOpts eo;
    memset(&eo, 0, sizeof eo);
    eo.on_early = ctx_early;
    eo.cancel = ctx_cancel;
    eo.ud = &ctx;
    for (int i = 0; i < n; i++) {
        scanner_yield(); /* a book being opened goes first */
        if (stopped(o)) { cancelled = 1; break; }
        BookSummary *b = &books[i];
        CachedBook *cb = &cat.books[k];
        ctx.k = k;
        ctx.pending = 0;
        int ri = reusable(o->resume, b);
        if (ri >= 0) {
            const CachedBook *src = &o->resume->books[ri];
            if (copy_cached(src, cb) != 0) { cancelled = 1; break; }
            /* the summary as enriched last time (title, author, cover, thumb, length) */
            uint64_t added = b->added_at;
            *b = src->sum;
            if (added) b->added_at = added;
            cb->sum = *b;
            st->reused++;
            if (o->on_book) o->on_book(b, 1, o->ud);
        } else {
            int erc = scanner_enrich_ex(b, &eo);
            int changed = erc == 0;
            if (erc == SCANNER_CANCELLED || stopped(o)) { cancelled = 1; break; } /* half-enriched: kept only as pending */
            st->enriched++;
            if (o->on_book) o->on_book(b, changed, o->ud);
            memset(cb, 0, sizeof *cb);
            cb->sum = *b;
            ctx.pending = 0;
            BookDetail d;
            int rc = scanner_load_detail(b, &d);
            if (rc == SCANNER_CANCELLED || stopped(o)) {
                if (rc == 0) book_detail_free(&d);
                /* the summary is complete; the tracks are redone (probe cache) next time */
                cb->sum.duration = 0;
                ctx.pending = 1;
                cancelled = 1;
                break;
            }
            if (rc == 0) {
                /* One compact file per book so opening it later is a single read. */
                scanner_save_detail(b, &d);
                cb->ntracks = d.ntracks;
                cb->tracks = d.tracks;
                cb->nchapters = d.nchapters;
                cb->chapters = d.chapters;
                d.tracks = NULL;
                d.chapters = NULL;
                d.ntracks = d.nchapters = 0;
                book_detail_free(&d);
            }
        }
        k++;
        cat.n = k;
        uint64_t now = now_ms();
        if (saving && k < n && now - last_save >= (uint64_t)o->save_every_ms) {
            ctx.k = k;
            ctx.pending = 0;
            save_partial(&ctx, 0);
        }
    }
    if (cancelled && saving && !app_quit_active() && (k > 0 || ctx.pending)) {
        /* 00.37: Refresh or folder change: keep what is done for the next build
         * (an exit relies on the last periodic save: it must be fast). */
        ctx.k = k;
        save_partial(&ctx, 1);
        dbg("lib: stopped build saved %d books%s as a partial catalog", k, ctx.pending ? " + 1 pending" : "");
    }
    if (cancelled) {
        dbg("lib: catalog build stopped after %d of %d books (%d enriched, %d reused, %d partial saves)", k, n,
            st->enriched, st->reused, st->partial_saves);
        libcache_free(&cat);
        return SCANNER_CANCELLED;
    }
    cat.partial = 0;
    cat.saved_at = wall_ms();
    if (saving) libcache_save(o->cache_path, &cat);
    dbg("lib: catalog built: %d books (%d enriched, %d reused from the partial catalog, %d partial saves)", k,
        st->enriched, st->reused, st->partial_saves);
    *out = cat;
    return 0;
}
