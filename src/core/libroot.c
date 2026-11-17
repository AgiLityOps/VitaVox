#include "libroot.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef __vita__
#include <psp2/io/dirent.h>
#else
#include <dirent.h>
#endif

#include "dbg.h"
#include "libcache.h"
#include "strutil.h"

static const char *const kDrives[] = {"ux0", "uma0", "imc0", "ur0", "grw0", "xmc0"};

bool libroot_known_drive(const char *drive) {
    for (size_t i = 0; i < sizeof kDrives / sizeof kDrives[0]; i++)
        if (drive && !strcmp(drive, kDrives[i])) return true;
    return false;
}

LibRootStatus libroot_normalize(const char *in, char *out, size_t n) {
    if (!out || !n) return LR_EMPTY;
    out[0] = 0;
    if (!in) return LR_EMPTY;
    while (*in && isspace((unsigned char)*in)) in++;
    size_t len = strlen(in);
    while (len && isspace((unsigned char)in[len - 1])) len--;
    if (!len) return LR_EMPTY;
    const char *col = memchr(in, ':', len);
    size_t k = 0;
    size_t start = 0;
    if (col) {
        size_t dl = (size_t)(col - in);
        for (size_t i = 0; i < dl && k + 1 < n; i++) out[k++] = (char)tolower((unsigned char)in[i]);
        if (k + 1 < n) out[k++] = ':';
        start = dl + 1;
        while (start < len && (in[start] == '/' || in[start] == '\\')) start++; /* no slash after the colon */
    }
    for (size_t i = start; i < len && k + 1 < n; i++) {
        char c = in[i] == '\\' ? '/' : in[i];
        if (c == '/' && k > 0 && (out[k - 1] == '/' || out[k - 1] == ':')) continue; /* doubled slash */
        out[k++] = c;
    }
    while (k > 0 && out[k - 1] == '/') k--;
    out[k] = 0;
    if (!col) return LR_NO_DRIVE;
    char drive[16];
    size_t dl = (size_t)(strchr(out, ':') - out);
    if (dl == 0 || dl >= sizeof drive) return LR_UNKNOWN_DRIVE;
    memcpy(drive, out, dl);
    drive[dl] = 0;
    return libroot_known_drive(drive) ? LR_OK : LR_UNKNOWN_DRIVE;
}

static char g_host_prefix[256];
void libroot_set_host_prefix(const char *prefix) { str_copy(g_host_prefix, sizeof g_host_prefix, prefix ? prefix : ""); }

bool libroot_dir_exists(const char *path) {
    if (!path || !path[0]) return false;
#ifdef __vita__
    char p[330];
    /* as given, then "ux0:/x", then with a trailing slash (same variants as the scanner) */
    SceUID fd = sceIoDopen(path);
    if (fd < 0) {
        const char *col = strchr(path, ':');
        if (col && col[1] != '/') {
            snprintf(p, sizeof p, "%.*s:/%s", (int)(col - path), path, col + 1);
            fd = sceIoDopen(p);
        }
    }
    if (fd < 0) {
        snprintf(p, sizeof p, "%s/", path);
        fd = sceIoDopen(p);
    }
    if (fd < 0) return false;
    sceIoDclose(fd);
    return true;
#else
    char p[600];
    const char *col = strchr(path, ':');
    if (g_host_prefix[0] && col)
        snprintf(p, sizeof p, "%s/%.*s/%s", g_host_prefix, (int)(col - path), path, col + 1);
    else
        str_copy(p, sizeof p, path);
    DIR *d = opendir(p);
    if (!d) return false;
    closedir(d);
    return true;
#endif
}

LibRootStatus libroot_check(const char *norm) {
    char tmp[320];
    LibRootStatus st = libroot_normalize(norm, tmp, sizeof tmp);
    if (st != LR_OK) return st;
    char drive[16];
    size_t dl = (size_t)(strchr(tmp, ':') - tmp);
    snprintf(drive, sizeof drive, "%.*s:", (int)dl, tmp);
    if (!libroot_dir_exists(drive)) return LR_DRIVE_MISSING;
    if (tmp[dl + 1] && !libroot_dir_exists(tmp)) return LR_NOT_FOUND;
    return LR_OK;
}

void libroot_message(LibRootStatus st, const char *path, char *out, size_t n) {
    const char *p = path && path[0] ? path : "?";
    switch (st) {
    case LR_OK: snprintf(out, n, "Library folder: %s", p); break;
    case LR_EMPTY: snprintf(out, n, "No folder entered - library folder unchanged"); break;
    case LR_NO_DRIVE:
    case LR_UNKNOWN_DRIVE:
        snprintf(out, n, "Unknown drive in %s (use ux0:, uma0:, imc0:, ur0:, grw0: or xmc0:) - library folder unchanged", p);
        break;
    case LR_DRIVE_MISSING: {
        const char *col = strchr(p, ':');
        snprintf(out, n, "Drive %.*s: is not available - library folder unchanged", col ? (int)(col - p) : 0, p);
        break;
    }
    case LR_NOT_FOUND: snprintf(out, n, "Folder not found: %s - library folder unchanged", p); break;
    }
}

void libroot_catalog_path(const char *dir, const char *norm, char *out, size_t n) {
    char name[64];
    size_t k = 0;
    uint32_t h = 2166136261u;
    for (const unsigned char *s = (const unsigned char *)(norm ? norm : ""); *s; s++) {
        h = (h ^ *s) * 16777619u;
        if (k + 1 < 48) name[k++] = (isalnum(*s) || *s == '-' || *s == '.') ? (char)*s : '_';
    }
    name[k] = 0;
    snprintf(out, n, "%s/%s-%08x.json", dir ? dir : ".", k ? name : "library", (unsigned)h);
}

static bool file_exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0;
}

int libroot_migrate_legacy(const char *legacy, const char *dir, const char *fallback_root) {
    if (!legacy || !legacy[0] || !file_exists(legacy)) return 0;
    LibraryCache c;
    char root[320] = "";
    if (libcache_load(legacy, &c) == 0 && c.root[0]) libroot_normalize(c.root, root, sizeof root);
    libcache_free(&c);
    if (!root[0]) libroot_normalize(fallback_root, root, sizeof root);
    if (!root[0]) return -1;
    mkdir(dir, 0777);
    char target[400];
    libroot_catalog_path(dir, root, target, sizeof target);
    if (file_exists(target)) {
        dbg("libroot: '%s' already has a catalog; removing old '%s'", root, legacy);
        remove(legacy);
        return 0;
    }
    if (rename(legacy, target) != 0) {
        dbg("libroot: could not move '%s' to '%s'", legacy, target);
        return -1;
    }
    dbg("libroot: migrated '%s' -> '%s' (folder %s)", legacy, target, root);
    return 1;
}

LibSwitchResult libswitch_apply(const char *input, const LibSwitchOps *ops, char *msg, size_t n) {
    char norm[320];
    LibRootStatus st = libroot_normalize(input, norm, sizeof norm);
    if (st == LR_OK) st = libroot_check(norm);
    if (st != LR_OK) {
        libroot_message(st, norm[0] ? norm : input, msg, n);
        dbg("libroot: change to '%s' refused (%d)", input ? input : "", (int)st);
        return LS_REFUSED;
    }
    const char *cur = ops && ops->current_root ? ops->current_root(ops->ud) : "";
    char curn[320];
    libroot_normalize(cur, curn, sizeof curn);
    if (!strcmp(curn, norm)) {
        snprintf(msg, n, "Library folder unchanged - refreshing");
        if (ops && ops->refresh) ops->refresh(ops->ud);
        return LS_SAME;
    }
    dbg("libroot: library folder %s -> %s", curn, norm);
    if (ops && ops->stop_playback) ops->stop_playback(ops->ud);
    if (ops && ops->switch_root) ops->switch_root(ops->ud, norm);
    if (ops && ops->refresh) ops->refresh(ops->ud);
    snprintf(msg, n, "Library folder changed to %s", norm);
    return LS_CHANGED;
}
