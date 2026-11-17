#include "textwrap.h"

#include <string.h>

static int utf8_len(unsigned char c) {
    return c < 0xC0 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
}

int wrap_next_line(WrapMeasure m, void *ud, const char *s, int pos, float max_w, char *line, int n) {
    int best = -1, i = pos;
    while (s[i] == ' ') i++;
    int start = i;
    for (;;) {
        while (s[i] && s[i] != ' ' && s[i] != '\n') i++;
        int len = i - start;
        if (len >= n) len = n - 1;
        memcpy(line, s + start, len);
        line[len] = 0;
        if (m(line, ud) > max_w) {
            if (best >= 0) break;
            /* the first word alone is too wide: as many whole characters as fit (at least one) */
            int e = start, fit = -1;
            while (e < i) {
                int ne = e + utf8_len((unsigned char)s[e]);
                if (ne > i) ne = i;
                if (ne - start >= n) break;
                memcpy(line, s + start, ne - start);
                line[ne - start] = 0;
                if (fit >= 0 && m(line, ud) > max_w) break;
                fit = ne;
                e = ne;
            }
            best = fit > start ? fit : i;
            break;
        }
        best = i;
        if (!s[i] || s[i] == '\n') break;
        i++;
    }
    int len = best - start;
    if (len >= n) {
        len = n - 1;
        while (len > 0 && ((unsigned char)s[start + len] & 0xC0) == 0x80) len--; /* whole characters */
    }
    memcpy(line, s + start, len);
    line[len] = 0;
    return best - pos;
}

int wrap_layout(WrapMeasure m, void *ud, float max_w, const char *s, int *start, int *len, int max_lines) {
    if (!s) return 0;
    char line[512];
    int pos = 0, lines = 0, slen = (int)strlen(s);
    while (pos < slen && lines < max_lines) {
        if (s[pos] == '\n') {
            if (start) { start[lines] = pos; len[lines] = 0; }
            pos++;
            lines++;
            continue;
        }
        int used = wrap_next_line(m, ud, s, pos, max_w, line, sizeof line);
        if (used <= 0) break;
        if (start) {
            int b = pos;
            while (s[b] == ' ') b++;
            start[lines] = b;
            len[lines] = (int)strlen(line);
        }
        pos += used;
        if (s[pos] == '\n') pos++;
        lines++;
    }
    return lines;
}
