#include "strutil.h"
#include "test.h"

TEST(copy_truncates_on_utf8_boundary) {
    char b[6];
    str_copy(b, sizeof b, "abcd\xC3\xA9xyz"); /* "abcdé..." : é is 2 bytes, would be split */
    CHECK_STR(b, "abcd");
    str_copy(b, sizeof b, NULL);
    CHECK_STR(b, "");
    str_copy(b, sizeof b, "hi");
    CHECK_STR(b, "hi");
    char c[8];
    str_copy(c, sizeof c, "\xE2\x80\x94\xE2\x80\x94\xE2\x80\x94"); /* three em dashes (3 bytes each) */
    CHECK_STR(c, "\xE2\x80\x94\xE2\x80\x94");
}
TEST(time_formats) {
    char b[16];
    fmt_time(0, b, sizeof b); CHECK_STR(b, "0:00");
    fmt_time(125.9, b, sizeof b); CHECK_STR(b, "2:05");
    fmt_time(3723, b, sizeof b); CHECK_STR(b, "1:02:03");
    fmt_time(-4, b, sizeof b); CHECK_STR(b, "0:00");
    fmt_duration(3600 * 12 + 300, b, sizeof b); CHECK_STR(b, "12h 5m");
    fmt_duration(2700, b, sizeof b); CHECK_STR(b, "45m");
    fmt_duration(30, b, sizeof b); CHECK_STR(b, "30s");
}
TEST(icontains) {
    CHECK(str_icontains("The Odyssey", "ody"));
    CHECK(!str_icontains("The Odyssey", "dune"));
    CHECK(str_icontains("anything", ""));
}
TEST(strip_html) {
    char s[] = "<p>Hello&nbsp;<b>world</b> &amp; more</p><p>Second &quot;para&quot;</p>";
    str_strip_html(s);
    CHECK_STR(s, "Hello world & more\nSecond \"para\"");
    char t[] = "a<br/>b &#39;c&#39; &lt;d&gt;";
    str_strip_html(t);
    CHECK_STR(t, "a\nb 'c' <d>");
}

TEST(natural_sort) {
    CHECK(str_natcasecmp("ch2.mp3", "ch10.mp3") < 0);
    CHECK(str_natcasecmp("ch10.mp3", "ch2.mp3") > 0);
    CHECK(str_natcasecmp("Chapter 2", "chapter 2") == 0);
    CHECK(str_natcasecmp("track02.m4b", "track2.m4b") == 0);
    CHECK(str_natcasecmp("a01b", "a1b") == 0);
    CHECK(str_natcasecmp("file 9.mp3", "file 09.mp3") == 0);
    CHECK(str_natcasecmp("book", "book 1") < 0);
    CHECK(str_natcasecmp("10", "2") > 0);
    CHECK(str_natcasecmp("Disc 1", "disc 2") < 0);
    CHECK(str_natcasecmp("CD1", "CD 2") < 0);
    CHECK(str_natcasecmp("CD 2", "CD1") > 0);
    CHECK(str_natcasecmp("CD1/01.mp3", "CD 2/01.mp3") < 0);
    CHECK(str_natcasecmp("Disc 2", "Disc 10") < 0);
    CHECK(str_natcasecmp("CD-1", "CD02") < 0);
}

#include <stdlib.h>
static void desc_is(const char *in, const char *want) {
    char *d = str_clean_description(in, DESC_MAX_BYTES);
    CHECK_STR(d, want);
    free(d);
}
TEST(clean_description) {
    char *d = str_clean_description(NULL, DESC_MAX_BYTES);
    CHECK(d == NULL);
    d = str_clean_description(" \r\n\t \n", DESC_MAX_BYTES);
    CHECK(d == NULL);
    desc_is("  A plain   comment.  ", "A plain comment.");
    /* CRLF, tabs, trailing spaces; more than one blank line collapses to one */
    desc_is("Line one\r\nLine\ttwo  \r\n\r\n\r\n\r\nPara two\n\n\n\n", "Line one\nLine two\n\nPara two");
    desc_is("one\n\ntwo", "one\n\ntwo");
    /* HTML from store descriptions */
    desc_is("<p>First &amp; <b>bold</b>.</p><p>Second&nbsp;para&#8217;s end<br/>next</p>",
            "First & bold.\n\nSecond para\xE2\x80\x99s end\nnext");
    desc_is("<div>A</div>\n\n<div>B</div>", "A\n\nB");
    desc_is("Hex &#x263A; and &#169; ok", "Hex \xE2\x98\xBA and \xC2\xA9 ok");
    /* plain text with angle brackets is not a tag */
    desc_is("a < b and c > d, 3 <4", "a < b and c > d, 3 <4");
    desc_is("unclosed <b tag", "unclosed <b tag");
    /* invalid UTF-8 bytes (e.g. raw Latin-1) become '?'; valid multi-byte kept */
    desc_is("caf\xE9 na\xC3\xAFve \xF0\x9F\x8E\xA7", "caf? na\xC3\xAFve \xF0\x9F\x8E\xA7");
    desc_is("\xC0\xAF overlong \xED\xA0\x80 surrogate", "?? overlong ??? surrogate");
    desc_is("\xEF\xBB\xBF" "BOM first", "BOM first");
    desc_is("ctrl\x01\x7f chars", "ctrl chars");
    /* cap: on a word, with an ellipsis, never splitting a character */
    char big[20000];
    size_t o = 0;
    while (o + 12 < sizeof big - 1) { memcpy(big + o, "word \xC3\xA9" "t\xC3\xA9 ", 10); o += 10; }
    big[o] = 0;
    d = str_clean_description(big, DESC_MAX_BYTES);
    CHECK(d != NULL);
    if (d) {
        size_t n = strlen(d);
        CHECK(n <= DESC_MAX_BYTES);
        CHECK(n > DESC_MAX_BYTES - 220);
        CHECK(!strcmp(d + n - 3, "\xE2\x80\xA6"));
        CHECK(d[n - 4] != ' ');
        char *chk = str_clean_description(d, 0); /* already clean: unchanged, valid UTF-8 throughout */
        CHECK_STR(chk, d);
        free(chk);
    }
    free(d);
    /* a single 10 KB "word" is still cut on a character boundary */
    for (o = 0; o + 2 < 10000; o += 2) { big[o] = '\xC3'; big[o + 1] = '\xA9'; }
    big[o] = 0;
    d = str_clean_description(big, DESC_MAX_BYTES);
    CHECK(d && strlen(d) <= DESC_MAX_BYTES && (strlen(d) - 3) % 2 == 0);
    free(d);
}

void suite_strutil(void) {
    RUN(clean_description);
    RUN(copy_truncates_on_utf8_boundary);
    RUN(time_formats);
    RUN(icontains);
    RUN(strip_html);
    RUN(natural_sort);
}
