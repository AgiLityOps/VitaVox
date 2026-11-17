#include "vsink.h"

#include <psp2/audioout.h>
#include <string.h>

#include "dbg.h"

#define GRAIN 1024

static struct {
    int port;
    int ch;
    int fill;
    int logged_out;
    int16_t buf[GRAIN * 2];
} S = {-1, 0, 0, 0, {0}};

static int s_open(void *ud, int rate, int ch) {
    (void)ud;
    if (S.port >= 0) { sceAudioOutReleasePort(S.port); S.port = -1; }
    S.fill = 0;
    S.logged_out = 0;
    S.ch = ch;
    int mode = ch == 1 ? SCE_AUDIO_OUT_MODE_MONO : SCE_AUDIO_OUT_MODE_STEREO;
    S.port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, GRAIN, rate, mode);
    if (S.port < 0) {
        dbg("audio: sceAudioOutOpenPort BGM %d Hz x%d grain=%d failed %08x",
            rate, ch, GRAIN, (unsigned)S.port);
        return -1;
    }
    int vol[2] = {SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB};
    int vrc = sceAudioOutSetVolume(S.port, SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH, vol);
    dbg("audio: port=%d BGM %d Hz x%d grain=%d vol_rc=%d", S.port, rate, ch, GRAIN, vrc);
    return 0;
}

/* Collects frames into whole grains; each full grain blocks until the hardware takes it. */
static int s_write(void *ud, const int16_t *pcm, int frames) {
    (void)ud;
    if (S.port < 0) return -1;
    int done = 0;
    while (done < frames) {
        int k = GRAIN - S.fill;
        if (k > frames - done) k = frames - done;
        memcpy(S.buf + S.fill * S.ch, pcm + done * S.ch, (size_t)k * S.ch * sizeof(int16_t));
        S.fill += k;
        done += k;
        if (S.fill == GRAIN) {
            int orc = sceAudioOutOutput(S.port, S.buf);
            if (!S.logged_out) {
                dbg("audio: first output rc=%d frames=%d", orc, GRAIN);
                S.logged_out = 1;
            } else if (orc < 0) {
                dbg("audio: sceAudioOutOutput failed %08x", (unsigned)orc);
            }
            S.fill = 0;
        }
    }
    return frames;
}

/* Pads a partial grain with silence and queues it (a short tail must not be lost). */
static void flush_partial(void) {
    if (S.port < 0 || S.fill == 0) return;
    memset(S.buf + S.fill * S.ch, 0, (size_t)(GRAIN - S.fill) * S.ch * sizeof(int16_t));
    int orc = sceAudioOutOutput(S.port, S.buf);
    if (orc < 0) dbg("audio: sceAudioOutOutput (tail) failed %08x", (unsigned)orc);
    S.fill = 0;
}

static void s_drain(void *ud) {
    (void)ud;
    flush_partial();
}

static void s_discard(void *ud) {
    (void)ud;
    S.fill = 0;
}

/* Rate/channel change at a track boundary: play out what is queued, then change
 * the same port in place (no second port, no release/reopen gap). Falls back to
 * release + open if the firmware refuses. */
static int s_reconfig(void *ud, int rate, int ch) {
    if (S.port < 0) return s_open(ud, rate, ch);
    flush_partial();
    sceAudioOutOutput(S.port, NULL); /* wait until the last grain has played */
    int mode = ch == 1 ? SCE_AUDIO_OUT_MODE_MONO : SCE_AUDIO_OUT_MODE_STEREO;
    int rc = sceAudioOutSetConfig(S.port, GRAIN, rate, mode);
    if (rc < 0) {
        dbg("audio: sceAudioOutSetConfig(%d Hz x%d) failed %08x, reopening the port", rate, ch, (unsigned)rc);
        return s_open(ud, rate, ch);
    }
    S.ch = ch;
    S.fill = 0;
    dbg("audio: port=%d reconfigured to %d Hz x%d", S.port, rate, ch);
    return 0;
}

static void s_close(void *ud) {
    (void)ud;
    if (S.port < 0) return;
    flush_partial();
    sceAudioOutOutput(S.port, NULL); /* wait for the last grain */
    sceAudioOutReleasePort(S.port);
    S.port = -1;
    S.fill = 0;
}

const AudioSink vita_sink = {s_open, s_write, s_close, NULL, s_reconfig, s_discard, s_drain};
