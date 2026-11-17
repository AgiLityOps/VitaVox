/* Persistent library catalog (books, tracks, durations, chapters, cover thumbs).
 * Written atomically to library.json. */
#ifndef VITAVOX_LIBCACHE_H
#define VITAVOX_LIBCACHE_H
#include "models.h"
#include <stdint.h>

#define LIBCACHE_VERSION 1

typedef struct {
    BookSummary sum;
    int ntracks;
    Track *tracks;       /* malloc'd */
    int nchapters;
    Chapter *chapters;   /* malloc'd */
} CachedBook;

typedef struct {
    int version;
    char root[320];
    uint64_t saved_at;
    int partial;         /* 00.36: saved during the first scan; the next start finishes it */
    int n;
    CachedBook *books;   /* malloc'd */
} LibraryCache;

void libcache_free(LibraryCache *c);
/* 0 ok. path is written via path.tmp + rename. */
int libcache_save(const char *path, const LibraryCache *c);
int libcache_load(const char *path, LibraryCache *out);
/* Deep-copy one book's detail (tracks/chapters malloc'd). 0 ok. */
int libcache_detail(const LibraryCache *c, const char *id, BookDetail *out);
int libcache_find(const LibraryCache *c, const char *id);
/* Deep copy (for saving off the UI thread). 0 ok. */
int libcache_clone(const LibraryCache *src, LibraryCache *dst);

#endif
