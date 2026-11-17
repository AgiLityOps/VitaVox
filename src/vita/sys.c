#include "sys.h"

#include <psp2/appmgr.h>
#include <psp2/apputil.h>
#include <psp2/common_dialog.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#include <psp2/rtc.h>

#include "stream.h"
#include <psp2/sysmodule.h>
#include <stdlib.h>
#include <string.h>

#include "dbg.h"
#include "quit.h"

static bool g_cross = true, g_screen = true;
static int g_lang = SCE_SYSTEM_PARAM_LANG_ENGLISH_US;
static volatile bool g_awake, g_fg = true;
static volatile unsigned g_last_active; /* seconds; 32 bits so the tick thread never reads it torn */
static volatile int g_tick_quit;
static SceUID g_tick_th = -1;

/* ATTRIBUTE 0x80000 turns off system suspend, so standby when idle ourselves */
#define IDLE_SUSPEND_S (10 * 60)

static unsigned now_s(void) { return (unsigned)(sceKernelGetProcessTimeWide() / 1000000); }

static int power_cb(int id, int count, int info, void *common) {
    (void)id; (void)count; (void)common;
    dbg("power event %08x", (unsigned)info);
    stream_note_power_event(); /* device I/O spanning this is not a stall */
    return 0;
}

/* Ticks from its own thread: the frame loop may stall while the display is off. */
static int tick_thread(SceSize args, void *argp) {
    (void)args; (void)argp;
    SceUID cb = sceKernelCreateCallback("power_cb", 0, power_cb, NULL);
    if (cb >= 0) scePowerRegisterCallback(cb);
    for (;;) {
        if (g_tick_quit) break;
        if (g_awake) {
            sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
        } else if (now_s() - g_last_active > IDLE_SUSPEND_S) {
            dbg("idle: suspend");
            g_last_active = now_s();
            scePowerRequestSuspend();
        }
        /* 200 ms so shutdown can join this thread quickly. */
        sceKernelDelayThreadCB(200 * 1000);
    }
    if (cb >= 0) scePowerUnregisterCallback(cb);
    return 0;
}

int sys_init(void) {
    /* headroom for decoding and drawing */
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);

    SceAppUtilInitParam init;
    SceAppUtilBootParam boot;
    memset(&init, 0, sizeof init);
    memset(&boot, 0, sizeof boot);
    sceAppUtilInit(&init, &boot);
    int v = 0;
    if (sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_ENTER_BUTTON, &v) == 0) g_cross = v == SCE_SYSTEM_PARAM_ENTER_BUTTON_CROSS;
    if (sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_LANG, &v) == 0) g_lang = v;
    SceCommonDialogConfigParam cfg;
    sceCommonDialogConfigParamInit(&cfg);
    cfg.language = g_lang;
    cfg.enterButtonAssign = g_cross ? SCE_SYSTEM_PARAM_ENTER_BUTTON_CROSS : SCE_SYSTEM_PARAM_ENTER_BUTTON_CIRCLE;
    sceCommonDialogSetConfigParam(&cfg);

    /* the BGM port keeps our audio running with the display off (as ElevenMPV does) */
    int r = sceAppMgrAcquireBgmPort();
    if (r < 0) dbg("sceAppMgrAcquireBgmPort %08x", (unsigned)r);
    g_last_active = now_s();
    g_tick_th = sceKernelCreateThread("power_tick", tick_thread, 0x10000100, 0x4000, 0, 0, NULL);
    if (g_tick_th >= 0) sceKernelStartThread(g_tick_th, 0, NULL);
    return 0;
}

static int shutdown_watchdog(SceSize args, void *argp) {
    (void)args;
    int ms = argp ? *(int *)argp : 4000;
    sceKernelDelayThread(ms * 1000);
    dbg("shutdown: watchdog fired after %d ms — ExitProcess", ms);
    sceKernelExitProcess(0);
    return 0;
}

void sys_shutdown_begin(int timeout_ms) {
    g_tick_quit = 1;
    if (timeout_ms < 500) timeout_ms = 500;
    static int wd_ms;
    wd_ms = timeout_ms;
    SceUID th = sceKernelCreateThread("shutdown_wd", shutdown_watchdog, 0x10000100, 0x2000, 0, 0, NULL);
    if (th >= 0) sceKernelStartThread(th, sizeof wd_ms, &wd_ms);
    shutdown_log("power tick stop requested");
}

void sys_exit(void) {
    g_tick_quit = 1;
    if (g_tick_th >= 0) {
        SceUInt to = 1000 * 1000;
        sceKernelWaitThreadEnd(g_tick_th, NULL, &to);
        g_tick_th = -1;
    }
    shutdown_log("release BGM port");
    sceAppMgrReleaseBgmPort();
    shutdown_log("sceAppUtilShutdown");
    sceAppUtilShutdown();
}

/* SceAppMgr's application event queue (missing from the SDK in our image) */
typedef struct { int event; SceUID app_id; char param[56]; } AppMgrEvent;
int sceAppMgrReceiveEventNum(int *num);
int sceAppMgrReceiveEvent(AppMgrEvent *ev);

static volatile bool g_quit;

/* ON_ACTIVATE, ON_DEACTIVATE and ON_RESUME arrive on both queues; older headers lack the names */
static void on_event(int e) {
    /* closing from LiveArea: with ATTRIBUTE 0x8 the system waits for us to go,
     * and ignoring it leaves the shell hung */
    if (e == 0x20000001) {
        g_quit = true;
        app_quit_begin();
    }
    if (e == 0x10000002) g_fg = false;
    if (e == 0x10000001 || e == 0x10000003) {
        g_fg = true;
        g_screen = true;
    }
}

static void poll_events(void) {
    /* leaving for LiveArea or another app, and the PS button or a wake from
     * standby turning the display back on behind our back */
    int n = 0;
    if (sceAppMgrReceiveEventNum(&n) == 0) {
        while (n-- > 0) {
            AppMgrEvent ev;
            if (sceAppMgrReceiveEvent(&ev) < 0) break;
            dbg("appmgr event %08x", (unsigned)ev.event);
            on_event(ev.event);
        }
    }
    SceAppMgrAppState st;
    memset(&st, 0, sizeof st);
    if (_sceAppMgrGetAppState(&st, sizeof st, 0x03570011) < 0) return;
    while (st.systemEventNum-- > 0) {
        SceAppMgrSystemEvent ev;
        if (sceAppMgrReceiveSystemEvent(&ev) < 0) break;
        dbg("system event %08x", (unsigned)ev.systemEvent);
        on_event(ev.systemEvent);
    }
}

void sys_tick(bool busy, bool input) {
    g_awake = busy;
    if (busy || input) g_last_active = now_s();
    /* events are IPC to the shell: ten polls a second is plenty */
    static SceUInt64 next;
    SceUInt64 now = sceKernelGetProcessTimeWide();
    if (now < next) return;
    next = now + 100 * 1000;
    poll_events();
}

bool sys_enter_is_cross(void) { return g_cross; }
int sys_battery(void) { int p = scePowerGetBatteryLifePercent(); return p < 0 ? 100 : p; }
bool sys_charging(void) { return scePowerIsBatteryCharging() > 0; }

bool sys_clock(int *hour, int *minute) {
    SceDateTime t;
    memset(&t, 0, sizeof t);
    if (sceRtcGetCurrentClockLocalTime(&t) < 0) return false;
    if (hour) *hour = t.hour;
    if (minute) *minute = t.minute;
    return true;
}

bool sys_time_24h(void) {
    static int fmt = -1;
    static unsigned next;
    unsigned now = now_s();
    if (fmt < 0 || now >= next) {
        int v = 0;
        if (sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_TIME_FORMAT, &v) == 0) {
            if (fmt != v) dbg("sys: clock format %s", v == SCE_SYSTEM_PARAM_TIME_FORMAT_24HR ? "24-hour" : "12-hour");
            fmt = v;
        } else if (fmt < 0) {
            fmt = SCE_SYSTEM_PARAM_TIME_FORMAT_12HR;
        }
        next = now + 10;
    }
    return fmt == SCE_SYSTEM_PARAM_TIME_FORMAT_24HR;
}

bool sys_wifi_connected(void) { return false; }

void sys_screen(bool on) {
    if (on == g_screen) return;
    g_screen = on;
    if (on) scePowerRequestDisplayOn();
    else scePowerRequestDisplayOff();
}

void sys_sleep_display_off(void) {
    g_awake = false;           /* no more DISABLE_AUTO_SUSPEND ticks from the tick thread */
    g_last_active = now_s();   /* idle standby countdown starts now */
    g_screen = true;           /* force the request even if we think it is already off */
    int r = scePowerRequestDisplayOff();
    g_screen = false;
    dbg("sleep timer: scePowerRequestDisplayOff rc=%08x, power tick stopped (paused)", (unsigned)r);
}

bool sys_screen_on(void) { return g_screen; }
bool sys_quit_requested(void) { return g_quit; }
bool sys_visible(void) { return g_screen && g_fg; }
const char *sys_model(void) { return "PS Vita"; }
int sys_language(void) { return g_lang; }
