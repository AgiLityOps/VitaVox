#include "strutil.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

void str_copy(char *dst, size_t n, const char *src) {
    if (!n) return;
    if (!src) { dst[0] = 0; return; }
    size_t len = strlen(src);
    if (len >= n) {
        len = n - 1;
        /* back up over continuation bytes, then drop the partial lead byte */
        while (len > 0 && ((unsigned char)src[len] & 0xC0) == 0x80) len--;
    }
    memcpy(dst, src, len);
    dst[len] = 0;
}

void fmt_time(double seconds, char *out, size_t n) {
    if (seconds < 0) seconds = 0;
    long s = (long)seconds;
    long h = s / 3600, m = (s / 60) % 60, sec = s % 60;
    if (h) snprintf(out, n, "%ld:%02ld:%02ld", h, m, sec);
    else snprintf(out, n, "%ld:%02ld", m, sec);
}

void fmt_duration(double seconds, char *out, size_t n) {
    if (seconds < 0) seconds = 0;
    long s = (long)(seconds + 0.5);
    long h = s / 3600, m = (s / 60) % 60;
    if (h) snprintf(out, n, "%ldh %ldm", h, m);
    else if (m) snprintf(out, n, "%ldm", m);
    else snprintf(out, n, "%lds", s);
}

int str_icontains(const char *hay, const char *needle) {
    if (!needle || !*needle) return 1;
    if (!hay) return 0;
    size_t nl = strlen(needle);
    for (; *hay; hay++) {
        size_t i = 0;
        while (i < nl && hay[i] && tolower((unsigned char)hay[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == nl) return 1;
    }
    return 0;
}

static const struct { const char *name; const char *rep; } kEntities[] = {
    {"&nbsp;", " "}, {"&amp;", "&"}, {"&quot;", "\""}, {"&#39;", "'"}, {"&apos;", "'"},
    {"&lt;", "<"}, {"&gt;", ">"}, {"&rsquo;", "\xE2\x80\x99"}, {"&lsquo;", "\xE2\x80\x98"},
    {"&ldquo;", "\xE2\x80\x9C"}, {"&rdquo;", "\xE2\x80\x9D"}, {"&mdash;", "\xE2\x80\x94"}, {"&ndash;", "\xE2\x80\x93"},
    {"&hellip;", "\xE2\x80\xA6"},
};

void str_strip_html(char *s) {
    char *w = s;
    const char *r = s;
    while (*r) {
        if (*r == '<') {
            const char *e = strchr(r, '>');
            if (!e) break;
            /* block-level breaks become newlines */
            int is_break = (r[1] == 'b' && r[2] == 'r') || (r[1] == '/' && (r[2] == 'p' || r[2] == 'd' || r[2] == 'l'));
            if (is_break && w > s && w[-1] != '\n') *w++ = '\n';
            r = e + 1;
            continue;
        }
        if (*r == '&') {
            int done = 0;
            for (size_t i = 0; i < sizeof kEntities / sizeof kEntities[0]; i++) {
                size_t l = strlen(kEntities[i].name);
                if (strncmp(r, kEntities[i].name, l) == 0) {
                    size_t rl = strlen(kEntities[i].rep);
                    if (rl <= l) { memcpy(w, kEntities[i].rep, rl); w += rl; r += l; done = 1; }
                    break;
                }
            }
            if (done) continue;
        }
        *w++ = *r++;
    }
    /* trim trailing whitespace/newlines */
    while (w > s && (w[-1] == '\n' || w[-1] == ' ')) w--;
    *w = 0;
}

static int nat_sep(unsigned char c) {
    return c == ' ' || c == '_' || c == '-' || c == '.';
}

int str_natcasecmp(const char *a, const char *b) {
    if (!a) a = "";
    if (!b) b = "";
    while (*a && *b) {
        /* "CD1" vs "CD 2" / "CD-02": ignore spaces and punctuation sitting
         * in front of a number so disc folders sort in numeric order. */
        if (nat_sep((unsigned char)*a) && isdigit((unsigned char)*b)) { a++; continue; }
        if (nat_sep((unsigned char)*b) && isdigit((unsigned char)*a)) { b++; continue; }
        if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
            while (*a == '0') a++;
            while (*b == '0') b++;
            const char *pa = a, *pb = b;
            while (isdigit((unsigned char)*a)) a++;
            while (isdigit((unsigned char)*b)) b++;
            size_t na = (size_t)(a - pa), nb = (size_t)(b - pb);
            if (na != nb) return na < nb ? -1 : 1;
            int c = strncmp(pa, pb, na);
            if (c) return c;
            continue;
        }
        int ca = tolower((unsigned char)*a);
        int cb = tolower((unsigned char)*b);
        if (ca != cb) return ca - cb;
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

/* ---- 00.38: book descriptions from file tags ------------------------------------ */

/* Length of the valid UTF-8 sequence at s (1..4), or 0 if it is not valid. */
static int utf8_seq(const unsigned char *s) {
    unsigned c = s[0];
    if (c < 0x80) return 1;
    int n;
    unsigned min;
    if (c >= 0xC2 && c <= 0xDF) { n = 2; min = 0x80; }
    else if (c >= 0xE0 && c <= 0xEF) { n = 3; min = 0x800; }
    else if (c >= 0xF0 && c <= 0xF4) { n = 4; min = 0x10000; }
    else return 0;
    unsigned cp = c & (0x3F >> (n - 1));
    for (int i = 1; i < n; i++) {
        if ((s[i] & 0xC0) != 0x80) return 0; /* also stops at the NUL */
        cp = (cp << 6) | (s[i] & 0x3F);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
    return n;
}

static int put_utf8(char *o, unsigned cp) {
    if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) { o[0] = '?'; return 1; }
    if (cp < 0x80) { o[0] = (char)cp; return 1; }
    if (cp < 0x800) { o[0] = (char)(0xC0 | (cp >> 6)); o[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) {
        o[0] = (char)(0xE0 | (cp >> 12)); o[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    o[0] = (char)(0xF0 | (cp >> 18)); o[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    o[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

static int tag_is(const char *t, const char *name) {
    size_t n = strlen(name);
    if (strncasecmp(t, name, n)) return 0;
    char c = t[n];
    return c == '>' || c == ' ' || c == '/' || c == '\t' || c == '\n' || c == '\r';
}

/* Pass 1: UTF-8 repair, HTML tags and entities, line ends. Output <= input + small. */
static size_t desc_decode(const char *in, char *w) {
    size_t o = 0;
    const unsigned char *r = (const unsigned char *)in;
    while (*r) {
        unsigned c = *r;
        if (c == '<' && (isalpha(r[1]) || r[1] == '/' || r[1] == '!')) {
            const char *e = strchr((const char *)r, '>');
            if (e && e - (const char *)r < 1024) {
                const char *t = (const char *)r + 1;
                int close = *t == '/';
                if (close) t++;
                if (tag_is(t, "br") || tag_is(t, "li") || tag_is(t, "tr")) {
                    w[o++] = '\n';
                } else if (tag_is(t, "p") || tag_is(t, "div") || tag_is(t, "ul") || tag_is(t, "ol")
                           || tag_is(t, "blockquote") || tag_is(t, "h1") || tag_is(t, "h2") || tag_is(t, "h3")
                           || tag_is(t, "h4") || tag_is(t, "h5") || tag_is(t, "h6")) {
                    w[o++] = '\n'; /* paragraph: a blank line (pass 2 collapses repeats) */
                    w[o++] = '\n';
                }
                (void)close;
                r = (const unsigned char *)e + 1;
                continue;
            }
        }
        if (c == '&') {
            const char *s = (const char *)r;
            if (s[1] == '#') {
                unsigned cp = 0;
                int hex = s[2] == 'x' || s[2] == 'X', k = hex ? 3 : 2, digits = 0;
                while (digits < 7 && (hex ? isxdigit((unsigned char)s[k]) : isdigit((unsigned char)s[k]))) {
                    char d = s[k++];
                    cp = cp * (hex ? 16 : 10) + (unsigned)(isdigit((unsigned char)d) ? d - '0' : (tolower((unsigned char)d) - 'a' + 10));
                    digits++;
                }
                if (digits && s[k] == ';') {
                    if (cp == 9 || cp == 10 || cp == 13) w[o++] = cp == 9 ? ' ' : '\n';
                    else if (cp >= 32) o += (size_t)put_utf8(w + o, cp == 0xA0 ? ' ' : cp);
                    r += k + 1;
                    continue;
                }
            }
            int done = 0;
            for (size_t i = 0; i < sizeof kEntities / sizeof kEntities[0]; i++) {
                size_t l = strlen(kEntities[i].name);
                if (strncmp(s, kEntities[i].name, l) == 0) {
                    size_t rl = strlen(kEntities[i].rep);
                    memcpy(w + o, kEntities[i].rep, rl);
                    o += rl;
                    r += l;
                    done = 1;
                    break;
                }
            }
            if (done) continue;
        }
        if (c == '\r') { w[o++] = '\n'; r += r[1] == '\n' ? 2 : 1; continue; }
        if (c == '\n') { w[o++] = '\n'; r++; continue; }
        if (c == '\t' || c == '\v' || c == '\f') { w[o++] = ' '; r++; continue; }
        if (c < 32 || c == 127) { r++; continue; } /* other control characters */
        int n = utf8_seq(r);
        if (n == 0) { w[o++] = '?'; r++; continue; } /* not UTF-8: never hand broken bytes to the font */
        if (n == 2 && r[0] == 0xC2 && r[1] == 0xA0) { w[o++] = ' '; r += 2; continue; } /* NBSP */
        if (n == 3 && r[0] == 0xEF && r[1] == 0xBB && r[2] == 0xBF) { r += 3; continue; } /* BOM */
        memcpy(w + o, r, (size_t)n);
        o += (size_t)n;
        r += n;
    }
    w[o] = 0;
    return o;
}

char *str_clean_description(const char *in, size_t max_bytes) {
    if (!in || !in[0]) return NULL;
    size_t len = strlen(in);
    if (len > 1024 * 1024) len = 1024 * 1024;
    /* entities never grow by more than 4 bytes over their 2+ byte source; tags add <= 2 newlines */
    char *src = malloc(len + 1);
    char *tmp = malloc(len * 2 + 8);
    if (!src || !tmp) { free(src); free(tmp); return NULL; }
    memcpy(src, in, len);
    src[len] = 0;
    /* a cut at 1 MB may split a character: utf8_seq turns the tail into '?' */
    desc_decode(src, tmp);
    free(src);
    /* Pass 2: per line, collapse spaces and trim; at most one blank line in a row. */
    char *out = tmp; /* in place: the output never outruns the read position */
    size_t o = 0;
    int blank_run = 0, any = 0;
    const char *p = tmp;
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t ll = e ? (size_t)(e - p) : strlen(p);
        size_t start = o;
        int space = 0;
        for (size_t i = 0; i < ll; i++) {
            char c = p[i];
            if (c == ' ') { space = 1; continue; }
            if (space && o > start) out[o++] = ' ';
            space = 0;
            out[o++] = c;
        }
        if (o == start) {
            blank_run++;
        } else {
            if (any) {
                /* line break before this line, plus one blank line if there were any */
                memmove(out + start + (blank_run ? 2 : 1), out + start, o - start);
                out[start] = '\n';
                if (blank_run) out[start + 1] = '\n';
                o += blank_run ? 2 : 1;
            }
            any = 1;
            blank_run = 0;
        }
        p += ll;
        if (*p == '\n') p++;
    }
    out[o] = 0;
    if (!o) { free(tmp); return NULL; }
    if (max_bytes >= 16 && o > max_bytes) {
        size_t cut = max_bytes - 4; /* room for the ellipsis */
        while (cut > 0 && ((unsigned char)out[cut] & 0xC0) == 0x80) cut--;
        /* end on a word when one is near */
        size_t k = cut;
        while (k > cut - (cut > 200 ? 200 : cut) && out[k] != ' ' && out[k] != '\n') k--;
        if (k > 0 && (out[k] == ' ' || out[k] == '\n')) cut = k;
        while (cut > 0 && (out[cut - 1] == ' ' || out[cut - 1] == '\n')) cut--;
        memcpy(out + cut, "\xE2\x80\xA6", 4);
        o = cut + 3;
    }
    char *res = realloc(out, o + 1);
    return res ? res : out;
}
