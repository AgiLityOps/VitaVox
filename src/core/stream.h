/* One local audio file: FFmpeg demux + decode + swresample -> s16 stereo PCM. */
#ifndef VITAVOX_STREAM_H
#define VITAVOX_STREAM_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "models.h"

typedef struct {
    char path[512];
    uint64_t size;
    char ext[8];
} TrackSource;

/* STREAM_EOF is 0 so it can never be mistaken for a frame count */
enum { STREAM_EOF = 0, STREAM_ERR = -1, STREAM_IO = -2, STREAM_INTERRUPTED = -3, STREAM_UNSUPPORTED = -4 };

#define STREAM_MAX_FRAMES 4096

typedef struct Stream Stream;

Stream *stream_open(const TrackSource *src, int *err);
/* Seeks to local time t seconds; *actual gets the real position. 0 ok. */
int stream_seek(Stream *s, double t, double *actual);
/* Decodes interleaved s16 stereo. Returns frames (>0), STREAM_EOF (0), or STREAM_* (<0). */
int stream_read(Stream *s, int16_t *out, int max_frames, int *channels, int *rate);
void stream_interrupt(Stream *s, int on);
/* 1 if the stream stopped because a device read/seek stayed blocked past the
 * stall limit (not a user cancel). The engine reopens such a track once. */
int stream_stalled(const Stream *s);
/* Diagnostics: device operations that were slow (>= stall limit) but completed. */
int stream_slow_io(const Stream *s);
/* Power event (display off/on, suspend, resume): device operations that span
 * one are not judged by wall time. Safe from any thread. */
void stream_note_power_event(void);
/* Tests: replace the clock used by the open deadline and stall watchdog (NULL = real). */
void stream_test_set_clock(uint64_t (*fn)(void));
int stream_net_state(Stream *s); /* always 0 (local) */
void stream_close(Stream *s);
bool stream_supported(const char *ext, const char *codec);
void stream_table_stats(Stream *s, int *requests, uint64_t *bytes);
/* Play-path I/O: deadline_armed must be 0 after a successful stream_open. */
void stream_io_info(const Stream *s, uint64_t *bytes_read, int *seeks, int *deadline_armed);

/* Output rate for a source rate: the source rate when a Vita BGM port supports
 * it (8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000), else the
 * nearest sensible supported rate. */
int stream_pick_out_rate(int src);
int stream_port_rate_supported(int rate);

/* Process-wide cancel for in-flight probes/opens (checked by io_interrupt). */
void stream_request_cancel(int on);
/* 0..1 during stream_open / probe, plus a short status. */
void stream_open_progress(float *frac, char *msg, size_t n);

/* Probe metadata without opening a playback stream. Cover bytes are malloc'd. */
typedef struct {
    char title[160];
    char artist[96];
    char album[96];
    double duration;
    int nchapters;
    Chapter *chapters;
    uint8_t *cover;
    size_t cover_len;
    int has_cover; /* attached art exists; cover bytes may be omitted in a quick probe */
    char *description; /* 00.38: comment tag, else description/synopsis; cleaned, malloc'd, may be NULL */
} MediaInfo;

#define STREAM_PROBE_TIMEOUT_MS 15000
#define STREAM_PROBE_QUICK 1 /* skip find_stream_info when codecpar is ready; still copy attached_pic */

int stream_probe(const char *path, MediaInfo *out);
int stream_probe_ex(const char *path, MediaInfo *out, int flags, uint64_t timeout_ms);
void media_info_free(MediaInfo *m);

/* Diagnostics: total avformat_find_stream_info calls (play path should add none). */
int stream_find_info_calls(void);
/* Lazy MP4/M4B reader on (default) or off (FFmpeg mov path), for tests/diagnostics. */
void stream_set_mp4_lazy(int on);
/* Diagnostics: 1 if this stream is demuxed by the lazy MP4 reader (no FFmpeg mov index). */
int stream_uses_mp4lazy(const Stream *s);

/* Parse a FLAC STREAMINFO body (34 bytes, or "fLaC"+header+body). 0 ok. */
int stream_parse_flac_streaminfo(const uint8_t *p, int len, int *rate, int *channels,
                                 int *bits, int64_t *samples);

#endif
