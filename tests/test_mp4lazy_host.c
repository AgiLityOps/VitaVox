/* Lazy MP4/M4B demuxer vs FFmpeg's mov path: same duration, tags, chapters,
 * cover, decoded PCM (bit-exact) and positions after seeks. Host FFmpeg only. */
#include "test.h"
#include "stream.h"
#include "osal.h"
#include "dbg.h"

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int sh(const char *cmd) {
    int rc = system(cmd);
    if (rc != 0) fprintf(stderr, "command failed (%d): %s\n", rc, cmd);
    return rc;
}

static int exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && st.st_size > 100;
}

typedef struct { uint64_t hash; int64_t frames; int rate; int err; } Pcm;

static void pcm_feed(Pcm *p, const int16_t *b, int n) {
    const uint8_t *x = (const uint8_t *)b;
    for (int i = 0; i < n * 4; i++) { p->hash ^= x[i]; p->hash *= 1099511628211ULL; }
    p->frames += n;
}

/* Decode up to max_frames (all if <0) from the current position. */
static void decode(Stream *s, int64_t max_frames, Pcm *p) {
    int16_t buf[4096 * 2];
    while (max_frames < 0 || p->frames < max_frames) {
        int ch = 0, rate = 0;
        int want = 4096;
        if (max_frames >= 0 && max_frames - p->frames < want) want = (int)(max_frames - p->frames);
        int n = stream_read(s, buf, want, &ch, &rate);
        if (n == STREAM_EOF) break;
        if (n < 0) { p->err = n; break; }
        if (rate) p->rate = rate;
        pcm_feed(p, buf, n);
    }
}

static Stream *open_mode(const char *path, int lazy, uint64_t *ms) {
    TrackSource src;
    memset(&src, 0, sizeof src);
    snprintf(src.path, sizeof src.path, "%s", path);
    const char *dot = strrchr(path, '.');
    snprintf(src.ext, sizeof src.ext, "%s", dot ? dot : "");
    stream_set_mp4_lazy(lazy);
    uint64_t t0 = now_ms();
    int err = 0;
    Stream *s = stream_open(&src, &err);
    if (ms) *ms = now_ms() - t0;
    stream_set_mp4_lazy(1);
    return s;
}

static void compare_probe(const char *path) {
    MediaInfo a, b;
    stream_set_mp4_lazy(1);
    int ra = stream_probe_ex(path, &a, 0, 15000);
    stream_set_mp4_lazy(0);
    int rb = stream_probe_ex(path, &b, 0, 15000);
    stream_set_mp4_lazy(1);
    CHECK_EQ_INT(ra, 0);
    CHECK_EQ_INT(rb, 0);
    CHECK_NEAR(a.duration, b.duration, 0.05);
    CHECK_STR(a.title, b.title);
    CHECK_STR(a.artist, b.artist);
    CHECK_STR(a.album, b.album);
    CHECK_EQ_INT(a.has_cover, b.has_cover);
    CHECK_EQ_INT(a.cover_len, b.cover_len);
    if (a.cover && b.cover && a.cover_len == b.cover_len) CHECK(memcmp(a.cover, b.cover, a.cover_len) == 0);
    CHECK_EQ_INT(a.nchapters, b.nchapters);
    for (int i = 0; i < a.nchapters && i < b.nchapters; i++) {
        CHECK_NEAR(a.chapters[i].start, b.chapters[i].start, 0.001);
        CHECK_STR(a.chapters[i].title, b.chapters[i].title);
        /* FFmpeg leaves chpl-only ends unset (garbage); compare only sane ones. */
        if (b.chapters[i].end >= b.chapters[i].start && b.chapters[i].end < 1e7)
            CHECK_NEAR(a.chapters[i].end, b.chapters[i].end, 0.001);
        CHECK(a.chapters[i].end >= a.chapters[i].start);
    }
    printf("  probe %-48.48s dur %.2f/%.2f ch %d/%d cover %u\n", strrchr(path, '/') + 1,
           a.duration, b.duration, a.nchapters, b.nchapters, (unsigned)a.cover_len);
    media_info_free(&a);
    media_info_free(&b);
}

static void compare_decode(const char *path, int full) {
    uint64_t ms_l = 0, ms_f = 0;
    Stream *l = open_mode(path, 1, &ms_l);
    Stream *f = open_mode(path, 0, &ms_f);
    if (!l && !f) { printf("  (neither path can decode %s on this host)\n", path); return; }
    CHECK(l != NULL);
    CHECK(f != NULL);
    if (l) CHECK(stream_uses_mp4lazy(l));
    if (f) CHECK(!stream_uses_mp4lazy(f));
    if (!l || !f) { stream_close(l); stream_close(f); return; }
    Pcm pl = {1469598103934665603ULL, 0, 0, 0}, pf = pl;
    int64_t lim = full ? -1 : 44100 * 20;
    decode(l, lim, &pl);
    decode(f, lim, &pf);
    CHECK_EQ_INT(pl.err, 0);
    CHECK_EQ_INT(pf.err, 0);
    CHECK_EQ_INT(pl.frames, pf.frames);
    CHECK(pl.hash == pf.hash);
    CHECK_EQ_INT(pl.rate, pf.rate);
    printf("  decode %-47.47s %lld frames %s, open lazy %llu ms / mov %llu ms\n", strrchr(path, '/') + 1,
           (long long)pl.frames, pl.hash == pf.hash ? "identical" : "DIFFERENT",
           (unsigned long long)ms_l, (unsigned long long)ms_f);
    MediaInfo mi;
    double dur = stream_probe_ex(path, &mi, 0, 15000) == 0 ? mi.duration : 30;
    media_info_free(&mi);
    double pos[] = { 0.0, 0.5, 7.3, 33.25, dur * 0.61, dur - 2.0, dur + 5.0 };
    for (unsigned i = 0; i < sizeof pos / sizeof pos[0]; i++) {
        if (pos[i] < 0) continue;
        double al = -1, af = -1;
        int sl = stream_seek(l, pos[i], &al), sf = stream_seek(f, pos[i], &af);
        CHECK_EQ_INT(sl, sf);
        Pcm ql = {1469598103934665603ULL, 0, 0, 0}, qf = ql;
        decode(l, 4096 * 3, &ql);
        decode(f, 4096 * 3, &qf);
        CHECK_EQ_INT(ql.frames, qf.frames);
        CHECK(ql.hash == qf.hash);
        if (ql.hash != qf.hash) printf("    seek %.2f differs (%lld / %lld frames)\n", pos[i], (long long)ql.frames, (long long)qf.frames);
    }
    stream_close(l);
    stream_close(f);
}

#define FX "build-host/fixtures/mp4lazy"

static void gen(void) {
    mkdir("build-host/fixtures", 0777);
    mkdir(FX, 0777);
    char cmd[2048];
    if (!exists(FX "/cover.jpg"))
        sh("ffmpeg -hide_banner -loglevel error -y -f lavfi -i testsrc=s=600x600:d=1 -frames:v 1 -q:v 3 " FX "/cover.jpg");
    if (!exists(FX "/ch.ini")) {
        FILE *f = fopen(FX "/ch.ini", "w");
        if (f) {
            fputs(";FFMETADATA1\ntitle=Lazy Book\nartist=Some Author\nalbum_artist=Album Person\nalbum=The Album\n", f);
            const char *names[] = { "Opening Credits", "Chapter One: Ünïcödé", "Chapter Two", "", "The End" };
            int t[] = { 0, 4000, 31000, 52000, 77000, 90000 };
            for (int i = 0; i < 5; i++)
                fprintf(f, "[CHAPTER]\nTIMEBASE=1/1000\nSTART=%d\nEND=%d\ntitle=%s\n", t[i], t[i + 1], names[i]);
            fclose(f);
        }
    }
    struct { const char *name, *args; int secs; } v[] = {
        /* chapters (chpl + QT text track), cover, faststart, 44.1 k stereo LC */
        { "aac_chapters.m4b", "-c:a aac -b:a 96k -ar 44100 -ac 2 -movflags +faststart", 90 },
        /* QT chapter track only (no chpl), moov at the end */
        { "aac_qtchap_moovend.m4b", "-c:a aac -b:a 48k -ar 22050 -ac 1 -movflags +disable_chpl", 90 },
        /* Apple Lossless */
        { "alac.m4a", "-c:a alac -ar 44100 -ac 2 -sample_fmt s16p", 30 },
        /* MP3 in MP4 */
        { "mp3.mp4", "-c:a libmp3lame -b:a 64k -ar 44100 -ac 2", 30 },
    };
    for (unsigned i = 0; i < sizeof v / sizeof v[0]; i++) {
        char out[256];
        snprintf(out, sizeof out, FX "/%s", v[i].name);
        if (exists(out)) continue;
        int pic = strstr(v[i].name, "mp3") == NULL;
        snprintf(cmd, sizeof cmd,
                 "ffmpeg -hide_banner -loglevel error -y -f lavfi -i \"sine=frequency=330:sample_rate=44100:duration=%d\" "
                 "-f lavfi -i \"anoisesrc=d=%d:c=pink:r=44100:a=0.08\" %s -i " FX "/ch.ini "
                 "-filter_complex \"[0][1]amix=inputs=2:duration=first[a]\" -map \"[a]\" %s -map_metadata %d -map_chapters %d "
                 "%s %s \"%s\"",
                 v[i].secs, v[i].secs, pic ? "-i " FX "/cover.jpg" : "", pic ? "-map 2:v -c:v copy -disposition:v attached_pic" : "",
                 pic ? 3 : 2, pic ? 3 : 2, v[i].args, strstr(v[i].name, ".mp4") ? "-f mp4" : "-f ipod", out);
        sh(cmd);
    }
    /* iTunes-style priming (elst media time 2112 instead of FFmpeg's 1024). */
    if (!exists(FX "/aac_prime2112.m4b") && exists(FX "/aac_chapters.m4b")) {
        FILE *in = fopen(FX "/aac_chapters.m4b", "rb");
        if (in) {
            fseek(in, 0, SEEK_END);
            long n = ftell(in);
            fseek(in, 0, SEEK_SET);
            uint8_t *d = malloc((size_t)n);
            if (d && fread(d, 1, (size_t)n, in) == (size_t)n) {
                for (long i = 0; i + 20 < n; i++) {
                    if (memcmp(d + i, "elst", 4) == 0) {
                        uint8_t *mt = d + i + 4 + 4 + 4 + 4; /* ver/flags, count, seg dur */
                        mt[0] = 0; mt[1] = 0; mt[2] = 2112 >> 8; mt[3] = 2112 & 255;
                        break;
                    }
                }
                FILE *o = fopen(FX "/aac_prime2112.m4b", "wb");
                if (o) { fwrite(d, 1, (size_t)n, o); fclose(o); }
            }
            free(d);
            fclose(in);
        }
    }
    /* Long book: > 16384 packets (crosses stsz/stco windows), many chunks. */
    if (!exists(FX "/long_32k_mono.m4b"))
        sh("ffmpeg -hide_banner -loglevel error -y -f lavfi -i \"sine=frequency=250:sample_rate=32000:duration=1500\" "
           "-c:a aac -b:a 32k -ar 32000 -ac 1 -f ipod " FX "/long_32k_mono.m4b");
}

TEST(lazy_matches_mov_path) {
    gen();
    const char *files[] = { FX "/aac_chapters.m4b", FX "/aac_qtchap_moovend.m4b", FX "/alac.m4a",
                            FX "/mp3.mp4", FX "/long_32k_mono.m4b", FX "/aac_prime2112.m4b",
                            "build-host/fixtures/lc.m4b", "build-host/fixtures/he.m4b",
                            "build-host/fixtures/formats/m4a_lc/track.m4a" };
    for (unsigned i = 0; i < sizeof files / sizeof files[0]; i++) {
        if (!exists(files[i])) { printf("  (missing %s)\n", files[i]); continue; }
        t_current = files[i];
        compare_probe(files[i]);
        compare_decode(files[i], 1);
    }
    t_current = "lazy_matches_mov_path";
    /* Lazy really ran (and found the chapters / tags). */
    MediaInfo mi;
    CHECK_EQ_INT(stream_probe_ex(FX "/aac_chapters.m4b", &mi, 0, 15000), 0);
    CHECK_EQ_INT(mi.nchapters, 5);
    CHECK_STR(mi.title, "Lazy Book");
    CHECK_STR(mi.artist, "Some Author");
    CHECK(mi.has_cover && mi.cover_len > 1000);
    if (mi.nchapters == 5) {
        CHECK_STR(mi.chapters[1].title, "Chapter One: Ünïcödé");
        CHECK_NEAR(mi.chapters[2].start, 31.0, 0.01);
        CHECK_NEAR(mi.chapters[4].end, 90.0, 0.1);
    }
    media_info_free(&mi);
}

/* Files the lazy reader declines (fragmented MP4) or cannot parse still play via FFmpeg. */
TEST(lazy_falls_back_to_mov) {
    gen();
    const char *frag = FX "/fragmented.m4a";
    if (!exists(frag))
        sh("ffmpeg -hide_banner -loglevel error -y -f lavfi -i \"sine=frequency=440:sample_rate=44100:duration=8\" "
           "-c:a aac -b:a 64k -movflags frag_keyframe+empty_moov -f mp4 " FX "/fragmented.m4a");
    uint64_t ms = 0;
    Stream *s = open_mode(frag, 1, &ms);
    CHECK(s != NULL);
    if (s) {
        CHECK(!stream_uses_mp4lazy(s));
        Pcm p = {1469598103934665603ULL, 0, 0, 0};
        decode(s, -1, &p);
        CHECK(p.frames > 44100 * 7);
        stream_close(s);
    }
    /* Garbage with an .m4b name: both readers refuse, no crash/hang. */
    FILE *g = fopen(FX "/garbage.m4b", "wb");
    if (g) {
        uint8_t junk[4096];
        for (int i = 0; i < (int)sizeof junk; i++) junk[i] = (uint8_t)(i * 37 + 11);
        memcpy(junk, "\0\0\0\x20ftypM4B ", 12);
        fwrite(junk, 1, sizeof junk, g);
        fclose(g);
    }
    s = open_mode(FX "/garbage.m4b", 1, &ms);
    CHECK(s == NULL);
    stream_close(s);
    MediaInfo mi;
    CHECK(stream_probe_ex(FX "/garbage.m4b", &mi, 0, 5000) != 0);
    media_info_free(&mi);
}

/* Every m4b in the generated copy of the user's library. */
static void walk_m4b(const char *dir, int depth) {
    DIR *d = opendir(dir);
    if (!d || depth > 4) { if (d) closedir(d); return; }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[1024];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(p, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) walk_m4b(p, depth + 1);
        else {
            size_t n = strlen(p);
            if (n > 4 && !strcmp(p + n - 4, ".m4b")) {
                t_current = p;
                compare_probe(p);
                compare_decode(p, 1);
            }
        }
    }
    closedir(d);
}

TEST(lazy_matches_on_sample_library_copy) {
    walk_m4b("build-host/fixtures/sample_library", 0);
    t_current = "lazy_matches_on_sample_library_copy";
}

/* Optional (MP4LAZY_BIG=1): a ~13 h book with a large moov, to compare open cost. */
TEST(lazy_big_moov_open) {
    if (!getenv("MP4LAZY_BIG")) return;
    gen();
    const char *big = FX "/big_13h.m4b";
    if (!exists(big))
        sh("ffmpeg -hide_banner -loglevel error -y -stream_loop 31 -i " FX "/long_32k_mono.m4b -c copy -f ipod " FX "/big_13h.m4b");
    uint64_t ml = 0, mf = 0;
    Stream *l = open_mode(big, 1, &ml);
    Stream *f = open_mode(big, 0, &mf);
    CHECK(l && f);
    printf("  big moov open: lazy %llu ms, mov %llu ms\n", (unsigned long long)ml, (unsigned long long)mf);
    if (l && f) {
        double pos[] = { 3600.0, 40000.0, 100.0, 47000.0 };
        for (int i = 0; i < 4; i++) {
            double a, b;
            CHECK_EQ_INT(stream_seek(l, pos[i], &a), stream_seek(f, pos[i], &b));
            Pcm ql = {1469598103934665603ULL, 0, 0, 0}, qf = ql;
            decode(l, 8192, &ql);
            decode(f, 8192, &qf);
            CHECK(ql.hash == qf.hash);
        }
    }
    stream_close(l);
    stream_close(f);
    compare_probe(big);
}

void suite_mp4lazy_host(void) {
    RUN(lazy_big_moov_open);
    RUN(lazy_matches_mov_path);
    RUN(lazy_matches_on_sample_library_copy);
    RUN(lazy_falls_back_to_mov);
}
