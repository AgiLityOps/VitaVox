/* Library folder handling (00.33): path normalisation, folder validation, one
 * catalog file per library folder, and the "change library folder" sequence.
 * Pure C apart from the directory checks; host-tested. */
#ifndef VITAVOX_LIBROOT_H
#define VITAVOX_LIBROOT_H
#include <stdbool.h>
#include <stddef.h>

typedef enum {
    LR_OK = 0,
    LR_EMPTY = -1,          /* nothing entered */
    LR_NO_DRIVE = -2,       /* no "xxx:" drive prefix */
    LR_UNKNOWN_DRIVE = -3,  /* not one of ux0 uma0 imc0 ur0 grw0 xmc0 */
    LR_DRIVE_MISSING = -4,  /* known drive, not mounted / not available */
    LR_NOT_FOUND = -5,      /* drive fine, folder does not exist */
} LibRootStatus;

/* Canonical form: trimmed, '\' -> '/', lower-case drive, no slash right after
 * the colon, no doubled or trailing slashes: " UX0:/audiobooks/ " -> "ux0:audiobooks".
 * Returns LR_OK, LR_EMPTY, LR_NO_DRIVE or LR_UNKNOWN_DRIVE (out is still filled). */
LibRootStatus libroot_normalize(const char *in, char *out, size_t n);
bool libroot_known_drive(const char *drive);   /* "ux0" (no colon) */
/* Full check of a normalised path: drive known and mounted, folder exists. */
LibRootStatus libroot_check(const char *norm);
/* "Folder not found: <path> - library folder unchanged" etc. */
void libroot_message(LibRootStatus st, const char *path, char *out, size_t n);

/* Host tests only: "ux0:books" is looked up as "<prefix>/ux0/books". */
void libroot_set_host_prefix(const char *prefix);
/* Does this directory exist (Vita: sceIoDopen, with the scanner's variants)? */
bool libroot_dir_exists(const char *path);

/* <dir>/<sanitised path>-<hash>.json, e.g. libraries/ux0_audiobooks-1a2b3c4d.json */
void libroot_catalog_path(const char *libraries_dir, const char *norm, char *out, size_t n);
/* Moves an old single library.json to its folder's catalog file (folder taken
 * from the file's "root", else fallback_root). If that catalog already exists
 * the old file is removed. 1 migrated, 0 nothing to do, -1 error. */
int libroot_migrate_legacy(const char *legacy_path, const char *libraries_dir, const char *fallback_root);

/* The confirmed "change library folder" sequence, with the app's actions as callbacks. */
typedef struct {
    void *ud;
    const char *(*current_root)(void *ud);           /* normalised current folder */
    void (*stop_playback)(void *ud);                 /* stop engine, save progress, close player, cancel jobs */
    void (*switch_root)(void *ud, const char *norm); /* save setting, swap catalogs */
    void (*refresh)(void *ud);                       /* rescan */
} LibSwitchOps;
typedef enum { LS_REFUSED = -1, LS_SAME = 0, LS_CHANGED = 1 } LibSwitchResult;
/* Validates input; refused: nothing changes (msg says why). Same folder: refresh
 * only. Changed: stop_playback, switch_root, refresh, in that order. */
LibSwitchResult libswitch_apply(const char *input, const LibSwitchOps *ops, char *msg, size_t n);

#endif
