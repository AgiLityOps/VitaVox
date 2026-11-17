/* Lightweight lazy MP4 / M4B demuxer.
 *
 * FFmpeg's mov demuxer builds an AVIndexEntry for every sample at open, which
 * costs seconds of CPU on a Vita for long audiobooks (a long M4B: ~10 s). This
 * reader keeps the sample tables (stts/stsc/stsz/stco/co64) on the card and
 * looks up only what it needs: small tables are loaded, big ones (stsz, stco)
 * are read through windows on demand. Packets are handed to FFmpeg's decoders
 * through avcodec only. Callers fall back to FFmpeg's mov path on any failure.
 *
 * Pure C, no FFmpeg dependency (host-testable). */
#ifndef VITAVOX_MP4LAZY_H
#define VITAVOX_MP4LAZY_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
    void *ud;
    /* Reads exactly n bytes at off (may go through a readahead cache). n or <0. */
    int (*read_at)(void *ud, int64_t off, void *buf, int n);
    /* Uncached read for table windows (optional; read_at is used if NULL). */
    int (*read_raw)(void *ud, int64_t off, void *buf, int n);
    int64_t size;
} Mp4Io;

enum { MP4C_NONE = 0, MP4C_AAC, MP4C_ALAC, MP4C_MP3 };

typedef struct {
    int codec;              /* MP4C_* */
    int sample_rate;        /* from the sample entry (AAC: core rate) */
    int channels;
    int bits;
    uint8_t *extradata;     /* AAC: AudioSpecificConfig; ALAC: the 36-byte 'alac' atom */
    int extradata_size;
    uint32_t timescale;     /* mdhd */
    int64_t nsamples;       /* packets */
    int64_t duration_ts;    /* mdhd duration (or stts total), timescale units */
    double duration;        /* seconds */
    uint32_t track_id;
    int64_t edit_start;     /* elst media time (priming), informational: not applied (ignore_editlist) */
    int64_t play_ts;        /* playable length used for last-packet trimming (= duration_ts) */
} Mp4Track;

typedef struct {
    double start, end;      /* seconds */
    char title[96];
} Mp4Chapter;

typedef struct {
    char title[160];
    char artist[96];        /* ©ART, else aART */
    char album[96];
    int nchapters;
    Mp4Chapter *chapters;   /* malloc'd */
    int64_t cover_off;      /* covr image payload (0 = none) */
    int cover_len;
    /* 00.38: description sources, raw UTF-8 (malloc'd, may be NULL) */
    char *comment;          /* \xa9cmt */
    char *ldes;             /* long description */
    char *desc;             /* description */
} Mp4Meta;

typedef struct {
    uint64_t parse_ms, bytes_read;
    int reads;
} Mp4Stats;

typedef struct Mp4 Mp4;

/* Parses the moov box structure and the audio track's small tables.
 * want_meta: also tags, covr location and chapters (chpl / QT chapter track).
 * 0 ok; <0 unsupported/invalid (caller falls back to FFmpeg). */
int mp4_open(const Mp4Io *io, int want_meta, Mp4 **out);
const Mp4Track *mp4_track(const Mp4 *m);
const Mp4Meta *mp4_meta(const Mp4 *m);   /* NULL unless want_meta */
void mp4_stats(const Mp4 *m, Mp4Stats *st);
/* Next packet in decode order: file offset + size, dts and duration (timescale).
 * 1 ok, 0 end of track, <0 error. */
int mp4_next(Mp4 *m, int64_t *off, int *size, int64_t *dts, int *dur);
/* Position at the sample whose time span contains ts (timescale units; like
 * FFmpeg's backward index search). *actual gets that sample's dts. 0 ok. */
int mp4_seek(Mp4 *m, int64_t ts, int64_t *actual);
/* Sample index the next mp4_next will return. */
int64_t mp4_tell(const Mp4 *m);
void mp4_close(Mp4 *m);

/* Read the covr image (malloc'd). 0 ok. */
int mp4_read_cover(Mp4 *m, uint8_t **out, size_t *len);

#endif
