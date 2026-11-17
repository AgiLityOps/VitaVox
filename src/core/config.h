/* Persistent settings (ux0:data/VitaVox/config.json). */
#ifndef VITAVOX_CONFIG_H
#define VITAVOX_CONFIG_H
#include <stdbool.h>

#define CFG_DEFAULT_LIBRARY "ux0:audiobooks"

typedef struct {
    char library_path[256];
    int skip_back, skip_fwd;   /* seconds */
    float speed;
    bool screen_off;           /* SELECT in the player turns the display off */
    bool list_view_library;    /* 00.39: Library list vs grid (persisted) */
    bool list_view_search;     /* 00.39: Search list vs grid (persisted) */
} Config;

void cfg_defaults(Config *c);
int cfg_load(Config *c, const char *path);
int cfg_save(const Config *c, const char *path);
float cfg_next_speed(float s);   /* 0.75 -> 1 -> 1.25 -> 1.5 -> 1.75 -> 2 -> 0.75 */
int cfg_next_skip(int s);        /* 10 -> 15 -> 30 -> 60 -> 10 */

#endif
