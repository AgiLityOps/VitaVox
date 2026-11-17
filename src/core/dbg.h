/* Debug log (ux0:data/VitaVox/debug.log on the Vita; stderr on the host
 * unless a path is set). Thread-safe.
 *
 * Rotation (00.32): the log is capped at DBG_MAX_BYTES (4 MB). When the next
 * line would go past it, debug.old.log is deleted, debug.log becomes
 * debug.old.log and a fresh debug.log starts with a line noting the rotation,
 * so there are never more than two files (8 MB). dbg_init rotates an oversized
 * log left by an older version at once, and removes the pre-00.32 "debug.log.old".
 *
 * Async mode (dbg_start_async, used on the Vita): dbg() only formats the line
 * and copies it into a RAM queue, so the audio decode/output threads never
 * wait on the memory card; a lowest-priority thread writes (and rotates). Once
 * app_quit_active() is set, lines are written synchronously again so the
 * shutdown steps reach the card even if the process is killed. */
#ifndef VITAVOX_DBG_H
#define VITAVOX_DBG_H
#include <stddef.h>

#define DBG_MAX_BYTES (4u * 1024u * 1024u)

void dbg_init(const char *path);
void dbg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Start the background writer (Vita). Without it every dbg() writes directly. */
void dbg_start_async(void);
/* Write everything queued and stop the background writer (shutdown, tests). */
void dbg_stop_async(void);
/* Tests: change the cap (0 = DBG_MAX_BYTES). Applies to the next write. */
void dbg_set_limit(size_t bytes);
/* "<dir>/debug.old.log" for "<dir>/debug.log" (or "<path>.old" without ".log"). */
void dbg_old_path(const char *path, char *out, size_t n);
/* Diagnostics: rotations done / lines dropped because the queue was full. */
int dbg_rotations(void);
unsigned dbg_dropped(void);
#endif
