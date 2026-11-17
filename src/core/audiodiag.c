#include "audiodiag.h"

#include <string.h>

void adiag_reset(AudioDiag *d) { memset(d, 0, sizeof *d); }

double adiag_ms(int frames, int rate) {
    return rate > 0 && frames > 0 ? (double)frames * 1000.0 / rate : 0.0;
}

int adiag_wait(AudioDiag *d, uint64_t waited_ms, int queued_frames, int rate) {
    d->waits++;
    if (waited_ms > d->max_wait_ms) d->max_wait_ms = waited_ms;
    /* The device still had queued_frames to play when we started waiting: a
     * wait shorter than that is inaudible. */
    if ((double)waited_ms > adiag_ms(queued_frames, rate) + 2.0) {
        d->underruns++;
        return 1;
    }
    return 0;
}

int adiag_write(AudioDiag *d, uint64_t interval_ms, int frames, int rate) {
    if (frames > 0 && rate > 0 && (double)interval_ms > 2.0 * adiag_ms(frames, rate) + 10.0) {
        d->late_writes++;
        return 1;
    }
    return 0;
}

int adiag_may_log(AudioDiag *d, int kind, uint64_t now_ms, uint32_t *skipped) {
    if (kind < 0 || kind > 1) return 0;
    if (d->last_log_ms[kind] && now_ms - d->last_log_ms[kind] < ADIAG_LOG_INTERVAL_MS) {
        d->suppressed[kind]++;
        return 0;
    }
    if (skipped) *skipped = d->suppressed[kind];
    d->suppressed[kind] = 0;
    d->last_log_ms[kind] = now_ms ? now_ms : 1;
    return 1;
}

uint64_t adiag_count_clip(AudioDiag *d, const int16_t *pcm, int n) {
    uint64_t c = 0;
    for (int i = 0; i < n; i++)
        if (pcm[i] >= 32767 || pcm[i] <= -32768) c++;
    if (d) d->clipped += c;
    return c;
}
