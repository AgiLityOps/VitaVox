/* series.json store (series.c). */
#include "test.h"
#include "series.h"
#include "strutil.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define SER_DIR "build-host/tmp-series"
#define SER_PATH SER_DIR "/series.json"

static void book(BookSummary *b, const char *id, const char *title) {
    memset(b, 0, sizeof *b);
    str_copy(b->id, sizeof b->id, id);
    str_copy(b->title, sizeof b->title, title);
}

static int file_exists(const char *p) { struct stat st; return stat(p, &st) == 0; }

static void write_text(const char *p, const char *t) {
    FILE *f = fopen(p, "wb");
    if (f) { fputs(t, f); fclose(f); }
}

TEST(series_create_rename_delete) {
    CHECK_EQ_INT(system("rm -rf " SER_DIR " && mkdir -p " SER_DIR), 0);
    SeriesStore st;
    CHECK_EQ_INT(series_load(&st, SER_PATH), 0); /* no file yet */
    CHECK_EQ_INT(st.n, 0);
    CHECK_EQ_INT(series_create(&st, "  The Count of Monte Cristo  ", 1), 0);
    CHECK_STR(st.s[0].name, "The Count of Monte Cristo");
    CHECK_EQ_INT(series_create(&st, "the count of monte cristo", 2), -2); /* duplicate */
    CHECK_EQ_INT(series_create(&st, "   ", 2), -1);              /* empty */
    CHECK_EQ_INT(series_create(&st, "Sherlock Holmes", 3), 1);
    CHECK_EQ_INT(series_create(&st, "Arthur Conan Doyle", 4), 2);
    CHECK_EQ_INT(series_rename(&st, 1, "The Adventures of Sherlock Holmes"), 0);
    CHECK_STR(st.s[1].name, "The Adventures of Sherlock Holmes");
    CHECK_EQ_INT(series_rename(&st, 1, "arthur conan doyle"), -2);
    CHECK_EQ_INT(series_rename(&st, 1, ""), -1);
    CHECK_EQ_INT(series_rename(&st, 9, "x"), -1);
    CHECK_EQ_INT(series_find(&st, " the adventures of sherlock holmes "), 1);
    int ord[8];
    CHECK_EQ_INT(series_order(&st, ord), 3);
    CHECK_STR(st.s[ord[0]].name, "Arthur Conan Doyle");
    CHECK_STR(st.s[ord[2]].name, "The Count of Monte Cristo");
    CHECK_EQ_INT(series_delete(&st, 2), 0);
    CHECK_EQ_INT(st.n, 2);
    CHECK_EQ_INT(series_find(&st, "Arthur Conan Doyle"), -1);
    CHECK_EQ_INT(series_save(&st), 0);
    series_free(&st);
    SeriesStore b;
    CHECK_EQ_INT(series_load(&b, SER_PATH), 0);
    CHECK_EQ_INT(b.n, 2);
    CHECK_STR(b.s[0].name, "The Count of Monte Cristo");
    CHECK_STR(b.s[1].name, "The Adventures of Sherlock Holmes");
    series_free(&b);
}

TEST(series_books_sort_reorder_missing) {
    CHECK_EQ_INT(system("rm -rf " SER_DIR " && mkdir -p " SER_DIR), 0);
    BookSummary lib[5];
    book(&lib[0], "cmc09", "The Count of Monte Cristo 09 - The Arrival");
    book(&lib[1], "cmc07", "The Count of Monte Cristo 07 - The Prisoner");
    book(&lib[2], "cmc08", "The Count of Monte Cristo 08 - The Escape");
    book(&lib[3], "moby", "Moby-Dick");
    book(&lib[4], "cmc10", "The Count of Monte Cristo 10 - The Count");
    SeriesStore st;
    CHECK_EQ_INT(series_load(&st, SER_PATH), 0);
    int s = series_create(&st, "Monte Cristo", 1);
    CHECK_EQ_INT(series_add(&st, s, "cmc09"), 0);
    CHECK_EQ_INT(series_add(&st, s, "cmc07"), 0);
    CHECK_EQ_INT(series_add(&st, s, "cmc10"), 0);
    CHECK_EQ_INT(series_add(&st, s, "cmc08"), 0);
    CHECK_EQ_INT(series_add(&st, s, "cmc07"), 1); /* already there */
    CHECK(series_contains(&st, s, "cmc08"));
    CHECK(!series_contains(&st, s, "moby"));
    int out[8];
    /* Alphabetical (Library title sort) by default. */
    CHECK_EQ_INT(series_visible(&st, s, lib, 5, out), 4);
    CHECK_EQ_INT(out[0], 1); CHECK_EQ_INT(out[1], 2); CHECK_EQ_INT(out[2], 0); CHECK_EQ_INT(out[3], 4);
    /* Manual: insertion order, then reorder. */
    CHECK_EQ_INT(series_set_sort(&st, s, SERIES_SORT_MANUAL), 0);
    CHECK_EQ_INT(series_visible(&st, s, lib, 5, out), 4);
    CHECK_EQ_INT(out[0], 0); CHECK_EQ_INT(out[1], 1); CHECK_EQ_INT(out[2], 4); CHECK_EQ_INT(out[3], 2);
    CHECK_EQ_INT(series_move(&st, s, "cmc08", -1, lib, 5), 0); /* 09 07 08 10 */
    CHECK_EQ_INT(series_move(&st, s, "cmc09", -1, lib, 5), 1); /* already first */
    CHECK_EQ_INT(series_move(&st, s, "cmc09", +1, lib, 5), 0); /* 07 09 08 10 */
    series_visible(&st, s, lib, 5, out);
    CHECK_EQ_INT(out[0], 1); CHECK_EQ_INT(out[1], 0); CHECK_EQ_INT(out[2], 2); CHECK_EQ_INT(out[3], 4);
    /* Alphabetical view keeps the manual order underneath. */
    CHECK_EQ_INT(series_set_sort(&st, s, SERIES_SORT_ALPHA), 0);
    series_visible(&st, s, lib, 5, out);
    CHECK_EQ_INT(out[0], 1); CHECK_EQ_INT(out[2], 0);
    CHECK_EQ_INT(series_set_sort(&st, s, SERIES_SORT_MANUAL), 0);
    series_visible(&st, s, lib, 5, out);
    CHECK_EQ_INT(out[0], 1); CHECK_EQ_INT(out[1], 0); CHECK_EQ_INT(out[2], 2); CHECK_EQ_INT(out[3], 4);
    CHECK_EQ_INT(series_save(&st), 0);
    series_free(&st);

    /* Restart: sort mode + manual order persisted. */
    CHECK_EQ_INT(series_load(&st, SER_PATH), 0);
    CHECK_EQ_INT(st.s[0].sort, SERIES_SORT_MANUAL);
    CHECK_EQ_INT(st.s[0].n, 4);
    series_visible(&st, 0, lib, 5, out);
    CHECK_EQ_INT(out[0], 1); CHECK_EQ_INT(out[1], 0); CHECK_EQ_INT(out[2], 2); CHECK_EQ_INT(out[3], 4);

    /* Refresh without volume 09 on the card: hidden, kept; moves skip over it. */
    BookSummary lib2[4] = { lib[1], lib[2], lib[3], lib[4] };
    CHECK_EQ_INT(series_visible(&st, 0, lib2, 4, out), 3); /* 07 08 10 */
    CHECK_EQ_INT(out[0], 0); CHECK_EQ_INT(out[1], 1); CHECK_EQ_INT(out[2], 3);
    CHECK_EQ_INT(series_move(&st, 0, "cmc07", +1, lib2, 4), 0); /* swaps with 08, 09 stays in its slot */
    CHECK_EQ_INT(st.s[0].n, 4);
    CHECK_STR(st.s[0].ids[0], "cmc08");
    CHECK_STR(st.s[0].ids[1], "cmc09");
    CHECK_STR(st.s[0].ids[2], "cmc07");
    CHECK_EQ_INT(series_save(&st), 0);
    series_free(&st);
    /* File back: reappears in its place. */
    CHECK_EQ_INT(series_load(&st, SER_PATH), 0);
    CHECK_EQ_INT(series_visible(&st, 0, lib, 5, out), 4);
    CHECK_EQ_INT(out[0], 2); CHECK_EQ_INT(out[1], 0); CHECK_EQ_INT(out[2], 1); CHECK_EQ_INT(out[3], 4);

    /* Remove from the series (never touches anything else). */
    CHECK_EQ_INT(series_remove(&st, 0, "cmc09"), 0);
    CHECK_EQ_INT(series_remove(&st, 0, "cmc09"), 1);
    CHECK_EQ_INT(series_visible(&st, 0, lib, 5, out), 3);
    CHECK_EQ_INT(series_add(&st, 0, ""), -1);
    series_free(&st);
}

TEST(series_corrupt_and_interrupted_files) {
    CHECK_EQ_INT(system("rm -rf " SER_DIR " && mkdir -p " SER_DIR), 0);
    write_text(SER_PATH, "{\"version\":1,\"series\":[{\"name\":\"Sherlock Holmes\",\"books\":[\"a\",");
    SeriesStore st;
    CHECK_EQ_INT(series_load(&st, SER_PATH), 1);
    CHECK_EQ_INT(st.n, 0);
    CHECK(file_exists(SER_PATH ".bad"));
    CHECK(!file_exists(SER_PATH));
    /* Still usable and saves a fresh file. */
    CHECK_EQ_INT(series_create(&st, "Sherlock Holmes", 1), 0);
    CHECK_EQ_INT(series_save(&st), 0);
    series_free(&st);
    CHECK_EQ_INT(series_load(&st, SER_PATH), 0);
    CHECK_EQ_INT(st.n, 1);
    series_free(&st);

    /* Wrong shape / junk entries are skipped, duplicates dropped. */
    write_text(SER_PATH, "{\"series\":[{\"name\":\"A\",\"sort\":\"manual\",\"books\":[\"x\",\"x\",7,\"y\"]},"
                         "{\"name\":\"a\"},{\"nope\":1},{\"name\":\"  \"},{\"name\":\"B\",\"sort\":\"weird\"}]}");
    CHECK_EQ_INT(series_load(&st, SER_PATH), 0);
    CHECK_EQ_INT(st.n, 2);
    CHECK_EQ_INT(st.s[0].sort, SERIES_SORT_MANUAL);
    CHECK_EQ_INT(st.s[0].n, 2);
    CHECK_EQ_INT(st.s[1].sort, SERIES_SORT_ALPHA);
    series_free(&st);
    write_text(SER_PATH, "[1,2,3]");
    CHECK_EQ_INT(series_load(&st, SER_PATH), 1);
    series_free(&st);

    /* Save interrupted after the old file was removed: the temp file is used. */
    CHECK_EQ_INT(system("rm -f " SER_PATH), 0);
    write_text(SER_PATH ".tmp", "{\"version\":1,\"series\":[{\"name\":\"Recovered\",\"books\":[\"q\"]}]}");
    CHECK_EQ_INT(series_load(&st, SER_PATH), 0);
    CHECK_EQ_INT(st.n, 1);
    CHECK_STR(st.s[0].name, "Recovered");
    CHECK(file_exists(SER_PATH));
    series_free(&st);
}

TEST(series_alpha_sort_numbers) {
    SeriesStore st = {0};
    CHECK_EQ_INT(system("rm -rf " SER_DIR " && mkdir -p " SER_DIR), 0);
    CHECK_EQ_INT(series_load(&st, SER_PATH), 0);
    int s = series_create(&st, "The Count of Monte Cristo", 1);
    CHECK_EQ_INT(s, 0);
    BookSummary lib[4];
    book(&lib[0], "cmc/10", "The Count of Monte Cristo 10");
    book(&lib[1], "cmc/07", "The Count of Monte Cristo 07");
    book(&lib[2], "cmc/01", "The Count of Monte Cristo 1");
    book(&lib[3], "cmc/02", "The Count of Monte Cristo 2");
    /* manual order as added: 10, 07, 1, 2 */
    for (int i = 0; i < 4; i++) CHECK_EQ_INT(series_add(&st, s, lib[i].id), 0);
    int out[4];
    CHECK_EQ_INT(series_set_sort(&st, s, SERIES_SORT_ALPHA), 0);
    CHECK_EQ_INT(series_visible(&st, s, lib, 4, out), 4);
    CHECK_EQ_INT(out[0], 2); CHECK_EQ_INT(out[1], 3); CHECK_EQ_INT(out[2], 1); CHECK_EQ_INT(out[3], 0);
    CHECK_EQ_INT(series_set_sort(&st, s, SERIES_SORT_MANUAL), 0);
    CHECK_EQ_INT(series_visible(&st, s, lib, 4, out), 4);
    CHECK_EQ_INT(out[0], 0); CHECK_EQ_INT(out[1], 1); CHECK_EQ_INT(out[2], 2); CHECK_EQ_INT(out[3], 3);
    /* series list itself: "Vol 2" before "Vol 10" */
    int a = series_create(&st, "Vol 10", 2), b = series_create(&st, "vol 2", 3);
    CHECK(a > 0 && b > 0);
    int ord[3];
    CHECK_EQ_INT(series_order(&st, ord), 3);
    CHECK_EQ_INT(ord[0], s); CHECK_EQ_INT(ord[1], b); CHECK_EQ_INT(ord[2], a);
    series_free(&st);
}


TEST(series_list_view_persists_per_series) {
    CHECK_EQ_INT(system("rm -rf " SER_DIR " && mkdir -p " SER_DIR), 0);
    SeriesStore st;
    CHECK_EQ_INT(series_load(&st, SER_PATH), 0);
    int a = series_create(&st, "Sherlock Holmes", 1);
    int b = series_create(&st, "Monte Cristo", 2);
    CHECK(a == 0 && b == 1);
    CHECK(!st.s[a].list_view);
    CHECK(!st.s[b].list_view);
    CHECK_EQ_INT(series_set_list(&st, a, true), 0);
    CHECK(st.s[a].list_view);
    CHECK(!st.s[b].list_view);
    CHECK_EQ_INT(series_set_sort(&st, a, SERIES_SORT_MANUAL), 0);
    CHECK_EQ_INT(series_set_list(&st, 9, true), -1);
    CHECK_EQ_INT(series_save(&st), 0);
    series_free(&st);
    CHECK_EQ_INT(series_load(&st, SER_PATH), 0);
    CHECK_EQ_INT(st.n, 2);
    CHECK(st.s[0].list_view);
    CHECK(!st.s[1].list_view);
    CHECK_EQ_INT(st.s[0].sort, SERIES_SORT_MANUAL);
    CHECK_EQ_INT(series_set_list(&st, 0, false), 0);
    CHECK_EQ_INT(series_save(&st), 0);
    series_free(&st);
    CHECK_EQ_INT(series_load(&st, SER_PATH), 0);
    CHECK(!st.s[0].list_view);
    CHECK(!st.s[1].list_view);
    series_free(&st);
    /* a file from before 00.39 (no listView key) opens in the grid */
    write_text(SER_PATH, "{\"version\":1,\"series\":[{\"name\":\"Sherlock Holmes\",\"sort\":\"alpha\",\"created\":1,\"books\":[]}]}");
    CHECK_EQ_INT(series_load(&st, SER_PATH), 0);
    CHECK_EQ_INT(st.n, 1);
    CHECK(!st.s[0].list_view);
    series_free(&st);
}

void suite_series(void) {
    RUN(series_create_rename_delete);
    RUN(series_books_sort_reorder_missing);
    RUN(series_corrupt_and_interrupted_files);
    RUN(series_alpha_sort_numbers);
    RUN(series_list_view_persists_per_series);
}
