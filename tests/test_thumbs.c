/* Menu thumbnail cache (image.c): 256 px PNGs keyed by source path+size+mtime,
 * and the 500 px player cover cache (covers/pa_<hash>.png). */
#include "test.h"
#include "image.h"
#include "stb_image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>
#include <time.h>

static int have_pil(void) {
    return system("python3 -c 'import PIL' >/dev/null 2>&1") == 0;
}

/* Big progressive JPEG like a 5.7 MB cover from a multi-disc rip. */
static int make_jpeg(const char *path, int w, int h, int progressive) {
    char cmd[600];
    snprintf(cmd, sizeof cmd,
             "python3 -c \"from PIL import Image; import random; im=Image.effect_noise((%d,%d),60).convert('RGB'); "
             "im.save('%s', quality=95, progressive=%s)\"",
             w, h, path, progressive ? "True" : "False");
    return system(cmd);
}

static int png_size(const char *path, int *w, int *h) {
    uint8_t *d = NULL;
    size_t n = 0;
    if (img_read_file(path, &d, &n) != 0) return -1;
    int c;
    uint8_t *px = stbi_load_from_memory(d, (int)n, w, h, &c, 4);
    free(d);
    if (!px) return -1;
    stbi_image_free(px);
    return 0;
}

TEST(thumb_from_big_progressive_jpeg) {
    if (!have_pil()) { printf("  note: PIL missing, thumb test skipped\n"); return; }
    const char *dir = "build-host/tmp-thumbs";
    char cmd[300];
    snprintf(cmd, sizeof cmd, "rm -rf %s && mkdir -p %s/covers", dir, dir);
    CHECK_EQ_INT(system(cmd), 0);
    char src[200], covers[200];
    snprintf(src, sizeof src, "%s/Big Cover.jpg", dir);
    snprintf(covers, sizeof covers, "%s/covers", dir);
    CHECK_EQ_INT(make_jpeg(src, 2400, 2400, 1), 0);

    char t1[400];
    int made = -1;
    CHECK_EQ_INT(img_thumb_ensure(covers, src, t1, sizeof t1, &made), 0);
    CHECK_EQ_INT(made, 1);
    CHECK(strstr(t1, "/covers/t_") != NULL);
    int w = 0, h = 0;
    CHECK_EQ_INT(png_size(t1, &w, &h), 0);
    CHECK_EQ_INT(w, 256);
    CHECK_EQ_INT(h, 256);
    struct stat st;
    CHECK(stat(t1, &st) == 0 && st.st_size < 400 * 1024);

    /* Second time: same key, no decode. */
    char t2[400];
    CHECK_EQ_INT(img_thumb_ensure(covers, src, t2, sizeof t2, &made), 0);
    CHECK_EQ_INT(made, 0);
    CHECK_STR(t2, t1);

    /* Changed image (mtime) -> new key -> regenerated. */
    struct utimbuf ut = { 1000000000, 1000000000 };
    CHECK_EQ_INT(utime(src, &ut), 0);
    char t3[400];
    CHECK_EQ_INT(img_thumb_ensure(covers, src, t3, sizeof t3, &made), 0);
    CHECK_EQ_INT(made, 1);
    CHECK(strcmp(t3, t1) != 0);

    /* Embedded-art PNG already in the covers folder is used as is. */
    char t4[400];
    CHECK_EQ_INT(img_thumb_ensure(covers, t1, t4, sizeof t4, &made), 0);
    CHECK_EQ_INT(made, 0);
    CHECK_STR(t4, t1);

    /* Missing source -> error, no file. */
    char t5[400];
    CHECK(img_thumb_ensure(covers, "build-host/tmp-thumbs/nope.jpg", t5, sizeof t5, &made) != 0);
    CHECK_EQ_INT(t5[0], 0);

    /* Full-size player decode is capped at 544 px. */
    uint8_t *d = NULL;
    size_t n = 0;
    CHECK_EQ_INT(img_read_file(src, &d, &n), 0);
    int side = 0, sw = 0, sh = 0;
    uint8_t *px = img_decode_square(d, n, 544, &side, &sw, &sh);
    CHECK(px != NULL);
    CHECK_EQ_INT(side, 544);
    CHECK_EQ_INT(sw, 2400);
    free(px);
    free(d);

    /* Small (non-square, baseline) image: thumb still 256 px. */
    char small[200];
    snprintf(small, sizeof small, "%s/small.jpg", dir);
    CHECK_EQ_INT(make_jpeg(small, 120, 90, 0), 0);
    char t6[400];
    CHECK_EQ_INT(img_thumb_ensure(covers, small, t6, sizeof t6, &made), 0);
    CHECK_EQ_INT(png_size(t6, &w, &h), 0);
    CHECK_EQ_INT(w, 256);
}

typedef struct { const char *src; int calls; int stop; } GetCtx;

static int get_bytes(void *ud, uint8_t **d, size_t *n) {
    GetCtx *g = ud;
    g->calls++;
    return img_read_file(g->src, d, n);
}

static int get_stop(void *ud) { return ((GetCtx *)ud)->stop; }

TEST(player_cover_500_made_once_then_cached) {
    if (!have_pil()) { printf("  note: PIL missing, player cover test skipped\n"); return; }
    const char *dir = "build-host/tmp-pcover";
    char cmd[300];
    snprintf(cmd, sizeof cmd, "rm -rf %s && mkdir -p %s/covers", dir, dir);
    CHECK_EQ_INT(system(cmd), 0);
    char src[200], covers[200];
    snprintf(src, sizeof src, "%s/folder.jpg", dir);
    snprintf(covers, sizeof covers, "%s/covers", dir);
    CHECK_EQ_INT(make_jpeg(src, 1800, 1600, 1), 0);

    /* Stale request: decoded but not saved. */
    GetCtx g = { src, 0, 1 };
    int side = 0, made = -1;
    char p0[400];
    uint8_t *px = img_player_cover(covers, src, 500, get_bytes, &g, get_stop, &side, &made, p0, sizeof p0);
    free(px);
    CHECK_EQ_INT(made, 0);
    struct stat st;
    CHECK(stat(p0, &st) != 0);

    g.stop = 0;
    g.calls = 0;
    char p1[400];
    px = img_player_cover(covers, src, 500, get_bytes, &g, get_stop, &side, &made, p1, sizeof p1);
    CHECK(px != NULL);
    CHECK_EQ_INT(side, 500);
    CHECK_EQ_INT(made, 1);
    CHECK_EQ_INT(g.calls, 1);
    CHECK(strstr(p1, "/covers/" IMG_PLAYER_PREFIX) != NULL);
    int w = 0, h = 0;
    CHECK_EQ_INT(png_size(p1, &w, &h), 0);
    CHECK_EQ_INT(w, 500);
    CHECK_EQ_INT(h, 500);

    /* Next play: straight from the cache, original never read, same pixels. */
    g.calls = 0;
    char p2[400];
    int side2 = 0;
    uint8_t *px2 = img_player_cover(covers, src, 500, get_bytes, &g, get_stop, &side2, &made, p2, sizeof p2);
    CHECK(px2 != NULL);
    CHECK_EQ_INT(made, 0);
    CHECK_EQ_INT(g.calls, 0);
    CHECK_EQ_INT(side2, 500);
    CHECK_STR(p2, p1);
    if (px && px2) CHECK(memcmp(px, px2, 500 * 500 * 4) == 0);
    free(px);
    free(px2);

    /* Different from the menu thumb name; small art is not scaled past its size. */
    char t[400];
    CHECK_EQ_INT(img_thumb_name(covers, src, t, sizeof t), 0);
    CHECK(strcmp(t, p1) != 0);
    char small[200];
    snprintf(small, sizeof small, "%s/small.jpg", dir);
    CHECK_EQ_INT(make_jpeg(small, 300, 300, 0), 0);
    GetCtx gs = { small, 0, 0 };
    px = img_player_cover(covers, small, 500, get_bytes, &gs, get_stop, &side, &made, p0, sizeof p0);
    CHECK(px != NULL);
    CHECK_EQ_INT(side, 300);
    free(px);
}

/* RGBA PNG: transparent outside a centred opaque disc; palette PNG with tRNS. */
static int make_png_alpha(const char *path, int side, int palette) {
    char cmd[900];
    snprintf(cmd, sizeof cmd,
             "python3 -c \"from PIL import Image, ImageDraw; s=%d; im=Image.new('RGBA',(s,s),(0,0,0,0)); "
             "d=ImageDraw.Draw(im); d.ellipse((s//4,s//4,3*s//4,3*s//4), fill=(20,200,40,255)); "
             "im=(im.convert('P', palette=Image.ADAPTIVE, colors=8) if %d else im); "
             "%s im.save('%s')\"",
             side, palette,
             palette ? "im.info['transparency']=im.getpixel((0,0)); " : "", path);
    return system(cmd);
}

static unsigned char px_a(const uint8_t *px, int side, int x, int y) { return px[((size_t)y * side + x) * 4 + 3]; }

TEST(player_cover_keeps_png_alpha) {
    if (!have_pil()) { printf("  note: PIL missing, alpha cover test skipped\n"); return; }
    const char *dir = "build-host/tmp-acover";
    char cmd[300];
    snprintf(cmd, sizeof cmd, "rm -rf %s && mkdir -p %s/covers", dir, dir);
    CHECK_EQ_INT(system(cmd), 0);
    char covers[200];
    snprintf(covers, sizeof covers, "%s/covers", dir);
    const char *names[2] = { "rgba.png", "palette.png" };
    for (int k = 0; k < 2; k++) {
        char src[200];
        snprintf(src, sizeof src, "%s/%s", dir, names[k]);
        t_current = names[k];
        CHECK_EQ_INT(make_png_alpha(src, 800, k), 0);
        uint8_t *d = NULL;
        size_t n = 0;
        CHECK_EQ_INT(img_read_file(src, &d, &n), 0);
        CHECK_EQ_INT(img_png_has_alpha(d, n), 1);
        free(d);

        /* An old opaque v0.2.11 player cover for this key is not reused and is removed. */
        char old[400];
        CHECK_EQ_INT(img_cache_name(covers, "p_", src, old, sizeof old), 0);
        uint8_t opaque[16 * 16 * 4];
        memset(opaque, 255, sizeof opaque);
        CHECK_EQ_INT(img_write_png(old, opaque, 16, 16), 0);

        GetCtx g = { src, 0, 0 };
        int side = 0, made = -1;
        char p1[400];
        uint8_t *px = img_player_cover(covers, src, 500, get_bytes, &g, get_stop, &side, &made, p1, sizeof p1);
        CHECK(px != NULL);
        CHECK_EQ_INT(g.calls, 1);
        CHECK_EQ_INT(made, 1);
        CHECK_EQ_INT(side, 500);
        struct stat st;
        CHECK(stat(old, &st) != 0);
        if (px) {
            CHECK_EQ_INT(px_a(px, 500, 5, 5), 0);       /* corner: clear */
            CHECK_EQ_INT(px_a(px, 500, 250, 250), 255); /* centre: opaque */
            CHECK(px[(250 * 500 + 250) * 4 + 1] > 150);  /* green */
            /* edge pixels: no dark fringe from the transparent black around the disc */
            int fringe = 0;
            for (int x = 0; x < 500; x++) {
                const uint8_t *q = px + ((size_t)250 * 500 + x) * 4;
                if (q[3] > 0 && q[3] < 255 && q[1] < 150) fringe++;
            }
            CHECK_EQ_INT(fringe, 0);
        }
        /* Reloaded from the pa_ cache: alpha survives the save and load. */
        g.calls = 0;
        uint8_t *px2 = img_player_cover(covers, src, 500, get_bytes, &g, get_stop, &side, &made, p1, sizeof p1);
        CHECK(px2 != NULL);
        CHECK_EQ_INT(g.calls, 0);
        if (px && px2) {
            CHECK_EQ_INT(px_a(px2, 500, 5, 5), 0);
            CHECK(memcmp(px, px2, 500 * 500 * 4) == 0);
        }
        free(px);
        free(px2);

        /* Menu thumb from the same PNG keeps alpha too. */
        char t[400];
        CHECK_EQ_INT(img_thumb_ensure(covers, src, t, sizeof t, &made), 0);
        CHECK_EQ_INT(img_read_file(t, &d, &n), 0);
        int w = 0, h = 0, c = 0;
        uint8_t *tp = d ? stbi_load_from_memory(d, (int)n, &w, &h, &c, 4) : NULL;
        CHECK(tp != NULL);
        if (tp) {
            CHECK_EQ_INT(px_a(tp, 256, 2, 2), 0);
            CHECK_EQ_INT(px_a(tp, 256, 128, 128), 255);
            stbi_image_free(tp);
        }
        free(d);
    }
    /* JPEG art stays opaque (decoded as RGB). */
    char jpg[200];
    snprintf(jpg, sizeof jpg, "%s/art.jpg", dir);
    CHECK_EQ_INT(make_jpeg(jpg, 600, 600, 0), 0);
    GetCtx gj = { jpg, 0, 0 };
    int side = 0, made = 0;
    char pj[400];
    uint8_t *px = img_player_cover(covers, jpg, 500, get_bytes, &gj, get_stop, &side, &made, pj, sizeof pj);
    CHECK(px != NULL);
    if (px) {
        int all = 1;
        for (int i = 0; i < 500 * 500; i++) if (px[i * 4 + 3] != 255) { all = 0; break; }
        CHECK(all);
    }
    free(px);
    uint8_t jd[4] = { 0xFF, 0xD8, 0xFF, 0xE0 };
    CHECK_EQ_INT(img_png_has_alpha(jd, sizeof jd), 0);
}

/* 00.37: reduced-scale JPEG decode for thumbs (256) and the player cover (500). */
TEST(jpeg_scale_choice) {
    if (!img_have_scaled_jpeg()) {
        CHECK_EQ_INT(img_jpeg_scale_for(2920, 3072, 256), 1);
        return;
    }
    CHECK_EQ_INT(img_jpeg_scale_for(2920, 3072, 256), 8); /* 365x384 */
    CHECK_EQ_INT(img_jpeg_scale_for(2920, 3072, 500), 4); /* 730x768 */
    CHECK_EQ_INT(img_jpeg_scale_for(2400, 2400, 256), 8); /* 300 */
    CHECK_EQ_INT(img_jpeg_scale_for(2400, 2400, 500), 4); /* 600 */
    CHECK_EQ_INT(img_jpeg_scale_for(1200, 1200, 256), 4); /* 300 */
    CHECK_EQ_INT(img_jpeg_scale_for(1200, 1200, 500), 2); /* 600 */
    CHECK_EQ_INT(img_jpeg_scale_for(1000, 3000, 500), 2); /* shorter side decides: 500 */
    CHECK_EQ_INT(img_jpeg_scale_for(999, 999, 500), 2);   /* ceil(999/2) = 500 */
    CHECK_EQ_INT(img_jpeg_scale_for(997, 997, 500), 1);   /* 499 would be too small */
    CHECK_EQ_INT(img_jpeg_scale_for(500, 500, 256), 1);
    CHECK_EQ_INT(img_jpeg_scale_for(300, 300, 256), 1);
}

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* Mean absolute difference of two 256 px RGBA thumbs (0..255). */
static double thumb_diff(const char *a, const char *b) {
    uint8_t *da = NULL, *db = NULL;
    size_t na = 0, nb = 0;
    if (img_read_file(a, &da, &na) != 0 || img_read_file(b, &db, &nb) != 0) { free(da); free(db); return 999; }
    int w1, h1, w2, h2, c;
    uint8_t *pa = stbi_load_from_memory(da, (int)na, &w1, &h1, &c, 3);
    uint8_t *pb = stbi_load_from_memory(db, (int)nb, &w2, &h2, &c, 3);
    free(da);
    free(db);
    double d = 999;
    if (pa && pb && w1 == w2 && h1 == h2) {
        double acc = 0;
        for (int i = 0; i < w1 * h1 * 3; i++) acc += abs((int)pa[i] - (int)pb[i]);
        d = acc / (w1 * h1 * 3);
    }
    if (pa) stbi_image_free(pa);
    if (pb) stbi_image_free(pb);
    return d;
}

static int make_cover_like(const char *path, int w, int h, int quality, int progressive) {
    char cmd[700];
    snprintf(cmd, sizeof cmd,
             "python3 -c \"from PIL import Image; n=Image.effect_noise((%d,%d),40).convert('RGB'); "
             "g=Image.linear_gradient('L').resize((%d,%d)).convert('RGB'); "
             "Image.blend(n,g,0.5).save('%s', quality=%d, progressive=%s)\"",
             w, h, w, h, path, quality, progressive ? "True" : "False");
    return system(cmd);
}

/* A 2920x3072, 5.7 MB progressive JPEG cover: 10.3 s for its thumb on the Vita
 * (00.36, stb_image at full size). Host timing of both paths. */
TEST(big_jpeg_thumb_scaled_decode_speedup) {
    if (!have_pil()) { printf("  note: PIL missing, benchmark skipped\n"); return; }
    if (!img_have_scaled_jpeg()) { printf("  note: built without libjpeg, benchmark skipped\n"); return; }
    const char *dir = "build-host/tmp-thumbbench";
    char cmd[300];
    snprintf(cmd, sizeof cmd, "rm -rf %s && mkdir -p %s/a %s/b", dir, dir, dir);
    CHECK_EQ_INT(system(cmd), 0);
    for (int prog = 1; prog >= 0; prog--) {
        char src[200], ca[200], cb[200];
        snprintf(src, sizeof src, "%s/largecover_like_%s.jpg", dir, prog ? "progressive" : "baseline");
        snprintf(ca, sizeof ca, "%s/a", dir);
        snprintf(cb, sizeof cb, "%s/b", dir);
        CHECK_EQ_INT(make_cover_like(src, 2920, 3072, 96, prog), 0);
        struct stat st;
        stat(src, &st);
        char ta[400], tb[400];
        int made = 0;
        /* best of 3 for each path (fresh file each time) */
        double best_stb = 1e9, best_jpg = 1e9;
        for (int r = 0; r < 3; r++) {
            img_test_force_stb(1);
            double t0 = now_s();
            CHECK_EQ_INT(img_thumb_ensure(cb, src, tb, sizeof tb, &made), 0);
            double t1 = now_s();
            img_test_force_stb(0);
            remove(tb);
            double t2 = now_s();
            CHECK_EQ_INT(img_thumb_ensure(ca, src, ta, sizeof ta, &made), 0);
            double t3 = now_s();
            if (r < 2) remove(ta);
            if (t1 - t0 < best_stb) best_stb = t1 - t0;
            if (t3 - t2 < best_jpg) best_jpg = t3 - t2;
        }
        /* keep one stb thumb to compare the pictures */
        img_test_force_stb(1);
        CHECK_EQ_INT(img_thumb_ensure(cb, src, tb, sizeof tb, &made), 0);
        img_test_force_stb(0);
        int w = 0, h = 0;
        CHECK_EQ_INT(png_size(ta, &w, &h), 0);
        CHECK_EQ_INT(w, 256);
        CHECK_EQ_INT(h, 256);
        double diff = thumb_diff(ta, tb);
        double ratio = best_stb / best_jpg;
        printf("  thumb 2920x3072 %s JPEG (%.1f MB): stb full decode %.0f ms, libjpeg 1/8 decode %.0f ms -> %.1fx faster "
               "(mean pixel diff %.1f/255; Vita estimate %.1f s instead of 10.3 s)\n",
               prog ? "progressive" : "baseline", st.st_size / 1048576.0, best_stb * 1000, best_jpg * 1000, ratio, diff,
               10.3 / ratio);
        CHECK(ratio > 3.0);
        CHECK(diff < 8.0);
        /* player cover: 500 px from a 1/4 decode */
        uint8_t *d = NULL;
        size_t n = 0;
        CHECK_EQ_INT(img_read_file(src, &d, &n), 0);
        int side = 0, sw = 0, sh = 0, den = 0;
        uint8_t *px = img_decode_square_ex(d, n, 500, &side, &sw, &sh, &den);
        CHECK(px != NULL);
        CHECK_EQ_INT(side, 500);
        CHECK_EQ_INT(den, 4);
        CHECK_EQ_INT(sw, 2920);
        CHECK_EQ_INT(sh, 3072);
        free(px);
        px = img_decode_square_ex(d, n, 256, &side, &sw, &sh, &den);
        CHECK(px != NULL);
        CHECK_EQ_INT(side, 256);
        CHECK_EQ_INT(den, 8);
        free(px);
        free(d);
    }
    /* Not a JPEG libjpeg takes (PNG) and damaged bytes: stb path / clean failure. */
    uint8_t junk[64];
    memset(junk, 0, sizeof junk);
    junk[0] = 0xFF; junk[1] = 0xD8;
    int side = 0;
    CHECK(img_decode_square(junk, sizeof junk, 256, &side, NULL, NULL) == NULL);
}

void suite_thumbs(void) {
    RUN(jpeg_scale_choice);
    RUN(big_jpeg_thumb_scaled_decode_speedup);
    RUN(thumb_from_big_progressive_jpeg);
    RUN(player_cover_500_made_once_then_cached);
    RUN(player_cover_keeps_png_alpha);
}
