/* 00.32 debug log rotation (dbg.c): 4 MB cap, two files at most, startup rotate. */
#include "test.h"
#include "dbg.h"
#include "osal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define LDIR "build-host/tmp-dbg"

static long fsize(const char *p) { struct stat st; return stat(p, &st) == 0 ? (long)st.st_size : -1; }

static int count_files(void) {
    int n = 0;
    const char *names[] = { LDIR "/debug.log", LDIR "/debug.old.log", LDIR "/debug.log.old" };
    for (int i = 0; i < 3; i++) if (fsize(names[i]) >= 0) n++;
    return n;
}

static int file_has(const char *p, const char *needle) {
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    static char buf[1 << 16];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    return strstr(buf, needle) != NULL;
}

TEST(dbg_old_path_names) {
    char o[128];
    dbg_old_path("ux0:data/VitaVox/debug.log", o, sizeof o);
    CHECK_STR(o, "ux0:data/VitaVox/debug.old.log");
    dbg_old_path("x/trace.txt", o, sizeof o);
    CHECK_STR(o, "x/trace.txt.old");
}

TEST(dbg_rotates_at_limit_two_files_max) {
    CHECK_EQ_INT(system("rm -rf " LDIR " && mkdir -p " LDIR), 0);
    dbg_set_limit(4096);
    int r0 = dbg_rotations();
    dbg_init(LDIR "/debug.log");
    char pad[200];
    memset(pad, 'x', sizeof pad - 1);
    pad[sizeof pad - 1] = 0;
    for (int i = 0; i < 18; i++) dbg("line %02d %s", i, pad); /* ~3.8 KB: under the cap */
    CHECK_EQ_INT(dbg_rotations() - r0, 0);
    CHECK(fsize(LDIR "/debug.log") <= 4096);
    CHECK_EQ_INT(count_files(), 1);
    for (int i = 18; i < 200; i++) dbg("line %03d %s", i, pad);
    CHECK(dbg_rotations() - r0 >= 5);
    CHECK_EQ_INT(count_files(), 2);                     /* never a third file */
    CHECK(fsize(LDIR "/debug.log") <= 4096);
    CHECK(fsize(LDIR "/debug.old.log") <= 4096);
    CHECK(fsize(LDIR "/debug.old.log") > 3500);         /* the old one was full */
    CHECK(file_has(LDIR "/debug.log", "log: rotated (size limit)"));
    CHECK(file_has(LDIR "/debug.log", "debug.old.log"));
    CHECK(file_has(LDIR "/debug.log", "line 199"));     /* the newest line is in the new file */
    dbg_set_limit(0);
    dbg_init(NULL);
}

TEST(dbg_startup_rotate_and_legacy_cleanup) {
    CHECK_EQ_INT(system("rm -rf " LDIR " && mkdir -p " LDIR), 0);
    /* An 8 KB log from an older version, plus its old-style debug.log.old. */
    FILE *f = fopen(LDIR "/debug.log", "wb");
    for (int i = 0; i < 8192; i++) fputc(i % 64 == 63 ? '\n' : 'a', f);
    fclose(f);
    f = fopen(LDIR "/debug.log.old", "wb");
    fputs("legacy\n", f);
    fclose(f);
    dbg_set_limit(4096);
    int r0 = dbg_rotations();
    dbg_init(LDIR "/debug.log");
    CHECK_EQ_INT(dbg_rotations() - r0, 1);
    CHECK_EQ_INT((int)fsize(LDIR "/debug.old.log"), 8192);
    CHECK(fsize(LDIR "/debug.log.old") < 0);            /* legacy third file removed */
    CHECK(file_has(LDIR "/debug.log", "over the limit at startup"));
    dbg("start: test");
    CHECK(file_has(LDIR "/debug.log", "start: test"));
    CHECK_EQ_INT(count_files(), 2);
    /* Under the cap at startup: appended, no rotation. */
    dbg_init(LDIR "/debug.log");
    CHECK_EQ_INT(dbg_rotations() - r0, 1);
    dbg("second start");
    CHECK(file_has(LDIR "/debug.log", "start: test"));
    CHECK(file_has(LDIR "/debug.log", "second start"));
    dbg_set_limit(0);
    dbg_init(NULL);
}

/* Async: dbg() returns without touching the file; the writer thread writes and
 * rotates, keeping order and the two-file cap. */
TEST(dbg_async_writer) {
    CHECK_EQ_INT(system("rm -rf " LDIR " && mkdir -p " LDIR), 0);
    dbg_set_limit(8192);
    dbg_init(LDIR "/debug.log");
    dbg_start_async();
    uint64_t t0 = now_ms();
    for (int i = 0; i < 1000; i++) dbg("async %04d ................................................", i);
    uint64_t dt = now_ms() - t0;
    CHECK(dt < 500);
    dbg_stop_async();
    CHECK_EQ_INT(count_files(), 2);
    CHECK(file_has(LDIR "/debug.log", "async 0999"));
    CHECK(fsize(LDIR "/debug.log") <= 8192);
    CHECK_EQ_INT((int)dbg_dropped(), 0);
    dbg_set_limit(0);
    dbg_init(NULL);
}

void suite_dbg(void) {
    RUN(dbg_old_path_names);
    RUN(dbg_rotates_at_limit_two_files_max);
    RUN(dbg_startup_rotate_and_legacy_cleanup);
    RUN(dbg_async_writer);
}
