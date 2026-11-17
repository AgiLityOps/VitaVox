/* Process-wide quit flag. Any long-running work (scan, probe, open, workers)
 * must check app_quit_active() and abort. Set as soon as LiveArea close /
 * 0x20000001 arrives, not only after the UI loop notices. */
#ifndef VITAVOX_QUIT_H
#define VITAVOX_QUIT_H

void app_quit_begin(void);
int app_quit_active(void);

/* Timed shutdown log: "shutdown: <step> +N ms". t0 is captured on first call
 * after app_quit_begin, or pass 0 to use that. */
void shutdown_log(const char *step);

#endif
