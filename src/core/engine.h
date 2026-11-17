/* Audio engine: a high-priority decode thread that decodes the current book and
 * applies speed (sonic) into a ~1 s decode-ahead ring, and a higher-priority
 * output thread that applies volume and writes the ring to a blocking sink at
 * the book's own sample rate. The UI talks to
 * it with commands and reads back state and position. */
#ifndef VITAVOX_ENGINE_H
#define VITAVOX_ENGINE_H
#include <stdbool.h>
#include <stdint.h>

#include "models.h"
#include "stream.h"

typedef enum { ENG_IDLE, ENG_LOADING, ENG_PLAYING, ENG_PAUSED, ENG_BUFFERING, ENG_ENDED, ENG_ERROR } EngineState;

/* Where PCM goes. write blocks until the device accepts the frames (that is the
 * engine's clock). open is called again whenever the format changes. */
typedef struct {
    int (*open)(void *ud, int rate, int channels);
    int (*write)(void *ud, const int16_t *pcm, int frames);
    void (*close)(void *ud);
    void *ud;
    /* Optional (NULL = not supported):
     * reconfig: change rate/channels of the open device in place, after playing
     *   out anything pending (used at a track change to a different rate);
     *   without it the engine closes and reopens.
     * discard: drop frames buffered but not yet given to the device (flush).
     * drain: give any buffered partial block to the device (end of book). */
    int (*reconfig)(void *ud, int rate, int channels);
    void (*discard)(void *ud);
    void (*drain)(void *ud);
} AudioSink;

typedef struct {
    int ntracks;
    Track *tracks;          /* copied by engine_load */
    TrackSource *sources;   /* one per track, copied */
    double duration;
} PlayableBook;

int engine_init(const AudioSink *sink);
void engine_exit(void);

void engine_load(const PlayableBook *b, double start_t, bool autoplay);
/* 00.36: new track lengths for the loaded book (same tracks), e.g. once the
 * estimated lengths of a big book are known exactly. Playback carries on; the
 * clock, queued audio and position move to the new timeline. */
void engine_retime(const Track *tracks, int ntracks, double duration);
void engine_stop(void);
void engine_play(void);
void engine_pause(void);
void engine_seek(double t);
void engine_set_speed(float speed);
void engine_set_volume(float v);  /* 0..1, applied to the samples */

EngineState engine_state(void);
bool engine_is_playing(void);     /* playing or buffering with the intent to play */
double engine_position(void);     /* book seconds */
double engine_duration(void);
float engine_cpu_load(void);      /* share of wall time spent decoding, 0..1 */
int engine_error(void);           /* STREAM_* code for ENG_ERROR */
float engine_speed(void);
/* True once the sink accepted PCM for the current load (first audio output). */
bool engine_audio_started(void);
/* Diagnostics: track switches served by the background preopen. */
int engine_preopen_hits(void);
/* Diagnostics: times a track was reopened after a mid-track stream error or stall. */
int engine_recoveries(void);
/* Diagnostics: output waits that ran the device dry / device writes that came back late. */
int engine_underruns(void);
int engine_late_writes(void);
int engine_ring_slots(void);

#endif
