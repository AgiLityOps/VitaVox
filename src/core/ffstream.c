#include "stream.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <limits.h>

#ifdef __vita__
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#endif

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>

#include "dbg.h"
#include "osal.h"
#include "pathutil.h"
#include "strutil.h"
#include "quit.h"
#include "tagart.h"
#include "mp4lazy.h"
#include <libavutil/intreadwrite.h>

#define IO_BUF_PROBE  (1024 * 1024)
#define IO_BUF_PLAY   (1024 * 1024)
#define STREAM_OPEN_TIMEOUT_MS 20000

typedef struct {
#ifdef __vita__
    SceUID fd;
#else
    FILE *fp;
#endif
    int64_t size, pos;
    uint64_t deadline_ms, t0;
    uint64_t deadline_span; /* length of the open deadline, restarted after a power event */
    uint64_t stall_ms, last_io_ms;
    uint64_t op_start_ms;  /* start of the device read/seek in progress (0 = none) */
    unsigned op_power_gen; /* g_power_gen when that operation started */
    unsigned power_gen;    /* g_power_gen last seen by the open deadline */
    int timed_out;
    int stalled;           /* timed_out came from the stall watchdog (not cancel / open deadline) */
    int slow_ops;          /* device operations that took >= stall_ms but completed */
    uint64_t bytes_read;   /* logical bytes delivered to FFmpeg */
    int seeks;             /* logical seeks (including those served from cache) */
    uint64_t disk_bytes;   /* bytes actually read from storage */
    int disk_seeks;        /* sceIoLseek / fseek that hit the device */
    uint64_t ra_hits;
    uint8_t *ra;
    int ra_cap, ra_len;
    int64_t ra_off;        /* file offset of ra[0]; -1 = empty */
    int *cancel;
    char path[512];
} IoFile;

#define RA_CAP (2 * 1024 * 1024)
#define RA_ALIGN (64 * 1024)

static volatile int g_cancel;
/* Bumped on every power event (display off/on, suspend, resume): a device
 * operation that spans one is not judged by wall time. */
static volatile unsigned g_power_gen;
static uint64_t (*g_clock)(void) = now_ms;

void stream_note_power_event(void) { g_power_gen++; }
void stream_test_set_clock(uint64_t (*fn)(void)) { g_clock = fn ? fn : now_ms; }
static uint64_t io_now(void) { return g_clock(); }
static volatile float g_open_frac;
static char g_open_msg[64];
static Mutex g_prog = PTHREAD_MUTEX_INITIALIZER;

void stream_request_cancel(int on) { g_cancel = on ? 1 : 0; }

void stream_open_progress(float *frac, char *msg, size_t n) {
    mutex_lock(&g_prog);
    if (frac) *frac = g_open_frac;
    if (msg && n) {
        size_t i = 0;
        for (; i + 1 < n && g_open_msg[i]; i++) msg[i] = g_open_msg[i];
        msg[i] = 0;
    }
    mutex_unlock(&g_prog);
}

static void set_open_progress(float frac, const char *msg) {
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    mutex_lock(&g_prog);
    g_open_frac = frac;
    if (msg) {
        size_t i = 0;
        for (; i + 1 < sizeof g_open_msg && msg[i]; i++) g_open_msg[i] = msg[i];
        g_open_msg[i] = 0;
    }
    mutex_unlock(&g_prog);
}

static int io_should_stop(IoFile *f) {
    if (app_quit_active() || g_cancel) return 1;
    if (f && f->cancel && *f->cancel) return 1;
    return 0;
}

static Mutex g_ff = PTHREAD_MUTEX_INITIALIZER;

static int io_interrupt(void *opaque);

/* FFmpeg 9 AVIOContext has no public interrupt_callback; custom reads check
 * io_interrupt themselves, and avformat uses ctx->interrupt_callback. */
static void apply_interrupt(AVFormatContext *fmt, AVIOContext *avio, IoFile *io)
{
    (void)avio;
    if (fmt) {
        fmt->interrupt_callback.callback = io_interrupt;
        fmt->interrupt_callback.opaque = io;
    }
}

struct Stream {
    AVFormatContext *fmt;
    AVIOContext *avio;
    int custom_io;
    IoFile io;
    AVCodecContext *dec;
    SwrContext *swr;
    AVPacket *pkt;
    AVFrame *frame;
    int stream_index;
    int out_rate;
    int leftover_off, leftover_frames;
    int leftover_cap;
    int16_t *leftover;
    bool eof, draining;
    int interrupted;
    int fail;              /* sticky mid-stream error: never turns into a silent EOF */
    /* swr is (re)built from the first / current decoded frame, not codecpar */
    int swr_in_rate;
    enum AVSampleFormat swr_in_fmt;
    AVChannelLayout swr_in_layout;
    int logged_pkt, logged_frame, logged_out;
    int pkts, frames, samples_out, underruns;
    uint64_t last_stat_ms;
    Mp4 *mp4;              /* lazy MP4/M4B demuxer (fmt is NULL then) */
              /* lazy MP4/M4B demuxer (fmt is NULL then) */
};

static volatile int g_mp4_lazy = 1;
void stream_set_mp4_lazy(int on) { g_mp4_lazy = on ? 1 : 0; }

bool stream_supported(const char *ext, const char *codec) {
    (void)codec;
    if (!ext || !ext[0]) return true;
    return path_is_audio(ext);
}

static void log_averr(const char *what, const char *url, int err) {
    char buf[128];
    av_strerror(err, buf, sizeof buf);
    dbg("ffstream: %s '%s' failed: %s (%d)", what, url, buf, err);
}

static void io_touch(IoFile *f) {
    if (f) f->last_io_ms = io_now();
}

/* Wall-clock open/probe deadline only. After a successful play open this is
 * cleared and a per-operation stall watchdog is used instead. */
static void io_disarm_deadline(IoFile *f) {
    if (!f) return;
    f->deadline_ms = 0;
    f->timed_out = 0;
    f->stall_ms = 15000;
    f->op_start_ms = 0;
    io_touch(f);
}

/* Stall watchdog (v0.2.12). It only measures time spent inside a device
 * read/seek that is still in progress (op_start_ms != 0), never the time
 * since the last device access: a fully buffered small file, a pause, a
 * preopened next track, or the screen being off must not count. A device
 * operation that spans a power event is not judged by wall time. */
static void io_op_begin(IoFile *f) {
    f->op_start_ms = io_now();
    f->op_power_gen = g_power_gen;
}

static int io_op_overdue(IoFile *f, uint64_t now) {
    return f->stall_ms && f->op_start_ms && f->op_power_gen == g_power_gen
        && now - f->op_start_ms >= f->stall_ms;
}

/* After a device call returned: a slow but successful call is logged, not fatal. */
static void io_op_end(IoFile *f, int ok) {
    if (!f->op_start_ms) return;
    uint64_t now = io_now();
    if (f->stall_ms && f->op_power_gen == g_power_gen && now - f->op_start_ms >= f->stall_ms) {
        f->slow_ops++;
        dbg("ffstream: slow device I/O %llu ms on '%s' (%s)", (unsigned long long)(now - f->op_start_ms),
            f->path, ok ? "completed, continuing" : "failed");
    }
    f->op_start_ms = 0;
}

static int io_interrupt(void *opaque) {
    IoFile *f = opaque;
    if (app_quit_active() || g_cancel) return 1;
    if (!f) return 0;
    if (f->cancel && *f->cancel) return 1;
    if (f->timed_out) return 1;
    uint64_t now = io_now();
    if (f->power_gen != g_power_gen) {
        /* Suspended or display power change during an open: restart its clock. */
        f->power_gen = g_power_gen;
        if (f->deadline_ms) f->deadline_ms = now + (f->deadline_span ? f->deadline_span : STREAM_OPEN_TIMEOUT_MS);
    }
    if (f->deadline_ms && now >= f->deadline_ms) {
        f->timed_out = 1;
        dbg("ffstream: open deadline after %llu ms on '%s' (read %llu bytes, %d seeks)",
            (unsigned long long)(now - f->t0), f->path,
            (unsigned long long)f->bytes_read, f->seeks);
        return 1;
    }
    if (io_op_overdue(f, now)) {
        f->timed_out = 1;
        f->stalled = 1;
        dbg("ffstream: stall: device I/O blocked %llu ms on '%s' (read %llu bytes, %d seeks)",
            (unsigned long long)(now - f->op_start_ms), f->path,
            (unsigned long long)f->bytes_read, f->seeks);
        return 1;
    }
    return 0;
}

static int io_raw_seek(IoFile *f, int64_t offset, int whence_set_cur_end) {
#ifdef __vita__
    int w = SCE_SEEK_SET;
    if (whence_set_cur_end == SEEK_CUR) w = SCE_SEEK_CUR;
    else if (whence_set_cur_end == SEEK_END) w = SCE_SEEK_END;
    int nested = f->op_start_ms != 0;
    if (!nested) io_op_begin(f);
    SceOff pos = sceIoLseek(f->fd, (SceOff)offset, w);
    if (!nested) io_op_end(f, pos >= 0);
    if (pos < 0) return -1;
    f->pos = (int64_t)pos;
#else
    int nested = f->op_start_ms != 0;
    if (!nested) io_op_begin(f);
    int sr = fseek(f->fp, (long)offset, whence_set_cur_end);
    if (!nested) io_op_end(f, sr == 0);
    if (sr != 0) return -1;
    f->pos = (int64_t)ftell(f->fp);
#endif
    f->disk_seeks++;
    return 0;
}

static int io_raw_read(IoFile *f, uint8_t *buf, int buf_size) {
    int nested = f->op_start_ms != 0;
    if (!nested) io_op_begin(f);
#ifdef __vita__
    int n = sceIoRead(f->fd, buf, (SceSize)buf_size);
    if (!nested) io_op_end(f, n >= 0);
    if (n < 0) return AVERROR(EIO);
    if (n == 0) return AVERROR_EOF;
#else
    size_t n = fread(buf, 1, (size_t)buf_size, f->fp);
    if (!nested) io_op_end(f, n > 0 || feof(f->fp));
    if (n == 0) return feof(f->fp) ? AVERROR_EOF : AVERROR(EIO);
#endif
    f->disk_bytes += (uint64_t)(n > 0 ? n : 0);
    f->pos += n > 0 ? n : 0;
    io_touch(f);
    return (int)n;
}

static int ra_refill(IoFile *f, int64_t want) {
    if (!f->ra || f->ra_cap <= 0) return -1;
    int64_t start = want < 0 ? 0 : want;
    start = start - (start % RA_ALIGN);
    if (start < 0) start = 0;
    if (f->size > 0 && start >= f->size) return AVERROR_EOF;
    /* The whole refill (seek + reads) is one device operation for the watchdog. */
    io_op_begin(f);
    if (io_raw_seek(f, start, SEEK_SET) != 0) { io_op_end(f, 0); return -1; }
    int want_n = f->ra_cap;
    if (f->size > 0 && start + want_n > f->size) want_n = (int)(f->size - start);
    int got = 0;
    while (got < want_n) {
        if (io_should_stop(f)) { io_op_end(f, 0); return AVERROR_EXIT; }
        if (got > 0 && io_interrupt(f)) { f->op_start_ms = 0; return AVERROR_EXIT; } /* still short after stall_ms */
        int n = io_raw_read(f, f->ra + got, want_n - got);
        if (n == AVERROR_EOF) break;
        if (n < 0) { io_op_end(f, 0); return n; }
        got += n;
    }
    io_op_end(f, 1);
    f->ra_off = start;
    f->ra_len = got;
    return got > 0 ? 0 : AVERROR_EOF;
}

static int io_open_file(IoFile *f, const char *path) {
    memset(f, 0, sizeof *f);
    str_copy(f->path, sizeof f->path, path);
    f->t0 = now_ms();
    f->ra_off = -1;
    f->ra_cap = RA_CAP;
    f->ra = (uint8_t *)malloc((size_t)f->ra_cap);
    if (!f->ra) f->ra_cap = 0;
#ifdef __vita__
    f->fd = -1;
    f->fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (f->fd < 0) {
        dbg("ffstream: sceIoOpen('%s') failed %08x", path, (unsigned)f->fd);
        free(f->ra); f->ra = NULL; return -1;
    }
    SceIoStat st;
    memset(&st, 0, sizeof st);
    if (sceIoGetstat(path, &st) >= 0) f->size = (int64_t)st.st_size;
    else {
        SceOff end = sceIoLseek(f->fd, 0, SCE_SEEK_END);
        sceIoLseek(f->fd, 0, SCE_SEEK_SET);
        if (end >= 0) f->size = (int64_t)end;
    }
    dbg("ffstream: sceIoOpen ok fd=%d size=%lld", (int)f->fd, (long long)f->size);
    return 0;
#else
    f->fp = fopen(path, "rb");
    if (!f->fp) { free(f->ra); f->ra = NULL; return -1; }
    if (fseek(f->fp, 0, SEEK_END) == 0) f->size = (int64_t)ftell(f->fp);
    fseek(f->fp, 0, SEEK_SET);
    return 0;
#endif
}

static void io_close_file(IoFile *f) {
    if (!f) return;
#ifdef __vita__
    if (f->fd >= 0) { sceIoClose(f->fd); f->fd = -1; }
#else
    if (f->fp) { fclose(f->fp); f->fp = NULL; }
#endif
    free(f->ra);
    f->ra = NULL;
    f->ra_len = 0;
    f->ra_off = -1;
}

static int io_read(void *opaque, uint8_t *buf, int buf_size) {
    IoFile *f = opaque;
    if (io_interrupt(f)) return AVERROR_EXIT;
    if (buf_size <= 0) return 0;
    int64_t pos = f->pos;
    if (f->size > 0 && pos >= f->size) return AVERROR_EOF;

    /* Serve from the readahead window when possible (turns moov parse seeks
     * into memcpy — the usual cause of 30s opens on SD2Vita). */
    if (f->ra && f->ra_len > 0 && pos >= f->ra_off && pos < f->ra_off + f->ra_len) {
        int have = (int)(f->ra_off + f->ra_len - pos);
        if (have > buf_size) have = buf_size;
        memcpy(buf, f->ra + (pos - f->ra_off), (size_t)have);
        f->pos = pos + have;
        f->bytes_read += (uint64_t)have;
        f->ra_hits += (uint64_t)have;
        io_touch(f);
        /* If FFmpeg wanted more than the window tail, refill and continue. */
        if (have < buf_size) {
            int rest = io_read(f, buf + have, buf_size - have);
            if (rest < 0) return have; /* return what we have */
            return have + rest;
        }
        return have;
    }

    if (f->ra && f->ra_cap > 0) {
        int rr = ra_refill(f, pos);
        f->pos = pos; /* refill advances the fd cursor; keep the logical read position */
        if (rr < 0) return rr;
        return io_read(f, buf, buf_size);
    }

    /* No cache: pass through. Keep the fd cursor at f->pos. */
    io_op_begin(f);
    if (io_raw_seek(f, pos, SEEK_SET) != 0) { io_op_end(f, 0); return AVERROR(EIO); }
    int n = io_raw_read(f, buf, buf_size);
    io_op_end(f, n >= 0 || n == AVERROR_EOF);
    if (n > 0) f->bytes_read += (uint64_t)n;
    return n;
}

static int64_t io_seek(void *opaque, int64_t offset, int whence) {
    IoFile *f = opaque;
    if (whence == AVSEEK_SIZE) return f->size;
    if (io_interrupt(f)) return AVERROR_EXIT;
    int64_t np = f->pos;
    if (whence == SEEK_SET) np = offset;
    else if (whence == SEEK_CUR) np = f->pos + offset;
    else if (whence == SEEK_END) np = (f->size >= 0 ? f->size : 0) + offset;
    else return -1;
    if (np < 0) return -1;
    /* In-window seek: no device I/O. */
    if (f->ra && f->ra_len > 0 && np >= f->ra_off && np <= f->ra_off + f->ra_len) {
        f->pos = np;
        f->seeks++;
        io_touch(f);
        return f->pos;
    }
    f->pos = np;
    f->seeks++;
    io_touch(f);
    return f->pos;
}

static void io_free_avio(AVIOContext **pb) {
    if (!pb || !*pb) return;
    av_freep(&(*pb)->buffer);
    avio_context_free(pb);
}

typedef struct {
    AVFormatContext *fmt;
    AVIOContext *avio;
    IoFile io;
    int custom_io;
} Opened;

static void opened_close(Opened *o) {
    if (!o) return;
    if (o->fmt) {
        if (o->custom_io) o->fmt->pb = NULL;
        avformat_close_input(&o->fmt);
    }
    if (o->custom_io) io_free_avio(&o->avio);
    io_close_file(&o->io);
    memset(o, 0, sizeof *o);
#ifdef __vita__
    o->io.fd = -1;
#endif
}

/* Open path with a large-buffer sceIo (or POSIX) AVIO. Falls back to file: if needed.
 * playback: set mov ignore_chapters (chapters already cached) and play-sized options. */
static void set_mov_opts(AVDictionary **opts, int playback) {
    av_dict_set(opts, "ignore_editlist", "1", 0);
    av_dict_set(opts, "advanced_editlist", "0", 0);
    av_dict_set(opts, "enable_drefs", "0", 0);
    av_dict_set(opts, "use_mfra_for", "-1", 0);
    /* chpl is also gated by ignore_chapters, so only skip chapter-track I/O on playback. */
    if (playback) av_dict_set(opts, "ignore_chapters", "1", 0);
}

static const AVInputFormat *hint_input_format(const char *path) {
    const char *name = path_demuxer_name(path);
    if (!name) return NULL;
    const AVInputFormat *f = av_find_input_format(name);
    if (!f) dbg("ffstream: no demuxer named '%s' for '%s'", name, path);
    else dbg("ffstream: force demuxer '%s' for '%s'", name, path);
    return f;
}

static void log_opened(const Opened *o, const char *via) {
    const AVFormatContext *fmt = o ? o->fmt : NULL;
    const char *iname = (fmt && fmt->iformat && fmt->iformat->name) ? fmt->iformat->name : "?";
    unsigned ns = fmt ? fmt->nb_streams : 0;
    dbg("ffstream: opened via %s format=%s streams=%u (logical %llu B / %d seeks, disk %llu B / %d seeks, cache_hits %llu B, %lld ms)",
        via, iname, ns,
        (unsigned long long)o->io.bytes_read, o->io.seeks,
        (unsigned long long)o->io.disk_bytes, o->io.disk_seeks,
        (unsigned long long)o->io.ra_hits,
        (long long)(now_ms() - o->io.t0));
}

static int opened_open(Opened *o, const char *path, int quick, int playback, uint64_t timeout_ms, int *cancel) {
    memset(o, 0, sizeof *o);
#ifdef __vita__
    o->io.fd = -1;
#endif
    int bufsz = quick ? IO_BUF_PROBE : IO_BUF_PLAY;
    uint64_t t0 = now_ms();
    dbg("ffstream: open start '%s' quick=%d play=%d timeout=%llu avio_buf=%d",
        path, quick, playback, (unsigned long long)timeout_ms, bufsz);

    AVDictionary *opts = NULL;
    set_mov_opts(&opts, playback);
    const AVInputFormat *iformat = hint_input_format(path);

    set_open_progress(0.05f, "Opening book");
    if (io_open_file(&o->io, path) == 0) {
        /* Preload moov (if we can find it without reading mdat) so sample-table
         * parse is memory-bound instead of hundreds of SD2Vita seeks. */
        if (playback || quick) {
            int64_t moff = 0, msz = 0;
            if (mp4_locate_box(path, "moov", &moff, &msz) == 0 && msz > 0) {
                dbg("ffstream: moov at %lld size=%lld (file %lld)",
                    (long long)moff, (long long)msz, (long long)o->io.size);
                set_open_progress(0.15f, "Reading index");
                if (o->io.ra && msz <= 12 * 1024 * 1024) {
                    /* Grow the window to hold the whole moov when it fits. */
                    int need = (int)msz + RA_ALIGN;
                    if (need > o->io.ra_cap) {
                        uint8_t *nra = (uint8_t *)realloc(o->io.ra, (size_t)need);
                        if (nra) { o->io.ra = nra; o->io.ra_cap = need; }
                    }
                }
                if (o->io.ra) {
                    o->io.pos = moff;
                    ra_refill(&o->io, moff);
                    o->io.pos = 0; /* AVIO starts at the beginning of the file */
                    /* If moov is larger than the window, we still have the head. */
                    set_open_progress(0.35f, "Reading index");
                }
            }
        }
        unsigned char *buf = av_malloc((size_t)bufsz);
        if (!buf) { io_close_file(&o->io); av_dict_free(&opts); return AVERROR(ENOMEM); }
        o->avio = avio_alloc_context(buf, bufsz, 0, &o->io, io_read, NULL, io_seek);
        if (!o->avio) { av_free(buf); io_close_file(&o->io); av_dict_free(&opts); return AVERROR(ENOMEM); }
        o->fmt = avformat_alloc_context();
        if (!o->fmt) { io_free_avio(&o->avio); io_close_file(&o->io); av_dict_free(&opts); return AVERROR(ENOMEM); }
        o->fmt->pb = o->avio;
        o->fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
        o->custom_io = 1;
        o->io.t0 = t0;
        o->io.cancel = cancel;
        if (timeout_ms) { o->io.deadline_span = timeout_ms; o->io.deadline_ms = io_now() + timeout_ms; }
        apply_interrupt(o->fmt, o->avio, &o->io);
        /* 1–2 MB so a large ID3/APIC or FLAC PICTURE does not starve STREAMINFO. */
        o->fmt->probesize = quick ? 1024 * 1024 : 2 * 1024 * 1024;
        o->fmt->max_analyze_duration = quick ? 500000 : 2000000;
        uint64_t t1 = now_ms();
        dbg("ffstream: avformat_open_input (custom AVIO) start");
        int err = avformat_open_input(&o->fmt, path, iformat, &opts);
        /* avformat_open_input may replace or clear the callback; put it back. */
        if (o->fmt) apply_interrupt(o->fmt, o->avio ? o->avio : o->fmt->pb, &o->io);
        dbg("ffstream: avformat_open_input end %llu ms rc=%d (read %llu bytes, %d seeks)",
            (unsigned long long)(now_ms() - t1), err,
            (unsigned long long)o->io.bytes_read, o->io.seeks);
        if (err < 0) {
            if (io_should_stop(&o->io)) {
                /* Shutdown / cancel: the reads were refused on purpose; no error, no file: retry. */
                dbg("ffstream: open of '%s' cancelled", path);
                av_dict_free(&opts);
                return AVERROR_EXIT;
            }
            log_averr("open_input avio", path, err);
            if (o->io.timed_out) {
                av_dict_free(&opts);
                return err;
            }
            /* fall through to file: fallback */
            if (o->custom_io) {
                if (o->fmt) {
                    o->fmt->pb = NULL;
                    avformat_free_context(o->fmt);
                    o->fmt = NULL;
                }
                io_free_avio(&o->avio);
            }
            io_close_file(&o->io);
            o->custom_io = 0;
        } else {
            av_dict_free(&opts);
            set_open_progress(0.9f, "Opening book");
            log_opened(o, "custom AVIO");
            return 0;
        }
    }

    /* Fallback: file: protocol (newlib). Still honor the interrupt timeout. */
    char url[576];
    path_file_url(path, url, sizeof url);
    dbg("ffstream: falling back to '%s'", url);
    o->fmt = avformat_alloc_context();
    if (!o->fmt) { av_dict_free(&opts); return AVERROR(ENOMEM); }
    o->io.t0 = t0;
    o->io.cancel = cancel;
    if (timeout_ms) { o->io.deadline_span = timeout_ms; o->io.deadline_ms = io_now() + timeout_ms; }
    str_copy(o->io.path, sizeof o->io.path, path);
    apply_interrupt(o->fmt, NULL, &o->io);
    o->fmt->probesize = quick ? 1024 * 1024 : 2 * 1024 * 1024;
    o->fmt->max_analyze_duration = quick ? 500000 : 2000000;
    set_mov_opts(&opts, playback);
    uint64_t t1 = now_ms();
    int err = avformat_open_input(&o->fmt, url, iformat, &opts);
    av_dict_free(&opts);
    if (o->fmt) apply_interrupt(o->fmt, o->fmt->pb, &o->io);
    dbg("ffstream: avformat_open_input (file:) end %llu ms rc=%d",
        (unsigned long long)(now_ms() - t1), err);
    if (err < 0) {
        log_averr("open_input file:", url, err);
        if (o->fmt) avformat_free_context(o->fmt);
        o->fmt = NULL;
        return err;
    }
    log_opened(o, "file:");
    return 0;
}

static int par_channels(const AVCodecParameters *p) {
    if (!p) return 0;
    if (p->ch_layout.nb_channels > 0) return p->ch_layout.nb_channels;
    return 0;
}

static int audio_par_ready(const AVCodecParameters *p) {
    return p && p->codec_id != AV_CODEC_ID_NONE && p->sample_rate > 0 && par_channels(p) > 0;
}

static int is_mov_like(const AVFormatContext *fmt, const char *path) {
    if (fmt && fmt->iformat && fmt->iformat->name && strstr(fmt->iformat->name, "mov")) return 1;
    const char *e = path_ext(path);
    if (!e || !e[0]) return 0;
    return !strcasecmp(e, ".m4b") || !strcasecmp(e, ".m4a") || !strcasecmp(e, ".mp4")
        || !strcasecmp(e, ".m4v") || !strcasecmp(e, ".mov") || !strcasecmp(e, ".3gp");
}

static void discard_other_streams(AVFormatContext *fmt, int keep) {
    if (!fmt) return;
    for (unsigned i = 0; i < fmt->nb_streams; i++) {
        if ((int)i == keep) continue;
        fmt->streams[i]->discard = AVDISCARD_ALL;
    }
}

/* FFmpeg 9's av_find_best_stream skips audio with sample_rate==0 or channels==0,
 * which is how flac/mp3 look after read_header, before find_stream_info. */
static int pick_audio_stream(AVFormatContext *fmt) {
    if (!fmt) return AVERROR(EINVAL);
    int ai = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (ai >= 0) return ai;
    for (unsigned i = 0; i < fmt->nb_streams; i++) {
        const AVCodecParameters *p = fmt->streams[i]->codecpar;
        if (p && p->codec_type == AVMEDIA_TYPE_AUDIO && p->codec_id != AV_CODEC_ID_NONE)
            return (int)i;
    }
    return AVERROR_STREAM_NOT_FOUND;
}

/* FFmpeg 9's flac demuxer stores STREAMINFO as extradata and sets only the time
 * base and duration; sample_rate / channels are left at 0 for the parser to fill
 * during avformat_find_stream_info. That made audio_par_ready() fail and sent
 * every FLAC open into find_stream_info (25.5 s on Vita for a long FLAC).
 * STREAMINFO (34 bytes) already has everything, so fill codecpar ourselves. */
int stream_parse_flac_streaminfo(const uint8_t *p, int len, int *rate, int *channels,
                                 int *bits, int64_t *samples) {
    if (!p || len < 34) return -1;
    if (len >= 42 && !memcmp(p, "fLaC", 4)) { p += 8; len -= 8; } /* "fLaC" + block header */
    if (len < 34) return -1;
    int sr = (p[10] << 12) | (p[11] << 4) | (p[12] >> 4);
    int ch = ((p[12] >> 1) & 7) + 1;
    int bps = (((p[12] & 1) << 4) | (p[13] >> 4)) + 1;
    int64_t ns = ((int64_t)(p[13] & 0x0F) << 32) | ((int64_t)p[14] << 24) | ((int64_t)p[15] << 16)
                 | ((int64_t)p[16] << 8) | (int64_t)p[17];
    if (sr <= 0 || sr > 655350) return -1;
    if (rate) *rate = sr;
    if (channels) *channels = ch;
    if (bits) *bits = bps;
    if (samples) *samples = ns;
    return 0;
}

/* Locate "fLaC" (after an optional ID3v2 tag) in the readahead window, which
 * holds the start of the file right after open, and return the STREAMINFO body. */
static const uint8_t *flac_header_from_window(const IoFile *f, int *len) {
    if (!f || !f->ra || f->ra_off != 0 || f->ra_len < 42) return NULL;
    const uint8_t *b = f->ra;
    int64_t off = 0;
    if (f->ra_len >= 10 && !memcmp(b, "ID3", 3)) {
        uint32_t sz = ((uint32_t)(b[6] & 0x7f) << 21) | ((uint32_t)(b[7] & 0x7f) << 14)
                    | ((uint32_t)(b[8] & 0x7f) << 7) | (uint32_t)(b[9] & 0x7f);
        off = 10 + (int64_t)sz + ((b[5] & 0x10) ? 10 : 0);
    }
    if (off + 42 > f->ra_len || memcmp(b + off, "fLaC", 4)) return NULL;
    if ((b[off + 4] & 0x7f) != 0) return NULL; /* STREAMINFO must be the first block */
    *len = 34;
    return b + off + 8;
}

static int fill_flac_par(AVStream *st, const IoFile *io) {
    AVCodecParameters *par = st->codecpar;
    const uint8_t *si = NULL;
    int si_len = 0;
    const char *from = "extradata";
    if (par->extradata && par->extradata_size >= 34) {
        si = par->extradata;
        si_len = par->extradata_size;
    } else {
        si = flac_header_from_window(io, &si_len);
        from = "file header";
    }
    int rate = 0, ch = 0, bits = 0;
    int64_t samples = 0;
    if (!si || stream_parse_flac_streaminfo(si, si_len, &rate, &ch, &bits, &samples) != 0) {
        dbg("ffstream: flac STREAMINFO not found (extradata=%d)", par->extradata_size);
        return -1;
    }
    if (!par->extradata || par->extradata_size < 34) {
        /* The decoder needs STREAMINFO too. */
        uint8_t *x = av_mallocz(34 + AV_INPUT_BUFFER_PADDING_SIZE);
        if (x) {
            memcpy(x, si, 34);
            av_freep(&par->extradata);
            par->extradata = x;
            par->extradata_size = 34;
        }
    }
    par->sample_rate = rate;
    av_channel_layout_uninit(&par->ch_layout);
    av_channel_layout_default(&par->ch_layout, ch);
    par->bits_per_raw_sample = bits;
    if (par->format < 0) par->format = bits <= 16 ? AV_SAMPLE_FMT_S16 : AV_SAMPLE_FMT_S32;
    if (st->duration <= 0 && samples > 0 && st->time_base.num > 0 && st->time_base.den > 0)
        st->duration = av_rescale_q(samples, (AVRational){1, rate}, st->time_base);
    dbg("ffstream: flac params from STREAMINFO (%s): %d Hz ch=%d bits=%d samples=%lld (%.1f s)",
        from, rate, ch, bits, (long long)samples, rate ? (double)samples / rate : 0.0);
    return 0;
}

static int fill_known_par(AVFormatContext *fmt, int ai, const IoFile *io) {
    if (!fmt || ai < 0) return -1;
    AVStream *st = fmt->streams[ai];
    if (st->codecpar->codec_id == AV_CODEC_ID_FLAC && !audio_par_ready(st->codecpar))
        return fill_flac_par(st, io);
    return audio_par_ready(st->codecpar) ? 0 : -1;
}

/* find_stream_info policy.
 *   play=1 (play path): never call it when the codec is known. FLAC params come
 *     from STREAMINFO; anything else still missing rate/channels is learned by
 *     opening the decoder and decoding the first frame (stream_open primes it).
 *     Only an unknown codec id falls through to a tight find_stream_info.
 *   play=0 (Refresh-time probe): as before (needed for CBR MP3 duration
 *     estimates), but FLAC is filled from STREAMINFO and skipped.
 *   force: last resort after the decoder failed to open. */
static volatile int g_find_info_calls;
int stream_find_info_calls(void) { return g_find_info_calls; }

static int maybe_find_stream_info_ex(Opened *o, int play, int force) {
    AVFormatContext *fmt = o->fmt;
    if (!fmt) return AVERROR(EINVAL);
    int ai = pick_audio_stream(fmt);
    if (ai >= 0) discard_other_streams(fmt, ai);
    if (ai >= 0 && !force) fill_known_par(fmt, ai, &o->io);
    const AVCodecParameters *par = (ai >= 0) ? fmt->streams[ai]->codecpar : NULL;
    int ready = audio_par_ready(par);
    int mov = is_mov_like(fmt, o->io.path);
    int have_codec = par && par->codec_id != AV_CODEC_ID_NONE;
    if (!force && (ready || (mov && have_codec) || (play && have_codec))) {
        dbg("ffstream: skip find_stream_info (codecpar %s, play=%d mov=%d, rate=%d ch=%d streams=%d format=%s)",
            ready ? "complete" : "codec-only: first frame decides", play, mov,
            par ? par->sample_rate : 0, par_channels(par), fmt->nb_streams,
            fmt->iformat && fmt->iformat->name ? fmt->iformat->name : "?");
        return 0;
    }
    uint64_t now = now_ms();
    if (o->io.deadline_ms && now + 250 >= o->io.deadline_ms) {
        dbg("ffstream: skip find_stream_info (deadline, audio=%d streams=%u)", ai, fmt->nb_streams);
        return 0;
    }
    apply_interrupt(fmt, o->avio ? o->avio : fmt->pb, &o->io);
    fmt->probesize = 128 * 1024;
    fmt->max_analyze_duration = 100000;
    uint64_t t1 = now_ms();
    dbg("ffstream: find_stream_info start (tight, play=%d force=%d audio=%d streams=%u format=%s codec=%d rate=%d ch=%d)",
        play, force, ai, fmt->nb_streams,
        fmt->iformat && fmt->iformat->name ? fmt->iformat->name : "?",
        par ? (int)par->codec_id : 0, par ? par->sample_rate : 0, par_channels(par));
    g_find_info_calls++;
    int err = avformat_find_stream_info(fmt, NULL);
    dbg("ffstream: find_stream_info end %llu ms rc=%d (read %llu bytes, %d seeks)%s",
        (unsigned long long)(now_ms() - t1), err,
        (unsigned long long)o->io.bytes_read, o->io.seeks,
        o->io.timed_out ? " TIMED OUT" : "");
    if (err < 0) log_averr("find_stream_info", o->io.path, err);
    return err;
}

static int maybe_find_stream_info(Opened *o) { return maybe_find_stream_info_ex(o, 0, 0); }

/* Sample rates a Vita BGM port accepts (psp2/audioout.h, sceAudioOutOpenPort). */
static const int k_port_rates[] = { 8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000 };

/* Native output (v00.31): the source rate when the port supports it, so swr only
 * converts the sample format / channels and never resamples. Otherwise the
 * nearest sensible supported rate: 88.2/176.4 kHz -> 44.1 kHz, anything above
 * 48 kHz (HE-AAC SBR at 64 kHz, 96 kHz) -> 48 kHz, odd low rates -> the next
 * supported rate up. */
int stream_pick_out_rate(int src) {
    int n = (int)(sizeof k_port_rates / sizeof *k_port_rates);
    for (int i = 0; i < n; i++)
        if (k_port_rates[i] == src) return src;
    if (src <= 0) return 48000;
    if (src % 44100 == 0) return 44100;
    if (src > 48000) return 48000;
    for (int i = 0; i < n; i++)
        if (k_port_rates[i] >= src) return k_port_rates[i];
    return 48000;
}

int stream_port_rate_supported(int rate) {
    int n = (int)(sizeof k_port_rates / sizeof *k_port_rates);
    for (int i = 0; i < n; i++)
        if (k_port_rates[i] == rate) return 1;
    return 0;
}

static int pick_out_rate(int src) { return stream_pick_out_rate(src); }

/* Configure swr from the actual decoded frame (HE-AAC SBR/PS can differ from codecpar). */
static int ensure_swr(Stream *s, const AVFrame *fr) {
    int in_rate = (fr && fr->sample_rate > 0) ? fr->sample_rate
                  : (s->dec->sample_rate > 0 ? s->dec->sample_rate : 44100);
    enum AVSampleFormat in_fmt = (fr && fr->format != AV_SAMPLE_FMT_NONE) ? fr->format : s->dec->sample_fmt;
    AVChannelLayout in;
    memset(&in, 0, sizeof in);
    if (fr && fr->ch_layout.nb_channels > 0)
        av_channel_layout_copy(&in, &fr->ch_layout);
    else if (s->dec->ch_layout.nb_channels > 0)
        av_channel_layout_copy(&in, &s->dec->ch_layout);
    else
        av_channel_layout_default(&in, 1);

    int same = s->swr && s->swr_in_rate == in_rate && s->swr_in_fmt == in_fmt
        && av_channel_layout_compare(&s->swr_in_layout, &in) == 0;
    if (same) { av_channel_layout_uninit(&in); return 0; }

    int out_rate = pick_out_rate(in_rate);
    swr_free(&s->swr);
    AVChannelLayout out = (AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO;
    int err = swr_alloc_set_opts2(&s->swr, &out, AV_SAMPLE_FMT_S16, out_rate,
                                  &in, in_fmt, in_rate, 0, NULL);
    if (err < 0 || !s->swr || swr_init(s->swr) < 0) {
        char buf[64];
        av_strerror(err, buf, sizeof buf);
        dbg("ffstream: swr init failed %s (%d) in=%d Hz fmt=%s ch=%d -> %d Hz stereo",
            buf, err, in_rate, av_get_sample_fmt_name(in_fmt) ? av_get_sample_fmt_name(in_fmt) : "?",
            in.nb_channels, out_rate);
        swr_free(&s->swr);
        av_channel_layout_uninit(&in);
        return -1;
    }
    dbg("ffstream: swr %d Hz %s ch=%d -> %d Hz stereo s16 (%s)",
        in_rate, av_get_sample_fmt_name(in_fmt) ? av_get_sample_fmt_name(in_fmt) : "?",
        in.nb_channels, out_rate,
        out_rate == in_rate ? "native rate, no resampling; format/channel conversion only" : "resampling: rate not supported by the port");
    s->out_rate = out_rate;
    s->swr_in_rate = in_rate;
    s->swr_in_fmt = in_fmt;
    av_channel_layout_uninit(&s->swr_in_layout);
    av_channel_layout_copy(&s->swr_in_layout, &in);
    av_channel_layout_uninit(&in);
    return 0;
}

static int open_codec(Stream *s) {
    const AVStream *st = s->fmt->streams[s->stream_index];
    const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) return STREAM_UNSUPPORTED;
    avcodec_free_context(&s->dec);
    s->dec = avcodec_alloc_context3(codec);
    if (!s->dec) return STREAM_ERR;
    if (avcodec_parameters_to_context(s->dec, st->codecpar) < 0) return STREAM_ERR;
    s->dec->pkt_timebase = st->time_base;
    int err = avcodec_open2(s->dec, codec, NULL);
    if (err < 0) {
        log_averr("avcodec_open2", s->io.path, err);
        return STREAM_UNSUPPORTED;
    }
    int src_rate = s->dec->sample_rate > 0 ? s->dec->sample_rate : 44100;
    s->out_rate = pick_out_rate(src_rate);
    dbg("ffstream: codec %s idx=%d profile=%d extra=%d rate=%d ch=%d fmt=%s",
        codec->name, s->stream_index, s->dec->profile, s->dec->extradata_size,
        s->dec->sample_rate, s->dec->ch_layout.nb_channels,
        av_get_sample_fmt_name(s->dec->sample_fmt) ? av_get_sample_fmt_name(s->dec->sample_fmt) : "?");
    /* swr is deferred until the first decoded frame so HE-AAC SBR/PS is correct. */
    return 0;
}

static int decode_one(Stream *s);

static void take_opened(Stream *s, Opened *o) {
    s->fmt = o->fmt;
    s->avio = o->avio;
    s->custom_io = o->custom_io;
    s->io = o->io;
    if (s->custom_io && s->avio) s->avio->opaque = &s->io;
    if (s->fmt) s->fmt->interrupt_callback.opaque = &s->io;
    s->io.cancel = &s->interrupted;
    o->fmt = NULL;
    o->avio = NULL;
#ifdef __vita__
    o->io.fd = -1;
#else
    o->io.fp = NULL;
#endif
    o->io.ra = NULL;
}

/* ---- lazy MP4 path ---------------------------------------------------------------- */

static int lazy_read_at(void *ud, int64_t off, void *buf, int n) {
    IoFile *f = ud;
    f->pos = off;
    int got = 0;
    while (got < n) {
        int r = io_read(f, (uint8_t *)buf + got, n - got);
        if (r <= 0) return got > 0 ? got : -1;
        got += r;
    }
    return got;
}

static int lazy_read_raw(void *ud, int64_t off, void *buf, int n) {
    IoFile *f = ud;
    if (io_should_stop(f) || f->timed_out) return -1;
    io_op_begin(f);
    if (io_raw_seek(f, off, SEEK_SET) != 0) { io_op_end(f, 0); return -1; }
    int got = 0;
    while (got < n) {
        if (got > 0 && io_interrupt(f)) { f->op_start_ms = 0; return got; }
        int r = io_raw_read(f, (uint8_t *)buf + got, n - got);
        if (r <= 0) { io_op_end(f, got > 0); return got > 0 ? got : -1; }
        got += r;
    }
    io_op_end(f, 1);
    f->bytes_read += (uint64_t)got;
    return got;
}

static int lazy_candidate(const char *path) {
    if (!g_mp4_lazy) return 0;
    const char *e = path_ext(path);
    return e && (!strcasecmp(e, ".m4b") || !strcasecmp(e, ".m4a") || !strcasecmp(e, ".mp4")
                 || !strcasecmp(e, ".mov"));
}

static const char *mp4c_name(int c) {
    return c == MP4C_AAC ? "aac" : c == MP4C_ALAC ? "alac" : c == MP4C_MP3 ? "mp3" : "?";
}

/* 0 ok: s->mp4, s->dec, s->pkt/frame ready. Otherwise everything is undone. */
static int lazy_open(Stream *s, const char *path) {
    uint64_t t0 = now_ms();
    if (io_open_file(&s->io, path) != 0) return -1;
    s->io.t0 = t0;
    s->io.cancel = &s->interrupted;
    s->io.deadline_span = STREAM_OPEN_TIMEOUT_MS;
    s->io.deadline_ms = io_now() + STREAM_OPEN_TIMEOUT_MS;
    Mp4Io mio = { &s->io, lazy_read_at, lazy_read_raw, s->io.size };
    int rc = mp4_open(&mio, 0, &s->mp4);
    uint64_t t_parse = now_ms();
    if (rc != 0) {
        dbg("ffstream: mp4lazy: cannot parse '%s' after %llu ms (read %llu B) -> FFmpeg mov path",
            path, (unsigned long long)(t_parse - t0), (unsigned long long)s->io.disk_bytes);
        goto fail;
    }
    const Mp4Track *tr = mp4_track(s->mp4);
    enum AVCodecID id = tr->codec == MP4C_AAC ? AV_CODEC_ID_AAC
                      : tr->codec == MP4C_ALAC ? AV_CODEC_ID_ALAC : AV_CODEC_ID_MP3;
    const AVCodec *codec = avcodec_find_decoder(id);
    if (!codec) goto fail;
    s->dec = avcodec_alloc_context3(codec);
    if (!s->dec) goto fail;
    if (tr->extradata_size > 0) {
        s->dec->extradata = av_mallocz((size_t)tr->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!s->dec->extradata) goto fail;
        memcpy(s->dec->extradata, tr->extradata, (size_t)tr->extradata_size);
        s->dec->extradata_size = tr->extradata_size;
    }
    if (tr->sample_rate > 0) s->dec->sample_rate = tr->sample_rate;
    if (tr->channels > 0) av_channel_layout_default(&s->dec->ch_layout, tr->channels);
    if (tr->codec == MP4C_ALAC && tr->bits > 0) s->dec->bits_per_coded_sample = tr->bits;
    s->dec->pkt_timebase = (AVRational){1, (int)tr->timescale};
    int err = avcodec_open2(s->dec, codec, NULL);
    if (err < 0) { log_averr("avcodec_open2 (mp4lazy)", path, err); goto fail; }
    s->pkt = av_packet_alloc();
    s->frame = av_frame_alloc();
    if (!s->pkt || !s->frame) goto fail;
    s->stream_index = 0;
    s->out_rate = pick_out_rate(s->dec->sample_rate > 0 ? s->dec->sample_rate : 44100);
    Mp4Stats st;
    mp4_stats(s->mp4, &st);
    dbg("ffstream: mp4lazy open '%s' %llu ms (parse %llu ms, %d reads, %llu B logical, disk %llu B / %d seeks; codec open %llu ms) "
        "%s %d Hz ch=%d extra=%d samples=%lld timescale=%u duration=%.1f s",
        path, (unsigned long long)(now_ms() - t0), (unsigned long long)st.parse_ms, st.reads,
        (unsigned long long)st.bytes_read, (unsigned long long)s->io.disk_bytes, s->io.disk_seeks,
        (unsigned long long)(now_ms() - t_parse), mp4c_name(tr->codec), s->dec->sample_rate,
        s->dec->ch_layout.nb_channels, tr->extradata_size, (long long)tr->nsamples,
        (unsigned)tr->timescale, tr->duration);
    if (tr->edit_start) dbg("ffstream: mp4lazy edit list start %lld ignored (as mov ignore_editlist), playable %lld",
                            (long long)tr->edit_start, (long long)tr->play_ts);
    return 0;
fail:
    avcodec_free_context(&s->dec);
    av_packet_free(&s->pkt);
    av_frame_free(&s->frame);
    mp4_close(s->mp4);
    s->mp4 = NULL;
    io_close_file(&s->io);
    memset(&s->io, 0, sizeof s->io);
#ifdef __vita__
    s->io.fd = -1;
#endif
    return -1;
}

Stream *stream_open(const TrackSource *src, int *err) {
    if (err) *err = STREAM_ERR;
    if (!src || !src->path[0]) return NULL;
    Stream *s = calloc(1, sizeof *s);
    if (!s) return NULL;
#ifdef __vita__
    s->io.fd = -1;
#endif
    s->stream_index = -1;
    if (lazy_candidate(src->path)) {
        set_open_progress(0.1f, "Opening book");
        if (lazy_open(s, src->path) == 0) {
            io_disarm_deadline(&s->io);
            set_open_progress(1.0f, "Opening book");
            dbg("ffstream: open ready '%s' -> %d Hz stereo in %llu ms (mp4lazy, no FFmpeg index)",
                src->path, s->out_rate, (unsigned long long)(now_ms() - s->io.t0));
            if (err) *err = 0;
            return s;
        }
        if (s->interrupted || app_quit_active()) {
            if (err) *err = STREAM_INTERRUPTED;
            stream_close(s);
            return NULL;
        }
    }
    Opened o;
    mutex_lock(&g_ff);
    int averr = opened_open(&o, src->path, 0, 1, STREAM_OPEN_TIMEOUT_MS, &s->interrupted);
    if (averr < 0) {
        if (err) *err = o.io.timed_out ? STREAM_INTERRUPTED : STREAM_IO;
        opened_close(&o);
        mutex_unlock(&g_ff);
        stream_close(s);
        return NULL;
    }
    uint64_t t_info = now_ms();
    averr = maybe_find_stream_info_ex(&o, 1, 0);
    uint64_t info_ms = now_ms() - t_info;
    if (averr < 0 && o.io.timed_out) {
        if (err) *err = STREAM_INTERRUPTED;
        opened_close(&o);
        mutex_unlock(&g_ff);
        stream_close(s);
        return NULL;
    }
    take_opened(s, &o);
    /* take_opened copied IoFile; keep interrupt pointing at the Stream-owned copy. */
    apply_interrupt(s->fmt, s->avio, &s->io);
    s->stream_index = pick_audio_stream(s->fmt);
    if (s->stream_index < 0) {
        if (err) *err = STREAM_UNSUPPORTED;
        mutex_unlock(&g_ff);
        stream_close(s);
        return NULL;
    }
    discard_other_streams(s->fmt, s->stream_index);
    int rc = open_codec(s);
    if (rc != 0) {
        /* Last resort: tight find_stream_info if codecpar was incomplete. */
        dbg("ffstream: codec open failed (%d), trying tight find_stream_info", rc);
        if (!s->io.timed_out) {
            Opened tmp = {0};
            tmp.fmt = s->fmt;
            tmp.avio = s->avio;
            tmp.io = s->io;
            tmp.custom_io = s->custom_io;
            if (maybe_find_stream_info_ex(&tmp, 1, 1) == 0) {
                avcodec_free_context(&s->dec);
                swr_free(&s->swr);
                rc = open_codec(s);
            }
            if (tmp.io.timed_out) s->io.timed_out = 1;
            apply_interrupt(s->fmt, s->avio, &s->io);
        }
        if (rc != 0) {
            if (err) *err = s->io.timed_out ? STREAM_INTERRUPTED : rc;
            mutex_unlock(&g_ff);
            stream_close(s);
            return NULL;
        }
    }
    s->pkt = av_packet_alloc();
    s->frame = av_frame_alloc();
    if (!s->pkt || !s->frame) {
        mutex_unlock(&g_ff);
        stream_close(s);
        return NULL;
    }
    /* Format still unknown (codec-only codecpar): learn it from the first decoded
     * frame instead of find_stream_info. The PCM stays queued for stream_read. */
    if (s->dec->sample_rate <= 0 || s->dec->ch_layout.nb_channels <= 0) {
        uint64_t tp = now_ms();
        int pr = decode_one(s);
        dbg("ffstream: primed first frame in %llu ms rc=%d -> %d Hz",
            (unsigned long long)(now_ms() - tp), pr, s->out_rate);
        if (pr < 0) {
            if (err) *err = s->io.timed_out ? STREAM_INTERRUPTED : pr;
            mutex_unlock(&g_ff);
            stream_close(s);
            return NULL;
        }
        if (pr == STREAM_EOF) s->eof = true;
    }
    io_disarm_deadline(&s->io);
    set_open_progress(1.0f, "Opening book");
    /* take_opened copied IoFile before disarm; keep the format callback on s->io. */
    apply_interrupt(s->fmt, s->avio, &s->io);
    dbg("ffstream: open ready '%s' -> %d Hz stereo in %llu ms (stream info %llu ms; read %llu bytes, %d seeks, deadline off)",
        src->path, s->out_rate, (unsigned long long)(now_ms() - s->io.t0), (unsigned long long)info_ms,
        (unsigned long long)s->io.bytes_read, s->io.seeks);
    mutex_unlock(&g_ff);
    if (err) *err = 0;
    return s;
}

static void clear_leftover(Stream *s) {
    s->leftover_off = s->leftover_frames = 0;
}

static int store_converted(Stream *s, const uint8_t **in, int in_samples) {
    if (!s->swr) return STREAM_ERR;
    int delay = swr_get_delay(s->swr, s->swr_in_rate > 0 ? s->swr_in_rate : s->out_rate);
    int out_samples = swr_get_out_samples(s->swr, in_samples) + 32;
    if (out_samples < delay + in_samples + 32) out_samples = delay + in_samples + 32;
    if (s->leftover_cap < out_samples) {
        int cap = out_samples < 2048 ? 2048 : out_samples;
        int16_t *nbuf = realloc(s->leftover, (size_t)cap * 2 * sizeof(int16_t));
        if (!nbuf) return STREAM_ERR;
        s->leftover = nbuf;
        s->leftover_cap = cap;
    }
    uint8_t *dst[] = { (uint8_t *)s->leftover };
    int n = swr_convert(s->swr, dst, s->leftover_cap, in, in_samples);
    if (n < 0) {
        log_averr("swr_convert", s->io.path, n);
        return STREAM_ERR;
    }
    s->leftover_frames = n;
    s->leftover_off = 0;
    return 0;
}

static int read_packet(Stream *s, AVPacket *pkt) {
    if (!s->mp4) return av_read_frame(s->fmt, pkt);
    for (;;) {
        int64_t off, dts;
        int size, dur;
        int r = mp4_next(s->mp4, &off, &size, &dts, &dur);
        if (r == 0) return AVERROR_EOF;
        if (r < 0) return (s->interrupted || s->io.timed_out || app_quit_active()) ? AVERROR_EXIT : AVERROR(EIO);
        if (size <= 0) continue;
        if (size > 8 * 1024 * 1024) return AVERROR_INVALIDDATA;
        int e = av_new_packet(pkt, size);
        if (e < 0) return e;
        if (lazy_read_at(&s->io, off, pkt->data, size) != size) {
            av_packet_unref(pkt);
            return (s->interrupted || s->io.timed_out || app_quit_active()) ? AVERROR_EXIT : AVERROR(EIO);
        }
        /* Emulate FFmpeg's mov demuxer (opened with ignore_editlist=1) so output is
         * sample-identical to the mov path: the last packet is trimmed to the track
         * duration via discard padding (demux.c first_discard_sample logic). */
        const Mp4Track *tr = mp4_track(s->mp4);
        int64_t pts = dts;
        int64_t skip = 0, disc = 0;
        if (mp4_tell(s->mp4) >= tr->nsamples) {
            int64_t fs = tr->codec == MP4C_AAC ? 1024 : tr->codec == MP4C_MP3 ? 1152 : 0;
            if (fs && tr->sample_rate > 0 && (int64_t)tr->timescale != tr->sample_rate)
                fs = av_rescale(fs, tr->timescale, tr->sample_rate);
            int64_t d = pts > tr->play_ts ? 0 : FFMIN((int64_t)dur, tr->play_ts - pts);
            int64_t first = pts + d, dd = FFMAX(fs, (int64_t)dur);
            if (dd > 0 && pts + dd >= first && pts < tr->play_ts) disc = FFMIN(pts + dd - first, dd);
        }
        if (skip || disc) {
            if (tr->sample_rate > 0 && (int64_t)tr->timescale != tr->sample_rate) {
                skip = av_rescale(skip, tr->sample_rate, tr->timescale);
                disc = av_rescale(disc, tr->sample_rate, tr->timescale);
            }
            uint8_t *p = av_packet_new_side_data(pkt, AV_PKT_DATA_SKIP_SAMPLES, 10);
            if (p) {
                AV_WL32(p, (uint32_t)skip);
                AV_WL32(p + 4, (uint32_t)disc);
            }
        }
        pkt->pts = pkt->dts = pts;
        pkt->duration = dur;
        pkt->pos = off;
        pkt->stream_index = 0;
        return 0;
    }
}

static int decode_one(Stream *s) {
    for (;;) {
        if (s->interrupted) return STREAM_INTERRUPTED;
        int ret = avcodec_receive_frame(s->dec, s->frame);
        if (ret == 0) {
            if (!s->logged_frame) {
                dbg("ffstream: first frame fmt=%s rate=%d ch=%d nb_samples=%d",
                    av_get_sample_fmt_name(s->frame->format) ? av_get_sample_fmt_name(s->frame->format) : "?",
                    s->frame->sample_rate, s->frame->ch_layout.nb_channels, s->frame->nb_samples);
                s->logged_frame = 1;
            }
            s->frames++;
            if (ensure_swr(s, s->frame) != 0) return STREAM_ERR;
            const uint8_t **in = (const uint8_t **)s->frame->extended_data;
            int rc = store_converted(s, in, s->frame->nb_samples);
            /* Success must not return 0 — STREAM_EOF is 0 and would end the book after one frame. */
            return rc < 0 ? rc : 1;
        }
        if (ret == AVERROR(EAGAIN)) {
            if (s->draining) return STREAM_EOF;
            int pr = read_packet(s, s->pkt);
            if (pr < 0) {
                if (pr == AVERROR_EOF) {
                    s->draining = true;
                    avcodec_send_packet(s->dec, NULL);
                    continue;
                }
                if (pr == AVERROR_EXIT || s->interrupted) return STREAM_INTERRUPTED;
                log_averr("av_read_frame", s->io.path, pr);
                return STREAM_IO;
            }
            if (!s->logged_pkt) {
                dbg("ffstream: first packet idx=%d (audio=%d) size=%d pts=%lld",
                    s->pkt->stream_index, s->stream_index, s->pkt->size, (long long)s->pkt->pts);
                s->logged_pkt = 1;
            }
            s->pkts++;
            if (s->pkt->stream_index != s->stream_index) {
                av_packet_unref(s->pkt);
                continue;
            }
            int sr = avcodec_send_packet(s->dec, s->pkt);
            av_packet_unref(s->pkt);
            if (sr < 0 && sr != AVERROR(EAGAIN)) {
                log_averr("avcodec_send_packet", s->io.path, sr);
                return STREAM_ERR;
            }
            continue;
        }
        if (ret == AVERROR_EOF) return STREAM_EOF;
        log_averr("avcodec_receive_frame", s->io.path, ret);
        return STREAM_ERR;
    }
}

int stream_read(Stream *s, int16_t *out, int max_frames, int *channels, int *rate) {
    if (!s || max_frames <= 0) return STREAM_ERR;
    if (channels) *channels = 2;
    if (rate) *rate = s->out_rate;
    int filled = 0;
    while (filled < max_frames) {
        if (s->leftover_off < s->leftover_frames) {
            int n = s->leftover_frames - s->leftover_off;
            if (n > max_frames - filled) n = max_frames - filled;
            memcpy(out + filled * 2, s->leftover + s->leftover_off * 2, (size_t)n * 2 * sizeof(int16_t));
            s->leftover_off += n;
            filled += n;
            s->samples_out += n;
            continue;
        }
        if (s->eof) return filled > 0 ? filled : STREAM_EOF;
        if (s->fail) return filled > 0 ? filled : s->fail;
        int rc = decode_one(s);
        if (rc == STREAM_EOF) {
            if (s->swr) store_converted(s, NULL, 0);
            s->eof = true;
            continue;
        }
        if (rc < 0) {
            if (rc != STREAM_INTERRUPTED) {
                if (!s->fail) dbg("ffstream: stream error %d mid-track on '%s' after %d samples", rc, s->io.path, s->samples_out);
                s->fail = rc;
            }
            return filled > 0 ? filled : rc;
        }
    }
    if (channels) *channels = 2;
    if (rate) *rate = s->out_rate;
    uint64_t now = now_ms();
    if (!s->last_stat_ms) s->last_stat_ms = now;
    if (now - s->last_stat_ms >= 5000) {
        dbg("ffstream: stats pkts=%d frames=%d samples_out=%d read=%llu seeks=%d",
            s->pkts, s->frames, s->samples_out,
            (unsigned long long)s->io.bytes_read, s->io.seeks);
        s->last_stat_ms = now;
    }
    return filled;
}

static int lazy_seek(Stream *s, double t, double *actual) {
    s->interrupted = 0;
    s->io.timed_out = 0;
    s->io.stalled = 0;
    io_touch(&s->io);
    const Mp4Track *tr = mp4_track(s->mp4);
    int64_t ts = av_rescale_q((int64_t)(t * 1000.0 + 0.5), (AVRational){1, 1000}, (AVRational){1, (int)tr->timescale});
    int64_t at = 0;
    uint64_t t0 = now_ms();
    if (mp4_seek(s->mp4, ts, &at) != 0) {
        dbg("ffstream: mp4lazy seek %.2f failed", t);
        return -1;
    }
    if (s->dec) avcodec_flush_buffers(s->dec);
    if (s->swr) { swr_close(s->swr); if (swr_init(s->swr) < 0) { swr_free(&s->swr); s->swr_in_rate = 0; } }
    clear_leftover(s);
    s->eof = s->draining = false;
    s->fail = 0;
    if (actual) *actual = t;
    dbg("ffstream: mp4lazy seek %.2f -> sample %lld (ts=%lld, sample dts=%lld) in %llu ms",
        t, (long long)mp4_tell(s->mp4), (long long)ts, (long long)at, (unsigned long long)(now_ms() - t0));
    return 0;
}

int stream_uses_mp4lazy(const Stream *s) { return s && s->mp4; }

int stream_seek(Stream *s, double t, double *actual) {
    if (s && s->mp4) return lazy_seek(s, t < 0 ? 0 : t, actual);
    if (!s || !s->fmt || s->stream_index < 0) return -1;
    if (t < 0) t = 0;
    /* A seek must not inherit a cancelled/stale interrupt from the command queue. */
    s->interrupted = 0;
    s->io.timed_out = 0;
    s->io.stalled = 0;
    io_touch(&s->io);
    const AVStream *st = s->fmt->streams[s->stream_index];
    int64_t ts = av_rescale_q((int64_t)(t * 1000.0 + 0.5), (AVRational){1, 1000}, st->time_base);
    int err = avformat_seek_file(s->fmt, s->stream_index, INT64_MIN, ts, ts, 0);
    if (err < 0)
        err = av_seek_frame(s->fmt, s->stream_index, ts, AVSEEK_FLAG_BACKWARD);
    if (err < 0)
        err = av_seek_frame(s->fmt, -1, (int64_t)(t * AV_TIME_BASE + 0.5), AVSEEK_FLAG_BACKWARD);
    if (err < 0) {
        log_averr("seek", s->io.path, err);
        return -1;
    }
    if (s->dec) avcodec_flush_buffers(s->dec);
    if (s->swr) { swr_close(s->swr); if (swr_init(s->swr) < 0) { swr_free(&s->swr); s->swr_in_rate = 0; } }
    clear_leftover(s);
    s->eof = s->draining = false;
    s->fail = 0;
    if (actual) *actual = t;
    dbg("ffstream: seek %.2f (ts=%lld) ok (read %llu, seeks=%d)",
        t, (long long)ts, (unsigned long long)s->io.bytes_read, s->io.seeks);
    return 0;
}

void stream_interrupt(Stream *s, int on) { if (s) s->interrupted = on; }
int stream_stalled(const Stream *s) { return s && s->io.stalled; }
int stream_slow_io(const Stream *s) { return s ? s->io.slow_ops : 0; }
int stream_net_state(Stream *s) { (void)s; return 0; }
void stream_table_stats(Stream *s, int *requests, uint64_t *bytes) {
    if (requests) *requests = s ? s->pkts : 0;
    if (bytes) *bytes = s ? s->io.bytes_read : 0;
}

void stream_io_info(const Stream *s, uint64_t *bytes_read, int *seeks, int *deadline_armed) {
    if (bytes_read) *bytes_read = s ? s->io.bytes_read : 0;
    if (seeks) *seeks = s ? s->io.seeks : 0;
    if (deadline_armed) *deadline_armed = s && s->io.deadline_ms ? 1 : 0;
}

void stream_close(Stream *s) {
    if (!s) return;
    mp4_close(s->mp4);
    s->mp4 = NULL;
    swr_free(&s->swr);
    av_channel_layout_uninit(&s->swr_in_layout);
    avcodec_free_context(&s->dec);
    av_packet_free(&s->pkt);
    av_frame_free(&s->frame);
    if (s->fmt) {
        if (s->custom_io) s->fmt->pb = NULL;
        avformat_close_input(&s->fmt);
    }
    if (s->custom_io) io_free_avio(&s->avio);
    io_close_file(&s->io);
    free(s->leftover);
    free(s);
}

static void copy_tag(AVDictionary *md, const char *key, char *dst, size_t n) {
    if (!md || dst[0]) return;
    const AVDictionaryEntry *e = av_dict_get(md, key, NULL, 0);
    if (e && e->value) str_copy(dst, n, e->value);
}

/* 00.38: the book description. 'comment' first (ID3 COMM, MP4 \xa9cmt, Vorbis
 * COMMENT, ID3v1), then a COMM frame with a descriptor ("Comment", "ID3v1
 * Comment"; FFmpeg keys those by the descriptor, never iTunes' iTunNORM etc.),
 * then 'synopsis' (MP4 ldes, the long description; also Vorbis/TXXX SYNOPSIS),
 * 'description' (MP4 desc, Vorbis DESCRIPTION, TXXX) and ID3 TDES. Keys match
 * case-insensitively. Stream tags (Ogg) before container tags at each step. */
static const char *pick_desc_dict(AVDictionary *md, int pass) {
    if (!md) return NULL;
    const AVDictionaryEntry *e = NULL;
    if (pass == 0) {
        e = av_dict_get(md, "comment", NULL, 0);
        if (e && e->value && e->value[0]) return e->value;
        e = NULL;
        while ((e = av_dict_get(md, "", e, AV_DICT_IGNORE_SUFFIX))) {
            if (!e->key || !e->value || !e->value[0] || !strncasecmp(e->key, "itun", 4)) continue;
            if (str_icontains(e->key, "comment")) return e->value;
        }
        return NULL;
    }
    static const char *const keys[] = {"synopsis", "long_description", "description", "TDES"};
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        e = av_dict_get(md, keys[i], NULL, 0);
        if (e && e->value && e->value[0]) return e->value;
    }
    return NULL;
}

static char *pick_description(AVDictionary *a, AVDictionary *b) {
    for (int pass = 0; pass < 2; pass++) {
        AVDictionary *mds[2] = {a, b};
        for (int k = 0; k < 2; k++) {
            const char *v = pick_desc_dict(mds[k], pass);
            if (!v) continue;
            char *d = str_clean_description(v, DESC_MAX_BYTES);
            if (d) return d;
        }
    }
    return NULL;
}

static int b64_digit(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static uint8_t *b64_decode(const char *s, size_t *out_len) {
    if (!s || !out_len) return NULL;
    size_t n = 0, cap = 0;
    uint8_t *out = NULL;
    int val = 0, valb = -8;
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '=' || c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
        int d = b64_digit((char)c);
        if (d < 0) continue;
        val = (val << 6) + d;
        valb += 6;
        if (valb >= 0) {
            if (n == cap) {
                cap = cap ? cap * 2 : 256;
                uint8_t *neo = realloc(out, cap);
                if (!neo) { free(out); *out_len = 0; return NULL; }
                out = neo;
            }
            out[n++] = (uint8_t)((val >> valb) & 0xFF);
            valb -= 8;
        }
    }
    *out_len = n;
    return out;
}

static unsigned pic_rb32(const uint8_t *p) {
    return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) | ((unsigned)p[2] << 8) | (unsigned)p[3];
}

/* FLAC PICTURE / Ogg METADATA_BLOCK_PICTURE payload. Prefer type 3 (front). */
static int parse_flac_picture(const uint8_t *buf, size_t len, MediaInfo *out, int *ptype) {
    if (!buf || len < 32 || !out) return -1;
    size_t o = 0;
    unsigned type = pic_rb32(buf + o); o += 4;
    unsigned mime_len = pic_rb32(buf + o); o += 4;
    if (o + mime_len + 4 > len) return -1;
    o += mime_len;
    unsigned desc_len = pic_rb32(buf + o); o += 4;
    if (o + desc_len + 20 > len) return -1;
    o += desc_len + 16; /* width, height, depth, colors */
    unsigned dlen = pic_rb32(buf + o); o += 4;
    if (o + dlen > len || dlen < 8) return -1;
    uint8_t *img = malloc(dlen);
    if (!img) return -1;
    memcpy(img, buf + o, dlen);
    free(out->cover);
    out->cover = img;
    out->cover_len = dlen;
    out->has_cover = 1;
    if (ptype) *ptype = (int)type;
    return 0;
}

static int try_vorbis_pictures(AVFormatContext *fmt, MediaInfo *out) {
    if (!fmt || !out || out->cover) return 0;
    int best_type = 99;
    AVDictionary *dicts[8];
    int nd = 0;
    if (fmt->metadata && nd < 8) dicts[nd++] = fmt->metadata;
    for (unsigned i = 0; i < fmt->nb_streams && nd < 8; i++)
        if (fmt->streams[i]->metadata) dicts[nd++] = fmt->streams[i]->metadata;
    for (int d = 0; d < nd; d++) {
        const AVDictionaryEntry *e = NULL;
        while ((e = av_dict_get(dicts[d], "METADATA_BLOCK_PICTURE", e, AV_DICT_IGNORE_SUFFIX))) {
            if (!e->value) continue;
            size_t blen = 0;
            uint8_t *raw = b64_decode(e->value, &blen);
            if (!raw) continue;
            int typ = -1;
            MediaInfo tmp;
            memset(&tmp, 0, sizeof tmp);
            if (parse_flac_picture(raw, blen, &tmp, &typ) == 0) {
                int rank = (typ == 3) ? 0 : typ;
                if (!out->cover || rank < best_type) {
                    free(out->cover);
                    out->cover = tmp.cover;
                    out->cover_len = tmp.cover_len;
                    out->has_cover = 1;
                    best_type = rank;
                    tmp.cover = NULL;
                } else {
                    free(tmp.cover);
                }
            }
            free(raw);
        }
    }
    if (out->cover) dbg("ffstream: vorbis/flac picture %u bytes type-rank=%d", (unsigned)out->cover_len, best_type);
    return out->cover ? 0 : -1;
}

/* Refresh-time probe through the lazy MP4 reader: tags, covr, chapters,
 * duration without FFmpeg building a sample index. 0 ok. */
static int lazy_probe(const char *path, MediaInfo *out, uint64_t timeout_ms) {
    uint64_t t0 = now_ms();
    IoFile io;
    if (io_open_file(&io, path) != 0) return -1;
    io.t0 = t0;
    if (timeout_ms) { io.deadline_span = timeout_ms; io.deadline_ms = io_now() + timeout_ms; }
    Mp4Io mio = { &io, lazy_read_at, lazy_read_raw, io.size };
    Mp4 *m = NULL;
    int rc = mp4_open(&mio, 1, &m);
    if (rc != 0) {
        dbg("ffstream: mp4lazy probe: cannot parse '%s' (%llu ms) -> FFmpeg", path,
            (unsigned long long)(now_ms() - t0));
        io_close_file(&io);
        return -1;
    }
    const Mp4Track *tr = mp4_track(m);
    const Mp4Meta *md = mp4_meta(m);
    out->duration = tr->duration;
    if (md) {
        str_copy(out->title, sizeof out->title, md->title);
        str_copy(out->artist, sizeof out->artist, md->artist);
        str_copy(out->album, sizeof out->album, md->album);
        /* 00.38: same order as the FFmpeg path: comment, then long description, then description. */
        const char *dsrc[3] = {md->comment, md->ldes, md->desc};
        for (int k = 0; k < 3 && !out->description; k++) out->description = str_clean_description(dsrc[k], DESC_MAX_BYTES);
        if (md->nchapters > 0) {
            out->chapters = calloc((size_t)md->nchapters, sizeof(Chapter));
            if (out->chapters) {
                out->nchapters = md->nchapters;
                for (int i = 0; i < md->nchapters; i++) {
                    out->chapters[i].start = md->chapters[i].start;
                    out->chapters[i].end = md->chapters[i].end;
                    str_copy(out->chapters[i].title, sizeof out->chapters[i].title, md->chapters[i].title);
                }
            }
        }
        if (md->cover_off) {
            out->has_cover = 1;
            uint8_t *c = NULL;
            size_t cl = 0;
            if (mp4_read_cover(m, &c, &cl) == 0) { out->cover = c; out->cover_len = cl; }
        }
    }
    Mp4Stats st;
    mp4_stats(m, &st);
    dbg("ffstream: mp4lazy probe '%s' %llu ms (parse %llu ms, %d reads, disk %llu B) %s duration=%.1f chapters=%d cover=%s/%u desc=%u",
        path, (unsigned long long)(now_ms() - t0), (unsigned long long)st.parse_ms, st.reads,
        (unsigned long long)io.disk_bytes, mp4c_name(tr->codec), out->duration, out->nchapters,
        out->has_cover ? "yes" : "no", (unsigned)out->cover_len,
        out->description ? (unsigned)strlen(out->description) : 0u);
    mp4_close(m);
    io_close_file(&io);
    return 0;
}

int stream_probe_ex(const char *path, MediaInfo *out, int flags, uint64_t timeout_ms) {
    memset(out, 0, sizeof *out);
    if (!path || !path[0]) return -1;
    if (io_should_stop(NULL)) {
        dbg("ffstream: probe of '%s' cancelled (shutting down)", path);
        return -1;
    }
    if (lazy_candidate(path)) {
        if (lazy_probe(path, out, timeout_ms) == 0) return 0;
        media_info_free(out);
        memset(out, 0, sizeof *out);
    }
    int quick = (flags & STREAM_PROBE_QUICK) != 0;
    uint64_t t0 = now_ms();
    dbg("ffstream: probe start '%s' quick=%d timeout=%llu",
        path, quick, (unsigned long long)timeout_ms);
    Opened o;
    mutex_lock(&g_ff);
    int err = opened_open(&o, path, quick, 0, timeout_ms, NULL);
    if (err < 0) {
        if (io_should_stop(NULL)) dbg("ffstream: probe cancelled after %llu ms", (unsigned long long)(now_ms() - t0));
        else dbg("ffstream: probe open failed after %llu ms", (unsigned long long)(now_ms() - t0));
        opened_close(&o);
        mutex_unlock(&g_ff);
        return -1;
    }
    err = maybe_find_stream_info(&o);
    if (err < 0 && !o.fmt) {
        opened_close(&o);
        mutex_unlock(&g_ff);
        return -1;
    }
    /* Even if find_stream_info failed, keep tags/duration already parsed from moov. */
    AVFormatContext *fmt = o.fmt;
    if (fmt && fmt->duration > 0) out->duration = (double)fmt->duration / AV_TIME_BASE;
    int ai = fmt ? pick_audio_stream(fmt) : -1;
    if (ai >= 0) {
        const AVStream *st = fmt->streams[ai];
        if (out->duration <= 0 && st->duration > 0)
            out->duration = st->duration * av_q2d(st->time_base);
        copy_tag(st->metadata, "title", out->title, sizeof out->title);
        copy_tag(st->metadata, "artist", out->artist, sizeof out->artist);
        copy_tag(st->metadata, "album_artist", out->artist, sizeof out->artist);
        copy_tag(st->metadata, "album", out->album, sizeof out->album);
    }
    if (fmt) {
        copy_tag(fmt->metadata, "title", out->title, sizeof out->title);
        copy_tag(fmt->metadata, "artist", out->artist, sizeof out->artist);
        if (!out->artist[0]) copy_tag(fmt->metadata, "album_artist", out->artist, sizeof out->artist);
        copy_tag(fmt->metadata, "album", out->album, sizeof out->album);
        out->description = pick_description(ai >= 0 ? fmt->streams[ai]->metadata : NULL, fmt->metadata);

        if (fmt->nb_chapters > 0) {
            out->chapters = calloc(fmt->nb_chapters, sizeof(Chapter));
            if (out->chapters) {
                out->nchapters = (int)fmt->nb_chapters;
                for (int i = 0; i < out->nchapters; i++) {
                    const AVChapter *ch = fmt->chapters[i];
                    out->chapters[i].start = ch->start * av_q2d(ch->time_base);
                    out->chapters[i].end = ch->end * av_q2d(ch->time_base);
                    const AVDictionaryEntry *t = av_dict_get(ch->metadata, "title", NULL, 0);
                    if (t && t->value) str_copy(out->chapters[i].title, sizeof out->chapters[i].title, t->value);
                    else snprintf(out->chapters[i].title, sizeof out->chapters[i].title, "Chapter %d", i + 1);
                }
            }
        }

        for (unsigned i = 0; i < fmt->nb_streams; i++) {
            const AVStream *st = fmt->streams[i];
            if (!(st->disposition & AV_DISPOSITION_ATTACHED_PIC)) continue;
            out->has_cover = 1;
            /* Packet is already in memory after moov parse; copy even on a quick probe. */
            const AVPacket *p = &st->attached_pic;
            if (p->size <= 0 || !p->data) break;
            out->cover = malloc((size_t)p->size);
            if (out->cover) {
                memcpy(out->cover, p->data, (size_t)p->size);
                out->cover_len = (size_t)p->size;
            }
            break;
        }
        if (!out->cover) try_vorbis_pictures(fmt, out);
    }

    int timed = o.io.timed_out;
    int had_fmt = fmt != NULL;
    dbg("ffstream: probe done '%s' %llu ms duration=%.1f chapters=%d cover=%s/%u desc=%u close (read %llu, seeks=%d)%s",
        path, (unsigned long long)(now_ms() - t0), out->duration, out->nchapters,
        out->has_cover ? "yes" : "no", (unsigned)out->cover_len,
        out->description ? (unsigned)strlen(out->description) : 0u,
        (unsigned long long)o.io.bytes_read, o.io.seeks,
        timed ? " TIMED OUT" : "");
    opened_close(&o);
    mutex_unlock(&g_ff);
    /* A timeout after we already have a title or duration is still useful. */
    if (out->duration > 0 || out->title[0] || out->nchapters > 0) return 0;
    return (had_fmt && !timed) ? 0 : -1;
}

int stream_probe(const char *path, MediaInfo *out) {
    return stream_probe_ex(path, out, 0, STREAM_PROBE_TIMEOUT_MS);
}

void media_info_free(MediaInfo *m) {
    if (!m) return;
    free(m->chapters);
    free(m->cover);
    free(m->description);
    m->description = NULL;
    m->chapters = NULL;
    m->cover = NULL;
    m->nchapters = 0;
    m->cover_len = 0;
    m->has_cover = 0;
}
