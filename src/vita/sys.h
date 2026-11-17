/* Vita system services: network, clocks, power, battery, Wi-Fi, system settings. */
#ifndef VITAVOX_SYS_H
#define VITAVOX_SYS_H
#include <stdbool.h>

int sys_init(void);          /* call first */
void sys_exit(void);
/* Stop the power-tick thread and start a watchdog that calls
 * sceKernelExitProcess if shutdown exceeds timeout_ms. */
void sys_shutdown_begin(int timeout_ms);
/* once per frame: keeps the console awake while busy (playing, downloading),
 * suspends after a long idle, follows the app moving to and from the background */
void sys_tick(bool busy, bool input);
bool sys_enter_is_cross(void);   /* system setting: confirm with X (else O) */
int sys_battery(void);           /* percent */
bool sys_charging(void);
/* 00.38: local wall-clock time (system time zone); false if unavailable. */
bool sys_clock(int *hour, int *minute);
/* 00.38: system setting: 24-hour clock (else 12-hour). Re-read at most every 10 s. */
bool sys_time_24h(void);
bool sys_wifi_connected(void);
void sys_screen(bool on);        /* turn the display off/on (audio continues) */
bool sys_screen_on(void);
/* Sleep timer ended: display off now and stop keeping the console awake. */
void sys_sleep_display_off(void);
bool sys_quit_requested(void);   /* the system asked the app to close */
bool sys_visible(void);          /* display on and the app in front: worth drawing */
const char *sys_model(void);
int sys_language(void);

#endif
