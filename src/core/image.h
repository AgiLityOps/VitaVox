/* Image helpers for cover art (pure C). */
#ifndef VITAVOX_IMAGE_H
#define VITAVOX_IMAGE_H
#include <stddef.h>
#include <stdint.h>

/* Box resize + centre crop to a square of size out_sz. src has comps bytes per
 * pixel (1..4); dst is out_sz*out_sz RGBA8. */
void img_crop_resize(const uint8_t *src, int sw, int sh, int comps, uint8_t *dst, int out_sz);

/* Write an uncompressed-filter PNG (zlib IDAT) of RGBA8 pixels. 0 on success. */
int img_write_png(const char *path, const uint8_t *rgba, int w, int h);

/* Save cover bytes under dir/stem.png (256 px thumbnail) when the image can be
 * decoded, otherwise dir/stem.jpg or .png with the original bytes. 0 on success
 * and out_path is filled with the written path. */
int img_save_cover(const char *dir, const char *stem, const uint8_t *data, size_t len,
                   char *out_path, size_t out_n);

/* Whole file into memory (malloc'd). 0 ok. */
int img_read_file(const char *path, uint8_t **data, size_t *len);

/* Decode (JPEG/PNG, baseline or progressive) and centre-crop to a square of
 * min(shorter side, max_side). RGBA8, malloc'd; *out_side is the edge. */
/* Player-cover cache file prefix. "p_" (v0.2.8-v0.2.11) dropped PNG alpha;
 * "pa_" files keep it. Bump this to invalidate cached player covers. */
#define IMG_PLAYER_PREFIX "pa_"
/* 1 if data is a PNG with an alpha channel or a tRNS chunk. */
int img_png_has_alpha(const uint8_t *data, size_t len);
uint8_t *img_decode_square(const uint8_t *data, size_t len, int max_side, int *out_side,
                           int *src_w, int *src_h);
/* 00.37: as img_decode_square; *scale_denom = the JPEG reduced-scale decode
 * used (1, 2, 4 or 8; 1 for PNG or without libjpeg). src_w/src_h stay the
 * original size. */
uint8_t *img_decode_square_ex(const uint8_t *data, size_t len, int max_side, int *out_side,
                              int *src_w, int *src_h, int *scale_denom);
/* Largest 1/d (d = 8, 4, 2, 1) that keeps min(w, h)/d >= min_side. */
int img_jpeg_scale_for(int w, int h, int min_side);
/* 1 when built with libjpeg (reduced-scale JPEG decode). */
int img_have_scaled_jpeg(void);
/* Tests/benchmarks: decode JPEGs with stb_image at full size (the 00.36 path). */
void img_test_force_stb(int on);

/* Menu thumbnail cache: dir/t_<hash(src path, size, mtime)>.png (256 px).
 * img_thumb_name only computes the name (stat of src). img_thumb_ensure makes
 * the thumb if missing; a PNG already inside dir (embedded art) is used as is.
 * 0 ok with out filled; *made = 1 when a new file was written. */
int img_thumb_name(const char *dir, const char *src, char *out, size_t n);
int img_thumb_ensure(const char *dir, const char *src, char *out, size_t n, int *made);

/* dir/<prefix><hash(src path, size, mtime)>.png */
int img_cache_name(const char *dir, const char *prefix, const char *src, char *out, size_t n);

/* Player cover cache: dir/pa_<hash(key)>.png at max_side px (key = the folder image,
 * or the audio file for embedded art). Cached: decoded from the PNG. Otherwise
 * get() supplies the original bytes, which are decoded, centre-cropped to
 * max_side and saved for next time (unless stop() says the request is stale).
 * Returns malloc'd RGBA (side x side) or NULL; *made = 1 when the PNG was written. */
uint8_t *img_player_cover(const char *dir, const char *key, int max_side,
                          int (*get)(void *ud, uint8_t **data, size_t *len), void *ud,
                          int (*stop)(void *ud), int *side, int *made, char *out, size_t n);

#endif
