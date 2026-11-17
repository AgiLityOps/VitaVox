/* User-managed series (series.json): a name, a sort mode and the user's own
 * (manual) order of book ids. Books are referenced by their stable id; a book
 * missing from the library is hidden but kept, so it reappears when it is
 * back. The manual order is kept while the alphabetical sort is shown. Pure C. */
#ifndef VITAVOX_SERIES_H
#define VITAVOX_SERIES_H
#include <stdbool.h>
#include <stdint.h>

#include "models.h"

#define SERIES_VER 1
#define SERIES_MAX 256
#define SERIES_BOOKS_MAX 4096
#define SERIES_NAME_MAX 96

typedef enum { SERIES_SORT_ALPHA = 0, SERIES_SORT_MANUAL = 1 } SeriesSort;

typedef struct {
    char name[SERIES_NAME_MAX];
    SeriesSort sort;
    bool list_view;      /* 00.39: books of this series as a list (persisted) */
    int n, cap;
    char (*ids)[40];       /* manual order (always kept) */
    uint64_t created;
} Series;

typedef struct {
    char path[320];
    Series *s;
    int n, cap;
} SeriesStore;

/* 0 loaded (or no file yet: empty store), 1 the file was corrupt: it is kept
 * as <path>.bad and the store starts empty, -1 bad args. */
int series_load(SeriesStore *st, const char *path);
/* Atomic write (path.tmp + rename). 0 ok. */
int series_save(const SeriesStore *st);
void series_free(SeriesStore *st);

/* New series named name (trimmed, non-empty). Returns its index, -1 invalid
 * name or full, -2 a series with that name exists (case-insensitive). */
int series_create(SeriesStore *st, const char *name, uint64_t now);
int series_rename(SeriesStore *st, int i, const char *name); /* 0, -1, -2 like create */
int series_delete(SeriesStore *st, int i);
int series_find(const SeriesStore *st, const char *name);
/* 0 added at the end of the manual order, 1 already there, -1 error. */
int series_add(SeriesStore *st, int i, const char *id);
int series_remove(SeriesStore *st, int i, const char *id);  /* 0 removed, 1 not there */
bool series_contains(const SeriesStore *st, int i, const char *id);
int series_set_sort(SeriesStore *st, int i, SeriesSort s);
int series_set_list(SeriesStore *st, int i, bool list);

/* Books of series i present in books[0..nbooks): library indices in display
 * order (manual, or the Library's title sort). out has room for st->s[i].n.
 * Returns the count. */
int series_visible(const SeriesStore *st, int i, const BookSummary *books, int nbooks, int *out);
/* Manual order: move the visible book id one visible place earlier (dir<0) or
 * later (dir>0); hidden (missing) books keep their slots. 0 moved, 1 at the edge, -1 error. */
int series_move(SeriesStore *st, int i, const char *id, int dir, const BookSummary *books, int nbooks);
/* Series indices ordered by name (natural, case-insensitive). out has room for st->n. */
int series_order(const SeriesStore *st, int *out);

#endif
