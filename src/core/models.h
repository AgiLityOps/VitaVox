/* Plain data types shared by the core, platform and UI layers. */
#ifndef VITAVOX_MODELS_H
#define VITAVOX_MODELS_H
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    double start, end;
    char title[96];
} Chapter;

typedef struct {
    int index;
    char ino[40];
    char path[320];         /* local file path */
    char codec[16];        /* "aac", "mp3", ... */
    char ext[8];           /* ".m4b", ".mp3", ... */
    char title[96];        /* usually the file name */
    double start_offset;   /* book time at which this track starts */
    double duration;
    uint64_t size;
} Track;

typedef struct {
    char id[40];
    char title[160];
    char author[96];
    char series[96];
    double duration;
    uint64_t added_at;
    float progress;
    double current_time;
    bool finished;
    uint64_t progress_updated; /* ms since epoch */
    char path[320];       /* book folder, or the single file at the library root */
    char cover_path[320]; /* full-size source: folder image, or the embedded-art PNG */
    char thumb_path[320]; /* 256 px menu thumbnail in the covers folder, may be empty */
} BookSummary;

typedef struct {
    char id[40];
    char title[160];
    char author[96];
    char narrator[96];
    char *description; /* malloc'd, may be NULL */
    double duration;
    int ntracks;
    Track *tracks; /* malloc'd */
    int nchapters;
    Chapter *chapters; /* malloc'd */
    int provisional;   /* 00.36: tracks whose length is still an estimate (opening a big book) */
} BookDetail;

typedef struct {
    char id[48];
    double current_time;
    BookDetail book;
} PlaySession;

void book_detail_free(BookDetail *b);

#endif
