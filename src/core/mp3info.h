/* Fast MP3 duration without FFmpeg (00.36): skip the ID3v2 tag, read the first
 * frame header and its Xing/Info or VBRI header (exact frame count), else
 * estimate from the bitrate and the file size like FFmpeg's mp3 demuxer does
 * for CBR files. Reads ~4 KB per file instead of the 1 MB a quick probe reads,
 * so a 354-file book is a few seconds of I/O, not a minute. */
#ifndef VITAVOX_MP3INFO_H
#define VITAVOX_MP3INFO_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int version;        /* 1 = MPEG-1, 2 = MPEG-2, 25 = MPEG-2.5 */
    int layer;          /* 1..3 */
    int bitrate;        /* bits/s of the first frame */
    int sample_rate;
    int channels;
    int samples_per_frame;
    int frame_len;      /* bytes, padding included */
} Mp3Frame;

typedef struct {
    double duration;    /* seconds */
    int exact;          /* 1: from a Xing/Info/VBRI frame count, 0: CBR estimate */
    uint64_t audio_offset; /* first frame (after the ID3v2 tag) */
    Mp3Frame first;
} Mp3Info;

/* Size of an ID3v2 tag from its 10-byte header (footer included), 0 if none. */
uint64_t mp3_id3v2_size(const uint8_t *hdr, size_t n);
/* Parses a 4-byte frame header. 0 ok. */
int mp3_parse_frame(const uint8_t *p, Mp3Frame *out);
/* Duration from a buffer that starts at (or a little before) the first frame.
 * buf_offset is the file offset of buf[0]; file_size the whole file. 0 ok. */
int mp3_info_from_buffer(const uint8_t *buf, size_t n, uint64_t buf_offset, uint64_t file_size, Mp3Info *out);
/* Opens the file and reads ~4 KB. 0 ok, -1 not an MP3 we can time (use FFmpeg). */
int mp3_fast_info(const char *path, Mp3Info *out);

/* 00.37: the background scan's read. Like mp3_fast_info, plus checks that the
 * result can be trusted (as good as FFmpeg's own): the first frame chains to
 * the next ones; a frame-count header (Xing/Info/VBRI) gives a plausible length
 * and average bitrate; without one, the CBR estimate is only used when the
 * frames at the start and at 1/3 and 2/3 of the file all share one bitrate.
 * 0 ok; -1 with *why (MP3_WHY_*) when the caller must probe with FFmpeg. */
enum {
    MP3_OK = 0,
    MP3_WHY_IO = 1,
    MP3_WHY_NOSYNC,
    MP3_WHY_FRAMES,
    MP3_WHY_DURATION,
    MP3_WHY_VBR_NO_TOC,
    MP3_WHY_SAMPLE,
};
int mp3_fast_info_checked(const char *path, Mp3Info *out, int *why);
const char *mp3_why_str(int why);

#endif
