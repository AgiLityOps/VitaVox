/* Content views and full-screen screens. */
#ifndef VITAVOX_VIEWS_H
#define VITAVOX_VIEWS_H
#include "app.h"
#include "groups.h"

View *view_home_new(void);
View *view_library_new(void);
View *view_groups_new(GroupKind kind);
/* a fixed list of books (a series or an author); idx is copied */
View *view_books_new(const char *title, const char *subtitle, const int *idx, int n, bool numbered, const double *seq);
View *view_search_new(bool open_keyboard);
View *view_settings_new(void);
View *view_book_new(const BookSummary *b); /* copied */
/* User-managed series (series.json) */
void series_ui_init(const char *path);
View *view_series_list_new(void);
View *view_series_pick_new(const BookSummary *b); /* "Add to Series" for book b (copied) */
/* Empty / error library state: path, files seen, book count. */
void draw_library_empty(float cx, float cy, bool focused);
/* Now Playing (full screen) */
void player_screen_enter(void);
void player_screen_update(const Input *in, float dt);
void player_screen_draw(void);


#endif
