#include "mp3info.h"

#include <stdio.h>
#include <string.h>

#ifdef __vita__
#include <psp2/io/fcntl.h>
#endif

#define MP3_SCAN 4096

uint64_t mp3_id3v2_size(const uint8_t *h, size_t n) {
    if (!h || n < 10 || h[0] != 'I' || h[1] != 'D' || h[2] != '3') return 0;
    if (h[3] == 0xFF || h[4] == 0xFF) return 0;
    if ((h[6] | h[7] | h[8] | h[9]) & 0x80) return 0; /* not syncsafe */
    uint64_t sz = ((uint64_t)h[6] << 21) | ((uint64_t)h[7] << 14) | ((uint64_t)h[8] << 7) | h[9];
    sz += 10;
    if (h[5] & 0x10) sz += 10; /* footer */
    return sz;
}

int mp3_parse_frame(const uint8_t *p, Mp3Frame *out) {
    static const int br_v1[3][16] = {
        {0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448, 0},
        {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 0},
        {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0},
    };
    static const int br_v2[3][16] = {
        {0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256, 0},
        {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0},
        {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0},
    };
    static const int sr[3] = {44100, 48000, 32000};
    if (!p || p[0] != 0xFF || (p[1] & 0xE0) != 0xE0) return -1;
    int ver_bits = (p[1] >> 3) & 3;    /* 0 = 2.5, 1 = reserved, 2 = 2, 3 = 1 */
    int layer_bits = (p[1] >> 1) & 3;  /* 1 = III, 2 = II, 3 = I */
    int br_idx = (p[2] >> 4) & 15;
    int sr_idx = (p[2] >> 2) & 3;
    int pad = (p[2] >> 1) & 1;
    int mode = (p[3] >> 6) & 3;
    if (ver_bits == 1 || layer_bits == 0 || br_idx == 0 || br_idx == 15 || sr_idx == 3) return -1;
    Mp3Frame f;
    memset(&f, 0, sizeof f);
    f.version = ver_bits == 3 ? 1 : ver_bits == 2 ? 2 : 25;
    f.layer = 4 - layer_bits;
    int kbps = f.version == 1 ? br_v1[f.layer - 1][br_idx] : br_v2[f.layer - 1][br_idx];
    f.bitrate = kbps * 1000;
    f.sample_rate = sr[sr_idx] >> (f.version == 1 ? 0 : f.version == 2 ? 1 : 2);
    f.channels = mode == 3 ? 1 : 2;
    if (f.layer == 1) {
        f.samples_per_frame = 384;
        f.frame_len = (12 * f.bitrate / f.sample_rate + pad) * 4;
    } else if (f.layer == 2 || f.version == 1) {
        f.samples_per_frame = 1152;
        f.frame_len = 144 * f.bitrate / f.sample_rate + pad;
    } else {
        f.samples_per_frame = 576;
        f.frame_len = 72 * f.bitrate / f.sample_rate + pad;
    }
    if (f.frame_len < 21 || f.bitrate <= 0 || f.sample_rate <= 0) return -1;
    if (out) *out = f;
    return 0;
}

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

int mp3_info_from_buffer(const uint8_t *buf, size_t n, uint64_t buf_offset, uint64_t file_size, Mp3Info *out) {
    if (!buf || !out || n < 4) return -1;
    memset(out, 0, sizeof *out);
    for (size_t i = 0; i + 4 <= n; i++) {
        if (buf[i] != 0xFF || (buf[i + 1] & 0xE0) != 0xE0) continue;
        Mp3Frame f;
        if (mp3_parse_frame(buf + i, &f) != 0) continue;
        /* Confirm with the next frame header when it is inside the buffer (a stray
         * 0xFFE in tag padding or art must not pass for audio). */
        size_t nx = i + (size_t)f.frame_len;
        if (nx + 4 <= n) {
            Mp3Frame g;
            if (mp3_parse_frame(buf + nx, &g) != 0 || g.sample_rate != f.sample_rate || g.layer != f.layer
                || g.version != f.version)
                continue;
        } else if (i > 0) {
            continue; /* cannot confirm a sync found deep in the buffer */
        }
        out->first = f;
        out->audio_offset = buf_offset + i;
        /* Xing / Info (LAME) */
        if (f.layer == 3) {
            size_t side = f.version == 1 ? (f.channels == 1 ? 17 : 32) : (f.channels == 1 ? 9 : 17);
            size_t x = i + 4 + side;
            if (x + 12 <= n && (!memcmp(buf + x, "Xing", 4) || !memcmp(buf + x, "Info", 4))) {
                uint32_t flags = be32(buf + x + 4);
                if (flags & 1) {
                    uint32_t frames = be32(buf + x + 8);
                    if (frames > 0) {
                        double samples = (double)frames * f.samples_per_frame;
                        /* LAME/Lavc tag after the optional fields: encoder delay and padding
                         * (12 bits each) at +21. FFmpeg leaves them out of the duration. */
                        size_t l = x + 8 + 4 + ((flags & 2) ? 4 : 0) + ((flags & 4) ? 100 : 0) + ((flags & 8) ? 4 : 0);
                        if (l + 24 <= n && (!memcmp(buf + l, "LAME", 4) || !memcmp(buf + l, "Lavf", 4)
                                            || !memcmp(buf + l, "Lavc", 4))) {
                            uint32_t dl = ((uint32_t)buf[l + 21] << 16) | ((uint32_t)buf[l + 22] << 8) | buf[l + 23];
                            double pad = (double)(dl >> 12) + (double)(dl & 0xFFF);
                            if (pad < samples) samples -= pad;
                        }
                        out->duration = samples / f.sample_rate;
                        out->exact = 1;
                        return 0;
                    }
                }
            }
            size_t v = i + 4 + 32;
            if (v + 18 <= n && !memcmp(buf + v, "VBRI", 4)) {
                uint32_t frames = be32(buf + v + 14);
                if (frames > 0) {
                    out->duration = (double)frames * f.samples_per_frame / f.sample_rate;
                    out->exact = 1;
                    return 0;
                }
            }
        }
        /* CBR: audio bytes over the first frame's bitrate. */
        if (file_size <= out->audio_offset) return -1;
        out->duration = (double)(file_size - out->audio_offset) * 8.0 / f.bitrate;
        out->exact = 0;
        return 0;
    }
    return -1;
}

/* ---- file access (sceIo on the Vita, stdio on the host) ---- */
typedef struct {
#ifdef __vita__
    SceUID fd;
#else
    FILE *f;
#endif
    uint64_t size;
} MFile;

static int mf_open(MFile *m, const char *path) {
    memset(m, 0, sizeof *m);
#ifdef __vita__
    m->fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (m->fd < 0) return -1;
    SceOff end = sceIoLseek(m->fd, 0, SCE_SEEK_END);
    if (end <= 0) { sceIoClose(m->fd); return -1; }
    m->size = (uint64_t)end;
#else
    m->f = fopen(path, "rb");
    if (!m->f) return -1;
    if (fseek(m->f, 0, SEEK_END) != 0) { fclose(m->f); return -1; }
    long end = ftell(m->f);
    if (end <= 0) { fclose(m->f); return -1; }
    m->size = (uint64_t)end;
#endif
    return 0;
}

static int mf_read(MFile *m, uint64_t off, uint8_t *buf, int n) {
    if (off >= m->size) return 0;
#ifdef __vita__
    if (sceIoLseek(m->fd, (SceOff)off, SCE_SEEK_SET) < 0) return -1;
    return sceIoRead(m->fd, buf, (SceSize)n);
#else
    if (fseek(m->f, (long)off, SEEK_SET) != 0) return -1;
    return (int)fread(buf, 1, (size_t)n, m->f);
#endif
}

static void mf_close(MFile *m) {
#ifdef __vita__
    if (m->fd >= 0) sceIoClose(m->fd);
#else
    if (m->f) fclose(m->f);
#endif
}

/* Reads the head of the file (past a big ID3v2 tag). *off = file offset of buf[0]. */
static int read_head(MFile *m, uint8_t *buf, int *n, uint64_t *off) {
    *off = 0;
    *n = mf_read(m, 0, buf, MP3_SCAN);
    if (*n < 10) return -1;
    uint64_t tag = mp3_id3v2_size(buf, (size_t)*n);
    if (tag > 0) {
        if (tag >= m->size) return -1;
        if (tag + 64 > (uint64_t)*n) {
            *off = tag;
            *n = mf_read(m, tag, buf, MP3_SCAN);
        }
    }
    return *n >= 4 ? 0 : -1;
}

static int parse_head(const uint8_t *buf, int n, uint64_t off, uint64_t size, Mp3Info *out, size_t *skip_out) {
    uint64_t skip = (off == 0) ? mp3_id3v2_size(buf, (size_t)n) : 0;
    if (skip >= (uint64_t)n) skip = 0;
    if (skip_out) *skip_out = (size_t)skip;
    return mp3_info_from_buffer(buf + skip, (size_t)n - (size_t)skip, off + skip, size, out);
}

int mp3_fast_info(const char *path, Mp3Info *out) {
    if (!path || !out) return -1;
    memset(out, 0, sizeof *out);
    uint8_t buf[MP3_SCAN];
    MFile m;
    if (mf_open(&m, path) != 0) return -1;
    int n = 0;
    uint64_t off = 0;
    int rc = read_head(&m, buf, &n, &off);
    uint64_t size = m.size;
    mf_close(&m);
    if (rc != 0) return -1;
    return parse_head(buf, n, off, size, out, NULL);
}

/* ---- 00.37: checked header read for the background scan ---- */

const char *mp3_why_str(int why) {
    switch (why) {
    case MP3_OK: return "ok";
    case MP3_WHY_IO: return "cannot read the file";
    case MP3_WHY_NOSYNC: return "no valid MPEG audio frame at the start";
    case MP3_WHY_FRAMES: return "frame headers do not chain (damaged or not MP3)";
    case MP3_WHY_DURATION: return "implausible duration";
    case MP3_WHY_VBR_NO_TOC: return "variable bitrate without a Xing/VBRI header";
    case MP3_WHY_SAMPLE: return "no valid frame in the middle of the file (damaged)";
    default: return "?";
    }
}

static int same_stream(const Mp3Frame *a, const Mp3Frame *b) {
    return a->version == b->version && a->layer == b->layer && a->sample_rate == b->sample_rate;
}

/* Walks the frames from buf[i]. Returns the number of chained frame headers
 * (the first included); *vbr = 1 if their bitrates differ. */
static int walk_frames(const uint8_t *buf, size_t n, size_t i, const Mp3Frame *first, int *vbr, int *broken) {
    int count = 0;
    *vbr = 0;
    *broken = 0;
    while (i + 4 <= n) {
        Mp3Frame f;
        if (mp3_parse_frame(buf + i, &f) != 0 || !same_stream(&f, first)) { *broken = 1; break; }
        if (f.bitrate != first->bitrate) *vbr = 1;
        count++;
        i += (size_t)f.frame_len;
    }
    return count;
}

/* Finds a frame inside a buffer read from the middle of the file, confirmed by
 * the two frames after it. 0 and *f on success. */
static int find_mid_frame(const uint8_t *buf, size_t n, const Mp3Frame *ref, Mp3Frame *f, int *vbr) {
    for (size_t i = 0; i + 4 <= n; i++) {
        if (buf[i] != 0xFF || (buf[i + 1] & 0xE0) != 0xE0) continue;
        Mp3Frame a;
        if (mp3_parse_frame(buf + i, &a) != 0 || !same_stream(&a, ref)) continue;
        size_t j = i + (size_t)a.frame_len;
        int ok = 1, chained = 0, differs = a.bitrate != ref->bitrate;
        for (int k = 0; k < 2; k++) {
            if (j + 4 > n) break;
            Mp3Frame b;
            if (mp3_parse_frame(buf + j, &b) != 0 || !same_stream(&b, ref)) { ok = 0; break; }
            if (b.bitrate != ref->bitrate) differs = 1;
            chained++;
            j += (size_t)b.frame_len;
        }
        if (!ok || chained < 2) continue;
        *f = a;
        *vbr = differs;
        return 0;
    }
    return -1;
}

int mp3_fast_info_checked(const char *path, Mp3Info *out, int *why) {
    int dummy;
    if (!why) why = &dummy;
    *why = MP3_WHY_IO;
    if (!path || !out) return -1;
    memset(out, 0, sizeof *out);
    uint8_t buf[MP3_SCAN];
    MFile m;
    if (mf_open(&m, path) != 0) return -1;
    int n = 0;
    uint64_t off = 0;
    if (read_head(&m, buf, &n, &off) != 0) { mf_close(&m); return -1; }
    size_t skip = 0;
    if (parse_head(buf, n, off, m.size, out, &skip) != 0) {
        mf_close(&m);
        *why = MP3_WHY_NOSYNC;
        return -1;
    }
    /* The first frame must chain to at least two more inside the 4 KB (or reach
     * the end of the file), all with the same MPEG version, layer and rate. */
    size_t i0 = (size_t)(out->audio_offset - off);
    int vbr = 0, broken = 0;
    int chained = walk_frames(buf, (size_t)n, i0, &out->first, &vbr, &broken);
    int at_eof = off + (uint64_t)n >= m.size;
    if (chained < 3 && !(at_eof && chained >= 1 && !broken)) {
        mf_close(&m);
        *why = MP3_WHY_FRAMES;
        return -1;
    }
    uint64_t audio = m.size > out->audio_offset ? m.size - out->audio_offset : 0;
    if (!out->exact) {
        /* CBR estimate: right only if every frame has the first one's bitrate.
         * Check the head, then a frame at 1/3 and 2/3 of the audio. */
        if (vbr) { mf_close(&m); *why = MP3_WHY_VBR_NO_TOC; return -1; }
        if (audio > 3 * MP3_SCAN) {
            for (int k = 1; k <= 2; k++) {
                uint64_t pos = out->audio_offset + audio * (uint64_t)k / 3;
                int got = mf_read(&m, pos, buf, MP3_SCAN);
                Mp3Frame f;
                int mvbr = 0;
                if (got < 64 || find_mid_frame(buf, (size_t)got, &out->first, &f, &mvbr) != 0) {
                    mf_close(&m);
                    *why = MP3_WHY_SAMPLE;
                    return -1;
                }
                if (mvbr) { mf_close(&m); *why = MP3_WHY_VBR_NO_TOC; return -1; }
            }
        }
    }
    mf_close(&m);
    /* Plausible length: 0 < d <= 100 h and an average bitrate a real MP3 can have
     * (a damaged Xing frame count gives seconds for a 20 MB file, or days). */
    double d = out->duration;
    if (!(d > 0.0) || d > 360000.0) { *why = MP3_WHY_DURATION; return -1; }
    if (audio > 16384) {
        double kbps = (double)audio * 8.0 / d / 1000.0;
        if (kbps < 4.0 || kbps > 700.0) { *why = MP3_WHY_DURATION; return -1; }
    }
    *why = MP3_OK;
    return 0;
}
