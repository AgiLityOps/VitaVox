/* Settings: library folder, playback defaults, refresh, about. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "covers.h"
#include "ime.h"
#include "libroot.h"
#include "libmodel.h"
#include "player.h"
#include "strutil.h"
#include "views.h"
#include "wavrec.h"

#define ROW_H 64.0f
#define TOP 80.0f

enum { S_LIBRARY, S_SKIP_BACK, S_SKIP_FWD, S_SPEED, S_REFRESH, S_ABOUT, S_COUNT, S_DEBUG_REC = S_COUNT };

/* Hidden debug row: OK on "About" five times shows "Debug: record output (60 s)"
 * (until the app is closed). It records the exact PCM sent to the audio device
 * to ux0:data/VitaVox/debug/out_<date>_<time>.wav. */
#define DEBUG_TAPS 5
#define DEBUG_REC_S 60
static int g_about_taps;
static bool g_debug_shown;
static char g_rec_name[96];

static int row_count(void) { return g_debug_shown ? S_COUNT + 1 : S_COUNT; }

typedef struct {
    int focus;
    Scroll sc;
} Settings;

static Settings *g_set;

/* ---- library folder change (00.33) ------------------------------------------- */

static const char *op_current_root(void *ud) { (void)ud; return lib_root(); }

/* Stop playback completely before the folder changes, so nothing from the old
 * folder can keep playing or be resumed. */
static void op_stop_playback(void *ud) {
    (void)ud;
    player_close();     /* saves progress, stops the engine and its preopen, forgets the book */
    app_close_player(); /* leaves the player screen; the mini player has nothing to show */
    covers_reset();     /* queued cover loads and textures of the old folder */
}

static void op_switch_root(void *ud, const char *norm) {
    (void)ud;
    str_copy(g_cfg.library_path, sizeof g_cfg.library_path, norm);
    app_save_config();
    lib_switch_root(norm);
}

static void op_refresh(void *ud) { (void)ud; lib_refresh(); }

static void change_library_folder(const char *typed) {
    LibSwitchOps ops = {NULL, op_current_root, op_stop_playback, op_switch_root, op_refresh};
    char msg[200];
    libswitch_apply(typed, &ops, msg, sizeof msg);
    app_toast(msg);
}

static void activate(Settings *st, int i) {
    (void)st;
    switch (i) {
    case S_LIBRARY:
        ime_open("Library folder", g_cfg.library_path, 240, IME_TEXT);
        return;
    case S_SKIP_BACK: g_cfg.skip_back = cfg_next_skip(g_cfg.skip_back); break;
    case S_SKIP_FWD: g_cfg.skip_fwd = cfg_next_skip(g_cfg.skip_fwd); break;
    case S_SPEED: player_set_speed(cfg_next_speed(g_cfg.speed)); break;
    case S_REFRESH: lib_refresh(); app_toast("Refreshing your library\xE2\x80\xA6"); return;
    case S_ABOUT:
        if (!g_debug_shown && ++g_about_taps >= DEBUG_TAPS) {
            g_debug_shown = true;
            app_toast("Debug options shown");
        }
        return;
    case S_DEBUG_REC: {
        if (wavrec_active()) {
            wavrec_stop();
            app_toast("Recording stopped");
            return;
        }
        char path[320];
        if (wavrec_start(DEBUG_REC_S, path, sizeof path) != 0) {
            app_toast("Could not start the recording");
            return;
        }
        const char *base = strrchr(path, '/');
        str_copy(g_rec_name, sizeof g_rec_name, base ? base + 1 : path);
        app_toast(player_playing() ? "Recording the next 60 s of playback" : "Recording starts when you press play");
        return;
    }
    default: return;
    }
    app_save_config();
}

static void on_tap(int i, int b) {
    (void)b;
    if (!g_set) return;
    g_set->focus = i;
    activate(g_set, i);
}

static void label(int i, char *name, char *val, int n) {
    switch (i) {
    case S_LIBRARY: snprintf(name, n, "Library folder"); snprintf(val, n, "%s", g_cfg.library_path); break;
    case S_SKIP_BACK: snprintf(name, n, "Skip back"); snprintf(val, n, "%d s", g_cfg.skip_back); break;
    case S_SKIP_FWD: snprintf(name, n, "Skip forward"); snprintf(val, n, "%d s", g_cfg.skip_fwd); break;
    case S_SPEED: snprintf(name, n, "Playback speed"); snprintf(val, n, "%g\xC3\x97", g_cfg.speed); break;
    case S_REFRESH: snprintf(name, n, "Refresh library"); snprintf(val, n, "%d books", lib_count()); break;
    case S_ABOUT: snprintf(name, n, "About"); snprintf(val, n, "Vita Vox %s", APP_VERSION); break;
    case S_DEBUG_REC:
        if (wavrec_active()) {
            snprintf(name, n, "Debug: recording output\xE2\x80\xA6");
            snprintf(val, n, "%.0f / %d s \xC2\xB7 %s", wavrec_seconds(), DEBUG_REC_S, g_rec_name);
        } else {
            snprintf(name, n, "Debug: record output (%d s)", DEBUG_REC_S);
            if (g_rec_name[0]) snprintf(val, n, "Saved debug/%s", g_rec_name);
            else snprintf(val, n, "Off");
        }
        break;
    }
}

static const int kIcon[S_COUNT + 1] = {IC_BOOK, IC_BACK, IC_FWD, IC_SPEED, IC_SEARCH, IC_INFO, IC_INFO};

static void settings_update(View *v, const Input *in, float dt) {
    Settings *st = v->data;
    scroll_step(&st->sc, dt);
    if (ime_active()) {
        char out[256];
        int r = ime_poll(out, sizeof out);
        if (r > 0) change_library_folder(out);
        return;
    }
    if (in->repeat & B_UP && st->focus > 0) st->focus--;
    if (in->repeat & B_DOWN && st->focus + 1 < row_count()) st->focus++;
    if (in->repeat & B_LEFT || in->down & B_BACK) { app_focus_sidebar(); return; }
    if (in->down & B_OK) activate(st, st->focus);
    float max = row_count() * ROW_H - (CONTENT_BOTTOM - TOP) + 8;
    scroll_show(&st->sc, st->focus * ROW_H, (st->focus + 1) * ROW_H, CONTENT_BOTTOM - TOP, max > 0 ? max : 0);
}

static void settings_draw(View *v, bool focused) {
    Settings *st = v->data;
    g_set = st;
    gfx_text(F_BOLD, 30, CX, 26, C_TEXT, "Settings");
    clip_content(TOP - 6);
    for (int i = 0; i < row_count(); i++) {
        float y = TOP + i * ROW_H - st->sc.y, h = ROW_H - 8;
        bool foc = focused && st->focus == i;
        gfx_rrect(CX, y, CW, h, 12, foc ? C_PANEL2 : C_PANEL);
        if (foc) draw_focus_ring(CX, y, CW, h, 12);
        char name[96], val[200], fit[200];
        label(i, name, val, sizeof val);
        gfx_icon(kIcon[i], CX + 32, y + h / 2, 22, C_SUB);
        gfx_text_vc(F_BOLD, 18, CX + 60, y + h / 2, C_TEXT, name);
        float nw = gfx_text_w(F_BOLD, 18, name);
        gfx_fit(F_REG, 16, val, CW - 60 - nw - 48, fit, sizeof fit);
        gfx_text_right_vc(F_REG, 16, CX + CW - 20, y + h / 2, C_SUB, fit);
        hit_add(CX, y, CW, h, on_tap, i, 0);
    }
    clip_off();
    if (focused) {
        hint(G_DPAD, "Move");
        if (st->focus == S_DEBUG_REC) hint(g_ok(), wavrec_active() ? "Stop" : "Record");
        else if (st->focus != S_ABOUT) hint(g_ok(), st->focus == S_LIBRARY ? "Edit" : st->focus == S_REFRESH ? "Refresh" : "Change");
        hint(g_back(), "Back");
    }
}

static void settings_destroy(View *v) {
    if (g_set == v->data) g_set = NULL;
    free(v->data);
    free(v);
}

View *view_settings_new(void) {
    View *v = calloc(1, sizeof *v);
    Settings *s = calloc(1, sizeof *s);
    if (!v || !s) { free(v); free(s); return NULL; }
    v->update = settings_update;
    v->draw = settings_draw;
    v->destroy = settings_destroy;
    v->data = s;
    return v;
}
