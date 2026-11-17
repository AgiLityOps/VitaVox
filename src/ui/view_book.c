/* Book details: cover, title, author, narrator, series, length, progress,
 * Resume / Play from start, and the description (scrollable). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vita2d.h>

#include "covers.h"
#include "scanner.h"
#include "libmodel.h"
#include "osal.h"
#include "player.h"
#include "strutil.h"
#include "views.h"
#include "worker.h"

#define COVER 250.0f
#define RX (CX + COVER + 32)
#define RW (CX + CW - RX)

enum { F_PLAY, F_START, F_DESC };

typedef struct {
    BookSummary copy;     /* used when the book isn't in the library (offline) */
    char id[40];
    int focus;            /* F_* */
    bool confirm_delete;
    Scroll desc;
    /* details fetched in the background */
    Mutex lock;
    volatile int state;   /* 0 loading, 1 ready, -1 failed */
    BookDetail d;
    int refs;             /* the job holds a reference */
    bool dead;
    /* 00.38: description laid out once (an 8 KB text is ~100 lines: too slow to wrap every frame) */
    int nlines;           /* -1 until laid out */
    int *ls, *ll;         /* byte start / length of each line in d.description */
} Book;

static Book *g_book;

static void release(Book *b) {
    mutex_lock(&b->lock);
    bool free_it = --b->refs == 0;
    mutex_unlock(&b->lock);
    if (free_it) {
        free(b->ls);
        free(b->ll);
        book_detail_free(&b->d);
        mutex_destroy(&b->lock);
        free(b);
    }
}

static void fetch_job(void *ud) {
    Book *b = ud;
    BookDetail d;
    memset(&d, 0, sizeof d);
    int rc = b->dead ? -1 : scanner_open_detail(&b->copy, &d);
    mutex_lock(&b->lock);
    if (rc == 0) {
        /* 00.38: descriptions come cleaned from the scanner (HTML, blank lines, UTF-8) */
        b->d = d;
        b->state = 1;
    } else {
        b->state = -1;
    }
    mutex_unlock(&b->lock);
    release(b);
}

static const BookSummary *summary(Book *b) {
    int i = lib_find_id(b->id);
    return i >= 0 ? lib_book(i) : &b->copy;
}

static void act(Book *b, int which) {
    const BookSummary *s = summary(b);
    if (!s) return;
    b->confirm_delete = false;
    if (which == F_PLAY) app_play(s, -1);
    else if (which == F_START) app_play(s, 0);
}

static void on_tap(int which, int x) {
    (void)x;
    if (!g_book) return;
    g_book->focus = which;
    act(g_book, which);
}

static float desc_top(void) { return 380; }

#define DESC_LH 22.0f
#define DESC_MAX_LINES 1000

/* UI thread, with b->lock held, once the details are in. */
static void layout_desc(Book *b) {
    if (b->nlines >= 0 || b->state != 1) return;
    b->nlines = 0;
    const char *d = b->d.description;
    if (!d || !d[0]) return;
    b->ls = malloc(sizeof(int) * DESC_MAX_LINES);
    b->ll = malloc(sizeof(int) * DESC_MAX_LINES);
    if (!b->ls || !b->ll) { free(b->ls); free(b->ll); b->ls = b->ll = NULL; return; }
    b->nlines = gfx_wrap_layout(F_REG, 15, CW, d, b->ls, b->ll, DESC_MAX_LINES);
}

static void book_update(View *v, const Input *in, float dt) {
    Book *b = v->data;
    scroll_step(&b->desc, dt);
    mutex_lock(&b->lock);
    layout_desc(b);
    mutex_unlock(&b->lock);
    int lines = b->nlines > 0 ? b->nlines : 0;
    float max = lines * DESC_LH - (CONTENT_BOTTOM - desc_top() - 12);
    if (max < 0) max = 0;
    if (in->down & B_BACK) { app_pop(); return; }
    if (b->focus < F_DESC) {
        int before = b->focus;
        if (in->repeat & B_LEFT) { if (b->focus == F_PLAY) { app_focus_sidebar(); return; } b->focus--; }
        if (in->repeat & B_RIGHT && b->focus < F_START) b->focus++;
        if (in->repeat & B_DOWN && max > 0) b->focus = F_DESC;
        if (b->focus != before) b->confirm_delete = false;
        if (in->down & B_OK) act(b, b->focus);
    } else {
        if (in->repeat & B_DOWN) scroll_to(&b->desc, b->desc.target + 66 > max ? max : b->desc.target + 66);
        if (in->repeat & B_UP) {
            if (b->desc.target <= 0) b->focus = F_PLAY;
            else scroll_to(&b->desc, b->desc.target - 66 < 0 ? 0 : b->desc.target - 66);
        }
        if (in->repeat & B_LEFT) { app_focus_sidebar(); return; }
    }
    if (in->down & B_SQUARE) act(b, 0);
    if (in->down & B_TRIANGLE) { const BookSummary *s = summary(b); if (s) app_push(view_series_pick_new(s)); return; }
    if (in->dragging && in->start_y > desc_top()) scroll_drag(&b->desc, in->drag_dy, max);
}

/* fill: 0..1 progress shown inside the button (<0: none) */
static void button(float x, float y, float w, float h, int icon, const char *label, bool primary, bool focus, int which,
                   float fill, uint32_t text) {
    gfx_rrect(x, y, w, h, h / 2, primary ? C_ACCENT : C_PANEL2);
    if (fill > 0) {
        vita2d_enable_clipping();
        vita2d_set_clip_rectangle((int)x, (int)y, (int)(x + w * (fill > 1 ? 1 : fill)), (int)(y + h));
        gfx_rrect(x, y, w, h, h / 2, gfx_alpha(C_ACCENT, 0.35f));
        vita2d_disable_clipping();
    }
    if (focus) draw_focus_ring(x, y, w, h, h / 2);
    uint32_t c = text ? text : primary ? C_ON_ACCENT : C_TEXT;
    float lw = gfx_ink_w(F_BOLD, 16, label), iw = 17, gap = 8;
    float sx = x + (w - (iw + gap + lw)) / 2;
    gfx_icon(icon, sx + iw / 2, y + h / 2, iw, c);
    gfx_text_vc(F_BOLD, 16, sx + iw + gap, y + h / 2, c, label);
    hit_add(x, y, w, h, on_tap, which, 0);
}

static void book_draw(View *v, bool focused) {
    Book *b = v->data;
    g_book = b;
    const BookSummary *s = summary(b);
    if (!s) return;
    float top = 30;
    covers_draw(s->id, CX, top, COVER, 1);
    char line[256];
    int tl = gfx_wrap(F_BOLD, 26, RX, top + 2, RW, 34, 2, C_TEXT, s->title);
    float y = top + 2 + tl * 34 + 4;
    gfx_fit(F_REG, 18, s->author[0] ? s->author : "Unknown author", RW, line, sizeof line);
    gfx_text(F_REG, 18, RX, y, C_ACCENT, line);
    y += 34;
    char dur[32];
    /* 00.37: a book whose parts are still being timed has no length yet. */
    if (s->duration > 0) fmt_duration(s->duration, dur, sizeof dur);
    else snprintf(dur, sizeof dur, "Length: scanning...");
    mutex_lock(&b->lock);
    const char *narr = b->state == 1 && b->d.narrator[0] ? b->d.narrator : NULL;
    mutex_unlock(&b->lock);
    if (narr) {
        snprintf(line, sizeof line, "Narrated by %s", narr);
        char fit[256];
        gfx_fit(F_REG, 15, line, RW, fit, sizeof fit);
        gfx_text(F_REG, 15, RX, y, C_SUB, fit);
        y += 24;
    }
    if (s->series[0]) {
        gfx_fit(F_REG, 15, s->series, RW, line, sizeof line);
        gfx_text(F_REG, 15, RX, y, C_SUB, line);
        y += 24;
    }
    gfx_text(F_REG, 15, RX, y, C_SUB, dur);
    y += 30;
    if (s->finished) {
        gfx_text(F_BOLD, 15, RX, y, C_ACCENT, "Finished");
        y += 26;
    } else if (s->progress > 0.005f && s->duration > 0) {
        draw_progress_bar(RX, y + 4, RW * 0.6f, 6, s->progress, C_ACCENT, C_LINE);
        char left[32];
        fmt_duration(s->duration - s->current_time, left, sizeof left);
        snprintf(line, sizeof line, "%s left", left);
        gfx_text_vc(F_REG, 15, RX + RW * 0.6f + 14, y + 7, C_SUB, line);
        y += 26;
    }
    bool resume = s->progress > 0.005f && !s->finished;
    float by = top + COVER - 52, gap = 8, bw0 = 180, bw = RW - bw0 - gap;
    button(RX, by, bw0, 52, IC_PLAY, resume ? "Resume" : "Play", true, focused && b->focus == F_PLAY, F_PLAY, -1, 0);
    button(RX + bw0 + gap, by, bw, 52, IC_BACK, "From start", false, focused && b->focus == F_START, F_START, -1, 0);
    /* description */
    float dy = desc_top();
    gfx_rect(CX, dy - 22, CW, 1, C_LINE);
    mutex_lock(&b->lock);
    layout_desc(b);
    if (b->state == 1 && b->nlines > 0) {
        clip_content(dy - 4);
        /* only the lines on screen */
        const char *d = b->d.description;
        int first = (int)(b->desc.y / DESC_LH) - 1;
        if (first < 0) first = 0;
        for (int i = first; i < b->nlines; i++) {
            float ly = dy - b->desc.y + i * DESC_LH;
            if (ly > CONTENT_BOTTOM) break;
            if (b->ll[i] <= 0) continue;
            char ln[512];
            int n = b->ll[i] < (int)sizeof ln - 1 ? b->ll[i] : (int)sizeof ln - 1;
            memcpy(ln, d + b->ls[i], (size_t)n);
            ln[n] = 0;
            gfx_text(F_REG, 15, CX, ly, C_SUB, ln);
        }
        clip_off();
        if (focused && b->focus == F_DESC) gfx_rect(CX - 10, dy - 4, 3, CONTENT_BOTTOM - dy - 8, C_ACCENT);
    } else if (b->state == 0) {
        gfx_text(F_REG, 15, CX, dy, C_DIM, "Loading details\xE2\x80\xA6");
    } else {
        gfx_text(F_REG, 15, CX, dy, C_DIM, "No description");
    }
    mutex_unlock(&b->lock);
    if (focused) {
        hint(G_DPAD, b->focus == F_DESC ? "Scroll" : "Move");
        if (b->focus == F_PLAY) hint(g_ok(), resume ? "Resume" : "Play");
        else if (b->focus == F_START) hint(g_ok(), "Play from start");
        hint(G_TRIANGLE, "Add to Series");
        hint(g_back(), "Back");
    }
}

static void book_destroy(View *v) {
    Book *b = v->data;
    if (g_book == b) g_book = NULL;
    b->dead = true;
    release(b);
    free(v);
}

View *view_book_new(const BookSummary *s) {
    if (!s) return NULL;
    View *v = calloc(1, sizeof *v);
    Book *b = calloc(1, sizeof *b);
    if (!v || !b) { free(v); free(b); return NULL; }
    b->copy = *s;
    str_copy(b->id, sizeof b->id, s->id);
    mutex_init(&b->lock);
    b->refs = 2;
    b->nlines = -1;
    if (!worker_post(g_api, fetch_job, b)) { b->refs = 1; b->state = -1; }
    v->update = book_update;
    v->draw = book_draw;
    v->destroy = book_destroy;
    v->data = b;
    v->own = B_TRIANGLE; /* Add to Series (instead of the global Search shortcut) */
    return v;
}
