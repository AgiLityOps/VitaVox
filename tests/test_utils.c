#include <stdlib.h>

#include "color.h"
#include "library.h"
#include "image.h"
#include "test.h"
#include <stdio.h>
#include <string.h>

TEST(accent_colour) {
    uint8_t px[16 * 16 * 4];
    for (int i = 0; i < 256; i++) { px[i * 4] = 20; px[i * 4 + 1] = 40; px[i * 4 + 2] = 200; px[i * 4 + 3] = 255; }
    Rgb a = accent_from_rgba(px, 16, 16);
    CHECK(a.b > a.r && a.b > a.g);
    float h, s, l;
    rgb_to_hsl(a, &h, &s, &l);
    CHECK(l >= 0.44f && l <= 0.71f);
    for (int i = 0; i < 256; i++) { px[i * 4] = px[i * 4 + 1] = px[i * 4 + 2] = 128; }
    a = accent_from_rgba(px, 16, 16);
    CHECK_EQ_INT(a.r, 247); /* grey cover -> default amber */
    Rgb red = hsl_to_rgb(0, 1, 0.5f);
    CHECK_EQ_INT(red.r, 255); CHECK_EQ_INT(red.g, 0);
}
TEST(crop_resize) {
    uint8_t big[4 * 2 * 3] = {10, 10, 10, 10, 10, 10, 200, 0, 0, 100, 0, 0, 10, 10, 10, 10, 10, 10, 200, 0, 0, 100, 0, 0};
    uint8_t out[1 * 4];
    img_crop_resize(big, 4, 2, 3, out, 1); /* centre square = columns 1..2 */
    CHECK_EQ_INT(out[0], 105);
    CHECK_EQ_INT(out[3], 255);
}
TEST(crop_resize_alpha_weighted) {
    /* 2x2 RGBA: opaque red column + fully transparent black column -> 1 px:
     * red at half alpha (no dark fringe). */
    uint8_t src[4 * 4] = { 255, 0, 0, 255, 0, 0, 0, 0, 255, 0, 0, 255, 0, 0, 0, 0 };
    uint8_t out[4];
    img_crop_resize(src, 2, 2, 4, out, 1);
    CHECK_EQ_INT(out[0], 255); CHECK_EQ_INT(out[1], 0); CHECK_EQ_INT(out[3], 127);
    uint8_t clear[4] = { 9, 9, 9, 0 };
    img_crop_resize(clear, 1, 1, 4, out, 1);
    CHECK_EQ_INT(out[3], 0);
}
TEST(library_sort_filter) {
    BookSummary b[4] = {
        {.id = "1", .title = "The Odyssey", .author = "Homer", .added_at = 5},
        {.id = "2", .title = "Dune", .author = "Herbert", .added_at = 9},
        {.id = "3", .title = "an Echo", .author = "", .added_at = 1},
        {.id = "4", .title = "Circe", .author = "Miller", .series = "Greek", .added_at = 7},
    };
    int idx[4] = {0, 1, 2, 3}, out[4];
    lib_sort(b, idx, 4, SORT_TITLE);
    CHECK_EQ_INT(idx[0], 3); CHECK_EQ_INT(idx[1], 1); CHECK_EQ_INT(idx[2], 2); CHECK_EQ_INT(idx[3], 0);
    lib_sort(b, idx, 4, SORT_AUTHOR);
    CHECK_EQ_INT(idx[0], 1); CHECK_EQ_INT(idx[3], 2);
    lib_sort(b, idx, 4, SORT_RECENT);
    CHECK_EQ_INT(idx[0], 1); CHECK_EQ_INT(idx[1], 3);
    CHECK_EQ_INT(lib_filter(b, idx, 4, "HOMER", out), 1);
    CHECK_EQ_INT(out[0], 0);
    CHECK_EQ_INT(lib_filter(b, idx, 4, "greek", out), 1);
    CHECK_EQ_INT(lib_filter(b, idx, 4, "", out), 4);
    CHECK_EQ_INT(lib_find(b, 4, "3"), 2);
    CHECK_EQ_INT(lib_find(b, 4, "x"), -1);
}
static int title_pos(const BookSummary *b, const int *idx, int n, const char *title) {
    for (int i = 0; i < n; i++)
        if (!strcmp(b[idx[i]].title, title)) return i;
    return -1;
}
TEST(library_title_sort_numbers) {
    static const char *titles[] = {
        "The Count of Monte Cristo 10", "The Count of Monte Cristo 07", "The Count of Monte Cristo 1",
        "Book 10", "book 9", "The Book 2",
        "Vol. 10", "Vol. 2", "vol. 1",
        "a Canterbury Tale", "Zebra", "Book",
    };
    enum { N = sizeof titles / sizeof *titles };
    BookSummary b[N];
    int idx[N];
    memset(b, 0, sizeof b);
    for (int i = 0; i < N; i++) {
        snprintf(b[i].id, sizeof b[i].id, "%d", i);
        snprintf(b[i].title, sizeof b[i].title, "%s", titles[i]);
        idx[i] = i;
    }
    lib_sort(b, idx, N, SORT_TITLE);
    /* Monte Cristo 1 < 07 < 10 */
    CHECK(title_pos(b, idx, N, "The Count of Monte Cristo 1") < title_pos(b, idx, N, "The Count of Monte Cristo 07"));
    CHECK(title_pos(b, idx, N, "The Count of Monte Cristo 07") < title_pos(b, idx, N, "The Count of Monte Cristo 10"));
    /* "Book" < "The Book 2" (article ignored) < "book 9" (case ignored) < "Book 10" */
    CHECK(title_pos(b, idx, N, "Book") < title_pos(b, idx, N, "The Book 2"));
    CHECK(title_pos(b, idx, N, "The Book 2") < title_pos(b, idx, N, "book 9"));
    CHECK(title_pos(b, idx, N, "book 9") < title_pos(b, idx, N, "Book 10"));
    CHECK(title_pos(b, idx, N, "vol. 1") < title_pos(b, idx, N, "Vol. 2"));
    CHECK(title_pos(b, idx, N, "Vol. 2") < title_pos(b, idx, N, "Vol. 10"));
    /* "a Canterbury Tale" sorts as Canterbury: after Book*, before Count*; Zebra last */
    CHECK(title_pos(b, idx, N, "Book 10") < title_pos(b, idx, N, "a Canterbury Tale"));
    CHECK(title_pos(b, idx, N, "a Canterbury Tale") < title_pos(b, idx, N, "The Count of Monte Cristo 1"));
    CHECK_EQ_INT(title_pos(b, idx, N, "Zebra"), N - 1);
    CHECK_EQ_INT(title_pos(b, idx, N, "Book"), 0);
    /* "Book 02" and "Book 2" compare equal naturally: still a fixed order */
    BookSummary e[2];
    memset(e, 0, sizeof e);
    snprintf(e[0].title, sizeof e[0].title, "Book 2");
    snprintf(e[1].title, sizeof e[1].title, "Book 02");
    int ei[2] = {0, 1}, ej[2] = {1, 0};
    lib_sort(e, ei, 2, SORT_TITLE);
    lib_sort(e, ej, 2, SORT_TITLE);
    CHECK_EQ_INT(ei[0], ej[0]);
    CHECK_EQ_INT(ei[0], 1); /* "Book 02" < "Book 2" by the plain fallback */
}
void suite_utils(void) {
    RUN(accent_colour);
    RUN(crop_resize);
    RUN(crop_resize_alpha_weighted);
    RUN(library_sort_filter);
    RUN(library_title_sort_numbers);
}
