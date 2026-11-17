/* Vita Vox entry point: Vita setup, the frame loop, shutdown. */
#include <psp2/kernel/processmgr.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <vita2d.h>

#include "app.h"
#include "views.h"
#include "wavrec.h"
#include "covers.h"
#include "dbg.h"
#include "engine.h"
#include "ime.h"
#include "input.h"
#include "libmodel.h"
#include "player.h"
#include "probecache.h"
#include "bookcache.h"
#include "progress.h"
#include "scanner.h"
#include "strutil.h"
#include "sys.h"
#include "vsink.h"
#include "worker.h"
#include "quit.h"
#include "stream.h"
#include "gfx.h"

#define DATA "ux0:data/VitaVox"

/* VitaSDK: a bigger heap for covers, rings and decoders */
int _newlib_heap_size_user = 192 * 1024 * 1024;

Config g_cfg;

const char *app_data_dir(void) { return DATA; }

void app_save_config(void) {
    cfg_save(&g_cfg, DATA "/config.json");
}

int main(void) {
    mkdir("ux0:data", 0777);
    mkdir(DATA, 0777);
    mkdir(DATA "/progress", 0777);
    mkdir(DATA "/covers", 0777);
    mkdir(DATA "/probe", 0777);
    dbg_init(DATA "/debug.log"); /* rotates an oversized (> 4 MB) log at launch */
    dbg_start_async();            /* audio threads never wait on the card for a log line */
    sys_init();
    dbg("start: Vita Vox %s", APP_VERSION);

    cfg_defaults(&g_cfg);
    bool first_run = cfg_load(&g_cfg, DATA "/config.json") != 0;
    /* 00.33: the configured library folder is never created here; a missing
     * folder shows "Can't open <path>" with an empty library (never another
     * folder's books). Only a first install makes the default ux0:audiobooks. */
    if (first_run) mkdir(CFG_DEFAULT_LIBRARY, 0777);
    app_save_config();

    g_api = worker_create("api", 256);
    g_sync = worker_create("sync", 256);
    g_covers = worker_create_prio("covers", 256, -2); /* lowest: never competes with audio */
    g_refine = worker_create_prio("refine", 256, -1); /* below the UI; probes are I/O-bound */
    g_scan = worker_create_prio("scan", 256, -1); /* 00.37: folder listing off g_api */
    if (engine_init(&vita_sink) != 0) dbg("engine_init failed");
    progress_init(DATA "/progress");
    scanner_set_cover_dir(DATA "/covers");
    probecache_init(DATA "/probe");
    bookcache_init(DATA "/books");
    lib_init();
    lib_set_root(g_cfg.library_path);
    lib_set_data_dir(DATA); /* catalogs: DATA/libraries/<folder>.json */
    player_init(&g_cfg);

    vita2d_init_advanced_with_msaa(4 * 1024 * 1024, SCE_GXM_MULTISAMPLE_4X);
    vita2d_set_clear_color(C_BG);
    vita2d_set_vblank_wait(1);
    input_init(sys_enter_is_cross());
    if (gfx_init("app0:assets") != 0) dbg("fonts missing");
    covers_init(DATA "/covers");
    app_init();
    series_ui_init(DATA "/series.json"); /* after app_init: may show a toast */
    wavrec_set_dir(DATA "/debug"); /* Settings > Debug: record output */

    uint64_t last = sceKernelGetProcessTimeWide();
    while (!sys_quit_requested()) {
        uint64_t now = sceKernelGetProcessTimeWide();
        float dt = (now - last) / 1e6f;
        last = now;
        if (dt > 0.25f) dt = 0.25f; /* after suspend or a long frame */
        Input in;
        input_poll(&in);
        if (!sys_screen_on() && (in.any || in.touching)) {
            /* Display was turned off (Select or the sleep timer): the first press
             * only wakes it, from any screen, and is not acted on. */
            sys_screen(true);
            dbg("display: on (button/touch)");
            memset(&in, 0, sizeof in);
        }
        if (ime_active()) {
            Input none;
            memset(&none, 0, sizeof none);
            app_update(&none, dt); /* views poll the keyboard themselves */
        } else {
            app_update(&in, dt);
        }
        if (player_take_sleep_ended()) {
            /* Sleep timer ended: playback is paused and saved; turn the display off.
             * Not playing any more, so the power tick stops (busy=false) and the
             * system's normal display/standby timers apply again. */
            sys_sleep_display_off();
        }
        bool busy = player_playing(); /* stay awake while listening */
        if (!sys_visible()) {
            /* nothing to see: skip drawing and let the CPU idle */
            sys_tick(busy, in.any || in.touching);
            sceKernelDelayThread(50 * 1000);
            continue;
        }
        vita2d_start_drawing();
        vita2d_clear_screen();
        app_draw();
        vita2d_end_drawing();
        vita2d_common_dialog_update();
        vita2d_swap_buffers();
        sys_tick(busy, in.any || in.touching);
    }

    dbg("quit requested");
    app_quit_begin();
    shutdown_log("cancel probes and workers");
    stream_request_cancel(1);
    scanner_cancel(1);
    sys_shutdown_begin(4000);

    shutdown_log("player_exit");
    player_exit();
    shutdown_log("engine_exit (join decode/audio)");
    engine_exit();
    wavrec_stop(); /* finish a debug recording so its WAV header is valid */
    wavrec_wait(1000);

    shutdown_log("join workers");
    worker_destroy_timeout(g_api, 1500);
    g_api = NULL;
    worker_destroy_timeout(g_sync, 1500);
    g_sync = NULL;
    worker_destroy_timeout(g_refine, 1000);
    g_refine = NULL;
    worker_destroy_timeout(g_scan, 1000);
    g_scan = NULL;
    /* A cover decode cannot be interrupted; do not wait on it at exit. */
    if (worker_pending(g_covers) > 0) worker_abandon(g_covers);
    else worker_destroy_timeout(g_covers, 1500);
    g_covers = NULL;

    shutdown_log("covers_exit");
    covers_exit();
    shutdown_log("gfx_exit");
    gfx_exit();
    shutdown_log("vita2d_fini");
    vita2d_fini();
    shutdown_log("sys_exit");
    sys_exit();
    shutdown_log("sceKernelExitProcess(0)");
    sceKernelExitProcess(0);
    return 0;
}
