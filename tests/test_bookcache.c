/* books/<id>.bin: per-book detail cache (bookcache.c). */
#include "test.h"
#include "bookcache.h"
#include "osal.h"
#include "strutil.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>

#define BC_ROOT "build-host/tmp-bookcache"

static void write_file(const char *path, int bytes) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    for (int i = 0; i < bytes; i++) fputc(i & 255, f);
    fclose(f);
}

static void make_book(BookSummary *b, BookDetail *d, int ntracks) {
    memset(b, 0, sizeof *b);
    memset(d, 0, sizeof *d);
    str_copy(b->id, sizeof b->id, "aabbccddeeff0011");
    snprintf(b->path, sizeof b->path, "%s/lib/Les Miserables", BC_ROOT);
    str_copy(d->id, sizeof d->id, b->id);
    str_copy(d->title, sizeof d->title, "Les Miserables");
    str_copy(d->author, sizeof d->author, "Victor Hugo");
    d->ntracks = ntracks;
    d->tracks = calloc(ntracks, sizeof(Track));
    d->nchapters = ntracks;
    d->chapters = calloc(ntracks, sizeof(Chapter));
    double off = 0;
    for (int i = 0; i < ntracks; i++) {
        Track *t = &d->tracks[i];
        t->index = i;
        snprintf(t->path, sizeof t->path, "%.200s/%04d - Chapter %d - Les Miserables.mp3", b->path, i + 101, i);
        str_copy(t->ext, sizeof t->ext, ".mp3");
        snprintf(t->title, sizeof t->title, "%04d - Chapter %d", i + 101, i);
        t->duration = 100.0 + i * 0.5;
        t->start_offset = off;
        write_file(t->path, 200 + i);
        t->size = 200 + i;
        d->chapters[i].start = off;
        d->chapters[i].end = off + t->duration;
        snprintf(d->chapters[i].title, sizeof d->chapters[i].title, "Chapter %d \xC3\xA9", i);
        off += t->duration;
    }
    d->duration = off;
}

static int same_detail(const BookDetail *a, const BookDetail *b) {
    if (strcmp(a->id, b->id) || strcmp(a->title, b->title) || strcmp(a->author, b->author)) return 0;
    if (a->ntracks != b->ntracks || a->nchapters != b->nchapters || a->duration != b->duration) return 0;
    for (int i = 0; i < a->ntracks; i++) {
        const Track *x = &a->tracks[i], *y = &b->tracks[i];
        if (strcmp(x->path, y->path) || strcmp(x->ext, y->ext) || strcmp(x->title, y->title)) return 0;
        if (x->duration != y->duration || x->start_offset != y->start_offset || x->size != y->size || x->index != y->index)
            return 0;
    }
    for (int i = 0; i < a->nchapters; i++)
        if (a->chapters[i].start != b->chapters[i].start || a->chapters[i].end != b->chapters[i].end
            || strcmp(a->chapters[i].title, b->chapters[i].title))
            return 0;
    return 1;
}

TEST(bookcache_roundtrip_and_staleness) {
    CHECK_EQ_INT(system("rm -rf " BC_ROOT " && mkdir -p '" BC_ROOT "/lib/Les Miserables'"), 0);
    bookcache_init(BC_ROOT "/books");
    BookSummary b;
    BookDetail d, got;
    make_book(&b, &d, 354);
    /* Folder mtime after the files exist (as at library build). */
    struct utimbuf ut = { 1700000000, 1700000000 };
    CHECK_EQ_INT(utime(b.path, &ut), 0);

    CHECK_EQ_INT(bookcache_load(&b, &got), -1); /* missing */
    CHECK_EQ_INT(bookcache_save(&b, &d), 0);
    uint64_t t0 = now_ms();
    CHECK_EQ_INT(bookcache_load(&b, &got), 0);
    uint64_t ms = now_ms() - t0;
    CHECK(same_detail(&d, &got));
    printf("  354-track book file load: %llu ms\n", (unsigned long long)ms);
    CHECK(ms < 100);
    book_detail_free(&got);

    /* Folder changed (file added/removed -> folder mtime): stale, file removed. */
    struct utimbuf ut2 = { 1700000100, 1700000100 };
    CHECK_EQ_INT(utime(b.path, &ut2), 0);
    CHECK_EQ_INT(bookcache_load(&b, &got), -2);
    CHECK_EQ_INT(bookcache_load(&b, &got), -1);

    /* Last track replaced with a different size: stale. */
    CHECK_EQ_INT(bookcache_save(&b, &d), 0);
    write_file(d.tracks[353].path, 10);
    CHECK_EQ_INT(utime(b.path, &ut2), 0);
    CHECK_EQ_INT(bookcache_load(&b, &got), -2);
    write_file(d.tracks[353].path, 200 + 353);
    CHECK_EQ_INT(utime(b.path, &ut2), 0);

    /* Different book id / moved book path: not used. */
    CHECK_EQ_INT(bookcache_save(&b, &d), 0);
    BookSummary moved = b;
    str_copy(moved.path, sizeof moved.path, BC_ROOT "/lib");
    CHECK_EQ_INT(bookcache_load(&moved, &got), -2);

    /* Truncated / corrupt file: rejected, never half-loaded. */
    CHECK_EQ_INT(bookcache_save(&b, &d), 0);
    char path[300];
    snprintf(path, sizeof path, "%s/books/%s.bin", BC_ROOT, b.id);
    struct stat st;
    CHECK_EQ_INT(stat(path, &st), 0);
    CHECK_EQ_INT(truncate(path, st.st_size - 20), 0);
    CHECK_EQ_INT(bookcache_load(&b, &got), -2);
    CHECK(got.tracks == NULL && got.ntracks == 0);

    /* Single-file book: stamp = file size+mtime. */
    BookSummary one;
    BookDetail od;
    memset(&one, 0, sizeof one);
    memset(&od, 0, sizeof od);
    str_copy(one.id, sizeof one.id, "single0001");
    snprintf(one.path, sizeof one.path, "%s/lib/One.m4b", BC_ROOT);
    write_file(one.path, 5000);
    od.ntracks = 1;
    od.tracks = calloc(1, sizeof(Track));
    str_copy(od.tracks[0].path, sizeof od.tracks[0].path, one.path);
    od.tracks[0].size = 5000;
    od.tracks[0].duration = 3600;
    od.duration = 3600;
    CHECK_EQ_INT(bookcache_save(&one, &od), 0);
    CHECK_EQ_INT(bookcache_load(&one, &got), 0);
    CHECK_EQ_INT(got.ntracks, 1);
    CHECK_EQ_INT(got.nchapters, 0);
    book_detail_free(&got);
    write_file(one.path, 6000);
    CHECK_EQ_INT(bookcache_load(&one, &got), -2);
    book_detail_free(&od);

    /* Hostile id never escapes the folder. */
    BookSummary bad = b;
    str_copy(bad.id, sizeof bad.id, "../evil");
    CHECK(bookcache_save(&bad, &d) != 0);
    book_detail_free(&d);
}

/* 00.38: version 2 carries the description; a version 1 file is stale. */
TEST(bookcache_description) {
    CHECK_EQ_INT(system("rm -rf " BC_ROOT " && mkdir -p '" BC_ROOT "/lib/Les Miserables'"), 0);
    bookcache_init(BC_ROOT "/books");
    BookSummary b;
    BookDetail d, got;
    make_book(&b, &d, 3);
    struct utimbuf ut = { 1700000000, 1700000000 };
    CHECK_EQ_INT(utime(b.path, &ut), 0);
    char big[DESC_MAX_BYTES + 1];
    for (int i = 0; i < DESC_MAX_BYTES; i++) big[i] = i % 61 == 60 ? '\n' : 'a' + i % 26;
    big[DESC_MAX_BYTES] = 0;
    d.description = strdup(big);
    CHECK_EQ_INT(bookcache_save(&b, &d), 0);
    CHECK_EQ_INT(bookcache_load(&b, &got), 0);
    CHECK(same_detail(&d, &got));
    CHECK_STR(got.description, big);
    book_detail_free(&got);
    /* none: loads as NULL */
    free(d.description);
    d.description = NULL;
    CHECK_EQ_INT(bookcache_save(&b, &d), 0);
    CHECK_EQ_INT(bookcache_load(&b, &got), 0);
    CHECK(got.description == NULL);
    book_detail_free(&got);
    /* a 00.37 (version 1) file: stale -> full load (which reads the description) */
    char path[300];
    snprintf(path, sizeof path, "%s/books/%s.bin", BC_ROOT, b.id);
    FILE *f = fopen(path, "r+b");
    CHECK(f != NULL);
    if (f) {
        uint32_t v1 = 1;
        fseek(f, 4, SEEK_SET);
        fwrite(&v1, 4, 1, f);
        fclose(f);
    }
    CHECK_EQ_INT(bookcache_load(&b, &got), -2);
    CHECK(got.description == NULL && got.ntracks == 0);
    book_detail_free(&d);
}

void suite_bookcache(void) {
    RUN(bookcache_roundtrip_and_staleness);
    RUN(bookcache_description);
}
