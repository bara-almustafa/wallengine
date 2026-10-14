/* Public API of the Wallpaper Engine (Steam workshop) compatibility layer. */
#ifndef WALLPAPER_ENGINE_H
#define WALLPAPER_ENGINE_H

#include <GL/gl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

typedef struct WeWallpaper WeWallpaper;

/* UV scaling modes (--scaling) */
enum { WE_SCALE_DEFAULT, WE_SCALE_STRETCH, WE_SCALE_FIT, WE_SCALE_FILL };
/* clamp modes (--clamp) */
enum { WE_CLAMP_CLAMP, WE_CLAMP_BORDER, WE_CLAMP_REPEAT };

/* Turn a workshop id, a project directory or a project.json path into a project directory. */
bool         we_resolve(const char *id_or_path, char *out, size_t n);
bool         we_is_project(const char *id_or_path);
/* Wallpaper Engine install / assets detection. */
bool         we_find_assets(char *out, size_t n);
bool         we_find_install(char *out, size_t n);
/* Read a playlist from Wallpaper Engine's config.json: fills paths, returns count. */
int          we_playlist(const char *name, char ***items, int *delay_minutes, bool *random);

void         we_global_init(void);
void         we_global_frame(double now);                 /* audio spectrum, media, timers */
void         we_global_shutdown(void);

WeWallpaper *we_open(const char *dir, int out_w, int out_h, int scaling, int clamp);
void         we_close(WeWallpaper *w);
/* Render one frame and draw it into the currently bound framebuffer at the viewport.
 * mouse: pointer position relative to the viewport, 0..1, y from the top; down = button. */
void         we_draw(WeWallpaper *w, int vx, int vy, int vw, int vh, float alpha, float mx, float my, bool down);
void         we_pause(WeWallpaper *w, bool paused);
const char  *we_title(const WeWallpaper *w);
const char  *we_kind(const WeWallpaper *w);
bool         we_set_property(WeWallpaper *w, const char *name, const char *value);
void         we_list_properties(const WeWallpaper *w, FILE *f);
void         we_dump(const WeWallpaper *w);
int          we_list_properties_path(const char *path);
/* Apply --set-property overrides stored in we_settings to a loaded wallpaper. */
void         we_apply_settings(WeWallpaper *w);

/* settings (filled from the command line) */
void we_opt_disable_particles(bool v);
void we_opt_disable_mouse(bool v);
void we_opt_disable_parallax(bool v);
void we_opt_audio(bool silent, int volume, bool automute, bool processing);
void we_opt_assets(const char *dir);
void we_opt_property(const char *name, const char *value);
void we_opt_dump(bool v);
void we_opt_render_debug(const char *spec);

#endif
