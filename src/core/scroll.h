/* Smooth list scrolling with touch drag, momentum, and no snap-back (pure C,
 * host-tested; used by every list/grid and the player's chapter list).
 *
 *   scroll_step  once per frame (animation, momentum, end-of-drag detection)
 *   scroll_drag  on frames the finger is dragging
 *   scroll_show  "keep this focused range visible" (D-pad); ignored after a
 *                touch scroll until the focus actually moves, so letting go
 *                of the list never springs back to the focused/playing row. */
#ifndef VITAVOX_SCROLL_H
#define VITAVOX_SCROLL_H
#include <stdbool.h>

typedef struct {
    float y, target;
    /* touch state (zero-initialised is fine) */
    float vel;          /* momentum, px/s (content moves up for > 0) */
    float drag_v;       /* smoothed finger speed while dragging */
    float max;          /* last known scroll limit */
    float dt;           /* last frame time from scroll_step */
    bool in_drag, dragged;
    bool free;          /* scrolled by touch: scroll_show leaves it alone ... */
    float free_top;     /* ... while it keeps asking for this same range */
    float last_top;
    bool have_top, have_free_top;
} Scroll;

#define SCROLL_MIN_FLING 60.0f   /* px/s: slower releases just stop */
#define SCROLL_FRICTION 3.5f     /* 1/s: momentum decay rate */

void scroll_step(Scroll *s, float dt);
/* moves target so [top, bottom) (content coordinates) is visible in a view of height view_h */
void scroll_show(Scroll *s, float top, float bottom, float view_h, float max);
void scroll_drag(Scroll *s, float dy, float max);
/* Puts y/target at pos at once and clears touch state (e.g. a menu opening on the current row). */
void scroll_jump(Scroll *s, float pos, float max);
/* Animates to pos (D-pad paging, sort reset), ending any touch scroll/momentum. */
void scroll_to(Scroll *s, float pos);
/* Stops momentum (finger down on a moving list). */
void scroll_stop(Scroll *s);

#endif
