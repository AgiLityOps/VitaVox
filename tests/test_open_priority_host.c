/* 00.36 host tests (need FFmpeg): opening a 354-part MP3 book on a first scan
 * (priority over enrich, quick start with estimates, refine), saving the
 * catalog during the scan and resuming it, and fast cancel at shutdown. */
#include "test.h"
#include "bookcache.h"
#include "catbuild.h"
#include "dbg.h"
#include "libcache.h"
#include "models.h"
#include "mp3info.h"
#include "osal.h"
#include "probecache.h"
#include "progress.h"
#include "scanner.h"
#include "stream.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define SIM_ROOT "build-host/fixtures/lesmis_sim"
#define SIM_BOOK SIM_ROOT "/Les Miserables"
#define SIM_PARTS 354

static int run_cmd(const char *cmd) {
    int rc = system(cmd);
    if (rc != 0) fprintf(stderr, "command failed (%d): %s\n", rc, cmd);
    return rc;
}

static int file_ok(const char *path) {
    struct stat st;
    return path && path[0] && stat(path, &st) == 0 && st.st_size > 8;
}

static void fresh_cache(const char *root) {
    char cmd[400];
    snprintf(cmd, sizeof cmd, "rm -rf %s && mkdir -p %s/covers %s/probe %s/progress %s/books", root, root, root, root, root);
    run_cmd(cmd);
    char p[320];
    snprintf(p, sizeof p, "%s/debug.log", root);
    dbg_init(p);
    snprintf(p, sizeof p, "%s/covers", root);
    scanner_set_cover_dir(p);
    snprintf(p, sizeof p, "%s/probe", root);
    probecache_init(p);
    snprintf(p, sizeof p, "%s/progress", root);
    progress_init(p);
    snprintf(p, sizeof p, "%s/books", root);
    bookcache_init(p);
}

/* 354 parts like a long multi-file book: an ID3v2 tag with ~125 KB of cover art in
 * every file, then a few seconds of mono MP3. Three kinds rotate: LAME VBR with
 * a Xing header, CBR with an Info header, and CBR without any (bitrate estimate). */
static int make_sim_book(void) {
    char last[400];
    snprintf(last, sizeof last, SIM_BOOK "/%04d - Part %d - Les Miserables.mp3", SIM_PARTS, SIM_PARTS);
    if (file_ok(last)) return 0;
    const char *cmd =
        "set -e; d=build-host/fixtures/lesmis_src; mkdir -p $d '" SIM_BOOK "'; "
        "ffmpeg -hide_banner -loglevel error -y -f lavfi -i 'nullsrc=s=520x520,geq=random(1)*255:random(2)*255:128' "
        "-frames:v 1 -q:v 2 $d/art.jpg; "
        "ffmpeg -hide_banner -loglevel error -y -f lavfi -i sine=frequency=220:sample_rate=22050:duration=7 "
        "-i $d/art.jpg -map 0 -map 1 -c:a libmp3lame -q:a 6 -ac 1 -c:v copy -id3v2_version 3 "
        "-disposition:v attached_pic $d/vbr.mp3; "
        "ffmpeg -hide_banner -loglevel error -y -f lavfi -i sine=frequency=330:sample_rate=22050:duration=9 "
        "-i $d/art.jpg -map 0 -map 1 -c:a libmp3lame -b:a 32k -ac 1 -c:v copy -id3v2_version 3 "
        "-disposition:v attached_pic $d/cbr.mp3; "
        "ffmpeg -hide_banner -loglevel error -y -f lavfi -i sine=frequency=440:sample_rate=22050:duration=11 "
        "-i $d/art.jpg -map 0 -map 1 -c:a libmp3lame -b:a 32k -ac 1 -write_xing 0 -c:v copy -id3v2_version 3 "
        "-disposition:v attached_pic $d/noxing.mp3; "
        "i=1; while [ $i -le 354 ]; do case $((i % 3)) in 0) s=vbr;; 1) s=cbr;; 2) s=noxing;; esac; "
        "cp $d/$s.mp3 \"" SIM_BOOK "/$(printf %04d $i) - Part $i - Les Miserables.mp3\"; "
        "i=$((i+1)); done";
    return run_cmd(cmd);
}

static int find_book(const char *root, BookSummary *out) {
    BookSummary *books = NULL;
    int n = 0;
    int rc = scanner_scan(root, &books, &n);
    if (rc == 0 && n >= 1) *out = books[0];
    free(books);
    return rc == 0 && n >= 1 ? 0 : -1;
}

/* Fast header durations agree with FFmpeg's for all three kinds. */
TEST(mp3_header_durations_match_ffmpeg) {
    const char *kinds[] = { "vbr", "cbr", "noxing" };
    for (int k = 0; k < 3; k++) {
        char p[200];
        snprintf(p, sizeof p, "build-host/fixtures/lesmis_src/%s.mp3", kinds[k]);
        Mp3Info mi;
        MediaInfo m;
        CHECK_EQ_INT(mp3_fast_info(p, &mi), 0);
        CHECK_EQ_INT(stream_probe_ex(p, &m, STREAM_PROBE_QUICK, STREAM_PROBE_TIMEOUT_MS), 0);
        double tol = m.duration * 0.01 > 0.1 ? m.duration * 0.01 : 0.1;
        printf("  %-7s header %.3f s (%s, data at %llu) vs FFmpeg %.3f s\n", kinds[k], mi.duration,
               mi.exact ? "frame count" : "bitrate", (unsigned long long)mi.audio_offset, m.duration);
        CHECK(mi.audio_offset > 100000); /* the cover art tag was skipped, not scanned */
        CHECK_NEAR(mi.duration, m.duration, tol);
        CHECK_EQ_INT(mi.exact, k < 2);
        CHECK_NEAR(mi.duration, m.duration, 0.002); /* LAME delay/padding taken off like FFmpeg does */
        media_info_free(&m);
    }
    /* the sample-library fixture parts too (no art) */
    MediaInfo m;
    Mp3Info mi;
    const char *u = "build-host/fixtures/sample_library/Les Miserables/"
                    "0102 - Preface - Les Miserables.mp3";
    CHECK_EQ_INT(mp3_fast_info(u, &mi), 0);
    CHECK_EQ_INT(stream_probe_ex(u, &m, STREAM_PROBE_QUICK, STREAM_PROBE_TIMEOUT_MS), 0);
    CHECK_NEAR(mi.duration, m.duration, 0.1);
    media_info_free(&m);
}

/* Cold first scan, nothing cached: opening the 354-part book. */
TEST(open_big_mp3_book_quick_then_refined) {
    fresh_cache("build-host/tmp-open354");
    BookSummary b;
    CHECK_EQ_INT(find_book(SIM_ROOT, &b), 0);

    /* Before 00.36: one FFmpeg quick probe per part, in order (what the log showed). */
    uint64_t t0 = now_ms();
    int p0 = scanner_ffmpeg_probes();
    BookDetail old;
    CHECK_EQ_INT(scanner_load_detail(&b, &old), 0);
    uint64_t t_old = now_ms() - t0;
    int old_probes = scanner_ffmpeg_probes() - p0;
    CHECK_EQ_INT(old.ntracks, SIM_PARTS);
    CHECK_EQ_INT(old.provisional, 0);
    fresh_cache("build-host/tmp-open354"); /* cold again */

    /* 00.36 with a Vita-like per-file cost (12 ms per opened part). */
    scanner_test_set_io_delay(12);
    t0 = now_ms();
    BookDetail d;
    CHECK_EQ_INT(scanner_open_detail_at(&b, &d, 0, NULL, NULL), 0);
    uint64_t t_quick = now_ms() - t0;
    scanner_test_set_io_delay(0);
    CHECK_EQ_INT(d.ntracks, SIM_PARTS);
    CHECK(d.provisional > 300);                       /* most parts estimated ... */
    CHECK(d.tracks[0].duration > 0);                  /* ... the start part exact */
    CHECK_NEAR(d.tracks[0].duration, old.tracks[0].duration, 0.15);
    CHECK(t_quick < SCANNER_OPEN_BUDGET_MS + 250);
    BookDetail none;
    CHECK(bookcache_load(&b, &none) != 0);            /* not saved with estimates */
    double err_total = fabs(d.duration - old.duration) / old.duration;
    printf("  354 parts, cold: old sequential probes %llu ms (%d FFmpeg probes, host); 00.36 quick open %llu ms "
           "with 12 ms/part simulated I/O: %d exact, %d estimated, total %.0f s est vs %.0f s (%.1f%% off)\n",
           (unsigned long long)t_old, old_probes, (unsigned long long)t_quick, d.ntracks - d.provisional, d.provisional,
           d.duration, old.duration, err_total * 100);
    CHECK(err_total < 0.25);

    /* Refine (while it plays): exact lengths, header reads only, book file saved. */
    p0 = scanner_ffmpeg_probes();
    t0 = now_ms();
    BookDetail r;
    CHECK_EQ_INT(scanner_refine_detail(&b, &r, NULL, NULL), 0);
    uint64_t t_ref = now_ms() - t0;
    CHECK_EQ_INT(scanner_ffmpeg_probes() - p0, 0);
    CHECK_EQ_INT(r.provisional, 0);
    CHECK_EQ_INT(r.ntracks, SIM_PARTS);
    int bad = 0;
    for (int i = 0; i < r.ntracks && i < old.ntracks; i++)
        if (fabs(r.tracks[i].duration - old.tracks[i].duration) > 0.15) bad++;
    CHECK_EQ_INT(bad, 0);
    CHECK_NEAR(r.duration, old.duration, SIM_PARTS * 0.06);
    CHECK_EQ_INT(r.nchapters, SIM_PARTS);
    printf("  refine: %llu ms on the host, 0 FFmpeg probes\n", (unsigned long long)t_ref);
    BookDetail again;
    t0 = now_ms();
    CHECK_EQ_INT(scanner_open_detail(&b, &again), 0);  /* now the book file */
    CHECK(now_ms() - t0 < 100);
    CHECK_EQ_INT(again.provisional, 0);
    CHECK_NEAR(again.duration, r.duration, 0.01);
    book_detail_free(&again);

    /* The probe cache has every part. 00.38: opening already probed the first
     * part for its tags (description, title, art), so enrich probes nothing. */
    ProbeCacheEnt e;
    char p[400];
    snprintf(p, sizeof p, "%s", r.tracks[200].path);
    struct stat st;
    stat(p, &st);
    CHECK_EQ_INT(probecache_get(p, (uint64_t)st.st_size, (uint64_t)st.st_mtime * 1000ULL, &e), 0);
    CHECK_EQ_INT(e.dur_only, 1);
    probecache_ent_free(&e);
    p0 = scanner_ffmpeg_probes();
    BookSummary eb = b;
    scanner_enrich(&eb);
    CHECK_EQ_INT(scanner_ffmpeg_probes() - p0, 0);
    CHECK(eb.cover_path[0] != 0);                      /* art from the first part */
    CHECK_NEAR(eb.duration, r.duration, 0.5);
    book_detail_free(&old);
    book_detail_free(&d);
    book_detail_free(&r);
}

/* Resume deep in the book: every part up to the position is timed exactly. */
TEST(open_big_mp3_book_resume_position_exact) {
    fresh_cache("build-host/tmp-open354");
    BookSummary b;
    CHECK_EQ_INT(find_book(SIM_ROOT, &b), 0);
    BookDetail ex;
    CHECK_EQ_INT(scanner_refine_detail(&b, &ex, NULL, NULL), 0);
    double start = ex.tracks[120].start_offset + 3.0;
    fresh_cache("build-host/tmp-open354");
    DetailOpts o;
    memset(&o, 0, sizeof o);
    o.priority = 1;
    o.start = start;
    o.budget_ms = 0; /* estimate as soon as allowed */
    BookDetail d;
    CHECK_EQ_INT(scanner_load_detail_opts(&b, &d, &o), 0);
    CHECK(d.provisional > 0 && d.provisional < SIM_PARTS - 120);
    for (int i = 0; i <= 120; i++) CHECK_NEAR(d.tracks[i].start_offset, ex.tracks[i].start_offset, 0.01);
    book_detail_free(&d);
    book_detail_free(&ex);
}

/* ---- priority over a running enrich ----------------------------------------- */

static struct {
    BookSummary b;
    volatile int done;
    volatile uint64_t t_done;
    volatile int probes_during;
} BG;

static void *enrich_thread(void *arg) {
    (void)arg;
    scanner_enrich(&BG.b);
    BG.t_done = now_ms();
    BG.done = 1;
    return NULL;
}

TEST(open_book_jumps_ahead_of_enrich) {
    fresh_cache("build-host/tmp-prio");
    /* enrich runs over the other 354-part copy (the sample-library fixture) */
    BookSummary *books = NULL;
    int n = 0;
    CHECK_EQ_INT(scanner_scan("build-host/fixtures/sample_library", &books, &n), 0);
    for (int i = 0; i < n; i++)
        if (strstr(books[i].path, "Les Miserables")) BG.b = books[i];
    free(books);
    CHECK(strstr(BG.b.path, "Les Miserables") != NULL);
    BookSummary open_b;
    CHECK_EQ_INT(find_book(SIM_ROOT, &open_b), 0);

    scanner_test_set_io_delay(12);
    BG.done = 0;
    pthread_t th;
    pthread_create(&th, NULL, enrich_thread, NULL);
    sleep_ms(300); /* enrich is well under way */
    int before = scanner_ffmpeg_probes();
    uint64_t waited0 = scanner_priority_waited_ms();
    uint64_t t0 = now_ms();
    BookDetail d;
    CHECK_EQ_INT(scanner_open_detail_at(&open_b, &d, 0, NULL, NULL), 0);
    uint64_t t_open = now_ms() - t0;
    int during = scanner_ffmpeg_probes() - before;
    CHECK(!BG.done);
    /* the enrich made at most the one probe it was in the middle of */
    CHECK(during <= 1);
    /* refine with enrich still waiting */
    before = scanner_ffmpeg_probes();
    BookDetail r;
    t0 = now_ms();
    CHECK_EQ_INT(scanner_refine_detail(&open_b, &r, NULL, NULL), 0);
    uint64_t t_ref = now_ms() - t0;
    CHECK(scanner_ffmpeg_probes() - before <= 2);
    CHECK(scanner_priority_waited_ms() > waited0);
    printf("  open during enrich: details in %llu ms (12 ms/part simulated), enrich probes meanwhile: %d; refine %llu ms\n",
           (unsigned long long)t_open, during, (unsigned long long)t_ref);
    CHECK(t_open < SCANNER_OPEN_BUDGET_MS + 450);
    scanner_test_set_io_delay(0);
    /* enrich carries on afterwards and finishes */
    pthread_join(th, NULL);
    CHECK(BG.done);
    CHECK(BG.b.duration > 0);
    book_detail_free(&d);
    book_detail_free(&r);
}

/* ---- fast exit ------------------------------------------------------------------ */

static struct {
    BookSummary b;
    volatile int rc;
    volatile uint64_t t_done;
} CX;

static void *detail_thread(void *arg) {
    (void)arg;
    BookDetail d;
    CX.rc = scanner_load_detail(&CX.b, &d);
    CX.t_done = now_ms();
    if (CX.rc == 0) book_detail_free(&d);
    return NULL;
}

static void *enrich_only_thread(void *arg) {
    (void)arg;
    CX.rc = scanner_enrich(&CX.b);
    CX.t_done = now_ms();
    return NULL;
}

static volatile int g_cancel_at;
static int cancel_after(void *ud) { (void)ud; return g_cancel_at && now_ms() >= (uint64_t)g_cancel_at; }

TEST(shutdown_cancels_scan_and_probes_fast) {
    BookSummary b;
    CHECK_EQ_INT(find_book(SIM_ROOT, &b), 0);
    /* 1) background detail load with FFmpeg probes (the path that ran on at exit) */
    fresh_cache("build-host/tmp-cancel");
    CX.b = b;
    CX.t_done = 0;
    scanner_test_set_io_delay(5);
    pthread_t th;
    pthread_create(&th, NULL, detail_thread, NULL);
    sleep_ms(400);
    uint64_t t0 = now_ms();
    scanner_cancel(1); /* what main.c does at shutdown, beside stream_request_cancel */
    pthread_join(th, NULL);
    uint64_t dt1 = CX.t_done - t0;
    CHECK_EQ_INT(CX.rc, SCANNER_CANCELLED);
    CHECK(dt1 < 300);
    scanner_cancel(0);
    /* 2) enrich */
    fresh_cache("build-host/tmp-cancel");
    CX.b = b;
    pthread_create(&th, NULL, enrich_only_thread, NULL);
    sleep_ms(400);
    t0 = now_ms();
    scanner_cancel(1);
    pthread_join(th, NULL);
    uint64_t dt2 = CX.t_done - t0;
    CHECK(dt2 < 300);
    scanner_cancel(0);
    /* 3) refine of an open book, cancelled by its callback (player closed) */
    fresh_cache("build-host/tmp-cancel");
    g_cancel_at = (int)(now_ms() + 200);
    BookDetail d;
    t0 = now_ms();
    int rc = scanner_refine_detail(&b, &d, cancel_after, NULL);
    uint64_t dt3 = now_ms() - (uint64_t)g_cancel_at;
    g_cancel_at = 0;
    CHECK_EQ_INT(rc, SCANNER_CANCELLED);
    CHECK(dt3 < 100);
    char bin[400];
    snprintf(bin, sizeof bin, "build-host/tmp-cancel/books/%s.bin", b.id);
    CHECK(!file_ok(bin)); /* nothing half-done saved */
    scanner_test_set_io_delay(0);
    /* 4) a probe asked for after the process-wide cancel returns at once, without the file: retry */
    stream_request_cancel(1);
    MediaInfo m;
    t0 = now_ms();
    CHECK(stream_probe_ex("build-host/fixtures/lesmis_src/cbr.mp3", &m, STREAM_PROBE_QUICK, STREAM_PROBE_TIMEOUT_MS) != 0);
    CHECK(now_ms() - t0 < 20);
    stream_request_cancel(0);
    CHECK_EQ_INT(stream_probe_ex("build-host/fixtures/lesmis_src/cbr.mp3", &m, STREAM_PROBE_QUICK, STREAM_PROBE_TIMEOUT_MS), 0);
    media_info_free(&m);
    printf("  cancel -> stopped: detail load %llu ms, enrich %llu ms, refine %llu ms (watchdog is 4000 ms)\n",
           (unsigned long long)dt1, (unsigned long long)dt2, (unsigned long long)dt3);
}

/* ---- catalog saved during the first scan, resumed next start -------------------- */

static int g_saves_seen, g_stop_after;
/* 00.37: partial saves also happen when a book's cover is known before its
 * lengths (a pending entry): count the books that are complete. */
static void on_partial_count(const LibraryCache *c, void *ud) {
    (void)ud;
    int done = 0;
    for (int i = 0; i < c->n; i++) done += c->books[i].ntracks > 0;
    g_saves_seen = done;
}
static int stop_after_saves(void *ud) { (void)ud; return g_stop_after && g_saves_seen >= g_stop_after; }

TEST(catalog_saved_during_scan_and_resumed) {
    fresh_cache("build-host/tmp-catbuild");
    const char *root = "build-host/fixtures/sample_library";
    const char *cat_path = "build-host/tmp-catbuild/lib.json";
    BookSummary *books = NULL;
    int n = 0;
    CHECK_EQ_INT(scanner_scan(root, &books, &n), 0);
    CHECK(n >= 10);
    /* first scan, quit after 4 books */
    CatBuildOpts o;
    memset(&o, 0, sizeof o);
    o.root = root;
    o.cache_path = cat_path;
    o.save_every_ms = 0;
    o.cancel = stop_after_saves;
    o.on_partial = on_partial_count;
    g_saves_seen = 0;
    g_stop_after = 4;
    LibraryCache out;
    CatBuildStats st;
    CHECK_EQ_INT(catbuild_run(books, n, &o, &out, &st), SCANNER_CANCELLED);
    CHECK_EQ_INT(st.enriched, 4);
    CHECK(st.partial_saves >= 4);
    LibraryCache part;
    CHECK_EQ_INT(libcache_load(cat_path, &part), 0);
    CHECK_EQ_INT(part.partial, 1);
    CHECK(part.n == 4 || part.n == 5);
    for (int i = 0; i < 4 && i < part.n; i++) CHECK(part.books[i].ntracks > 0 && part.books[i].sum.duration > 0);
    if (part.n == 5) /* the book being timed when it stopped: pending, not complete */
        CHECK(part.books[4].ntracks == 0 && part.books[4].sum.duration == 0 && part.books[4].sum.title[0]);
    free(books);

    /* next start: filename scan again, the partial catalog is reused */
    CHECK_EQ_INT(scanner_scan(root, &books, &n), 0);
    g_stop_after = 0;
    o.resume = &part;
    o.save_every_ms = CATBUILD_SAVE_EVERY_MS;
    int p0 = scanner_ffmpeg_probes();
    CHECK_EQ_INT(catbuild_run(books, n, &o, &out, &st), 0);
    CHECK_EQ_INT(st.reused, 4);
    CHECK_EQ_INT(st.enriched, n - 4);
    CHECK_EQ_INT(out.n, n);
    CHECK_EQ_INT(out.partial, 0);
    int resumed_probes = scanner_ffmpeg_probes() - p0;
    LibraryCache full;
    CHECK_EQ_INT(libcache_load(cat_path, &full), 0);
    CHECK_EQ_INT(full.partial, 0);
    CHECK_EQ_INT(full.n, n);

    /* same result as one uninterrupted build */
    fresh_cache("build-host/tmp-catbuild2");
    free(books);
    CHECK_EQ_INT(scanner_scan(root, &books, &n), 0);
    CatBuildOpts o2;
    memset(&o2, 0, sizeof o2);
    o2.root = root;
    o2.cache_path = "build-host/tmp-catbuild2/lib.json";
    o2.save_every_ms = CATBUILD_SAVE_EVERY_MS;
    LibraryCache ref;
    p0 = scanner_ffmpeg_probes();
    CHECK_EQ_INT(catbuild_run(books, n, &o2, &ref, &st), 0);
    int full_probes = scanner_ffmpeg_probes() - p0;
    CHECK_EQ_INT(ref.n, out.n);
    int same = 1;
    for (int i = 0; i < ref.n && i < out.n; i++) {
        if (strcmp(ref.books[i].sum.id, out.books[i].sum.id) || ref.books[i].ntracks != out.books[i].ntracks
            || fabs(ref.books[i].sum.duration - out.books[i].sum.duration) > 0.01
            || strcmp(ref.books[i].sum.title, out.books[i].sum.title))
            same = 0;
    }
    CHECK(same);
    printf("  partial catalog: 4 books saved before quitting; resume reused 4, enriched %d (%d probes vs %d for a "
           "full build in a cold cache)\n", n - 4, resumed_probes, full_probes);
    CHECK(resumed_probes < full_probes);
    libcache_free(&ref);
    libcache_free(&full);
    libcache_free(&out);
    libcache_free(&part);
    free(books);
}

void suite_open_priority_host(void) {
    printf("suite_open_priority_host\n");
    if (make_sim_book() != 0) {
        CHECK(0);
        return;
    }
    RUN(mp3_header_durations_match_ffmpeg);
    RUN(open_big_mp3_book_quick_then_refined);
    RUN(open_big_mp3_book_resume_position_exact);
    RUN(open_book_jumps_ahead_of_enrich);
    RUN(shutdown_cancels_scan_and_probes_fast);
    RUN(catalog_saved_during_scan_and_resumed);
}
