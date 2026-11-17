#include "mp4lazy.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "osal.h"

#define WIN_ENTRIES 16384          /* stsz / stco window: 64 KB (128 KB for co64) */
#define MAX_TRAKS 16
#define MAX_SMALL_TABLE (32 * 1024 * 1024)

typedef struct {
    int64_t off;                   /* file offset of entry 0 */
    int64_t count;
    int esize;                     /* 4 or 8 */
    uint8_t *win;
    int64_t wstart;
    int wlen;
} LazyTab;

typedef struct {
    uint32_t id;
    char handler[5];
    uint32_t timescale;
    int64_t mdhd_dur;
    uint32_t chap[8];
    int nchap;
    int64_t stsd_off, stsd_end;
    int64_t stts_off, nstts;
    int64_t stsc_off, nstsc;
    uint32_t stsz_const;
    int64_t stsz_off, nsamples;
    int64_t stco_off, nchunks;
    int stco_esize;
    int have_stbl;
    int nedit, nempty, edit_complex;  /* elst */
    int64_t edit_media, edit_dur;     /* first non-empty edit: media time (track ts), duration (mvhd ts) */
} Trak;

typedef struct {
    int64_t sample, chunk, in_chunk, spc, stsc_i, off;
    int64_t stts_i, stts_left, dts;
} Cursor;

typedef struct {
    uint32_t *stts;                /* count, delta pairs */
    int64_t nstts;
    uint32_t *stsc;                /* first_chunk (1-based), spc, sdi */
    int64_t nstsc;
    uint32_t stsz_const;
    LazyTab stsz, stco;
    int64_t nsamples, nchunks;
    Cursor cur;
} Tables;

struct Mp4 {
    Mp4Io io;
    Mp4Track tr;
    Mp4Meta meta;
    int have_meta;
    Tables t;
    uint32_t mvhd_scale;
    int64_t mvhd_dur;
    Trak traks[MAX_TRAKS];
    int ntraks;
    /* chpl collected during parse */
    Mp4Chapter *chpl;
    int nchpl;
    Mp4Stats st;
    uint64_t t0;
    char aart[96];
};

/* ---- I/O ----------------------------------------------------------------------- */

static uint32_t rb16(const uint8_t *p) { return ((uint32_t)p[0] << 8) | p[1]; }
static uint32_t rb32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static uint64_t rb64(const uint8_t *p) { return ((uint64_t)rb32(p) << 32) | rb32(p + 4); }

static int rd(Mp4 *m, int64_t off, void *buf, int n) {
    if (n <= 0) return 0;
    if (off < 0 || (m->io.size > 0 && off + n > m->io.size)) return -1;
    m->st.reads++;
    m->st.bytes_read += (uint64_t)n;
    return m->io.read_at(m->io.ud, off, buf, n) == n ? 0 : -1;
}

static int rd_raw(Mp4 *m, int64_t off, void *buf, int n) {
    if (!m->io.read_raw) return rd(m, off, buf, n);
    if (n <= 0) return 0;
    if (off < 0 || (m->io.size > 0 && off + n > m->io.size)) return -1;
    m->st.reads++;
    m->st.bytes_read += (uint64_t)n;
    return m->io.read_raw(m->io.ud, off, buf, n) == n ? 0 : -1;
}

/* Box header at off within [.., end). 0 ok. */
static int box(Mp4 *m, int64_t off, int64_t end, char typ[5], int64_t *pay, int64_t *bend) {
    uint8_t h[16];
    if (off + 8 > end) return -1;
    if (rd(m, off, h, 8) != 0) return -1;
    uint64_t sz = rb32(h);
    memcpy(typ, h + 4, 4);
    typ[4] = 0;
    int64_t p = off + 8;
    if (sz == 1) {
        if (off + 16 > end || rd(m, off + 8, h + 8, 8) != 0) return -1;
        sz = rb64(h + 8);
        p = off + 16;
    } else if (sz == 0) {
        sz = (uint64_t)(end - off);
    }
    if (sz < (uint64_t)(p - off) || off + (int64_t)sz > end) return -1;
    *pay = p;
    *bend = off + (int64_t)sz;
    return 0;
}

/* ---- lazy tables ----------------------------------------------------------------- */

static int lt_get(Mp4 *m, LazyTab *t, int64_t i, uint64_t *v) {
    if (i < 0 || i >= t->count) return -1;
    if (!t->win || i < t->wstart || i >= t->wstart + t->wlen) {
        if (!t->win) {
            t->win = malloc((size_t)WIN_ENTRIES * (size_t)t->esize);
            if (!t->win) return -1;
        }
        int64_t start = i - (i % WIN_ENTRIES);
        int64_t n = t->count - start;
        if (n > WIN_ENTRIES) n = WIN_ENTRIES;
        if (rd_raw(m, t->off + start * t->esize, t->win, (int)(n * t->esize)) != 0) {
            t->wlen = 0;
            return -1;
        }
        t->wstart = start;
        t->wlen = (int)n;
    }
    const uint8_t *p = t->win + (i - t->wstart) * t->esize;
    *v = t->esize == 8 ? rb64(p) : rb32(p);
    return 0;
}

static uint32_t *load_u32(Mp4 *m, int64_t off, int64_t n32) {
    if (n32 <= 0) return NULL;
    if (n32 * 4 > MAX_SMALL_TABLE) return NULL;
    uint8_t *raw = malloc((size_t)n32 * 4);
    if (!raw) return NULL;
    if (rd_raw(m, off, raw, (int)(n32 * 4)) != 0) { free(raw); return NULL; }
    uint32_t *v = (uint32_t *)raw;
    for (int64_t i = 0; i < n32; i++) v[i] = rb32(raw + i * 4);
    return v;
}

static int sample_size(Mp4 *m, Tables *t, int64_t s, int *size) {
    if (t->stsz_const) { *size = (int)t->stsz_const; return 0; }
    uint64_t v;
    if (lt_get(m, &t->stsz, s, &v) != 0) return -1;
    *size = (int)v;
    return 0;
}

static int chunk_off(Mp4 *m, Tables *t, int64_t c, int64_t *off) {
    uint64_t v;
    if (lt_get(m, &t->stco, c, &v) != 0) return -1;
    *off = (int64_t)v;
    return 0;
}

static int tables_load(Mp4 *m, const Trak *k, Tables *t) {
    memset(t, 0, sizeof *t);
    t->nstts = k->nstts;
    t->nstsc = k->nstsc;
    t->nsamples = k->nsamples;
    t->nchunks = k->nchunks;
    t->stsz_const = k->stsz_const;
    if (t->nsamples <= 0 || t->nchunks <= 0 || t->nstts <= 0 || t->nstsc <= 0) return -1;
    t->stts = load_u32(m, k->stts_off, t->nstts * 2);
    t->stsc = load_u32(m, k->stsc_off, t->nstsc * 3);
    if (!t->stts || !t->stsc) return -1;
    t->stsz.off = k->stsz_off;
    t->stsz.count = k->stsz_const ? 0 : k->nsamples;
    t->stsz.esize = 4;
    t->stco.off = k->stco_off;
    t->stco.count = k->nchunks;
    t->stco.esize = k->stco_esize;
    return 0;
}

static void tables_free(Tables *t) {
    free(t->stts);
    free(t->stsc);
    free(t->stsz.win);
    free(t->stco.win);
    memset(t, 0, sizeof *t);
}

/* first chunk (0-based) of stsc entry e, and its samples per chunk */
static int64_t stsc_first(const Tables *t, int64_t e) {
    if (e >= t->nstsc) return t->nchunks;
    int64_t f = (int64_t)t->stsc[e * 3] - 1;
    return f < 0 ? 0 : f;
}

/* Positions the cursor at sample s. 0 ok. */
static int cursor_at(Mp4 *m, Tables *t, int64_t s) {
    Cursor *c = &t->cur;
    if (s < 0) s = 0;
    if (s >= t->nsamples) {
        c->sample = t->nsamples;
        return 0;
    }
    /* time */
    int64_t base = 0, dts = 0, i;
    for (i = 0; i < t->nstts; i++) {
        int64_t cnt = t->stts[i * 2], d = t->stts[i * 2 + 1];
        if (s < base + cnt) {
            c->stts_i = i;
            c->stts_left = cnt - (s - base);
            c->dts = dts + (s - base) * d;
            break;
        }
        base += cnt;
        dts += cnt * d;
    }
    if (i == t->nstts) { c->stts_i = t->nstts; c->stts_left = 0; c->dts = dts; }
    /* chunk */
    base = 0;
    int64_t e;
    for (e = 0; e < t->nstsc; e++) {
        int64_t f0 = stsc_first(t, e), f1 = stsc_first(t, e + 1);
        int64_t spc = t->stsc[e * 3 + 1];
        if (f1 > t->nchunks) f1 = t->nchunks;
        if (f1 <= f0 || spc <= 0) continue;
        int64_t n = (f1 - f0) * spc;
        if (s < base + n) {
            int64_t k = s - base;
            c->stsc_i = e;
            c->spc = spc;
            c->chunk = f0 + k / spc;
            c->in_chunk = k % spc;
            break;
        }
        base += n;
    }
    if (e == t->nstsc) return -1; /* sample table and chunk map disagree */
    int64_t off;
    if (chunk_off(m, t, c->chunk, &off) != 0) return -1;
    if (t->stsz_const) off += c->in_chunk * (int64_t)t->stsz_const;
    else
        for (int64_t j = s - c->in_chunk; j < s; j++) {
            int sz;
            if (sample_size(m, t, j, &sz) != 0) return -1;
            off += sz;
        }
    c->off = off;
    c->sample = s;
    return 0;
}

static int cursor_next(Mp4 *m, Tables *t, int64_t *off, int *size, int64_t *dts, int *dur) {
    Cursor *c = &t->cur;
    if (c->sample >= t->nsamples) return 0;
    if (sample_size(m, t, c->sample, size) != 0) return -1;
    *off = c->off;
    *dts = c->dts;
    int64_t d = c->stts_i < t->nstts ? t->stts[c->stts_i * 2 + 1] : 0;
    *dur = (int)d;
    /* advance */
    c->off += *size;
    c->sample++;
    c->dts += d;
    if (c->stts_i < t->nstts && --c->stts_left <= 0) {
        do { c->stts_i++; } while (c->stts_i < t->nstts && t->stts[c->stts_i * 2] == 0);
        c->stts_left = c->stts_i < t->nstts ? t->stts[c->stts_i * 2] : 0;
    }
    if (++c->in_chunk >= c->spc && c->sample < t->nsamples) {
        c->in_chunk = 0;
        c->chunk++;
        while (c->stsc_i + 1 < t->nstsc && c->chunk >= stsc_first(t, c->stsc_i + 1)) c->stsc_i++;
        c->spc = t->stsc[c->stsc_i * 3 + 1];
        if (c->spc <= 0 || c->chunk >= t->nchunks) return -1;
        if (chunk_off(m, t, c->chunk, &c->off) != 0) return -1;
    }
    return 1;
}

/* ---- box parsing ------------------------------------------------------------------- */

static int parse_stbl(Mp4 *m, Trak *k, int64_t off, int64_t end) {
    char typ[5];
    int64_t pay, bend;
    uint8_t b[12];
    while (off + 8 <= end) {
        if (box(m, off, end, typ, &pay, &bend) != 0) return -1;
        if (!strcmp(typ, "stsd")) {
            k->stsd_off = pay;
            k->stsd_end = bend;
        } else if (!strcmp(typ, "stts") || !strcmp(typ, "stsc") || !strcmp(typ, "stco") || !strcmp(typ, "co64")) {
            if (rd(m, pay, b, 8) != 0) return -1;
            int64_t n = rb32(b + 4);
            int64_t esz = !strcmp(typ, "stts") ? 8 : !strcmp(typ, "stsc") ? 12 : !strcmp(typ, "co64") ? 8 : 4;
            if (pay + 8 + n * esz > bend) return -1;
            if (typ[1] == 't' && typ[2] == 't') { k->stts_off = pay + 8; k->nstts = n; }
            else if (typ[2] == 's') { k->stsc_off = pay + 8; k->nstsc = n; }
            else { k->stco_off = pay + 8; k->nchunks = n; k->stco_esize = (int)esz; }
        } else if (!strcmp(typ, "stsz")) {
            if (rd(m, pay, b, 12) != 0) return -1;
            k->stsz_const = rb32(b + 4);
            k->nsamples = rb32(b + 8);
            k->stsz_off = pay + 12;
            if (!k->stsz_const && pay + 12 + k->nsamples * 4 > bend) return -1;
        } else if (!strcmp(typ, "stz2")) {
            return -1; /* compact sizes: leave to FFmpeg */
        }
        off = bend;
    }
    k->have_stbl = 1;
    return 0;
}

static int parse_container(Mp4 *m, Trak *k, int64_t off, int64_t end, int depth);

static int parse_trak_box(Mp4 *m, Trak *k, const char *typ, int64_t pay, int64_t bend, int depth) {
    uint8_t b[32];
    if (!strcmp(typ, "tkhd")) {
        if (rd(m, pay, b, 24) != 0) return -1;
        k->id = b[0] == 1 ? rb32(b + 20) : rb32(b + 12);
    } else if (!strcmp(typ, "mdhd")) {
        if (rd(m, pay, b, 32) != 0) return -1;
        if (b[0] == 1) { k->timescale = rb32(b + 20); k->mdhd_dur = (int64_t)rb64(b + 24); }
        else { k->timescale = rb32(b + 12); k->mdhd_dur = rb32(b + 16); }
        if ((b[0] == 1 && (uint64_t)k->mdhd_dur == UINT64_MAX) || (b[0] != 1 && k->mdhd_dur == 0xFFFFFFFFLL))
            k->mdhd_dur = 0;
    } else if (!strcmp(typ, "hdlr")) {
        if (rd(m, pay, b, 12) != 0) return -1;
        memcpy(k->handler, b + 8, 4);
        k->handler[4] = 0;
    } else if (!strcmp(typ, "chap")) {
        for (int64_t o = pay; o + 4 <= bend && k->nchap < 8; o += 4) {
            if (rd(m, o, b, 4) != 0) return -1;
            k->chap[k->nchap++] = rb32(b);
        }
    } else if (!strcmp(typ, "elst")) {
        if (rd(m, pay, b, 8) != 0) return -1;
        int v1 = b[0] == 1;
        uint32_t n = rb32(b + 4);
        int64_t o = pay + 8;
        for (uint32_t i = 0; i < n && i < 64; i++) {
            int esz = v1 ? 20 : 12;
            if (o + esz > bend || rd(m, o, b, esz) != 0) return -1;
            int64_t dur = v1 ? (int64_t)rb64(b) : rb32(b);
            int64_t mt = v1 ? (int64_t)rb64(b + 8) : (int32_t)rb32(b + 4);
            uint32_t rate = rb32(b + (v1 ? 16 : 8));
            o += esz;
            if (mt == -1) { if (k->nedit) k->edit_complex = 1; k->nempty++; continue; }
            if (rate != 0x10000 || mt < 0) k->edit_complex = 1;
            if (k->nedit++ == 0) { k->edit_media = mt; k->edit_dur = dur; }
            else k->edit_complex = 1;
        }
        if (n > 64) k->edit_complex = 1;
    } else if (!strcmp(typ, "stbl")) {
        return parse_stbl(m, k, pay, bend);
    } else if (!strcmp(typ, "mdia") || !strcmp(typ, "minf") || !strcmp(typ, "tref") || !strcmp(typ, "edts")) {
        return parse_container(m, k, pay, bend, depth + 1);
    }
    return 0;
}

static int parse_container(Mp4 *m, Trak *k, int64_t off, int64_t end, int depth) {
    if (depth > 8) return -1;
    char typ[5];
    int64_t pay, bend;
    while (off + 8 <= end) {
        if (box(m, off, end, typ, &pay, &bend) != 0) return -1;
        if (parse_trak_box(m, k, typ, pay, bend, depth) != 0) return -1;
        off = bend;
    }
    return 0;
}

/* ---- sample description ------------------------------------------------------------ */

static int read_desc_len(const uint8_t *p, int n, int *pos, int *len) {
    int v = 0;
    for (int i = 0; i < 4; i++) {
        if (*pos >= n) return -1;
        uint8_t c = p[(*pos)++];
        v = (v << 7) | (c & 0x7f);
        if (!(c & 0x80)) { *len = v; return 0; }
    }
    *len = v;
    return 0;
}

/* esds payload (after version/flags): ES_Descriptor -> DecoderConfig -> DecSpecificInfo */
static int parse_esds(Mp4 *m, const uint8_t *p, int n) {
    int pos = 0, len;
    if (pos >= n || p[pos++] != 0x03) return -1;
    if (read_desc_len(p, n, &pos, &len) != 0) return -1;
    if (pos + 3 > n) return -1;
    pos += 2;                              /* ES_ID */
    int fl = p[pos++];
    if (fl & 0x80) pos += 2;               /* dependsOn_ES_ID */
    if (fl & 0x40) { if (pos >= n) return -1; pos += 1 + p[pos]; } /* URL */
    if (fl & 0x20) pos += 2;               /* OCR_ES_Id */
    if (pos >= n || p[pos++] != 0x04) return -1;
    if (read_desc_len(p, n, &pos, &len) != 0) return -1;
    if (pos + 13 > n) return -1;
    int oti = p[pos];
    pos += 13;
    if (oti == 0x40 || oti == 0x66 || oti == 0x67 || oti == 0x68) m->tr.codec = MP4C_AAC;
    else if (oti == 0x69 || oti == 0x6B) m->tr.codec = MP4C_MP3;
    else return -1;
    if (pos < n && p[pos] == 0x05) {
        pos++;
        if (read_desc_len(p, n, &pos, &len) != 0) return -1;
        if (len < 0 || pos + len > n) return -1;
        free(m->tr.extradata);
        m->tr.extradata = calloc(1, (size_t)len + 64);
        if (!m->tr.extradata) return -1;
        memcpy(m->tr.extradata, p + pos, (size_t)len);
        m->tr.extradata_size = len;
    }
    if (m->tr.codec == MP4C_AAC && m->tr.extradata_size < 2) return -1;
    return 0;
}

/* Look for esds / alac among child boxes of a sample entry (or inside 'wave'). */
static int parse_entry_children(Mp4 *m, int64_t off, int64_t end, int depth) {
    char typ[5];
    int64_t pay, bend;
    while (off + 8 <= end && depth < 4) {
        if (box(m, off, end, typ, &pay, &bend) != 0) return -1;
        int64_t n = bend - pay;
        if (!strcmp(typ, "esds") && n > 4 && n < 65536) {
            uint8_t *b = malloc((size_t)n);
            if (!b) return -1;
            int rc = rd(m, pay, b, (int)n) == 0 ? parse_esds(m, b + 4, (int)n - 4) : -1;
            free(b);
            if (rc == 0) return 0;
        } else if (!strcmp(typ, "alac") && n >= 28 && n < 256) {
            /* FFmpeg's ALAC decoder wants the whole 36-byte atom (size + 'alac' + payload). */
            int total = (int)(bend - off);
            free(m->tr.extradata);
            m->tr.extradata = calloc(1, (size_t)total + 64);
            if (!m->tr.extradata) return -1;
            if (rd(m, off, m->tr.extradata, total) != 0) return -1;
            m->tr.extradata_size = total;
            m->tr.codec = MP4C_ALAC;
            const uint8_t *c = m->tr.extradata + 12; /* after size, 'alac', version/flags */
            if (total >= 36) {
                m->tr.bits = c[5];
                m->tr.channels = c[9];
                m->tr.sample_rate = (int)rb32(c + 20);
            }
            return 0;
        } else if (!strcmp(typ, "wave")) {
            if (parse_entry_children(m, pay, bend, depth + 1) == 0) return 0;
        }
        off = bend;
    }
    return -1;
}

static int parse_stsd(Mp4 *m, const Trak *k) {
    uint8_t b[64];
    if (k->stsd_end - k->stsd_off < 8 + 36) return -1;
    if (rd(m, k->stsd_off, b, 8) != 0 || rb32(b + 4) < 1) return -1;
    char typ[5];
    int64_t pay, bend;
    if (box(m, k->stsd_off + 8, k->stsd_end, typ, &pay, &bend) != 0) return -1;
    if (strcmp(typ, "mp4a") && strcmp(typ, "alac")) return -1;
    if (bend - pay < 28 || rd(m, pay, b, 28) != 0) return -1;
    int ver = (int)rb16(b + 8);
    m->tr.channels = (int)rb16(b + 16);
    m->tr.bits = (int)rb16(b + 18);
    m->tr.sample_rate = (int)(rb32(b + 24) >> 16);
    int64_t child = pay + 28;
    if (ver == 1) child += 16;
    else if (ver == 2) {
        if (bend - pay < 64 || rd(m, pay + 28, b, 36) != 0) return -1;
        uint64_t bits = rb64(b + 4);
        double sr;
        memcpy(&sr, &bits, sizeof sr);
        m->tr.sample_rate = (int)(sr + 0.5);
        m->tr.channels = (int)rb32(b + 12);
        m->tr.bits = (int)rb32(b + 20);
        child += 36;
    }
    if (parse_entry_children(m, child, bend, 0) != 0) return -1;
    return m->tr.codec != MP4C_NONE ? 0 : -1;
}

/* ---- tags and chapters ---------------------------------------------------------------- */

static void utf16_to_utf8(const uint8_t *p, int n, int be, char *out, int cap) {
    int o = 0;
    for (int i = 0; i + 1 < n && o + 4 < cap; i += 2) {
        uint32_t c = be ? rb16(p + i) : (uint32_t)(p[i] | (p[i + 1] << 8));
        if (c >= 0xD800 && c < 0xDC00 && i + 3 < n) {
            uint32_t c2 = be ? rb16(p + i + 2) : (uint32_t)(p[i + 2] | (p[i + 3] << 8));
            if (c2 >= 0xDC00 && c2 < 0xE000) { c = 0x10000 + ((c - 0xD800) << 10) + (c2 - 0xDC00); i += 2; }
        }
        if (c == 0) break;
        if (c < 0x80) out[o++] = (char)c;
        else if (c < 0x800) { out[o++] = (char)(0xC0 | (c >> 6)); out[o++] = (char)(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) {
            out[o++] = (char)(0xE0 | (c >> 12)); out[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (c & 0x3F));
        } else {
            out[o++] = (char)(0xF0 | (c >> 18)); out[o++] = (char)(0x80 | ((c >> 12) & 0x3F));
            out[o++] = (char)(0x80 | ((c >> 6) & 0x3F)); out[o++] = (char)(0x80 | (c & 0x3F));
        }
    }
    out[o] = 0;
}

static void set_str(char *dst, size_t cap, const uint8_t *s, int n) {
    if (n < 0) n = 0;
    if ((size_t)n >= cap) n = (int)cap - 1;
    memcpy(dst, s, (size_t)n);
    dst[n] = 0;
}

static void tag_store(Mp4 *m, const char *key, const uint8_t *val, int n) {
    char *dst = NULL;
    size_t cap = 0;
    if (!memcmp(key, "\xa9nam", 4)) { dst = m->meta.title; cap = sizeof m->meta.title; }
    else if (!memcmp(key, "\xa9" "ART", 4)) { dst = m->meta.artist; cap = sizeof m->meta.artist; }
    else if (!memcmp(key, "\xa9" "alb", 4)) { dst = m->meta.album; cap = sizeof m->meta.album; }
    else if (!memcmp(key, "aART", 4)) { dst = m->aart; cap = sizeof m->aart; } /* artist fallback */
    if (dst) set_str(dst, cap, val, n);
}

static void tag_item(Mp4 *m, const char *key, int64_t pay, int64_t bend) {
    char typ[5];
    int64_t dp, de;
    if (box(m, pay, bend, typ, &dp, &de) != 0 || strcmp(typ, "data")) return;
    int64_t n = de - dp - 8;
    if (!memcmp(key, "covr", 4)) {
        if (!m->meta.cover_off && n > 8 && n < 32 * 1024 * 1024) {
            m->meta.cover_off = dp + 8;
            m->meta.cover_len = (int)n;
        }
        return;
    }
    if (!memcmp(key, "\xa9" "cmt", 4) || !memcmp(key, "ldes", 4) || !memcmp(key, "desc", 4)) {
        /* 00.38: description text, up to 64 KB (the caller cleans and caps it) */
        char **slot = key[0] == 'l' ? &m->meta.ldes : key[0] == 'd' ? &m->meta.desc : &m->meta.comment;
        if (*slot || n <= 0 || n > 64 * 1024) return;
        uint8_t ty[4];
        if (rd(m, dp, ty, 4) != 0) return;
        uint32_t type = rb32(ty) & 0xFFFFFF; /* well-known type: 1 UTF-8, 2 UTF-16 BE */
        uint8_t *raw = malloc((size_t)n);
        if (!raw) return;
        if (rd(m, dp + 8, raw, (int)n) != 0) { free(raw); return; }
        size_t cap = type == 2 ? (size_t)n * 2 + 8 : (size_t)n + 1;
        char *txt = malloc(cap);
        if (txt) {
            if (type == 2) utf16_to_utf8(raw, (int)n, 1, txt, (int)cap);
            else { memcpy(txt, raw, (size_t)n); txt[n] = 0; }
            *slot = txt;
        }
        free(raw);
        return;
    }
    if (n <= 0 || n > 4096) return;
    int is_text = !memcmp(key, "\xa9nam", 4) || !memcmp(key, "\xa9" "ART", 4)
               || !memcmp(key, "\xa9" "alb", 4) || !memcmp(key, "aART", 4);
    if (!is_text) return;
    uint8_t buf[4096];
    if (rd(m, dp + 8, buf, (int)n) != 0) return;
    tag_store(m, key, buf, (int)n);
}

static void parse_ilst(Mp4 *m, int64_t off, int64_t end) {
    char typ[5];
    int64_t pay, bend;
    while (off + 8 <= end) {
        if (box(m, off, end, typ, &pay, &bend) != 0) return;
        tag_item(m, typ, pay, bend);
        off = bend;
    }
}

static void parse_meta(Mp4 *m, int64_t pay, int64_t end) {
    uint8_t b[12];
    if (pay + 12 > end || rd(m, pay, b, 12) != 0) return;
    /* ISO 'meta' is a full box (4 bytes version/flags); QuickTime's is not. */
    int64_t off = (!memcmp(b + 4, "hdlr", 4) || !memcmp(b + 4, "ilst", 4)) ? pay : pay + 4;
    char typ[5];
    int64_t p, e;
    while (off + 8 <= end) {
        if (box(m, off, end, typ, &p, &e) != 0) return;
        if (!strcmp(typ, "ilst")) parse_ilst(m, p, e);
        off = e;
    }
}

static void parse_chpl(Mp4 *m, int64_t pay, int64_t end) {
    int64_t n = end - pay;
    if (n < 5 || n > 1024 * 1024) return;
    uint8_t *b = malloc((size_t)n);
    if (!b) return;
    if (rd(m, pay, b, (int)n) != 0) { free(b); return; }
    int pos = 0;
    int64_t left = n - 5;
    int ver = b[pos];
    pos += 4;
    if (ver) pos += 4;
    if (pos >= n) { free(b); return; }
    int cnt = b[pos++];
    free(m->chpl);
    m->chpl = calloc((size_t)(cnt ? cnt : 1), sizeof(Mp4Chapter));
    m->nchpl = 0;
    for (int i = 0; m->chpl && i < cnt; i++) {
        if (left < 9 || pos + 9 > n) break;
        uint64_t start = rb64(b + pos);
        int sl = b[pos + 8];
        pos += 9;
        left -= 9 + sl;
        if (left < 0 || pos + sl > n) break;
        m->chpl[i].start = (double)(int64_t)start / 1e7;
        m->chpl[i].end = -1;
        set_str(m->chpl[i].title, sizeof m->chpl[i].title, b + pos, sl);
        pos += sl;
        m->nchpl = i + 1;
    }
    free(b);
}

static void parse_udta(Mp4 *m, int64_t off, int64_t end) {
    char typ[5];
    int64_t pay, bend;
    while (off + 8 <= end) {
        if (box(m, off, end, typ, &pay, &bend) != 0) return;
        if (!strcmp(typ, "chpl")) parse_chpl(m, pay, bend);
        else if (!strcmp(typ, "meta")) parse_meta(m, pay, bend);
        else if ((uint8_t)typ[0] == 0xA9 && bend - pay > 4) {
            uint8_t b[8];
            if (rd(m, pay, b, 8) == 0 && !memcmp(b + 4, "data", 4)) {
                tag_item(m, typ, pay, bend);
            } else if (rd(m, pay, b, 4) == 0) {
                /* QuickTime string: 16-bit length, 16-bit language, text */
                int sl = (int)rb16(b);
                if (sl > 0 && pay + 4 + sl <= bend && sl < 4096) {
                    uint8_t s[4096];
                    if (rd(m, pay + 4, s, sl) == 0) tag_store(m, typ, s, sl);
                }
            }
        }
        off = bend;
    }
}

/* QuickTime chapter track: each sample is a 16-bit length + UTF-8/UTF-16 title. */
static int qt_chapters(Mp4 *m, const Trak *ct, Mp4Chapter **out, int *nout) {
    *out = NULL;
    *nout = 0;
    if (!ct->have_stbl || !ct->timescale || ct->nsamples <= 0 || ct->nsamples > 4096) return -1;
    Tables t;
    if (tables_load(m, ct, &t) != 0 || cursor_at(m, &t, 0) != 0) { tables_free(&t); return -1; }
    Mp4Chapter *ch = calloc((size_t)ct->nsamples, sizeof *ch);
    int64_t *ts = calloc((size_t)ct->nsamples + 1, sizeof *ts);
    int *ok = calloc((size_t)ct->nsamples, sizeof *ok);
    if (!ch || !ts || !ok) { free(ch); free(ts); free(ok); tables_free(&t); return -1; }
    int64_t n = 0;
    for (; n < ct->nsamples; n++) {
        int64_t off, dts;
        int size, dur;
        if (cursor_next(m, &t, &off, &size, &dts, &dur) != 1) break;
        ts[n] = dts;
        if (size < 2 || size > 4096) continue;
        uint8_t b[4096];
        if (rd(m, off, b, size) != 0) continue;
        int len = (int)rb16(b);
        if (len > size - 2) continue;
        ok[n] = 1;
        const uint8_t *s = b + 2;
        if (len >= 2 && rb16(s) == 0xFEFF) utf16_to_utf8(s + 2, len - 2, 1, ch[n].title, sizeof ch[n].title);
        else if (len >= 2 && rb16(s) == 0xFFFE) utf16_to_utf8(s + 2, len - 2, 0, ch[n].title, sizeof ch[n].title);
        else set_str(ch[n].title, sizeof ch[n].title, s, len);
    }
    int64_t total = ct->mdhd_dur;
    for (int64_t i = 0; i < n; i++) {
        int64_t end = i + 1 < n ? ts[i + 1] : total;
        ch[i].start = (double)ts[i] / ct->timescale;
        ch[i].end = end >= ts[i] ? (double)end / ct->timescale : -1;
        if (!ok[i]) ch[i].start = -1; /* skipped (FFmpeg drops it) */
    }
    tables_free(&t);
    free(ts);
    free(ok);
    *out = ch;
    *nout = (int)n;
    return 0;
}

static void build_chapters(Mp4 *m, const Trak *audio) {
    Mp4Chapter *qt = NULL;
    int nqt = 0;
    for (int i = 0; i < audio->nchap && !qt; i++)
        for (int j = 0; j < m->ntraks; j++)
            if (m->traks[j].id == audio->chap[i] && strcmp(m->traks[j].handler, "vide")) {
                qt_chapters(m, &m->traks[j], &qt, &nqt);
                break;
            }
    /* Same merge as FFmpeg: chpl chapter i, replaced by QT chapter i when present. */
    int n = nqt > m->nchpl ? nqt : m->nchpl;
    if (n <= 0) { free(qt); return; }
    Mp4Chapter *out = calloc((size_t)n, sizeof *out);
    if (!out) { free(qt); return; }
    int k = 0;
    for (int i = 0; i < n; i++) {
        if (i < nqt && qt[i].start >= 0) out[k++] = qt[i];
        else if (i < m->nchpl) out[k++] = m->chpl[i];
    }
    /* Missing ends: next start, or the book's end. */
    for (int i = 0; i < k; i++)
        if (out[i].end < out[i].start) {
            double e = m->tr.duration;
            for (int j = 0; j < k; j++)
                if (out[j].start > out[i].start && out[j].start < e) e = out[j].start;
            out[i].end = e > out[i].start ? e : out[i].start;
        }
    free(qt);
    m->meta.chapters = out;
    m->meta.nchapters = k;
}

/* ---- open / read ------------------------------------------------------------------- */

static int parse_moov(Mp4 *m, int64_t off, int64_t end, int want_meta) {
    char typ[5];
    int64_t pay, bend;
    uint8_t b[32];
    while (off + 8 <= end) {
        if (box(m, off, end, typ, &pay, &bend) != 0) return -1;
        if (!strcmp(typ, "mvhd")) {
            if (rd(m, pay, b, 32) != 0) return -1;
            if (b[0] == 1) { m->mvhd_scale = rb32(b + 20); m->mvhd_dur = (int64_t)rb64(b + 24); }
            else { m->mvhd_scale = rb32(b + 12); m->mvhd_dur = rb32(b + 16); }
        } else if (!strcmp(typ, "trak") && m->ntraks < MAX_TRAKS) {
            Trak *k = &m->traks[m->ntraks];
            memset(k, 0, sizeof *k);
            if (parse_container(m, k, pay, bend, 0) == 0) m->ntraks++;
        } else if (!strcmp(typ, "udta") && want_meta) {
            parse_udta(m, pay, bend);
        } else if (!strcmp(typ, "meta") && want_meta) {
            parse_meta(m, pay, bend);
        } else if (!strcmp(typ, "mvex")) {
            return -1; /* fragmented MP4: leave to FFmpeg */
        }
        off = bend;
    }
    return 0;
}

int mp4_open(const Mp4Io *io, int want_meta, Mp4 **out) {
    *out = NULL;
    if (!io || !io->read_at || io->size < 16) return -1;
    Mp4 *m = calloc(1, sizeof *m);
    if (!m) return -1;
    m->io = *io;
    m->t0 = now_ms();
    /* top level: find moov without touching mdat */
    int64_t off = 0, moov = -1, moov_end = 0;
    char typ[5];
    int64_t pay, bend;
    int first = 1;
    while (off + 8 <= io->size) {
        if (box(m, off, io->size, typ, &pay, &bend) != 0) break;
        if (first && strcmp(typ, "ftyp") && strcmp(typ, "moov") && strcmp(typ, "wide")
            && strcmp(typ, "free") && strcmp(typ, "mdat") && strcmp(typ, "skip")) break;
        first = 0;
        if (!strcmp(typ, "moov")) { moov = pay; moov_end = bend; break; }
        if (!strcmp(typ, "moof")) break;
        off = bend;
    }
    if (moov < 0) { mp4_close(m); return -1; }
    if (parse_moov(m, moov, moov_end, want_meta) != 0) { mp4_close(m); return -1; }
    const Trak *audio = NULL;
    for (int i = 0; i < m->ntraks && !audio; i++) {
        const Trak *k = &m->traks[i];
        if (strcmp(k->handler, "soun") || !k->have_stbl || !k->timescale) continue;
        memset(&m->tr, 0, sizeof m->tr);
        if (parse_stsd(m, k) == 0) audio = k;
        else { free(m->tr.extradata); m->tr.extradata = NULL; m->tr.codec = MP4C_NONE; }
    }
    if (!audio || tables_load(m, audio, &m->t) != 0 || cursor_at(m, &m->t, 0) != 0) {
        mp4_close(m);
        return -1;
    }
    m->tr.timescale = audio->timescale;
    m->tr.nsamples = audio->nsamples;
    m->tr.track_id = audio->id;
    int64_t stts_total = 0;
    for (int64_t i = 0; i < m->t.nstts; i++) stts_total += (int64_t)m->t.stts[i * 2] * m->t.stts[i * 2 + 1];
    m->tr.duration_ts = audio->mdhd_dur > 0 ? audio->mdhd_dur : stts_total;
    m->tr.duration = (double)m->tr.duration_ts / audio->timescale;
    /* The app opens the mov path with ignore_editlist=1, so edit lists are recorded
     * (logged) but not applied: the timeline is the raw stts one, like FFmpeg's. */
    m->tr.edit_start = audio->nedit == 1 ? audio->edit_media : 0;
    m->tr.play_ts = m->tr.duration_ts;
    if (want_meta) {
        if (!m->meta.artist[0] && m->aart[0]) snprintf(m->meta.artist, sizeof m->meta.artist, "%s", m->aart);
        build_chapters(m, audio);
        m->have_meta = 1;
    }
    m->st.parse_ms = now_ms() - m->t0;
    *out = m;
    return 0;
}

const Mp4Track *mp4_track(const Mp4 *m) { return m ? &m->tr : NULL; }
const Mp4Meta *mp4_meta(const Mp4 *m) { return m && m->have_meta ? &m->meta : NULL; }
void mp4_stats(const Mp4 *m, Mp4Stats *st) { if (m && st) *st = m->st; }
int64_t mp4_tell(const Mp4 *m) { return m ? m->t.cur.sample : 0; }

int mp4_next(Mp4 *m, int64_t *off, int *size, int64_t *dts, int *dur) {
    if (!m) return -1;
    return cursor_next(m, &m->t, off, size, dts, dur);
}

int mp4_seek(Mp4 *m, int64_t ts, int64_t *actual) {
    if (!m) return -1;
    Tables *t = &m->t;
    if (ts < 0) ts = 0;
    int64_t base = 0, dts = 0, s = -1;
    for (int64_t i = 0; i < t->nstts; i++) {
        int64_t cnt = t->stts[i * 2], d = t->stts[i * 2 + 1];
        if (cnt <= 0) continue;
        if (d > 0 && ts < dts + cnt * d) { s = base + (ts - dts) / d; break; }
        base += cnt;
        dts += cnt * d;
    }
    if (s < 0) s = t->nsamples > 0 ? t->nsamples - 1 : 0; /* past the end: last sample */
    if (s >= t->nsamples) s = t->nsamples - 1;
    if (cursor_at(m, t, s) != 0) return -1;
    if (actual) *actual = t->cur.dts;
    return 0;
}

int mp4_read_cover(Mp4 *m, uint8_t **out, size_t *len) {
    *out = NULL;
    *len = 0;
    if (!m || !m->meta.cover_off || m->meta.cover_len <= 8) return -1;
    uint8_t *b = malloc((size_t)m->meta.cover_len);
    if (!b) return -1;
    if (rd(m, m->meta.cover_off, b, m->meta.cover_len) != 0) { free(b); return -1; }
    *out = b;
    *len = (size_t)m->meta.cover_len;
    return 0;
}

void mp4_close(Mp4 *m) {
    if (!m) return;
    tables_free(&m->t);
    free(m->tr.extradata);
    free(m->meta.chapters);
    free(m->meta.comment);
    free(m->meta.ldes);
    free(m->meta.desc);
    free(m->chpl);
    free(m);
}
