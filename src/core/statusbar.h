/* 00.38: bottom-left status of the hint bar: clock, battery icon, percentage.
 * Pure formatting/layout maths (host-tested); app.c draws it and sys.c reads
 * the system clock and battery. */
#ifndef VITAVOX_STATUSBAR_H
#define VITAVOX_STATUSBAR_H
#include <stddef.h>

/* Battery icon geometry (px). */
#define SB_BATT_W 24
#define SB_BATT_H 12
#define SB_BATT_R 3
#define SB_BATT_STROKE 2
#define SB_BATT_INSET 3
#define SB_NUB_W 3
#define SB_NUB_H 6
#define SB_GAP_TIME 16   /* time text -> icon */
#define SB_GAP_PCT 10    /* icon (with its nub) -> percentage */

typedef enum { SB_LVL_NORMAL, SB_LVL_LOW, SB_LVL_CRITICAL, SB_LVL_CHARGING } SbLevel;

/* "12:31 PM" / "9:05 AM" (12 h: no leading zero, 12 for noon and midnight) or
 * "21:05" / "09:05" (24 h). Out-of-range values are clamped. */
void sb_format_time(int hour, int minute, int h24, char *out, size_t n);
/* 0..100 */
int sb_clamp_pct(int pct);
/* charging wins, then <= 15 critical, <= 30 low */
SbLevel sb_level(int pct, int charging);
/* inner fill width: (SB_BATT_W - 2 * SB_BATT_INSET) * pct / 100 */
float sb_fill_w(int pct);
/* Width of the whole status (time, gap, icon + nub, gap, percentage) given the
 * ink widths of its two texts; the right-aligned hints must start after it. */
float sb_total_w(float time_w, float pct_w);

/* Once-a-second cache for the reads (battery calls can be slow on the Vita). */
typedef struct {
    double next_s;      /* refresh due at this time (seconds, any clock) */
    int pct, charging, hour, minute, h24;
    char time[16], pct_text[8];
} SbCache;
/* Returns 1 when it refreshed. read(): fills the raw values. */
int sb_cache_tick(SbCache *c, double now_s, void (*read)(int *pct, int *charging, int *hour, int *minute, int *h24));

#endif
