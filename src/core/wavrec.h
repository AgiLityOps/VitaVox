/* Debug recorder: the exact PCM sent to the audio device, to a WAV file.
 * The audio thread only copies into a RAM ring (never blocks, never touches
 * the file); a lowest-priority thread writes the ring to disk in chunks. */
#ifndef VITAVOX_WAVREC_H
#define VITAVOX_WAVREC_H
#include <stddef.h>
#include <stdint.h>

/* 44-byte canonical PCM s16 WAV header. */
void wav_header(uint8_t h[44], int rate, int channels, uint32_t data_bytes);

/* Folder for recordings (created on start). */
void wavrec_set_dir(const char *dir);
/* Starts a recording of up to seconds of output. The file is
 * <dir>/out_<YYYYMMDD_HHMMSS>.wav; its path goes to out. 0 ok, -1 busy/failed. */
int wavrec_start(int seconds, char *out, size_t n);
/* Ends a recording early (the file is finished by the writer thread). */
void wavrec_stop(void);
/* 1 while recording or still writing. */
int wavrec_active(void);
/* Seconds recorded so far (of the current or last recording). */
double wavrec_seconds(void);
/* Audio thread: frames of interleaved s16 at rate/channels. Non-blocking; a
 * format change ends the recording. */
void wavrec_feed(const int16_t *pcm, int frames, int rate, int channels);
/* Waits up to ms for the writer to finish (tests, shutdown). 0 = finished. */
int wavrec_wait(int ms);
/* Frames dropped because the writer fell behind (current/last recording). */
uint64_t wavrec_dropped(void);

#endif
