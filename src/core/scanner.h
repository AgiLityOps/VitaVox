/* Walk a local audiobook folder (default ux0:audiobooks/).
 * A subfolder is a book (audio files inside, naturally sorted).
 * A single audio file in the library root is a book of its own. */
#ifndef VITAVOX_SCANNER_H
#define VITAVOX_SCANNER_H
#include <stdint.h>
#include "models.h"

#define SCANNER_DEFAULT_ROOT "ux0:audiobooks"

typedef struct {
    char root[320];
    int entries_seen;   /* directory entries examined (recursive) */
    int audio_files;    /* of those, files with a supported extension */
    int books;          /* books added to the library */
    int skipped;        /* rejected entries (non-audio, empty dirs, dots) */
    char error[160];    /* last open/scan error, else empty */
} ScannerStats;

void scanner_set_cover_dir(const char *dir);
/* Cooperative cancel for an in-flight scan/enrich (also honors app_quit). */
void scanner_cancel(int on);
/* Shutdown or scanner_cancel: long work should stop. */
int scanner_stopping(void);
/* Allocates *books (caller frees). 0 ok, -1 on error (*books may still be set). */
int scanner_scan(const char *root, BookSummary **books, int *n);
/* Returned when a load stopped for shutdown / scanner_cancel / its cancel callback. */
#define SCANNER_CANCELLED (-2)
/* Opening a big book: time spent on exact durations before the tracks after the
 * start are estimated from their size (refined after playback starts). */
#define SCANNER_OPEN_BUDGET_MS 300

typedef struct {
    int priority;        /* opening/playing: background enrich waits; MP3 parts timed from their header */
    double start;        /* book time playback starts at (with priority); < 0 = every track exact */
    int budget_ms;       /* with start >= 0: after this, tracks past the start are estimated */
    int (*cancel)(void *ud);
    void *ud;
} DetailOpts;

/* Fills *out (owned: call book_detail_free). Tracks have local file paths.
 * Exact durations; waits between files while a book is being opened. */
int scanner_load_detail(const BookSummary *b, BookDetail *out);
int scanner_load_detail_opts(const BookSummary *b, BookDetail *out, const DetailOpts *o);
/* Opening a book: the per-book file (books/<id>.bin) when valid, else a priority
 * load (enrich pauses). For many-file books the tracks after the start may be
 * estimated (out->provisional > 0): play at once, then scanner_refine_detail.
 * The file is (re)written only when every length is exact. Same ownership. */
int scanner_open_detail(const BookSummary *b, BookDetail *out);
int scanner_open_detail_at(const BookSummary *b, BookDetail *out, double start, int (*cancel)(void *), void *ud);
/* Exact priority load of every track (probe cache first), then books/<id>.bin.
 * SCANNER_CANCELLED when cancel() said so between files. */
int scanner_refine_detail(const BookSummary *b, BookDetail *out, int (*cancel)(void *), void *ud);
/* Write books/<id>.bin for a freshly loaded detail (skipped if any track lacks a
 * duration or is estimated). */
int scanner_save_detail(const BookSummary *b, const BookDetail *d);
/* Priority loads in flight (enrich and background detail loads wait for them). */
void scanner_priority_begin(void);
void scanner_priority_end(void);
int scanner_priority_active(void);
/* Background work calls this between books: waits while a book is being opened. */
void scanner_yield(void);
/* Diagnostics/tests: total ms background work spent waiting for priority loads. */
uint64_t scanner_priority_waited_ms(void);
/* Tests: FFmpeg probes the scanner ran; extra ms per file to emulate Vita I/O. */
int scanner_ffmpeg_probes(void);
void scanner_test_set_io_delay(int ms);
/* Cheap/timed metadata fill (and cache write). 0 if the summary was updated. */
int scanner_enrich(BookSummary *b);
/* 00.37: options for the background scan's enrich of one book. */
typedef struct {
    /* Multi-file book: called once its first file (or folder image) is read,
     * with title, author, cover and thumbnail set and duration 0 (pending),
     * before the other tracks are timed. */
    void (*on_early)(const BookSummary *b, void *ud);
    int (*cancel)(void *ud); /* checked between files: Refresh / folder change */
    void *ud;
} EnrichOpts;
/* As scanner_enrich; SCANNER_CANCELLED when cancel() or shutdown stopped it. */
int scanner_enrich_ex(BookSummary *b, const EnrichOpts *o);
/* Tests/diagnostics: MP3 tracks the scan timed from a checked header, and the
 * ones that fell back to FFmpeg (header missing / damaged / VBR without TOC). */
int scanner_mp3_header_reads(void);
int scanner_mp3_fallbacks(void);
bool scanner_is_audio(const char *name);
const ScannerStats *scanner_last_stats(void);

#endif
