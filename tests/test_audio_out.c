/* v00.31 output diagnostics (audiodiag.c) and the debug WAV recorder (wavrec.c). */
#include "test.h"
#include "audiodiag.h"
#include "osal.h"
#include "wavrec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t rd16(const uint8_t *p) { return p[0] | p[1] << 8; }

TEST(wav_header_fields) {
    uint8_t h[44];
    wav_header(h, 32000, 2, 32000 * 4 * 3);
    CHECK(!memcmp(h, "RIFF", 4));
    CHECK_EQ_INT((int)rd32(h + 4), 36 + 32000 * 4 * 3);
    CHECK(!memcmp(h + 8, "WAVEfmt ", 8));
    CHECK_EQ_INT((int)rd32(h + 16), 16);
    CHECK_EQ_INT((int)rd16(h + 20), 1);
    CHECK_EQ_INT((int)rd16(h + 22), 2);
    CHECK_EQ_INT((int)rd32(h + 24), 32000);
    CHECK_EQ_INT((int)rd32(h + 28), 32000 * 4);
    CHECK_EQ_INT((int)rd16(h + 32), 4);
    CHECK_EQ_INT((int)rd16(h + 34), 16);
    CHECK(!memcmp(h + 36, "data", 4));
    CHECK_EQ_INT((int)rd32(h + 40), 32000 * 4 * 3);
    wav_header(h, 44100, 1, 0);
    CHECK_EQ_INT((int)rd32(h + 4), 36);
    CHECK_EQ_INT((int)rd32(h + 28), 88200);
    CHECK_EQ_INT((int)rd16(h + 32), 2);
}

static uint8_t *slurp(const char *path, long *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)*n);
    if (b && fread(b, 1, (size_t)*n, f) != (size_t)*n) { free(b); b = NULL; }
    fclose(f);
    return b;
}

/* Fed in odd-sized chunks past its limit: exactly 2 s of the exact samples,
 * header sizes match the file. */
TEST(wavrec_exact_length_and_samples) {
    CHECK_EQ_INT(system("rm -rf build-host/tmp-wav && mkdir -p build-host/tmp-wav"), 0);
    wavrec_set_dir("build-host/tmp-wav/debug");
    char path[320];
    CHECK_EQ_INT(wavrec_start(2, path, sizeof path), 0);
    CHECK(strstr(path, "build-host/tmp-wav/debug/out_") != NULL);
    CHECK(strstr(path, ".wav") != NULL);
    CHECK_EQ_INT(wavrec_start(2, NULL, 0), -1); /* one at a time */
    const int rate = 32000;
    int16_t chunk[777 * 2];
    int sent = 0;
    for (int k = 0; k < 120; k++) {           /* ~2.9 s offered */
        for (int i = 0; i < 777; i++) {
            int n = sent + i;
            chunk[i * 2] = (int16_t)(n & 0x7FFF);
            chunk[i * 2 + 1] = (int16_t)-(n & 0x7FFF);
        }
        wavrec_feed(chunk, 777, rate, 2);
        sent += 777;
        if (k % 10 == 0) sleep_ms(2);
    }
    CHECK_EQ_INT(wavrec_wait(3000), 0);
    CHECK_EQ_INT(wavrec_active(), 0);
    CHECK_EQ_INT((int)wavrec_dropped(), 0);
    long n = 0;
    uint8_t *b = slurp(path, &n);
    CHECK(b != NULL);
    if (b) {
        CHECK_EQ_INT((int)n, 44 + rate * 2 * 4);
        CHECK_EQ_INT((int)rd32(b + 4), (int)n - 8);
        CHECK_EQ_INT((int)rd32(b + 24), rate);
        CHECK_EQ_INT((int)rd16(b + 22), 2);
        CHECK_EQ_INT((int)rd32(b + 40), rate * 2 * 4);
        const int16_t *pcm = (const int16_t *)(b + 44);
        int bad = 0;
        for (int i = 0; i < rate * 2; i++)
            if (pcm[i * 2] != (int16_t)(i & 0x7FFF) || pcm[i * 2 + 1] != (int16_t)-(i & 0x7FFF)) bad++;
        CHECK_EQ_INT(bad, 0);
        free(b);
    }
    CHECK(wavrec_seconds() > 1.99 && wavrec_seconds() < 2.01);
}

/* A format change or stop ends the recording early with a valid file. */
TEST(wavrec_format_change_and_stop) {
    char path[320];
    CHECK_EQ_INT(wavrec_start(60, path, sizeof path), 0);
    int16_t buf[1000 * 2];
    memset(buf, 0, sizeof buf);
    wavrec_feed(buf, 1000, 44100, 2);
    wavrec_feed(buf, 1000, 44100, 2);
    wavrec_feed(buf, 1000, 48000, 2);         /* format change: ends here */
    wavrec_feed(buf, 1000, 44100, 2);         /* ignored */
    CHECK_EQ_INT(wavrec_wait(3000), 0);
    long n = 0;
    uint8_t *b = slurp(path, &n);
    CHECK(b != NULL);
    if (b) {
        CHECK_EQ_INT((int)n, 44 + 2000 * 4);
        CHECK_EQ_INT((int)rd32(b + 24), 44100);
        CHECK_EQ_INT((int)rd32(b + 40), 2000 * 4);
        free(b);
    }
    sleep_ms(1100); /* new second -> new file name */
    char p2[320];
    CHECK_EQ_INT(wavrec_start(60, p2, sizeof p2), 0);
    CHECK(strcmp(p2, path) != 0);
    wavrec_feed(buf, 500, 32000, 1);
    wavrec_stop();
    CHECK_EQ_INT(wavrec_wait(3000), 0);
    b = slurp(p2, &n);
    CHECK(b != NULL);
    if (b) {
        CHECK_EQ_INT((int)n, 44 + 500 * 2);
        CHECK_EQ_INT((int)rd16(b + 22), 1);
        free(b);
    }
}

TEST(underrun_and_late_counters) {
    AudioDiag d;
    adiag_reset(&d);
    /* 1024 frames at 32 kHz = 32 ms queued: a 20 ms wait is harmless, 40 ms ran dry */
    CHECK_EQ_INT(adiag_wait(&d, 20, 1024, 32000), 0);
    CHECK_EQ_INT(adiag_wait(&d, 34, 1024, 32000), 0);   /* within the 2 ms margin */
    CHECK_EQ_INT(adiag_wait(&d, 40, 1024, 32000), 1);
    CHECK_EQ_INT(adiag_wait(&d, 250, 1024, 48000), 1);
    CHECK_EQ_INT((int)d.underruns, 2);
    CHECK_EQ_INT((int)d.waits, 4);
    CHECK_EQ_INT((int)d.max_wait_ms, 250);
    /* writes: 1024 frames at 48 kHz = 21.3 ms; late beyond 2x + 10 ms = 52.7 ms */
    CHECK_EQ_INT(adiag_write(&d, 21, 1024, 48000), 0);
    CHECK_EQ_INT(adiag_write(&d, 50, 1024, 48000), 0);
    CHECK_EQ_INT(adiag_write(&d, 60, 1024, 48000), 1);
    CHECK_EQ_INT(adiag_write(&d, 500, 0, 48000), 0);    /* no frames: not judged */
    CHECK_EQ_INT((int)d.late_writes, 1);
    /* rate limit: one line per second per kind, suppressed count reported */
    uint32_t sk = 99;
    CHECK_EQ_INT(adiag_may_log(&d, ADIAG_UNDERRUN, 10000, &sk), 1);
    CHECK_EQ_INT((int)sk, 0);
    CHECK_EQ_INT(adiag_may_log(&d, ADIAG_UNDERRUN, 10200, &sk), 0);
    CHECK_EQ_INT(adiag_may_log(&d, ADIAG_UNDERRUN, 10900, &sk), 0);
    CHECK_EQ_INT(adiag_may_log(&d, ADIAG_LATE, 10900, &sk), 1); /* other kind independent */
    CHECK_EQ_INT(adiag_may_log(&d, ADIAG_UNDERRUN, 11001, &sk), 1);
    CHECK_EQ_INT((int)sk, 2);
    CHECK_EQ_INT(adiag_may_log(&d, 7, 20000, &sk), 0);
    /* clipping */
    int16_t pcm[6] = { 0, 32767, -32768, 32766, -32767, 32767 };
    CHECK_EQ_INT((int)adiag_count_clip(&d, pcm, 6), 3);
    CHECK_EQ_INT((int)d.clipped, 3);
    CHECK(adiag_ms(1024, 32000) > 31.9 && adiag_ms(1024, 32000) < 32.1);
    CHECK(adiag_ms(10, 0) == 0.0);
}

void suite_audio_out(void) {
    RUN(wav_header_fields);
    RUN(wavrec_exact_length_and_samples);
    RUN(wavrec_format_change_and_stop);
    RUN(underrun_and_late_counters);
}
