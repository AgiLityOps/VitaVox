/* Per-book detail cache: books/<id>.bin holds the full BookDetail (ordered track
 * paths, per-track durations, chapters) so opening a many-file book is one small
 * file read instead of a folder listing plus one probe-cache lookup per track.
 * Written at library build / Refresh (enrich) and after any fallback load. */
#ifndef VITAVOX_BOOKCACHE_H
#define VITAVOX_BOOKCACHE_H
#include <stdint.h>

#include "models.h"

/* 2 (00.38): + the description (from the first file's tags) */
#define BOOKCACHE_VER 2

void bookcache_init(const char *dir);
/* 0 ok (out filled, call book_detail_free), -1 missing/unreadable, -2 stale
 * (folder/file changed, version or id mismatch). The stale file is removed. */
int bookcache_load(const BookSummary *b, BookDetail *out);
/* 0 ok. Atomic (tmp + rename). */
int bookcache_save(const BookSummary *b, const BookDetail *d);
void bookcache_remove(const char *id);
/* Change stamp of a book path: folder mtime, or size+mtime of a single file. 0 = unknown. */
uint64_t bookcache_stamp(const char *path);

#endif
