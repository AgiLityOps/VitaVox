#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "strutil.h"

void cfg_defaults(Config *c) {
    memset(c, 0, sizeof *c);
    str_copy(c->library_path, sizeof c->library_path, CFG_DEFAULT_LIBRARY);
    c->skip_back = 15;
    c->skip_fwd = 30;
    c->speed = 1.0f;
    c->screen_off = true;
    c->list_view_library = false;
    c->list_view_search = false;
}

static const int kSkips[] = {10, 15, 30, 60};
static const float kSpeeds[] = {0.75f, 1.0f, 1.25f, 1.5f, 1.75f, 2.0f};

static int clamp_skip(int s) {
    for (size_t i = 0; i < 4; i++) if (kSkips[i] == s) return s;
    return 15;
}

float cfg_next_speed(float s) {
    for (size_t i = 0; i < 6; i++)
        if (s < kSpeeds[i] - 0.01f) return kSpeeds[i];
    return kSpeeds[0];
}

int cfg_next_skip(int s) {
    for (size_t i = 0; i < 4; i++)
        if (kSkips[i] > s) return kSkips[i];
    return kSkips[0];
}

static void get_str(const cJSON *o, const char *k, char *dst, size_t n) {
    const cJSON *v = cJSON_GetObjectItem(o, k);
    if (cJSON_IsString(v)) str_copy(dst, n, v->valuestring);
}

int cfg_load(Config *c, const char *path) {
    cfg_defaults(c);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    char buf[2048];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    cJSON *o = cJSON_Parse(buf);
    if (!cJSON_IsObject(o)) { cJSON_Delete(o); return -1; }
    get_str(o, "libraryPath", c->library_path, sizeof c->library_path);
    const cJSON *v;
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(o, "skipBack"))) c->skip_back = clamp_skip(v->valueint);
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(o, "skipFwd"))) c->skip_fwd = clamp_skip(v->valueint);
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(o, "speed"))) {
        float s = (float)v->valuedouble;
        c->speed = s < 0.75f ? 0.75f : s > 2.0f ? 2.0f : s;
    }
    if (cJSON_IsBool(v = cJSON_GetObjectItem(o, "screenOff"))) c->screen_off = cJSON_IsTrue(v);
    if (cJSON_IsBool(v = cJSON_GetObjectItem(o, "listViewLibrary"))) c->list_view_library = cJSON_IsTrue(v);
    if (cJSON_IsBool(v = cJSON_GetObjectItem(o, "listViewSearch"))) c->list_view_search = cJSON_IsTrue(v);
    if (!c->library_path[0]) str_copy(c->library_path, sizeof c->library_path, CFG_DEFAULT_LIBRARY);
    cJSON_Delete(o);
    return 0;
}

int cfg_save(const Config *c, const char *path) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "libraryPath", c->library_path);
    cJSON_AddNumberToObject(o, "skipBack", c->skip_back);
    cJSON_AddNumberToObject(o, "skipFwd", c->skip_fwd);
    cJSON_AddNumberToObject(o, "speed", c->speed);
    cJSON_AddBoolToObject(o, "screenOff", c->screen_off);
    cJSON_AddBoolToObject(o, "listViewLibrary", c->list_view_library);
    cJSON_AddBoolToObject(o, "listViewSearch", c->list_view_search);
    char *s = cJSON_Print(o);
    cJSON_Delete(o);
    if (!s) return -1;
    char tmp[300];
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
