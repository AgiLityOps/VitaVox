/* On-disk cache of cheap probe results, keyed by path + size + mtime. */
#ifndef VITAVOX_PROBECACHE_H
#define VITAVOX_PROBECACHE_H
#include <stdint.h>

#include "models.h"

#define PROBE_CACHE_VER 2

/* dur_only records: 0 = 00.36 unchecked header read (redone with the check),
 * 1 = checked frame count (Xing/Info/VBRI, exact), 2 = checked constant bitrate. */
/* 00.38: what a full (non dur_only) record holds beyond title/artist/album:
 * 0 = written before 00.38 (no description read), 1 = description read
 * (stored when the file is a book's first file). A book's first file with
 * tags < PROBE_TAGS_VER is probed again once; other parts are not affected. */
#define PROBE_TAGS_VER 1

#define PROBE_HDR_NONE 0
#define PROBE_HDR_EXACT 1
#define PROBE_HDR_CBR 2

typedef struct {
    char path[320];
    uint64_t size, mtime_ms;
    char title[160];
    char artist[96];
    char album[96];
    double duration;
    int has_cover;
    char cover_path[320];
    int ver;
    int dur_only;      /* 00.36: duration from the fast MP3 header read; no tags/art/chapters */
    int hdr;           /* 00.37: how a dur_only length was checked: PROBE_HDR_* */
    int tags;          /* 00.38: PROBE_TAGS_VER when written; 0 for older records */
    char *description; /* 00.38: cleaned description (first files only); malloc'd, may be NULL */
    int nchapters;
    Chapter *chapters; /* malloc'd; call probecache_ent_free */
} ProbeCacheEnt;

void probecache_init(const char *dir);
/* 0 if a matching record was loaded, -1 if none / stale / error. */
int probecache_get(const char *path, uint64_t size, uint64_t mtime_ms, ProbeCacheEnt *out);
int probecache_put(const ProbeCacheEnt *e);
void probecache_ent_free(ProbeCacheEnt *e);

#endif
