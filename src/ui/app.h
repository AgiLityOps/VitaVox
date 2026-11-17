/* App shell: modes (main / player), the sidebar with its mini player,
 * the content view stack, the button-hint bar, toasts and touch hit-testing. */
#ifndef VITAVOX_APP_H
#define VITAVOX_APP_H
#include <stdbool.h>

#include "config.h"
#include "gfx.h"
#include "input.h"
#include "models.h"

/* content area (right of the sidebar, above the hint bar) */
#define SIDEBAR_W 220
#define CX 244
#define CW (SCREEN_W - CX - 20)
#define HINT_H 40
#define CONTENT_BOTTOM (SCREEN_H - HINT_H)
#define LIST_ROW 76.0f   /* 00.39: book list row (matches Series/Authors lists) */

typedef enum { SEC_HOME, SEC_LIBRARY, SEC_SERIES, SEC_AUTHORS, SEC_SEARCH, SEC_SETTINGS, SEC_COUNT } Section;

typedef struct View View;
struct View {
    void (*update)(View *v, const Input *in, float dt);
    void (*draw)(View *v, bool focused);
    void (*destroy)(View *v);
    /* called when the view becomes the top of the stack again */
    void (*resume)(View *v);
    void *data;
    /* buttons this view handles itself while focused (skips the global
     * Triangle = Search, L/R = section, Start = player shortcuts) */
    uint32_t own;
};

extern Config g_cfg;
void app_save_config(void);
const char *app_data_dir(void);

void app_init(void);
void app_update(const Input *in, float dt);
void app_draw(void);

/* navigation */
void app_push(View *v);
void app_pop(void);
void app_focus_sidebar(void);
void app_set_section(Section s, bool focus_content);
void app_open_book(int lib_index);
void app_open_book_summary(const BookSummary *b); /* e.g. a download while offline */
void app_play(const BookSummary *b, double start); /* start < 0: resume */
void app_open_player(void);
void app_close_player(void);

/* the confirm / back button glyphs (follow the system's X/O setting) */
Glyph g_ok(void);
Glyph g_back(void);
/* clip drawing to the content area below y (scrolling lists) */
void clip_content(float top);
void clip_off(void);

/* Modal option menu / confirm drawn over the content area. */
#define MENU_MAX 8
typedef struct {
    bool open;
    char title[96];
    char msg[160];           /* optional line under the title (confirms) */
    int n, focus;
    char items[MENU_MAX][64];
    bool danger[MENU_MAX];   /* drawn in the error colour */
    int tapped;              /* touch result: item + 1, -1 tapped outside, 0 none */
} Menu;
void menu_open(Menu *m, const char *title, const char *msg);
void menu_add(Menu *m, const char *label, bool danger);
/* -1 still open / nothing, -2 cancelled (Back or a tap outside), else the chosen
 * item (D-pad + OK or a tap on it; closes). */
int menu_update(Menu *m, const Input *in);
void menu_draw(Menu *m);

/* feedback */
void app_toast(const char *msg);
/* adds a button hint to this frame's hint bar */
void hint(Glyph g, const char *label);
/* Two buttons for one action, e.g. (L) (R) Skip. */
void hint2(Glyph g, Glyph g2, const char *label);

/* touch: rectangles registered while drawing; a tap calls fn(a, b) */
typedef void (*HitFn)(int a, int b);
void hit_add(float x, float y, float w, float h, HitFn fn, int a, int b);
/* position of the tap being handled */
float app_tap_x(void);
float app_tap_y(void);

/* smooth scrolling with touch momentum (src/core/scroll.h) */
#include "scroll.h"

/* shared widgets */
void draw_progress_bar(float x, float y, float w, float h, float frac, uint32_t fill, uint32_t track);
void draw_focus_ring(float x, float y, float w, float h, float r);
void draw_spinner(float cx, float cy, float r, uint32_t c);
/* book cell: cover with progress, title and author below */
void draw_book_cell(const BookSummary *b, float x, float y, float cover, bool focused);
/* book list row: cover, title, author, progress % / Finished. badge may be NULL. */
void draw_book_row(const BookSummary *b, float x, float y, float w, bool focused, const char *badge);

/* SELECT short press vs 2 s hold. Call from a focused view that wants the split
 * (Library, Search, a series' books, an author's books). enable false (IME or a
 * menu) resets. Returns 1 on an early release (short press), 2 the frame the
 * hold fires, 0 otherwise. A fill is drawn on the SELECT hint while held. */
int app_select_hold(const Input *in, float dt, bool enable);

#endif
