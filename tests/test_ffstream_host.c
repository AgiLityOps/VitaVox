/* Host decode/seek harness for ffstream + engine, linked against a host FFmpeg.
 *   make -f Makefile.host test-decode
 * Not part of the default (no-libav) test binary. */
#include "test.h"
#include "engine.h"
#include "stream.h"
#include "osal.h"
#include "dbg.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int t_failures, t_checks;
const char *t_current;

void suite_library_host(void);
void suite_mp4lazy_host(void);
void suite_open_priority_host(void);
void suite_mp3scan_host(void);
void suite_desc_host(void);

static int run_cmd(const char *cmd) {
    dbg("gen: %s", cmd);
    int rc = system(cmd);
    if (rc != 0) fprintf(stderr, "command failed (%d): %s\n", rc, cmd);
    return rc;
}

static int file_ok(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0 && st.st_size > 100;
}

/* Tiny 32x32 solid-red JPEG via ffmpeg lavfi, plus ffmetadata chapters. */
static int make_assets(const char *dir) {
    char cmd[1024];
    mkdir(dir, 0777);
    snprintf(cmd, sizeof cmd,
             "ffmpeg -hide_banner -loglevel error -y -f lavfi -i color=c=red:s=32x32:d=1 -frames:v 1 -q:v 5 %s/cover.jpg",
             dir);
    if (run_cmd(cmd) != 0) return -1;
    char meta[320];
    snprintf(meta, sizeof meta, "%s/chapters.ini", dir);
    FILE *f = fopen(meta, "w");
    if (!f) return -1;
    fputs(";FFMETADATA1\n"
          "title=Host Test Book\n"
          "artist=Tester\n"
          "[CHAPTER]\nTIMEBASE=1/1000\nSTART=0\nEND=5000\ntitle=Intro\n"
          "[CHAPTER]\nTIMEBASE=1/1000\nSTART=5000\nEND=20000\ntitle=Chapter 2\n", f);
    fclose(f);
    return 0;
}

static int make_m4b(const char *dir, const char *name, const char *aac_extra, int duration_s) {
    char out[320], cmd[1600];
    snprintf(out, sizeof out, "%s/%s", dir, name);
    /* ipod muxer writes .m4a/.m4b-style MP4. Cover as attached_pic. */
    snprintf(cmd, sizeof cmd,
             "ffmpeg -hide_banner -loglevel error -y "
             "-f lavfi -i sine=frequency=440:sample_rate=32000:duration=%d "
             "-i %s/cover.jpg -i %s/chapters.ini "
             "-map 0:a -map 1:v -map_metadata 2 "
             "-c:a aac %s -ar 32000 -ac 1 -b:a 64k "
             "-c:v mjpeg -disposition:v attached_pic "
             "-f mp4 -movflags +faststart %s",
             duration_s, dir, dir, aac_extra, out);
    if (run_cmd(cmd) != 0) return -1;
    return file_ok(out) ? 0 : -1;
}

static int decode_all(Stream *s, int *out_rate, int64_t *frames, int *packets_via_stats) {
    int16_t buf[4096 * 2];
    *frames = 0;
    *out_rate = 0;
    int safety = 0;
    for (;;) {
        int ch = 0, rate = 0;
        int n = stream_read(s, buf, 4096, &ch, &rate);
        if (n == STREAM_EOF) break;
        if (n < 0) {
            fprintf(stderr, "stream_read failed %d after %lld frames\n", n, (long long)*frames);
            return n;
        }
        if (rate) *out_rate = rate;
        *frames += n;
        if (++safety > 2000000) {
            fprintf(stderr, "decode loop ran away\n");
            return -1;
        }
    }
    int req = 0;
    uint64_t bytes = 0;
    stream_table_stats(s, &req, &bytes);
    *packets_via_stats = req;
    (void)bytes;
    return 0;
}

static int energy(const int16_t *pcm, int frames) {
    long acc = 0;
    int n = frames * 2;
    if (n > 4000) n = 4000;
    for (int i = 0; i < n; i++) acc += pcm[i] < 0 ? -pcm[i] : pcm[i];
    return (int)(acc / (n ? n : 1));
}

TEST(stream_decode_and_seek) {
    const char *dir = "build-host/fixtures";
    CHECK_EQ_INT(make_assets(dir), 0);
    CHECK_EQ_INT(make_m4b(dir, "lc.m4b", "-profile:a aac_low", 20), 0);

    const char *he_name = "he.m4b";
    int have_he = make_m4b(dir, he_name, "-profile:a aac_he", 12) == 0;

    const char *files[2] = { "lc.m4b", he_name };
    int nfiles = have_he ? 2 : 1;
    if (!have_he) fprintf(stderr, "note: HE-AAC fixture failed to encode; LC only\n");

    for (int i = 0; i < nfiles; i++) {
        char path[320];
        snprintf(path, sizeof path, "%s/%s", dir, files[i]);
        t_current = files[i];

        TrackSource src;
        memset(&src, 0, sizeof src);
        snprintf(src.path, sizeof src.path, "%s", path);
        src.ext[0] = '.'; src.ext[1] = 'm'; src.ext[2] = '4'; src.ext[3] = 'b'; src.ext[4] = 0;

        int err = 0;
        Stream *s = stream_open(&src, &err);
        if (!s) {
            /* Slim FFmpeg 9 builds may reject some HE-AAC object types; LC is required. */
            fprintf(stderr, "skip %s: stream_open failed (%d)\n", files[i], err);
            if (i == 0) { CHECK(s != NULL); CHECK_EQ_INT(err, 0); }
            continue;
        }
        CHECK_EQ_INT(err, 0);
        uint64_t bytes = 0;
        int seeks = 0, armed = 1;
        stream_io_info(s, &bytes, &seeks, &armed);
        CHECK_EQ_INT(armed, 0); /* wall-clock open deadline must be disarmed */

        int rate = 0, pkts = 0;
        int64_t frames = 0;
        CHECK_EQ_INT(decode_all(s, &rate, &frames, &pkts), 0);
        /* v00.31: native rate. LC is 32 kHz; HE-AAC SBR decodes at 64 kHz -> 48 kHz. */
        CHECK(rate == 32000 || rate == 48000);
        if (i == 0) CHECK_EQ_INT(rate, 32000);
        /* 20s (or 12s) of stereo frames. The v0.2.3 bug stopped after one AAC frame (~20 ms). */
        double secs = rate ? (double)frames / (double)rate : 0;
        CHECK(secs > 8.0);
        CHECK(pkts > 20);

        /* Seek to 3s and confirm we get more PCM, not immediate EOF. */
        double actual = -1;
        CHECK_EQ_INT(stream_seek(s, 3.0, &actual), 0);
        int16_t buf[2048 * 2];
        int ch = 0, r2 = 0;
        int n = stream_read(s, buf, 2048, &ch, &r2);
        CHECK(n > 256);
        CHECK(energy(buf, n) > 10);
        stream_io_info(s, &bytes, &seeks, &armed);
        CHECK_EQ_INT(armed, 0);
        CHECK(bytes > 0);
        stream_close(s);
    }
}

static struct {
    int opened, writes, frames;
    int rate, ch;
} G;

static int dummy_open(void *ud, int rate, int ch) {
    (void)ud;
    G.opened++;
    G.rate = rate;
    G.ch = ch;
    return 0;
}
static int dummy_write(void *ud, const int16_t *pcm, int frames) {
    (void)ud;
    (void)pcm;
    G.writes++;
    G.frames += frames;
    return frames;
}
static void dummy_close(void *ud) { (void)ud; }
/* 1024 frames per ms ~ 23x realtime: blocking like a device, still quick. */
static int slow_write(void *ud, const int16_t *pcm, int frames) {
    (void)ud;
    (void)pcm;
    G.writes++;
    G.frames += frames;
    sleep_ms(1);
    return frames;
}

TEST(engine_plays_without_jumping_to_end) {
    const char *path = "build-host/fixtures/lc.m4b";
    CHECK(file_ok(path));

    memset(&G, 0, sizeof G);
    AudioSink sink = { .open = dummy_open, .write = dummy_write, .close = dummy_close };
    CHECK_EQ_INT(engine_init(&sink), 0);

    Track track;
    memset(&track, 0, sizeof track);
    snprintf(track.path, sizeof track.path, "%s", path);
    snprintf(track.ext, sizeof track.ext, ".m4b");
    track.duration = 20;
    TrackSource src;
    memset(&src, 0, sizeof src);
    snprintf(src.path, sizeof src.path, "%s", path);
    snprintf(src.ext, sizeof src.ext, ".m4b");

    PlayableBook book = { 1, &track, &src, 20 };
    engine_load(&book, 0, true);

    /* Dummy sink does not block, so a 20 s file finishes in milliseconds.
     * The v0.2.3 bug would have stopped after one AAC frame (~1024 samples). */
    int saw_playing = 0;
    for (int i = 0; i < 100; i++) {
        EngineState st = engine_state();
        if (st == ENG_ERROR) {
            fprintf(stderr, "engine error %d\n", engine_error());
            break;
        }
        if (st == ENG_PLAYING || st == ENG_BUFFERING) saw_playing = 1;
        if (st == ENG_ENDED) break;
        if (G.frames > 48000 * 5) break;
        sleep_ms(50);
    }
    CHECK(saw_playing || G.frames > 0);
    CHECK(G.opened >= 1);
    CHECK(G.frames > 48000 * 10); /* ~10+ seconds of PCM, not a single frame */

    engine_seek(8.0);
    /* engine_play() on ENG_ENDED restarts at 0; wait until the seek is applied. */
    for (int i = 0; i < 50 && engine_state() == ENG_ENDED; i++) sleep_ms(10);
    engine_play();
    int frames_after = G.frames;
    for (int i = 0; i < 50 && G.frames <= frames_after; i++) sleep_ms(10);
    CHECK(G.frames > frames_after); /* still producing after seek */

    engine_stop();
    engine_exit();
}


/* FLAC like a multi-disc rip: 44.1 kHz stereo 16-bit with a big embedded PICTURE.
 * v0.2.6 sent this into find_stream_info (25.5 s on Vita) because FFmpeg 9's
 * flac demuxer leaves sample_rate/channels at 0 until the parser fills them. */
static int make_flac(const char *path, int seconds, int freq) {
    char cmd[1200];
    const char *pic = "build-host/fixtures/flac/pic.jpg";
    if (!file_ok(pic)) {
        snprintf(cmd, sizeof cmd,
                 "ffmpeg -hide_banner -loglevel error -y -f lavfi -i \"testsrc=s=1400x1400:d=1\" "
                 "-frames:v 1 -q:v 2 %s", pic);
        if (run_cmd(cmd) != 0) return -1;
    }
    snprintf(cmd, sizeof cmd,
             "ffmpeg -hide_banner -loglevel error -y "
             "-f lavfi -i \"sine=frequency=%d:sample_rate=44100:duration=%d\" "
             "-f lavfi -i \"anoisesrc=d=%d:c=pink:r=44100:a=0.05\" "
             "-i %s "
             "-filter_complex \"[0][1]amix=inputs=2:duration=first,aformat=channel_layouts=stereo[a]\" "
             "-map \"[a]\" -map 2:v -c:a flac -sample_fmt s16 "
             "-c:v copy -disposition:v attached_pic \"%s\"",
             freq, seconds, seconds, pic, path);
    if (run_cmd(cmd) != 0) return -1;
    return file_ok(path) ? 0 : -1;
}

TEST(flac_play_open_without_find_stream_info) {
    const char *dir = "build-host/fixtures/flac";
    mkdir("build-host/fixtures", 0777);
    mkdir(dir, 0777);
    char path[256];
    snprintf(path, sizeof path, "%s/01 - Called Back.flac", dir);
    CHECK_EQ_INT(make_flac(path, 120, 330), 0);

    /* STREAMINFO parser on the raw file header (fLaC + block header + body). */
    {
        FILE *f = fopen(path, "rb");
        uint8_t hdr[64];
        size_t n = f ? fread(hdr, 1, sizeof hdr, f) : 0;
        if (f) fclose(f);
        CHECK(n >= 42);
        int rate = 0, ch = 0, bits = 0;
        int64_t samples = 0;
        CHECK_EQ_INT(stream_parse_flac_streaminfo(hdr, (int)n, &rate, &ch, &bits, &samples), 0);
        CHECK_EQ_INT(rate, 44100);
        CHECK_EQ_INT(ch, 2);
        CHECK_EQ_INT(bits, 16);
        CHECK_EQ_INT(samples, 44100LL * 120);
        CHECK(stream_parse_flac_streaminfo(hdr, 20, &rate, &ch, &bits, &samples) != 0);
    }

    TrackSource src;
    memset(&src, 0, sizeof src);
    snprintf(src.path, sizeof src.path, "%s", path);
    snprintf(src.ext, sizeof src.ext, ".flac");
    int before = stream_find_info_calls();
    uint64_t t0 = now_ms();
    int err = 0;
    Stream *s = stream_open(&src, &err);
    uint64_t open_ms = now_ms() - t0;
    CHECK(s != NULL);
    CHECK_EQ_INT(stream_find_info_calls(), before); /* no find_stream_info on the play path */
    CHECK(open_ms < 500);
    printf("  flac play-open %llu ms\n", (unsigned long long)open_ms);
    if (s) {
        int16_t buf[4096 * 2];
        int ch = 0, rate = 0;
        int n = stream_read(s, buf, 4096, &ch, &rate);
        CHECK(n > 0);
        CHECK_EQ_INT(rate, 44100);
        CHECK(energy(buf, n) > 10);
        double actual = 0;
        CHECK_EQ_INT(stream_seek(s, 63.0, &actual), 0);
        n = stream_read(s, buf, 4096, &ch, &rate);
        CHECK(n > 0);
        int armed = 1;
        stream_io_info(s, NULL, NULL, &armed);
        CHECK_EQ_INT(armed, 0);
        stream_close(s);
    }

    /* MP3 (codec-only codecpar after open) also skips it: the first frame decides. */
    char mp3[256];
    snprintf(mp3, sizeof mp3, "%s/track.mp3", dir);
    char cmd[600];
    snprintf(cmd, sizeof cmd,
             "ffmpeg -hide_banner -loglevel error -y -f lavfi -i sine=frequency=500:sample_rate=22050:duration=10 "
             "-c:a libmp3lame -b:a 48k -ac 1 \"%s\"", mp3);
    if (run_cmd(cmd) == 0) {
        TrackSource m;
        memset(&m, 0, sizeof m);
        snprintf(m.path, sizeof m.path, "%s", mp3);
        snprintf(m.ext, sizeof m.ext, ".mp3");
        before = stream_find_info_calls();
        Stream *ms = stream_open(&m, &err);
        CHECK(ms != NULL);
        CHECK_EQ_INT(stream_find_info_calls(), before);
        if (ms) {
            int16_t buf[4096 * 2];
            int ch = 0, rate = 0;
            int n = stream_read(ms, buf, 4096, &ch, &rate);
            CHECK(n > 0);
            CHECK_EQ_INT(rate, 22050); /* v00.31: 22.05 kHz plays at its own rate */
            stream_close(ms);
        }
    }
}

/* Multi-file FLAC book: track 2 and 3 come from the background preopen and
 * playback runs straight through (gapless switch, no synchronous open). */
TEST(engine_multitrack_flac_preopen) {
    const char *dir = "build-host/fixtures/flac";
    char paths[3][256];
    Track tracks[3];
    TrackSource srcs[3];
    memset(tracks, 0, sizeof tracks);
    memset(srcs, 0, sizeof srcs);
    double off = 0;
    for (int i = 0; i < 3; i++) {
        snprintf(paths[i], sizeof paths[i], "%s/disc_%02d.flac", dir, i + 1);
        CHECK_EQ_INT(make_flac(paths[i], 6, 300 + 100 * i), 0);
        snprintf(tracks[i].path, sizeof tracks[i].path, "%s", paths[i]);
        snprintf(tracks[i].ext, sizeof tracks[i].ext, ".flac");
        tracks[i].index = i;
        tracks[i].duration = 6;
        tracks[i].start_offset = off;
        off += 6;
        snprintf(srcs[i].path, sizeof srcs[i].path, "%s", paths[i]);
        snprintf(srcs[i].ext, sizeof srcs[i].ext, ".flac");
    }
    memset(&G, 0, sizeof G);
    AudioSink sink = { .open = dummy_open, .write = slow_write, .close = dummy_close };
    CHECK_EQ_INT(engine_init(&sink), 0);
    int hits0 = engine_preopen_hits();
    int calls0 = stream_find_info_calls();
    PlayableBook book = { 3, tracks, srcs, 18 };
    CHECK(!engine_audio_started());
    engine_load(&book, 0, true);
    CHECK(!engine_audio_started());
    int started = 0;
    for (int i = 0; i < 1500; i++) {
        if (engine_audio_started()) started = 1;
        EngineState st = engine_state();
        if (st == ENG_ERROR) { fprintf(stderr, "engine error %d\n", engine_error()); break; }
        if (st == ENG_ENDED) break;
        sleep_ms(10);
    }
    CHECK(started);
    CHECK_EQ_INT(engine_state(), ENG_ENDED);
    CHECK_EQ_INT(engine_preopen_hits() - hits0, 2);
    CHECK_EQ_INT(stream_find_info_calls(), calls0);
    /* All 18 s of PCM came out (44.1 kHz), nothing dropped at the switches. */
    CHECK(G.frames > 44100 * 18 - 4096);
    CHECK(G.frames < 44100 * 18 + 8192);
    engine_stop();
    engine_exit();
}

/* ---- v0.2.12: stall watchdog only counts blocked device I/O ------------------ */

static volatile uint64_t g_fake_ms = 1000000;
static volatile uint64_t g_fake_step; /* added on every clock read */
static uint64_t fake_clock(void) {
    return __atomic_add_fetch(&g_fake_ms, g_fake_step, __ATOMIC_RELAXED);
}

static int make_mp3(const char *path, int secs, int freq) {
    char cmd[700];
    snprintf(cmd, sizeof cmd,
             "ffmpeg -hide_banner -loglevel error -y -f lavfi -i sine=frequency=%d:sample_rate=32000:duration=%d "
             "-ac 1 -b:a 64k %s", freq, secs, path);
    if (run_cmd(cmd) != 0) return -1;
    return file_ok(path) ? 0 : -1;
}

static Stream *open_src(const char *path, const char *ext) {
    TrackSource src;
    memset(&src, 0, sizeof src);
    snprintf(src.path, sizeof src.path, "%s", path);
    snprintf(src.ext, sizeof src.ext, "%s", ext);
    int err = 0;
    Stream *s = stream_open(&src, &err);
    if (!s) fprintf(stderr, "open %s failed %d\n", path, err);
    return s;
}

/* Decodes to EOF; every chunk advances simulated time by its real duration
 * (+ extra_ms), and after first_frames frames simulated time jumps pause_ms. */
static int64_t play_simulated(Stream *s, int extra_ms, int64_t first_frames, uint64_t pause_ms, int *rc_out) {
    int16_t buf[1024 * 2];
    int64_t frames = 0;
    int paused = 0;
    *rc_out = 0;
    for (int guard = 0; guard < 1000000; guard++) {
        int ch = 0, rate = 0;
        int n = stream_read(s, buf, 1024, &ch, &rate);
        if (n == STREAM_EOF) return frames;
        if (n < 0) { *rc_out = n; fprintf(stderr, "stream_read %d after %lld frames\n", n, (long long)frames); return frames; }
        frames += n;
        __atomic_add_fetch(&g_fake_ms, (uint64_t)(n * 1000 / (rate ? rate : 48000)) + (uint64_t)extra_ms, __ATOMIC_RELAXED);
        if (!paused && pause_ms && frames >= first_frames) {
            __atomic_add_fetch(&g_fake_ms, pause_ms, __ATOMIC_RELAXED);
            paused = 1;
        }
    }
    *rc_out = -99;
    return frames;
}

static int64_t reference_frames(const char *path, const char *ext) {
    stream_test_set_clock(NULL);
    Stream *s = open_src(path, ext);
    if (!s) return -1;
    int rc = 0;
    int64_t f = play_simulated(s, 0, 0, 0, &rc);
    stream_close(s);
    return rc ? -1 : f;
}

/* The v0.2.11 bug: a small MP3 is fully in the readahead/AVIO buffers, no
 * device I/O happens for > 15 s, and the old "time since last I/O" watchdog
 * cut the track short. */
TEST(stall_small_file_fully_buffered) {
    const char *path = "build-host/fixtures/small40.mp3";
    CHECK_EQ_INT(make_mp3(path, 40, 330), 0);
    int64_t ref = reference_frames(path, ".mp3");
    CHECK(ref > 32000 * 39); /* 32 kHz source, native output */
    stream_test_set_clock(fake_clock);
    g_fake_step = 0;
    Stream *s = open_src(path, ".mp3");
    CHECK(s != NULL);
    if (!s) { stream_test_set_clock(NULL); return; }
    uint64_t t0 = g_fake_ms;
    int rc = 0;
    int64_t f = play_simulated(s, 0, 0, 0, &rc);
    CHECK_EQ_INT(rc, 0);
    CHECK_EQ_INT((int)(f - ref), 0);       /* every sample, no early cut */
    CHECK(g_fake_ms - t0 > 30000);          /* > 15 s of simulated play with no device I/O */
    CHECK_EQ_INT(stream_stalled(s), 0);
    stream_close(s);
    stream_test_set_clock(NULL);
}

/* Paused for half an hour mid-track (and a preopened track idle for 10 min
 * before its first read): resume plays to the end. */
TEST(stall_long_pause_and_idle_preopen) {
    const char *path = "build-host/fixtures/small40.mp3";
    int64_t ref = reference_frames(path, ".mp3");
    CHECK(ref > 0);
    stream_test_set_clock(fake_clock);
    g_fake_step = 0;
    Stream *s = open_src(path, ".mp3");
    CHECK(s != NULL);
    if (s) {
        int rc = 0;
        int64_t f = play_simulated(s, 0, 48000 * 5, 30ull * 60 * 1000, &rc);
        CHECK_EQ_INT(rc, 0);
        CHECK_EQ_INT((int)(f - ref), 0);
        CHECK_EQ_INT(stream_stalled(s), 0);
        stream_close(s);
    }
    s = open_src(path, ".mp3");
    CHECK(s != NULL);
    if (s) {
        __atomic_add_fetch(&g_fake_ms, 10ull * 60 * 1000, __ATOMIC_RELAXED); /* idle preopen */
        int rc = 0;
        int64_t f = play_simulated(s, 0, 0, 0, &rc);
        CHECK_EQ_INT(rc, 0);
        CHECK_EQ_INT((int)(f - ref), 0);
        stream_close(s);
    }
    stream_test_set_clock(NULL);
}

/* Slow device reads that still complete (every clock read jumps 20 s, so each
 * device call "takes" > 15 s) are logged, never fatal: MP3 and the lazy MP4 path. */
TEST(stall_slow_reads_complete_mp3_and_mp4lazy) {
    const char *files[2][2] = { { "build-host/fixtures/small40.mp3", ".mp3" },
                                { "build-host/fixtures/lc.m4b", ".m4b" } };
    for (int i = 0; i < 2; i++) {
        t_current = files[i][0];
        int64_t ref = reference_frames(files[i][0], files[i][1]);
        CHECK(ref > 0);
        stream_test_set_clock(fake_clock);
        g_fake_step = 0;
        Stream *s = open_src(files[i][0], files[i][1]);
        CHECK(s != NULL);
        if (!s) continue;
        if (i == 1) CHECK_EQ_INT(stream_uses_mp4lazy(s), 1);
        double actual = 0;
        CHECK_EQ_INT(stream_seek(s, 0, &actual), 0); /* drop buffers: reads hit the device again */
        g_fake_step = 20000;
        int rc = 0;
        int64_t f = play_simulated(s, 0, 0, 0, &rc);
        g_fake_step = 0;
        CHECK_EQ_INT(rc, 0);
        CHECK(f >= ref - 2048 && f <= ref + 2048);
        CHECK_EQ_INT(stream_stalled(s), 0);
        stream_close(s);
    }
    stream_test_set_clock(NULL);
}

/* Simulated time runs 20x faster than the audio written (each 1024-frame
 * write = ~0.43 s of fake time), so both small tracks sit fully buffered for
 * minutes of "time" and the preopened track idles long before use. */
static int fast_clock_write(void *ud, const int16_t *pcm, int frames) {
    __atomic_add_fetch(&g_fake_ms, (uint64_t)frames * 20000 / (G.rate > 0 ? G.rate : 48000), __ATOMIC_RELAXED);
    return slow_write(ud, pcm, frames);
}

/* Engine: a two-track MP3 book plays straight through (no stall, no recovery,
 * nothing skipped). */
TEST(engine_small_mp3_tracks_no_stall) {
    const char *paths[2] = { "build-host/fixtures/small40.mp3", "build-host/fixtures/small20.mp3" };
    CHECK_EQ_INT(make_mp3(paths[1], 20, 550), 0);
    Track tracks[2];
    TrackSource srcs[2];
    memset(tracks, 0, sizeof tracks);
    memset(srcs, 0, sizeof srcs);
    double durs[2] = { 40, 20 }, off = 0;
    for (int i = 0; i < 2; i++) {
        snprintf(tracks[i].path, sizeof tracks[i].path, "%s", paths[i]);
        snprintf(tracks[i].ext, sizeof tracks[i].ext, ".mp3");
        tracks[i].index = i;
        tracks[i].duration = durs[i];
        tracks[i].start_offset = off;
        off += durs[i];
        snprintf(srcs[i].path, sizeof srcs[i].path, "%s", paths[i]);
        snprintf(srcs[i].ext, sizeof srcs[i].ext, ".mp3");
    }
    int64_t ref = reference_frames(paths[0], ".mp3") + reference_frames(paths[1], ".mp3");
    stream_test_set_clock(fake_clock);
    g_fake_step = 0;
    uint64_t t0 = g_fake_ms;
    memset(&G, 0, sizeof G);
    AudioSink sink = { .open = dummy_open, .write = fast_clock_write, .close = dummy_close };
    CHECK_EQ_INT(engine_init(&sink), 0);
    int rec0 = engine_recoveries();
    PlayableBook book = { 2, tracks, srcs, 60 };
    engine_load(&book, 0, true);
    for (int i = 0; i < 3000; i++) {
        EngineState st = engine_state();
        if (st == ENG_ERROR) { fprintf(stderr, "engine error %d\n", engine_error()); break; }
        if (st == ENG_ENDED) break;
        sleep_ms(10);
    }
    CHECK_EQ_INT(engine_state(), ENG_ENDED);
    CHECK_EQ_INT(engine_recoveries() - rec0, 0);
    CHECK(G.frames >= ref - 4096 && G.frames <= ref + 8192);
    CHECK(g_fake_ms - t0 > 15ull * 60 * 1000); /* > 15 min of simulated time */
    engine_stop();
    engine_exit();
    g_fake_step = 0;
    stream_test_set_clock(NULL);
}

/* ---- v00.31: native output rate, decode-ahead, output diagnostics ---------------- */

TEST(native_rate_table) {
    static const int ok[] = { 8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000 };
    for (int i = 0; i < 9; i++) {
        CHECK_EQ_INT(stream_pick_out_rate(ok[i]), ok[i]);
        CHECK_EQ_INT(stream_port_rate_supported(ok[i]), 1);
    }
    CHECK_EQ_INT(stream_pick_out_rate(88200), 44100);
    CHECK_EQ_INT(stream_pick_out_rate(176400), 44100);
    CHECK_EQ_INT(stream_pick_out_rate(64000), 48000);   /* HE-AAC SBR of 32 kHz */
    CHECK_EQ_INT(stream_pick_out_rate(96000), 48000);
    CHECK_EQ_INT(stream_pick_out_rate(192000), 48000);
    CHECK_EQ_INT(stream_pick_out_rate(37800), 44100);
    CHECK_EQ_INT(stream_pick_out_rate(7350), 8000);
    CHECK_EQ_INT(stream_pick_out_rate(0), 48000);
    CHECK_EQ_INT(stream_port_rate_supported(64000), 0);
}

/* Recording sink: format of every write, reconfig calls, realtime pacing optional. */
static struct {
    int opens, reconfigs, closes, discards, drains;
    int rate, ch;
    int64_t frames_at[2];        /* frames written at 32000 / 44100 */
    int bad_format_writes;
    int realtime;                /* write blocks for the audio length */
    volatile int64_t total;
    volatile uint64_t last_write_ms;
} R;

static int r_open(void *ud, int rate, int ch) { (void)ud; R.opens++; R.rate = rate; R.ch = ch; return 0; }
static int r_reconfig(void *ud, int rate, int ch) { (void)ud; R.reconfigs++; R.rate = rate; R.ch = ch; return 0; }
static void r_close(void *ud) { (void)ud; R.closes++; }
static void r_discard(void *ud) { (void)ud; R.discards++; }
static void r_drain(void *ud) { (void)ud; R.drains++; }
static int r_write(void *ud, const int16_t *pcm, int frames) {
    (void)ud; (void)pcm;
    if (R.rate == 32000) R.frames_at[0] += frames;
    else if (R.rate == 44100) R.frames_at[1] += frames;
    else R.bad_format_writes++;
    R.total += frames;
    if (R.realtime) sleep_ms(frames * 1000 / (R.rate > 0 ? R.rate : 48000));
    R.last_write_ms = now_ms();
    return frames;
}

static void track_at(Track *t, TrackSource *s, const char *path, const char *ext, int idx, double dur, double off) {
    memset(t, 0, sizeof *t);
    memset(s, 0, sizeof *s);
    snprintf(t->path, sizeof t->path, "%s", path);
    snprintf(t->ext, sizeof t->ext, "%s", ext);
    t->index = idx;
    t->duration = dur;
    t->start_offset = off;
    snprintf(s->path, sizeof s->path, "%s", path);
    snprintf(s->ext, sizeof s->ext, "%s", ext);
}

static int wait_state(EngineState want, int ms) {
    for (int t = 0; t < ms; t += 10) {
        EngineState st = engine_state();
        if (st == want) return 1;
        if (st == ENG_ERROR) { fprintf(stderr, "engine error %d\n", engine_error()); return 0; }
        sleep_ms(10);
    }
    return engine_state() == want;
}

/* 32 kHz MP3 then 44.1 kHz FLAC (preopened): the device is reconfigured in place
 * at the boundary, every chunk goes out at its own rate, nothing is lost. */
TEST(engine_rate_change_between_tracks) {
    Track tr[2];
    TrackSource src[2];
    track_at(&tr[0], &src[0], "build-host/fixtures/small20.mp3", ".mp3", 0, 20, 0);
    track_at(&tr[1], &src[1], "build-host/fixtures/flac/disc_01.flac", ".flac", 1, 6, 20);
    int64_t ref0 = reference_frames(src[0].path, ".mp3");
    int64_t ref1 = reference_frames(src[1].path, ".flac");
    CHECK(ref0 > 0 && ref1 > 0);
    memset(&R, 0, sizeof R);
    AudioSink sink = { .open = r_open, .write = r_write, .close = r_close,
                       .reconfig = r_reconfig, .discard = r_discard, .drain = r_drain };
    CHECK_EQ_INT(engine_init(&sink), 0);
    PlayableBook book = { 2, tr, src, 26 };
    engine_load(&book, 0, true);
    CHECK(wait_state(ENG_ENDED, 20000));
    CHECK_EQ_INT(R.opens, 1);
    CHECK_EQ_INT(R.reconfigs, 1);
    CHECK_EQ_INT(R.bad_format_writes, 0);
    CHECK(R.frames_at[0] >= ref0 - 2048 && R.frames_at[0] <= ref0 + 2048);
    CHECK(R.frames_at[1] >= ref1 - 2048 && R.frames_at[1] <= ref1 + 2048);
    CHECK(R.drains >= 1);                    /* last partial grain pushed out at the end */
    CHECK(engine_position() > 25.9);
    engine_stop();
    engine_exit();
    CHECK_EQ_INT(R.closes, 1);               /* the one device is closed once, nothing leaked */
}

/* Realtime sink: pause stops output within 150 ms despite ~1 s decode-ahead,
 * seek reports the new position at once and restarts output quickly, and the
 * position follows what was written, not what was decoded. */
TEST(engine_decode_ahead_pause_seek_latency) {
    Track tr;
    TrackSource src;
    track_at(&tr, &src, "build-host/fixtures/small40.mp3", ".mp3", 0, 40, 0);
    memset(&R, 0, sizeof R);
    R.realtime = 1;
    AudioSink sink = { .open = r_open, .write = r_write, .close = r_close,
                       .reconfig = r_reconfig, .discard = r_discard, .drain = r_drain };
    CHECK_EQ_INT(engine_init(&sink), 0);
    PlayableBook book = { 1, &tr, &src, 40 };
    engine_load(&book, 0, true);
    for (int i = 0; i < 300 && R.total < 32000; i++) sleep_ms(10);
    sleep_ms(400);
    /* position = frames written / rate (not the decode-ahead) */
    double pos = engine_position();
    double written = (double)R.total / 32000.0;
    CHECK(pos > 0.5);
    CHECK(pos <= written + 0.05 && pos >= written - 0.1);
    engine_pause();
    uint64_t t0 = now_ms();
    sleep_ms(150);
    int64_t f1 = R.total;
    sleep_ms(300);
    CHECK_EQ_INT((int)(R.total - f1), 0);    /* silent within 150 ms of pause */
    double ppos = engine_position();
    CHECK(fabs(ppos - (double)R.total / 32000.0) < 0.05);
    (void)t0;
    engine_play();
    for (int i = 0; i < 30 && R.total == f1; i++) sleep_ms(5);
    CHECK(R.total > f1);
    /* seek: position at once, output from the new place within 150 ms */
    engine_seek(30.0);
    CHECK(fabs(engine_position() - 30.0) < 0.001);
    uint64_t ts = now_ms();
    for (int i = 0; i < 100 && engine_position() < 30.01; i++) sleep_ms(2);
    CHECK(engine_position() >= 30.0 && engine_position() < 30.5);
    CHECK(now_ms() - ts < 150);
    CHECK(R.discards >= 1);
    CHECK_EQ_INT(engine_underruns(), 0);
    engine_stop();
    engine_exit();
}

/* 00.36: a big book opened with estimated lengths gets the exact ones while it
 * plays: no flush, no gap, the clock and the position follow the new timeline. */
TEST(engine_retime_while_playing) {
    Track tr[2];
    TrackSource src[2];
    /* track 2 really starts at 20 s; it was loaded as 26 s in (track 1 estimated long) */
    track_at(&tr[0], &src[0], "build-host/fixtures/small20.mp3", ".mp3", 0, 26, 0);
    track_at(&tr[1], &src[1], "build-host/fixtures/small40.mp3", ".mp3", 1, 30, 26);
    memset(&R, 0, sizeof R);
    R.realtime = 1;
    AudioSink sink = { .open = r_open, .write = r_write, .close = r_close,
                       .reconfig = r_reconfig, .discard = r_discard, .drain = r_drain };
    CHECK_EQ_INT(engine_init(&sink), 0);
    PlayableBook book = { 2, tr, src, 56 };
    engine_load(&book, 30.0, true); /* 4 s into track 2 by the estimate */
    for (int i = 0; i < 300 && R.total < 16000; i++) sleep_ms(10);
    sleep_ms(200);
    double before = engine_position();
    CHECK(before > 30.2 && before < 31.5);
    int discards = R.discards;
    int64_t f0 = R.total;
    Track ex[2];
    ex[0] = tr[0];
    ex[1] = tr[1];
    ex[0].duration = 20;
    ex[1].start_offset = 20;
    ex[1].duration = 40;
    engine_retime(ex, 2, 60);
    CHECK_NEAR(engine_duration(), 60, 1e-9);
    sleep_ms(150);
    double after = engine_position();
    /* moved 6 s earlier (plus the 150 ms that played) */
    CHECK(after > before - 6.1 && after < before - 5.6);
    CHECK_EQ_INT(R.discards, discards);     /* nothing flushed */
    CHECK(R.total > f0);                    /* still playing */
    CHECK_EQ_INT(engine_underruns(), 0);
    /* a seek uses the new timeline: 25 s = 5 s into track 2 */
    engine_seek(25.0);
    for (int i = 0; i < 100 && engine_position() < 25.01; i++) sleep_ms(2);
    CHECK(engine_position() >= 25.0 && engine_position() < 25.5);
    /* a retime for another track count is ignored */
    engine_retime(ex, 1, 20);
    sleep_ms(50);
    CHECK(engine_position() >= 25.0 && engine_position() < 25.6);
    printf("  retime: %.2f s -> %.2f s, no flush, no underrun\n", before, after);
    engine_stop();
    engine_exit();
}

int main(void) {
    printf("suite_ffstream_host\n");
    t_current = "stream_decode_and_seek";
    stream_decode_and_seek();
    t_current = "engine_plays_without_jumping_to_end";
    engine_plays_without_jumping_to_end();
    t_current = "flac_play_open_without_find_stream_info";
    flac_play_open_without_find_stream_info();
    t_current = "engine_multitrack_flac_preopen";
    engine_multitrack_flac_preopen();
    t_current = "stall_small_file_fully_buffered";
    stall_small_file_fully_buffered();
    t_current = "stall_long_pause_and_idle_preopen";
    stall_long_pause_and_idle_preopen();
    stall_slow_reads_complete_mp3_and_mp4lazy();
    t_current = "engine_small_mp3_tracks_no_stall";
    engine_small_mp3_tracks_no_stall();
    t_current = "native_rate_table";
    native_rate_table();
    t_current = "engine_rate_change_between_tracks";
    engine_rate_change_between_tracks();
    t_current = "engine_decode_ahead_pause_seek_latency";
    engine_decode_ahead_pause_seek_latency();
    t_current = "engine_retime_while_playing";
    engine_retime_while_playing();
    suite_library_host();
    suite_mp4lazy_host();
    suite_open_priority_host();
    suite_mp3scan_host();
    printf("suite_desc_host\n");
    suite_desc_host();
    printf("\n%d checks, %d failures\n", t_checks, t_failures);
    return t_failures ? 1 : 0;
}
