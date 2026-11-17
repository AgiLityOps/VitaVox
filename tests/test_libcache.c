#include "test.h"
#include "libcache.h"
#include "strutil.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void fill_book(CachedBook *b, const char *id, const char *title, int tracks) {
    memset(b, 0, sizeof *b);
    str_copy(b->sum.id, sizeof b->sum.id, id);
    str_copy(b->sum.title, sizeof b->sum.title, title);
    str_copy(b->sum.author, sizeof b->sum.author, "Author");
    str_copy(b->sum.path, sizeof b->sum.path, "/tmp/book");
    str_copy(b->sum.cover_path, sizeof b->sum.cover_path, "/tmp/cover.png");
    str_copy(b->sum.thumb_path, sizeof b->sum.thumb_path, "/tmp/covers/t_0123456789abcdef.png");
    b->sum.duration = 123.5;
    b->sum.added_at = 42;
    b->ntracks = tracks;
    b->tracks = calloc((size_t)tracks, sizeof(Track));
    for (int i = 0; i < tracks; i++) {
        b->tracks[i].index = i;
        snprintf(b->tracks[i].path, sizeof b->tracks[i].path, "/tmp/book/t%d.mp3", i);
        snprintf(b->tracks[i].title, sizeof b->tracks[i].title, "Track %d", i + 1);
        str_copy(b->tracks[i].ext, sizeof b->tracks[i].ext, ".mp3");
        b->tracks[i].duration = 10.0 + i;
        b->tracks[i].start_offset = i * 10.0;
        b->tracks[i].size = 1000u + (unsigned)i;
    }
    b->nchapters = 1;
    b->chapters = calloc(1, sizeof(Chapter));
    b->chapters[0].start = 0;
    b->chapters[0].end = 10;
    str_copy(b->chapters[0].title, sizeof b->chapters[0].title, "Ch 1");
}

TEST(roundtrip) {
    char path[] = "/tmp/vita-libcache-test.json";
    unlink(path);
    LibraryCache c;
    memset(&c, 0, sizeof c);
    c.version = LIBCACHE_VERSION;
    str_copy(c.root, sizeof c.root, "/tmp/audiobooks");
    c.saved_at = 99;
    c.n = 2;
    c.books = calloc(2, sizeof(CachedBook));
    fill_book(&c.books[0], "idA", "Book A", 3);
    fill_book(&c.books[1], "idB", "Book B", 1);
    CHECK(libcache_save(path, &c) == 0);

    LibraryCache d;
    memset(&d, 0, sizeof d);
    CHECK(libcache_load(path, &d) == 0);
    CHECK_EQ_INT(d.n, 2);
    CHECK_STR(d.root, "/tmp/audiobooks");
    CHECK_STR(d.books[0].sum.title, "Book A");
    CHECK_STR(d.books[0].sum.cover_path, "/tmp/cover.png");
    CHECK_STR(d.books[0].sum.thumb_path, "/tmp/covers/t_0123456789abcdef.png");

    LibraryCache cl;
    CHECK(libcache_clone(&d, &cl) == 0);
    CHECK_EQ_INT(cl.n, 2);
    CHECK_EQ_INT(cl.books[0].ntracks, 3);
    CHECK(cl.books[0].tracks != d.books[0].tracks);
    CHECK_STR(cl.books[0].tracks[1].path, "/tmp/book/t1.mp3");
    CHECK_STR(cl.books[0].sum.thumb_path, d.books[0].sum.thumb_path);
    libcache_free(&cl);
    CHECK_EQ_INT(d.books[0].ntracks, 3);
    CHECK_STR(d.books[0].tracks[2].title, "Track 3");
    CHECK_NEAR(d.books[0].tracks[2].start_offset, 20.0, 0.01);
    CHECK_EQ_INT(d.books[1].nchapters, 1);
    CHECK_STR(d.books[1].chapters[0].title, "Ch 1");

    BookDetail det;
    CHECK(libcache_detail(&d, "idA", &det) == 0);
    CHECK_EQ_INT(det.ntracks, 3);
    CHECK_STR(det.tracks[0].path, "/tmp/book/t0.mp3");
    book_detail_free(&det);
    CHECK(libcache_detail(&d, "missing", &det) != 0);

    libcache_free(&c);
    libcache_free(&d);
    unlink(path);
}

void suite_libcache(void) {
    RUN(roundtrip);
}
