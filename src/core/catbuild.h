/* Building a folder's catalog after the filename scan (enrich every book, then
 * its tracks), saved as it goes (00.36): during the first scan the catalog is
 * written every few seconds marked "partial", so quitting mid-scan keeps the
 * books already done; the next start loads that partial catalog, reuses its
 * books and only enriches the rest.
 * 00.37: a book whose cover is known but whose length is still being timed is
 * saved in the partial catalog as pending (duration 0, no tracks): shown with
 * its cover after a restart, re-timed (from the probe cache) by the resumed
 * scan. A build stopped by a Refresh or a folder change (not by exit) saves
 * what it has, so the next build continues from there. */
#ifndef VITAVOX_CATBUILD_H
#define VITAVOX_CATBUILD_H
#include "libcache.h"
#include "models.h"

#define CATBUILD_SAVE_EVERY_MS 5000

typedef struct {
    const char *root;             /* library folder (catalog root) */
    const char *cache_path;       /* its catalog file; "" = do not save */
    const LibraryCache *resume;   /* partial catalog of an interrupted scan, or NULL */
    int save_every_ms;            /* partial saves at most this often (0 = after every book) */
    int (*cancel)(void *ud);      /* folder changed / quitting: stop between books */
    void (*on_book)(BookSummary *b, int changed, void *ud);   /* summary ready (UI update) */
    /* 00.37: a multi-file book's title/cover/thumb are known, its length is
     * pending (duration 0); on_book follows when every part is timed. */
    void (*on_early)(BookSummary *b, void *ud);
    void (*on_partial)(const LibraryCache *cat, void *ud);    /* after each partial save */
    void *ud;
} CatBuildOpts;

typedef struct {
    int enriched, reused, partial_saves;
} CatBuildStats;

/* books[0..n) are updated in place. On 0, *out is the complete catalog (also
 * saved, not partial). SCANNER_CANCELLED (-2) when stopped: *out is empty and
 * the last partial save (if any) stays on disk. */
int catbuild_run(BookSummary *books, int n, const CatBuildOpts *o, LibraryCache *out, CatBuildStats *st);

#endif
