/* The library as the UI sees it: all books (loaded in the background), sorted
 * views, series and author groups, continue-listening and recently-added lists. */
#ifndef VITAVOX_LIBMODEL_H
#define VITAVOX_LIBMODEL_H
#include <stdbool.h>

#include "groups.h"
#include "library.h"
#include "models.h"
#include "scanner.h"

typedef enum { LIB_EMPTY, LIB_LOADING, LIB_READY, LIB_ERROR } LibState;

void lib_init(void);
/* App data folder: catalogs go to <dir>/libraries/, one per library folder (00.33). */
void lib_set_data_dir(const char *dir);
/* Shows the current folder's saved catalog instantly, or scans if it has none.
 * A missing folder gives an empty library with "Can't open <path>". */
void lib_startup(void);
/* Library folder changed (validated, playback already stopped): empties the
 * library, then shows the new folder's saved catalog if any. Call lib_refresh after. */
void lib_switch_root(const char *path);
/* Copy cached tracks/chapters for id (malloc'd). 0 ok, else fall back to a scan. */
int lib_copy_detail(const char *id, BookDetail *out);
void lib_set_root(const char *path);  /* normalised; also picks the folder's catalog file */
const char *lib_root(void);
const char *lib_cover_path(const char *id);
/* Any thread: a thumbnail made lazily for book id from cover source src. */
void lib_post_thumb(const char *id, const char *src, const char *thumb);
void lib_clear(void);                 /* on logout */
void lib_refresh(void);               /* Settings > Refresh only (full rescan) */
void lib_tick(void);                  /* adopt finished loads (UI thread) */
LibState lib_state(void);
bool lib_loading(void);               /* a refresh is running (data may already be shown) */
const char *lib_error(void);
void lib_progress(int *done, int *total);

int lib_count(void);
const BookSummary *lib_book(int i);
int lib_find_id(const char *id);
/* Indices of all books in this order. */
const int *lib_sorted(SortMode m, int *n);
const int *lib_continue(int *n);      /* in progress, most recent first */
const int *lib_recent(int *n);        /* recently added */
const Group *lib_groups(GroupKind k, int *n);
/* Matches for a query (title, author or series), in title order. */
int lib_search(const char *q, int *out, int max);
/* Local progress update after listening (keeps Continue current between refreshes). */
void lib_note_progress(const char *id, double t, double duration);
/* Stats from the most recently completed (or failed) scan. */
const ScannerStats *lib_scan_stats(void);

#endif
