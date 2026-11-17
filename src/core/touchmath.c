#include "touchmath.h"

#include <math.h>

bool touch_moved(float sx, float sy, float x, float y) {
    return hypotf(x - sx, y - sy) > TOUCH_TAP_SLOP;
}

bool touch_is_tap(bool moved, uint64_t held_us) {
    return !moved && held_us < TOUCH_TAP_MAX_US;
}

double scrub_target(float x, float bar_x, float bar_w, double span_start, double span_end) {
    if (bar_w <= 0 || span_end <= span_start) return span_start;
    double f = (x - bar_x) / bar_w;
    if (f < 0) f = 0;
    if (f > 1) f = 1;
    return span_start + (span_end - span_start) * f;
}

bool scrub_in_range(float y, float bar_cy, float range) {
    return fabsf(y - bar_cy) <= range;
}

bool scrub_hit(float x, float y, float bar_x, float bar_w, float bar_cy, float half_h, float pad_x) {
    return x >= bar_x - pad_x && x < bar_x + bar_w + pad_x && fabsf(y - bar_cy) <= half_h;
}
