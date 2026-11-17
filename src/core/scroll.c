#include "scroll.h"

#include <math.h>

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

/* Finger lifted after a drag: keep its speed as momentum, and stop following
 * the focused row until the focus moves. */
static void release(Scroll *s) {
    s->in_drag = false;
    s->vel = fabsf(s->drag_v) >= SCROLL_MIN_FLING ? s->drag_v : 0;
    s->free = true;
    s->free_top = s->last_top;
    s->have_free_top = s->have_top; /* else adopt the first range asked for */
}

void scroll_step(Scroll *s, float dt) {
    if (dt <= 0) dt = 1.0f / 60.0f;
    if (dt > 0.1f) dt = 0.1f;
    s->dt = dt;
    /* no scroll_drag/scroll_show since the last step: the finger was lifted */
    if (s->in_drag && !s->dragged) release(s);
    s->dragged = false;
    if (s->vel != 0 && !s->in_drag) {
        s->target = clampf(s->target + s->vel * dt, 0, s->max > 0 ? s->max : 0);
        s->y = s->target;
        s->vel *= expf(-dt * SCROLL_FRICTION);
        if (fabsf(s->vel) < SCROLL_MIN_FLING / 2 || s->target <= 0 || s->target >= s->max) s->vel = 0;
        return;
    }
    float k = 1 - expf(-dt * 14);
    s->y += (s->target - s->y) * k;
    if (fabsf(s->target - s->y) < 0.5f) s->y = s->target;
}

void scroll_show(Scroll *s, float top, float bottom, float view_h, float max) {
    s->max = max > 0 ? max : 0;
    if (s->in_drag) release(s); /* show instead of drag: the finger is up */
    if (s->free) {
        if (!s->have_free_top) { s->free_top = top; s->have_free_top = true; }
        if (top == s->free_top) { s->last_top = top; return; } /* focus unchanged: stay where the finger left it */
        s->free = false; /* the focus moved (D-pad): follow it again */
        s->vel = 0;
    }
    s->last_top = top;
    s->have_top = true;
    if (top < s->target) s->target = top;
    if (bottom > s->target + view_h) s->target = bottom - view_h;
    if (s->target > s->max) s->target = s->max;
    if (s->target < 0) s->target = 0;
}

void scroll_drag(Scroll *s, float dy, float max) {
    s->max = max > 0 ? max : 0;
    if (!s->in_drag) { s->in_drag = true; s->drag_v = 0; s->vel = 0; }
    s->dragged = true;
    float dt = s->dt > 0 ? s->dt : 1.0f / 60.0f;
    float v = -dy / dt;
    s->drag_v = s->drag_v * 0.5f + v * 0.5f;
    s->target = clampf(s->target - dy, 0, s->max);
    s->y = s->target;
}

void scroll_jump(Scroll *s, float pos, float max) {
    s->max = max > 0 ? max : 0;
    s->y = s->target = clampf(pos, 0, s->max);
    s->vel = s->drag_v = 0;
    s->in_drag = s->dragged = s->free = false;
}

void scroll_to(Scroll *s, float pos) {
    s->target = pos < 0 ? 0 : pos;
    s->vel = 0;
    s->free = false;
}

void scroll_stop(Scroll *s) { s->vel = 0; }
