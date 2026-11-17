/* Audio output diagnostics (pure C, host-tested): underruns, late device
 * writes, clipped samples, and rate-limited logging for them. */
#ifndef VITAVOX_AUDIODIAG_H
#define VITAVOX_AUDIODIAG_H
#include <stdint.h>

#define ADIAG_LOG_INTERVAL_MS 1000 /* at most one detail line per second per kind */

typedef struct {
    uint32_t underruns;      /* output waited for decoded data longer than the device had queued */
    uint32_t late_writes;    /* a device write returned much later than its audio length */
    uint64_t clipped;        /* s16 samples at +32767 / -32768 */
    uint64_t frames;         /* frames written to the device (kept by the caller) */
    uint32_t waits;          /* any wait for data while playing (incl. harmless short ones) */
    uint64_t max_wait_ms;
    uint64_t last_log_ms[2]; /* per kind: 0 underrun, 1 late */
    uint32_t suppressed[2];  /* detail lines skipped by the rate limit since the last one */
} AudioDiag;

enum { ADIAG_UNDERRUN = 0, ADIAG_LATE = 1 };

void adiag_reset(AudioDiag *d);
/* Milliseconds of audio in frames at rate (rate <= 0 -> 0). */
double adiag_ms(int frames, int rate);
/* The output thread waited waited_ms for data with queued_frames still queued in
 * the device. Returns 1 if that was an underrun (the device ran dry). */
int adiag_wait(AudioDiag *d, uint64_t waited_ms, int queued_frames, int rate);
/* A write of frames returned interval_ms after the previous one returned (no
 * wait for data in between). Returns 1 if it was late: more than twice its
 * audio length plus 10 ms. */
int adiag_write(AudioDiag *d, uint64_t interval_ms, int frames, int rate);
/* Rate limit for detail lines of kind. Returns 1 if a line may be logged now
 * (and *skipped gets how many were suppressed since the last one), else 0. */
int adiag_may_log(AudioDiag *d, int kind, uint64_t now_ms, uint32_t *skipped);
/* Counts clipped samples (s16 at the rails) in n interleaved samples, adds them. */
uint64_t adiag_count_clip(AudioDiag *d, const int16_t *pcm, int n);

#endif
