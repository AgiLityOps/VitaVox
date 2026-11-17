/* Playback domain logic: opening local books, resume position, chapters and
 * skips, speed, sleep timer, and saving progress. Called from the UI thread. */
#ifndef VITAVOX_PLAYER_H
#define VITAVOX_PLAYER_H
#include <stdbool.h>

#include "config.h"
#include "models.h"
#include "policy.h"

typedef enum { PL_IDLE, PL_OPENING, PL_READY, PL_FAILED } PlayerPhase;

void player_init(Config *cfg);
void player_exit(void);  /* saves position and stops playback */
/* Stops playback completely (library folder change): saves progress, stops the
 * engine and its preopen, forgets the book and any open in flight. */
void player_close(void);

/* Starts opening a book. start < 0 resumes where the user left off. */
void player_open(const BookSummary *b, double start);
PlayerPhase player_phase(void);
const char *player_error(void);
bool player_has_book(void);
const BookDetail *player_book(void);
const char *player_book_id(void);
bool player_is_local(void);   /* always true in this offline player */

void player_toggle(void);
void player_pause(void);
bool player_playing(void);
void player_skip(double seconds);
void player_chapter(int dir);
void player_jump(double t);
void player_cycle_speed(void);
void player_set_speed(float s);
float player_speed(void);
void player_cycle_sleep(void);
const SleepTimer *player_sleep(void);
void player_sleep_label(char *out, int n);
/* True once after the sleep timer paused playback (then: turn the display off). */
bool player_take_sleep_ended(void);

double player_position(void);
double player_duration(void);
int player_chapter_index(void);

/* per frame: sleep timer and periodic progress saves */
void player_tick(double dt);
/* write the current position now (pause, exit, or a periodic flush) */
void player_flush_queue(void);
/* status line for the UI, or NULL */
const char *player_status(void);

#endif
