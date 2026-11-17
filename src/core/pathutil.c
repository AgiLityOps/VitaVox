#include "pathutil.h"

#include <stdio.h>
#include <string.h>

#include "strutil.h"

static const char *kExt[] = {
    ".m4b", ".m4a", ".mp4", ".mov", ".mp3", ".aac", ".flac", ".ogg", ".oga",
    ".opus", ".mka", ".mkv", ".webm", ".wav", ".aiff", ".aif", ".wma", ".asf",
    ".ape", ".wv", ".caf", ".tta", NULL
};

int path_join(char *dst, size_t n, const char *a, const char *b) {
    if (!a) a = "";
    if (!b) b = "";
    size_t la = strlen(a);
    int slash = la > 0 && a[la - 1] != '/' && a[la - 1] != '\\' && a[la - 1] != ':';
    return snprintf(dst, n, "%s%s%s", a, slash ? "/" : "", b);
}

const char *path_ext(const char *path) {
    const char *dot = NULL, *base = path ? path : "";
    for (const char *p = base; *p; p++) {
        if (*p == '/' || *p == '\\') {
            dot = NULL;
            base = p + 1;
        } else if (*p == '.') {
            dot = p;
        }
    }
    return dot ? dot : "";
}

static void lower_copy(char *dst, size_t n, const char *s) {
    size_t i = 0;
    if (!s) s = "";
    for (; s[i] && i + 1 < n; i++) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        dst[i] = c;
    }
    if (n) dst[i] = 0;
}

bool path_is_audio(const char *name) {
    if (!name || !name[0]) return false;
    const char *ext = path_ext(name);
    char e[16];
    if (ext[0] == '.') {
        lower_copy(e, sizeof e, ext);
    } else {
        /* no dot: accept a bare extension such as "m4b" or "MP3", but not a path */
        for (const char *p = name; *p; p++)
            if (*p == '/' || *p == '\\') return false;
        e[0] = '.';
        lower_copy(e + 1, sizeof e - 1, name);
    }
    for (int i = 0; kExt[i]; i++) if (!strcmp(e, kExt[i])) return true;
    return false;
}


static const char *kImg[] = { ".jpg", ".jpeg", ".png", ".webp", NULL };
static const char *kSide[] = {
    ".cue", ".m3u", ".m3u8", ".pls", ".nfo", ".log", ".txt", ".ini", ".md",
    ".sfv", ".accurip", ".url", ".html", ".htm", ".pdf", ".toc", NULL
};

static int ext_in(const char *name, const char *const *list) {
    if (!name || !name[0]) return 0;
    const char *ext = path_ext(name);
    char e[16];
    if (ext[0] == '.') lower_copy(e, sizeof e, ext);
    else {
        for (const char *p = name; *p; p++)
            if (*p == '/' || *p == '\\') return 0;
        e[0] = '.';
        lower_copy(e + 1, sizeof e - 1, name);
    }
    for (int i = 0; list[i]; i++) if (!strcmp(e, list[i])) return 1;
    return 0;
}

bool path_is_image(const char *name) {
    return ext_in(name, kImg);
}

bool path_is_sidecar(const char *name) {
    return ext_in(name, kSide);
}

bool path_is_disc_folder(const char *name) {
    if (!name || !name[0]) return false;
    /* Basename only. */
    const char *s = name;
    for (const char *p = name; *p; p++)
        if (*p == '/' || *p == '\\') s = p + 1;
    char b[256];
    lower_copy(b, sizeof b, s);
    char *p = b;
    while (*p == ' ' || *p == '_' || *p == '.' || *p == '-') p++;
    if (!strncmp(p, "digital", 7)) {
        p += 7;
        while (*p == ' ' || *p == '_' || *p == '.' || *p == '-') p++;
    }
    static const char *kw[] = { "disc", "disk", "dvd", "cd", "part", "pt", NULL };
    int kwlen = 0;
    for (int i = 0; kw[i]; i++) {
        size_t n = strlen(kw[i]);
        if (!strncmp(p, kw[i], n)) { kwlen = (int)n; break; }
    }
    if (!kwlen) return false;
    p += kwlen;
    while (*p == ' ' || *p == '_' || *p == '.' || *p == '-') p++;
    return *p >= '0' && *p <= '9';
}

const char *path_demuxer_name(const char *path) {
    const char *ext = path_ext(path);
    if (!ext || ext[0] != '.') return NULL;
    char e[16];
    lower_copy(e, sizeof e, ext);
    if (!strcmp(e, ".flac")) return "flac";
    if (!strcmp(e, ".mp3")) return "mp3";
    if (!strcmp(e, ".ogg") || !strcmp(e, ".oga") || !strcmp(e, ".opus")) return "ogg";
    if (!strcmp(e, ".wav")) return "wav";
    if (!strcmp(e, ".wma") || !strcmp(e, ".asf")) return "asf";
    if (!strcmp(e, ".m4b") || !strcmp(e, ".m4a") || !strcmp(e, ".mp4")
        || !strcmp(e, ".mov") || !strcmp(e, ".m4v") || !strcmp(e, ".3gp"))
        return "mov";
    if (!strcmp(e, ".mkv") || !strcmp(e, ".mka") || !strcmp(e, ".webm")) return "matroska";
    if (!strcmp(e, ".aac")) return "aac";
    if (!strcmp(e, ".aiff") || !strcmp(e, ".aif")) return "aiff";
    if (!strcmp(e, ".ape")) return "ape";
    if (!strcmp(e, ".wv")) return "wv";
    if (!strcmp(e, ".caf")) return "caf";
    if (!strcmp(e, ".tta")) return "tta";
    return NULL;
}

const char *path_file_url(const char *path, char *buf, size_t n) {
    if (!buf || !n) return buf;
    buf[0] = 0;
    if (!path || !path[0]) return buf;
    if (!strncmp(path, "file:", 5)) {
        str_copy(buf, n, path);
        return buf;
    }
    /* A colon before the first slash is a URL/device scheme (ux0:book.m4b). */
    const char *colon = strchr(path, ':');
    const char *slash = strpbrk(path, "/\\");
    if (colon && (!slash || colon < slash)) {
        snprintf(buf, n, "file:%s", path);
        return buf;
    }
    str_copy(buf, n, path);
    return buf;
}
