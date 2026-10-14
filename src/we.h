/* wallengine - a live wallpaper engine for X11 / OpenGL, written in C. */
#ifndef WE_H
#define WE_H

#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/glx.h>
#include <X11/Xlib.h>
#include <limits.h>
#include <stdbool.h>
#include <sys/types.h>
#include <time.h>

#define WE_VERSION   "1.0.0"
#define MAX_MONITORS 16
#define MAX_PLAYLIST 4096
#define AUDIO_BINS   512

typedef enum { MODE_DESKTOP, MODE_ROOT, MODE_WINDOW } WinMode;
typedef enum { FIT_COVER, FIT_CONTAIN, FIT_STRETCH, FIT_CENTER } FitMode;
typedef enum { WP_SHADER, WP_IMAGE, WP_VIDEO, WP_WORKSHOP } WpKind;
typedef enum { FX_NONE, FX_SNOW, FX_RAIN, FX_FIREFLIES, FX_BOKEH, FX__COUNT } Effect;
typedef enum { VIS_NONE, VIS_BARS, VIS_WAVE, VIS_CIRCLE, VIS__COUNT } Visualizer;

typedef struct {
    WinMode    mode;
    int        fps;
    bool       span;
    bool       different;         /* different wallpaper per monitor */
    char       playlist[PATH_MAX];
    int        interval;          /* seconds between playlist switches, 0 = never */
    bool       shuffle;
    float      transition;        /* crossfade seconds */
    bool       pause_fullscreen;
    bool       pause_maximized;
    bool       pause_battery;
    bool       audio;
    char       audio_cmd[1024];
    bool       video_sound;
    int        volume;
    int        video_max_width;
    FitMode    fit;
    float      parallax;
    Effect     effect;
    Visualizer vis;
    float      speed;
    char       monitor_wp[MAX_MONITORS][PATH_MAX];
    /* linux-wallpaperengine style per-screen settings (--screen-root/--bg/--scaling/--clamp) */
    int        nscreens;
    char       screen_name[MAX_MONITORS][64];
    char       screen_bg[MAX_MONITORS][PATH_MAX];
    int        screen_scaling[MAX_MONITORS], screen_clamp[MAX_MONITORS];
    char      *screen_playlist[MAX_MONITORS];
    char       span_list[512];          /* --screen-span a,b,c */
    char       span_bg[PATH_MAX];
    int        span_scaling, span_clamp;
    int        scaling, clamp;           /* defaults for workshop wallpapers */
    int        win_x, win_y;
    bool       win_pos;
    int        screenshot_delay;         /* frames */
    int        win_w, win_h;
    char       screenshot[PATH_MAX];
    float      duration;
    bool       verbose;
} Config;

extern Config cfg;

/* ---- util (main.c) ---- */
double now_sec(void);
void   logmsg(const char *fmt, ...);
void   dbg(const char *fmt, ...);
bool   in_path(const char *prog);
const char *effect_name(Effect e);
const char *vis_name(Visualizer v);
int    parse_effect(const char *s);
int    parse_vis(const char *s);
int    parse_fit(const char *s);

/* ---- video.c ---- */
typedef struct Video Video;
Video *video_open(const char *path, int w, int h, FitMode fit, int fps);
bool   video_update(Video *v, GLuint tex);   /* upload newest frame, true if new */
void   video_pause(Video *v, bool paused);
void   video_close(Video *v);
void   video_size(Video *v, int *w, int *h);

/* ---- audio.c ---- */
bool   audio_start(const char *cmd);
void   audio_update(void);
GLuint audio_texture(void);
float  audio_bass(void);
float  audio_level(void);
void   audio_stop(void);

/* ---- render.c ---- */
typedef struct WeWallpaper WeWallpaper;

typedef struct {
    WpKind     kind;
    WeWallpaper *we;               /* Wallpaper Engine project */
    char       path[PATH_MAX];     /* what the user selected */
    char       source[PATH_MAX];   /* media file for image/video */
    GLuint     prog;
    time_t     mtime;
    double     last_check;
    GLuint     tex;
    int        tw, th;
    Video     *video;
    FitMode    fit;
    float      parallax;
    Effect     effect;
    Visualizer vis;
    float      speed;
    float      tint[3];
    float      brightness, saturation;
    double     start, paused_at;
    bool       paused;
    int        frame;
    bool       own_fx, own_vis;    /* set by a .scene file, survives global changes */
} Wallpaper;

typedef struct {
    int        x, y, w, h;         /* in root coordinates */
    char       name[64];
    Wallpaper *cur, *prev;
    double     trans_start;
    double     last_switch;
    int        idx;
    bool       covered;
    int        scaling, clamp;     /* for Wallpaper Engine projects */
    char     **playlist;           /* per-screen playlist (NULL = global) */
    int        nplaylist;
    int        interval;
} Monitor;

bool       render_init(void);
Wallpaper *wp_load(const char *path, int mw, int mh, int scaling, int clamp);
void       wp_free(Wallpaper *w);
void       wp_pause(Wallpaper *w, bool paused, double now);
void       wp_tick(Wallpaper *w, double now);  /* video upload, shader hot-reload */
void       wp_draw(Wallpaper *w, const Monitor *m, int win_h, double now,
                   float alpha, const float mouse[4]);
bool       is_wallpaper_file(const char *path);
bool       save_screenshot(const char *path, int w, int h);

/* ---- x11.c ---- */
typedef struct {
    Display    *dpy;
    int         scr;
    Window      root, win;
    GLXContext  ctx;
    int         sw, sh;            /* root size */
    int         ww, wh;            /* drawable size */
    Atom        wm_delete;
    bool        closed;
    bool        reconfigure;
} XState;

bool x_init(XState *x, WinMode mode, int ww, int wh);
void x_shutdown(XState *x);
void x_poll(XState *x);
int  x_monitors(XState *x, Monitor *out, int max, WinMode mode, bool span);
bool x_fullscreen_rect(XState *x, bool maximized_too, int r[4]);
void x_pointer(XState *x, int *px, int *py, bool *down);
void x_swap(XState *x);
bool on_battery(void);

#endif
