/* 00.36: fast MP3 durations (header parse), probe-cache duration-only records
 * and the partial-catalog flag. Pure C, no FFmpeg. */
#include "test.h"
#include "libcache.h"
#include "mp3info.h"
#include "probecache.h"

#include <stdlib.h>
#include <sys/stat.h>

/* MPEG-1 Layer III, 128 kb/s, 44.1 kHz, stereo, no padding: FF FB 90 00, 417 bytes. */
static void put_frame(uint8_t *p, uint8_t b2, uint8_t b3) { p[0] = 0xFF; p[1] = 0xFB; p[2] = b2; p[3] = b3; }

TEST(mp3_frame_header) {
    uint8_t h[4];
    Mp3Frame f;
    put_frame(h, 0x90, 0x00);
    CHECK_EQ_INT(mp3_parse_frame(h, &f), 0);
    CHECK_EQ_INT(f.version, 1);
    CHECK_EQ_INT(f.layer, 3);
    CHECK_EQ_INT(f.bitrate, 128000);
    CHECK_EQ_INT(f.sample_rate, 44100);
    CHECK_EQ_INT(f.channels, 2);
    CHECK_EQ_INT(f.samples_per_frame, 1152);
    CHECK_EQ_INT(f.frame_len, 417);
    /* MPEG-2 Layer III 32 kb/s 22.05 kHz mono: FF F3 48 C0 */
    uint8_t m2[4] = {0xFF, 0xF3, 0x40, 0xC0};
    CHECK_EQ_INT(mp3_parse_frame(m2, &f), 0);
    CHECK_EQ_INT(f.version, 2);
    CHECK_EQ_INT(f.sample_rate, 22050);
    CHECK_EQ_INT(f.bitrate, 32000);
    CHECK_EQ_INT(f.channels, 1);
    CHECK_EQ_INT(f.samples_per_frame, 576);
    CHECK_EQ_INT(f.frame_len, 72 * 32000 / 22050);
    /* bad: bitrate index 15, reserved rate, no sync */
    put_frame(h, 0xF0, 0x00);
    CHECK(mp3_parse_frame(h, &f) != 0);
    put_frame(h, 0x9C, 0x00);
    CHECK(mp3_parse_frame(h, &f) != 0);
    uint8_t z[4] = {0, 0, 0, 0};
    CHECK(mp3_parse_frame(z, &f) != 0);
}

TEST(mp3_id3_size) {
    uint8_t t[10] = {'I', 'D', '3', 3, 0, 0, 0x00, 0x07, 0x76, 0x2C}; /* 7*16384 + 118*128 + 44 */
    CHECK_EQ_INT((long long)mp3_id3v2_size(t, 10), 7 * 16384 + 118 * 128 + 44 + 10);
    t[5] = 0x10; /* footer */
    CHECK_EQ_INT((long long)mp3_id3v2_size(t, 10), 7 * 16384 + 118 * 128 + 44 + 20);
    t[6] = 0x80; /* not syncsafe */
    CHECK_EQ_INT((long long)mp3_id3v2_size(t, 10), 0);
    uint8_t n[10] = {0xFF, 0xFB, 0x90, 0, 0, 0, 0, 0, 0, 0};
    CHECK_EQ_INT((long long)mp3_id3v2_size(n, 10), 0);
}

static uint8_t *cbr_stream(int frames, size_t *len) {
    *len = (size_t)frames * 417;
    uint8_t *b = calloc(1, *len);
    for (int i = 0; i < frames; i++) put_frame(b + (size_t)i * 417, 0x90, 0x00);
    return b;
}

TEST(mp3_xing_vbri_cbr) {
    size_t len;
    uint8_t *b = cbr_stream(10, &len);
    Mp3Info mi;
    /* CBR: no tag -> bytes * 8 / bitrate, as FFmpeg's estimate */
    uint64_t file_size = 1000000;
    CHECK_EQ_INT(mp3_info_from_buffer(b, len, 0, file_size, &mi), 0);
    CHECK_EQ_INT(mi.exact, 0);
    CHECK_NEAR(mi.duration, 1000000.0 * 8 / 128000, 1e-6);
    /* garbage before the first frame (tag padding): found and offset counted */
    uint8_t *g = calloc(1, len + 100);
    memcpy(g + 100, b, len);
    g[10] = 0xFF; g[11] = 0xFB; g[12] = 0x90; /* a stray sync that is not followed by a frame */
    CHECK_EQ_INT(mp3_info_from_buffer(g, len + 100, 5000, file_size, &mi), 0);
    CHECK_EQ_INT((long long)mi.audio_offset, 5100);
    CHECK_NEAR(mi.duration, (1000000.0 - 5100) * 8 / 128000, 1e-6);
    free(g);
    /* Xing: stereo MPEG-1 side info 32 bytes, flags=1, frames=1000 */
    uint8_t *x = b;
    memcpy(x + 4 + 32, "Xing", 4);
    x[4 + 32 + 7] = 1;
    x[4 + 32 + 8] = 0; x[4 + 32 + 9] = 0; x[4 + 32 + 10] = 0x03; x[4 + 32 + 11] = 0xE8;
    CHECK_EQ_INT(mp3_info_from_buffer(x, len, 0, file_size, &mi), 0);
    CHECK_EQ_INT(mi.exact, 1);
    CHECK_NEAR(mi.duration, 1000.0 * 1152 / 44100, 1e-6);
    /* Info (LAME CBR) the same way */
    memcpy(x + 4 + 32, "Info", 4);
    CHECK_EQ_INT(mp3_info_from_buffer(x, len, 0, file_size, &mi), 0);
    CHECK_EQ_INT(mi.exact, 1);
    /* VBRI at 32 bytes after the header, frame count at +14 */
    memset(x + 4, 0, 60);
    memcpy(x + 4 + 32, "VBRI", 4);
    x[4 + 32 + 14] = 0; x[4 + 32 + 15] = 0; x[4 + 32 + 16] = 0x07; x[4 + 32 + 17] = 0xD0; /* 2000 */
    CHECK_EQ_INT(mp3_info_from_buffer(x, len, 0, file_size, &mi), 0);
    CHECK_EQ_INT(mi.exact, 1);
    CHECK_NEAR(mi.duration, 2000.0 * 1152 / 44100, 1e-6);
    free(b);
    /* nothing that looks like audio */
    uint8_t junk[2048];
    memset(junk, 0x41, sizeof junk);
    CHECK(mp3_info_from_buffer(junk, sizeof junk, 0, 1 << 20, &mi) != 0);
}

TEST(mp3_fast_info_file_with_big_tag) {
    mkdir("build-host", 0777);
    const char *p = "build-host/tmp-mp3info.mp3";
    FILE *f = fopen(p, "wb");
    CHECK(f != NULL);
    if (!f) return;
    /* ID3v2 with a 150 KB body (like embedded cover art), then 200 CBR frames */
    uint32_t body = 150000;
    uint8_t t[10] = {'I', 'D', '3', 3, 0, 0, (uint8_t)((body >> 21) & 0x7F), (uint8_t)((body >> 14) & 0x7F),
                     (uint8_t)((body >> 7) & 0x7F), (uint8_t)(body & 0x7F)};
    fwrite(t, 1, 10, f);
    uint8_t *pad = calloc(1, body);
    pad[1000] = 0xFF; pad[1001] = 0xFB; pad[1002] = 0x90; /* fake sync inside the art */
    fwrite(pad, 1, body, f);
    free(pad);
    size_t len;
    uint8_t *b = cbr_stream(200, &len);
    fwrite(b, 1, len, f);
    free(b);
    fclose(f);
    Mp3Info mi;
    CHECK_EQ_INT(mp3_fast_info(p, &mi), 0);
    CHECK_EQ_INT((long long)mi.audio_offset, 150010);
    CHECK_NEAR(mi.duration, 200.0 * 417 * 8 / 128000, 1e-6);
    CHECK_EQ_INT(mp3_fast_info("build-host/no-such-file.mp3", &mi), -1);
    remove(p);
}

TEST(probecache_duration_only_and_partial_catalog) {
    mkdir("build-host/tmp-mp3info-cache", 0777);
    probecache_init("build-host/tmp-mp3info-cache");
    ProbeCacheEnt e;
    memset(&e, 0, sizeof e);
    snprintf(e.path, sizeof e.path, "ux0:audiobooks/Big/0102.mp3");
    e.size = 12345;
    e.mtime_ms = 678000;
    e.duration = 161.5;
    e.ver = PROBE_CACHE_VER;
    e.dur_only = 1;
    CHECK_EQ_INT(probecache_put(&e), 0);
    ProbeCacheEnt g;
    CHECK_EQ_INT(probecache_get(e.path, 12345, 678000, &g), 0);
    CHECK_EQ_INT(g.dur_only, 1);
    CHECK_NEAR(g.duration, 161.5, 1e-9);
    probecache_ent_free(&g);
    e.dur_only = 0;
    CHECK_EQ_INT(probecache_put(&e), 0); /* a full probe replaces it */
    CHECK_EQ_INT(probecache_get(e.path, 12345, 678000, &g), 0);
    CHECK_EQ_INT(g.dur_only, 0);
    probecache_ent_free(&g);

    LibraryCache c;
    memset(&c, 0, sizeof c);
    c.version = LIBCACHE_VERSION;
    snprintf(c.root, sizeof c.root, "ux0:audiobooks");
    c.partial = 1;
    c.n = 1;
    c.books = calloc(1, sizeof(CachedBook));
    snprintf(c.books[0].sum.id, sizeof c.books[0].sum.id, "abc");
    c.books[0].sum.duration = 10;
    snprintf(c.books[0].sum.path, sizeof c.books[0].sum.path, "ux0:audiobooks/Big");
    const char *cp = "build-host/tmp-mp3info-cache/lib.json";
    CHECK_EQ_INT(libcache_save(cp, &c), 0);
    LibraryCache l;
    CHECK_EQ_INT(libcache_load(cp, &l), 0);
    CHECK_EQ_INT(l.partial, 1);
    CHECK_EQ_INT(l.n, 1);
    LibraryCache k;
    CHECK_EQ_INT(libcache_clone(&l, &k), 0);
    CHECK_EQ_INT(k.partial, 1);
    libcache_free(&k);
    libcache_free(&l);
    c.partial = 0; /* the finished catalog: no flag, loads as complete (also older files) */
    CHECK_EQ_INT(libcache_save(cp, &c), 0);
    CHECK_EQ_INT(libcache_load(cp, &l), 0);
    CHECK_EQ_INT(l.partial, 0);
    libcache_free(&l);
    libcache_free(&c);
}

/* ---- 00.37: the checked read the background scan uses ---- */

/* frames: MPEG-1 L3 44.1 kHz stereo; b2 = 0x90 (128k, 417 B), 0xA0 (160k, 522 B), 0x50 (64k, 208 B) */
static int frame_len_for(uint8_t b2) { Mp3Frame f; uint8_t h[4] = {0xFF, 0xFB, b2, 0}; mp3_parse_frame(h, &f); return f.frame_len; }

typedef struct { uint8_t *b; size_t n, cap; } Buf;
static void buf_put(Buf *x, const void *p, size_t n) {
    if (x->n + n > x->cap) { x->cap = (x->n + n) * 2; x->b = realloc(x->b, x->cap); }
    memcpy(x->b + x->n, p, n);
    x->n += n;
}
static void buf_frames(Buf *x, uint8_t b2, int count) {
    int len = frame_len_for(b2);
    uint8_t *f = calloc(1, (size_t)len);
    put_frame(f, b2, 0x00);
    for (int i = 0; i < count; i++) buf_put(x, f, (size_t)len);
    free(f);
}
static int buf_write(Buf *x, const char *p) {
    FILE *f = fopen(p, "wb");
    if (!f) return -1;
    fwrite(x->b, 1, x->n, f);
    fclose(f);
    free(x->b);
    memset(x, 0, sizeof *x);
    return 0;
}

TEST(mp3_checked_synthetic) {
    mkdir("build-host", 0777);
    const char *p = "build-host/tmp-mp3chk.mp3";
    Mp3Info mi;
    int why = 0;
    Buf x = {0};

    /* CBR, 1000 frames: trusted, the CBR estimate */
    buf_frames(&x, 0x90, 1000);
    buf_write(&x, p);
    CHECK_EQ_INT(mp3_fast_info_checked(p, &mi, &why), 0);
    CHECK_EQ_INT(why, MP3_OK);
    CHECK_EQ_INT(mi.exact, 0);
    CHECK_NEAR(mi.duration, 1000.0 * 417 * 8 / 128000, 1e-6);

    /* VBR without a header, bitrate changes at the start: fallback */
    for (int i = 0; i < 300; i++) { buf_frames(&x, 0x90, 1); buf_frames(&x, 0xA0, 1); }
    buf_write(&x, p);
    CHECK(mp3_fast_info_checked(p, &mi, &why) != 0);
    CHECK_EQ_INT(why, MP3_WHY_VBR_NO_TOC);
    /* the unchecked 00.36 read would have taken the CBR estimate */
    CHECK_EQ_INT(mp3_fast_info(p, &mi), 0);

    /* VBR without a header, same bitrate for a while, then lower: the 1/3-2/3 samples catch it */
    buf_frames(&x, 0x90, 400);
    buf_frames(&x, 0x50, 1200);
    buf_write(&x, p);
    CHECK(mp3_fast_info_checked(p, &mi, &why) != 0);
    CHECK_EQ_INT(why, MP3_WHY_VBR_NO_TOC);

    /* damaged middle (zeros from 25% to 80%): no frame there -> fallback */
    buf_frames(&x, 0x90, 1000);
    memset(x.b + x.n / 4, 0, x.n * 55 / 100);
    buf_write(&x, p);
    CHECK(mp3_fast_info_checked(p, &mi, &why) != 0);
    CHECK_EQ_INT(why, MP3_WHY_SAMPLE);

    /* corrupt start: 64 KB of junk before the audio */
    uint8_t junk[65536];
    memset(junk, 0x41, sizeof junk);
    buf_put(&x, junk, sizeof junk);
    buf_frames(&x, 0x90, 200);
    buf_write(&x, p);
    CHECK(mp3_fast_info_checked(p, &mi, &why) != 0);
    CHECK_EQ_INT(why, MP3_WHY_NOSYNC);

    /* two good frames, then garbage: the frames do not chain */
    buf_frames(&x, 0x90, 2);
    buf_put(&x, junk, 8000);
    buf_write(&x, p);
    CHECK(mp3_fast_info_checked(p, &mi, &why) != 0);
    CHECK_EQ_INT(why, MP3_WHY_FRAMES);

    /* Xing with a plausible count: exact */
    buf_frames(&x, 0x90, 1000);
    memcpy(x.b + 4 + 32, "Xing", 4);
    x.b[4 + 32 + 7] = 1;
    x.b[4 + 32 + 10] = 0x03; x.b[4 + 32 + 11] = 0xE7; /* 999 */
    buf_write(&x, p);
    CHECK_EQ_INT(mp3_fast_info_checked(p, &mi, &why), 0);
    CHECK_EQ_INT(mi.exact, 1);
    CHECK_NEAR(mi.duration, 999.0 * 1152 / 44100, 1e-6);

    /* Xing count damaged: 3 frames for 417 KB (implausible bitrate) / 2^31 frames (days) */
    buf_frames(&x, 0x90, 1000);
    memcpy(x.b + 4 + 32, "Xing", 4);
    x.b[4 + 32 + 7] = 1;
    x.b[4 + 32 + 11] = 3;
    buf_write(&x, p);
    CHECK(mp3_fast_info_checked(p, &mi, &why) != 0);
    CHECK_EQ_INT(why, MP3_WHY_DURATION);
    buf_frames(&x, 0x90, 1000);
    memcpy(x.b + 4 + 32, "Xing", 4);
    x.b[4 + 32 + 7] = 1;
    x.b[4 + 32 + 8] = 0x7F; x.b[4 + 32 + 9] = 0xFF; x.b[4 + 32 + 10] = 0xFF; x.b[4 + 32 + 11] = 0xFF;
    buf_write(&x, p);
    CHECK(mp3_fast_info_checked(p, &mi, &why) != 0);
    CHECK_EQ_INT(why, MP3_WHY_DURATION);

    /* VBRI: exact */
    buf_frames(&x, 0x90, 1000);
    memcpy(x.b + 4 + 32, "VBRI", 4);
    x.b[4 + 32 + 5] = 1;
    x.b[4 + 32 + 16] = 0x03; x.b[4 + 32 + 17] = 0xE7; /* 999 */
    buf_write(&x, p);
    CHECK_EQ_INT(mp3_fast_info_checked(p, &mi, &why), 0);
    CHECK_EQ_INT(mi.exact, 1);
    CHECK_NEAR(mi.duration, 999.0 * 1152 / 44100, 1e-6);

    /* ID3v2 with 700 KB of art before the first frame, then VBR with Xing */
    uint32_t body = 700000;
    uint8_t t[10] = {'I', 'D', '3', 3, 0, 0, (uint8_t)((body >> 21) & 0x7F), (uint8_t)((body >> 14) & 0x7F),
                     (uint8_t)((body >> 7) & 0x7F), (uint8_t)(body & 0x7F)};
    buf_put(&x, t, 10);
    uint8_t *art = calloc(1, body);
    for (uint32_t i = 0; i < body; i += 997) { art[i] = 0xFF; if (i + 1 < body) art[i + 1] = 0xFB; }
    buf_put(&x, art, body);
    free(art);
    size_t xo = x.n;
    for (int i = 0; i < 500; i++) { buf_frames(&x, 0x90, 1); buf_frames(&x, 0xA0, 1); }
    memcpy(x.b + xo + 4 + 32, "Xing", 4);
    x.b[xo + 4 + 32 + 7] = 1;
    x.b[xo + 4 + 32 + 10] = 0x03; x.b[xo + 4 + 32 + 11] = 0xE7;
    buf_write(&x, p);
    CHECK_EQ_INT(mp3_fast_info_checked(p, &mi, &why), 0);
    CHECK_EQ_INT((long long)mi.audio_offset, 700010);
    CHECK_EQ_INT(mi.exact, 1);

    /* short file (a few frames, all inside the first read) */
    buf_frames(&x, 0x90, 2);
    buf_write(&x, p);
    CHECK_EQ_INT(mp3_fast_info_checked(p, &mi, &why), 0);
    CHECK(mp3_fast_info_checked("build-host/no-such.mp3", &mi, &why) != 0);
    CHECK_EQ_INT(why, MP3_WHY_IO);
    CHECK_STR(mp3_why_str(MP3_WHY_VBR_NO_TOC), "variable bitrate without a Xing/VBRI header");
    remove(p);
}

TEST(probecache_header_kind_roundtrip) {
    mkdir("build-host/tmp-mp3info-cache", 0777);
    probecache_init("build-host/tmp-mp3info-cache");
    ProbeCacheEnt e;
    memset(&e, 0, sizeof e);
    snprintf(e.path, sizeof e.path, "ux0:audiobooks/Big/0103.mp3");
    e.size = 1;
    e.mtime_ms = 2;
    e.duration = 3.5;
    e.ver = PROBE_CACHE_VER;
    e.dur_only = 1;
    e.hdr = PROBE_HDR_EXACT;
    CHECK_EQ_INT(probecache_put(&e), 0);
    ProbeCacheEnt g;
    CHECK_EQ_INT(probecache_get(e.path, 1, 2, &g), 0);
    CHECK_EQ_INT(g.hdr, PROBE_HDR_EXACT);
    probecache_ent_free(&g);
    e.hdr = PROBE_HDR_CBR;
    CHECK_EQ_INT(probecache_put(&e), 0);
    CHECK_EQ_INT(probecache_get(e.path, 1, 2, &g), 0);
    CHECK_EQ_INT(g.hdr, PROBE_HDR_CBR);
    probecache_ent_free(&g);
    e.hdr = PROBE_HDR_NONE; /* a 00.36 record */
    CHECK_EQ_INT(probecache_put(&e), 0);
    CHECK_EQ_INT(probecache_get(e.path, 1, 2, &g), 0);
    CHECK_EQ_INT(g.hdr, PROBE_HDR_NONE);
    probecache_ent_free(&g);
}

void suite_mp3info(void) {
    RUN(mp3_checked_synthetic);
    RUN(probecache_header_kind_roundtrip);
    RUN(mp3_frame_header);
    RUN(mp3_id3_size);
    RUN(mp3_xing_vbri_cbr);
    RUN(mp3_fast_info_file_with_big_tag);
    RUN(probecache_duration_only_and_partial_catalog);
}
