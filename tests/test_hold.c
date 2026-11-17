/* SELECT-style short press vs 2 s hold (holdbtn.c). */
#include "test.h"
#include "holdbtn.h"

TEST(hold_short_press_on_early_release) {
    HoldTrack h;
    float p = -1;
    hold_reset(&h);
    CHECK_EQ_INT(hold_update(&h, true, 0.016f, &p), HOLD_ARMED);
    CHECK_NEAR(p, 0.016f / HOLD_SEC, 1e-5);
    CHECK_EQ_INT(hold_update(&h, true, 0.5f, &p), HOLD_ARMED);
    CHECK(p > 0 && p < 1);
    CHECK_EQ_INT(hold_update(&h, false, 0.016f, &p), HOLD_SHORT);
    CHECK_NEAR(p, 0, 1e-9);
    CHECK(!h.held);
    CHECK_NEAR(h.t, 0, 1e-9);
    /* a second tap is independent */
    CHECK_EQ_INT(hold_update(&h, true, 0.016f, &p), HOLD_ARMED);
    CHECK_EQ_INT(hold_update(&h, false, 0.016f, &p), HOLD_SHORT);
}

TEST(hold_fires_at_two_seconds_and_swallows_release) {
    HoldTrack h;
    float p = 0;
    hold_reset(&h);
    float t = 0;
    HoldEvent ev = HOLD_IDLE;
    while (t < 1.9f) {
        ev = hold_update(&h, true, 0.1f, &p);
        t += 0.1f;
        CHECK_EQ_INT(ev, HOLD_ARMED);
    }
    CHECK(p < 1);
    ev = hold_update(&h, true, 0.2f, &p);
    CHECK_EQ_INT(ev, HOLD_FIRED);
    CHECK_NEAR(p, 1, 1e-5);
    /* still held: no second fire, progress stays full */
    CHECK_EQ_INT(hold_update(&h, true, 0.1f, &p), HOLD_IDLE);
    CHECK_NEAR(p, 1, 1e-5);
    /* release after fire is swallowed (not a short press) */
    CHECK_EQ_INT(hold_update(&h, false, 0.016f, &p), HOLD_IDLE);
    CHECK_NEAR(p, 0, 1e-9);
    CHECK(!h.held && !h.fired);
}

TEST(hold_exact_two_seconds_and_large_dt) {
    HoldTrack h;
    float p;
    hold_reset(&h);
    CHECK_EQ_INT(hold_update(&h, true, HOLD_SEC, &p), HOLD_FIRED);
    CHECK_NEAR(p, 1, 1e-6);
    hold_reset(&h);
    /* a long paused frame still fires once */
    CHECK_EQ_INT(hold_update(&h, true, 5.0f, &p), HOLD_FIRED);
    CHECK_NEAR(p, 1, 1e-6);
    CHECK_NEAR(h.t, HOLD_SEC, 1e-6);
    hold_reset(&h);
    /* negative dt is ignored */
    CHECK_EQ_INT(hold_update(&h, true, -1.0f, &p), HOLD_ARMED);
    CHECK_NEAR(p, 0, 1e-9);
    /* no tracker: idle, progress cleared */
    p = 1;
    CHECK_EQ_INT(hold_update(NULL, true, 1.0f, &p), HOLD_IDLE);
    CHECK_NEAR(p, 0, 1e-9);
    /* release with no prior press */
    hold_reset(&h);
    CHECK_EQ_INT(hold_update(&h, false, 0.016f, &p), HOLD_IDLE);
}

TEST(hold_reset_cancels_an_in_progress_press) {
    HoldTrack h;
    float p;
    hold_reset(&h);
    hold_update(&h, true, 1.5f, &p);
    hold_reset(&h);
    CHECK_EQ_INT(hold_update(&h, false, 0.016f, &p), HOLD_IDLE);
    /* a fresh press after reset can still fire */
    CHECK_EQ_INT(hold_update(&h, true, HOLD_SEC, &p), HOLD_FIRED);
}

void suite_hold(void) {
    RUN(hold_short_press_on_early_release);
    RUN(hold_fires_at_two_seconds_and_swallows_release);
    RUN(hold_exact_two_seconds_and_large_dt);
    RUN(hold_reset_cancels_an_in_progress_press);
}
