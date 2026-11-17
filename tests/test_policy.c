#include <stdlib.h>

#include "config.h"
#include "strutil.h"
#include "policy.h"
#include "test.h"

TEST(sync_interval_only_while_playing) {
    SyncPolicy p;
    sync_init(&p, 15);
    int fired = 0;
    for (int i = 0; i < 14; i++) fired += sync_tick(&p, 1.0, true);
    CHECK_EQ_INT(fired, 0);
    for (int i = 0; i < 100; i++) fired += sync_tick(&p, 1.0, false);
    CHECK_EQ_INT(fired, 0);
    CHECK(sync_tick(&p, 1.0, true));
    CHECK_NEAR(sync_take_listened(&p), 15, 1e-9);
    CHECK(!sync_tick(&p, 1.0, true));
    CHECK_NEAR(sync_take_listened(&p), 1, 1e-9);
    CHECK_NEAR(sync_take_listened(&p), 0, 1e-9);
    sync_init(&p, 0);
    CHECK_NEAR(p.interval, 15, 1e-9);
}
TEST(sleep_minutes_fade_and_stop) {
    SleepTimer s;
    sleep_set(&s, SLEEP_MINUTES, 1, 0);
    CHECK_EQ_INT(sleep_tick(&s, 50, 0), SLEEP_NONE);
    CHECK_NEAR(sleep_volume(&s, 0, 1), 1.0, 1e-6);
    CHECK_EQ_INT(sleep_tick(&s, 7.5, 0), SLEEP_FADING);
    CHECK_NEAR(sleep_volume(&s, 0, 1), 0.5, 1e-6);
    CHECK_EQ_INT(sleep_tick(&s, 3, 0), SLEEP_STOP);
    CHECK_EQ_INT(s.mode, SLEEP_OFF);
    CHECK_EQ_INT(sleep_tick(&s, 3, 0), SLEEP_NONE);
}
TEST(sleep_end_of_chapter) {
    SleepTimer s;
    sleep_set(&s, SLEEP_END_CHAPTER, 0, 600);
    CHECK_EQ_INT(sleep_tick(&s, 1, 500), SLEEP_NONE);
    CHECK_EQ_INT(sleep_tick(&s, 1, 597), SLEEP_FADING);
    CHECK_NEAR(sleep_volume(&s, 597.5, 1), 0.5, 1e-6);
    CHECK_NEAR(sleep_volume(&s, 595, 2), 0.5, 1e-6); /* 5 book s at 2x = 2.5 wall s */
    CHECK_EQ_INT(sleep_tick(&s, 1, 600), SLEEP_STOP);
    sleep_set(&s, SLEEP_OFF, 0, 0);
    CHECK_NEAR(sleep_volume(&s, 0, 1), 1, 1e-9);
}
TEST(config_roundtrip_and_clamp) {
    const char *p = "build-host/cfg_test.json";
    Config c, d;
    cfg_defaults(&c);
    CHECK_STR(c.library_path, CFG_DEFAULT_LIBRARY);
    CHECK_EQ_INT(c.skip_back, 15);
    CHECK_EQ_INT(c.skip_fwd, 30);
    CHECK(!c.list_view_library);
    CHECK(!c.list_view_search);
    strcpy(c.library_path, "ux0:music/books");
    c.speed = 1.5f;
    c.skip_fwd = 60;
    c.screen_off = false;
    c.list_view_library = true;
    c.list_view_search = false;
    CHECK_EQ_INT(cfg_save(&c, p), 0);
    CHECK_EQ_INT(cfg_load(&d, p), 0);
    CHECK_STR(d.library_path, "ux0:music/books");
    CHECK_NEAR(d.speed, 1.5, 1e-6);
    CHECK_EQ_INT(d.skip_fwd, 60);
    CHECK(!d.screen_off);
    CHECK(d.list_view_library);
    CHECK(!d.list_view_search);
    FILE *f = fopen(p, "wb"); fputs("{\"speed\":9,\"skipBack\":7,\"libraryPath\":\"\"}", f); fclose(f);
    CHECK_EQ_INT(cfg_load(&d, p), 0);
    CHECK_NEAR(d.speed, 2.0, 1e-6);
    CHECK_EQ_INT(d.skip_back, 15);
    CHECK_STR(d.library_path, CFG_DEFAULT_LIBRARY);
    CHECK_EQ_INT(cfg_load(&d, "build-host/none.json"), -1);
    CHECK_STR(d.library_path, CFG_DEFAULT_LIBRARY);
    CHECK(!d.list_view_library); CHECK(!d.list_view_search);
    CHECK_NEAR(cfg_next_speed(1.0f), 1.25, 1e-6);
    CHECK_NEAR(cfg_next_speed(2.0f), 0.75, 1e-6);
    CHECK_EQ_INT(cfg_next_skip(30), 60);
    CHECK_EQ_INT(cfg_next_skip(60), 10);
}


/* Logging out writes a shorter config over a longer one; it must still parse
 * (the emulator showed stale tails), and no temp file is left behind. */
TEST(config_shrinks_cleanly) {
    const char *p = "build-host/cfg_shrink.json";
    Config c, d;
    cfg_defaults(&c);
    memset(c.library_path, 'x', sizeof c.library_path - 1);
    c.library_path[sizeof c.library_path - 1] = 0;
    CHECK_EQ_INT(cfg_save(&c, p), 0);
    FILE *f = fopen("build-host/cfg_shrink.json.tmp", "wb");
    fputs("{\"stale\": \"temp file from a crash, longer than the next save\", \"pad\": \"....................\"}", f);
    fclose(f);
    str_copy(c.library_path, sizeof c.library_path, "ux0:audiobooks");
    CHECK_EQ_INT(cfg_save(&c, p), 0);
    CHECK_EQ_INT(cfg_load(&d, p), 0);
    CHECK_STR(d.library_path, "ux0:audiobooks");
    f = fopen("build-host/cfg_shrink.json.tmp", "rb");
    CHECK(f == NULL);
    if (f) fclose(f);
}



TEST(config_list_view_independent) {
    const char *p = "build-host/cfg_listview.json";
    Config c, d;
    cfg_defaults(&c);
    c.list_view_search = true;
    CHECK_EQ_INT(cfg_save(&c, p), 0);
    CHECK_EQ_INT(cfg_load(&d, p), 0);
    CHECK(!d.list_view_library);
    CHECK(d.list_view_search);
    d.list_view_library = true;
    d.list_view_search = false;
    CHECK_EQ_INT(cfg_save(&d, p), 0);
    CHECK_EQ_INT(cfg_load(&c, p), 0);
    CHECK(c.list_view_library);
    CHECK(!c.list_view_search);
    /* a file from before 00.39 (no list keys) stays on grid */
    FILE *f = fopen(p, "wb");
    fputs("{\"libraryPath\":\"ux0:audiobooks\",\"skipBack\":15,\"skipFwd\":30,\"speed\":1,\"screenOff\":true}", f);
    fclose(f);
    CHECK_EQ_INT(cfg_load(&d, p), 0);
    CHECK(!d.list_view_library);
    CHECK(!d.list_view_search);
}

void suite_policy(void) {
    RUN(config_shrinks_cleanly);
    RUN(sync_interval_only_while_playing);
    RUN(sleep_minutes_fade_and_stop);
    RUN(sleep_end_of_chapter);
    RUN(config_roundtrip_and_clamp);
    RUN(config_list_view_independent);
}
