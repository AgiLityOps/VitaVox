#include "wavrec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "dbg.h"
#include "osal.h"
#include "strutil.h"

#define RING_BYTES (1024 * 1024) /* ~5 s at 48 kHz stereo s16 */
#define CHUNK_BYTES (64 * 1024)

static void le16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void le32(uint8_t *p, uint32_t v) { le16(p, v & 0xFFFF); le16(p + 2, v >> 16); }

void wav_header(uint8_t h[44], int rate, int channels, uint32_t data_bytes) {
    uint32_t align = (uint32_t)channels * 2;
    memcpy(h, "RIFF", 4);
    le32(h + 4, 36 + data_bytes);
    memcpy(h + 8, "WAVEfmt ", 8);
    le32(h + 16, 16);
    le16(h + 20, 1);                 /* PCM */
    le16(h + 22, (uint32_t)channels);
    le32(h + 24, (uint32_t)rate);
    le32(h + 28, (uint32_t)rate * align);
    le16(h + 32, align);
    le16(h + 34, 16);
    memcpy(h + 36, "data", 4);
    le32(h + 40, data_bytes);
}

static struct {
    char dir[256];
    char path[320];
    Thread th;
    bool th_started;
    volatile int state;         /* 0 idle, 1 recording, 2 finishing (no more input) */
    volatile int stop_req;
    int rate, ch;               /* format of this recording (0 = not yet known) */
    uint64_t want_frames, got_frames, dropped;
    uint8_t *ring;
    volatile uint32_t head, tail; /* producer writes head, consumer tail (byte counters) */
    FILE *f;
    uint32_t written;
} W;

void wavrec_set_dir(const char *dir) { str_copy(W.dir, sizeof W.dir, dir ? dir : ""); }
int wavrec_active(void) { return W.state != 0; }
uint64_t wavrec_dropped(void) { return W.dropped; }
double wavrec_seconds(void) { return W.rate > 0 ? (double)W.got_frames / W.rate : 0.0; }

static uint32_t ring_used(void) {
    return __atomic_load_n(&W.head, __ATOMIC_ACQUIRE) - __atomic_load_n(&W.tail, __ATOMIC_ACQUIRE);
}

static void drain(int all) {
    for (;;) {
        uint32_t used = ring_used();
        if (!used || (!all && used < CHUNK_BYTES)) return;
        uint32_t t = W.tail;
        uint32_t off = t % RING_BYTES;
        uint32_t k = used < CHUNK_BYTES ? used : CHUNK_BYTES;
        if (k > RING_BYTES - off) k = RING_BYTES - off;
        if (W.f && fwrite(W.ring + off, 1, k, W.f) == k) W.written += k;
        __atomic_store_n(&W.tail, t + k, __ATOMIC_RELEASE);
    }
}

static void *writer(void *arg) {
    (void)arg;
    while (W.state == 1 && !W.stop_req) {
        drain(0);
        sleep_ms(50);
    }
    W.state = 2;
    sleep_ms(5); /* let an in-flight feed finish its copy */
    drain(1);
    uint8_t h[44];
    wav_header(h, W.rate > 0 ? W.rate : 48000, W.ch > 0 ? W.ch : 2, W.written);
    if (W.f) {
        fseek(W.f, 0, SEEK_SET);
        fwrite(h, 1, 44, W.f);
        fclose(W.f);
        W.f = NULL;
    }
    dbg("debug: output recording stopped '%s': %llu frames (%.1f s) at %d Hz x%d, %u bytes, dropped %llu frames",
        W.path, (unsigned long long)W.got_frames, wavrec_seconds(), W.rate, W.ch, (unsigned)W.written,
        (unsigned long long)W.dropped);
    free(W.ring);
    W.ring = NULL;
    W.state = 0;
    return NULL;
}

int wavrec_start(int seconds, char *out, size_t n) {
    if (out && n) out[0] = 0;
    if (W.state != 0 || seconds <= 0 || !W.dir[0]) return -1;
    if (W.th_started) { thread_join(&W.th); W.th_started = false; }
    mkdir(W.dir, 0777);
    time_t now = time(NULL);
    struct tm tmv;
    struct tm *lt = localtime(&now);
    if (lt) tmv = *lt; else memset(&tmv, 0, sizeof tmv);
    snprintf(W.path, sizeof W.path, "%s/out_%04d%02d%02d_%02d%02d%02d.wav", W.dir, tmv.tm_year + 1900,
             tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    W.ring = malloc(RING_BYTES);
    if (!W.ring) return -1;
    W.f = fopen(W.path, "wb");
    if (!W.f) {
        dbg("debug: cannot create '%s'", W.path);
        free(W.ring);
        W.ring = NULL;
        return -1;
    }
    uint8_t h[44];
    wav_header(h, 48000, 2, 0); /* placeholder, rewritten when finished */
    fwrite(h, 1, 44, W.f);
    W.written = 0;
    W.head = W.tail = 0;
    W.rate = W.ch = 0;
    W.got_frames = W.dropped = 0;
    W.want_frames = (uint64_t)seconds; /* converted to frames on the first feed */
    W.stop_req = 0;
    W.state = 1;
    if (!thread_start(&W.th, writer, NULL, 64, -2)) {
        W.state = 0;
        fclose(W.f);
        W.f = NULL;
        free(W.ring);
        W.ring = NULL;
        return -1;
    }
    W.th_started = true;
    dbg("debug: output recording started '%s' (%d s)", W.path, seconds);
    if (out && n) str_copy(out, n, W.path);
    return 0;
}

void wavrec_stop(void) {
    if (W.state == 1) W.stop_req = 1;
}

int wavrec_wait(int ms) {
    for (int t = 0; W.state != 0 && t < ms; t += 10) sleep_ms(10);
    if (W.state == 0 && W.th_started) { thread_join(&W.th); W.th_started = false; }
    return W.state == 0 ? 0 : -1;
}

void wavrec_feed(const int16_t *pcm, int frames, int rate, int channels) {
    if (W.state != 1 || W.stop_req || frames <= 0 || !pcm) return;
    if (!W.rate) {
        W.rate = rate;
        W.ch = channels;
        W.want_frames *= (uint64_t)rate;
    } else if (rate != W.rate || channels != W.ch) {
        dbg("debug: output format changed to %d Hz x%d, recording ends", rate, channels);
        W.stop_req = 1;
        return;
    }
    if (W.got_frames + (uint64_t)frames > W.want_frames) frames = (int)(W.want_frames - W.got_frames);
    uint32_t bytes = (uint32_t)frames * (uint32_t)channels * 2;
    uint32_t used = ring_used();
    if (bytes > RING_BYTES - used) {
        W.dropped += (uint64_t)frames; /* writer fell behind: never block the audio thread */
    } else {
        uint32_t h = W.head;
        uint32_t off = h % RING_BYTES;
        uint32_t k = bytes < RING_BYTES - off ? bytes : RING_BYTES - off;
        memcpy(W.ring + off, pcm, k);
        if (k < bytes) memcpy(W.ring, (const uint8_t *)pcm + k, bytes - k);
        __atomic_store_n(&W.head, h + bytes, __ATOMIC_RELEASE);
    }
    W.got_frames += (uint64_t)frames;
    if (W.got_frames >= W.want_frames) W.stop_req = 1;
}
