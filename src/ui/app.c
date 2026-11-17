#include "app.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <vita2d.h>

#include "covers.h"
#include "engine.h"
#include "libmodel.h"
#include "player.h"
#include "strutil.h"
#include "sys.h"
#include "views.h"
#include "hintlayout.h"
#include "osal.h"
#include "statusbar.h"
#include "holdbtn.h"

typedef enum { MODE_MAIN, MODE_PLAYER } Mode;

#define MAX_STACK 8
#define MAX_HINTS 8
#define MAX_HITS 160

static struct {
    Mode mode;
    Section section;
    bool sidebar_focus;
    int sb_focus;            /* 0..SEC_COUNT-1, SEC_COUNT = mini player */
    View *stack[MAX_STACK];
    int depth;
    char toast[200];
    float toast_t;
    struct { Glyph g, g2; int ng; const char *label; } hints[MAX_HINTS];
    int nhints;
    float sb_anim;           /* animated sidebar highlight position */
    HoldTrack hold;          /* 00.39: SELECT short vs 2 s hold */
    float hold_progress;
    bool hold_used;
} A;

static struct { float x, y, w, h; HitFn fn; int a, b; } g_hits[MAX_HITS], g_prev_hits[MAX_HITS];
static int g_nhits, g_nprev;
static float g_tap_x, g_tap_y;
float app_tap_x(void) { return g_tap_x; }
float app_tap_y(void) { return g_tap_y; }

static const char *kSectionName[SEC_COUNT] = {"Home", "Library", "Series", "Authors", "Search", "Settings"};
static const int kSectionIcon[SEC_COUNT] = {IC_HOME, IC_BOOK, IC_SERIES, IC_USER, IC_SEARCH, IC_GEAR};

/* ---- helpers ------------------------------------------------------------------- */

Glyph g_ok(void) { return sys_enter_is_cross() ? G_CROSS : G_CIRCLE; }
Glyph g_back(void) { return sys_enter_is_cross() ? G_CIRCLE : G_CROSS; }

void clip_content(float top) {
    vita2d_enable_clipping();
    vita2d_set_clip_rectangle(SIDEBAR_W, (int)top, SCREEN_W, CONTENT_BOTTOM);
}

void clip_off(void) { vita2d_disable_clipping(); }

void hint(Glyph g, const char *label) {
    if (A.nhints < MAX_HINTS) { A.hints[A.nhints].g = g; A.hints[A.nhints].ng = 1; A.hints[A.nhints].label = label; A.nhints++; }
}

void hint2(Glyph g, Glyph g2, const char *label) {
    if (A.nhints < MAX_HINTS) {
        A.hints[A.nhints].g = g;
        A.hints[A.nhints].g2 = g2;
        A.hints[A.nhints].ng = 2;
        A.hints[A.nhints].label = label;
        A.nhints++;
    }
}

void hit_add(float x, float y, float w, float h, HitFn fn, int a, int b) {
    if (g_nhits < MAX_HITS) g_hits[g_nhits++] = (typeof(g_hits[0])){x, y, w, h, fn, a, b};
}

void menu_open(Menu *m, const char *title, const char *msg) {
    memset(m, 0, sizeof *m);
    m->open = true;
    str_copy(m->title, sizeof m->title, title ? title : "");
    str_copy(m->msg, sizeof m->msg, msg ? msg : "");
}

void menu_add(Menu *m, const char *label, bool danger) {
    if (m->n >= MENU_MAX) return;
    str_copy(m->items[m->n], sizeof m->items[0], label);
    m->danger[m->n] = danger;
    m->n++;
}

/* The menu drawn last frame; its touch hits report into it. */
static Menu *g_hit_menu;
static void on_tap_menu(int a, int b) {
    (void)b;
    if (g_hit_menu && g_hit_menu->open) g_hit_menu->tapped = a;
}

int menu_update(Menu *m, const Input *in) {
    if (!m->open) return -1;
    if (m->tapped) {
        int t = m->tapped;
        m->tapped = 0;
        m->open = false;
        if (t < 0) return -2;
        m->focus = t - 1;
        return t - 1;
    }
    if (in->repeat & B_UP) m->focus = (m->focus + m->n - 1) % m->n;
    if (in->repeat & B_DOWN) m->focus = (m->focus + 1) % m->n;
    if (in->down & B_BACK || in->down & B_TRIANGLE) { m->open = false; return -2; }
    if (in->down & B_OK && m->n) { m->open = false; return m->focus; }
    return -1;
}

void menu_draw(Menu *m) {
    if (!m->open) return;
    gfx_rect(SIDEBAR_W, 0, SCREEN_W - SIDEBAR_W, CONTENT_BOTTOM, gfx_alpha(0xFF000000, 0.55f));
    float w = 400, row = 46, head = m->msg[0] ? 84 : 56;
    float h = head + m->n * row + 14;
    float x = CX + (CW - w) / 2, y = (CONTENT_BOTTOM - h) / 2;
    /* touch: a tap outside cancels, a tap on a row picks it, the rest of the panel does nothing */
    g_hit_menu = m;
    hit_add(0, 0, SCREEN_W, SCREEN_H, on_tap_menu, -1, 0);
    hit_add(x, y, w, h, on_tap_menu, 0, 0);
    for (int i = 0; i < m->n; i++) hit_add(x + 10, y + head + i * row, w - 20, row - 6, on_tap_menu, i + 1, 0);
    gfx_rrect(x, y, w, h, 16, C_PANEL2);
    char line[200];
    gfx_fit(F_BOLD, 19, m->title, w - 40, line, sizeof line);
    gfx_text(F_BOLD, 19, x + 20, y + 18, C_TEXT, line);
    if (m->msg[0]) {
        gfx_fit(F_REG, 15, m->msg, w - 40, line, sizeof line);
        gfx_text(F_REG, 15, x + 20, y + 48, C_SUB, line);
    }
    for (int i = 0; i < m->n; i++) {
        float ry = y + head + i * row;
        if (i == m->focus) {
            gfx_rrect(x + 10, ry, w - 20, row - 6, 10, C_PANEL);
            draw_focus_ring(x + 10, ry, w - 20, row - 6, 10);
        }
        gfx_text_vc(F_BOLD, 16, x + 26, ry + (row - 6) / 2, m->danger[i] ? C_ERROR : C_TEXT, m->items[i]);
    }
    hint(G_DPAD, "Move");
    hint(g_ok(), "Select");
    hint(g_back(), "Cancel");
}

void app_toast(const char *msg) {
    str_copy(A.toast, sizeof A.toast, msg);
    /* longer messages (e.g. a refused library folder) stay up longer */
    float t = 2.5f + (float)strlen(A.toast) / 40.0f;
    A.toast_t = t > 6 ? 6 : t;
}

void draw_progress_bar(float x, float y, float w, float h, float frac, uint32_t fill, uint32_t track) {
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    gfx_rrect(x, y, w, h, h / 2, track);
    if (frac > 0) gfx_rrect(x, y, fmaxf(h, w * frac), h, h / 2, fill);
}

void draw_focus_ring(float x, float y, float w, float h, float r) {
    gfx_ring(x, y, w, h, r, 3, C_ACCENT);
}

void draw_spinner(float cx, float cy, float r, uint32_t c) {
    static float t;
    t += 0.12f;
    for (int i = 0; i < 8; i++) {
        float a = t + i * (float)M_PI / 4;
        gfx_circle(cx + cosf(a) * r, cy + sinf(a) * r, r * 0.18f, gfx_alpha(c, 0.25f + i / 10.0f));
    }
}

void draw_book_cell(const BookSummary *b, float x, float y, float cover, bool focused) {
    covers_draw(b->id, x, y, cover, 1);
    if (b->finished) {
        gfx_circle(x + cover - 16, y + 16, 12, C_ACCENT);
        gfx_icon(IC_CHECK, x + cover - 16, y + 16, 14, C_ON_ACCENT);
    } else if (b->progress > 0.005f) {
        gfx_rect(x, y + cover - 5, cover, 5, gfx_alpha(0xFF000000, 0.6f));
        gfx_rect(x, y + cover - 5, cover * b->progress, 5, C_ACCENT);
    }
    if (focused) draw_focus_ring(x, y, cover, cover, 2);
    char line[200];
    gfx_fit(F_BOLD, 15, b->title, cover, line, sizeof line);
    gfx_text(F_BOLD, 15, x, y + cover + 10, focused ? C_TEXT : gfx_mix(C_TEXT, C_SUB, 0.3f), line);
    gfx_fit(F_REG, 14, b->author, cover, line, sizeof line);
    gfx_text(F_REG, 14, x, y + cover + 32, C_SUB, line);
}

void draw_book_row(const BookSummary *b, float x, float y, float w, bool focused, const char *badge) {
    if (!b) return;
    float h = LIST_ROW - 8.0f;
    gfx_rrect(x, y, w, h, 12, focused ? C_PANEL2 : C_PANEL);
    if (focused) draw_focus_ring(x, y, w, h, 12);
    float tile = h - 14.0f, cx = x + 10, cy = y + 7;
    covers_draw(b->id, cx, cy, tile, 1);
    if (b->finished) {
        gfx_circle(cx + tile - 8, cy + 8, 8, C_ACCENT);
        gfx_icon(IC_CHECK, cx + tile - 8, cy + 8, 10, C_ON_ACCENT);
    } else if (b->progress > 0.005f) {
        gfx_rect(cx, cy + tile - 4, tile, 4, gfx_alpha(0xFF000000, 0.6f));
        gfx_rect(cx, cy + tile - 4, tile * b->progress, 4, C_ACCENT);
    }
    if (badge && badge[0]) {
        float bw = gfx_ink_w(F_BOLD, 12, badge) + 12;
        if (bw < 22) bw = 22;
        gfx_rrect(cx + 3, cy + 3, bw, 18, 9, gfx_alpha(0xFF000000, 0.7f));
        gfx_text_center(F_BOLD, 12, cx + 3 + bw / 2, cy + 12, C_TEXT, badge);
    }
    char right[24];
    const char *rt = NULL;
    uint32_t rc = C_SUB;
    if (b->finished) { rt = "Finished"; rc = C_ACCENT; }
    else if (b->progress > 0.005f) {
        int pct = (int)(b->progress * 100.0f + 0.5f);
        if (pct > 100) pct = 100;
        snprintf(right, sizeof right, "%d%%", pct);
        rt = right;
    }
    float rw = 0;
    if (rt) {
        rw = gfx_ink_w(F_REG, 14, rt) + 16;
        gfx_text_right_vc(F_REG, 14, x + w - 20, y + h / 2, rc, rt);
    }
    float tx = cx + tile + 18, tw = w - (tx - x) - 20 - rw;
    if (tw < 40) tw = 40;
    char line[200];
    gfx_fit(F_BOLD, 19, b->title, tw, line, sizeof line);
    if (b->author[0]) {
        gfx_text(F_BOLD, 19, tx, y + 16, C_TEXT, line);
        gfx_fit(F_REG, 15, b->author, tw, line, sizeof line);
        gfx_text(F_REG, 15, tx, y + 42, C_SUB, line);
    } else {
        gfx_text_vc(F_BOLD, 19, tx, y + h / 2, C_TEXT, line);
    }
}

int app_select_hold(const Input *in, float dt, bool enable) {
    if (!enable || !in) {
        hold_reset(&A.hold);
        A.hold_progress = 0;
        A.hold_used = true;
        return 0;
    }
    A.hold_used = true;
    HoldEvent ev = hold_update(&A.hold, (in->held & B_SELECT) != 0, dt, &A.hold_progress);
    if (ev == HOLD_FIRED) return 2;
    if (ev == HOLD_SHORT) return 1;
    return 0;
}

/* ---- navigation --------------------------------------------------------------- */

static void clear_stack(void) {
    while (A.depth) {
        View *v = A.stack[--A.depth];
        if (v->destroy) v->destroy(v);
    }
}

void app_push(View *v) {
    if (!v) return;
    if (A.depth == MAX_STACK) { app_pop(); }
    A.stack[A.depth++] = v;
    A.sidebar_focus = false;
}

void app_pop(void) {
    if (A.depth <= 1) { app_focus_sidebar(); return; }
    View *v = A.stack[--A.depth];
    if (v->destroy) v->destroy(v);
    View *top = A.stack[A.depth - 1];
    if (top->resume) top->resume(top);
}

void app_focus_sidebar(void) {
    A.sidebar_focus = true;
    A.sb_focus = A.section;
}

static View *section_root(Section s) {
    switch (s) {
    case SEC_HOME: return view_home_new();
    case SEC_LIBRARY: return view_library_new();
    case SEC_SERIES: return view_series_list_new();
    case SEC_AUTHORS: return view_groups_new(GROUP_AUTHOR);
    case SEC_SEARCH: return view_search_new(false);
    case SEC_SETTINGS: return view_settings_new();
    default: return view_home_new();
    }
}

void app_set_section(Section s, bool focus_content) {
    if (s != A.section || A.depth == 0) {
        clear_stack();
        A.section = s;
        app_push(section_root(s));
    } else {
        while (A.depth > 1) app_pop();
    }
    A.sb_focus = s;
    A.sidebar_focus = !focus_content;
}

void app_open_book(int lib_index) { app_push(view_book_new(lib_book(lib_index))); }
void app_open_book_summary(const BookSummary *b) { app_push(view_book_new(b)); }

void app_play(const BookSummary *b, double start) {
    player_open(b, start);
    app_open_player();
}

void app_open_player(void) {
    A.mode = MODE_PLAYER;
    player_screen_enter();
}

void app_close_player(void) { A.mode = MODE_MAIN; }

void app_init(void) {
    memset(&A, 0, sizeof A);
    A.sb_anim = SEC_HOME;
    A.mode = MODE_MAIN;
    lib_startup();
    app_set_section(SEC_HOME, true);
}

/* ---- sidebar -------------------------------------------------------------------- */

#define SB_Y0 76
#define SB_ITEM_H 44
#define SB_GAP 4
#define MINI_Y 416
#define MINI_H 80

static float sb_item_y(int i) { return SB_Y0 + i * (SB_ITEM_H + SB_GAP); }

static void on_sidebar_tap(int i, int b) {
    (void)b;
    if (i == SEC_COUNT) { app_open_player(); return; }
    app_set_section((Section)i, true);
}

static void sidebar_update(const Input *in) {
    int n = SEC_COUNT + (player_book() ? 1 : 0);
    if (in->repeat & B_UP) A.sb_focus = (A.sb_focus + n - 1) % n;
    if (in->repeat & B_DOWN) A.sb_focus = (A.sb_focus + 1) % n;
    if (in->repeat & (B_UP | B_DOWN) && A.sb_focus < SEC_COUNT && A.sb_focus != (int)A.section) {
        /* moving through the sidebar previews the section */
        clear_stack();
        A.section = (Section)A.sb_focus;
        app_push(section_root(A.section));
        A.sidebar_focus = true;
    }
    if (in->down & (B_OK | B_RIGHT)) {
        if (A.sb_focus == SEC_COUNT) app_open_player();
        else app_set_section((Section)A.sb_focus, true);
    }
}

static void mini_player_draw(bool focused) {
    const BookDetail *b = player_book();
    if (!b) return;
    float x = 12, y = MINI_Y, w = SIDEBAR_W - 24, h = MINI_H;
    gfx_rrect(x, y, w, h, 12, C_PANEL2);
    if (focused) draw_focus_ring(x, y, w, h, 12);
    covers_draw(b->id, x + 10, y + 10, 48, 1);
    char line[160];
    float tx = x + 68, tw = w - 68 - 10;
    gfx_fit(F_BOLD, 14, b->title, tw, line, sizeof line);
    gfx_text(F_BOLD, 14, tx, y + 12, C_TEXT, line);
    const char *st = player_status();
    gfx_fit(F_REG, 13, st ? st : b->author, tw, line, sizeof line);
    gfx_text(F_REG, 13, tx, y + 31, st ? C_ACCENT : C_SUB, line);
    gfx_icon(player_playing() ? IC_PAUSE : IC_PLAY, tx + 7, y + 55, 14, C_TEXT);
    char t[32];
    fmt_time(player_position(), t, sizeof t);
    gfx_text_vc(F_REG, 13, tx + 20, y + 55, C_SUB, t);
    double d = player_duration();
    draw_progress_bar(x + 10, y + h - 10, w - 20, 4, d > 0 ? (float)(player_position() / d) : 0, C_ACCENT, C_LINE);
    hit_add(x, y, w, h, on_sidebar_tap, SEC_COUNT, 0);
}

static void sidebar_draw(float dt) {
    gfx_rect(0, 0, SIDEBAR_W, CONTENT_BOTTOM, C_PANEL);
    gfx_logo(36, 38, 28);
    gfx_text_vc(F_BOLD, 20, 60, 38, C_TEXT, "Vita Vox");
    float target = A.section;
    A.sb_anim += (target - A.sb_anim) * (1 - expf(-dt * 16));
    float hy = SB_Y0 + A.sb_anim * (SB_ITEM_H + SB_GAP);
    gfx_rrect(12, hy, SIDEBAR_W - 24, SB_ITEM_H, 12, C_PANEL2);
    gfx_rrect(12, hy + 12, 4, SB_ITEM_H - 24, 2, C_ACCENT);
    for (int i = 0; i < SEC_COUNT; i++) {
        float y = sb_item_y(i), cy = y + SB_ITEM_H / 2;
        bool sel = i == (int)A.section, foc = A.sidebar_focus && A.sb_focus == i;
        uint32_t c = sel || foc ? C_TEXT : C_SUB;
        gfx_icon(kSectionIcon[i], 46, cy, 22, sel ? C_ACCENT : c);
        gfx_text_vc(F_BOLD, 18, 72, cy, c, kSectionName[i]);
        if (foc) draw_focus_ring(12, y, SIDEBAR_W - 24, SB_ITEM_H, 12);
        hit_add(0, y, SIDEBAR_W, SB_ITEM_H, on_sidebar_tap, i, 0);
    }
    mini_player_draw(A.sidebar_focus && A.sb_focus == SEC_COUNT);
}

/* ---- hint bar and toast ------------------------------------------------------------ */

/* 00.38: bottom-left status: clock, battery icon, percentage (reads cached ~1 s). */
static void status_read(int *pct, int *charging, int *hour, int *minute, int *h24) {
    *pct = sys_battery();
    *charging = sys_charging();
    if (!sys_clock(hour, minute)) { *hour = 0; *minute = 0; }
    *h24 = sys_time_24h();
}

#define C_BATT_LOW C_ACCENT
#define C_BATT_CRIT RGBA(255, 102, 102, 255)
#define C_BATT_CHG RGBA(64, 200, 120, 255)
#define C_BOLT RGBA(255, 230, 120, 255)

static void battery_draw(float x, float cy, int pct, int charging) {
    float y = roundf(cy - SB_BATT_H / 2.0f);
    const float t = SB_BATT_STROKE;
    /* 2 px rounded outline (ring drawn outside the inner rect) and the nub */
    gfx_ring(x + t, y + t, SB_BATT_W - 2 * t, SB_BATT_H - 2 * t, SB_BATT_R - t, t, C_SUB);
    gfx_rrect(x + SB_BATT_W, y + (SB_BATT_H - SB_NUB_H) / 2.0f, SB_NUB_W, SB_NUB_H, 1, C_SUB);
    SbLevel lv = sb_level(pct, charging);
    uint32_t fc = lv == SB_LVL_CHARGING ? C_BATT_CHG : lv == SB_LVL_CRITICAL ? C_BATT_CRIT
                : lv == SB_LVL_LOW ? C_BATT_LOW : C_TEXT;
    float fw = sb_fill_w(pct);
    if (fw >= 0.5f) gfx_rrect(x + SB_BATT_INSET, y + SB_BATT_INSET, fw, SB_BATT_H - 2 * SB_BATT_INSET, 1, fc);
    if (charging) gfx_icon(IC_BOLT, x + SB_BATT_W / 2.0f, cy, 16, C_BOLT);
}

static void hintbar_draw(void) {
    gfx_rect(0, CONTENT_BOTTOM, SCREEN_W, HINT_H, RGBA(10, 11, 14, 255));
    gfx_rect(0, CONTENT_BOTTOM, SCREEN_W, 1, C_LINE);
    float cy = CONTENT_BOTTOM + HINT_H / 2;
    /* right-aligned, in the order added; one or two glyphs then the label */
    HintItem it[MAX_HINTS];
    for (int i = 0; i < A.nhints; i++) {
        it[i].label_w = gfx_ink_w(F_REG, 15, A.hints[i].label);
        it[i].nglyphs = A.hints[i].ng;
        it[i].glyph_w[0] = gfx_glyph_w(A.hints[i].g, 24);
        it[i].glyph_w[1] = A.hints[i].ng == 2 ? gfx_glyph_w(A.hints[i].g2, 24) : 0;
    }
    float hints_left = A.nhints ? hint_layout(it, A.nhints, SCREEN_W - 20) : SCREEN_W;
    for (int i = 0; i < A.nhints; i++) {
        gfx_text_vc(F_REG, 15, it[i].label_x, cy, C_SUB, A.hints[i].label);
        gfx_glyph(A.hints[i].g, it[i].glyph_cx[0], cy, 24);
        if (A.hints[i].ng == 2) gfx_glyph(A.hints[i].g2, it[i].glyph_cx[1], cy, 24);
    }
    /* 00.39: fill the SELECT pill (and a 3 px cap above the bar) while held */
    if (A.hold_progress > 0) {
        float p = A.hold_progress > 1 ? 1 : A.hold_progress;
        for (int i = 0; i < A.nhints; i++) {
            if (A.hints[i].g != G_SELECT) continue;
            float size = 24, r = size / 2, gw = it[i].glyph_w[0], gcx = it[i].glyph_cx[0];
            float pw = gw, ph = size * 0.8f, px = gcx - pw / 2, py = cy - r * 0.8f;
            float fw = pw * p, rr = ph / 2;
            if (fw > 0.5f) gfx_rrect(px, py, fw, ph, fw < 2 * rr ? fw / 2 : rr, C_ACCENT);
            gfx_text_center(F_BOLD, (int)(size * 0.5f), gcx, cy, C_TEXT, "SELECT");
            if (fw > 0.5f) gfx_rrect(px, CONTENT_BOTTOM - 3, fw, 3, 1.5f, C_ACCENT);
        }
    }
    /* left: clock, battery icon, percentage */
    static SbCache sb;
    sb_cache_tick(&sb, now_ms() / 1000.0, status_read);
    float tw = (float)gfx_text_w(F_REG, 15, sb.time), pw = (float)gfx_text_w(F_REG, 15, sb.pct_text);
    float x = 20;
    /* never under the hints: drop the clock first if a screen ever needs the room */
    bool clock = 20 + sb_total_w(tw, pw) + 24 <= hints_left;
    if (clock) {
        gfx_text_vc(F_REG, 15, x, cy, C_SUB, sb.time);
        x += tw + SB_GAP_TIME;
    }
    if (x + SB_BATT_W + SB_NUB_W + 24 <= hints_left) {
        battery_draw(x, cy, sb.pct, sb.charging);
        x += SB_BATT_W + SB_NUB_W + SB_GAP_PCT;
        if (x + pw + 24 <= hints_left) gfx_text_vc(F_REG, 15, x, cy, C_SUB, sb.pct_text);
    }
}

#define TOAST_MAX_W 760.0f
#define TOAST_LH 22.0f

static void toast_draw(float dt) {
    if (A.toast_t <= 0) return;
    A.toast_t -= dt;
    float a = A.toast_t < 0.3f ? A.toast_t / 0.3f : 1;
    float ink = gfx_ink_w(F_BOLD, 16, A.toast);
    if (ink <= TOAST_MAX_W) {
        float w = ink + 48, h = 44;
        float x = (SCREEN_W - w) / 2, y = CONTENT_BOTTOM - h - 16;
        gfx_rrect(x, y, w, h, h / 2, gfx_alpha(C_PANEL2, a * 0.97f));
        gfx_text_center(F_BOLD, 16, x + w / 2, y + h / 2, gfx_alpha(C_TEXT, a), A.toast);
        return;
    }
    /* too long for one line (00.33): wrapped, up to three lines */
    int lines = gfx_wrap_count(F_BOLD, 16, TOAST_MAX_W, A.toast);
    if (lines > 3) lines = 3;
    if (lines < 1) lines = 1;
    float w = TOAST_MAX_W + 48, h = lines * TOAST_LH + 22;
    float x = (SCREEN_W - w) / 2, y = CONTENT_BOTTOM - h - 16;
    gfx_rrect(x, y, w, h, 18, gfx_alpha(C_PANEL2, a * 0.97f));
    float cap = 16 * 0.72f; /* Manrope cap height */
    float y0 = y + (h - ((lines - 1) * TOAST_LH + cap)) / 2;
    gfx_wrap(F_BOLD, 16, x + 24, y0, TOAST_MAX_W, TOAST_LH, lines, gfx_alpha(C_TEXT, a), A.toast);
}

/* ---- frame ---------------------------------------------------------------------------- */

static float g_dt;

static void hold_end_frame(void) {
    if (!A.hold_used) {
        hold_reset(&A.hold);
        A.hold_progress = 0;
    }
}

void app_update(const Input *in, float dt) {
    g_dt = dt;
    A.hold_used = false;
    A.hold_progress = 0;
    lib_tick();
    covers_tick();
    player_tick(dt);
    /* keep Continue listening current while a book plays */
    static float note_t;
    if (player_book() && (note_t += dt) > 5) {
        note_t = 0;
        lib_note_progress(player_book_id(), player_position(), player_duration());
    }
    A.nhints = 0;

    /* touch: taps hit the rectangles drawn last frame (topmost first) */
    memcpy(g_prev_hits, g_hits, sizeof(g_hits[0]) * g_nhits);
    g_nprev = g_nhits;
    g_nhits = 0;
    Input local = *in;
    if (in->tap) {
        g_tap_x = in->tap_x;
        g_tap_y = in->tap_y;
        for (int i = g_nprev - 1; i >= 0; i--) {
            typeof(g_hits[0]) *h = &g_prev_hits[i];
            if (in->tap_x >= h->x && in->tap_x < h->x + h->w && in->tap_y >= h->y && in->tap_y < h->y + h->h) {
                h->fn(h->a, h->b);
                local.tap = false;
                break;
            }
        }
    }

    switch (A.mode) {
    case MODE_PLAYER: hold_end_frame(); player_screen_update(&local, dt); return;
    case MODE_MAIN: break;
    }

    /* global shortcuts (unless the focused view handles the button itself) */
    uint32_t own = !A.sidebar_focus && A.depth ? A.stack[A.depth - 1]->own : 0;
    if (in->down & B_START && !(own & B_START)) { hold_end_frame(); if (player_book() || player_phase() == PL_OPENING) app_open_player(); return; }
    if (in->down & B_SQUARE && player_book() && A.sidebar_focus) { hold_end_frame(); player_toggle(); return; }
    if (in->repeat & B_L && !(own & B_L)) { hold_end_frame(); app_set_section((Section)((A.section + SEC_COUNT - 1) % SEC_COUNT), !A.sidebar_focus); return; }
    if (in->repeat & B_R && !(own & B_R)) { hold_end_frame(); app_set_section((Section)((A.section + 1) % SEC_COUNT), !A.sidebar_focus); return; }
    if (in->down & B_TRIANGLE && A.section != SEC_SEARCH && !(own & B_TRIANGLE)) {
        hold_end_frame();
        clear_stack();
        A.section = SEC_SEARCH;
        app_push(view_search_new(true));
        return;
    }

    if (A.sidebar_focus) sidebar_update(&local);
    else if (A.depth) {
        View *v = A.stack[A.depth - 1];
        if (v->update) v->update(v, &local, dt);
    }
    hold_end_frame();
}

void app_draw(void) {
    float dt = g_dt;
    if (A.mode == MODE_PLAYER) {
        player_screen_draw();
    } else {
        gfx_rect(0, 0, SCREEN_W, SCREEN_H, C_BG);
        if (A.depth) {
            View *v = A.stack[A.depth - 1];
            if (v->draw) v->draw(v, !A.sidebar_focus);
        }
        sidebar_draw(dt);
        if (A.sidebar_focus) {
            hint(G_DPAD, "Move");
            hint(g_ok(), "Open");
            if (player_book()) hint(G_SQUARE, player_playing() ? "Pause" : "Play");
            hint(G_TRIANGLE, "Search");
            if (player_book()) hint(G_START, "Player");
        }
    }
    hintbar_draw();
    toast_draw(dt);
}
