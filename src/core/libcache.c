#include "libcache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "dbg.h"
#include "strutil.h"

void libcache_free(LibraryCache *c) {
    if (!c) return;
    for (int i = 0; i < c->n; i++) {
        free(c->books[i].tracks);
        free(c->books[i].chapters);
    }
    free(c->books);
    memset(c, 0, sizeof *c);
}

int libcache_clone(const LibraryCache *src, LibraryCache *dst) {
    memset(dst, 0, sizeof *dst);
    if (!src) return -1;
    *dst = *src;
    dst->books = NULL;
    dst->n = 0;
    if (src->n <= 0) return 0;
    dst->books = calloc((size_t)src->n, sizeof(CachedBook));
    if (!dst->books) return -1;
    for (int i = 0; i < src->n; i++) {
        const CachedBook *a = &src->books[i];
        CachedBook *b = &dst->books[i];
        *b = *a;
        b->tracks = NULL;
        b->chapters = NULL;
        b->ntracks = b->nchapters = 0;
        dst->n = i + 1;
        if (a->ntracks > 0) {
            b->tracks = malloc(sizeof(Track) * (size_t)a->ntracks);
            if (!b->tracks) { libcache_free(dst); return -1; }
            memcpy(b->tracks, a->tracks, sizeof(Track) * (size_t)a->ntracks);
            b->ntracks = a->ntracks;
        }
        if (a->nchapters > 0) {
            b->chapters = malloc(sizeof(Chapter) * (size_t)a->nchapters);
            if (!b->chapters) { libcache_free(dst); return -1; }
            memcpy(b->chapters, a->chapters, sizeof(Chapter) * (size_t)a->nchapters);
            b->nchapters = a->nchapters;
        }
    }
    return 0;
}

int libcache_find(const LibraryCache *c, const char *id) {
    if (!c || !id) return -1;
    for (int i = 0; i < c->n; i++)
        if (!strcmp(c->books[i].sum.id, id)) return i;
    return -1;
}

int libcache_detail(const LibraryCache *c, const char *id, BookDetail *out) {
    memset(out, 0, sizeof *out);
    int i = libcache_find(c, id);
    if (i < 0) return -1;
    const CachedBook *b = &c->books[i];
    str_copy(out->id, sizeof out->id, b->sum.id);
    str_copy(out->title, sizeof out->title, b->sum.title);
    str_copy(out->author, sizeof out->author, b->sum.author);
    out->duration = b->sum.duration;
    out->ntracks = b->ntracks;
    out->nchapters = b->nchapters;
    if (b->ntracks > 0) {
        out->tracks = malloc(sizeof(Track) * (size_t)b->ntracks);
        if (!out->tracks) return -1;
        memcpy(out->tracks, b->tracks, sizeof(Track) * (size_t)b->ntracks);
    }
    if (b->nchapters > 0) {
        out->chapters = malloc(sizeof(Chapter) * (size_t)b->nchapters);
        if (!out->chapters) { free(out->tracks); out->tracks = NULL; return -1; }
        memcpy(out->chapters, b->chapters, sizeof(Chapter) * (size_t)b->nchapters);
    }
    return 0;
}

static cJSON *track_json(const Track *t) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "index", t->index);
    cJSON_AddStringToObject(o, "path", t->path);
    cJSON_AddStringToObject(o, "title", t->title);
    cJSON_AddStringToObject(o, "ext", t->ext);
    cJSON_AddStringToObject(o, "codec", t->codec);
    cJSON_AddNumberToObject(o, "start", t->start_offset);
    cJSON_AddNumberToObject(o, "duration", t->duration);
    cJSON_AddNumberToObject(o, "size", (double)t->size);
    return o;
}

static cJSON *chapter_json(const Chapter *c) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "start", c->start);
    cJSON_AddNumberToObject(o, "end", c->end);
    cJSON_AddStringToObject(o, "title", c->title);
    return o;
}

static cJSON *book_json(const CachedBook *b) {
    const BookSummary *s = &b->sum;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "id", s->id);
    cJSON_AddStringToObject(o, "title", s->title);
    cJSON_AddStringToObject(o, "author", s->author);
    cJSON_AddStringToObject(o, "series", s->series);
    cJSON_AddStringToObject(o, "path", s->path);
    cJSON_AddStringToObject(o, "cover", s->cover_path);
    cJSON_AddStringToObject(o, "thumb", s->thumb_path);
    cJSON_AddNumberToObject(o, "duration", s->duration);
    cJSON_AddNumberToObject(o, "added_at", (double)s->added_at);
    cJSON_AddNumberToObject(o, "progress", s->progress);
    cJSON_AddNumberToObject(o, "current_time", s->current_time);
    cJSON_AddBoolToObject(o, "finished", s->finished);
    cJSON_AddNumberToObject(o, "progress_updated", (double)s->progress_updated);
    cJSON *tracks = cJSON_AddArrayToObject(o, "tracks");
    for (int i = 0; i < b->ntracks; i++) cJSON_AddItemToArray(tracks, track_json(&b->tracks[i]));
    cJSON *chs = cJSON_AddArrayToObject(o, "chapters");
    for (int i = 0; i < b->nchapters; i++) cJSON_AddItemToArray(chs, chapter_json(&b->chapters[i]));
    return o;
}

int libcache_save(const char *path, const LibraryCache *c) {
    if (!path || !path[0] || !c) return -1;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "version", LIBCACHE_VERSION);
    cJSON_AddStringToObject(o, "root", c->root);
    cJSON_AddNumberToObject(o, "saved_at", (double)c->saved_at);
    if (c->partial) cJSON_AddBoolToObject(o, "partial", 1);
    cJSON *books = cJSON_AddArrayToObject(o, "books");
    for (int i = 0; i < c->n; i++) cJSON_AddItemToArray(books, book_json(&c->books[i]));
    char *s = cJSON_Print(o);
    cJSON_Delete(o);
    if (!s) return -1;
    char tmp[340];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    remove(tmp);
    FILE *f = fopen(tmp, "wb");
    int rc = -1;
    if (f) {
        size_t n = strlen(s);
        rc = fwrite(s, 1, n, f) == n ? 0 : -1;
        if (fclose(f) != 0) rc = -1;
        if (rc == 0) { remove(path); rc = rename(tmp, path) == 0 ? 0 : -1; }
        else remove(tmp);
    }
    free(s);
    if (rc == 0) dbg("libcache: saved %d books to '%s'%s", c->n, path, c->partial ? " (scan still running)" : "");
    else dbg("libcache: save failed for '%s'", path);
    return rc;
}

static void load_track(cJSON *o, Track *t) {
    memset(t, 0, sizeof *t);
    const cJSON *v;
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(o, "index"))) t->index = v->valueint;
    if (cJSON_IsString(v = cJSON_GetObjectItem(o, "path")) && v->valuestring)
        str_copy(t->path, sizeof t->path, v->valuestring);
    if (cJSON_IsString(v = cJSON_GetObjectItem(o, "title")) && v->valuestring)
        str_copy(t->title, sizeof t->title, v->valuestring);
    if (cJSON_IsString(v = cJSON_GetObjectItem(o, "ext")) && v->valuestring)
        str_copy(t->ext, sizeof t->ext, v->valuestring);
    if (cJSON_IsString(v = cJSON_GetObjectItem(o, "codec")) && v->valuestring)
        str_copy(t->codec, sizeof t->codec, v->valuestring);
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(o, "start"))) t->start_offset = v->valuedouble;
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(o, "duration"))) t->duration = v->valuedouble;
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(o, "size"))) t->size = (uint64_t)v->valuedouble;
}

static void load_chapter(cJSON *o, Chapter *c) {
    memset(c, 0, sizeof *c);
    const cJSON *v;
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(o, "start"))) c->start = v->valuedouble;
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(o, "end"))) c->end = v->valuedouble;
    if (cJSON_IsString(v = cJSON_GetObjectItem(o, "title")) && v->valuestring)
        str_copy(c->title, sizeof c->title, v->valuestring);
}

static int load_book(cJSON *o, CachedBook *b) {
    memset(b, 0, sizeof *b);
    BookSummary *s = &b->sum;
    const cJSON *v;
    if (cJSON_IsString(v = cJSON_GetObjectItem(o, "id")) && v->valuestring)
        str_copy(s->id, sizeof s->id, v->valuestring);
    if (cJSON_IsString(v = cJSON_GetObjectItem(o, "title")) && v->valuestring)
        str_copy(s->title, sizeof s->title, v->valuestring);
    if (cJSON_IsString(v = cJSON_GetObjectItem(o, "author")) && v->valuestring)
        str_copy(s->author, sizeof s->author, v->valuestring);
    if (cJSON_IsString(v = cJSON_GetObjectItem(o, "series")) && v->valuestring)
        str_copy(s->series, sizeof s->series, v->valuestring);
    if (cJSON_IsString(v = cJSON_GetObjectItem(o, "path")) && v->valuestring)
        str_copy(s->path, sizeof s->path, v->valuestring);
    if (cJSON_IsString(v = cJSON_GetObjectItem(o, "cover")) && v->valuestring)
        str_copy(s->cover_path, sizeof s->cover_path, v->valuestring);
    if (cJSON_IsString(v = cJSON_GetObjectItem(o, "thumb")) && v->valuestring)
        str_copy(s->thumb_path, sizeof s->thumb_path, v->valuestring);
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(o, "duration"))) s->duration = v->valuedouble;
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(o, "added_at"))) s->added_at = (uint64_t)v->valuedouble;
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(o, "progress"))) s->progress = (float)v->valuedouble;
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(o, "current_time"))) s->current_time = v->valuedouble;
    if (cJSON_IsBool(v = cJSON_GetObjectItem(o, "finished"))) s->finished = cJSON_IsTrue(v);
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(o, "progress_updated")))
        s->progress_updated = (uint64_t)v->valuedouble;
    cJSON *tracks = cJSON_GetObjectItem(o, "tracks");
    if (cJSON_IsArray(tracks)) {
        int n = cJSON_GetArraySize(tracks);
        if (n > 0) {
            b->tracks = calloc((size_t)n, sizeof(Track));
            if (!b->tracks) return -1;
            b->ntracks = n;
            for (int i = 0; i < n; i++) load_track(cJSON_GetArrayItem(tracks, i), &b->tracks[i]);
        }
    }
    cJSON *chs = cJSON_GetObjectItem(o, "chapters");
    if (cJSON_IsArray(chs)) {
        int n = cJSON_GetArraySize(chs);
        if (n > 0) {
            b->chapters = calloc((size_t)n, sizeof(Chapter));
            if (!b->chapters) return -1;
            b->nchapters = n;
            for (int i = 0; i < n; i++) load_chapter(cJSON_GetArrayItem(chs, i), &b->chapters[i]);
        }
    }
    return s->id[0] && s->path[0] ? 0 : -1;
}

int libcache_load(const char *path, LibraryCache *out) {
    memset(out, 0, sizeof *out);
    if (!path || !path[0]) return -1;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long sz = ftell(f);
    if (sz <= 0 || sz > 16 * 1024 * 1024) { fclose(f); return -1; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return -1; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = 0;
    cJSON *o = cJSON_Parse(buf);
    free(buf);
    if (!cJSON_IsObject(o)) { cJSON_Delete(o); return -1; }
    const cJSON *v;
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(o, "version"))) out->version = v->valueint;
    if (out->version != LIBCACHE_VERSION) {
        dbg("libcache: unsupported version %d in '%s'", out->version, path);
        cJSON_Delete(o);
        return -1;
    }
    if (cJSON_IsString(v = cJSON_GetObjectItem(o, "root")) && v->valuestring)
        str_copy(out->root, sizeof out->root, v->valuestring);
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(o, "saved_at")))
        out->saved_at = (uint64_t)v->valuedouble;
    if (cJSON_IsBool(v = cJSON_GetObjectItem(o, "partial"))) out->partial = cJSON_IsTrue(v);
    cJSON *books = cJSON_GetObjectItem(o, "books");
    int rc = 0;
    if (cJSON_IsArray(books)) {
        int nb = cJSON_GetArraySize(books);
        if (nb > 0) {
            out->books = calloc((size_t)nb, sizeof(CachedBook));
            if (!out->books) rc = -1;
            else {
                int k = 0;
                for (int i = 0; i < nb; i++) {
                    if (load_book(cJSON_GetArrayItem(books, i), &out->books[k]) == 0) k++;
                    else { free(out->books[k].tracks); free(out->books[k].chapters); memset(&out->books[k], 0, sizeof out->books[k]); }
                }
                out->n = k;
            }
        }
    }
    cJSON_Delete(o);
    if (rc != 0) { libcache_free(out); return -1; }
    dbg("libcache: loaded %d books from '%s'%s", out->n, path, out->partial ? " (partial: first scan was interrupted)" : "");
    return 0;
}
