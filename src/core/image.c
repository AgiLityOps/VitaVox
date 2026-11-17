#include "image.h"

void img_crop_resize(const uint8_t *src, int sw, int sh, int comps, uint8_t *dst, int out_sz) {
    int side = sw < sh ? sw : sh;
    int ox = (sw - side) / 2, oy = (sh - side) / 2;
    for (int y = 0; y < out_sz; y++) {
        int y0 = oy + y * side / out_sz, y1 = oy + (y + 1) * side / out_sz;
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < out_sz; x++) {
            int x0 = ox + x * side / out_sz, x1 = ox + (x + 1) * side / out_sz;
            if (x1 <= x0) x1 = x0 + 1;
            /* Colour is averaged weighted by alpha, so fully transparent
             * pixels (often black or white underneath) do not bleed a dark or
             * light fringe into the edges of a transparent cover. */
            uint64_t acc[3] = {0, 0, 0};
            unsigned asum = 0, n = 0;
            for (int yy = y0; yy < y1; yy++)
                for (int xx = x0; xx < x1; xx++) {
                    const uint8_t *p = src + (yy * sw + xx) * comps;
                    unsigned a = comps == 4 ? p[3] : (comps == 2 ? p[1] : 255);
                    acc[0] += (uint64_t)p[0] * a;
                    acc[1] += (uint64_t)(comps >= 3 ? p[1] : p[0]) * a;
                    acc[2] += (uint64_t)(comps >= 3 ? p[2] : p[0]) * a;
                    asum += a;
                    n++;
                }
            uint8_t *d = dst + (y * out_sz + x) * 4;
            for (int c = 0; c < 3; c++) d[c] = asum ? (uint8_t)(acc[c] / asum) : 0;
            d[3] = (uint8_t)(asum / n);
        }
    }
}

#include "dbg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "osal.h"
#if defined(__has_include)
#  if __has_include(<zlib.h>)
#    define VA_HAVE_ZLIB 1
#  endif
#endif
#ifdef VA_HAVE_ZLIB
#include <zlib.h>
#endif

#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define THUMB 256

/* 00.37: JPEG covers are decoded by libjpeg(-turbo) at 1/2, 1/4 or 1/8 scale
 * (the largest reduction that keeps the shorter side >= the size wanted), so a
 * 2920x3072 cover becomes a 365x384 decode for a 256 px thumb instead of a
 * 9-megapixel one. PNG has no reduced-scale decode (libpng and stb_image only
 * decode at full size; Adam7 pass tricks would cover interlaced files only),
 * so PNG covers still decode at full size. Without libjpeg, stb_image is used. */
#ifdef VA_HAVE_LIBJPEG
#include <setjmp.h>
#include <jpeglib.h>

typedef struct {
    struct jpeg_error_mgr pub;
    jmp_buf jb;
} JErr;

static void jerr_exit(j_common_ptr c) { longjmp(((JErr *)c->err)->jb, 1); }
static void jerr_out(j_common_ptr c) { (void)c; }

int img_jpeg_scale_for(int w, int h, int min_side) {
    int side = w < h ? w : h;
    if (side <= 0 || min_side <= 0) return 1;
    int d = 8;
    while (d > 1 && (side + d - 1) / d < min_side) d /= 2;
    return d;
}

/* RGB8 (3 bytes per pixel), malloc'd; NULL if not a JPEG libjpeg can decode
 * (CMYK, damaged...): the caller falls back to stb_image. */
static uint8_t *jpeg_decode_scaled(const uint8_t *data, size_t len, int min_side, int *ow, int *oh, int *sw,
                                   int *sh, int *denom) {
    if (len < 4 || data[0] != 0xFF || data[1] != 0xD8) return NULL;
    struct jpeg_decompress_struct ci;
    JErr je;
    uint8_t *volatile px = NULL;
    memset(&ci, 0, sizeof ci);
    ci.err = jpeg_std_error(&je.pub);
    je.pub.error_exit = jerr_exit;
    je.pub.output_message = jerr_out;
    if (setjmp(je.jb)) {
        jpeg_destroy_decompress(&ci);
        free(px);
        return NULL;
    }
    jpeg_create_decompress(&ci);
    jpeg_mem_src(&ci, (const unsigned char *)data, (unsigned long)len);
    if (jpeg_read_header(&ci, TRUE) != JPEG_HEADER_OK) { jpeg_destroy_decompress(&ci); return NULL; }
    if (ci.jpeg_color_space == JCS_CMYK || ci.jpeg_color_space == JCS_YCCK) {
        jpeg_destroy_decompress(&ci);
        return NULL;
    }
    int d = img_jpeg_scale_for((int)ci.image_width, (int)ci.image_height, min_side);
    ci.scale_num = 1;
    ci.scale_denom = (unsigned)d;
    ci.out_color_space = JCS_RGB;
    ci.do_fancy_upsampling = d > 1 ? FALSE : TRUE;
    ci.do_block_smoothing = FALSE;
    /* A progressive file at 1/8 needs only the DC coefficients (one per 8x8
     * block), and its first scans carry them: stop reading at the first AC scan
     * instead of entropy-decoding the whole multi-MB file. */
    int dc_only = ci.progressive_mode && d == 8;
    if (dc_only) ci.buffered_image = TRUE;
    jpeg_start_decompress(&ci);
    int w = (int)ci.output_width, h = (int)ci.output_height;
    if (w <= 0 || h <= 0 || ci.output_components != 3) {
        jpeg_destroy_decompress(&ci);
        return NULL;
    }
    px = malloc((size_t)w * (size_t)h * 3);
    if (!px) { jpeg_destroy_decompress(&ci); return NULL; }
    if (dc_only) {
        for (;;) {
            int r = jpeg_consume_input(&ci);
            if (r == JPEG_REACHED_EOI || r == JPEG_SUSPENDED) break;
            if (r == JPEG_REACHED_SOS && ci.Ss > 0) break; /* first AC scan: the DC scans are in */
        }
        jpeg_start_output(&ci, ci.input_scan_number);
    }
    while (ci.output_scanline < ci.output_height) {
        JSAMPROW row = (JSAMPROW)(px + (size_t)ci.output_scanline * (size_t)w * 3);
        jpeg_read_scanlines(&ci, &row, 1);
    }
    if (sw) *sw = (int)ci.image_width;
    if (sh) *sh = (int)ci.image_height;
    if (dc_only) {
        jpeg_finish_output(&ci);
        jpeg_abort_decompress(&ci); /* the AC scans are not needed */
    } else {
        jpeg_finish_decompress(&ci);
    }
    jpeg_destroy_decompress(&ci);
    *ow = w;
    *oh = h;
    if (denom) *denom = d;
    return px;
}
#else
int img_jpeg_scale_for(int w, int h, int min_side) {
    (void)w; (void)h; (void)min_side;
    return 1;
}
#endif

static int g_force_stb;
void img_test_force_stb(int on) { g_force_stb = on; }

int img_have_scaled_jpeg(void) {
#ifdef VA_HAVE_LIBJPEG
    return 1;
#else
    return 0;
#endif
}

#ifdef VA_HAVE_ZLIB
static void be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static int write_chunk(FILE *f, const char *type, const uint8_t *data, size_t n) {
    uint8_t hdr[8], crc[4];
    be32(hdr, (uint32_t)n);
    memcpy(hdr + 4, type, 4);
    if (fwrite(hdr, 1, 8, f) != 8) return -1;
    if (n && fwrite(data, 1, n, f) != n) return -1;
    uint32_t c = crc32(0L, (const Bytef *)type, 4);
    if (n) c = crc32(c, data, (uInt)n);
    be32(crc, c);
    return fwrite(crc, 1, 4, f) == 4 ? 0 : -1;
}
#endif

#ifndef VA_HAVE_ZLIB
/* No zlib (host test builds): PNG with stored (uncompressed) deflate blocks. */
static uint32_t crc_tab[256];
static void crc_init(void) {
    if (crc_tab[1]) return;
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_tab[n] = c;
    }
}
static uint32_t crc_upd(uint32_t c, const uint8_t *p, size_t n) {
    c ^= 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = crc_tab[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}
static void be32s(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static int write_chunk_s(FILE *f, const char *type, const uint8_t *data, size_t n) {
    uint8_t hdr[8], crc[4];
    be32s(hdr, (uint32_t)n);
    memcpy(hdr + 4, type, 4);
    if (fwrite(hdr, 1, 8, f) != 8) return -1;
    if (n && fwrite(data, 1, n, f) != n) return -1;
    uint32_t c = crc_upd(0, (const uint8_t *)type, 4);
    if (n) {
        /* continue the CRC over data */
        c ^= 0xFFFFFFFFu;
        for (size_t i = 0; i < n; i++) c = crc_tab[(c ^ data[i]) & 0xFF] ^ (c >> 8);
        c ^= 0xFFFFFFFFu;
    }
    be32s(crc, c);
    return fwrite(crc, 1, 4, f) == 4 ? 0 : -1;
}
#endif

int img_write_png(const char *path, const uint8_t *rgba, int w, int h) {
#ifndef VA_HAVE_ZLIB
    if (!path || !rgba || w <= 0 || h <= 0) return -1;
    crc_init();
    size_t raw = (size_t)(1 + w * 4) * (size_t)h;
    size_t nblk = (raw + 65534) / 65535;
    size_t zlen = 2 + raw + nblk * 5 + 4;
    uint8_t *z = malloc(zlen);
    if (!z) return -1;
    size_t o = 0;
    z[o++] = 0x78; z[o++] = 0x01;
    uint32_t a = 1, b = 0;
    size_t done = 0;
    int y = 0, x = -1; /* x = -1: filter byte */
    while (done < raw) {
        size_t k = raw - done > 65535 ? 65535 : raw - done;
        z[o++] = (uint8_t)(done + k == raw ? 1 : 0);
        z[o++] = (uint8_t)(k & 0xFF); z[o++] = (uint8_t)(k >> 8);
        z[o++] = (uint8_t)(~k & 0xFF); z[o++] = (uint8_t)((~k >> 8) & 0xFF);
        for (size_t i = 0; i < k; i++) {
            uint8_t v;
            if (x < 0) { v = 0; x = 0; }
            else {
                v = rgba[(size_t)y * (size_t)w * 4 + (size_t)x];
                if (++x == w * 4) { x = -1; y++; }
            }
            z[o++] = v;
            a = (a + v) % 65521u;
            b = (b + a) % 65521u;
        }
        done += k;
    }
    be32s(z + o, (b << 16) | a); o += 4;
    FILE *f = fopen(path, "wb");
    if (!f) { free(z); return -1; }
    static const uint8_t sig[] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    int rc = fwrite(sig, 1, 8, f) == 8 ? 0 : -1;
    uint8_t ihdr[13];
    be32s(ihdr, (uint32_t)w);
    be32s(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8; ihdr[9] = 6; ihdr[10] = ihdr[11] = ihdr[12] = 0;
    if (rc == 0) rc = write_chunk_s(f, "IHDR", ihdr, 13);
    if (rc == 0) rc = write_chunk_s(f, "IDAT", z, o);
    if (rc == 0) rc = write_chunk_s(f, "IEND", NULL, 0);
    if (fclose(f) != 0) rc = -1;
    free(z);
    if (rc != 0) remove(path);
    return rc;
#endif
#ifdef VA_HAVE_ZLIB
    if (!path || !rgba || w <= 0 || h <= 0) return -1;
    size_t raw = (size_t)(1 + w * 4) * (size_t)h;
    uint8_t *src = malloc(raw);
    if (!src) return -1;
    for (int y = 0; y < h; y++) {
        src[(size_t)y * (size_t)(1 + w * 4)] = 0;
        memcpy(src + (size_t)y * (size_t)(1 + w * 4) + 1, rgba + (size_t)y * (size_t)w * 4, (size_t)w * 4);
    }
    uLongf zlen = compressBound((uLong)raw);
    uint8_t *zbuf = malloc(zlen);
    int zr = zbuf ? compress(zbuf, &zlen, src, (uLong)raw) : Z_MEM_ERROR;
    free(src);
    if (zr != Z_OK) { free(zbuf); return -1; }
    FILE *f = fopen(path, "wb");
    if (!f) { free(zbuf); return -1; }
    static const uint8_t sig[] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    int rc = fwrite(sig, 1, 8, f) == 8 ? 0 : -1;
    uint8_t ihdr[13];
    be32(ihdr, (uint32_t)w);
    be32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8;
    ihdr[9] = 6;
    ihdr[10] = ihdr[11] = ihdr[12] = 0;
    if (rc == 0) rc = write_chunk(f, "IHDR", ihdr, 13);
    if (rc == 0) rc = write_chunk(f, "IDAT", zbuf, zlen);
    if (rc == 0) rc = write_chunk(f, "IEND", NULL, 0);
    if (fclose(f) != 0) rc = -1;
    free(zbuf);
    if (rc != 0) remove(path);
    return rc;
#endif
}

static int write_raw(const char *path, const uint8_t *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    int rc = fwrite(data, 1, len, f) == len ? 0 : -1;
    if (fclose(f) != 0) rc = -1;
    if (rc != 0) remove(path);
    return rc;
}

int img_save_cover(const char *dir, const char *stem, const uint8_t *data, size_t len,
                   char *out_path, size_t out_n) {
    if (out_path && out_n) out_path[0] = 0;
    if (!dir || !dir[0] || !stem || !stem[0] || !data || len < 8) return -1;
    char path[320];
    int w = 0, h = 0, side = 0;
    uint8_t *sq = img_decode_square(data, len, THUMB, &side, &w, &h);
    if (sq && side > 0) {
        uint8_t *full = sq;
        if (side != THUMB) {
            full = malloc((size_t)THUMB * THUMB * 4);
            if (full) img_crop_resize(sq, side, side, 4, full, THUMB);
            free(sq);
        }
        if (full) {
            snprintf(path, sizeof path, "%s/%s.png", dir, stem);
            int rc = img_write_png(path, full, THUMB, THUMB);
            free(full);
            if (rc == 0) {
                dbg("cover: saved thumb %s (%d x %d -> %d, src %u bytes)",
                    path, w, h, THUMB, (unsigned)len);
                if (out_path && out_n) snprintf(out_path, out_n, "%s", path);
                return 0;
            }
            dbg("cover: png write failed for %s", path);
        }
    } else {
        free(sq);
        dbg("cover: decode failed (%u bytes, maybe unsupported) — saving original", (unsigned)len);
    }

    const char *ext = ".img";
    if (data[0] == 0xFF && data[1] == 0xD8) ext = ".jpg";
    else if (data[0] == 0x89 && data[1] == 'P') ext = ".png";
    snprintf(path, sizeof path, "%s/%s%s", dir, stem, ext);
    if (write_raw(path, data, len) != 0) {
        dbg("cover: failed to write original %s", path);
        return -1;
    }
    dbg("cover: saved original %s (%u bytes)", path, (unsigned)len);
    if (out_path && out_n) snprintf(out_path, out_n, "%s", path);
    return 0;
}

/* ---- menu thumbnails and full-size player covers -------------------------------- */

int img_read_file(const char *path, uint8_t **data, size_t *len) {
    *data = NULL;
    *len = 0;
    if (!path || !path[0]) return -1;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    /* Big buffered reads; covers can be several MB on the SD2Vita. */
    setvbuf(f, NULL, _IOFBF, 256 * 1024);
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 32L * 1024 * 1024) { fclose(f); return -1; }
    uint8_t *buf = malloc((size_t)sz);
    if (!buf) { fclose(f); return -1; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) { free(buf); return -1; }
    *data = buf;
    *len = (size_t)sz;
    return 0;
}

static uint32_t rd_be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

int img_png_has_alpha(const uint8_t *data, size_t len) {
    static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    if (!data || len < 33 || memcmp(data, sig, 8) != 0 || memcmp(data + 12, "IHDR", 4) != 0) return 0;
    int ctype = data[25];
    if (ctype == 4 || ctype == 6) return 1; /* grey+alpha, RGBA */
    /* Palette / grey / RGB with a tRNS chunk (comes before the first IDAT). */
    size_t off = 8;
    while (off + 12 <= len) {
        uint32_t n = rd_be32(data + off);
        const uint8_t *type = data + off + 4;
        if (!memcmp(type, "tRNS", 4)) return 1;
        if (!memcmp(type, "IDAT", 4) || !memcmp(type, "IEND", 4)) break;
        if (n > len - off - 12) break;
        off += 12 + (size_t)n;
    }
    return 0;
}

uint8_t *img_decode_square_ex(const uint8_t *data, size_t len, int max_side, int *out_side,
                              int *src_w, int *src_h, int *scale_denom) {
    if (out_side) *out_side = 0;
    if (scale_denom) *scale_denom = 1;
    if (!data || len < 8 || max_side <= 0) return NULL;
    int w = 0, h = 0, comps = 0, sw = 0, sh = 0;
    int want = 3;
    uint8_t *px = NULL;
    int from_jpeg = 0;
#ifdef VA_HAVE_LIBJPEG
    /* 00.37: reduced-scale JPEG decode (see above); shorter side stays >= max_side. */
    if (!g_force_stb) px = jpeg_decode_scaled(data, len, max_side, &w, &h, &sw, &sh, scale_denom);
    if (px) from_jpeg = 1;
#endif
    if (!px) {
        /* 3 components for JPEG and opaque PNG: a 4000x4000 JPEG is 48 MB instead of
         * 64 MB while decoding. PNGs with transparency keep their alpha (v0.2.12). */
        want = img_png_has_alpha(data, len) ? 4 : 3;
        px = stbi_load_from_memory(data, (int)len, &w, &h, &comps, want);
        if (!px || w <= 0 || h <= 0) {
            if (px) stbi_image_free(px);
            return NULL;
        }
        sw = w;
        sh = h;
        if (scale_denom) *scale_denom = 1;
    }
    int side = w < h ? w : h;
    if (side > max_side) side = max_side;
    uint8_t *sq = malloc((size_t)side * (size_t)side * 4);
    if (sq) img_crop_resize(px, w, h, want, sq, side);
    if (from_jpeg) free(px);
    else stbi_image_free(px);
    if (sq && out_side) *out_side = side;
    if (src_w) *src_w = sw;
    if (src_h) *src_h = sh;
    return sq;
}

uint8_t *img_decode_square(const uint8_t *data, size_t len, int max_side, int *out_side,
                           int *src_w, int *src_h) {
    return img_decode_square_ex(data, len, max_side, out_side, src_w, src_h, NULL);
}

static uint64_t fnv1a64(const char *s) {
    uint64_t h = 1469598103934665603ULL;
    for (; *s; s++) { h ^= (uint8_t)*s; h *= 1099511628211ULL; }
    return h;
}

int img_cache_name(const char *dir, const char *prefix, const char *src, char *out, size_t n) {
    if (out && n) out[0] = 0;
    if (!dir || !dir[0] || !src || !src[0] || !out || n < 8 || !prefix) return -1;
    struct stat st;
    if (stat(src, &st) != 0) return -1;
    uint64_t h = fnv1a64(src);
    h ^= (uint64_t)st.st_size * 0x9E3779B97F4A7C15ULL;
    h *= 1099511628211ULL;
    h ^= (uint64_t)st.st_mtime;
    h *= 1099511628211ULL;
    snprintf(out, n, "%s/%s%016llx.png", dir, prefix, (unsigned long long)h);
    return 0;
}

int img_thumb_name(const char *dir, const char *src, char *out, size_t n) {
    return img_cache_name(dir, "t_", src, out, n);
}

static int path_in_dir(const char *dir, const char *p) {
    size_t n = strlen(dir);
    return n && !strncmp(p, dir, n) && (p[n] == '/' || p[n] == '\\');
}

static int has_png_ext(const char *p) {
    size_t n = strlen(p);
    return n > 4 && (!strcmp(p + n - 4, ".png") || !strcmp(p + n - 4, ".PNG"));
}

static int file_size_ok(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && st.st_size > 32;
}

int img_thumb_ensure(const char *dir, const char *src, char *out, size_t n, int *made) {
    if (made) *made = 0;
    if (out && n) out[0] = 0;
    if (!dir || !dir[0] || !src || !src[0] || !out) return -1;
    /* Embedded art is already saved as a 256 px PNG in the covers folder. */
    if (path_in_dir(dir, src) && has_png_ext(src)) {
        if (!file_size_ok(src)) return -1;
        snprintf(out, n, "%s", src);
        return 0;
    }
    char path[400];
    if (img_thumb_name(dir, src, path, sizeof path) != 0) return -1;
    if (file_size_ok(path)) {
        snprintf(out, n, "%s", path);
        return 0;
    }
    uint64_t t0 = now_ms();
    uint8_t *data = NULL;
    size_t len = 0;
    if (img_read_file(src, &data, &len) != 0) {
        dbg("thumb: cannot read '%s'", src);
        return -1;
    }
    uint64_t t_read = now_ms();
    int side = 0, w = 0, h = 0, denom = 1;
    uint8_t *sq = img_decode_square_ex(data, len, THUMB, &side, &w, &h, &denom);
    free(data);
    if (!sq) {
        dbg("thumb: decode failed '%s' (%u bytes)", src, (unsigned)len);
        return -1;
    }
    uint8_t *full = sq;
    if (side != THUMB) {
        /* Images smaller than 256 px: scale up to the fixed thumb size. */
        full = malloc((size_t)THUMB * THUMB * 4);
        if (full) img_crop_resize(sq, side, side, 4, full, THUMB);
        free(sq);
        if (!full) return -1;
    }
    char tmp[420];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    int rc = img_write_png(tmp, full, THUMB, THUMB);
    free(full);
    if (rc == 0) {
        remove(path);
        rc = rename(tmp, path) == 0 ? 0 : -1;
    }
    if (rc != 0) { remove(tmp); dbg("thumb: write failed '%s'", path); return -1; }
    dbg("thumb: made '%s' from '%s' (%dx%d, %u bytes, decoded at 1/%d) read %llu ms, decode+write %llu ms",
        path, src, w, h, (unsigned)len, denom, (unsigned long long)(t_read - t0),
        (unsigned long long)(now_ms() - t_read));
    snprintf(out, n, "%s", path);
    if (made) *made = 1;
    return 0;
}

uint8_t *img_player_cover(const char *dir, const char *key, int max_side,
                          int (*get)(void *ud, uint8_t **data, size_t *len), void *ud,
                          int (*stop)(void *ud), int *side, int *made, char *out, size_t n) {
    if (side) *side = 0;
    if (made) *made = 0;
    if (out && n) out[0] = 0;
    char path[400];
    if (img_cache_name(dir, IMG_PLAYER_PREFIX, key, path, sizeof path) != 0) return NULL;
    if (out) snprintf(out, n, "%s", path);
    {
        /* v0.2.11 and older saved player covers as p_<hash>.png without alpha:
         * never reuse those, and drop the old file for this key. */
        char old[400];
        if (img_cache_name(dir, "p_", key, old, sizeof old) == 0 && file_size_ok(old)) {
            remove(old);
            dbg("cover: removed old opaque player cover '%s'", old);
        }
    }
    uint64_t t0 = now_ms();
    uint8_t *data = NULL;
    size_t len = 0;
    int sd = 0, w = 0, h = 0;
    if (file_size_ok(path) && img_read_file(path, &data, &len) == 0) {
        uint8_t *px = img_decode_square(data, len, max_side, &sd, &w, &h);
        free(data);
        if (px) {
            dbg("cover: player cover cached '%s' %d px, load %llu ms", path, sd, (unsigned long long)(now_ms() - t0));
            if (side) *side = sd;
            return px;
        }
        dbg("cover: cached player cover '%s' unreadable, remaking", path);
        remove(path);
    }
    data = NULL;
    len = 0;
    if (!get || get(ud, &data, &len) != 0 || !data) return NULL;
    uint64_t t_read = now_ms();
    if (stop && stop(ud)) { free(data); return NULL; }
    int denom = 1;
    uint8_t *px = img_decode_square_ex(data, len, max_side, &sd, &w, &h, &denom);
    free(data);
    if (!px) { dbg("cover: player cover decode failed for '%s'", key); return NULL; }
    uint64_t t_dec = now_ms();
    int wrote = 0;
    if (!(stop && stop(ud))) {
        char tmp[420];
        snprintf(tmp, sizeof tmp, "%s.tmp", path);
        if (img_write_png(tmp, px, sd, sd) == 0) {
            remove(path);
            wrote = rename(tmp, path) == 0;
        }
        if (!wrote) remove(tmp);
    }
    dbg("cover: player cover made '%s' from '%s' (%dx%d at 1/%d -> %d px): read %llu ms, decode %llu ms, write %llu ms%s",
        path, key, w, h, denom, sd, (unsigned long long)(t_read - t0), (unsigned long long)(t_dec - t_read),
        (unsigned long long)(now_ms() - t_dec), wrote ? "" : " (not saved)");
    if (made) *made = wrote;
    if (side) *side = sd;
    return px;
}
