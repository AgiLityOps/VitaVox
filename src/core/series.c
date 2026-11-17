#include "series.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "dbg.h"
#include "library.h"
#include "strutil.h"

static void trim_name(const char *in, char *out, size_t n) {
    if (!in) in = "";
    while (*in == ' ' || *in == '\t' || *in == '\n' || *in == '\r') in++;
    str_copy(out, n, in);
    size_t l = strlen(out);
    while (l && (out[l - 1] == ' ' || out[l - 1] == '\t' || out[l - 1] == '\n' || out[l - 1] == '\r')) out[--l] = 0;
}

static int valid(const SeriesStore *st, int i) { return st && i >= 0 && i < st->n; }

static int name_taken(const SeriesStore *st, const char *name, int except) {
    for (int k = 0; k < st->n; k++)
        if (k != except && !strcasecmp(st->s[k].name, name)) return 1;
    return 0;
}

int series_find(const SeriesStore *st, const char *name) {
    if (!st || !name) return -1;
    char t[SERIES_NAME_MAX];
    trim_name(name, t, sizeof t);
    for (int k = 0; k < st->n; k++)
        if (!strcasecmp(st->s[k].name, t)) return k;
    return -1;
}

static int push_series(SeriesStore *st) {
    if (st->n >= SERIES_MAX) return -1;
    if (st->n == st->cap) {
        int c = st->cap ? st->cap * 2 : 8;
        Series *q = realloc(st->s, sizeof(Series) * (size_t)c);
        if (!q) return -1;
        st->s = q;
        st->cap = c;
    }
    memset(&st->s[st->n], 0, sizeof(Series));
    return st->n++;
}

static int push_id(Series *s, const char *id) {
    if (s->n >= SERIES_BOOKS_MAX) return -1;
    if (s->n == s->cap) {
        int c = s->cap ? s->cap * 2 : 8;
        char (*q)[40] = realloc(s->ids, sizeof(*s->ids) * (size_t)c);
        if (!q) return -1;
        s->ids = q;
        s->cap = c;
    }
    str_copy(s->ids[s->n], sizeof s->ids[0], id);
    s->n++;
    return 0;
}

static int index_of(const Series *s, const char *id) {
    for (int k = 0; k < s->n; k++)
        if (!strcmp(s->ids[k], id)) return k;
    return -1;
}

int series_create(SeriesStore *st, const char *name, uint64_t now) {
    if (!st) return -1;
    char t[SERIES_NAME_MAX];
    trim_name(name, t, sizeof t);
    if (!t[0]) return -1;
    if (name_taken(st, t, -1)) return -2;
    int i = push_series(st);
    if (i < 0) return -1;
    str_copy(st->s[i].name, sizeof st->s[i].name, t);
    st->s[i].sort = SERIES_SORT_ALPHA;
    st->s[i].list_view = false;
    st->s[i].created = now;
    return i;
}

int series_rename(SeriesStore *st, int i, const char *name) {
    if (!valid(st, i)) return -1;
    char t[SERIES_NAME_MAX];
    trim_name(name, t, sizeof t);
    if (!t[0]) return -1;
    if (name_taken(st, t, i)) return -2;
    str_copy(st->s[i].name, sizeof st->s[i].name, t);
    return 0;
}

int series_delete(SeriesStore *st, int i) {
    if (!valid(st, i)) return -1;
    free(st->s[i].ids);
    memmove(&st->s[i], &st->s[i + 1], sizeof(Series) * (size_t)(st->n - i - 1));
    st->n--;
    return 0;
}

int series_add(SeriesStore *st, int i, const char *id) {
    if (!valid(st, i) || !id || !id[0] || strlen(id) >= 40) return -1;
    if (index_of(&st->s[i], id) >= 0) return 1;
    return push_id(&st->s[i], id);
}

int series_remove(SeriesStore *st, int i, const char *id) {
    if (!valid(st, i) || !id) return -1;
    Series *s = &st->s[i];
    int k = index_of(s, id);
    if (k < 0) return 1;
    memmove(s->ids[k], s->ids[k + 1], sizeof(*s->ids) * (size_t)(s->n - k - 1));
    s->n--;
    return 0;
}

bool series_contains(const SeriesStore *st, int i, const char *id) {
    return valid(st, i) && id && index_of(&st->s[i], id) >= 0;
}

int series_set_sort(SeriesStore *st, int i, SeriesSort s) {
    if (!valid(st, i) || (s != SERIES_SORT_ALPHA && s != SERIES_SORT_MANUAL)) return -1;
    st->s[i].sort = s;
    return 0;
}

int series_set_list(SeriesStore *st, int i, bool list) {
    if (!valid(st, i)) return -1;
    st->s[i].list_view = list;
    return 0;
}

int series_visible(const SeriesStore *st, int i, const BookSummary *books, int nbooks, int *out) {
    if (!valid(st, i) || !out) return 0;
    const Series *s = &st->s[i];
    int k = 0;
    for (int j = 0; j < s->n; j++) {
        int b = books ? lib_find(books, nbooks, s->ids[j]) : -1;
        if (b >= 0) out[k++] = b;
    }
    if (s->sort == SERIES_SORT_ALPHA && k > 1) lib_sort(books, out, k, SORT_TITLE);
    return k;
}

int series_move(SeriesStore *st, int i, const char *id, int dir, const BookSummary *books, int nbooks) {
    if (!valid(st, i) || !id || !dir) return -1;
    Series *s = &st->s[i];
    int k = index_of(s, id);
    if (k < 0) return -1;
    int j = k;
    do { j += dir > 0 ? 1 : -1; } while (j >= 0 && j < s->n && books && lib_find(books, nbooks, s->ids[j]) < 0);
    if (j < 0 || j >= s->n) return 1;
    char tmp[40];
    memcpy(tmp, s->ids[k], sizeof tmp);
    memcpy(s->ids[k], s->ids[j], sizeof tmp);
    memcpy(s->ids[j], tmp, sizeof tmp);
    return 0;
}

static const SeriesStore *g_ord;
static int cmp_name(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    int c = str_natcasecmp(g_ord->s[x].name, g_ord->s[y].name);
    return c ? c : x - y;
}

int series_order(const SeriesStore *st, int *out) {
    if (!st || !out) return 0;
    for (int k = 0; k < st->n; k++) out[k] = k;
    g_ord = st;
    qsort(out, (size_t)st->n, sizeof *out, cmp_name);
    return st->n;
}

void series_free(SeriesStore *st) {
    if (!st) return;
    for (int k = 0; k < st->n; k++) free(st->s[k].ids);
    free(st->s);
    st->s = NULL;
    st->n = st->cap = 0;
}

/* ---- persistence ------------------------------------------------------------------ */

static int parse(SeriesStore *st, const char *text) {
    cJSON *root = cJSON_Parse(text);
    if (!root) return -1;
    cJSON *arr = cJSON_GetObjectItem(root, "series");
    if (!cJSON_IsObject(root) || !cJSON_IsArray(arr)) { cJSON_Delete(root); return -1; }
    cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        cJSON *nm = cJSON_GetObjectItem(it, "name");
        if (!cJSON_IsString(nm)) continue;
        char t[SERIES_NAME_MAX];
        trim_name(nm->valuestring, t, sizeof t);
        if (!t[0] || name_taken(st, t, -1)) continue;
        int i = push_series(st);
        if (i < 0) break;
        Series *s = &st->s[i];
        str_copy(s->name, sizeof s->name, t);
        cJSON *so = cJSON_GetObjectItem(it, "sort");
        s->sort = cJSON_IsString(so) && !strcmp(so->valuestring, "manual") ? SERIES_SORT_MANUAL : SERIES_SORT_ALPHA;
        cJSON *lv = cJSON_GetObjectItem(it, "listView");
        s->list_view = cJSON_IsTrue(lv);
        cJSON *cr = cJSON_GetObjectItem(it, "created");
        if (cJSON_IsNumber(cr) && cr->valuedouble > 0) s->created = (uint64_t)cr->valuedouble;
        cJSON *books = cJSON_GetObjectItem(it, "books");
        cJSON *b;
        cJSON_ArrayForEach(b, books) {
            if (cJSON_IsString(b) && b->valuestring[0] && strlen(b->valuestring) < 40 && index_of(s, b->valuestring) < 0)
                push_id(s, b->valuestring);
        }
    }
    cJSON_Delete(root);
    return 0;
}

int series_load(SeriesStore *st, const char *path) {
    if (!st || !path) return -1;
    memset(st, 0, sizeof *st);
    str_copy(st->path, sizeof st->path, path);
    FILE *f = fopen(path, "rb");
    if (!f) {
        /* Interrupted save (old file removed, rename not done): the complete temp file. */
        char tmp[340];
        snprintf(tmp, sizeof tmp, "%s.tmp", path);
        if (rename(tmp, path) == 0) { dbg("series: recovered '%s' from temp file", path); f = fopen(path, "rb"); }
    }
    if (!f) { dbg("series: no %s yet", path); return 0; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = sz >= 0 && sz < 4 * 1024 * 1024 ? malloc((size_t)sz + 1) : NULL;
    int ok = buf && fread(buf, 1, (size_t)sz, f) == (size_t)sz;
    fclose(f);
    if (ok) { buf[sz] = 0; ok = parse(st, buf) == 0; }
    free(buf);
    if (!ok) {
        series_free(st);
        char bad[340];
        snprintf(bad, sizeof bad, "%s.bad", path);
        remove(bad);
        rename(path, bad);
        dbg("series: '%s' unreadable/corrupt, kept as '%s', starting empty", path, bad);
        return 1;
    }
    dbg("series: loaded %d series from '%s'", st->n, path);
    return 0;
}

int series_save(const SeriesStore *st) {
    if (!st || !st->path[0]) return -1;
    cJSON *root = cJSON_CreateObject();
    if (!root) return -1;
    cJSON_AddNumberToObject(root, "version", SERIES_VER);
    cJSON *arr = cJSON_AddArrayToObject(root, "series");
    for (int k = 0; k < st->n; k++) {
        const Series *s = &st->s[k];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", s->name);
        cJSON_AddStringToObject(o, "sort", s->sort == SERIES_SORT_MANUAL ? "manual" : "alpha");
        cJSON_AddBoolToObject(o, "listView", s->list_view);
        cJSON_AddNumberToObject(o, "created", (double)s->created);
        cJSON *b = cJSON_AddArrayToObject(o, "books");
        for (int j = 0; j < s->n; j++) cJSON_AddItemToArray(b, cJSON_CreateString(s->ids[j]));
        cJSON_AddItemToArray(arr, o);
    }
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!text) return -1;
    char tmp[340];
    snprintf(tmp, sizeof tmp, "%s.tmp", st->path);
    FILE *f = fopen(tmp, "wb");
    size_t len = strlen(text);
    int ok = f && fwrite(text, 1, len, f) == len;
    if (f && fclose(f) != 0) ok = 0;
    free(text);
    if (ok) {
        remove(st->path);
        ok = rename(tmp, st->path) == 0;
    }
    if (!ok) remove(tmp);
    dbg("series: saved %d series to '%s'%s", st->n, st->path, ok ? "" : " FAILED");
    return ok ? 0 : -1;
}
