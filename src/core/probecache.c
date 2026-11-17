#include "probecache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "strutil.h"

static char g_dir[256];

void probecache_init(const char *dir) {
    str_copy(g_dir, sizeof g_dir, dir ? dir : "");
    if (g_dir[0]) mkdir(g_dir, 0777);
}

void probecache_ent_free(ProbeCacheEnt *e) {
    if (!e) return;
    free(e->chapters);
    free(e->description);
    e->chapters = NULL;
    e->description = NULL;
    e->nchapters = 0;
}

static void key_path(const char *src, char *out, size_t n) {
    uint64_t h = 14695981039346656037ULL;
    for (const unsigned char *p = (const unsigned char *)(src ? src : ""); *p; p++) {
        h ^= *p;
        h *= 1099511628211ULL;
    }
    snprintf(out, n, "%s/%016llx.json", g_dir, (unsigned long long)h);
}

static char *read_file(const char *fp) {
    FILE *f = fopen(fp, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0 || sz > 2 * 1024 * 1024) { fclose(f); return NULL; }
    rewind(f);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = 0;
    return buf;
}

int probecache_get(const char *path, uint64_t size, uint64_t mtime_ms, ProbeCacheEnt *out) {
    memset(out, 0, sizeof *out);
    if (!path || !path[0] || !g_dir[0]) return -1;
    char fp[300];
    key_path(path, fp, sizeof fp);
    char *buf = read_file(fp);
    if (!buf) return -1;
    cJSON *o = cJSON_Parse(buf);
    free(buf);
    if (!cJSON_IsObject(o)) { cJSON_Delete(o); return -1; }
    const cJSON *v;
    const cJSON *p = cJSON_GetObjectItem(o, "path");
    uint64_t sz = 0, mt = 0;
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(o, "size"))) sz = (uint64_t)v->valuedouble;
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(o, "mtime"))) mt = (uint64_t)v->valuedouble;
    if (!cJSON_IsString(p) || strcmp(p->valuestring, path) || sz != size || mt != mtime_ms) {
        cJSON_Delete(o);
        return -1;
    }
    str_copy(out->path, sizeof out->path, path);
    out->size = sz;
    out->mtime_ms = mt;
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(o, "v"))) out->ver = v->valueint;
    if (cJSON_IsString(v = cJSON_GetObjectItem(o, "title"))) str_copy(out->title, sizeof out->title, v->valuestring);
    if (cJSON_IsString(v = cJSON_GetObjectItem(o, "artist"))) str_copy(out->artist, sizeof out->artist, v->valuestring);
    if (cJSON_IsString(v = cJSON_GetObjectItem(o, "album"))) str_copy(out->album, sizeof out->album, v->valuestring);
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(o, "duration"))) out->duration = v->valuedouble;
    if (cJSON_IsBool(v = cJSON_GetObjectItem(o, "hasCover"))) out->has_cover = cJSON_IsTrue(v);
    if (cJSON_IsString(v = cJSON_GetObjectItem(o, "coverPath"))) str_copy(out->cover_path, sizeof out->cover_path, v->valuestring);
    if (cJSON_IsBool(v = cJSON_GetObjectItem(o, "durOnly"))) out->dur_only = cJSON_IsTrue(v);
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(o, "hdr"))) out->hdr = v->valueint;
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(o, "tags"))) out->tags = v->valueint;
    if (cJSON_IsString(v = cJSON_GetObjectItem(o, "description")) && v->valuestring[0])
        out->description = strdup(v->valuestring);
    const cJSON *chs = cJSON_GetObjectItem(o, "chapters");
    if (cJSON_IsArray(chs)) {
        int n = cJSON_GetArraySize(chs);
        if (n > 0 && n < 4096) {
            out->chapters = calloc((size_t)n, sizeof(Chapter));
            if (out->chapters) {
                out->nchapters = n;
                for (int i = 0; i < n; i++) {
                    const cJSON *c = cJSON_GetArrayItem(chs, i);
                    if (!cJSON_IsObject(c)) continue;
                    const cJSON *x;
                    if (cJSON_IsNumber(x = cJSON_GetObjectItem(c, "start"))) out->chapters[i].start = x->valuedouble;
                    if (cJSON_IsNumber(x = cJSON_GetObjectItem(c, "end"))) out->chapters[i].end = x->valuedouble;
                    if (cJSON_IsString(x = cJSON_GetObjectItem(c, "title")))
                        str_copy(out->chapters[i].title, sizeof out->chapters[i].title, x->valuestring);
                }
            }
        }
    }
    cJSON_Delete(o);
    return 0;
}

int probecache_put(const ProbeCacheEnt *e) {
    if (!e || !e->path[0] || !g_dir[0]) return -1;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "v", PROBE_CACHE_VER);
    cJSON_AddStringToObject(o, "path", e->path);
    cJSON_AddNumberToObject(o, "size", (double)e->size);
    cJSON_AddNumberToObject(o, "mtime", (double)e->mtime_ms);
    cJSON_AddStringToObject(o, "title", e->title);
    cJSON_AddStringToObject(o, "artist", e->artist);
    cJSON_AddStringToObject(o, "album", e->album);
    cJSON_AddNumberToObject(o, "duration", e->duration);
    cJSON_AddBoolToObject(o, "hasCover", e->has_cover);
    cJSON_AddStringToObject(o, "coverPath", e->cover_path);
    if (e->dur_only) cJSON_AddBoolToObject(o, "durOnly", 1);
    if (e->dur_only && e->hdr) {
        cJSON_AddNumberToObject(o, "hdr", e->hdr);
        cJSON_AddBoolToObject(o, "exact", e->hdr == PROBE_HDR_EXACT);
    }
    if (!e->dur_only) cJSON_AddNumberToObject(o, "tags", e->tags);
    if (e->description && e->description[0]) cJSON_AddStringToObject(o, "description", e->description);
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < e->nchapters; i++) {
        if (!e->chapters) break;
        cJSON *c = cJSON_CreateObject();
        cJSON_AddNumberToObject(c, "start", e->chapters[i].start);
        cJSON_AddNumberToObject(c, "end", e->chapters[i].end);
        cJSON_AddStringToObject(c, "title", e->chapters[i].title);
        cJSON_AddItemToArray(arr, c);
    }
    cJSON_AddItemToObject(o, "chapters", arr);
    char *s = cJSON_Print(o);
    cJSON_Delete(o);
    if (!s) return -1;
    char path[300], tmp[300];
    key_path(e->path, path, sizeof path);
    if (e->dur_only) {
        /* Many small records while a big book is opened: one create+write, no
         * tmp/rename (a torn record does not parse and is just a miss). */
        FILE *df = fopen(path, "wb");
        int drc = -1;
        if (df) {
            drc = fputs(s, df) >= 0 ? 0 : -1;
            if (fclose(df) != 0) drc = -1;
        }
        free(s);
        return drc;
    }
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    remove(tmp);
    FILE *f = fopen(tmp, "wb");
    int rc = -1;
    if (f) {
        rc = fputs(s, f) >= 0 ? 0 : -1;
        if (fclose(f) != 0) rc = -1;
        if (rc == 0) { remove(path); rc = rename(tmp, path); }
        else remove(tmp);
    }
    free(s);
    return rc;
}
