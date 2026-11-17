#include "holdbtn.h"

#include <string.h>

void hold_reset(HoldTrack *h) {
    if (h) memset(h, 0, sizeof *h);
}

HoldEvent hold_update(HoldTrack *h, bool down, float dt, float *progress) {
    if (progress) *progress = 0;
    if (!h) return HOLD_IDLE;
    if (dt < 0) dt = 0;
    if (!down) {
        HoldEvent ev = (h->held && !h->fired) ? HOLD_SHORT : HOLD_IDLE;
        hold_reset(h);
        return ev;
    }
    if (!h->held) {
        h->held = true;
        h->fired = false;
        h->t = 0;
    }
    h->t += dt;
    if (h->t > HOLD_SEC) h->t = HOLD_SEC;
    if (progress) *progress = h->t / HOLD_SEC;
    if (!h->fired && h->t >= HOLD_SEC - 1e-6f) {
        h->fired = true;
        if (progress) *progress = 1;
        return HOLD_FIRED;
    }
    return h->fired ? HOLD_IDLE : HOLD_ARMED;
}
