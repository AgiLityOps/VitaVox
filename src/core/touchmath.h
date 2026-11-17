/* Pure touch helpers (host-tested): tap vs drag, slider scrubbing. */
#ifndef VITAVOX_TOUCHMATH_H
#define VITAVOX_TOUCHMATH_H
#include <stdbool.h>
#include <stdint.h>

#define TOUCH_TAP_SLOP 18.0f          /* px: moving further makes it a drag */
#define TOUCH_TAP_MAX_US 500000       /* a longer press is not a tap */

/* Has a touch that started at (sx, sy) moved far enough to be a drag? */
bool touch_moved(float sx, float sy, float x, float y);
/* Was a touch released after held_us a tap? (not moved past the slop, short) */
bool touch_is_tap(bool moved, uint64_t held_us);

/* Book/chapter time under finger x on a slider at [bar_x, bar_x + bar_w)
 * covering [span_start, span_end]; clamped to the span. */
double scrub_target(float x, float bar_x, float bar_w, double span_start, double span_end);
/* Is the finger close enough to the slider (vertically) for the scrub to count?
 * Further away the scrub is cancelled and release does nothing. */
bool scrub_in_range(float y, float bar_cy, float range);
/* Did a touch starting at (x, y) land on the slider's (generous) hit area? */
bool scrub_hit(float x, float y, float bar_x, float bar_w, float bar_cy, float half_h, float pad_x);

#endif
