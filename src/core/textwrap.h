/* 00.38: word wrapping shared by the UI (gfx_wrap*) and the host tests. The
 * width of a run of text comes from a callback, so it runs without a font. */
#ifndef VITAVOX_TEXTWRAP_H
#define VITAVOX_TEXTWRAP_H

typedef float (*WrapMeasure)(const char *text, void *ud);

/* The next line of s from pos that fits max_w, copied (leading spaces skipped)
 * into line (n bytes). Returns the bytes consumed from pos (0 at the end).
 * Breaks at spaces; a word wider than a whole line (a URL, CJK text without
 * spaces) is broken between characters, at least one character per line.
 * Stops at '\n' without consuming it. */
int wrap_next_line(WrapMeasure m, void *ud, const char *s, int pos, float max_w, char *line, int n);

/* Lines s wraps into; empty lines ("\n\n") count as lines. With start/len
 * (max_lines entries each) also the byte range of each line in s; empty
 * lines have len 0. */
int wrap_layout(WrapMeasure m, void *ud, float max_w, const char *s, int *start, int *len, int max_lines);

#endif
