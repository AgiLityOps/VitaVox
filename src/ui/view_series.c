/* User-managed series (series.json): the Series section list, one series'
 * books (alphabetical like the Library, or the user's manual order), the
 * "Add Books" checklist and the "Add to Series" picker used from a book's
 * details. Menus draw 256 px thumbnails only; nothing here scans or probes. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "covers.h"
#include "dbg.h"
#include "ime.h"
#include "libmodel.h"
#include "osal.h"
#include "series.h"
#include "strutil.h"
#include "views.h"

static SeriesStore g_st;
static unsigned g_rev = 1; /* bumps on every change: views recompute */

void series_ui_init(const char *path) {
    series_free(&g_st);
    int rc = series_load(&g_st, path);
    if (rc == 1) app_toast("Series file was damaged; kept a copy as series.json.bad");
    g_rev++;
}

static void persist(void) {
    g_rev++;
    if (series_save(&g_st) != 0) app_toast("Couldn't save series");
}

/* Library indices of series si in display order (cached per view). */
typedef struct {
    unsigned rev;
    const BookSummary *lib0;
    int libn;
    int *idx;
    int n;
} VisCache;

static const int *vis_get(VisCache *c, int si, int *n) {
    const BookSummary *b0 = lib_book(0);
    int ln = lib_count();
    if (c->rev != g_rev || c->lib0 != b0 || c->libn != ln) {
        c->rev = g_rev;
        c->lib0 = b0;
        c->libn = ln;
        c->n = 0;
        free(c->idx);
        c->idx = NULL;
        if (si >= 0 && si < g_st.n && g_st.s[si].n > 0) {
            c->idx = malloc(sizeof(int) * (size_t)g_st.s[si].n);
            if (c->idx) c->n = series_visible(&g_st, si, b0, ln, c->idx);
        }
    }
    *n = c->n;
    return c->idx;
}

/* ===================================================================================
 * Add Books: multi-select checklist of the library
 * =================================================================================== */

#define CL_ROW 64.0f
#define CL_TOP 76.0f

typedef struct {
    int si;
    int focus;
    Scroll sc;
    bool *sel;
    int nsel_cap;
    int nsel;
} Check;

static Check *g_check;

static void check_toggle(Check *c, int i) {
    int n;
    const int *idx = lib_sorted(SORT_TITLE, &n);
    if (!idx || i < 0 || i >= n || i >= c->nsel_cap) return;
    const BookSummary *b = lib_book(idx[i]);
    if (!b || series_contains(&g_st, c->si, b->id)) return;
    c->sel[i] = !c->sel[i];
    c->nsel += c->sel[i] ? 1 : -1;
}

static void check_commit(Check *c) {
    int n;
    const int *idx = lib_sorted(SORT_TITLE, &n);
    if (!c->nsel || !idx) { app_toast("Select books with the confirm button first"); return; }
    int added = 0;
    for (int i = 0; i < n && i < c->nsel_cap; i++) {
        const BookSummary *b = lib_book(idx[i]);
        if (c->sel[i] && b && series_add(&g_st, c->si, b->id) == 0) added++;
    }
    persist();
    char t[96];
    snprintf(t, sizeof t, "Added %d book%s", added, added == 1 ? "" : "s");
    app_toast(t);
    dbg("series: added %d books to '%s'", added, g_st.s[c->si].name);
    app_pop();
}

static void on_check_tap(int i, int b) {
    (void)b;
    if (!g_check) return;
    g_check->focus = i;
    check_toggle(g_check, i);
}

static void check_update(View *v, const Input *in, float dt) {
    Check *c = v->data;
    if (c->si < 0 || c->si >= g_st.n) { app_pop(); return; }
    int n;
    lib_sorted(SORT_TITLE, &n);
    if (n != c->nsel_cap) { /* library changed under us: start over */
        free(c->sel);
        c->sel = calloc((size_t)(n ? n : 1), sizeof(bool));
        c->nsel_cap = c->sel ? n : 0;
        c->nsel = 0;
        if (c->focus >= n) c->focus = n ? n - 1 : 0;
    }
    scroll_step(&c->sc, dt);
    if (in->repeat & B_UP && c->focus > 0) c->focus--;
    if (in->repeat & B_DOWN && c->focus + 1 < n) c->focus++;
    if (in->repeat & B_LEFT) c->focus = c->focus > 8 ? c->focus - 8 : 0;
    if (in->repeat & B_RIGHT) c->focus = c->focus + 8 < n ? c->focus + 8 : (n ? n - 1 : 0);
    if (in->down & B_OK) check_toggle(c, c->focus);
    if (in->down & B_TRIANGLE) { check_commit(c); return; }
    if (in->down & B_BACK) { app_pop(); return; }
    float max = n * CL_ROW - (CONTENT_BOTTOM - CL_TOP) + 12;
    if (max < 0) max = 0;
    if (in->dragging) scroll_drag(&c->sc, in->drag_dy, max);
    else scroll_show(&c->sc, c->focus * CL_ROW - (c->focus ? 30 : 0), (c->focus + 1) * CL_ROW + 30, CONTENT_BOTTOM - CL_TOP, max);
}

static void check_draw(View *v, bool focused) {
    Check *c = v->data;
    g_check = c;
    if (c->si < 0 || c->si >= g_st.n) return;
    int n;
    const int *idx = lib_sorted(SORT_TITLE, &n);
    char line[200];
    gfx_text(F_BOLD, 30, CX, 26, C_TEXT, "Add Books");
    snprintf(line, sizeof line, "to %s \xC2\xB7 %d selected", g_st.s[c->si].name, c->nsel);
    char fit[200];
    gfx_fit(F_REG, 16, line, CW - 200, fit, sizeof fit);
    gfx_text_right(F_REG, 16, CX + CW, 36, C_SUB, fit);
    if (!n || !idx) {
        gfx_text_center(F_REG, 17, CX + CW / 2, (CL_TOP + CONTENT_BOTTOM) / 2, C_SUB, "Your library is empty");
    } else {
        clip_content(CL_TOP - 6);
        int first = (int)(c->sc.y / CL_ROW), last = first + (int)((CONTENT_BOTTOM - CL_TOP) / CL_ROW) + 2;
        for (int i = first; i <= last && i < n; i++) {
            const BookSummary *b = lib_book(idx[i]);
            if (!b) continue;
            float y = CL_TOP + i * CL_ROW - c->sc.y, h = CL_ROW - 6;
            bool foc = focused && c->focus == i;
            bool in_series = series_contains(&g_st, c->si, b->id);
            bool on = in_series || (i < c->nsel_cap && c->sel[i]);
            gfx_rrect(CX, y, CW, h, 10, foc ? C_PANEL2 : C_PANEL);
            if (foc) draw_focus_ring(CX, y, CW, h, 10);
            covers_draw(b->id, CX + 8, y + 5, h - 10, 1);
            float tx = CX + 8 + h - 10 + 14, tw = CW - (tx - CX) - 70;
            gfx_fit(F_BOLD, 16, b->title, tw, line, sizeof line);
            gfx_text(F_BOLD, 16, tx, y + 10, in_series ? C_DIM : C_TEXT, line);
            gfx_fit(F_REG, 14, in_series ? "Already in this series" : (b->author[0] ? b->author : "Unknown author"), tw, line, sizeof line);
            gfx_text(F_REG, 14, tx, y + 32, in_series ? C_DIM : C_SUB, line);
            float bx = CX + CW - 44, by = y + h / 2 - 13;
            gfx_rrect(bx, by, 26, 26, 6, on ? (in_series ? C_DIM : C_ACCENT) : C_LINE);
            if (on) gfx_icon(IC_CHECK, bx + 13, by + 13, 16, C_ON_ACCENT);
            hit_add(CX, y, CW, h, on_check_tap, i, 0);
        }
        clip_off();
    }
    if (focused) {
        hint(G_DPAD, "Move");
        hint(g_ok(), "Select");
        hint(G_TRIANGLE, "Add selected");
        hint(g_back(), "Cancel");
    }
}

static void check_destroy(View *v) {
    Check *c = v->data;
    if (g_check == c) g_check = NULL;
    free(c->sel);
    free(c);
    free(v);
}

static View *view_series_add_new(int si) {
    View *v = calloc(1, sizeof *v);
    Check *c = calloc(1, sizeof *c);
    if (!v || !c) { free(v); free(c); return NULL; }
    c->si = si;
    c->nsel_cap = -1; /* allocate on first update */
    v->update = check_update;
    v->draw = check_draw;
    v->destroy = check_destroy;
    v->data = c;
    v->own = B_TRIANGLE;
    return v;
}

/* ===================================================================================
 * One series: grid or list of its books
 * =================================================================================== */

#define COLS 5
#define CELL_W ((float)CW / COLS)
#define COVER 122.0f
#define CELL_H 194.0f
#define HEAD_H 76.0f

enum { OPT_ADD, OPT_SORT, OPT_MOVE, OPT_REMOVE, OPT_RENAME, OPT_DELETE };

typedef struct {
    int si;
    int focus;
    Scroll sc;
    VisCache vc;
    bool moving;
    Menu menu;
    int menu_map[MENU_MAX];
    Menu confirm;
    bool renaming;
} Ser;

static Ser *g_ser;

static const char *sort_name(SeriesSort s) { return s == SERIES_SORT_MANUAL ? "Manual order" : "Alphabetical"; }

static void ser_set_sort(Ser *s, SeriesSort m) {
    if (g_st.s[s->si].sort == m) return;
    series_set_sort(&g_st, s->si, m);
    persist();
    s->focus = 0;
    scroll_to(&s->sc, 0);
    app_toast(m == SERIES_SORT_MANUAL ? "Sort: Manual order" : "Sort: Alphabetical (manual order kept)");
}

static void ser_open_menu(Ser *s, int n) {
    Series *x = &g_st.s[s->si];
    menu_open(&s->menu, x->name, NULL);
    int k = 0;
#define ADD(label, danger, id) do { menu_add(&s->menu, label, danger); s->menu_map[k++] = id; } while (0)
    ADD("Add Books", false, OPT_ADD);
    ADD(x->sort == SERIES_SORT_MANUAL ? "Sort: Alphabetical" : "Sort: Manual order", false, OPT_SORT);
    if (n > 1) ADD("Move book", false, OPT_MOVE);
    if (n > 0) ADD("Remove book from series", false, OPT_REMOVE);
    ADD("Rename series", false, OPT_RENAME);
    ADD("Delete series", true, OPT_DELETE);
#undef ADD
}

static void ser_option(Ser *s, int opt, const int *idx, int n) {
    Series *x = &g_st.s[s->si];
    switch (opt) {
    case OPT_ADD: app_push(view_series_add_new(s->si)); break;
    case OPT_SORT: ser_set_sort(s, x->sort == SERIES_SORT_MANUAL ? SERIES_SORT_ALPHA : SERIES_SORT_MANUAL); break;
    case OPT_MOVE: {
        if (x->sort != SERIES_SORT_MANUAL) {
            /* Keep the book under the cursor focused in the manual order. */
            const BookSummary *b = idx && s->focus < n ? lib_book(idx[s->focus]) : NULL;
            char id[40] = "";
            if (b) str_copy(id, sizeof id, b->id);
            ser_set_sort(s, SERIES_SORT_MANUAL);
            int m;
            const int *v2 = vis_get(&s->vc, s->si, &m);
            for (int i = 0; v2 && i < m; i++)
                if (!strcmp(lib_book(v2[i])->id, id)) s->focus = i;
        }
        s->moving = true;
        break;
    }
    case OPT_REMOVE: {
        const BookSummary *b = idx && s->focus < n ? lib_book(idx[s->focus]) : NULL;
        if (!b) break;
        series_remove(&g_st, s->si, b->id);
        persist();
        if (s->focus > 0 && s->focus >= n - 1) s->focus--;
        app_toast("Removed from series (the book stays in your library)");
        break;
    }
    case OPT_RENAME:
        s->renaming = ime_open("Rename series", x->name, SERIES_NAME_MAX - 1, IME_TEXT);
        break;
    case OPT_DELETE: {
        char msg[160];
        snprintf(msg, sizeof msg, "%d book%s stay in your library; no files are deleted.", x->n, x->n == 1 ? "" : "s");
        char t[140];
        snprintf(t, sizeof t, "Delete \"%s\"?", x->name);
        menu_open(&s->confirm, t, msg);
        menu_add(&s->confirm, "Delete series", true);
        menu_add(&s->confirm, "Cancel", false);
        s->confirm.focus = 1;
        break;
    }
    }
}

static void ser_open_focused(Ser *s, bool play) {
    int n;
    const int *idx = vis_get(&s->vc, s->si, &n);
    if (!idx || s->focus < 0 || s->focus >= n) return;
    if (play) app_play(lib_book(idx[s->focus]), -1);
    else app_open_book(idx[s->focus]);
}

static void on_ser_tap(int i, int b) {
    (void)b;
    if (!g_ser || g_ser->moving || g_ser->menu.open || g_ser->confirm.open) return;
    g_ser->focus = i;
    ser_open_focused(g_ser, false);
}

static void ser_move(Ser *s, const int *idx, int n, int steps) {
    if (!idx || s->focus < 0 || s->focus >= n) return;
    char id[40];
    str_copy(id, sizeof id, lib_book(idx[s->focus])->id);
    int dir = steps < 0 ? -1 : 1, moved = 0;
    for (int k = 0; k < (steps < 0 ? -steps : steps); k++) {
        if (series_move(&g_st, s->si, id, dir, lib_book(0), lib_count()) != 0) break;
        moved++;
    }
    if (!moved) return;
    g_rev++;
    s->focus += dir * moved;
}

static bool ser_is_list(const Ser *s) {
    return s->si >= 0 && s->si < g_st.n && g_st.s[s->si].list_view;
}

static void ser_toggle_list(Ser *s, int n) {
    if (s->si < 0 || s->si >= g_st.n) return;
    bool was = g_st.s[s->si].list_view;
    float rh = was ? LIST_ROW : CELL_H;
    int row = was ? s->focus : s->focus / COLS;
    if (row < 0) row = 0;
    float screen_off = row * rh - s->sc.y;
    series_set_list(&g_st, s->si, !was);
    persist();
    app_toast(g_st.s[s->si].list_view ? "List view" : "Grid view");
    bool list = g_st.s[s->si].list_view;
    rh = list ? LIST_ROW : CELL_H;
    row = list ? s->focus : s->focus / COLS;
    if (row < 0) row = 0;
    int rows = list ? n : (n + COLS - 1) / COLS;
    float max = rows * rh - (CONTENT_BOTTOM - HEAD_H) + 16;
    if (max < 0) max = 0;
    scroll_jump(&s->sc, row * rh - screen_off, max);
}

static void ser_update(View *v, const Input *in, float dt) {
    Ser *s = v->data;
    if (s->si < 0 || s->si >= g_st.n) { app_pop(); return; }
    scroll_step(&s->sc, dt);
    if (s->renaming) {
        if (ime_active()) {
            char out[SERIES_NAME_MAX];
            int r = ime_poll(out, sizeof out);
            if (r == 0) return;
            s->renaming = false;
            if (r == 1) {
                int rc = series_rename(&g_st, s->si, out);
                if (rc == 0) { persist(); app_toast("Series renamed"); }
                else app_toast(rc == -2 ? "A series with that name already exists" : "The name can't be empty");
            }
        } else {
            s->renaming = false;
        }
        return;
    }
    int n;
    const int *idx = vis_get(&s->vc, s->si, &n);
    if (s->confirm.open) {
        int r = menu_update(&s->confirm, in);
        if (r == 0) {
            dbg("series: deleted '%s' (%d books referenced, no files touched)", g_st.s[s->si].name, g_st.s[s->si].n);
            series_delete(&g_st, s->si);
            persist();
            app_toast("Series deleted");
            app_pop();
        }
        return;
    }
    if (s->menu.open) {
        int r = menu_update(&s->menu, in);
        if (r >= 0) ser_option(s, s->menu_map[r], idx, n);
        return;
    }
    bool list = ser_is_list(s);
    if (s->moving) {
        if (in->repeat & B_LEFT) ser_move(s, idx, n, -1);
        if (in->repeat & B_RIGHT) ser_move(s, idx, n, +1);
        if (in->repeat & B_UP) ser_move(s, idx, n, list ? -1 : -COLS);
        if (in->repeat & B_DOWN) ser_move(s, idx, n, list ? 1 : COLS);
        if (in->down & (B_OK | B_BACK | B_TRIANGLE)) {
            s->moving = false;
            persist();
            app_toast("Order saved");
        }
        app_select_hold(in, dt, false);
    } else {
        int f = s->focus;
        if (list) {
            if (in->repeat & B_DOWN && f + 1 < n) f++;
            if (in->repeat & B_UP && f > 0) f--;
            if (in->repeat & B_LEFT) { app_focus_sidebar(); return; }
        } else {
            if (in->repeat & B_RIGHT && f + 1 < n && (f + 1) % COLS) f++;
            if (in->repeat & B_LEFT) { if (f % COLS == 0) { app_focus_sidebar(); return; } f--; }
            if (in->repeat & B_DOWN) f = f + COLS < n ? f + COLS : (f / COLS < (n - 1) / COLS ? n - 1 : f);
            if (in->repeat & B_UP && f >= COLS) f -= COLS;
        }
        if (n && f >= n) f = n - 1;
        if (f < 0) f = 0;
        s->focus = f;
        if (in->down & B_OK) { if (n) ser_open_focused(s, false); else app_push(view_series_add_new(s->si)); }
        if (in->down & B_SQUARE) ser_open_focused(s, true);
        int sel = app_select_hold(in, dt, true);
        if (sel == 2) ser_toggle_list(s, n);
        else if (sel == 1)
            ser_set_sort(s, g_st.s[s->si].sort == SERIES_SORT_MANUAL ? SERIES_SORT_ALPHA : SERIES_SORT_MANUAL);
        if (in->down & B_TRIANGLE) ser_open_menu(s, n);
        if (in->down & B_BACK) { app_pop(); return; }
    }
    vis_get(&s->vc, s->si, &n);
    list = ser_is_list(s);
    float rh = list ? LIST_ROW : CELL_H;
    int rows = list ? n : (n + COLS - 1) / COLS;
    float max = rows * rh - (CONTENT_BOTTOM - HEAD_H) + 16;
    if (max < 0) max = 0;
    if (in->dragging && !s->moving) scroll_drag(&s->sc, in->drag_dy, max);
    else {
        int row = list ? s->focus : s->focus / COLS;
        scroll_show(&s->sc, row * rh - (row ? 24 : 0), (row + 1) * rh + 8, CONTENT_BOTTOM - HEAD_H, max);
    }
}

static void ser_draw(View *v, bool focused) {
    Ser *s = v->data;
    g_ser = s;
    if (s->si < 0 || s->si >= g_st.n) return;
    Series *x = &g_st.s[s->si];
    int n;
    const int *idx = vis_get(&s->vc, s->si, &n);
    char line[200];
    gfx_fit(F_BOLD, 30, x->name, CW - 260, line, sizeof line);
    gfx_text(F_BOLD, 30, CX, 26, C_TEXT, line);
    int hidden = x->n - n;
    if (hidden > 0) snprintf(line, sizeof line, "%d book%s \xC2\xB7 %s \xC2\xB7 %d not found", n, n == 1 ? "" : "s", sort_name(x->sort), hidden);
    else snprintf(line, sizeof line, "%d book%s \xC2\xB7 %s", n, n == 1 ? "" : "s", sort_name(x->sort));
    gfx_text_right(F_REG, 16, CX + CW, 36, C_SUB, line);
    if (!n || !idx) {
        float cy = (HEAD_H + CONTENT_BOTTOM) / 2;
        gfx_text_center(F_BOLD, 19, CX + CW / 2, cy - 14, C_TEXT, "No books in this series yet");
        gfx_text_center(F_REG, 16, CX + CW / 2, cy + 16, C_SUB, "Press confirm to add books from your library");
    } else if (x->list_view) {
        clip_content(HEAD_H - 6);
        int first = (int)(s->sc.y / LIST_ROW), last = first + (int)((CONTENT_BOTTOM - HEAD_H) / LIST_ROW) + 2;
        if (first < 0) first = 0;
        for (int i = first; i <= last && i < n; i++) {
            float y = HEAD_H + i * LIST_ROW - s->sc.y;
            const BookSummary *b = lib_book(idx[i]);
            if (!b) continue;
            bool foc = focused && s->focus == i;
            bool lift = foc && s->moving;
            char num[12], *badge = NULL;
            if (x->sort == SERIES_SORT_MANUAL) {
                snprintf(num, sizeof num, "%d", i + 1);
                badge = num;
            }
            draw_book_row(b, CX, y - (lift ? 6 : 0), CW, foc, badge);
            if (y + LIST_ROW > HEAD_H && y < CONTENT_BOTTOM) hit_add(CX, y, CW, LIST_ROW - 8, on_ser_tap, i, 0);
        }
        clip_off();
    } else {
        clip_content(HEAD_H - 6);
        int first_row = (int)(s->sc.y / CELL_H), last_row = (int)((s->sc.y + CONTENT_BOTTOM - HEAD_H) / CELL_H) + 1;
        for (int r = first_row; r <= last_row; r++)
            for (int c = 0; c < COLS; c++) {
                int i = r * COLS + c;
                if (i >= n) break;
                float cx = CX + c * CELL_W, y = HEAD_H + r * CELL_H - s->sc.y;
                const BookSummary *b = lib_book(idx[i]);
                if (!b) continue;
                bool foc = focused && s->focus == i;
                bool lift = foc && s->moving;
                draw_book_cell(b, cx, y - (lift ? 6 : 0), COVER, foc);
                if (x->sort == SERIES_SORT_MANUAL) {
                    char num[12];
                    snprintf(num, sizeof num, "%d", i + 1);
                    float w = gfx_ink_w(F_BOLD, 14, num) + 16;
                    if (w < 26) w = 26;
                    gfx_rrect(cx + 6, y + 6 - (lift ? 6 : 0), w, 26, 13, lift ? C_ACCENT : gfx_alpha(0xFF000000, 0.7f));
                    gfx_text_center(F_BOLD, 14, cx + 6 + w / 2, y + 19 - (lift ? 6 : 0), lift ? C_ON_ACCENT : C_TEXT, num);
                }
                if (y + CELL_H > HEAD_H && y < CONTENT_BOTTOM) hit_add(cx, y, COVER, COVER + 50, on_ser_tap, i, 0);
            }
        clip_off();
    }
    if (s->menu.open) { menu_draw(&s->menu); return; }
    if (s->confirm.open) { menu_draw(&s->confirm); return; }
    if (!focused) return;
    if (s->moving) {
        hint(G_DPAD, "Move book");
        hint(g_ok(), "Done");
        return;
    }
    hint(G_DPAD, "Move");
    if (n) {
        hint(g_ok(), "Details");
        hint(G_SQUARE, "Play");
    } else {
        hint(g_ok(), "Add Books");
    }
    hint(G_TRIANGLE, "Options");
    hint(G_SELECT, "Sort (hold) View");
    hint(g_back(), "Back");
}

static void ser_destroy(View *v) {
    Ser *s = v->data;
    if (s->moving) persist();
    if (g_ser == s) g_ser = NULL;
    free(s->vc.idx);
    free(s);
    free(v);
}

static View *view_series_new(int si) {
    View *v = calloc(1, sizeof *v);
    Ser *s = calloc(1, sizeof *s);
    if (!v || !s) { free(v); free(s); return NULL; }
    s->si = si;
    v->update = ser_update;
    v->draw = ser_draw;
    v->destroy = ser_destroy;
    v->data = s;
    v->own = B_TRIANGLE;
    return v;
}

/* ===================================================================================
 * Series list (the Series section) and the "Add to Series" picker
 * =================================================================================== */

#define ROW_H 76.0f
#define TOP 76.0f

enum { LIST_SECTION, LIST_PICK };

typedef struct {
    int mode;
    BookSummary book;      /* LIST_PICK: the book being added */
    int focus;             /* 0 = "New Series", 1.. = series in name order */
    Scroll sc;
    int *order;
    int norder;
    unsigned rev;
    const BookSummary *lib0;
    int libn;
    int *first;            /* per series: library index of the first visible book (-1) */
    int *count;            /* per series: visible books */
    bool naming;           /* IME open for a new name */
    bool renaming;
    int target;            /* series index for rename / delete */
    Menu menu;
    Menu confirm;
} List;

static List *g_list;

static void list_sync(List *l) {
    const BookSummary *b0 = lib_book(0);
    int ln = lib_count();
    if (l->rev == g_rev && l->order && l->lib0 == b0 && l->libn == ln) return;
    l->rev = g_rev;
    l->lib0 = b0;
    l->libn = ln;
    free(l->order);
    free(l->first);
    free(l->count);
    size_t cnt = (size_t)(g_st.n ? g_st.n : 1);
    l->order = malloc(sizeof(int) * cnt);
    l->first = malloc(sizeof(int) * cnt);
    l->count = malloc(sizeof(int) * cnt);
    l->norder = l->order && l->first && l->count ? series_order(&g_st, l->order) : 0;
    /* Cover = first book in the series' current sort; counts only books present. */
    for (int si = 0; si < l->norder; si++) {
        VisCache c = {0};
        int vn;
        const int *vi = vis_get(&c, si, &vn);
        l->first[si] = vn > 0 && vi ? vi[0] : -1;
        l->count[si] = vn;
        free(c.idx);
    }
    if (l->focus > l->norder) l->focus = l->norder;
}

static void pick_add(List *l, int si) {
    char t[160];
    int rc = series_add(&g_st, si, l->book.id);
    if (rc == 1) {
        snprintf(t, sizeof t, "Already in %s", g_st.s[si].name);
        app_toast(t);
        return;
    }
    if (rc != 0) { app_toast("Couldn't add the book"); return; }
    persist();
    snprintf(t, sizeof t, "Added to %s", g_st.s[si].name);
    app_toast(t);
    dbg("series: '%s' added to '%s'", l->book.id, g_st.s[si].name);
    app_pop();
}

static void list_activate(List *l) {
    list_sync(l);
    if (l->focus == 0) {
        l->naming = ime_open("New series name", "", SERIES_NAME_MAX - 1, IME_TEXT);
        return;
    }
    int si = l->order[l->focus - 1];
    if (l->mode == LIST_PICK) pick_add(l, si);
    else app_push(view_series_new(si));
}

static void on_list_tap(int i, int b) {
    (void)b;
    if (!g_list || g_list->menu.open || g_list->confirm.open) return;
    g_list->focus = i;
    list_activate(g_list);
}

static void list_update(View *v, const Input *in, float dt) {
    List *l = v->data;
    list_sync(l);
    scroll_step(&l->sc, dt);
    if (l->naming || l->renaming) {
        if (!ime_active()) { l->naming = l->renaming = false; return; }
        char out[SERIES_NAME_MAX];
        int r = ime_poll(out, sizeof out);
        if (r == 0) return;
        bool naming = l->naming;
        l->naming = l->renaming = false;
        if (r != 1) return;
        if (naming) {
            int si = series_create(&g_st, out, wall_ms());
            if (si == -2 && l->mode == LIST_PICK) { pick_add(l, series_find(&g_st, out)); return; }
            if (si < 0) { app_toast(si == -2 ? "A series with that name already exists" : "The name can't be empty"); return; }
            dbg("series: created '%s'", g_st.s[si].name);
            if (l->mode == LIST_PICK) { pick_add(l, si); return; }
            persist();
            list_sync(l);
            for (int k = 0; k < l->norder; k++)
                if (l->order[k] == si) l->focus = k + 1;
            app_push(view_series_new(si));
        } else if (l->target >= 0 && l->target < g_st.n) {
            int rc = series_rename(&g_st, l->target, out);
            if (rc == 0) { persist(); app_toast("Series renamed"); }
            else app_toast(rc == -2 ? "A series with that name already exists" : "The name can't be empty");
        }
        return;
    }
    if (l->confirm.open) {
        int r = menu_update(&l->confirm, in);
        if (r == 0 && l->target >= 0 && l->target < g_st.n) {
            dbg("series: deleted '%s' (no files touched)", g_st.s[l->target].name);
            series_delete(&g_st, l->target);
            persist();
            app_toast("Series deleted");
        }
        return;
    }
    if (l->menu.open) {
        int r = menu_update(&l->menu, in);
        if (r == 0) app_push(view_series_new(l->target));
        else if (r == 1) l->renaming = ime_open("Rename series", g_st.s[l->target].name, SERIES_NAME_MAX - 1, IME_TEXT);
        else if (r == 2) {
            Series *x = &g_st.s[l->target];
            char t[140], msg[160];
            snprintf(t, sizeof t, "Delete \"%s\"?", x->name);
            snprintf(msg, sizeof msg, "%d book%s stay in your library; no files are deleted.", x->n, x->n == 1 ? "" : "s");
            menu_open(&l->confirm, t, msg);
            menu_add(&l->confirm, "Delete series", true);
            menu_add(&l->confirm, "Cancel", false);
            l->confirm.focus = 1;
        }
        return;
    }
    int n = l->norder + 1;
    if (in->repeat & B_UP && l->focus > 0) l->focus--;
    if (in->repeat & B_DOWN && l->focus + 1 < n) l->focus++;
    if (l->mode == LIST_SECTION && (in->repeat & B_LEFT || in->down & B_BACK)) { app_focus_sidebar(); return; }
    if (l->mode == LIST_PICK && in->down & B_BACK) { app_pop(); return; }
    if (in->down & B_OK) { list_activate(l); return; }
    if (l->mode == LIST_SECTION && in->down & B_TRIANGLE && l->focus > 0) {
        l->target = l->order[l->focus - 1];
        menu_open(&l->menu, g_st.s[l->target].name, NULL);
        menu_add(&l->menu, "Open", false);
        menu_add(&l->menu, "Rename series", false);
        menu_add(&l->menu, "Delete series", true);
    }
    float max = n * ROW_H - (CONTENT_BOTTOM - TOP) + 12;
    if (max < 0) max = 0;
    if (in->dragging) scroll_drag(&l->sc, in->drag_dy, max);
    else scroll_show(&l->sc, l->focus * ROW_H - (l->focus ? 30 : 0), (l->focus + 1) * ROW_H + 30, CONTENT_BOTTOM - TOP, max);
}

static void list_draw(View *v, bool focused) {
    List *l = v->data;
    g_list = l;
    list_sync(l);
    char line[200];
    if (l->mode == LIST_PICK) {
        gfx_text(F_BOLD, 30, CX, 26, C_TEXT, "Add to Series");
        gfx_fit(F_REG, 16, l->book.title, CW - 280, line, sizeof line);
        gfx_text_right(F_REG, 16, CX + CW, 36, C_SUB, line);
    } else {
        gfx_text(F_BOLD, 30, CX, 26, C_TEXT, "Series");
        snprintf(line, sizeof line, "%d series", l->norder);
        gfx_text_right(F_REG, 16, CX + CW, 36, C_SUB, line);
    }
    int n = l->norder + 1;
    clip_content(TOP - 6);
    int first = (int)(l->sc.y / ROW_H), last = first + (int)((CONTENT_BOTTOM - TOP) / ROW_H) + 2;
    for (int i = first; i <= last && i < n; i++) {
        float y = TOP + i * ROW_H - l->sc.y, h = ROW_H - 8;
        bool foc = focused && l->focus == i && !l->menu.open && !l->confirm.open;
        gfx_rrect(CX, y, CW, h, 12, foc ? C_PANEL2 : C_PANEL);
        if (foc) draw_focus_ring(CX, y, CW, h, 12);
        float tile = h - 14, tx = CX + 10 + tile + 18;
        if (i == 0) {
            gfx_rrect(CX + 10, y + 7, tile, tile, 8, C_ACCENT);
            gfx_text_center(F_BOLD, 34, CX + 10 + tile / 2, y + 7 + tile / 2, C_ON_ACCENT, "+");
            gfx_text(F_BOLD, 19, tx, y + 16, C_TEXT, "New Series");
            gfx_text(F_REG, 15, tx, y + 42, C_SUB, l->mode == LIST_PICK ? "Name it and add this book" : "Name a series, then add books");
        } else {
            int si = l->order[i - 1];
            Series *x = &g_st.s[si];
            int vn = l->count[si];
            const BookSummary *first_book = l->first[si] >= 0 ? lib_book(l->first[si]) : NULL;
            if (first_book) covers_draw(first_book->id, CX + 10, y + 7, tile, 1);
            else {
                gfx_rrect(CX + 10, y + 7, tile, tile, 8, C_PANEL2);
                gfx_icon(IC_SERIES, CX + 10 + tile / 2, y + 7 + tile / 2, 26, C_DIM);
            }
            bool has = l->mode == LIST_PICK && series_contains(&g_st, si, l->book.id);
            gfx_fit(F_BOLD, 19, x->name, CW - (tx - CX) - 70, line, sizeof line);
            gfx_text(F_BOLD, 19, tx, y + 16, C_TEXT, line);
            if (has) snprintf(line, sizeof line, "Already contains this book");
            else snprintf(line, sizeof line, "%d book%s \xC2\xB7 %s", vn, vn == 1 ? "" : "s", sort_name(x->sort));
            gfx_text(F_REG, 15, tx, y + 42, has ? C_ACCENT : C_SUB, line);
            if (has) gfx_icon(IC_CHECK, CX + CW - 28, y + h / 2, 18, C_ACCENT);
            else gfx_icon(IC_CHEVRON, CX + CW - 28, y + h / 2, 18, foc ? C_TEXT : C_DIM);
        }
        hit_add(CX, y, CW, h, on_list_tap, i, 0);
    }
    clip_off();
    if (l->menu.open) { menu_draw(&l->menu); return; }
    if (l->confirm.open) { menu_draw(&l->confirm); return; }
    if (focused) {
        hint(G_DPAD, "Move");
        if (l->focus == 0) hint(g_ok(), "New Series");
        else hint(g_ok(), l->mode == LIST_PICK ? "Add here" : "Open");
        if (l->mode == LIST_SECTION && l->focus > 0) hint(G_TRIANGLE, "Options");
        hint(g_back(), l->mode == LIST_PICK ? "Cancel" : "Back");
    }
}

static void list_destroy(View *v) {
    List *l = v->data;
    if (g_list == l) g_list = NULL;
    free(l->order);
    free(l->first);
    free(l->count);
    free(l);
    free(v);
}

static View *list_new(int mode, const BookSummary *b) {
    View *v = calloc(1, sizeof *v);
    List *l = calloc(1, sizeof *l);
    if (!v || !l) { free(v); free(l); return NULL; }
    l->mode = mode;
    l->target = -1;
    if (b) l->book = *b;
    v->update = list_update;
    v->draw = list_draw;
    v->destroy = list_destroy;
    v->data = l;
    v->own = B_TRIANGLE;
    return v;
}

View *view_series_list_new(void) { return list_new(LIST_SECTION, NULL); }
View *view_series_pick_new(const BookSummary *b) { return b ? list_new(LIST_PICK, b) : NULL; }
