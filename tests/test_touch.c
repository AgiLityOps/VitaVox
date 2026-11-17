/* 00.32 touch logic: tap vs drag, slider scrub mapping, list scrolling with
 * momentum and no snap-back (scroll.c, touchmath.c). */
#include "test.h"
#include "scroll.h"
#include "touchmath.h"

#include <string.h>

#define DT (1.0f / 60.0f)

TEST(touch_tap_vs_drag) {
    CHECK(!touch_moved(100, 100, 100, 100));
    CHECK(!touch_moved(100, 100, 110, 110));       /* 14 px: still a tap */
    CHECK(!touch_moved(100, 100, 118, 100));       /* exactly the slop */
    CHECK(touch_moved(100, 100, 119, 100));
    CHECK(touch_moved(100, 100, 100, 70));
    CHECK(touch_is_tap(false, 100000));
    CHECK(!touch_is_tap(true, 100000));            /* moved: a drag */
    CHECK(!touch_is_tap(false, 600000));           /* held too long */
}

TEST(touch_scrub_mapping) {
    /* bar at x 420..904 showing a chapter 3600..5400 s of a multi-file book (book-wide times) */
    CHECK_NEAR(scrub_target(420, 420, 484, 3600, 5400), 3600, 1e-9);
    CHECK_NEAR(scrub_target(904, 420, 484, 3600, 5400), 5400, 1e-9);
    CHECK_NEAR(scrub_target(420 + 484 * 0.75f, 420, 484, 3600, 5400), 4950, 1e-3);
    CHECK_NEAR(scrub_target(300, 420, 484, 3600, 5400), 3600, 1e-9);   /* left of the bar: start */
    CHECK_NEAR(scrub_target(960, 420, 484, 3600, 5400), 5400, 1e-9);   /* right of it: end */
    CHECK_NEAR(scrub_target(600, 420, 0, 10, 20), 10, 1e-9);           /* degenerate bar */
    CHECK_NEAR(scrub_target(600, 420, 484, 10, 10), 10, 1e-9);         /* empty span */
    /* hit area: generous vertically, a little past the ends */
    CHECK(scrub_hit(420, 279, 420, 484, 279, 34, 14));
    CHECK(scrub_hit(410, 250, 420, 484, 279, 34, 14));
    CHECK(scrub_hit(910, 310, 420, 484, 279, 34, 14));
    CHECK(!scrub_hit(420, 330, 420, 484, 279, 34, 14));
    CHECK(!scrub_hit(400, 279, 420, 484, 279, 34, 14));
    /* cancel when far off vertically */
    CHECK(scrub_in_range(279 + 100, 279, 110));
    CHECK(scrub_in_range(279 - 110, 279, 110));
    CHECK(!scrub_in_range(279 + 140, 279, 110));
}

/* One frame of a list that keeps row `focus` visible (like the chapter menu). */
static void frame(Scroll *s, int focus, float max, int dragging, float dy) {
    scroll_step(s, DT);
    if (dragging) scroll_drag(s, dy, max);
    else scroll_show(s, focus * 52.0f - 52, (focus + 2) * 52.0f, 320, max);
}

TEST(scroll_no_snap_back_after_drag) {
    Scroll s;
    memset(&s, 0, sizeof s);
    int n = 200, focus = 10;
    float max = n * 52.0f - 350;
    scroll_jump(&s, focus * 52.0f - 120, max);       /* menu opens centred on the playing chapter */
    CHECK_NEAR(s.y, 400, 1e-3);
    for (int i = 0; i < 10; i++) frame(&s, focus, max, 0, 0);
    CHECK_NEAR(s.y, 400, 1e-3);                      /* stays put while untouched */
    for (int i = 0; i < 40; i++) frame(&s, focus, max, 1, -25); /* drag up: content scrolls down 1000 px */
    CHECK_NEAR(s.y, 1400, 1e-3);
    for (int i = 0; i < 10; i++) frame(&s, focus, max, 1, 0);   /* hold still, then lift: no fling */
    float at_release = s.y;
    for (int i = 0; i < 180; i++) frame(&s, focus, max, 0, 0);
    CHECK_NEAR(s.y, at_release, 1.0);                /* no snap back to the playing chapter */
    /* the D-pad moves the selection: the list follows it again */
    for (int i = 0; i < 180; i++) frame(&s, focus + 1, max, 0, 0);
    CHECK(s.y <= (focus + 1) * 52.0f - 52 + 0.5f);
    CHECK(s.y >= (focus + 3) * 52.0f - 320 - 0.5f);
}

TEST(scroll_fling_momentum) {
    Scroll s;
    memset(&s, 0, sizeof s);
    float max = 200 * 52.0f - 350;
    for (int i = 0; i < 10; i++) frame(&s, 0, max, 1, -30);     /* quick flick: 1800 px/s */
    float at_release = s.y;
    CHECK_NEAR(at_release, 300, 1e-3);
    frame(&s, 0, max, 0, 0);
    CHECK(s.vel > SCROLL_MIN_FLING);
    float prev = s.y;
    int monotonic = 1;
    for (int i = 0; i < 600; i++) {
        frame(&s, 0, max, 0, 0);
        if (s.y < prev - 1e-3f) monotonic = 0;
        prev = s.y;
    }
    CHECK(monotonic);                                /* keeps going the same way, never back */
    CHECK(s.y > at_release + 200);                   /* momentum carried it on */
    CHECK_NEAR(s.vel, 0, 1e-6);                      /* and it stopped */
    CHECK(s.y <= max);
    float rest = s.y;
    for (int i = 0; i < 60; i++) frame(&s, 0, max, 0, 0);
    CHECK_NEAR(s.y, rest, 1e-3);                     /* rests where it stopped (focus 0 is off screen) */
    /* a hard fling near the end clamps at max */
    for (int i = 0; i < 30; i++) frame(&s, 0, max, 1, -200);
    for (int i = 0; i < 300; i++) frame(&s, 0, max, 0, 0);
    CHECK(s.y <= max + 1e-3f);
    CHECK_NEAR(s.y, max, 1e-3);
    /* fling back up stops at 0 */
    for (int i = 0; i < 400; i++) frame(&s, 0, max, 1, 200);
    for (int i = 0; i < 300; i++) frame(&s, 0, max, 0, 0);
    CHECK_NEAR(s.y, 0, 1e-3);
}

TEST(scroll_stop_and_scroll_to) {
    Scroll s;
    memset(&s, 0, sizeof s);
    float max = 5000;
    for (int i = 0; i < 10; i++) frame(&s, 0, max, 1, -30);
    frame(&s, 0, max, 0, 0);
    CHECK(s.vel > 0);
    scroll_stop(&s);                                 /* finger down on a moving list */
    float y = s.y;
    for (int i = 0; i < 30; i++) frame(&s, 0, max, 0, 0);
    CHECK_NEAR(s.y, y, 1e-3);
    scroll_to(&s, 0);                                /* sort change: back to the top */
    for (int i = 0; i < 120; i++) frame(&s, 0, max, 0, 0);
    CHECK_NEAR(s.y, 0, 1e-3);
    /* D-pad only (never touched): plain keep-visible behaviour */
    Scroll d;
    memset(&d, 0, sizeof d);
    for (int i = 0; i < 120; i++) frame(&d, 30, max, 0, 0);
    CHECK_NEAR(d.y, 32 * 52.0f - 320, 1.0);
}

void suite_touch(void) {
    RUN(touch_tap_vs_drag);
    RUN(touch_scrub_mapping);
    RUN(scroll_no_snap_back_after_drag);
    RUN(scroll_fling_momentum);
    RUN(scroll_stop_and_scroll_to);
}
