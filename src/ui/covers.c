#include "covers.h"
#include "pathutil.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <vita2d.h>

#include "dbg.h"
#include "libmodel.h"
#include "color.h"
#include "gfx.h"
#include "image.h"
#include "osal.h"
#include "quit.h"
#include "strutil.h"
#include "tagart.h"
#include "worker.h"

#include "stb_image.h"

/* Menus: only 256 px PNG thumbnails (covers/t_<hash>.png, or the embedded-art
 * PNG), made at library build / Refresh. The original image is decoded at most
 * once more, to make a missing thumb, on the lowest-priority worker.
 * Player: the 256 px thumb is shown at once (scaled up). Only after audio output
 * has started, a FULL_MAX px player cover is loaded on its own lowest-priority
 * worker: covers/p_<hash>.png if it was made before, otherwise made from the
 * original art and saved. It is cross-faded in over the thumb (no flicker), and
 * an in-flight load is dropped if the user leaves the player. */

#define SIDE 256
#define FULL_MAX 500
#define FADE_MS 350
#define MAX_COVERS 72

typedef enum { CV_EMPTY, CV_QUEUED, CV_DECODED, CV_READY, CV_FAILED } CoverState;

typedef struct {
    char id[40];
    volatile CoverState state;
    int gen;
    uint8_t *rgba;       /* SIDE*SIDE*4 while CV_DECODED */
    vita2d_texture *tex;
    uint32_t accent, backdrop;
    uint64_t used;
} Cover;

static Cover g_c[MAX_COVERS];
static Mutex g_lock;
static char g_dir[200];
static uint64_t g_frame;

typedef struct { int slot, gen; char id[40]; char thumb[320]; char src[320]; uint64_t queued; } Job;

static uint32_t to_c(Rgb c) { return RGBA(c.r, c.g, c.b, 255); }

static int file_ok(const char *p) {
    struct stat st;
    return p && p[0] && stat(p, &st) == 0 && st.st_size > 32;
}

static void job(void *ud) {
    Job *j = ud;
    uint64_t t0 = now_ms();
    if (app_quit_active()) { free(j); return; }
    /* Stale before we start (slot reused or invalidated)? */
    mutex_lock(&g_lock);
    int stale = g_c[j->slot].gen != j->gen;
    mutex_unlock(&g_lock);
    if (stale) { free(j); return; }

    char thumb[400];
    str_copy(thumb, sizeof thumb, j->thumb);
    const char *how = "thumb";
    if (!file_ok(thumb)) {
        thumb[0] = 0;
        /* Lazily (once) make the 256 px thumb from the cover source. Never an audio file. */
        if (j->src[0] && !path_is_audio(j->src)) {
            int made = 0;
            if (img_thumb_ensure(g_dir, j->src, thumb, sizeof thumb, &made) == 0) {
                how = made ? "thumb made lazily" : "thumb found";
                lib_post_thumb(j->id, j->src, thumb);
            } else {
                thumb[0] = 0;
            }
        }
    }
    uint8_t *data = NULL;
    size_t len = 0;
    if (thumb[0]) img_read_file(thumb, &data, &len);
    uint8_t *sq = NULL;
    Rgb accent = {245, 165, 36}, back = {30, 30, 36};
    if (data) {
        int w, h, comps;
        uint8_t *px = stbi_load_from_memory(data, (int)len, &w, &h, &comps, 4);
        if (px) {
            sq = malloc(SIDE * SIDE * 4);
            if (sq) {
                if (w == SIDE && h == SIDE) memcpy(sq, px, SIDE * SIDE * 4);
                else img_crop_resize(px, w, h, 4, sq, SIDE);
                accent = accent_from_rgba(sq, SIDE, SIDE);
                back = average_rgba(sq, SIDE, SIDE, 0.35f);
            }
            stbi_image_free(px);
        } else {
            dbg("covers: thumb decode failed '%s' (%u bytes)", thumb, (unsigned)len);
        }
        free(data);
    }
    dbg("covers: load id=%s %s '%s' %s (%u bytes) %llu ms (queued %llu ms)",
        j->id, how, thumb, sq ? "ok" : (j->src[0] ? "failed" : "no cover"), (unsigned)len,
        (unsigned long long)(now_ms() - t0), (unsigned long long)(t0 - j->queued));
    mutex_lock(&g_lock);
    Cover *c = &g_c[j->slot];
    if (c->gen == j->gen && c->state == CV_QUEUED) {
        c->rgba = sq;
        c->accent = to_c(accent);
        c->backdrop = to_c(back);
        c->state = sq ? CV_DECODED : CV_FAILED;
        sq = NULL;
    }
    mutex_unlock(&g_lock);
    free(sq);
    free(j);
}

/* ---- full-size player cover ------------------------------------------------------ */

typedef enum { FS_EMPTY, FS_QUEUED, FS_DECODED, FS_READY, FS_FAILED } FullState;

static struct {
    char id[40];
    volatile FullState state;
    volatile int gen;
    uint8_t *rgba;
    int side;
    vita2d_texture *tex;
    uint64_t seen_frame;     /* last frame the player drew it */
    uint64_t ready_ms;       /* texture uploaded (start of the cross-fade) */
    uint64_t requested_ms;
} F;

static Worker *g_art;

typedef struct { int gen; char id[40]; char src[320]; char audio[320]; uint64_t queued; } FullJob;

static int full_stale(int gen) { return gen != F.gen || app_quit_active(); }

static int full_get(void *ud, uint8_t **data, size_t *len) {
    FullJob *j = ud;
    if (j->src[0] && strncmp(j->src, g_dir, strlen(g_dir)) && !path_is_audio(j->src))
        return img_read_file(j->src, data, len);
    /* Embedded art: only the tag / box payload is read, never audio data. */
    if (j->audio[0]) return tagart_extract(j->audio, data, len);
    return -1;
}

static int full_stop(void *ud) { return full_stale(((FullJob *)ud)->gen); }

static void full_job(void *ud) {
    FullJob *j = ud;
    uint64_t t0 = now_ms();
    if (full_stale(j->gen)) { dbg("covers: player cover for %s cancelled before start", j->id); free(j); return; }
    /* Cache key: the folder image itself, or the audio file holding the embedded art. */
    int folder = j->src[0] && strncmp(j->src, g_dir, strlen(g_dir)) && !path_is_audio(j->src);
    const char *key = folder ? j->src : j->audio;
    int side = 0, made = 0;
    char path[400];
    uint8_t *px = key[0] ? img_player_cover(g_dir, key, FULL_MAX, full_get, j, full_stop, &side, &made, path, sizeof path)
                         : NULL;
    int ok = 0;
    mutex_lock(&g_lock);
    if (px && j->gen == F.gen && F.state == FS_QUEUED) {
        free(F.rgba);
        F.rgba = px;
        F.side = side;
        F.state = FS_DECODED;
        px = NULL;
        ok = 1;
    } else if (j->gen == F.gen && F.state == FS_QUEUED) {
        F.state = FS_FAILED;
    }
    mutex_unlock(&g_lock);
    dbg("covers: player cover id=%s %s '%s' -> %d px %s in %llu ms (queued %llu ms after audio start)",
        j->id, made ? "made from" : (ok ? "cached for" : "for"), folder ? j->src : (j->audio[0] ? j->audio : "?"),
        side, ok ? "ok" : (px ? "cancelled" : "failed"),
        (unsigned long long)(now_ms() - t0), (unsigned long long)(t0 - j->queued));
    free(px);
    free(j);
}

static void full_drop(void) {
    if (F.tex) { vita2d_wait_rendering_done(); vita2d_free_texture(F.tex); F.tex = NULL; }
    mutex_lock(&g_lock);
    F.gen++;
    free(F.rgba);
    F.rgba = NULL;
    F.state = FS_EMPTY;
    F.id[0] = 0;
    mutex_unlock(&g_lock);
}

void covers_full_request(const char *id, const char *src, const char *audio) {
    if (!id || !id[0]) return;
    F.seen_frame = g_frame;
    if (!strcmp(F.id, id) && F.state != FS_EMPTY) return;
    full_drop();
    if ((!src || !src[0]) && (!audio || !audio[0])) return;
    if (!g_art) return;
    FullJob *j = calloc(1, sizeof *j);
    if (!j) return;
    mutex_lock(&g_lock);
    str_copy(F.id, sizeof F.id, id);
    F.state = FS_QUEUED;
    j->gen = F.gen;
    mutex_unlock(&g_lock);
    str_copy(j->id, sizeof j->id, id);
    str_copy(j->src, sizeof j->src, src ? src : "");
    str_copy(j->audio, sizeof j->audio, audio ? audio : "");
    j->queued = now_ms();
    F.requested_ms = j->queued;
    if (!worker_post(g_art, full_job, j)) { free(j); F.state = FS_FAILED; return; }
    dbg("covers: player cover requested id=%s (audio is playing; 256 px thumb shown meanwhile)", id);
}

float covers_full_fade(const char *id) {
    if (!id || strcmp(F.id, id)) return 0;
    F.seen_frame = g_frame;
    if (F.state != FS_READY || !F.tex) return 0;
    uint64_t dt = now_ms() - F.ready_ms;
    if (dt >= FADE_MS) return 1;
    float t = (float)dt / FADE_MS;
    return t * t * (3 - 2 * t); /* smoothstep */
}

bool covers_full_draw(const char *id, float x, float y, float size, float alpha) {
    if (!id || strcmp(F.id, id)) return false;
    F.seen_frame = g_frame;
    if (F.state != FS_READY || !F.tex) return false;
    float sc = size / (float)F.side;
    vita2d_draw_texture_tint_scale(F.tex, (int)x, (int)y, sc, sc, gfx_alpha(0xFFFFFFFF, alpha));
    return true;
}

void covers_full_cancel(void) {
    if (F.state == FS_QUEUED) {
        dbg("covers: full cover for %s cancelled (left the player)", F.id);
        full_drop();
    }
}

static void full_tick(void) {
    /* The player did not draw for a few frames: it was left. Cancel an
     * in-flight load; a finished texture is kept for coming back. */
    if (F.state == FS_QUEUED && F.seen_frame + 3 < g_frame) covers_full_cancel();
    if (F.state != FS_DECODED) return;
    mutex_lock(&g_lock);
    uint8_t *px = F.rgba;
    int side = F.side;
    F.rgba = NULL;
    mutex_unlock(&g_lock);
    vita2d_texture *t = px ? vita2d_create_empty_texture(side, side) : NULL;
    if (t) {
        uint8_t *dst = vita2d_texture_get_datap(t);
        unsigned stride = vita2d_texture_get_stride(t);
        for (int y = 0; y < side; y++) memcpy(dst + y * stride, px + (size_t)y * side * 4, (size_t)side * 4);
        vita2d_texture_set_filters(t, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
    }
    free(px);
    F.tex = t;
    F.ready_ms = now_ms();
    F.state = t ? FS_READY : FS_FAILED;
    if (t) dbg("covers: player cover id=%s on screen %llu ms after request, cross-fading over the thumb",
               F.id, (unsigned long long)(F.ready_ms - F.requested_ms));
}

/* ---- thumbnails ------------------------------------------------------------------ */

void covers_init(const char *cache_dir) {
    mutex_init(&g_lock);
    str_copy(g_dir, sizeof g_dir, cache_dir);
    mkdir(g_dir, 0777);
    g_art = worker_create_prio("art", 128, -2);
}

void covers_exit(void) {
    mutex_lock(&g_lock);
    F.gen++;
    mutex_unlock(&g_lock);
    if (g_art) {
        if (worker_pending(g_art) > 0) worker_abandon(g_art);
        else worker_destroy_timeout(g_art, 500);
        g_art = NULL;
    }
    if (F.tex) vita2d_free_texture(F.tex);
    F.tex = NULL;
    for (int i = 0; i < MAX_COVERS; i++) {
        if (g_c[i].tex) vita2d_free_texture(g_c[i].tex);
        g_c[i].tex = NULL;
    }
}

static Cover *lookup(const char *id, bool request) {
    if (!id || !id[0]) return NULL;
    for (int i = 0; i < MAX_COVERS; i++)
        if (g_c[i].state != CV_EMPTY && !strcmp(g_c[i].id, id)) {
            g_c[i].used = g_frame;
            return &g_c[i];
        }
    if (!request) return NULL;
    /* a free slot, else the least recently used one not drawn this frame */
    int slot = -1;
    uint64_t oldest = UINT64_MAX;
    mutex_lock(&g_lock);
    for (int i = 0; i < MAX_COVERS; i++) {
        if (g_c[i].state == CV_EMPTY) { slot = i; break; }
        if (g_c[i].state != CV_QUEUED && g_c[i].used + 1 < g_frame && g_c[i].used < oldest) { oldest = g_c[i].used; slot = i; }
    }
    if (slot < 0) { mutex_unlock(&g_lock); return NULL; }
    Cover *c = &g_c[slot];
    if (c->tex) { vita2d_wait_rendering_done(); vita2d_free_texture(c->tex); c->tex = NULL; }
    free(c->rgba);
    c->rgba = NULL;
    str_copy(c->id, sizeof c->id, id);
    c->gen++;
    c->state = CV_QUEUED;
    c->used = g_frame;
    c->accent = C_ACCENT;
    c->backdrop = RGBA(30, 30, 36, 255);
    mutex_unlock(&g_lock);
    Job *j = calloc(1, sizeof *j);
    if (j) {
        j->slot = slot;
        j->gen = c->gen;
        j->queued = now_ms();
        str_copy(j->id, sizeof j->id, id);
        /* Paths are copied here on the UI thread; the worker never reads the library. */
        int bi = lib_find_id(id);
        const BookSummary *b = bi >= 0 ? lib_book(bi) : NULL;
        if (b) {
            str_copy(j->thumb, sizeof j->thumb, b->thumb_path);
            str_copy(j->src, sizeof j->src, b->cover_path);
        }
        if (!worker_post(g_covers, job, j)) { free(j); c->state = CV_EMPTY; return NULL; }
    }
    return c;
}

void covers_invalidate(const char *id) {
    if (!id || !id[0]) return;
    mutex_lock(&g_lock);
    for (int i = 0; i < MAX_COVERS; i++) {
        Cover *c = &g_c[i];
        if (c->state == CV_EMPTY || strcmp(c->id, id)) continue;
        if (c->state == CV_QUEUED) { c->gen++; } /* ignore in-flight job */
        if (c->tex) { vita2d_wait_rendering_done(); vita2d_free_texture(c->tex); c->tex = NULL; }
        free(c->rgba);
        c->rgba = NULL;
        c->state = CV_EMPTY;
        c->id[0] = 0;
        c->gen++;
    }
    mutex_unlock(&g_lock);
}

void covers_reset(void) {
    mutex_lock(&g_lock);
    for (int i = 0; i < MAX_COVERS; i++) {
        Cover *c = &g_c[i];
        if (c->state == CV_EMPTY) continue;
        if (c->tex) { vita2d_wait_rendering_done(); vita2d_free_texture(c->tex); c->tex = NULL; }
        free(c->rgba);
        c->rgba = NULL;
        c->state = CV_EMPTY;
        c->id[0] = 0;
        c->gen++; /* queued jobs see a stale generation and do nothing */
    }
    mutex_unlock(&g_lock);
    full_drop();
    dbg("covers: reset (library folder changed)");
}

void covers_tick(void) {
    g_frame++;
    full_tick();
    int uploads = 0;
    for (int i = 0; i < MAX_COVERS && uploads < 2; i++) {
        Cover *c = &g_c[i];
        if (c->state != CV_DECODED) continue;
        mutex_lock(&g_lock);
        uint8_t *px = c->rgba;
        c->rgba = NULL;
        mutex_unlock(&g_lock);
        vita2d_texture *t = px ? vita2d_create_empty_texture(SIDE, SIDE) : NULL;
        if (t) {
            uint8_t *dst = vita2d_texture_get_datap(t);
            unsigned stride = vita2d_texture_get_stride(t);
            for (int y = 0; y < SIDE; y++) memcpy(dst + y * stride, px + y * SIDE * 4, SIDE * 4);
            vita2d_texture_set_filters(t, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
        }
        free(px);
        c->tex = t;
        c->state = t ? CV_READY : CV_FAILED;
        uploads++;
    }
}

void covers_draw(const char *id, float x, float y, float size, float alpha) {
    Cover *c = lookup(id, true);
    if (c && c->state == CV_READY && c->tex) {
        vita2d_draw_texture_tint_scale(c->tex, (int)x, (int)y, size / SIDE, size / SIDE, gfx_alpha(0xFFFFFFFF, alpha));
        return;
    }
    gfx_rect(x, y, size, size, gfx_alpha(C_PANEL2, alpha));
    gfx_icon(IC_BOOK, x + size / 2, y + size / 2, (int)(size * 0.32f), gfx_alpha(C_DIM, alpha));
}

uint32_t covers_accent(const char *id, uint32_t fallback) {
    Cover *c = lookup(id, false);
    return c && (c->state == CV_READY || c->state == CV_DECODED) ? c->accent : fallback;
}

uint32_t covers_backdrop(const char *id, uint32_t fallback) {
    Cover *c = lookup(id, false);
    return c && (c->state == CV_READY || c->state == CV_DECODED) ? c->backdrop : fallback;
}
