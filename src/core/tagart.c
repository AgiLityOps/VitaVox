#include "tagart.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "dbg.h"
#include "pathutil.h"
#include "quit.h"

#define MAX_ART (4 * 1024 * 1024)
#define MAX_WALK 256

static int read_at(FILE *f, int64_t off, void *dst, size_t n) {
    if (fseek(f, (long)off, SEEK_SET) != 0) return -1;
    return fread(dst, 1, n, f) == n ? 0 : -1;
}

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint64_t be64(const uint8_t *p) {
    return ((uint64_t)be32(p) << 32) | be32(p + 4);
}

static uint32_t id3_synchsafe(const uint8_t *p) {
    return ((uint32_t)(p[0] & 0x7f) << 21) | ((uint32_t)(p[1] & 0x7f) << 14)
        | ((uint32_t)(p[2] & 0x7f) << 7) | (uint32_t)(p[3] & 0x7f);
}

static int looks_image(const uint8_t *p, size_t n) {
    if (n >= 3 && p[0] == 0xFF && p[1] == 0xD8 && p[2] == 0xFF) return 1; /* JPEG */
    if (n >= 8 && memcmp(p, "\x89PNG\r\n\x1a\n", 8) == 0) return 1;
    if (n >= 12 && memcmp(p, "RIFF", 4) == 0 && memcmp(p + 8, "WEBP", 4) == 0) return 1;
    if (n >= 6 && (memcmp(p, "GIF87a", 6) == 0 || memcmp(p, "GIF89a", 6) == 0)) return 1;
    if (n >= 2 && p[0] == 0x42 && p[1] == 0x4D) return 1; /* BMP */
    return 0;
}

/* ---- ID3v2 APIC ----------------------------------------------------------- */

static const uint8_t *cstr_skip(const uint8_t *p, const uint8_t *end, int utf16) {
    if (p >= end) return end;
    if (!utf16) {
        while (p < end && *p) p++;
        return p < end ? p + 1 : end;
    }
    if (p + 1 < end && ((p[0] == 0xFF && p[1] == 0xFE) || (p[0] == 0xFE && p[1] == 0xFF))) p += 2;
    while (p + 1 < end && (p[0] || p[1])) p += 2;
    return p + 1 < end ? p + 2 : end;
}

static int take_apic(const uint8_t *body, size_t n, uint8_t **out, size_t *len) {
    if (!body || n < 4) return -1;
    int enc = body[0];
    const uint8_t *p = body + 1, *end = body + n;
    /* v2.3/2.4: mime\0, type, desc, data. v2.2 uses 3-char format, handled by caller. */
    if (enc > 3) return -1;
    int utf16 = (enc == 1 || enc == 2);
    if (p + 3 <= end && p[0] && p[1] && p[2] && p[3] != '/' && !memchr(p, '/', 8 < (size_t)(end - p) ? 8 : (size_t)(end - p))) {
        /* maybe already at "image/..." */
    }
    p = cstr_skip(p, end, 0); /* mime is always latin-1 / 3-char */
    if (p >= end) return -1;
    p++; /* picture type */
    p = cstr_skip(p, end, utf16);
    if (p >= end) return -1;
    size_t sz = (size_t)(end - p);
    if (sz < 8 || sz > MAX_ART || !looks_image(p, sz)) return -1;
    uint8_t *buf = malloc(sz);
    if (!buf) return -1;
    memcpy(buf, p, sz);
    *out = buf;
    *len = sz;
    return 0;
}

static int id3_extract(FILE *f, int64_t fsz, uint8_t **out, size_t *len) {
    uint8_t hdr[10];
    if (read_at(f, 0, hdr, 10) != 0) return -1;
    if (memcmp(hdr, "ID3", 3) != 0) return -1;
    int ver = hdr[3];
    if (ver < 2 || ver > 4) return -1;
    uint32_t tag = id3_synchsafe(hdr + 6);
    int footer = (hdr[5] & 0x10) ? 10 : 0;
    int64_t end = 10 + (int64_t)tag + footer;
    if (end > fsz) end = fsz;
    int64_t off = 10;
    if (hdr[5] & 0x40) { /* extended header */
        uint8_t ex[6];
        if (read_at(f, off, ex, 4) != 0) return -1;
        uint32_t exs = ver == 4 ? id3_synchsafe(ex) : be32(ex);
        if (exs < 4 || off + exs >= end) return -1;
        off += ver == 3 ? 4 + (int64_t)be32(ex) : (int64_t)exs;
    }
    int frames = 0;
    while (off + 6 <= end && frames++ < 512) {
        if (app_quit_active()) return -1;
        if (ver == 2) {
            uint8_t fh[6];
            if (read_at(f, off, fh, 6) != 0) return -1;
            if (!fh[0]) break;
            uint32_t fs = ((uint32_t)fh[3] << 16) | ((uint32_t)fh[4] << 8) | fh[5];
            off += 6;
            if (fs == 0 || off + fs > end) break;
            if (memcmp(fh, "PIC", 3) == 0) {
                uint8_t *body = malloc(fs);
                if (!body) return -1;
                if (read_at(f, off, body, fs) != 0) { free(body); return -1; }
                /* enc + 3-char format + type + desc + data */
                int rc = -1;
                if (fs >= 5) {
                    uint8_t *rew = malloc(fs + 8);
                    if (rew) {
                        /* rewrite as mime-style for take_apic */
                        rew[0] = body[0];
                        memcpy(rew + 1, "image/jpeg", 11); /* 10 chars + NUL; format is ignored */
                        /* actually take_apic expects mime\0 type desc. Build: enc, mime, type, desc, data */
                        const uint8_t *dp = body + 1 + 3 + 1;
                        const uint8_t *endb = body + fs;
                        dp = cstr_skip(dp, endb, body[0] == 1 || body[0] == 2);
                        if (dp < endb && looks_image(dp, (size_t)(endb - dp))) {
                            size_t sz = (size_t)(endb - dp);
                            uint8_t *img = malloc(sz);
                            if (img) { memcpy(img, dp, sz); *out = img; *len = sz; rc = 0; }
                        }
                        free(rew);
                    }
                }
                free(body);
                if (rc == 0) return 0;
            }
            off += fs;
            continue;
        }
        uint8_t fh[10];
        if (read_at(f, off, fh, 10) != 0) return -1;
        if (!fh[0]) break;
        uint32_t fs = ver == 4 ? id3_synchsafe(fh + 4) : be32(fh + 4);
        off += 10;
        if (fs == 0 || off + fs > end) break;
        if (memcmp(fh, "APIC", 4) == 0 && fs <= MAX_ART) {
            uint8_t *body = malloc(fs);
            if (!body) return -1;
            if (read_at(f, off, body, fs) != 0) { free(body); return -1; }
            int rc = take_apic(body, fs, out, len);
            free(body);
            if (rc == 0) return 0;
        }
        off += fs;
    }
    return -1;
}

/* ---- MP4 covr / moov locate ------------------------------------------------ */

static int read_box(FILE *f, int64_t off, int64_t fsz, char typ[5], int64_t *hdr, int64_t *payload, int64_t *size) {
    uint8_t h[16];
    if (off < 0 || off + 8 > fsz) return -1;
    if (read_at(f, off, h, 8) != 0) return -1;
    uint64_t sz = be32(h);
    memcpy(typ, h + 4, 4);
    typ[4] = 0;
    int64_t hs = 8;
    if (sz == 1) {
        if (off + 16 > fsz || read_at(f, off + 8, h + 8, 8) != 0) return -1;
        sz = be64(h + 8);
        hs = 16;
    } else if (sz == 0) {
        sz = (uint64_t)(fsz - off);
    }
    if (sz < (uint64_t)hs || off + (int64_t)sz > fsz + 8) return -1;
    *hdr = hs;
    *payload = off + hs;
    *size = (int64_t)sz;
    return 0;
}

/* Walk boxes in [range_off, range_off+range_sz). Skip mdat/free payloads. */
static int walk_find(FILE *f, int64_t range_off, int64_t range_sz, int64_t fsz,
                     const char *want, int in_ilst,
                     int64_t *found_off, int64_t *found_sz, int depth) {
    if (depth > 12 || range_sz < 8) return -1;
    int64_t off = range_off, end = range_off + range_sz;
    if (end > fsz) end = fsz;
    int n = 0;
    while (off + 8 <= end && n++ < MAX_WALK) {
        if (app_quit_active()) return -1;
        char typ[5];
        int64_t hdr, pay, sz;
        if (read_box(f, off, fsz, typ, &hdr, &pay, &sz) != 0) break;
        if (sz < hdr) break;
        if (memcmp(typ, want, 4) == 0) {
            *found_off = off;
            *found_sz = sz;
            return 0;
        }
        int descend = 0;
        if (!memcmp(typ, "moov", 4) || !memcmp(typ, "udta", 4) || !memcmp(typ, "ilst", 4)
            || !memcmp(typ, "trak", 4) || !memcmp(typ, "mdia", 4))
            descend = 1;
        if (!memcmp(typ, "meta", 4)) {
            /* meta has 4-byte version/flags then children */
            if (walk_find(f, pay + 4, sz - hdr - 4, fsz, want, in_ilst || 0, found_off, found_sz, depth + 1) == 0)
                return 0;
        } else if (descend) {
            if (walk_find(f, pay, sz - hdr, fsz, want, in_ilst || !memcmp(typ, "ilst", 4),
                          found_off, found_sz, depth + 1) == 0)
                return 0;
        } else if (in_ilst && typ[0] == '\xa9') {
            /* skip itunes text atoms */
        }
        /* Never enter mdat — jump by declared size. */
        off += sz;
        if (sz <= 0) break;
    }
    return -1;
}

int mp4_locate_box(const char *path, const char typ[4], int64_t *off, int64_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    int64_t fsz = (int64_t)ftell(f);
    int64_t o = 0, s = 0;
    int rc = walk_find(f, 0, fsz, fsz, typ, 0, &o, &s, 0);
    fclose(f);
    if (rc != 0) return -1;
    if (off) *off = o;
    if (size) *size = s;
    return 0;
}

static int covr_payload(FILE *f, int64_t box_off, int64_t box_sz, int64_t fsz,
                        uint8_t **out, size_t *len) {
    /* covr: [size][covr][size][data][ver/flags][image...]  or just data after 16 bytes */
    char typ[5];
    int64_t hdr, pay, sz;
    if (read_box(f, box_off, fsz, typ, &hdr, &pay, &sz) != 0) return -1;
    /* child 'data' box is usual */
    if (pay + 8 <= box_off + box_sz) {
        char ct[5];
        int64_t ch, cp, cs;
        if (read_box(f, pay, fsz, ct, &ch, &cp, &cs) == 0 && !memcmp(ct, "data", 4) && cs > ch + 8) {
            int64_t img_off = cp + 8; /* version/flags + reserved? data box: 4 flags + 4 locale */
            int64_t img_sz = cs - ch - 8;
            if (img_sz < 8 || img_sz > MAX_ART) return -1;
            uint8_t *buf = malloc((size_t)img_sz);
            if (!buf) return -1;
            if (read_at(f, img_off, buf, (size_t)img_sz) != 0) { free(buf); return -1; }
            if (!looks_image(buf, (size_t)img_sz)) { free(buf); return -1; }
            *out = buf;
            *len = (size_t)img_sz;
            return 0;
        }
    }
    return -1;
}

static int mp4_extract(FILE *f, int64_t fsz, uint8_t **out, size_t *len) {
    uint8_t h[8];
    if (read_at(f, 0, h, 8) != 0) return -1;
    /* ftyp or moov at start */
    if (memcmp(h + 4, "ftyp", 4) && memcmp(h + 4, "moov", 4) && memcmp(h + 4, "wide", 4)
        && memcmp(h + 4, "mdat", 4) && memcmp(h + 4, "free", 4) && memcmp(h + 4, "junk", 4))
        return -1;
    int64_t off = 0, sz = 0;
    if (walk_find(f, 0, fsz, fsz, "covr", 0, &off, &sz, 0) != 0) return -1;
    return covr_payload(f, off, sz, fsz, out, len);
}

/* ---- FLAC METADATA_BLOCK_PICTURE ------------------------------------------ */

static int flac_extract(FILE *f, int64_t fsz, uint8_t **out, size_t *len) {
    uint8_t mag[4];
    if (read_at(f, 0, mag, 4) != 0 || memcmp(mag, "fLaC", 4) != 0) return -1;
    int64_t off = 4;
    for (int i = 0; i < 64 && off + 4 <= fsz; i++) {
        if (app_quit_active()) return -1;
        uint8_t bh[4];
        if (read_at(f, off, bh, 4) != 0) return -1;
        int last = bh[0] & 0x80;
        int type = bh[0] & 0x7f;
        uint32_t sz = ((uint32_t)bh[1] << 16) | ((uint32_t)bh[2] << 8) | bh[3];
        off += 4;
        if (off + sz > fsz) return -1;
        if (type == 6 && sz >= 32 && sz <= MAX_ART + 64) {
            uint8_t *blk = malloc(sz);
            if (!blk) return -1;
            if (read_at(f, off, blk, sz) != 0) { free(blk); return -1; }
            /* type(4) mime_len(4) mime typ_len typ desc_len desc w h d idx data_len data */
            size_t p = 4;
            if (p + 4 > sz) { free(blk); return -1; }
            uint32_t ml = be32(blk + p); p += 4;
            if (p + ml + 4 > sz) { free(blk); return -1; }
            p += ml;
            uint32_t dl = be32(blk + p); p += 4;
            if (p + dl + 20 > sz) { free(blk); return -1; }
            p += dl;
            p += 16; /* w h depth idx */
            if (p + 4 > sz) { free(blk); return -1; }
            uint32_t il = be32(blk + p); p += 4;
            if (p + il > sz || il < 8 || il > MAX_ART) { free(blk); return -1; }
            if (!looks_image(blk + p, il)) { free(blk); return -1; }
            uint8_t *img = malloc(il);
            if (!img) { free(blk); return -1; }
            memcpy(img, blk + p, il);
            free(blk);
            *out = img;
            *len = il;
            return 0;
        }
        off += sz;
        if (last) break;
    }
    return -1;
}

int tagart_extract(const char *path, uint8_t **out, size_t *len) {
    if (out) *out = NULL;
    if (len) *len = 0;
    if (!path || !path[0] || !out || !len) return -1;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    int64_t fsz = (int64_t)ftell(f);
    if (fsz < 16) { fclose(f); return -1; }
    uint8_t *img = NULL;
    size_t n = 0;
    int rc = -1;
    if (id3_extract(f, fsz, &img, &n) == 0) rc = 0;
    else if (mp4_extract(f, fsz, &img, &n) == 0) rc = 0;
    else if (flac_extract(f, fsz, &img, &n) == 0) rc = 0;
    fclose(f);
    if (rc == 0) {
        *out = img;
        *len = n;
        dbg("tagart: extracted %u bytes from '%s'", (unsigned)n, path);
        return 0;
    }
    return -1;
}
