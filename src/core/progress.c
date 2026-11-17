#include "progress.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "dbg.h"
#include "strutil.h"

static char g_dir[256];

void progress_init(const char *dir) {
    str_copy(g_dir, sizeof g_dir, dir ? dir : "");
    if (g_dir[0]) mkdir(g_dir, 0777);
}

static void path_for(const char *id, char *out, size_t n) {
    snprintf(out, n, "%s/%s.json", g_dir, id);
}

int progress_load(const char *id, BookProgress *out) {
    memset(out, 0, sizeof *out);
    if (!id || !id[0] || !g_dir[0]) return -1;
    char path[300];
    path_for(id, path, sizeof path);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    char buf[1024];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    cJSON *o = cJSON_Parse(buf);
    if (!cJSON_IsObject(o)) { cJSON_Delete(o); return -1; }
    const cJSON *v;
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(o, "position"))) out->position = v->valuedouble;
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(o, "speed"))) out->speed = (float)v->valuedouble;
    if (cJSON_IsBool(v = cJSON_GetObjectItem(o, "finished"))) out->finished = cJSON_IsTrue(v);
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(o, "updated"))) out->updated_ms = (uint64_t)v->valuedouble;
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(o, "duration"))) out->duration = v->valuedouble;
    cJSON_Delete(o);
    return 0;
}

bool progress_duration_matches(double saved, double actual) {
    if (saved <= 0 || actual <= 0) return true;
    double tol = actual * 0.01;
    if (tol < 5) tol = 5;
    return fabs(saved - actual) <= tol;
}

int progress_load_for(const char *id, double duration, BookProgress *out) {
    if (progress_load(id, out) != 0) return -1;
    if (progress_duration_matches(out->duration, duration)) return 0;
    dbg("progress: %s saved for a %.0f s book, this one is %.0f s: starting from the beginning", id, out->duration, duration);
    memset(out, 0, sizeof *out);
    return -2;
}

int progress_save(const char *id, const BookProgress *p) {
    if (!id || !id[0] || !g_dir[0] || !p) return -1;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "position", p->position);
    cJSON_AddNumberToObject(o, "speed", p->speed);
    cJSON_AddBoolToObject(o, "finished", p->finished);
    cJSON_AddNumberToObject(o, "updated", (double)p->updated_ms);
    if (p->duration > 0) cJSON_AddNumberToObject(o, "duration", p->duration);
    char *s = cJSON_Print(o);
    cJSON_Delete(o);
    if (!s) return -1;
    char path[300], tmp[300];
    path_for(id, path, sizeof path);
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
