#include "engine.h"

#include <stdlib.h>
#include <string.h>

#include "audiodiag.h"
#include "dbg.h"
#include "osal.h"
#include "sonic.h"
#include "timeline.h"
#include "quit.h"
#include "stream.h"
#include "wavrec.h"

#define OUT_FRAMES 1024 /* per chunk: ~21 ms at 48 kHz, 32 ms at 32 kHz */
/* Decode-ahead (v00.31): the decode thread fills a ring of chunks that a
 * separate, higher-priority output thread feeds to the device. 48 chunks are
 * ~1.0 s at 48 kHz (~1.5 s at 32 kHz), enough to ride out a slow SD read,
 * a cover load or a library save. Seek/load/stop/speed flush it, and pause
 * stops the output thread within one chunk, so neither waits for the ring. */
#define RING_SLOTS 48

typedef struct {
    int16_t pcm[OUT_FRAMES * 2];
    int frames, rate, ch, track;
    double t0, step;     /* book time of the first frame, book seconds per frame */
    unsigned gen, load;
} Slot;

typedef enum { C_LOAD, C_STOP, C_SEEK, C_SPEED, C_RETIME } CmdType;
typedef struct { CmdType type; double arg; bool autoplay; } Cmd;

static struct {
    Thread th;
    volatile bool quit;
    Mutex lock;
    Cmd q[16];
    int qn;
    PlayableBook pending_book;
    bool has_pending_book;
    Track *retime_tracks;    /* engine_retime: new lengths waiting for the engine thread */
    int retime_n;
    double retime_dur;
    AudioSink sink;

    /* published state */
    volatile EngineState state;
    volatile double pos;
    volatile float cpu;
    volatile int error;
    volatile bool want_play;
    volatile float speed, volume;

    /* engine-thread private */
    PlayableBook book;
    int cur;
    Stream *st;
    int rate, ch;        /* sink format (0 = not open) */
    sonicStream son;
    float son_speed;
    double anchor_t;     /* book time of the first output frame since the anchor */
    double out_frames;   /* output frames since the anchor */
    bool end_of_book, flushed;
    int16_t dec[STREAM_MAX_FRAMES * 2];
    int dec_frames, dec_pos, dec_ch, dec_rate;
    int16_t out[OUT_FRAMES * 2];
    uint64_t busy_ms, window_start;

    /* decode-ahead ring (lock: rlock) */
    Mutex rlock;
    Cond rcv;
    Slot ring[RING_SLOTS];
    int r_head, r_count;
    volatile unsigned gen;   /* bumped by every flush; older chunks are dropped */
    Slot pend;               /* decode thread: chunk waiting for room in the ring */
    bool has_pend;
    volatile bool end_wait;  /* decode thread reached the end of the book */
    volatile bool drained;   /* output thread pushed out the device's last partial grain */
    volatile bool o_busy;    /* output thread is writing a chunk */
    /* output thread */
    Thread out_th;
    volatile bool out_quit;
    int o_rate, o_ch;        /* format the device is open with (0 = closed) */
    AudioDiag diag;
    int ring_min;            /* lowest ring fill seen while playing since the last stats line */
    /* decoder position in the current stream, for resuming after a stream error */
    double st_local0;    /* local time the stream was opened/seeked at */
    int64_t st_frames;   /* frames decoded since then */
    int st_rate;
    int rec_track;       /* last track reopened after an error (-1 none) ... */
    double rec_local;    /* ... and where */
    int recoveries;      /* diagnostics */

    /* audio-start signal for the UI (full-size cover waits for it) */
    volatile unsigned load_seq, out_seq;
    unsigned active_load;    /* engine thread: load_seq of the book now decoding */

    /* next-track preopen (gapless): a background thread opens track cur+1
     * while the current one plays, so the switch at EOF is a pointer swap. */
    Thread pre_th;
    Mutex pre_lock;
    Cond pre_cv;
    volatile bool pre_quit;
    int pre_want;            /* track index requested, -1 none */
    unsigned pre_gen;        /* bumped on load/seek/stop: stale results are dropped */
    unsigned pre_want_gen;
    TrackSource pre_src;
    bool pre_busy;           /* open in flight */
    int pre_busy_idx;
    unsigned pre_busy_gen;
    Stream *pre;             /* ready stream for pre_idx */
    int pre_idx;
    unsigned pre_ready_gen;
    volatile int pre_hits;
} E;

static void set_state(EngineState s) { E.state = s; }

/* ---- next-track preopen -------------------------------------------------------- */

static void *preopen_thread(void *arg) {
    (void)arg;
    mutex_lock(&E.pre_lock);
    while (!E.pre_quit) {
        if (E.pre_want < 0) { cond_wait_ms(&E.pre_cv, &E.pre_lock, 500); continue; }
        int idx = E.pre_want;
        unsigned gen = E.pre_want_gen;
        TrackSource src = E.pre_src;
        E.pre_want = -1;
        E.pre_busy = true;
        E.pre_busy_idx = idx;
        E.pre_busy_gen = gen;
        mutex_unlock(&E.pre_lock);
        uint64_t t0 = now_ms();
        int err = 0;
        Stream *s = (E.pre_quit || app_quit_active()) ? NULL : stream_open(&src, &err);
        dbg("engine: preopen track %d %s in %llu ms (err=%d)", idx, s ? "ready" : "failed",
            (unsigned long long)(now_ms() - t0), err);
        mutex_lock(&E.pre_lock);
        E.pre_busy = false;
        if (s && gen == E.pre_gen && !E.pre_quit) {
            Stream *old = E.pre;
            E.pre = s;
            E.pre_idx = idx;
            E.pre_ready_gen = gen;
            s = old;
        }
        cond_broadcast(&E.pre_cv);
        mutex_unlock(&E.pre_lock);
        stream_close(s); /* stale or replaced */
        mutex_lock(&E.pre_lock);
    }
    mutex_unlock(&E.pre_lock);
    return NULL;
}

/* Drop any ready or pending preopen (book changed, seek, stop). */
static void preopen_reset(void) {
    mutex_lock(&E.pre_lock);
    E.pre_gen++;
    E.pre_want = -1;
    Stream *s = E.pre;
    E.pre = NULL;
    E.pre_idx = -1;
    mutex_unlock(&E.pre_lock);
    stream_close(s);
}

static void preopen_request(int idx) {
    if (idx < 0 || idx >= E.book.ntracks || !E.pre_th.ok) return;
    mutex_lock(&E.pre_lock);
    bool have = (E.pre && E.pre_idx == idx && E.pre_ready_gen == E.pre_gen)
             || (E.pre_want == idx && E.pre_want_gen == E.pre_gen);
    if (E.pre_busy && E.pre_busy_idx == idx && E.pre_busy_gen == E.pre_gen) have = true;
    if (!have) {
        E.pre_want = idx;
        E.pre_want_gen = E.pre_gen;
        E.pre_src = E.book.sources[idx];
        cond_broadcast(&E.pre_cv);
    }
    mutex_unlock(&E.pre_lock);
}

/* Takes the preopened stream for idx, waiting for an in-flight open of it. */
static Stream *preopen_take(int idx) {
    Stream *s = NULL;
    mutex_lock(&E.pre_lock);
    uint64_t t0 = now_ms();
    while (!E.quit && !E.pre_quit && !(E.pre && E.pre_idx == idx && E.pre_ready_gen == E.pre_gen)
           && ((E.pre_busy && E.pre_busy_idx == idx && E.pre_busy_gen == E.pre_gen)
               || (E.pre_want == idx && E.pre_want_gen == E.pre_gen))
           && now_ms() - t0 < 25000)
        cond_wait_ms(&E.pre_cv, &E.pre_lock, 50);
    if (E.pre && E.pre_idx == idx && E.pre_ready_gen == E.pre_gen) {
        s = E.pre;
        E.pre = NULL;
        E.pre_idx = -1;
    }
    mutex_unlock(&E.pre_lock);
    if (s) {
        E.pre_hits++;
        dbg("engine: using preopened track %d (waited %llu ms)", idx, (unsigned long long)(now_ms() - t0));
    }
    return s;
}

static void close_stream(void) {
    mutex_lock(&E.lock);
    Stream *s = E.st;
    E.st = NULL;
    mutex_unlock(&E.lock);
    stream_close(s);
}

static void reset_output(void) {
    if (E.son) { sonicDestroyStream(E.son); E.son = NULL; }
    E.son_speed = 1.0f;
    E.dec_frames = E.dec_pos = 0;
    E.end_of_book = E.flushed = false;
}

/* ---- decode-ahead ring ------------------------------------------------------ */

/* Drops everything queued for the device (seek, load, stop, speed change). Safe
 * from any thread. */
static void ring_flush(void) {
    mutex_lock(&E.rlock);
    E.gen++;
    E.r_count = 0;
    E.drained = false;
    cond_broadcast(&E.rcv);
    mutex_unlock(&E.rlock);
}

/* Decode thread: queue E.pend, waiting up to wait_ms for room. 1 done (queued,
 * or dropped because a flush made it stale), 0 still full. */
static int ring_push(int wait_ms) {
    mutex_lock(&E.rlock);
    if (E.pend.gen != E.gen) { mutex_unlock(&E.rlock); return 1; }
    if (E.r_count == RING_SLOTS && wait_ms > 0) cond_wait_ms(&E.rcv, &E.rlock, wait_ms);
    if (E.pend.gen != E.gen) { mutex_unlock(&E.rlock); return 1; }
    if (E.r_count == RING_SLOTS) { mutex_unlock(&E.rlock); return 0; }
    int i = (E.r_head + E.r_count) % RING_SLOTS;
    E.ring[i] = E.pend;
    E.r_count++;
    E.drained = false;
    cond_broadcast(&E.rcv);
    mutex_unlock(&E.rlock);
    return 1;
}

static int ring_fill(void) {
    mutex_lock(&E.rlock);
    int n = E.r_count;
    mutex_unlock(&E.rlock);
    return n;
}

/* Everything decoded so far has been handed to the device (end of book). */
static bool ring_played_out(void) {
    mutex_lock(&E.rlock);
    bool done = E.r_count == 0 && !E.o_busy && (E.drained || !E.sink.drain);
    mutex_unlock(&E.rlock);
    return done;
}

/* Opens track i at local time t and anchors the clock there. */
static int open_track(int i, double t) {
    close_stream();
    int err = 0;
    const TrackSource *src = &E.book.sources[i];
    if (E.quit || app_quit_active()) return STREAM_INTERRUPTED;
    Stream *s = NULL;
    if (t <= 0.05) s = preopen_take(i);
    if (!s) {
        dbg("engine: open track %d at %.1f (%s)", i, t, src->path);
        uint64_t t0 = now_ms();
        s = stream_open(src, &err);
        if (s) dbg("engine: track %d opened in %llu ms", i, (unsigned long long)(now_ms() - t0));
    }
    if (!s) {
        dbg("engine: track %d failed to open (%d)", i, err);
        return err ? err : STREAM_ERR;
    }
    double actual = t;
    if (t > 0.05 && stream_seek(s, t, &actual) != 0) actual = 0;
    mutex_lock(&E.lock);
    E.st = s;
    mutex_unlock(&E.lock);
    E.cur = i;
    E.anchor_t = E.book.tracks[i].start_offset + actual;
    E.out_frames = 0;
    E.end_of_book = false;
    E.st_local0 = actual;
    E.st_frames = 0;
    return 0;
}

/* A stream error or I/O stall in the middle of a track (v0.2.12). Never treated
 * as end of track: log it, reopen the same track at the decoder position once,
 * and only if that fails or the error repeats at the same place move on to the
 * next track. Returns like pull_decoded(). */
static int recover_stream(int err) {
    int cur = E.cur;
    bool stalled = E.st && stream_stalled(E.st);
    double local = E.st_local0 + (E.st_rate > 0 ? (double)E.st_frames / E.st_rate : 0.0);
    dbg("engine: track %d stream %s (%d) at %.2f s", cur, stalled ? "stalled" : "error", err, local);
    bool repeat = E.rec_track == cur && local <= E.rec_local + 1.0;
    if (!repeat) {
        E.rec_track = cur;
        E.rec_local = local;
        E.recoveries++;
        double t = local > 0.06 ? local : 0.06; /* > 0.05: never take a preopen for this */
        dbg("engine: reopening track %d at %.2f s", cur, t);
        int rc = open_track(cur, t);
        if (rc == 0) return 0;
        dbg("engine: reopen of track %d failed (%d)", cur, rc);
        if (rc == STREAM_INTERRUPTED && (E.quit || app_quit_active())) return 0;
    } else {
        dbg("engine: track %d failed again at %.2f s after a reopen", cur, local);
    }
    if (cur + 1 < E.book.ntracks) {
        dbg("engine: skipping the rest of track %d, continuing with track %d", cur, cur + 1);
        int rc = open_track(cur + 1, 0);
        return rc ? rc : 0;
    }
    return err < 0 ? err : STREAM_ERR;
}

int engine_recoveries(void) { return E.recoveries; }

static void reanchor(void) {
    if (E.rate) E.anchor_t += E.out_frames * E.son_speed / E.rate;
    E.out_frames = 0;
}

static void ensure_sonic(void) {
    float sp = E.speed;
    if (sp > 0.99f && sp < 1.01f) {
        if (E.son) {
            reanchor();
            sonicDestroyStream(E.son);
            E.son = NULL;
        }
        E.son_speed = 1.0f;
        return;
    }
    if (!E.son || E.son_speed != sp) {
        reanchor();
        if (!E.son) E.son = sonicCreateStream(E.rate, E.ch);
        if (E.son) sonicSetSpeed(E.son, sp);
        E.son_speed = E.son ? sp : 1.0f;
    }
}

/* More decoded PCM into E.dec. 1 ok, 0 nothing now (new track / wait / end), <0 error. */
static int pull_decoded(void) {
    while (E.st) {
        int ch, rate;
        int n = stream_read(E.st, E.dec, STREAM_MAX_FRAMES, &ch, &rate);
        if (n > 0) {
            E.st_frames += n;
            E.st_rate = rate;
            E.dec_frames = n;
            E.dec_pos = 0;
            E.dec_ch = ch;
            E.dec_rate = rate;
            return 1;
        }
        if (n == STREAM_EOF) {
            if (E.cur + 1 < E.book.ntracks) {
                int rc = open_track(E.cur + 1, 0);
                return rc ? rc : 0;
            }
            dbg("engine: end of book after track %d", E.cur);
            E.end_of_book = true;
            return 0;
        }
        /* Seek/cancel is temporary — do not treat it as EOF or a hard error. */
        if (n == STREAM_INTERRUPTED && !stream_stalled(E.st)) return 0;
        dbg("engine: stream_read error %d", n);
        return recover_stream(n);
    }
    return 0;
}

/* Fills E.out. Returns frames (0 = nothing available now), <0 on error.
 * *t0 gets the book time of the first frame. */
static int produce(double *t0) {
    int frames = 0;
    *t0 = E.anchor_t + (E.rate ? E.out_frames * E.son_speed / E.rate : 0);
    while (frames < OUT_FRAMES) {
        if (E.son && E.rate) {
            int k = sonicReadShortFromStream(E.son, E.out + frames * E.ch, OUT_FRAMES - frames);
            if (k > 0) { frames += k; continue; }
        }
        if (E.dec_pos < E.dec_frames) {
            if (E.dec_ch != E.ch || E.dec_rate != E.rate) {
                if (frames > 0) break; /* finish this chunk in the old format first */
                /* The output thread reconfigures the device when it reaches
                 * the first chunk in the new format (gapless track change). */
                if (E.rate) dbg("engine: output format %d Hz x%d -> %d Hz x%d at track %d",
                                E.rate, E.ch, E.dec_rate, E.dec_ch, E.cur);
                E.rate = E.dec_rate;
                E.ch = E.dec_ch;
                if (E.rate <= 0 || E.ch <= 0) return STREAM_ERR;
                if (E.son) { sonicDestroyStream(E.son); E.son = NULL; }
                E.son_speed = 1.0f;
                *t0 = E.anchor_t + E.out_frames / E.rate;
            }
            ensure_sonic();
            int n = E.dec_frames - E.dec_pos;
            if (E.son) {
                sonicWriteShortToStream(E.son, E.dec + E.dec_pos * E.ch, n);
                E.dec_pos = E.dec_frames;
            } else {
                if (n > OUT_FRAMES - frames) n = OUT_FRAMES - frames;
                memcpy(E.out + frames * E.ch, E.dec + E.dec_pos * E.ch, (size_t)n * E.ch * 2);
                E.dec_pos += n;
                frames += n;
            }
            continue;
        }
        if (E.end_of_book) {
            if (E.son && !E.flushed) { sonicFlushStream(E.son); E.flushed = true; continue; }
            break;
        }
        int cur_before = E.cur;
        int rc = pull_decoded();
        if (rc < 0) return rc;
        if (rc == 0) {
            if (E.cur != cur_before && frames == 0) { /* fresh track, fresh anchor */
                *t0 = E.anchor_t;
                continue;
            }
            break;
        }
    }
    return frames;
}

/* Output thread: volume is applied as the chunk goes to the device, so a
 * change is heard at once rather than after the decode-ahead. */
static void apply_volume(int16_t *pcm, int frames, int ch) {
    float v = E.volume;
    if (v >= 0.999f) return;
    int n = frames * ch;
    int g = (int)(v * 32768.0f);
    for (int i = 0; i < n; i++) pcm[i] = (int16_t)((pcm[i] * g) >> 15);
}

/* ---- commands ------------------------------------------------------------------ */

static void do_load(PlayableBook *b, double t, bool autoplay) {
    set_state(ENG_LOADING);
    E.active_load = E.load_seq;
    ring_flush();
    E.has_pend = false;
    E.end_wait = false;
    preopen_reset();
    close_stream();
    reset_output();
    free(E.book.tracks);
    free(E.book.sources);
    E.book = *b;
    E.want_play = autoplay;
    E.pos = t;
    E.error = 0;
    E.rec_track = -1;
    double local;
    int i = tl_locate(E.book.tracks, E.book.ntracks, t, &local);
    int rc = i < 0 ? STREAM_ERR : open_track(i, local);
    if (rc) { E.error = rc; set_state(ENG_ERROR); return; }
    E.pos = E.anchor_t;
    set_state(autoplay ? ENG_BUFFERING : ENG_PAUSED);
}

static void do_seek(double t) {
    if (!E.book.ntracks) return;
    if (t > E.book.duration - 0.5) t = E.book.duration - 0.5;
    if (t < 0) t = 0;
    ring_flush();
    E.has_pend = false;
    E.end_wait = false;
    reset_output();
    double local;
    int i = tl_locate(E.book.tracks, E.book.ntracks, t, &local);
    int rc;
    /* Keep a preopen of the track after the target; drop anything else. */
    mutex_lock(&E.pre_lock);
    bool keep = (E.pre && E.pre_idx == i + 1 && E.pre_ready_gen == E.pre_gen)
             || (E.pre_busy && E.pre_busy_idx == i + 1 && E.pre_busy_gen == E.pre_gen);
    mutex_unlock(&E.pre_lock);
    if (!keep) preopen_reset();
    if (i == E.cur && E.st) {
        double actual = local;
        rc = stream_seek(E.st, local, &actual);
        E.anchor_t = E.book.tracks[i].start_offset + actual;
        E.out_frames = 0;
        E.st_local0 = actual;
        E.st_frames = 0;
    } else {
        rc = open_track(i, local);
    }
    if (rc) { E.error = rc; set_state(ENG_ERROR); return; }
    dbg("engine: seek %.2f -> track %d at %.2f", t, i, E.anchor_t);
    E.pos = E.anchor_t;
    set_state(E.want_play ? ENG_BUFFERING : ENG_PAUSED);
}

/* 00.36: the book's track lengths changed (estimates of a big book refined
 * while it plays). Moves the clock, the queued chunks and the position to the
 * new timeline without touching the stream: no gap, no reopen. */
static void do_retime(void) {
    mutex_lock(&E.lock);
    Track *nt = E.retime_tracks;
    int n = E.retime_n;
    double dur = E.retime_dur;
    E.retime_tracks = NULL;
    E.retime_n = 0;
    mutex_unlock(&E.lock);
    if (!nt) return;
    if (n != E.book.ntracks || !E.book.tracks || n <= 0) {
        dbg("engine: retime ignored (%d tracks for a %d-track book)", n, E.book.ntracks);
        free(nt);
        return;
    }
    double *d = malloc(sizeof(double) * (size_t)n);
    if (!d) { free(nt); return; }
    for (int i = 0; i < n; i++) {
        d[i] = nt[i].start_offset - E.book.tracks[i].start_offset;
        E.book.tracks[i].start_offset = nt[i].start_offset;
        E.book.tracks[i].duration = nt[i].duration;
    }
    E.book.duration = dur > 0 ? dur : tl_duration(E.book.tracks, n);
    int cur = E.cur >= 0 && E.cur < n ? E.cur : -1;
    if (cur >= 0) E.anchor_t += d[cur];
    mutex_lock(&E.rlock);
    for (int k = 0; k < E.r_count; k++) {
        Slot *sl = &E.ring[(E.r_head + k) % RING_SLOTS];
        if (sl->track >= 0 && sl->track < n) sl->t0 += d[sl->track];
    }
    if (E.has_pend && E.pend.track >= 0 && E.pend.track < n) E.pend.t0 += d[E.pend.track];
    int pt = E.r_count > 0 ? E.ring[E.r_head].track : cur;
    if (pt >= 0 && pt < n) E.pos += d[pt];
    mutex_unlock(&E.rlock);
    dbg("engine: retimed %d tracks: %.1f s total, playing track %d moved %+.2f s", n, E.book.duration, cur,
        cur >= 0 ? d[cur] : 0.0);
    free(d);
    free(nt);
}

static void process_commands(void) {
    for (;;) {
        mutex_lock(&E.lock);
        if (!E.qn) { mutex_unlock(&E.lock); return; }
        Cmd c = E.q[0];
        memmove(E.q, E.q + 1, sizeof(Cmd) * (--E.qn));
        PlayableBook b = E.pending_book;
        bool hb = E.has_pending_book;
        if (c.type == C_LOAD) E.has_pending_book = false;
        if (E.st && !E.qn) stream_interrupt(E.st, 0);
        mutex_unlock(&E.lock);
        switch (c.type) {
        case C_LOAD: if (hb) do_load(&b, c.arg, c.autoplay); break;
        case C_STOP:
            ring_flush();
            E.has_pend = false;
            E.end_wait = false;
            preopen_reset();
            close_stream();
            reset_output();
            set_state(ENG_IDLE);
            break;
        case C_SEEK: do_seek(c.arg); break;
        case C_RETIME: do_retime(); break;
        case C_SPEED:
            /* Speed is applied by sonic while decoding: restart decoding from
             * what is being heard so the decode-ahead is not at the old speed. */
            if (E.st && (E.has_pend || ring_fill() > 0)
                && (E.state == ENG_PLAYING || E.state == ENG_BUFFERING || E.state == ENG_PAUSED)) {
                double at = E.pos;
                dbg("engine: speed %.2fx, restarting decode at %.2f", (double)E.speed, at);
                do_seek(at);
            }
            break;
        }
    }
}

/* ---- thread ------------------------------------------------------------------- */

static void *engine_thread(void *arg) {
    (void)arg;
    thread_pin_audio("decode");
    E.window_start = now_ms();
    while (!E.quit) {
        process_commands();
        EngineState s = E.state;
        bool active = (s == ENG_PLAYING || s == ENG_BUFFERING) && E.want_play;
        if (s == ENG_PAUSED && E.want_play) { set_state(ENG_BUFFERING); active = true; }
        if (!active) {
            if (s == ENG_PLAYING || s == ENG_BUFFERING) set_state(ENG_PAUSED);
            sleep_ms(10);
        } else if (E.has_pend) {
            /* Ring full: the output thread is far enough ahead; wait for room
             * (in short steps so commands stay responsive). */
            if (ring_push(10)) E.has_pend = false;
        } else {
            uint64_t t_start = now_ms();
            double t0;
            unsigned gen = E.gen; /* before decoding: a flush meanwhile makes this chunk stale */
            int frames = produce(&t0);
            E.busy_ms += now_ms() - t_start;
            if (frames < 0) {
                dbg("engine: decode error %d", frames);
                E.error = frames;
                set_state(ENG_ERROR);
            } else if (frames == 0) {
                if (E.end_of_book) {
                    E.end_wait = true;
                    if (ring_played_out()) {
                        dbg("engine: playback ended at %.1f / %.1f", E.pos, E.book.duration);
                        E.pos = E.book.duration > 0 ? E.book.duration : E.pos;
                        E.want_play = false;
                        E.end_wait = false;
                        set_state(ENG_ENDED);
                    } else {
                        sleep_ms(5); /* the output thread is still playing the tail */
                    }
                } else {
                    set_state(ENG_BUFFERING);
                    sleep_ms(10);
                }
            } else {
                Slot *p = &E.pend;
                memcpy(p->pcm, E.out, (size_t)frames * E.ch * sizeof(int16_t));
                p->frames = frames;
                p->rate = E.rate;
                p->ch = E.ch;
                p->track = E.cur;
                p->t0 = t0;
                p->step = (double)E.son_speed / E.rate;
                p->gen = gen;
                p->load = E.active_load;
                E.out_frames += frames;
                E.has_pend = true;
                if (ring_push(0)) E.has_pend = false;
                if (E.state != ENG_PLAYING && E.qn == 0) set_state(ENG_PLAYING);
                /* 2 s into a track, open the next one in the background. */
                if (E.cur + 1 < E.book.ntracks && E.out_frames > 2.0 * E.rate)
                    preopen_request(E.cur + 1);
            }
        }
        uint64_t now = now_ms();
        if (now - E.window_start >= 1000) {
            E.cpu = (float)E.busy_ms / (float)(now - E.window_start);
            E.busy_ms = 0;
            E.window_start = now;
        }
    }
    close_stream();
    reset_output();
    return NULL;
}

/* ---- output thread ------------------------------------------------------------- */

static void out_stats(uint64_t now, uint64_t *last) {
    if (now - *last < 5000) return;
    *last = now;
    AudioDiag *d = &E.diag;
    dbg("audio: stats rate=%d ch=%d ring=%d/%d (min %d) underruns=%u late_writes=%u waits=%u max_wait=%llu ms "
        "clipped=%llu frames=%llu%s",
        E.o_rate, E.o_ch, ring_fill(), RING_SLOTS, E.ring_min < 0 ? 0 : E.ring_min, d->underruns, d->late_writes,
        d->waits, (unsigned long long)d->max_wait_ms, (unsigned long long)d->clipped,
        (unsigned long long)d->frames, wavrec_active() ? " (recording)" : "");
    E.ring_min = -1;
}

/* Puts the device into the chunk's format: opened the first time, then
 * reconfigured in place (a pending partial grain is padded out first), or as a
 * fallback closed and reopened. Never leaves a second port open. */
static int out_format(const Slot *sl) {
    if (E.o_rate == sl->rate && E.o_ch == sl->ch) return 0;
    int rc;
    if (!E.o_rate) {
        rc = E.sink.open(E.sink.ud, sl->rate, sl->ch);
        dbg("audio: output open %d Hz x%d for track %d (%s)", sl->rate, sl->ch, sl->track, rc == 0 ? "ok" : "failed");
    } else if (E.sink.reconfig) {
        rc = E.sink.reconfig(E.sink.ud, sl->rate, sl->ch);
        dbg("audio: output %d Hz x%d -> %d Hz x%d at track %d (reconfigured, rc=%d)",
            E.o_rate, E.o_ch, sl->rate, sl->ch, sl->track, rc);
    } else {
        E.sink.close(E.sink.ud);
        rc = E.sink.open(E.sink.ud, sl->rate, sl->ch);
        dbg("audio: output %d Hz x%d -> %d Hz x%d at track %d (reopened, rc=%d)",
            E.o_rate, E.o_ch, sl->rate, sl->ch, sl->track, rc);
    }
    if (rc == 0) { E.o_rate = sl->rate; E.o_ch = sl->ch; }
    else { E.o_rate = E.o_ch = 0; }
    return rc;
}

static void *out_thread(void *arg) {
    (void)arg;
    thread_pin_audio("audio-out");
    static Slot sl; /* one chunk being written (output thread only) */
    unsigned o_gen = E.gen;
    bool primed = false;          /* a chunk went out since the last flush/pause: waits now matter */
    uint64_t wait_since = 0, last_ret = 0, last_stats = now_ms();
    int last_frames = 0;
    E.ring_min = -1;
    while (!E.out_quit) {
        mutex_lock(&E.rlock);
        if (E.gen != o_gen) {
            o_gen = E.gen;
            primed = false;
            wait_since = 0;
            mutex_unlock(&E.rlock);
            if (E.sink.discard && E.o_rate) E.sink.discard(E.sink.ud); /* stale partial grain */
            continue;
        }
        bool play = E.want_play;
        if (!play || E.r_count == 0) {
            if (!play) { primed = false; wait_since = 0; }
            else if (E.r_count == 0) {
                if (E.end_wait) {
                    /* End of book: push out the last partial grain once. */
                    if (!E.drained && E.sink.drain && E.o_rate) {
                        mutex_unlock(&E.rlock);
                        E.sink.drain(E.sink.ud);
                        mutex_lock(&E.rlock);
                        if (E.gen == o_gen) E.drained = true;
                    }
                    primed = false;
                    wait_since = 0;
                } else if (primed && !wait_since) {
                    wait_since = now_ms();
                }
            }
            cond_wait_ms(&E.rcv, &E.rlock, play ? 5 : 20);
            mutex_unlock(&E.rlock);
            out_stats(now_ms(), &last_stats);
            continue;
        }
        sl = E.ring[E.r_head];
        E.r_head = (E.r_head + 1) % RING_SLOTS;
        E.r_count--;
        int fill = E.r_count;
        E.o_busy = true;
        cond_broadcast(&E.rcv);
        mutex_unlock(&E.rlock);
        if (primed && (E.ring_min < 0 || fill < E.ring_min)) E.ring_min = fill;

        uint64_t now = now_ms();
        bool waited = false;
        if (wait_since) {
            uint64_t w = now - wait_since;
            waited = true;
            wait_since = 0;
            if (adiag_wait(&E.diag, w, last_frames, E.o_rate)) {
                uint32_t skipped = 0;
                if (adiag_may_log(&E.diag, ADIAG_UNDERRUN, now, &skipped))
                    dbg("audio: underrun: waited %llu ms for decoded data (device had ~%.0f ms queued); track %d at %.2f s, "
                        "ring %d/%d after refill; %u underruns total%s",
                        (unsigned long long)w, adiag_ms(last_frames, E.o_rate), sl.track, sl.t0, fill + 1, RING_SLOTS,
                        E.diag.underruns, skipped ? " (more suppressed)" : "");
            }
        }
        if (out_format(&sl) != 0) {
            mutex_lock(&E.rlock);
            E.o_busy = false;
            mutex_unlock(&E.rlock);
            sleep_ms(20);
            continue;
        }
        apply_volume(sl.pcm, sl.frames, sl.ch);
        adiag_count_clip(&E.diag, sl.pcm, sl.frames * sl.ch);
        wavrec_feed(sl.pcm, sl.frames, sl.rate, sl.ch);
        int wr = E.sink.write(E.sink.ud, sl.pcm, sl.frames);
        uint64_t ret = now_ms();
        if (primed && !waited && last_ret && adiag_write(&E.diag, ret - last_ret, last_frames, E.o_rate)) {
            uint32_t skipped = 0;
            if (adiag_may_log(&E.diag, ADIAG_LATE, ret, &skipped))
                dbg("audio: late device write: %llu ms since the previous one for %.1f ms of audio; track %d at %.2f s, "
                    "ring %d/%d; %u late total%s",
                    (unsigned long long)(ret - last_ret), adiag_ms(last_frames, E.o_rate), sl.track, sl.t0, fill,
                    RING_SLOTS, E.diag.late_writes, skipped ? " (more suppressed)" : "");
        }
        E.diag.frames += (uint64_t)sl.frames;
        last_ret = ret;
        last_frames = sl.frames;
        primed = true;
        mutex_lock(&E.rlock);
        if (sl.gen == E.gen) {
            E.pos = sl.t0 + sl.frames * sl.step;
            if (wr > 0 && E.out_seq != sl.load) {
                E.out_seq = sl.load;
                dbg("engine: audio output started (load %u) at %d Hz x%d", sl.load, sl.rate, sl.ch);
            }
        }
        E.o_busy = false;
        cond_broadcast(&E.rcv);
        mutex_unlock(&E.rlock);
        out_stats(ret, &last_stats);
    }
    if (E.o_rate) E.sink.close(E.sink.ud);
    E.o_rate = E.o_ch = 0;
    return NULL;
}

int engine_underruns(void) { return (int)E.diag.underruns; }
int engine_late_writes(void) { return (int)E.diag.late_writes; }
int engine_ring_slots(void) { return RING_SLOTS; }

int engine_preopen_hits(void) { return E.pre_hits; }
bool engine_audio_started(void) { return E.load_seq != 0 && E.out_seq == E.load_seq; }

int engine_init(const AudioSink *sink) {
    memset(&E, 0, sizeof E);
    mutex_init(&E.lock);
    mutex_init(&E.rlock);
    cond_init(&E.rcv);
    adiag_reset(&E.diag);
    E.sink = *sink;
    E.speed = 1.0f;
    E.son_speed = 1.0f;
    E.volume = 1.0f;
    mutex_init(&E.pre_lock);
    cond_init(&E.pre_cv);
    E.pre_want = -1;
    E.rec_track = -1;
    E.pre_idx = -1;
    if (!thread_start(&E.pre_th, preopen_thread, NULL, 128, -1)) E.pre_th.ok = false;
    if (!thread_start(&E.out_th, out_thread, NULL, 64, 2)) return -1;
    return thread_start(&E.th, engine_thread, NULL, 128, 1) ? 0 : -1;
}

void engine_exit(void) {
    mutex_lock(&E.lock);
    if (E.st) stream_interrupt(E.st, 1);
    mutex_unlock(&E.lock);
    E.quit = true;
    thread_join_timeout(&E.th, 2000);
    mutex_lock(&E.rlock);
    E.out_quit = true;
    cond_broadcast(&E.rcv);
    mutex_unlock(&E.rlock);
    thread_join_timeout(&E.out_th, 1500);
    mutex_lock(&E.pre_lock);
    E.pre_quit = true;
    cond_broadcast(&E.pre_cv);
    mutex_unlock(&E.pre_lock);
    if (E.pre_th.ok) thread_join_timeout(&E.pre_th, 1500);
    {
        mutex_lock(&E.pre_lock);
        Stream *ps = E.pre;
        E.pre = NULL;
        mutex_unlock(&E.pre_lock);
        stream_close(ps);
    }
    free(E.book.tracks);
    free(E.book.sources);
    if (E.has_pending_book) { free(E.pending_book.tracks); free(E.pending_book.sources); }
    free(E.retime_tracks);
    E.retime_tracks = NULL;
    memset(&E.book, 0, sizeof E.book);
    mutex_destroy(&E.lock);
    mutex_destroy(&E.rlock);
}

static void push(Cmd c) {
    mutex_lock(&E.lock);
    /* collapse repeated seeks/speed changes */
    if (E.qn && E.q[E.qn - 1].type == c.type && (c.type == C_SEEK || c.type == C_SPEED)) E.q[E.qn - 1] = c;
    else if (E.qn < 16) E.q[E.qn++] = c;
    if (E.st) stream_interrupt(E.st, 1); /* unblock a waiting network read */
    mutex_unlock(&E.lock);
}

void engine_load(const PlayableBook *b, double start_t, bool autoplay) {
    PlayableBook copy = *b;
    copy.tracks = malloc(sizeof(Track) * b->ntracks);
    copy.sources = malloc(sizeof(TrackSource) * b->ntracks);
    if (!copy.tracks || !copy.sources) { free(copy.tracks); free(copy.sources); return; }
    memcpy(copy.tracks, b->tracks, sizeof(Track) * b->ntracks);
    memcpy(copy.sources, b->sources, sizeof(TrackSource) * b->ntracks);
    if (copy.duration <= 0) copy.duration = tl_duration(copy.tracks, copy.ntracks);
    mutex_lock(&E.lock);
    free(E.retime_tracks); /* lengths of the previous book */
    E.retime_tracks = NULL;
    E.retime_n = 0;
    if (E.has_pending_book) { free(E.pending_book.tracks); free(E.pending_book.sources); }
    E.pending_book = copy;
    E.has_pending_book = true;
    E.state = ENG_LOADING;
    E.load_seq++;
    E.pos = start_t;
    E.want_play = autoplay;
    mutex_unlock(&E.lock);
    E.book.duration = copy.duration;
    ring_flush();
    push((Cmd){C_LOAD, start_t, autoplay});
}

void engine_retime(const Track *tracks, int ntracks, double duration) {
    if (!tracks || ntracks <= 0) return;
    Track *copy = malloc(sizeof(Track) * (size_t)ntracks);
    if (!copy) return;
    memcpy(copy, tracks, sizeof(Track) * (size_t)ntracks);
    mutex_lock(&E.lock);
    free(E.retime_tracks);
    E.retime_tracks = copy;
    E.retime_n = ntracks;
    E.retime_dur = duration;
    mutex_unlock(&E.lock);
    E.book.duration = duration > 0 ? duration : E.book.duration;
    push((Cmd){C_RETIME, 0, false});
}

void engine_stop(void) {
    E.want_play = false;
    ring_flush();
    push((Cmd){C_STOP, 0, false});
}

void engine_play(void) {
    if (E.state == ENG_ENDED) {
        engine_seek(0);
        E.state = ENG_BUFFERING; /* reads as playing at once, before the thread catches up */
    }
    E.want_play = true;
}

void engine_pause(void) { E.want_play = false; }

void engine_seek(double t) {
    ring_flush(); /* first: the output thread must not report an old position after this */
    mutex_lock(&E.rlock);
    E.pos = t < 0 ? 0 : t;
    mutex_unlock(&E.rlock);
    push((Cmd){C_SEEK, t, false});
}

void engine_set_speed(float speed) {
    if (speed < 0.5f) speed = 0.5f;
    if (speed > 3.0f) speed = 3.0f;
    E.speed = speed;
    push((Cmd){C_SPEED, speed, false});
}

void engine_set_volume(float v) { E.volume = v < 0 ? 0 : v > 1 ? 1 : v; }

EngineState engine_state(void) { return E.state; }
bool engine_is_playing(void) {
    return E.want_play && (E.state == ENG_PLAYING || E.state == ENG_BUFFERING || E.state == ENG_LOADING);
}
double engine_position(void) { return E.pos; }
double engine_duration(void) { return E.book.duration; }
float engine_cpu_load(void) { return E.cpu; }
int engine_error(void) { return E.error; }
float engine_speed(void) { return E.speed; }
