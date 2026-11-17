/* 00.38: book descriptions from the first file's tags (FFmpeg + mp4lazy paths,
 * probe cache, book cache, multi-file books). Built only into test-decode. */
#include "test.h"
#include "bookcache.h"
#include "dbg.h"
#include "models.h"
#include "osal.h"
#include "probecache.h"
#include "progress.h"
#include "scanner.h"
#include "stream.h"
#include "strutil.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define DR "build-host/desc"
#define FXD DR "/fx"
#define Q "ffmpeg -hide_banner -loglevel error -y "
#define SINE(d) "-f lavfi -i sine=frequency=440:duration=" #d ":sample_rate=44100 "

static int sh(const char *cmd) {
    int rc = system(cmd);
    if (rc != 0) fprintf(stderr, "command failed (%d): %s\n", rc, cmd);
    return rc;
}

static void fresh_caches(void) {
    sh("rm -rf " DR "/cache && mkdir -p " DR "/cache/covers " DR "/cache/probe " DR "/cache/progress " DR "/cache/books");
    dbg_init(DR "/cache/debug.log");
    scanner_set_cover_dir(DR "/cache/covers");
    probecache_init(DR "/cache/probe");
    progress_init(DR "/cache/progress");
    bookcache_init(DR "/cache/books");
}

/* ID3v2.3 tag with two COMM frames: iTunes' iTunNORM (must be ignored) and one
 * whose descriptor is "Comment" in Latin-1 (FFmpeg keys it "Comment"). */
static void put_be32(FILE *f, unsigned v) { fputc(v >> 24, f); fputc(v >> 16 & 255, f); fputc(v >> 8 & 255, f); fputc(v & 255, f); }
static void comm_frame(FILE *f, const char *desc, const char *text) {
    unsigned n = 1 + 3 + (unsigned)strlen(desc) + 1 + (unsigned)strlen(text);
    fwrite("COMM", 1, 4, f);
    put_be32(f, n);
    fputc(0, f); fputc(0, f);
    fputc(0, f); /* Latin-1 */
    fwrite("eng", 1, 3, f);
    fwrite(desc, 1, strlen(desc) + 1, f);
    fwrite(text, 1, strlen(text), f);
}
static int make_comm_mp3(const char *raw_mp3, const char *out) {
    FILE *in = fopen(raw_mp3, "rb"), *f = fopen(out, "wb");
    if (!in || !f) { if (in) fclose(in); if (f) fclose(f); return -1; }
    const char *t1 = " 00000A2B 00000A2B";
    const char *t2 = "Described in Latin-1: caf\xE9.";
    unsigned size = (10 + 1 + 3 + 9 + strlen(t1)) + (10 + 1 + 3 + 8 + strlen(t2));
    fwrite("ID3\x03\x00\x00", 1, 6, f);
    fputc(size >> 21 & 127, f); fputc(size >> 14 & 127, f); fputc(size >> 7 & 127, f); fputc(size & 127, f);
    comm_frame(f, "iTunNORM", t1);
    comm_frame(f, "Comment", t2);
    char buf[8192];
    size_t k;
    while ((k = fread(buf, 1, sizeof buf, in)) > 0) fwrite(buf, 1, k, f);
    fclose(in);
    fclose(f);
    return 0;
}

static int g_fx_ok;
static void make_fixtures(void) {
    static int done;
    if (done) return;
    done = 1;
    sh("rm -rf " DR " && mkdir -p " FXD " " DR "/lib");
    FILE *f = fopen(FXD "/long.txt", "wb");
    if (f) {
        for (int i = 0; i < 1200; i++) fprintf(f, "word%04d ", i); /* ~10.8 KB */
        fclose(f);
    }
    int rc = 0;
    rc |= sh(Q SINE(3) "-c:a libmp3lame -b:a 64k -metadata title='MP3 Book' "
             "-metadata comment='<p>An <b>MP3</b> book &amp; its comment.</p><p>Second para.</p>' " FXD "/c.mp3");
    rc |= sh(Q SINE(3) "-c:a libmp3lame -b:a 64k -write_id3v2 0 -write_xing 1 " FXD "/raw.mp3");
    rc |= make_comm_mp3(FXD "/raw.mp3", FXD "/comm_desc.mp3");
    rc |= sh(Q SINE(3) "-c:a libmp3lame -b:a 64k -metadata 'TXXX=x' -metadata description='Only a description tag' " FXD "/d.mp3");
    rc |= sh(Q SINE(3) "-c:a aac -b:a 64k -metadata title='M4B Book' -metadata comment='M4B comment wins' "
             "-metadata synopsis='M4B long description' -metadata description='M4B short description' -f mp4 " FXD "/c.m4b");
    rc |= sh(Q SINE(3) "-c:a aac -b:a 64k -metadata synopsis='M4B long description only' "
             "-metadata description='M4B short description' -f mp4 " FXD "/ldes.m4b");
    rc |= sh(Q SINE(3) "-c:a aac -b:a 64k -metadata description='M4B desc only' -f mp4 " FXD "/desc.m4b");
    rc |= sh(Q SINE(3) "-c:a flac -metadata comment='FLAC comment\r\n\r\n\r\n\r\nafter blank lines' " FXD "/c.flac");
    rc |= sh(Q SINE(3) "-c:a flac -metadata DESCRIPTION='FLAC description tag' " FXD "/d.flac");
    rc |= sh(Q SINE(3) "-c:a libopus -b:a 32k -metadata comment='Opus comment' " FXD "/c.opus");
    rc |= sh(Q SINE(3) "-c:a libmp3lame -b:a 64k " FXD "/none.mp3");
    char cmd[600];
    snprintf(cmd, sizeof cmd, Q SINE(3) "-c:a libmp3lame -b:a 64k -metadata comment=\"$(cat %s/long.txt)\" %s/long.mp3", FXD, FXD);
    rc |= sh(cmd);
    g_fx_ok = rc == 0;
}

static char *probe_desc(const char *path) {
    MediaInfo m;
    if (stream_probe_ex(path, &m, STREAM_PROBE_QUICK, STREAM_PROBE_TIMEOUT_MS) != 0) return NULL;
    char *d = m.description;
    m.description = NULL;
    media_info_free(&m);
    return d;
}

static void expect_probe(const char *path, const char *want) {
    char *d = probe_desc(path);
    t_checks++;
    if (want ? !d || strcmp(d, want) : d != NULL) {
        t_failures++;
        printf("  FAIL [%s] %s: description \"%s\", expected \"%s\"\n", t_current, path, d ? d : "(null)",
               want ? want : "(null)");
    }
    free(d);
}

TEST(desc_probe_per_format) {
    make_fixtures();
    CHECK(g_fx_ok);
    expect_probe(FXD "/c.mp3", "An MP3 book & its comment.\n\nSecond para.");
    expect_probe(FXD "/comm_desc.mp3", "Described in Latin-1: caf\xC3\xA9.");  /* not iTunNORM */
    expect_probe(FXD "/d.mp3", "Only a description tag");
    expect_probe(FXD "/none.mp3", NULL);
    expect_probe(FXD "/c.flac", "FLAC comment\n\nafter blank lines");
    expect_probe(FXD "/d.flac", "FLAC description tag");
    expect_probe(FXD "/c.opus", "Opus comment");
    /* M4B: mp4lazy reader (default) and FFmpeg's mov demuxer agree */
    for (int lazy = 1; lazy >= 0; lazy--) {
        stream_set_mp4_lazy(lazy);
        expect_probe(FXD "/c.m4b", "M4B comment wins");
        expect_probe(FXD "/ldes.m4b", "M4B long description only");
        expect_probe(FXD "/desc.m4b", "M4B desc only");
    }
    stream_set_mp4_lazy(1);
    /* capped at 8 KB on a word with an ellipsis */
    char *d = probe_desc(FXD "/long.mp3");
    CHECK(d != NULL);
    if (d) {
        size_t n = strlen(d);
        CHECK(n <= DESC_MAX_BYTES && n > DESC_MAX_BYTES - 64);
        CHECK(!strncmp(d, "word0000 word0001", 17));
        CHECK(!strcmp(d + n - 3, "\xE2\x80\xA6"));
        CHECK(d[n - 4] >= '0' && d[n - 4] <= '9'); /* ends on a whole word */
    }
    free(d);
}

static void mkbook(const char *name, const char *f1, const char *f2, const char *f3) {
    char cmd[1200];
    snprintf(cmd, sizeof cmd, "mkdir -p '" DR "/lib/%s' && cp %s '" DR "/lib/%s/01 - part.mp3' && cp %s '" DR
             "/lib/%s/02 - part.mp3' && cp %s '" DR "/lib/%s/03 - part.mp3'", name, f1, name, f2, name, f3, name);
    sh(cmd);
}

static BookSummary *by_title(BookSummary *b, int n, const char *t) {
    for (int i = 0; i < n; i++) if (!strcmp(b[i].title, t)) return &b[i];
    return NULL;
}

static char *detail_desc(const BookSummary *b, int open) {
    BookDetail d;
    int rc = open ? scanner_open_detail_at(b, &d, 0, NULL, NULL) : scanner_load_detail(b, &d);
    if (rc != 0) return strdup("(load failed)");
    char *s = d.description ? strdup(d.description) : NULL;
    book_detail_free(&d);
    return s;
}

TEST(desc_multi_file_first_only) {
    make_fixtures();
    sh("rm -rf " DR "/lib && mkdir -p " DR "/lib");
    /* parts 2 and 3 carry their own (chapter) comments */
    sh(Q SINE(3) "-c:a libmp3lame -b:a 64k -metadata comment='The book description' " FXD "/p1.mp3");
    sh(Q SINE(3) "-c:a libmp3lame -b:a 64k -metadata comment='Chapter 2 note' " FXD "/p2.mp3");
    sh(Q SINE(3) "-c:a libmp3lame -b:a 64k -metadata comment='Chapter 3 note' " FXD "/p3.mp3");
    mkbook("First Has It", FXD "/p1.mp3", FXD "/p2.mp3", FXD "/p3.mp3");
    mkbook("Only Later Parts", FXD "/none.mp3", FXD "/p2.mp3", FXD "/p3.mp3");
    sh("cp " FXD "/c.m4b '" DR "/lib/Single M4B.m4b' && cp " FXD "/c.flac '" DR "/lib/Single FLAC.flac'");

    /* cold: the background scan (enrich + book file), as at library build */
    fresh_caches();
    BookSummary *books = NULL;
    int n = 0;
    CHECK_EQ_INT(scanner_scan(DR "/lib", &books, &n), 0);
    CHECK_EQ_INT(n, 4);
    BookSummary *a = by_title(books, n, "First Has It"), *b = by_title(books, n, "Only Later Parts");
    BookSummary *m = NULL, *fl = NULL;
    for (int i = 0; i < n; i++) {
        if (strstr(books[i].path, "Single M4B")) m = &books[i];
        if (strstr(books[i].path, "Single FLAC")) fl = &books[i];
    }
    CHECK(a && b && m && fl);
    if (!a || !b || !m || !fl) { free(books); return; }
    int hdr0 = scanner_mp3_header_reads();
    for (int i = 0; i < n; i++) {
        scanner_enrich(&books[i]);
        BookDetail d;
        if (scanner_load_detail(&books[i], &d) == 0) { scanner_save_detail(&books[i], &d); book_detail_free(&d); }
    }
    CHECK_EQ_INT(scanner_mp3_header_reads() - hdr0, 4); /* parts 2-3 of both books: still the fast header path */
    char *s;
    s = detail_desc(a, 0); CHECK_STR(s, "The book description"); free(s);
    s = detail_desc(b, 0); CHECK(s == NULL); free(s);   /* never a later part's comment */
    s = detail_desc(m, 0); CHECK_STR(s, "M4B comment wins"); free(s);
    s = detail_desc(fl, 0); CHECK_STR(s, "FLAC comment\n\nafter blank lines"); free(s);
    /* opening: straight from the book file (bookcache), same text */
    s = detail_desc(a, 1); CHECK_STR(s, "The book description"); free(s);
    s = detail_desc(m, 1); CHECK_STR(s, "M4B comment wins"); free(s);

    /* cold open before the scan (00.36 priority path): part 1 probed for its tags,
     * the other parts timed from their headers */
    fresh_caches();
    hdr0 = scanner_mp3_header_reads();
    int ff0 = scanner_ffmpeg_probes();
    s = detail_desc(a, 1); CHECK_STR(s, "The book description"); free(s);
    CHECK_EQ_INT(scanner_mp3_header_reads() - hdr0, 2);
    CHECK_EQ_INT(scanner_ffmpeg_probes() - ff0, 1);
    s = detail_desc(b, 1); CHECK(s == NULL); free(s);

    /* upgrade from 00.37: the first file's record has no "tags" and the book
     * file is version 1 -> only the first file is probed again */
    fresh_caches();
    for (int i = 0; i < n; i++) {
        scanner_enrich(&books[i]);
        BookDetail d;
        if (scanner_load_detail(&books[i], &d) == 0) { scanner_save_detail(&books[i], &d); book_detail_free(&d); }
    }
    sh("for f in " DR "/cache/probe/*.json; do sed -i -e 's/\"tags\":[^,}]*,\\?//' -e 's/\"description\":\"[^\"]*\",\\?//' \"$f\"; done");
    sh("for f in " DR "/cache/books/*.bin; do printf '\\001\\000\\000\\000' | dd of=\"$f\" bs=1 seek=4 conv=notrunc 2>/dev/null; done");
    hdr0 = scanner_mp3_header_reads();
    ff0 = scanner_ffmpeg_probes();
    BookSummary a2 = *a;
    scanner_enrich(&a2);
    CHECK_EQ_INT(scanner_ffmpeg_probes() - ff0, 1);   /* part 1 only */
    CHECK_EQ_INT(scanner_mp3_header_reads() - hdr0, 0); /* parts 2-3: cached lengths kept */
    s = detail_desc(&a2, 1); CHECK_STR(s, "The book description"); free(s);
    /* opening a not-yet-rescanned single-file book upgrades it too */
    ff0 = scanner_ffmpeg_probes();
    s = detail_desc(m, 1); CHECK_STR(s, "M4B comment wins"); free(s);
    CHECK_EQ_INT(scanner_ffmpeg_probes() - ff0, 1);
    ff0 = scanner_ffmpeg_probes();
    s = detail_desc(m, 1); CHECK_STR(s, "M4B comment wins"); free(s); /* now cached */
    CHECK_EQ_INT(scanner_ffmpeg_probes() - ff0, 0);
    free(books);
}

void suite_desc_host(void) {
    RUN(desc_probe_per_format);
    RUN(desc_multi_file_first_only);
}
