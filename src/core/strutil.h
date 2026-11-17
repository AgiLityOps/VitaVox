#ifndef VITAVOX_STRUTIL_H
#define VITAVOX_STRUTIL_H
#include <stddef.h>

/* Copies src into dst (size n), truncating on a UTF-8 character boundary.
 * NULL src yields "". Always NUL-terminates when n > 0. */
void str_copy(char *dst, size_t n, const char *src);
/* "1:02:03" for >= 1 h, else "2:03". Negative values clamp to 0. */
void fmt_time(double seconds, char *out, size_t n);
/* "12h 5m" / "45m" / "30s" style duration for book cards. */
void fmt_duration(double seconds, char *out, size_t n);
/* Case-insensitive (ASCII) substring search. */
int str_icontains(const char *hay, const char *needle);
/* Removes HTML tags and decodes a few entities in place (ABS descriptions are HTML). */
void str_strip_html(char *s);
/* 00.38: a book description from a file tag, ready to draw: invalid UTF-8 bytes
 * become '?', HTML tags are removed (block tags become line breaks; a plain
 * "a < b" is kept), entities decoded, CR/LF and tabs normalised, runs of spaces
 * collapsed, lines trimmed, more than one blank line in a row collapsed to one,
 * and the result capped at max_bytes on a character (preferably word) boundary
 * with an ellipsis. malloc'd; NULL when nothing is left. */
char *str_clean_description(const char *in, size_t max_bytes);
#define DESC_MAX_BYTES 8192

#endif
/* Natural, case-insensitive compare: "Chapter 2" < "Chapter 10". */
int str_natcasecmp(const char *a, const char *b);
