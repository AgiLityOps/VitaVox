/* Cover art: fetched and decoded on a worker (JPEG -> 256 px square RGBA plus an
 * accent colour), uploaded to textures on the UI thread, cached on the memory card. */
#ifndef VITAVOX_COVERS_H
#define VITAVOX_COVERS_H
#include <stdbool.h>
#include <stdint.h>

void covers_init(const char *cache_dir);
void covers_exit(void);
/* Uploads finished decodes (a couple per frame). */
void covers_tick(void);
/* Draws the cover of book id (or a placeholder while it loads). */
void covers_draw(const char *id, float x, float y, float size, float alpha);
/* Accent colour of the cover (fallback until it is known). */
uint32_t covers_accent(const char *id, uint32_t fallback);
/* Average (darkened) colour for backdrops. */
uint32_t covers_backdrop(const char *id, uint32_t fallback);
/* Drop a cached slot so the next draw reloads from disk (after enrich saves a cover). */
void covers_invalidate(const char *id);

/* Player only: 500 px player cover of the book being played. Call once audio
 * output has started; idempotent. src is the book's cover source; audio is its
 * first track (embedded art). Loaded from covers/p_<hash>.png when it exists,
 * otherwise made from the original art and saved, on a lowest-priority worker. */
void covers_full_request(const char *id, const char *src, const char *audio);
/* 0 = player cover not ready (draw the thumb), rising to 1 over the cross-fade. */
float covers_full_fade(const char *id);
/* Draws the player cover if ready (alpha = fade for the cross-fade); false if not. */
bool covers_full_draw(const char *id, float x, float y, float size, float alpha);
/* Leaving the player: drop an in-flight full-size load (also automatic when the
 * player stops drawing). */
void covers_full_cancel(void);
/* Library folder changed: forget every cover (book IDs can mean other books
 * now) and drop queued loads and the player cover. Card caches are kept. */
void covers_reset(void);

#endif
