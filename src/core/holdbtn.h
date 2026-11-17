/* Short press vs a timed hold of one button (pure C, host-tested).
 * Used for SELECT: release before HOLD_SEC is a short press (sort); reaching
 * HOLD_SEC fires the long action (list/grid) and the later release is ignored. */
#ifndef VITAVOX_HOLDBTN_H
#define VITAVOX_HOLDBTN_H
#include <stdbool.h>

#define HOLD_SEC 2.0f

typedef enum {
    HOLD_IDLE = 0,  /* nothing this step (or still held after a long fire) */
    HOLD_ARMED,     /* held, not yet HOLD_SEC */
    HOLD_FIRED,     /* crossed HOLD_SEC this step */
    HOLD_SHORT      /* released before HOLD_SEC */
} HoldEvent;

typedef struct {
    bool held;   /* button was down last step */
    bool fired;  /* long action already taken this press */
    float t;     /* seconds held */
} HoldTrack;

void hold_reset(HoldTrack *h);
/* down: button is down this step. dt is frame time (seconds).
 * progress (optional) is 0..1 while held, 1 after fire until release, else 0. */
HoldEvent hold_update(HoldTrack *h, bool down, float dt, float *progress);

#endif
