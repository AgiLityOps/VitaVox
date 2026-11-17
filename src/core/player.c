#include "player.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dbg.h"
#include "engine.h"
#include "osal.h"
#include "progress.h"
#include "scanner.h"
#include "libmodel.h"
#include "quit.h"
#include "stream.h"
#include "strutil.h"
#include "timeline.h"
#include "worker.h"

static Config *g_cfg;

static struct {
    Mutex lock;
    volatile PlayerPhase phase;
    char error[96];
    BookDetail book;
    bool has_book;
    SleepTimer sleep;
    float speed;
    bool was_playing;
    volatile int open_gen;
    uint64_t last_save;
    volatile bool sleep_ended;
} P;

typedef struct {
    int gen;
    BookSummary hint;
    double start;
    uint64_t queued;
} OpenJob;

typedef struct {
    int gen;
    int rc;
    char msg[96];
    BookDetail book;
    double start;
    float speed;
} OpenResult;

static OpenResult *g_open_result;

/* 00.36: exact track lengths for a book opened with estimates. */
typedef struct {
    int gen;
    BookSummary hint;
} RefineJob;

typedef struct {
    int gen;
    BookDetail book;
} RefineResult;

static RefineResult *g_refine_result;

static int open_cancelled(void *ud) {
    const int *gen = ud;
    return app_quit_active() || *gen != P.open_gen;
}

static void refine_job(void *ud) {
    RefineJob *j = ud;
    uint64_t t0 = now_ms();
    BookDetail d;
    memset(&d, 0, sizeof d);
    int rc = open_cancelled(&j->gen) ? SCANNER_CANCELLED : scanner_refine_detail(&j->hint, &d, open_cancelled, &j->gen);
    if (rc == 0 && !open_cancelled(&j->gen)) {
        RefineResult *r = calloc(1, sizeof *r);
        if (r) {
            r->gen = j->gen;
            r->book = d;
            memset(&d, 0, sizeof d);
            mutex_lock(&P.lock);
            if (g_refine_result) { book_detail_free(&g_refine_result->book); free(g_refine_result); }
            g_refine_result = r;
            mutex_unlock(&P.lock);
        }
        dbg("play: lengths of %s refined in %llu ms", j->hint.id, (unsigned long long)(now_ms() - t0));
    } else {
        dbg("play: refining %s %s after %llu ms", j->hint.id, rc == SCANNER_CANCELLED || rc == 0 ? "cancelled" : "failed",
            (unsigned long long)(now_ms() - t0));
    }
    book_detail_free(&d);
    free(j);
}

static void save_progress(bool force) {
    if (!P.has_book) return;
    uint64_t now = now_ms();
    if (!force && now - P.last_save < 2000) return;
    BookProgress p = {0};
    p.position = engine_position();
    p.speed = P.speed;
    p.updated_ms = wall_ms();
    p.finished = P.book.duration > 0 && p.position >= P.book.duration - 1;
    p.duration = P.book.provisional ? 0 : P.book.duration; /* an estimate must not fail the length check later */
    progress_save(P.book.id, &p);
    P.last_save = now;
}

static void open_job(void *ud) {
    OpenJob *j = ud;
    OpenResult *r = calloc(1, sizeof *r);
    if (!r) { free(j); return; }
    r->gen = j->gen;
    r->speed = P.speed;
    int got = 0;
    if (app_quit_active()) {
        r->rc = -1;
        str_copy(r->msg, sizeof r->msg, "Cancelled");
    } else if (lib_copy_detail(j->hint.id, &r->book) == 0 && r->book.ntracks > 0) {
        dbg("play: using cached detail for %s (%d tracks)", j->hint.id, r->book.ntracks);
        got = 1;
    } else {
        /* Per-book file (books/<id>.bin, one read) or, if missing/stale, a priority
         * load: the background scan waits, and for a many-file book only the start
         * is timed exactly before playback (the rest is refined while it plays). */
        book_detail_free(&r->book);
        double want = j->start;
        if (want < 0) {
            BookProgress pre;
            want = progress_load_for(j->hint.id, 0, &pre) == 0 ? pre.position : j->hint.current_time;
        }
        int orc = scanner_open_detail_at(&j->hint, &r->book, want, open_cancelled, &j->gen);
        if (orc == SCANNER_CANCELLED) {
            r->rc = -1;
            str_copy(r->msg, sizeof r->msg, "Cancelled");
        } else if (orc != 0) {
            r->rc = -1;
            str_copy(r->msg, sizeof r->msg, "Couldn't open this book");
        } else if (r->book.ntracks <= 0) {
            r->rc = -1;
            str_copy(r->msg, sizeof r->msg, "This book has no audio files");
            book_detail_free(&r->book);
        } else {
            got = 1;
        }
    }
    if (got) {
        BookProgress prog;
        double start = j->start;
        if (start < 0) {
            /* Estimated total: no length check yet (it is exact once refined). */
            int pr = progress_load_for(j->hint.id, r->book.provisional ? 0 : r->book.duration, &prog);
            if (pr == 0) {
                start = prog.position;
                if (prog.speed >= 0.75f && prog.speed <= 2.0f) r->speed = prog.speed;
            } else if (pr == -2) {
                start = 0; /* progress of a different book with the same path in another folder */
            } else {
                start = j->hint.current_time;
            }
        }
        if (r->book.duration > 0 && start > r->book.duration - 5) start = 0;
        if (start < 0) start = 0;
        r->start = start;
        dbg("play %s at %.0f (%d tracks, %.0f s%s; detail ready %llu ms after open)", r->book.id, r->start,
            r->book.ntracks, r->book.duration, r->book.provisional ? " estimated" : "",
            (unsigned long long)(now_ms() - j->queued));
    }
    int refine = got && r->book.provisional > 0;
    mutex_lock(&P.lock);
    if (g_open_result) { book_detail_free(&g_open_result->book); free(g_open_result); }
    g_open_result = r;
    mutex_unlock(&P.lock);
    if (refine) {
        /* Playback can start now; the exact lengths follow (enrich still waits). */
        RefineJob *rj = calloc(1, sizeof *rj);
        if (rj) {
            rj->gen = j->gen;
            rj->hint = j->hint;
            if (!g_refine || !worker_post(g_refine, refine_job, rj)) refine_job(rj);
        }
    }
    free(j);
}

void player_open(const BookSummary *b, double start) {
    if (P.has_book && !strcmp(P.book.id, b->id) && P.phase == PL_READY) {
        if (start >= 0) player_jump(start);
        if (!engine_is_playing()) engine_play();
        return;
    }
    if (P.has_book) {
        engine_stop();
        save_progress(true);
    }
    OpenJob *j = calloc(1, sizeof *j);
    if (!j) return;
    j->gen = ++P.open_gen;
    j->hint = *b;
    j->start = start;
    j->queued = now_ms();
    P.phase = PL_OPENING;
    P.error[0] = 0;
    if (!worker_post(g_api, open_job, j)) {
        free(j);
        P.phase = PL_FAILED;
        str_copy(P.error, sizeof P.error, "Busy, try again");
    }
}

static void take_refine_result(void) {
    mutex_lock(&P.lock);
    RefineResult *r = g_refine_result;
    g_refine_result = NULL;
    mutex_unlock(&P.lock);
    if (!r) return;
    if (r->gen != P.open_gen || !P.has_book || strcmp(P.book.id, r->book.id) || r->book.ntracks != P.book.ntracks) {
        book_detail_free(&r->book);
        free(r);
        return;
    }
    double was = P.book.duration;
    free(P.book.tracks);
    free(P.book.chapters);
    P.book.tracks = r->book.tracks;
    P.book.chapters = r->book.chapters;
    P.book.nchapters = r->book.nchapters;
    P.book.duration = r->book.duration;
    P.book.provisional = 0;
    r->book.tracks = NULL;
    r->book.chapters = NULL;
    r->book.ntracks = r->book.nchapters = 0;
    book_detail_free(&r->book);
    free(r);
    engine_retime(P.book.tracks, P.book.ntracks, P.book.duration);
    dbg("player: %s track lengths refined: %.1f s (estimate was %.1f s)", P.book.id, P.book.duration, was);
}

static void take_open_result(void) {
    mutex_lock(&P.lock);
    OpenResult *r = g_open_result;
    g_open_result = NULL;
    mutex_unlock(&P.lock);
    if (!r) return;
    if (r->gen != P.open_gen) { book_detail_free(&r->book); free(r); return; }
    if (r->rc != 0) {
        P.phase = PL_FAILED;
        str_copy(P.error, sizeof P.error, r->msg);
        book_detail_free(&r->book);
        free(r);
        return;
    }
    if (P.has_book) book_detail_free(&P.book);
    P.book = r->book;
    P.has_book = true;
    P.speed = r->speed;
    sleep_set(&P.sleep, SLEEP_OFF, 0, 0);
    engine_set_volume(1.0f);

    PlayableBook pb = {r->book.ntracks, r->book.tracks, calloc(r->book.ntracks, sizeof(TrackSource)), r->book.duration};
    if (!pb.sources) {
        P.phase = PL_FAILED;
        str_copy(P.error, sizeof P.error, "Out of memory");
        free(r);
        return;
    }
    for (int i = 0; i < pb.ntracks; i++) {
        const Track *t = &r->book.tracks[i];
        TrackSource *s = &pb.sources[i];
        s->size = t->size;
        str_copy(s->ext, sizeof s->ext, t->ext);
        str_copy(s->path, sizeof s->path, t->path);
    }
    engine_set_speed(P.speed);
    engine_load(&pb, r->start, true);
    free(pb.sources);
    /* Stay in PL_OPENING until the engine finishes stream_open (the one file open). */
    P.last_save = now_ms();
    free(r);
}

void player_init(Config *cfg) {
    memset(&P, 0, sizeof P);
    mutex_init(&P.lock);
    g_cfg = cfg;
    P.speed = cfg && cfg->speed > 0 ? cfg->speed : 1.0f;
    P.last_save = now_ms();
}

void player_exit(void) {
    if (P.has_book) {
        engine_pause();
        save_progress(true);
        engine_stop();
        book_detail_free(&P.book);
        P.has_book = false;
    }
}

void player_close(void) {
    /* Any open still in flight is ignored when it finishes (generation bump). */
    P.open_gen++;
    mutex_lock(&P.lock);
    if (g_open_result) { book_detail_free(&g_open_result->book); free(g_open_result); g_open_result = NULL; }
    if (g_refine_result) { book_detail_free(&g_refine_result->book); free(g_refine_result); g_refine_result = NULL; }
    mutex_unlock(&P.lock);
    if (P.has_book) {
        engine_pause();
        if (P.phase == PL_READY) save_progress(true); /* not while still opening: the position isn't the book's yet */
        dbg("player: closed %s at %.1f s", P.book.id, engine_position());
    }
    engine_stop(); /* also drops the next-track preopen */
    if (P.has_book) book_detail_free(&P.book);
    P.has_book = false;
    P.phase = PL_IDLE;
    P.error[0] = 0;
    P.was_playing = false;
    P.sleep_ended = false;
    sleep_set(&P.sleep, SLEEP_OFF, 0, 0);
    engine_set_volume(1.0f);
}

void player_flush_queue(void) { save_progress(true); }

bool player_take_sleep_ended(void) {
    bool v = P.sleep_ended;
    P.sleep_ended = false;
    return v;
}

PlayerPhase player_phase(void) { return P.phase; }
const char *player_error(void) { return P.error; }
bool player_has_book(void) { return P.has_book && P.phase == PL_READY; }
const BookDetail *player_book(void) { return P.has_book ? &P.book : NULL; }
const char *player_book_id(void) { return P.has_book ? P.book.id : ""; }
bool player_is_local(void) { return P.has_book; }
bool player_playing(void) { return P.has_book && engine_is_playing(); }
double player_position(void) { return engine_position(); }
double player_duration(void) { return P.has_book ? P.book.duration : 0; }
float player_speed(void) { return P.speed; }
const SleepTimer *player_sleep(void) { return &P.sleep; }

int player_chapter_index(void) {
    return P.has_book ? tl_chapter_at(P.book.chapters, P.book.nchapters, engine_position()) : -1;
}

void player_pause(void) {
    if (!P.has_book || !engine_is_playing()) return;
    engine_pause();
    save_progress(true);
}

void player_toggle(void) {
    if (!P.has_book) return;
    if (engine_is_playing()) player_pause();
    else engine_play();
}

void player_jump(double t) {
    if (!P.has_book) return;
    if (t < 0) t = 0;
    if (t > P.book.duration) t = P.book.duration;
    engine_seek(t);
    save_progress(true);
}

void player_skip(double seconds) { player_jump(engine_position() + seconds); }

void player_chapter(int dir) {
    if (!P.has_book) return;
    double t = engine_position();
    double target = dir > 0 ? tl_next_chapter(P.book.chapters, P.book.nchapters, t)
                            : tl_prev_chapter(P.book.chapters, P.book.nchapters, t);
    if (dir > 0 && target < 0) return;
    player_jump(target);
}

void player_set_speed(float s) {
    P.speed = s;
    if (g_cfg) g_cfg->speed = s;
    engine_set_speed(s);
    save_progress(true);
}

void player_cycle_speed(void) { player_set_speed(cfg_next_speed(P.speed)); }

void player_cycle_sleep(void) {
    static const int mins[] = {15, 30, 45, 60};
    SleepTimer *s = &P.sleep;
    if (s->mode == SLEEP_OFF) { sleep_set(s, SLEEP_MINUTES, mins[0], 0); return; }
    if (s->mode == SLEEP_MINUTES) {
        double cur = s->remaining / 60.0;
        for (int i = 0; i < 4; i++)
            if (cur < mins[i] - 0.5) { sleep_set(s, SLEEP_MINUTES, mins[i], 0); return; }
        int ci = player_chapter_index();
        double end = ci >= 0 ? P.book.chapters[ci].end : P.book.duration;
        sleep_set(s, SLEEP_END_CHAPTER, 0, end);
        return;
    }
    sleep_set(s, SLEEP_OFF, 0, 0);
    engine_set_volume(1.0f);
}

void player_sleep_label(char *out, int n) {
    const SleepTimer *s = &P.sleep;
    if (s->mode == SLEEP_OFF) snprintf(out, n, "Off");
    else if (s->mode == SLEEP_END_CHAPTER) snprintf(out, n, "End of chapter");
    else {
        char t[16];
        fmt_time(s->remaining, t, sizeof t);
        snprintf(out, n, "%s", t);
    }
}

const char *player_status(void) {
    if (P.phase == PL_OPENING) {
        static char buf[48];
        float frac = 0;
        char msg[64];
        msg[0] = 0;
        stream_open_progress(&frac, msg, sizeof msg);
        int pct = (int)(frac * 100.0f + 0.5f);
        if (pct < 0) pct = 0;
        if (pct > 99) pct = 99;
        if (pct > 0) snprintf(buf, sizeof buf, "Opening book\xE2\x80\xA6 %d%%", pct);
        else snprintf(buf, sizeof buf, "Opening book\xE2\x80\xA6");
        return buf;
    }
    if (P.phase == PL_FAILED) return P.error;
    if (!P.has_book) return NULL;
    static char buf[64];
    EngineState s = engine_state();
    if (s == ENG_ERROR) {
        int e = engine_error();
        if (e == STREAM_INTERRUPTED) return "Opening timed out";
        if (e == STREAM_UNSUPPORTED) return "Format not supported";
        if (e == STREAM_IO) return "Can't read the file";
        snprintf(buf, sizeof buf, "Playback error (%d)", e);
        return buf;
    }
    if (s == ENG_LOADING) return "Opening book\xE2\x80\xA6";
    return NULL;
}

static void fail_open(const char *msg) {
    P.phase = PL_FAILED;
    str_copy(P.error, sizeof P.error, msg);
}

void player_tick(double dt) {
    take_open_result();
    take_refine_result(); /* after the open it belongs to (both can be waiting) */
    if (P.phase == PL_OPENING && P.has_book) {
        EngineState s = engine_state();
        if (s == ENG_ERROR) {
            int e = engine_error();
            if (e == STREAM_INTERRUPTED) fail_open("Opening timed out");
            else if (e == STREAM_UNSUPPORTED) fail_open("Format not supported");
            else if (e == STREAM_IO) fail_open("Can't read the file");
            else fail_open("Couldn't start playback");
        } else if (s != ENG_LOADING && s != ENG_IDLE) {
            P.phase = PL_READY;
        }
    }
    if (!P.has_book || P.phase != PL_READY) return;
    bool playing = engine_is_playing();
    double pos = engine_position();

    if (playing) save_progress(false);
    if (P.was_playing && !playing) save_progress(true);
    P.was_playing = playing;

    if (playing && engine_state() == ENG_PLAYING) {
        int r = sleep_tick(&P.sleep, dt, pos);
        if (r == SLEEP_STOP) {
            player_pause();
            engine_set_volume(1.0f);
            save_progress(true);
            P.sleep_ended = true; /* the platform layer turns the display off */
            dbg("sleep timer: ended at %.1f s -> paused, progress saved, display off requested", pos);
        } else if (P.sleep.mode != SLEEP_OFF) {
            engine_set_volume(sleep_volume(&P.sleep, pos, P.speed));
        }
    }
}
