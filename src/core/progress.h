/* Per-book playback position and speed, stored under the app data directory. */
#ifndef VITAVOX_PROGRESS_H
#define VITAVOX_PROGRESS_H
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    double position;
    float speed;
    bool finished;
    uint64_t updated_ms;
    double duration;   /* book length when saved (00.33); 0 = unknown (older records) */
} BookProgress;

void progress_init(const char *dir);
/* 0 if a record was loaded, -1 if none / error (out left zeroed). */
int progress_load(const char *id, BookProgress *out);
int progress_save(const char *id, const BookProgress *p);

/* Length check (00.33): book IDs are the path inside the library folder, so two
 * folders can hold different books under the same ID. Saved progress only
 * applies if the saved length matches the book's (within max(5 s, 1 %)); an
 * unknown length on either side matches. */
bool progress_duration_matches(double saved, double actual);
/* progress_load + length check: 0 loaded and matching, -1 none, -2 a record
 * exists for a different-length book (out zeroed; the record is left alone
 * until this book's own progress overwrites it). */
int progress_load_for(const char *id, double duration, BookProgress *out);

#endif
