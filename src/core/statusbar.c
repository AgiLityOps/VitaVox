#include "statusbar.h"

#include <stdio.h>
#include <string.h>

void sb_format_time(int hour, int minute, int h24, char *out, size_t n) {
    if (!out || !n) return;
    if (hour < 0) hour = 0;
    if (hour > 23) hour = 23;
    if (minute < 0) minute = 0;
    if (minute > 59) minute = 59;
    if (h24) {
        snprintf(out, n, "%02d:%02d", hour, minute);
        return;
    }
    int h = hour % 12;
    if (h == 0) h = 12;
    snprintf(out, n, "%d:%02d %s", h, minute, hour < 12 ? "AM" : "PM");
}

int sb_clamp_pct(int pct) { return pct < 0 ? 0 : pct > 100 ? 100 : pct; }

SbLevel sb_level(int pct, int charging) {
    pct = sb_clamp_pct(pct);
    if (charging) return SB_LVL_CHARGING;
    if (pct <= 15) return SB_LVL_CRITICAL;
    if (pct <= 30) return SB_LVL_LOW;
    return SB_LVL_NORMAL;
}

float sb_fill_w(int pct) {
    return (float)(SB_BATT_W - 2 * SB_BATT_INSET) * (float)sb_clamp_pct(pct) / 100.0f;
}

float sb_total_w(float time_w, float pct_w) {
    return time_w + SB_GAP_TIME + SB_BATT_W + SB_NUB_W + SB_GAP_PCT + pct_w;
}

int sb_cache_tick(SbCache *c, double now_s, void (*read)(int *, int *, int *, int *, int *)) {
    if (!c || !read) return 0;
    if (c->time[0] && now_s < c->next_s && now_s >= c->next_s - 2.0) return 0;
    int pct = 100, chg = 0, h = 0, m = 0, h24 = 0;
    read(&pct, &chg, &h, &m, &h24);
    c->pct = sb_clamp_pct(pct);
    c->charging = chg ? 1 : 0;
    c->hour = h;
    c->minute = m;
    c->h24 = h24 ? 1 : 0;
    sb_format_time(h, m, c->h24, c->time, sizeof c->time);
    snprintf(c->pct_text, sizeof c->pct_text, "%d%%", c->pct);
    c->next_s = now_s + 1.0;
    return 1;
}
