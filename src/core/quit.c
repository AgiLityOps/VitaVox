#include "quit.h"

#include "dbg.h"
#include "osal.h"

static volatile int g_quit;
static uint64_t g_t0;

void app_quit_begin(void) {
    if (g_quit) return;
    g_t0 = now_ms();
    g_quit = 1;
    dbg("shutdown: begin");
}

int app_quit_active(void) { return g_quit; }

void shutdown_log(const char *step) {
    uint64_t dt = g_t0 ? now_ms() - g_t0 : 0;
    dbg("shutdown: %s +%llu ms", step ? step : "?", (unsigned long long)dt);
}
