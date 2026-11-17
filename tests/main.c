#include "test.h"

int t_failures, t_checks;
const char *t_current;

#define SUITES(X) X(suite_timeline) X(suite_strutil) X(suite_pathutil) X(suite_probecache) X(suite_policy) X(suite_utils) X(suite_groups) X(suite_icons) X(suite_libcache) X(suite_tagart) X(suite_thumbs) X(suite_bookcache) X(suite_series) X(suite_audio_out) X(suite_dbg) X(suite_touch) X(suite_libroot) X(suite_mp3info) X(suite_statusbar) X(suite_hold)
#define DECL(s) void s(void);
SUITES(DECL)

int main(void) {
#define CALL(s) printf("%s\n", #s); s();
    SUITES(CALL)
    printf("\n%d checks, %d failures\n", t_checks, t_failures);
    return t_failures ? 1 : 0;
}
