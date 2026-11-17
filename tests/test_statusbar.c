/* 00.38: bottom-left clock + battery (statusbar.c) and the shared word wrap (textwrap.c). */
#include "hintlayout.h"
#include "statusbar.h"
#include "textwrap.h"
#include "test.h"

#include <stdlib.h>

TEST(statusbar_time_format) {
    char b[16];
    sb_format_time(12, 31, 0, b, sizeof b); CHECK_STR(b, "12:31 PM");
    sb_format_time(0, 5, 0, b, sizeof b); CHECK_STR(b, "12:05 AM");
    sb_format_time(9, 7, 0, b, sizeof b); CHECK_STR(b, "9:07 AM");   /* no leading zero */
    sb_format_time(13, 0, 0, b, sizeof b); CHECK_STR(b, "1:00 PM");
    sb_format_time(23, 59, 0, b, sizeof b); CHECK_STR(b, "11:59 PM");
    sb_format_time(11, 59, 0, b, sizeof b); CHECK_STR(b, "11:59 AM");
    sb_format_time(21, 5, 1, b, sizeof b); CHECK_STR(b, "21:05");
    sb_format_time(9, 5, 1, b, sizeof b); CHECK_STR(b, "09:05");
    sb_format_time(0, 0, 1, b, sizeof b); CHECK_STR(b, "00:00");
    sb_format_time(30, -4, 1, b, sizeof b); CHECK_STR(b, "23:00");     /* clamped */
}

TEST(statusbar_battery) {
    CHECK_EQ_INT(sb_level(87, 0), SB_LVL_NORMAL);
    CHECK_EQ_INT(sb_level(31, 0), SB_LVL_NORMAL);
    CHECK_EQ_INT(sb_level(30, 0), SB_LVL_LOW);
    CHECK_EQ_INT(sb_level(16, 0), SB_LVL_LOW);
    CHECK_EQ_INT(sb_level(15, 0), SB_LVL_CRITICAL);
    CHECK_EQ_INT(sb_level(0, 0), SB_LVL_CRITICAL);
    CHECK_EQ_INT(sb_level(10, 1), SB_LVL_CHARGING);
    CHECK_EQ_INT(sb_level(100, 1), SB_LVL_CHARGING);
    CHECK_NEAR(sb_fill_w(100), 18, 1e-6);
    CHECK_NEAR(sb_fill_w(50), 9, 1e-6);
    CHECK_NEAR(sb_fill_w(0), 0, 1e-6);
    CHECK_NEAR(sb_fill_w(150), 18, 1e-6);
    CHECK_NEAR(sb_fill_w(-3), 0, 1e-6);
}

static int g_reads;
static void fake_read(int *pct, int *chg, int *h, int *m, int *h24) {
    g_reads++;
    *pct = 87 - g_reads; *chg = 0; *h = 12; *m = 30 + g_reads; *h24 = 0;
}

TEST(statusbar_cache_once_a_second) {
    SbCache c = {0};
    g_reads = 0;
    CHECK_EQ_INT(sb_cache_tick(&c, 100.0, fake_read), 1);
    CHECK_STR(c.time, "12:31 PM");
    CHECK_STR(c.pct_text, "86%");
    for (int i = 1; i < 60; i++) sb_cache_tick(&c, 100.0 + i / 60.0, fake_read); /* one second of frames */
    CHECK_EQ_INT(g_reads, 1);
    CHECK_EQ_INT(sb_cache_tick(&c, 101.0, fake_read), 1);
    CHECK_EQ_INT(g_reads, 2);
    CHECK_STR(c.time, "12:32 PM");
    /* the clock going backwards (another time base) refreshes at once */
    CHECK_EQ_INT(sb_cache_tick(&c, 5.0, fake_read), 1);
}

/* Ink widths of Manrope 500 at 15 px (measured with the bundled font). */
#define W_TIME_WIDEST 63.63f /* "12:00 AM" advance */
#define W_PCT_100 38.39f     /* "100%" advance */
#define W_L 31.2f
#define W_SELECT 60.39f
#define W_ROUND 24.0f

TEST(statusbar_clear_of_hints) {
    float end = 20 + sb_total_w(W_TIME_WIDEST, W_PCT_100);
    CHECK_NEAR(end, 20 + 63.63f + 16 + 24 + 3 + 10 + 38.39f, 1e-3);
    /* the widest hint bar (player, "Book details" focused, (L) (R) Skip) still clears it */
    HintItem p[6] = {
        {84.17f, {W_ROUND, 0}, 1, 0, {0, 0}},
        {41.65f, {W_ROUND, 0}, 1, 0, {0, 0}},
        {28.72f, {W_L, W_L}, 2, 0, {0, 0}},
        {64.24f, {W_ROUND, 0}, 1, 0, {0, 0}},
        {71.42f, {W_SELECT, 0}, 1, 0, {0, 0}},
        {33.05f, {W_ROUND, 0}, 1, 0, {0, 0}},
    };
    float left = hint_layout(p, 6, 940);
    printf("  status ends at x=%.1f, leftmost hint at x=%.1f\n", end, left);
    CHECK(end + 24 <= left); /* the clock is shown (the app's margin) */
}

/* 10 px per character (UTF-8 aware) */
static float mono(const char *t, void *ud) {
    (void)ud;
    int n = 0;
    for (const unsigned char *p = (const unsigned char *)t; *p; p++) if ((*p & 0xC0) != 0x80) n++;
    return n * 10.0f;
}

TEST(textwrap_layout) {
    int st[64], ln[64];
    const char *s = "aaa bbb ccc\n\nddd";
    int n = wrap_layout(mono, NULL, 75, s, st, ln, 64);
    CHECK_EQ_INT(n, 4); /* "aaa bbb" / "ccc" / "" / "ddd" */
    CHECK_EQ_INT(st[0], 0); CHECK_EQ_INT(ln[0], 7);
    CHECK_EQ_INT(st[1], 8); CHECK_EQ_INT(ln[1], 3);
    CHECK_EQ_INT(ln[2], 0);
    CHECK_EQ_INT(st[3], 13); CHECK_EQ_INT(ln[3], 3);
    CHECK_EQ_INT(wrap_layout(mono, NULL, 75, s, NULL, NULL, 999), 4);
    /* paragraphs used to stop the old wrap at the first blank line */
    CHECK_EQ_INT(wrap_layout(mono, NULL, 1000, "one\n\ntwo\n\nthree", NULL, NULL, 999), 5);
    /* a word wider than the line breaks between characters, never inside one */
    const char *cjk = "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\xE6\x97\xA5"; /* 7 chars */
    n = wrap_layout(mono, NULL, 30, cjk, st, ln, 64);
    CHECK_EQ_INT(n, 3);
    CHECK_EQ_INT(ln[0], 9); CHECK_EQ_INT(ln[1], 9); CHECK_EQ_INT(ln[2], 3);
    n = wrap_layout(mono, NULL, 50, "see http://example.com/a/very/long/path ok", st, ln, 64);
    CHECK(n >= 4);
    int fits = 1;
    for (int i = 0; i < n; i++) fits &= ln[i] * 10 <= 50 || ln[i] == 1;
    CHECK(fits);
    /* narrower than one character: still one per line, always progresses */
    CHECK_EQ_INT(wrap_layout(mono, NULL, 5, "abc", NULL, NULL, 999), 3);
    CHECK_EQ_INT(wrap_layout(mono, NULL, 100, "", NULL, NULL, 999), 0);
    CHECK_EQ_INT(wrap_layout(mono, NULL, 100, NULL, NULL, NULL, 999), 0);
    /* an 8 KB description wraps fully, line ranges stay inside the text */
    char *big = malloc(8200);
    int o = 0;
    while (o < 8000) { memcpy(big + o, "lorem ipsum dolor sit amet\n", 27); o += 27; }
    big[o] = 0;
    static int bs[1000], bl[1000];
    n = wrap_layout(mono, NULL, 120, big, bs, bl, 1000);
    CHECK(n > 500);
    int inside = 1;
    for (int i = 0; i < n; i++) inside &= bs[i] >= 0 && bs[i] + bl[i] <= o;
    CHECK(inside);
    free(big);
}

void suite_statusbar(void) {
    RUN(statusbar_time_format);
    RUN(statusbar_battery);
    RUN(statusbar_cache_once_a_second);
    RUN(statusbar_clear_of_hints);
    RUN(textwrap_layout);
}
