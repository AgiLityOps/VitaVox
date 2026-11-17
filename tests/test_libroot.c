/* 00.33: library folder change. Path normalisation, folder checks, one catalog
 * per folder, legacy migration, the change sequence (refuse / stop playback
 * first), switching back restoring books/progress/series, the progress length
 * check, and the two-glyph hint layout. */
#include "test.h"
#include "hintlayout.h"
#include "libcache.h"
#include "libroot.h"
#include "progress.h"
#include "series.h"
#include "strutil.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define LR "build-host/tmp-libroot"

static bool exists(const char *p) { struct stat st; return stat(p, &st) == 0; }

static void setup_dirs(void) {
    if (system("rm -rf " LR " && mkdir -p " LR "/ux0/books_a " LR "/ux0/books_b " LR "/imc0/x " LR "/data/libraries " LR "/data/progress") != 0) { /* checked below */ }
    libroot_set_host_prefix(LR);
}

TEST(libroot_normalize_paths) {
    char o[320];
    CHECK_EQ_INT(libroot_normalize("ux0:audiobooks", o, sizeof o), LR_OK); CHECK_STR(o, "ux0:audiobooks");
    CHECK_EQ_INT(libroot_normalize("  UX0:/audiobooks/ ", o, sizeof o), LR_OK); CHECK_STR(o, "ux0:audiobooks");
    CHECK_EQ_INT(libroot_normalize("ux0://Audio Books//Fantasy\\", o, sizeof o), LR_OK); CHECK_STR(o, "ux0:Audio Books/Fantasy");
    CHECK_EQ_INT(libroot_normalize("uma0:\\books\\x", o, sizeof o), LR_OK); CHECK_STR(o, "uma0:books/x");
    CHECK_EQ_INT(libroot_normalize("imc0:", o, sizeof o), LR_OK); CHECK_STR(o, "imc0:");
    CHECK_EQ_INT(libroot_normalize("ur0:/", o, sizeof o), LR_OK); CHECK_STR(o, "ur0:");
    CHECK_EQ_INT(libroot_normalize("grw0:a", o, sizeof o), LR_OK);
    CHECK_EQ_INT(libroot_normalize("xmc0:a", o, sizeof o), LR_OK);
    CHECK_EQ_INT(libroot_normalize("   ", o, sizeof o), LR_EMPTY);
    CHECK_EQ_INT(libroot_normalize("", o, sizeof o), LR_EMPTY);
    CHECK_EQ_INT(libroot_normalize(NULL, o, sizeof o), LR_EMPTY);
    CHECK_EQ_INT(libroot_normalize("audiobooks", o, sizeof o), LR_NO_DRIVE);
    CHECK_EQ_INT(libroot_normalize("ux1:audiobooks", o, sizeof o), LR_UNKNOWN_DRIVE);
    CHECK_EQ_INT(libroot_normalize("os0:kd", o, sizeof o), LR_UNKNOWN_DRIVE);
    CHECK_EQ_INT(libroot_normalize(":x", o, sizeof o), LR_UNKNOWN_DRIVE);
    CHECK(libroot_known_drive("uma0"));
    CHECK(!libroot_known_drive("vs0"));
}

TEST(libroot_check_folders) {
    setup_dirs();
    CHECK_EQ_INT(libroot_check("ux0:books_a"), LR_OK);
    CHECK_EQ_INT(libroot_check("ux0:/books_a/"), LR_OK);
    CHECK_EQ_INT(libroot_check("ux0:"), LR_OK);
    CHECK_EQ_INT(libroot_check("ux0:missing"), LR_NOT_FOUND);
    CHECK_EQ_INT(libroot_check("uma0:books"), LR_DRIVE_MISSING);  /* no uma0 on this "Vita" */
    CHECK_EQ_INT(libroot_check("imc0:x"), LR_OK);
    CHECK_EQ_INT(libroot_check("foo0:x"), LR_UNKNOWN_DRIVE);
    char m[200];
    libroot_message(LR_NOT_FOUND, "ux0:missing", m, sizeof m);
    CHECK_STR(m, "Folder not found: ux0:missing - library folder unchanged");
    libroot_message(LR_DRIVE_MISSING, "uma0:books", m, sizeof m);
    CHECK_STR(m, "Drive uma0: is not available - library folder unchanged");
    libroot_message(LR_UNKNOWN_DRIVE, "foo0:x", m, sizeof m);
    CHECK(strstr(m, "Unknown drive in foo0:x") && strstr(m, "library folder unchanged"));
}

static void make_cat(LibraryCache *c, const char *root, int n, const char *const *ids, double dur) {
    memset(c, 0, sizeof *c);
    c->version = LIBCACHE_VERSION;
    str_copy(c->root, sizeof c->root, root);
    c->n = n;
    c->books = n ? calloc((size_t)n, sizeof(CachedBook)) : NULL;
    for (int i = 0; i < n; i++) {
        str_copy(c->books[i].sum.id, sizeof c->books[i].sum.id, ids[i]);
        snprintf(c->books[i].sum.title, sizeof c->books[i].sum.title, "Book %d", i);
        snprintf(c->books[i].sum.path, sizeof c->books[i].sum.path, "%s/b%d", root, i);
        c->books[i].sum.duration = dur;
    }
}

TEST(libroot_catalog_per_folder) {
    setup_dirs();
    char a[400], b[400], a2[400];
    libroot_catalog_path(LR "/data/libraries", "ux0:books_a", a, sizeof a);
    libroot_catalog_path(LR "/data/libraries", "ux0:books_b", b, sizeof b);
    libroot_catalog_path(LR "/data/libraries", "ux0:books_a", a2, sizeof a2);
    CHECK(strcmp(a, b) != 0);
    CHECK_STR(a, a2);                                   /* stable */
    CHECK(strstr(a, "/libraries/ux0_books_a-") != NULL);  /* readable name + hash */
    const char *ids[] = {"aaaa000000000001", "aaaa000000000002"};
    LibraryCache c, l;
    make_cat(&c, "ux0:books_a", 2, ids, 3600);
    CHECK_EQ_INT(libcache_save(a, &c), 0);
    libcache_free(&c);
    /* an empty folder is saved as empty, and loads as empty (not as the other folder) */
    make_cat(&c, "ux0:books_b", 0, NULL, 0);
    CHECK_EQ_INT(libcache_save(b, &c), 0);
    libcache_free(&c);
    CHECK_EQ_INT(libcache_load(b, &l), 0);
    CHECK_EQ_INT(l.n, 0);
    CHECK_STR(l.root, "ux0:books_b");
    libcache_free(&l);
    CHECK_EQ_INT(libcache_load(a, &l), 0);
    CHECK_EQ_INT(l.n, 2);
    libcache_free(&l);
}

TEST(libroot_migrate_legacy_catalog) {
    setup_dirs();
    const char *ids[] = {"bbbb000000000001"};
    LibraryCache c, l;
    make_cat(&c, "ux0:/books_a/", 1, ids, 100);   /* old file: root as typed */
    CHECK_EQ_INT(libcache_save(LR "/data/library.json", &c), 0);
    libcache_free(&c);
    CHECK_EQ_INT(libroot_migrate_legacy(LR "/data/library.json", LR "/data/libraries", "ux0:books_b"), 1);
    CHECK(!exists(LR "/data/library.json"));
    char a[400];
    libroot_catalog_path(LR "/data/libraries", "ux0:books_a", a, sizeof a); /* its own folder, not the fallback */
    CHECK_EQ_INT(libcache_load(a, &l), 0);
    CHECK_EQ_INT(l.n, 1);
    libcache_free(&l);
    /* nothing to do the second time */
    CHECK_EQ_INT(libroot_migrate_legacy(LR "/data/library.json", LR "/data/libraries", "ux0:books_b"), 0);
    /* an old file for a folder that already has a catalog is dropped, the catalog kept */
    make_cat(&c, "ux0:books_a", 0, NULL, 0);
    CHECK_EQ_INT(libcache_save(LR "/data/library.json", &c), 0);
    libcache_free(&c);
    CHECK_EQ_INT(libroot_migrate_legacy(LR "/data/library.json", LR "/data/libraries", "ux0:books_b"), 0);
    CHECK(!exists(LR "/data/library.json"));
    CHECK_EQ_INT(libcache_load(a, &l), 0);
    CHECK_EQ_INT(l.n, 1);
    libcache_free(&l);
    /* no root in the old file: the current folder */
    make_cat(&c, "", 0, NULL, 0);
    CHECK_EQ_INT(libcache_save(LR "/data/library.json", &c), 0);
    libcache_free(&c);
    CHECK_EQ_INT(libroot_migrate_legacy(LR "/data/library.json", LR "/data/libraries", "ux0:/books_b"), 1);
    char b[400];
    libroot_catalog_path(LR "/data/libraries", "ux0:books_b", b, sizeof b);
    CHECK(exists(b));
}

/* Fake app for the change sequence. */
typedef struct {
    char root[320];
    char log[256];
    bool playing;
    int refreshes;
} FakeApp;
static const char *f_root(void *ud) { return ((FakeApp *)ud)->root; }
static void f_stop(void *ud) { FakeApp *a = ud; strcat(a->log, "stop;"); a->playing = false; }
static void f_switch(void *ud, const char *n) {
    FakeApp *a = ud;
    strcat(a->log, a->playing ? "switch-while-playing;" : "switch;");
    str_copy(a->root, sizeof a->root, n);
}
static void f_refresh(void *ud) { FakeApp *a = ud; strcat(a->log, "refresh;"); a->refreshes++; }

TEST(libroot_change_sequence) {
    setup_dirs();
    FakeApp app;
    memset(&app, 0, sizeof app);
    str_copy(app.root, sizeof app.root, "ux0:books_a");
    app.playing = true;
    LibSwitchOps ops = {&app, f_root, f_stop, f_switch, f_refresh};
    char msg[200];
    /* missing folder: refused, nothing touched, old setting kept, playback untouched */
    CHECK_EQ_INT(libswitch_apply("ux0:/no_such_folder", &ops, msg, sizeof msg), LS_REFUSED);
    CHECK_STR(msg, "Folder not found: ux0:no_such_folder - library folder unchanged");
    CHECK_STR(app.root, "ux0:books_a");
    CHECK_STR(app.log, "");
    CHECK(app.playing);
    /* drive not available / unknown / empty: refused too */
    CHECK_EQ_INT(libswitch_apply("uma0:books", &ops, msg, sizeof msg), LS_REFUSED);
    CHECK(strstr(msg, "uma0: is not available") != NULL);
    CHECK_EQ_INT(libswitch_apply("books", &ops, msg, sizeof msg), LS_REFUSED);
    CHECK_EQ_INT(libswitch_apply("", &ops, msg, sizeof msg), LS_REFUSED);
    CHECK_STR(app.root, "ux0:books_a");
    CHECK_STR(app.log, "");
    /* same folder written differently: just a refresh, playback continues */
    CHECK_EQ_INT(libswitch_apply(" UX0:/books_a/ ", &ops, msg, sizeof msg), LS_SAME);
    CHECK_STR(app.log, "refresh;");
    CHECK(app.playing);
    /* a real change: playback stopped first, then the switch, then the rescan */
    app.log[0] = 0;
    CHECK_EQ_INT(libswitch_apply("ux0:/books_b", &ops, msg, sizeof msg), LS_CHANGED);
    CHECK_STR(app.log, "stop;switch;refresh;");
    CHECK_STR(app.root, "ux0:books_b");
    CHECK(!app.playing);
    CHECK_STR(msg, "Library folder changed to ux0:books_b");
}

TEST(libroot_switch_back_restores) {
    setup_dirs();
    progress_init(LR "/data/progress");
    char a[400], b[400];
    libroot_catalog_path(LR "/data/libraries", "ux0:books_a", a, sizeof a);
    libroot_catalog_path(LR "/data/libraries", "ux0:books_b", b, sizeof b);
    const char *ids[] = {"cccc000000000001", "cccc000000000002"};
    LibraryCache c, l;
    make_cat(&c, "ux0:books_a", 2, ids, 7200);
    CHECK_EQ_INT(libcache_save(a, &c), 0);
    libcache_free(&c);
    BookProgress p = {0};
    p.position = 1234.5;
    p.speed = 1.25f;
    p.duration = 7200;
    CHECK_EQ_INT(progress_save(ids[0], &p), 0);
    SeriesStore st;
    memset(&st, 0, sizeof st);
    CHECK_EQ_INT(series_load(&st, LR "/data/series.json"), 0);
    int si = series_create(&st, "Saga", 1);
    CHECK(si >= 0);
    series_add(&st, si, ids[0]);
    series_add(&st, si, ids[1]);
    CHECK_EQ_INT(series_save(&st), 0);
    /* switch to the empty folder: its catalog is empty, the series shows nothing */
    make_cat(&c, "ux0:books_b", 0, NULL, 0);
    CHECK_EQ_INT(libcache_save(b, &c), 0);
    libcache_free(&c);
    CHECK_EQ_INT(libcache_load(b, &l), 0);
    int out[8];
    CHECK_EQ_INT(series_visible(&st, si, NULL, 0, out), 0);
    libcache_free(&l);
    /* back to the first folder: books, progress and series all there */
    CHECK_EQ_INT(libcache_load(a, &l), 0);
    CHECK_EQ_INT(l.n, 2);
    BookSummary books[2] = {l.books[0].sum, l.books[1].sum};
    CHECK_EQ_INT(series_visible(&st, si, books, 2, out), 2);
    BookProgress q;
    CHECK_EQ_INT(progress_load_for(ids[0], l.books[0].sum.duration, &q), 0);
    CHECK_NEAR(q.position, 1234.5, 1e-6);
    CHECK_NEAR(q.speed, 1.25, 1e-6);
    libcache_free(&l);
    series_free(&st);
    SeriesStore st2;
    memset(&st2, 0, sizeof st2);
    CHECK_EQ_INT(series_load(&st2, LR "/data/series.json"), 0);
    CHECK(series_contains(&st2, 0, ids[1]));      /* never removed from disk */
    series_free(&st2);
}

TEST(progress_length_check) {
    setup_dirs();
    progress_init(LR "/data/progress");
    CHECK(progress_duration_matches(100, 100));
    CHECK(progress_duration_matches(100, 104.9));       /* within 5 s */
    CHECK(!progress_duration_matches(100, 106));
    CHECK(progress_duration_matches(36000, 36300));     /* within 1 % of 10 h */
    CHECK(!progress_duration_matches(36000, 36400));
    CHECK(progress_duration_matches(0, 500));           /* unknown saved length: accept */
    CHECK(progress_duration_matches(500, 0));           /* unknown book length: accept */
    BookProgress p = {0}, q;
    p.position = 50; p.duration = 3000; p.updated_ms = 7;
    CHECK_EQ_INT(progress_save("dddd000000000001", &p), 0);
    CHECK_EQ_INT(progress_load_for("dddd000000000001", 3002, &q), 0);
    CHECK_NEAR(q.position, 50, 1e-9);
    CHECK_NEAR(q.duration, 3000, 1e-9);
    /* same ID, different book (another folder): ignored, starts from 0, record kept */
    CHECK_EQ_INT(progress_load_for("dddd000000000001", 4000, &q), -2);
    CHECK_NEAR(q.position, 0, 1e-9);
    CHECK_EQ_INT(progress_load("dddd000000000001", &q), 0);
    CHECK_NEAR(q.position, 50, 1e-9);
    CHECK_EQ_INT(progress_load_for("eeee000000000001", 100, &q), -1); /* none */
    /* legacy record (no duration) is accepted; the next save adds the length */
    FILE *f = fopen(LR "/data/progress/ffff000000000001.json", "wb");
    CHECK(f != NULL);
    if (f) { fputs("{\"position\": 77, \"speed\": 1, \"finished\": false, \"updated\": 5}", f); fclose(f); }
    CHECK_EQ_INT(progress_load_for("ffff000000000001", 9999, &q), 0);
    CHECK_NEAR(q.position, 77, 1e-9);
    CHECK_NEAR(q.duration, 0, 1e-9);
    q.position = 80; q.duration = 9999;
    CHECK_EQ_INT(progress_save("ffff000000000001", &q), 0);
    CHECK_EQ_INT(progress_load("ffff000000000001", &q), 0);
    CHECK_NEAR(q.duration, 9999, 1e-9);
    CHECK_EQ_INT(progress_load_for("ffff000000000001", 5000, &q), -2);
}

/* Ink widths of Manrope 500 at 15 px and gfx_glyph_w at 24 px (measured with the bundled font). */
#define W_L 31.2f
#define W_SELECT 60.39f
#define W_ROUND 24.0f
#define BATTERY_END (20 + 45.22f) /* "100%+" */

static void check_bar(HintItem *h, int n) {
    float left = hint_layout(h, n, 960 - 20);
    CHECK(left > BATTERY_END + 40);                      /* clear of the battery text */
    for (int i = 0; i < n; i++) {
        float gl = h[i].glyph_cx[0] - h[i].glyph_w[0] / 2;
        float r = h[i].label_x + h[i].label_w;
        CHECK_NEAR(r - gl, hint_width(&h[i]), 1e-3);    /* the item spans exactly its width */
        if (h[i].nglyphs == 2) {
            float gap = (h[i].glyph_cx[1] - h[i].glyph_w[1] / 2) - (h[i].glyph_cx[0] + h[i].glyph_w[0] / 2);
            CHECK_NEAR(gap, HINT_GLYPH_GAP, 1e-4);
            CHECK(h[i].glyph_cx[1] + h[i].glyph_w[1] / 2 + HINT_LABEL_GAP <= h[i].label_x + 1e-3);
        }
        if (i + 1 < n) CHECK(r + HINT_ITEM_GAP <= h[i + 1].glyph_cx[0] - h[i + 1].glyph_w[0] / 2 + 1e-3);
    }
    CHECK_NEAR(h[n - 1].label_x + h[n - 1].label_w, 940, 1e-3);
}

TEST(hint_layout_two_glyphs_fit) {
    /* player, worst case: "Book details" focused, (L) (R) Skip */
    HintItem p[6] = {
        {84.17f, {W_ROUND, 0}, 1, 0, {0, 0}},   /* X Book details */
        {41.65f, {W_ROUND, 0}, 1, 0, {0, 0}},   /* [] Pause */
        {28.72f, {W_L, W_L}, 2, 0, {0, 0}},     /* (L) (R) Skip */
        {64.24f, {W_ROUND, 0}, 1, 0, {0, 0}},   /* /\ Chapters */
        {71.42f, {W_SELECT, 0}, 1, 0, {0, 0}},  /* SELECT Screen off */
        {33.05f, {W_ROUND, 0}, 1, 0, {0, 0}},   /* O Back */
    };
    check_bar(p, 6);
    float left = hint_layout(p, 6, 940);
    CHECK_NEAR(left, 236.0, 1.0);                  /* matches the approved preview */
    float left5 = hint_layout(p + 1, 5, 940);
    CHECK_NEAR(left5, 374.1, 1.0);
    /* chapters menu: D-pad Move, (L) (R) Page, X Play chapter, O Close */
    HintItem c[4] = {
        {36.19f, {W_ROUND, 0}, 1, 0, {0, 0}},
        {33.63f, {W_L, W_L}, 2, 0, {0, 0}},
        {85.81f, {W_ROUND, 0}, 1, 0, {0, 0}},
        {39.28f, {W_ROUND, 0}, 1, 0, {0, 0}},
    };
    check_bar(c, 4);
    /* one glyph behaves exactly like before (00.32) */
    HintItem one = {28.72f, {W_L, 0}, 1, 0, {0, 0}};
    float l1 = hint_layout(&one, 1, 940);
    CHECK_NEAR(one.label_x, 940 - 28.72f, 1e-3);
    CHECK_NEAR(one.glyph_cx[0], 940 - 28.72f - 8 - W_L / 2, 1e-3);
    CHECK_NEAR(l1, 940 - 28.72f - 8 - W_L, 1e-3);
}

void suite_libroot(void) {
    RUN(libroot_normalize_paths);
    RUN(libroot_check_folders);
    RUN(libroot_catalog_per_folder);
    RUN(libroot_migrate_legacy_catalog);
    RUN(libroot_change_sequence);
    RUN(libroot_switch_back_restores);
    RUN(progress_length_check);
    RUN(hint_layout_two_glyphs_fit);
    libroot_set_host_prefix(NULL);
}
