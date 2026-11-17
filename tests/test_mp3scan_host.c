/* 00.37 host tests (need FFmpeg): the background scan times MP3 parts from a
 * checked header and falls back to FFmpeg when the header cannot be trusted;
 * early cover publishing; a scan never blocks opening a book. */
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

#define FX "build-host/fixtures/mp3check"
#define BOOK FX "/lib/Checked Book"

static int sh(const char *cmd) {
    int rc = system(cmd);
    if (rc != 0) fprintf(stderr, "command failed (%d): %s\n", rc, cmd);
    return rc;
}

static int exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && st.st_size > 0;
}

static uint8_t *slurp(const char *p, size_t *n) {
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    uint8_t *b = malloc((size_t)sz);
    if (b && fread(b, 1, (size_t)sz, f) != (size_t)sz) { free(b); b = NULL; }
    fclose(f);
    *n = (size_t)sz;
    return b;
}

static int spit(const char *p, const uint8_t *b, size_t n) {
    FILE *f = fopen(p, "wb");
    if (!f) return -1;
    int ok = fwrite(b, 1, n, f) == n;
    return fclose(f) == 0 && ok ? 0 : -1;
}

/* VBRI: CBR frames (no Xing, no ID3) behind one extra frame that carries a
 * Fraunhofer VBRI header with the frame count, as FhG encoders write it. */
static int make_vbri(const char *src, const char *dst) {
    size_t n;
    uint8_t *b = slurp(src, &n);
    if (!b) return -1;
    size_t off = (size_t)mp3_id3v2_size(b, n);
    Mp3Frame f;
    if (off + 4 > n || mp3_parse_frame(b + off, &f) != 0) { free(b); return -1; }
    /* count the frames */
    uint32_t frames = 0;
    for (size_t i = off; i + 4 <= n;) {
        Mp3Frame g;
        if (mp3_parse_frame(b + i, &g) != 0) break;
        frames++;
        i += (size_t)g.frame_len;
    }
    size_t out_n = (size_t)f.frame_len + (n - off);
    uint8_t *o = calloc(1, out_n);
    memcpy(o, b + off, 4); /* same header as the first audio frame */
    o[2] &= (uint8_t)~0x02; /* no padding: exactly frame_len - pad */
    Mp3Frame h;
    mp3_parse_frame(o, &h);
    uint8_t *v = o + 4 + 32;
    memcpy(v, "VBRI", 4);
    v[4] = 0; v[5] = 1;              /* version 1 */
    uint32_t bytes = (uint32_t)(n - off);
    v[10] = (uint8_t)(bytes >> 24); v[11] = (uint8_t)(bytes >> 16); v[12] = (uint8_t)(bytes >> 8); v[13] = (uint8_t)bytes;
    v[14] = (uint8_t)(frames >> 24); v[15] = (uint8_t)(frames >> 16); v[16] = (uint8_t)(frames >> 8); v[17] = (uint8_t)frames;
    memcpy(o + h.frame_len, b + off, n - off);
    int rc = spit(dst, o, (size_t)h.frame_len + (n - off));
    free(o);
    free(b);
    return rc;
}

/* Damage: overwrite 24 KB after the ID3v2 tag with pseudo-random bytes. */
static int make_corrupt(const char *src, const char *dst) {
    size_t n;
    uint8_t *b = slurp(src, &n);
    if (!b) return -1;
    size_t off = (size_t)mp3_id3v2_size(b, n);
    uint32_t x = 12345;
    for (size_t i = off; i < off + 24576 && i < n; i++) {
        x = x * 1103515245u + 12345u;
        b[i] = (uint8_t)(x >> 16);
        if (b[i] == 0xFF) b[i] = 0x7F; /* no accidental frame syncs */
    }
    int rc = spit(dst, b, n);
    free(b);
    return rc;
}

/* Xing frame count overwritten with 3 (a damaged header: 0.08 s for 30 s of audio). */
static int make_bad_xing(const char *src, const char *dst) {
    size_t n;
    uint8_t *b = slurp(src, &n);
    if (!b) return -1;
    int rc = -1;
    for (size_t i = 0; i + 16 < n && i < 300000; i++) {
        if ((!memcmp(b + i, "Xing", 4) || !memcmp(b + i, "Info", 4)) && (b[i + 7] & 1)) {
            b[i + 8] = 0; b[i + 9] = 0; b[i + 10] = 0; b[i + 11] = 3;
            rc = spit(dst, b, n);
            break;
        }
    }
    free(b);
    return rc;
}

static int make_fixtures(void) {
    if (exists(BOOK "/07 - bigart.mp3")) return 0;
    /* half tone, half noise: a VBR encoder really varies the bitrate */
    const char *src = "-filter_complex 'sine=frequency=330:sample_rate=44100:duration=15[a];"
                      "anoisesrc=d=15:a=0.25:r=44100[b];[a][b]concat=n=2:v=0:a=1[out]' -map '[out]'";
    char cmd[2400];
    snprintf(cmd, sizeof cmd,
             "set -e; d=" FX "; mkdir -p $d '" BOOK "'; Q='-hide_banner -loglevel error -y'; "
             "ffmpeg $Q %s -c:a libmp3lame -b:a 128k -write_xing 0 $d/cbr_noxing.mp3; "
             "ffmpeg $Q %s -c:a libmp3lame -b:a 64k -ac 1 $d/cbr_info.mp3; "
             "ffmpeg $Q %s -c:a libmp3lame -q:a 4 $d/vbr_xing.mp3; "
             "ffmpeg $Q %s -c:a libmp3lame -q:a 4 -write_xing 0 $d/vbr_noxing.mp3; "
             "ffmpeg $Q -f lavfi -i 'nullsrc=s=1400x1400,geq=random(1)*255:random(2)*255:128' -frames:v 1 -q:v 1 $d/bigart.jpg; "
             "ffmpeg $Q -i $d/bigart.jpg %s -map 0:v -c:a libmp3lame -q:a 5 -c:v copy -id3v2_version 3 "
             "-disposition:v attached_pic $d/bigart.mp3; "
             "ffmpeg $Q -f lavfi -i 'nullsrc=s=600x600,geq=random(1)*255:128:random(2)*255' -frames:v 1 '" BOOK "/cover.jpg'",
             src, src, src, src, src);
    if (sh(cmd) != 0) return -1;
    if (make_vbri(FX "/cbr_noxing.mp3", FX "/vbri.mp3") != 0) return -1;
    if (make_corrupt(FX "/cbr_noxing.mp3", FX "/corrupt.mp3") != 0) return -1;
    if (make_bad_xing(FX "/vbr_xing.mp3", FX "/bad_xing.mp3") != 0) return -1;
    /* the book: first part VBR+Xing (probed for tags), then one of each kind */
    return sh("set -e; d=" FX "; b='" BOOK "'; cp $d/vbr_xing.mp3 \"$b/01 - first.mp3\"; "
              "cp $d/cbr_noxing.mp3 \"$b/02 - cbr.mp3\"; cp $d/cbr_info.mp3 \"$b/03 - info.mp3\"; "
              "cp $d/vbri.mp3 \"$b/04 - vbri.mp3\"; cp $d/vbr_noxing.mp3 \"$b/05 - vbr noxing.mp3\"; "
              "cp $d/corrupt.mp3 \"$b/06 - corrupt.mp3\"; cp $d/bigart.mp3 \"$b/07 - bigart.mp3\"");
}

static double ff_duration(const char *p) {
    MediaInfo m;
    if (stream_probe_ex(p, &m, STREAM_PROBE_QUICK, STREAM_PROBE_TIMEOUT_MS) != 0) return -1;
    double d = m.duration;
    media_info_free(&m);
    return d;
}

static void fresh(const char *root) {
    char cmd[400];
    snprintf(cmd, sizeof cmd, "rm -rf %s && mkdir -p %s/covers %s/probe %s/progress %s/books", root, root, root, root, root);
    sh(cmd);
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

/* Each kind: checked header == FFmpeg (tight), or the fallback triggers. */
TEST(checked_header_matches_ffmpeg_or_falls_back) {
    static const struct { const char *name; int ok; int exact; int why; } k[] = {
        { "cbr_noxing", 1, 0, 0 },
        { "cbr_info", 1, 1, 0 },
        { "vbr_xing", 1, 1, 0 },
        { "vbri", 1, 1, 0 },
        { "bigart", 1, 1, 0 },
        { "vbr_noxing", 0, 0, MP3_WHY_VBR_NO_TOC },
        { "corrupt", 0, 0, -1 },
        { "bad_xing", 0, 0, MP3_WHY_DURATION },
    };
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++) {
        char p[200];
        snprintf(p, sizeof p, FX "/%s.mp3", k[i].name);
        Mp3Info mi;
        int why = -9;
        int rc = mp3_fast_info_checked(p, &mi, &why);
        double ff = ff_duration(p);
        printf("  %-10s checked: %s%s%s  header %.3f s  FFmpeg %.3f s\n", k[i].name, rc == 0 ? "ok" : "fallback (",
               rc == 0 ? (mi.exact ? " (frame count)" : " (CBR, checked)") : mp3_why_str(why), rc == 0 ? "" : ")",
               rc == 0 ? mi.duration : 0.0, ff);
        CHECK(ff > 0);
        if (k[i].ok) {
            CHECK_EQ_INT(rc, 0);
            CHECK_EQ_INT(mi.exact, k[i].exact);
            CHECK_NEAR(mi.duration, ff, 0.01);
        } else {
            CHECK(rc != 0);
            if (k[i].why > 0) CHECK_EQ_INT(why, k[i].why);
            else CHECK(why == MP3_WHY_NOSYNC || why == MP3_WHY_FRAMES || why == MP3_WHY_SAMPLE);
        }
    }
    /* The ID3v2 art was skipped by seeking (not scanned). */
    Mp3Info mi;
    CHECK_EQ_INT(mp3_fast_info_checked(FX "/bigart.mp3", &mi, NULL), 0);
    CHECK(mi.audio_offset > 500000);
    /* Without the check, VBR without a TOC gives a wrong CBR estimate: 30 s of
     * audio, estimated from the first frame's bitrate. That is why it falls back. */
    CHECK_EQ_INT(mp3_fast_info(FX "/vbr_noxing.mp3", &mi), 0);
    printf("  vbr_noxing unchecked estimate %.1f s for 30.0 s of audio (FFmpeg estimates too: %.1f s)\n",
           mi.duration, ff_duration(FX "/vbr_noxing.mp3"));
    CHECK(fabs(mi.duration - 30.0) > 1.0);
}

static int g_early_calls;
static BookSummary g_early;
static double g_early_at_dur;
static void on_early(const BookSummary *b, void *ud) {
    (void)ud;
    g_early_calls++;
    g_early = *b;
}
static int g_cancel_after_early;
static int cancel_cb(void *ud) { (void)ud; return g_cancel_after_early && g_early_calls > 0; }

TEST(enrich_times_mp3_parts_from_headers_with_fallback) {
    fresh("build-host/tmp-mp3scan");
    BookSummary *books = NULL;
    int n = 0;
    CHECK_EQ_INT(scanner_scan(FX "/lib", &books, &n), 0);
    CHECK_EQ_INT(n, 1);
    if (n != 1) { free(books); return; }
    BookSummary b = books[0];
    free(books);
    CHECK(b.duration == 0); /* nothing cached: length pending, not a partial sum */

    const char *parts[] = { "01 - first", "02 - cbr", "03 - info", "04 - vbri", "05 - vbr noxing", "06 - corrupt",
                            "07 - bigart" };
    double want = 0;
    for (int i = 0; i < 7; i++) {
        char p[300];
        snprintf(p, sizeof p, BOOK "/%s.mp3", parts[i]);
        want += ff_duration(p);
    }
    int ff0 = scanner_ffmpeg_probes(), h0 = scanner_mp3_header_reads(), f0 = scanner_mp3_fallbacks();
    EnrichOpts o;
    memset(&o, 0, sizeof o);
    o.on_early = on_early;
    g_early_calls = 0;
    g_early_at_dur = -1;
    CHECK(scanner_enrich_ex(&b, &o) != SCANNER_CANCELLED);
    int ff = scanner_ffmpeg_probes() - ff0, hdr = scanner_mp3_header_reads() - h0, fb = scanner_mp3_fallbacks() - f0;
    printf("  7-part book: %d checked headers, %d fallbacks, %d FFmpeg probes; %.3f s vs FFmpeg sum %.3f s\n", hdr, fb,
           ff, b.duration, want);
    CHECK_EQ_INT(hdr, 4);  /* cbr, info, vbri, bigart */
    CHECK_EQ_INT(fb, 2);   /* vbr noxing, corrupt */
    CHECK_EQ_INT(ff, 3);   /* first part (tags) + the two fallbacks */
    CHECK_NEAR(b.duration, want, 0.05);
    /* early: once, with the folder cover and its thumb, length pending */
    CHECK_EQ_INT(g_early_calls, 1);
    CHECK(strstr(g_early.cover_path, "cover.jpg") != NULL);
    CHECK(g_early.thumb_path[0] != 0);
    CHECK(g_early.duration == 0);
    CHECK_STR(g_early.title, "Checked Book");
    CHECK_STR(b.thumb_path, g_early.thumb_path);

    /* again: everything from the probe cache */
    ff0 = scanner_ffmpeg_probes(); h0 = scanner_mp3_header_reads(); f0 = scanner_mp3_fallbacks();
    BookSummary b2 = b;
    b2.duration = 0;
    scanner_enrich_ex(&b2, NULL);
    CHECK_EQ_INT(scanner_ffmpeg_probes() - ff0, 0);
    CHECK_EQ_INT(scanner_mp3_header_reads() - h0, 0);
    CHECK_NEAR(b2.duration, b.duration, 1e-6);

    /* the detail (tracks) agrees with FFmpeg part by part */
    BookDetail d;
    CHECK_EQ_INT(scanner_load_detail(&b, &d), 0);
    CHECK_EQ_INT(d.ntracks, 7);
    for (int i = 0; i < d.ntracks && i < 7; i++) CHECK_NEAR(d.tracks[i].duration, ff_duration(d.tracks[i].path), 0.01);
    CHECK_EQ_INT(scanner_ffmpeg_probes() - ff0, 0);
    book_detail_free(&d);

    /* cache records say how the length was found */
    ProbeCacheEnt e;
    struct stat st;
    stat(BOOK "/04 - vbri.mp3", &st);
    CHECK_EQ_INT(probecache_get(BOOK "/04 - vbri.mp3", (uint64_t)st.st_size, (uint64_t)st.st_mtime * 1000ULL, &e), 0);
    CHECK_EQ_INT(e.dur_only, 1);
    CHECK_EQ_INT(e.hdr, PROBE_HDR_EXACT);
    probecache_ent_free(&e);
    stat(BOOK "/02 - cbr.mp3", &st);
    CHECK_EQ_INT(probecache_get(BOOK "/02 - cbr.mp3", (uint64_t)st.st_size, (uint64_t)st.st_mtime * 1000ULL, &e), 0);
    CHECK_EQ_INT(e.hdr, PROBE_HDR_CBR);
    probecache_ent_free(&e);
    stat(BOOK "/05 - vbr noxing.mp3", &st);
    CHECK_EQ_INT(probecache_get(BOOK "/05 - vbr noxing.mp3", (uint64_t)st.st_size, (uint64_t)st.st_mtime * 1000ULL, &e), 0);
    CHECK_EQ_INT(e.dur_only, 0); /* FFmpeg record */
    probecache_ent_free(&e);

    /* a 00.36 record (unchecked header read) is redone with the check */
    stat(BOOK "/03 - info.mp3", &st);
    memset(&e, 0, sizeof e);
    snprintf(e.path, sizeof e.path, BOOK "/03 - info.mp3");
    e.size = (uint64_t)st.st_size;
    e.mtime_ms = (uint64_t)st.st_mtime * 1000ULL;
    e.duration = 999;
    e.ver = PROBE_CACHE_VER;
    e.dur_only = 1;
    probecache_put(&e);
    h0 = scanner_mp3_header_reads();
    b2.duration = 0;
    scanner_enrich_ex(&b2, NULL);
    CHECK_EQ_INT(scanner_mp3_header_reads() - h0, 1);
    CHECK_NEAR(b2.duration, b.duration, 1e-6);

    /* Refresh / folder change: stops between files, after the early cover */
    fresh("build-host/tmp-mp3scan2");
    BookSummary b3 = b;
    b3.duration = 0;
    b3.thumb_path[0] = 0;
    g_early_calls = 0;
    g_cancel_after_early = 1;
    o.cancel = cancel_cb;
    ff0 = scanner_ffmpeg_probes(); h0 = scanner_mp3_header_reads();
    CHECK_EQ_INT(scanner_enrich_ex(&b3, &o), SCANNER_CANCELLED);
    CHECK_EQ_INT(g_early_calls, 1);
    CHECK_EQ_INT(scanner_ffmpeg_probes() - ff0, 1);   /* only the first part */
    CHECK_EQ_INT(scanner_mp3_header_reads() - h0, 0);
    g_cancel_after_early = 0;
}

/* Early cover reaches the partial catalog as a pending entry. */
static int g_pending_seen;
static void count_pending(const LibraryCache *c, void *ud) {
    (void)ud;
    for (int i = 0; i < c->n; i++)
        if (c->books[i].ntracks == 0 && c->books[i].sum.duration == 0 && c->books[i].sum.thumb_path[0]) g_pending_seen++;
}
static int g_early_books;
static void cat_early(BookSummary *b, void *ud) { (void)ud; if (b->duration == 0 && b->thumb_path[0]) g_early_books++; }

TEST(partial_catalog_has_pending_book_with_cover) {
    fresh("build-host/tmp-mp3scan3");
    BookSummary *books = NULL;
    int n = 0;
    CHECK_EQ_INT(scanner_scan(FX "/lib", &books, &n), 0);
    CatBuildOpts o;
    memset(&o, 0, sizeof o);
    o.root = FX "/lib";
    o.cache_path = "build-host/tmp-mp3scan3/lib.json";
    o.save_every_ms = 0;
    o.on_partial = count_pending;
    o.on_early = cat_early;
    g_pending_seen = 0;
    g_early_books = 0;
    LibraryCache out;
    CatBuildStats st;
    CHECK_EQ_INT(catbuild_run(books, n, &o, &out, &st), 0);
    CHECK_EQ_INT(g_early_books, 1);
    CHECK_EQ_INT(g_pending_seen, 1);
    CHECK_EQ_INT(out.partial, 0);
    CHECK_EQ_INT(out.n, 1);
    CHECK(out.books[0].ntracks == 7 && out.books[0].sum.duration > 0);
    libcache_free(&out);
    free(books);
}

/* Opening a book while a (re)scan lists the folder: no waiting on it. */
static volatile int g_scan_done;
static void *scan_thread(void *arg) {
    BookSummary *books = NULL;
    int n = 0;
    scanner_scan((const char *)arg, &books, &n);
    free(books);
    g_scan_done = 1;
    return NULL;
}

TEST(open_does_not_wait_for_rescan) {
    fresh("build-host/tmp-mp3scan4");
    BookSummary *books = NULL;
    int n = 0;
    CHECK_EQ_INT(scanner_scan(FX "/lib", &books, &n), 0);
    if (n < 1) { free(books); return; }
    BookSummary b = books[0];
    free(books);
    /* a slow rescan of the big simulated library (20 ms per file) */
    scanner_test_set_io_delay(20);
    g_scan_done = 0;
    pthread_t th;
    pthread_create(&th, NULL, scan_thread, (void *)"build-host/fixtures/lesmis_sim");
    sleep_ms(100);
    scanner_test_set_io_delay(0);
    uint64_t t0 = now_ms();
    BookDetail d;
    int rc = scanner_open_detail_at(&b, &d, 0, NULL, NULL);
    uint64_t dt = now_ms() - t0;
    int scan_still_running = !g_scan_done;
    printf("  open during a rescan: %llu ms (rescan still running: %s)\n", (unsigned long long)dt,
           scan_still_running ? "yes" : "no");
    CHECK_EQ_INT(rc, 0);
    CHECK(dt < 2000);
    if (rc == 0) book_detail_free(&d);
    scanner_test_set_io_delay(0);
    pthread_join(th, NULL);
}

/* The 354-part simulated multi-file book, cold cache: the background enrich
 * (00.36: one 1 MB FFmpeg probe per part, ~120-200 ms each on the Vita). */
TEST(enrich_354_parts_cold) {
    fresh("build-host/tmp-mp3scan5");
    BookSummary *books = NULL;
    int n = 0;
    CHECK_EQ_INT(scanner_scan("build-host/fixtures/lesmis_sim", &books, &n), 0);
    if (n != 1) { free(books); CHECK(0); return; }
    BookSummary b = books[0];
    free(books);
    int ff0 = scanner_ffmpeg_probes(), h0 = scanner_mp3_header_reads(), f0 = scanner_mp3_fallbacks();
    uint64_t t0 = now_ms();
    g_early_calls = 0;
    EnrichOpts o;
    memset(&o, 0, sizeof o);
    o.on_early = on_early;
    scanner_enrich_ex(&b, &o);
    uint64_t dt = now_ms() - t0;
    int ff = scanner_ffmpeg_probes() - ff0, hdr = scanner_mp3_header_reads() - h0, fb = scanner_mp3_fallbacks() - f0;
    printf("  354-part book, cold enrich: %d checked headers, %d fallbacks, %d FFmpeg probes, %llu ms on the host "
           "(00.36: 354 FFmpeg probes); %.0f s total\n", hdr, fb, ff, (unsigned long long)dt, b.duration);
    CHECK_EQ_INT(ff, 1);
    CHECK_EQ_INT(hdr, 353);
    CHECK_EQ_INT(fb, 0);
    CHECK_EQ_INT(g_early_calls, 1);
    CHECK(g_early.thumb_path[0] != 0); /* the cover from the first part's tag */
    /* same total as the exact detail load */
    BookDetail d;
    CHECK_EQ_INT(scanner_load_detail(&b, &d), 0);
    double sum = 0;
    for (int i = 0; i < d.ntracks; i++) sum += d.tracks[i].duration;
    CHECK_NEAR(sum, b.duration, 1e-6);
    CHECK_NEAR(b.duration, 118.0 * (7.0 + 9.0) + 118.0 * 11.076, 2.0);
    book_detail_free(&d);
}

void suite_mp3scan_host(void) {
    printf("suite_mp3scan_host\n");
    if (make_fixtures() != 0) {
        CHECK(0);
        return;
    }
    RUN(checked_header_matches_ffmpeg_or_falls_back);
    RUN(enrich_times_mp3_parts_from_headers_with_fallback);
    RUN(partial_catalog_has_pending_book_with_cover);
    RUN(open_does_not_wait_for_rescan);
    RUN(enrich_354_parts_cold);
}
