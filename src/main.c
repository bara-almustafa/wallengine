/* wallengine - live wallpaper engine: entry point, config, playlist, IPC, main loop. */
#include "we.h"
#include "we/wallpaper_engine.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#ifndef SAMPLES_DIR
#define SAMPLES_DIR "samples"
#endif

Config cfg = {
    .mode = MODE_DESKTOP,
    .fps = 30,
    .interval = 0,
    .transition = 1.5f,
    .pause_fullscreen = true,
    .audio = true,
    .volume = 50,
    .video_max_width = 1920,
    .fit = FIT_COVER,
    .parallax = 0.02f,
    .speed = 1.0f,
    .win_w = 1280,
    .win_h = 720,
};

/* ------------------------------------------------------------------ util */

double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

void logmsg(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("wallengine: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

void dbg(const char *fmt, ...)
{
    if (!cfg.verbose) return;
    va_list ap;
    va_start(ap, fmt);
    fputs("wallengine: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

bool in_path(const char *prog)
{
    const char *path = getenv("PATH");
    if (!path) return false;
    char buf[PATH_MAX];
    for (const char *p = path; *p;) {
        const char *c = strchr(p, ':');
        size_t n = c ? (size_t)(c - p) : strlen(p);
        snprintf(buf, sizeof buf, "%.*s/%s", (int)n, p, prog);
        if (access(buf, X_OK) == 0) return true;
        if (!c) break;
        p = c + 1;
    }
    return false;
}

static const char *EFFECTS[] = { "none", "snow", "rain", "fireflies", "bokeh" };
static const char *VISES[]   = { "none", "bars", "wave", "circle" };
static const char *FITS[]    = { "cover", "contain", "stretch", "center" };

const char *effect_name(Effect e) { return EFFECTS[e]; }
const char *vis_name(Visualizer v) { return VISES[v]; }

static int lookup(const char *s, const char **names, int n)
{
    for (int i = 0; i < n; i++)
        if (!strcasecmp(s, names[i])) return i;
    return -1;
}
int parse_effect(const char *s) { return lookup(s, EFFECTS, FX__COUNT); }
int parse_vis(const char *s) { return lookup(s, VISES, VIS__COUNT); }
int parse_fit(const char *s) { return lookup(s, FITS, 4); }

static bool truthy(const char *v)
{
    return !strcasecmp(v, "1") || !strcasecmp(v, "true") || !strcasecmp(v, "yes") ||
           !strcasecmp(v, "on");
}

static void expand_home(char *out, size_t n, const char *in)
{
    if (in[0] == '~' && (in[1] == '/' || !in[1])) snprintf(out, n, "%s%s", getenv("HOME"), in + 1);
    else snprintf(out, n, "%s", in);
}

static char *trim(char *s)
{
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

/* --------------------------------------------------------------- config */

/* Wallpaper Engine sound settings (linux-wallpaperengine semantics, volume 0..128) */
static bool we_silent, we_automute = true, we_processing = true;
static int  we_volume = 15;

static int parse_scaling(const char *v)
{
    return !strcmp(v, "stretch") ? WE_SCALE_STRETCH : !strcmp(v, "fit") ? WE_SCALE_FIT
         : !strcmp(v, "fill") ? WE_SCALE_FILL : !strcmp(v, "default") ? WE_SCALE_DEFAULT : -1;
}

static int parse_clamp(const char *v)
{
    return !strcmp(v, "clamp") ? WE_CLAMP_CLAMP : !strcmp(v, "border") ? WE_CLAMP_BORDER
         : !strcmp(v, "repeat") ? WE_CLAMP_REPEAT : -1;
}

static bool set_option(const char *k, const char *v)
{
    int x;
    if (!strcmp(k, "mode")) {
        if (!strcmp(v, "desktop")) cfg.mode = MODE_DESKTOP;
        else if (!strcmp(v, "root")) cfg.mode = MODE_ROOT;
        else if (!strcmp(v, "window")) cfg.mode = MODE_WINDOW;
        else return false;
    } else if (!strcmp(k, "fps")) cfg.fps = atoi(v) > 0 ? atoi(v) : 30;
    else if (!strcmp(k, "span")) cfg.span = truthy(v);
    else if (!strcmp(k, "different")) cfg.different = truthy(v);
    else if (!strcmp(k, "playlist")) expand_home(cfg.playlist, sizeof cfg.playlist, v);
    else if (!strcmp(k, "interval")) cfg.interval = atoi(v);
    else if (!strcmp(k, "shuffle")) cfg.shuffle = truthy(v);
    else if (!strcmp(k, "transition")) cfg.transition = atof(v);
    else if (!strcmp(k, "pause_on_fullscreen")) cfg.pause_fullscreen = truthy(v);
    else if (!strcmp(k, "pause_on_maximized")) cfg.pause_maximized = truthy(v);
    else if (!strcmp(k, "pause_on_battery")) cfg.pause_battery = truthy(v);
    else if (!strcmp(k, "audio")) cfg.audio = truthy(v);
    else if (!strcmp(k, "audio_cmd")) snprintf(cfg.audio_cmd, sizeof cfg.audio_cmd, "%s", v);
    else if (!strcmp(k, "video_sound")) cfg.video_sound = truthy(v);
    else if (!strcmp(k, "volume")) cfg.volume = atoi(v);
    else if (!strcmp(k, "video_max_width")) cfg.video_max_width = atoi(v);
    else if (!strcmp(k, "fit")) { if ((x = parse_fit(v)) < 0) return false; cfg.fit = x; }
    else if (!strcmp(k, "effect")) { if ((x = parse_effect(v)) < 0) return false; cfg.effect = x; }
    else if (!strcmp(k, "visualizer")) { if ((x = parse_vis(v)) < 0) return false; cfg.vis = x; }
    else if (!strcmp(k, "parallax")) cfg.parallax = atof(v);
    else if (!strcmp(k, "speed")) cfg.speed = atof(v);
    else if (!strcmp(k, "window_size")) sscanf(v, "%dx%d", &cfg.win_w, &cfg.win_h);
    else if (!strcmp(k, "scaling")) { if ((x = parse_scaling(v)) < 0) return false; cfg.scaling = x; }
    else if (!strcmp(k, "clamp")) { if ((x = parse_clamp(v)) < 0) return false; cfg.clamp = x; }
    else if (!strcmp(k, "assets_dir")) { char d[PATH_MAX]; expand_home(d, sizeof d, v); we_opt_assets(d); }
    else if (!strcmp(k, "silent")) we_silent = truthy(v);
    else if (!strcmp(k, "automute")) we_automute = truthy(v);
    else if (!strcmp(k, "audio_processing")) we_processing = truthy(v);
    else if (!strcmp(k, "wallpaper_volume")) we_volume = atoi(v);
    else if (!strcmp(k, "disable_particles")) we_opt_disable_particles(truthy(v));
    else if (!strcmp(k, "disable_mouse")) we_opt_disable_mouse(truthy(v));
    else if (!strcmp(k, "disable_parallax")) we_opt_disable_parallax(truthy(v));
    else if (!strncmp(k, "monitor", 7) && isdigit((unsigned char)k[7])) {
        int i = atoi(k + 7);
        if (i < 0 || i >= MAX_MONITORS) return false;
        expand_home(cfg.monitor_wp[i], PATH_MAX, v);
    } else return false;
    return true;
}

static void load_config(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[2048];
    int ln = 0;
    while (fgets(line, sizeof line, f)) {
        ln++;
        char *s = trim(line);
        if (!*s || *s == '#' || *s == ';' || *s == '[') continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        char *hash = strstr(eq + 1, " #");
        if (hash) *hash = 0;   /* inline comment */
        char *k = trim(s), *v = trim(eq + 1);
        if (!set_option(k, v)) logmsg("%s:%d: bad option '%s = %s'", path, ln, k, v);
    }
    fclose(f);
    dbg("loaded config %s", path);
}

/* -------------------------------------------------------------- playlist */

static char *playlist[MAX_PLAYLIST];
static int   nplay;

static int cmpstr(const void *a, const void *b) { return strcmp(*(char **)a, *(char **)b); }

static void add_path(const char *p, int depth)
{
    struct stat st;
    char wdir[PATH_MAX];
    if (stat(p, &st) < 0) {
        /* a Steam workshop id */
        if (we_resolve(p, wdir, sizeof wdir) && nplay < MAX_PLAYLIST) {
            playlist[nplay++] = strdup(wdir);
            return;
        }
        logmsg("not found: %s", p);
        return;
    }
    if (S_ISDIR(st.st_mode) && we_is_project(p)) {
        /* a Wallpaper Engine project folder is one wallpaper */
        if (nplay < MAX_PLAYLIST && we_resolve(p, wdir, sizeof wdir)) playlist[nplay++] = strdup(wdir);
        return;
    }
    if (S_ISDIR(st.st_mode)) {
        if (depth > 4) return;
        DIR *d = opendir(p);
        if (!d) return;
        int first = nplay;
        struct dirent *e;
        char sub[PATH_MAX];
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.' || !strcmp(e->d_name, "assets")) continue;
            snprintf(sub, sizeof sub, "%s/%s", p, e->d_name);
            add_path(sub, depth + 1);
        }
        closedir(d);
        qsort(playlist + first, nplay - first, sizeof(char *), cmpstr);
        return;
    }
    if (!is_wallpaper_file(p) || nplay >= MAX_PLAYLIST) return;
    /* a scene's own image shouldn't also show up as a plain image */
    if (depth > 0 && !strstr(p, ".scene")) {
        char scene[PATH_MAX];
        snprintf(scene, sizeof scene, "%.*s.scene", (int)(strrchr(p, '.') - p), p);
        if (!access(scene, R_OK)) return;
    }
    char *r = realpath(p, NULL);
    playlist[nplay++] = r ? r : strdup(p);
}

static void build_playlist(int argc, char **argv, int first)
{
    for (int i = first; i < argc; i++) add_path(argv[i], 0);
    if (nplay == 0 && cfg.playlist[0]) {
        char *s = strdup(cfg.playlist);
        for (char *tok = strtok(s, ","); tok; tok = strtok(NULL, ",")) {
            char p[PATH_MAX];
            expand_home(p, sizeof p, trim(tok));
            add_path(p, 0);
        }
        free(s);
    }
    if (nplay == 0) {
        /* bundled samples: build-time path, else next to the binary (release tarballs) */
        char exe[PATH_MAX], cand[PATH_MAX + 64];
        struct stat st;
        if (stat(SAMPLES_DIR, &st) == 0) {
            add_path(SAMPLES_DIR, 0);
        } else {
            ssize_t len = readlink("/proc/self/exe", exe, sizeof exe - 1);
            if (len > 0) {
                exe[len] = 0;
                *strrchr(exe, '/') = 0;
                const char *rel[] = { "samples", "../share/wallengine/samples" };
                for (int k = 0; k < 2 && nplay == 0; k++) {
                    snprintf(cand, sizeof cand, "%s/%s", exe, rel[k]);
                    if (stat(cand, &st) == 0) add_path(cand, 0);
                }
            }
        }
    }
    if (cfg.shuffle) {
        srand(time(NULL) ^ getpid());
        for (int i = nplay - 1; i > 0; i--) {
            int j = rand() % (i + 1);
            char *t = playlist[i]; playlist[i] = playlist[j]; playlist[j] = t;
        }
    }
    dbg("playlist: %d wallpapers", nplay);
}

/* ------------------------------------------------------------ state */

static XState  X;
static Monitor mons[MAX_MONITORS];
static int     nmon;
static bool    user_paused;
static volatile sig_atomic_t quit_flag;
static char    pending_shot[PATH_MAX];

static void on_signal(int s) { (void)s; quit_flag = 1; }

static bool switch_to(Monitor *m, const char *path, double now)
{
    Wallpaper *w = wp_load(path, m->w, m->h, m->scaling, m->clamp);
    if (!w) return false;
    if (m->prev) wp_free(m->prev);
    m->prev = NULL;
    if (m->cur && cfg.transition > 0) m->prev = m->cur;
    else if (m->cur) wp_free(m->cur);
    m->cur = w;
    m->trans_start = now;
    m->last_switch = now;
    logmsg("[%s] %s", m->name, path);
    return true;
}

static void advance(Monitor *m, int dir, double now)
{
    char **pl = m->playlist ? m->playlist : playlist;
    int n = m->playlist ? m->nplaylist : nplay;
    if (n == 0) return;
    for (int tries = 0; tries < n; tries++) {
        m->idx = ((m->idx + dir) % n + n) % n;
        if (switch_to(m, pl[m->idx], now)) return;
    }
    m->last_switch = now;
}

static int screen_index(const char *name)
{
    for (int i = 0; i < cfg.nscreens; i++)
        if (!strcmp(cfg.screen_name[i], name)) return i;
    return -1;
}

static void load_screen_playlist(Monitor *m, const char *name)
{
    char **items = NULL;
    int delay = 60;
    bool random = false;
    int n = we_playlist(name, &items, &delay, &random);
    if (!n) return;
    if (random) {
        for (int i = n - 1; i > 0; i--) {
            int j = rand() % (i + 1);
            char *t = items[i]; items[i] = items[j]; items[j] = t;
        }
    }
    m->playlist = items;
    m->nplaylist = n;
    m->interval = (delay > 0 ? delay : 1) * 60;
}

static void setup_monitors(double now)
{
    for (int i = 0; i < nmon; i++) {
        wp_free(mons[i].cur);
        wp_free(mons[i].prev);
    }
    for (int i = 0; i < nmon; i++) {
        for (int k = 0; k < mons[i].nplaylist; k++) free(mons[i].playlist[k]);
        free(mons[i].playlist);
    }
    nmon = x_monitors(&X, mons, MAX_MONITORS, cfg.mode, cfg.span);
    /* --screen-span a,b: merge the listed monitors into one */
    if (cfg.span_list[0] && cfg.mode != MODE_WINDOW && !cfg.span) {
        int x0 = 1 << 30, y0 = 1 << 30, x1 = -(1 << 30), y1 = -(1 << 30), first = -1;
        char list[512];
        snprintf(list, sizeof list, ",%s,", cfg.span_list);
        for (int i = 0; i < nmon; i++) {
            char needle[80];
            snprintf(needle, sizeof needle, ",%s,", mons[i].name);
            if (!strstr(list, needle)) continue;
            if (first < 0) first = i;
            if (mons[i].x < x0) x0 = mons[i].x;
            if (mons[i].y < y0) y0 = mons[i].y;
            if (mons[i].x + mons[i].w > x1) x1 = mons[i].x + mons[i].w;
            if (mons[i].y + mons[i].h > y1) y1 = mons[i].y + mons[i].h;
            mons[i].name[0] = 0;   /* mark as merged */
        }
        if (first >= 0) {
            Monitor merged = { .x = x0, .y = y0, .w = x1 - x0, .h = y1 - y0 };
            snprintf(merged.name, sizeof merged.name, "span:%s", cfg.span_list);
            int k = 0;
            for (int i = 0; i < nmon; i++)
                if (mons[i].name[0]) mons[k++] = mons[i];
            mons[k++] = merged;
            nmon = k;
        } else {
            logmsg("--screen-span: none of %s were found (see --list-monitors)", cfg.span_list);
        }
    }
    for (int i = 0; i < nmon; i++) {
        Monitor *m = &mons[i];
        dbg("monitor %d: %s %dx%d+%d+%d", i, m->name, m->w, m->h, m->x, m->y);
        m->scaling = cfg.scaling;
        m->clamp = cfg.clamp;
        m->playlist = NULL;
        m->nplaylist = 0;
        m->interval = 0;
        bool ok = false;
        const char *bg = NULL;
        int si = screen_index(m->name);
        if (!strncmp(m->name, "span:", 5)) {
            m->scaling = cfg.span_scaling;
            m->clamp = cfg.span_clamp;
            if (cfg.span_bg[0]) bg = cfg.span_bg;
        } else if (si >= 0) {
            m->scaling = cfg.screen_scaling[si];
            m->clamp = cfg.screen_clamp[si];
            if (cfg.screen_playlist[si]) load_screen_playlist(m, cfg.screen_playlist[si]);
            if (cfg.screen_bg[si][0]) bg = cfg.screen_bg[si];
        }
        if (m->playlist) {
            m->idx = -1;
            advance(m, 1, now);
            ok = m->cur != NULL;
        }
        if (!ok && bg) ok = switch_to(m, bg, now);
        if (!ok && i < MAX_MONITORS && cfg.monitor_wp[i][0]) ok = switch_to(m, cfg.monitor_wp[i], now);
        if (!ok && nplay) {
            m->idx = (cfg.different ? i : 0) % nplay - 1;
            advance(m, 1, now);
            ok = m->cur != NULL;
        }
        if (!ok) switch_to(m, "builtin", now);
    }
}

static Monitor *find_monitor(const char *s)
{
    if (!s || !*s) return NULL;
    char *end;
    long i = strtol(s, &end, 10);
    if (!*end && i >= 0 && i < nmon) return &mons[i];
    for (int j = 0; j < nmon; j++)
        if (!strcmp(mons[j].name, s)) return &mons[j];
    return NULL;
}

/* ------------------------------------------------------------------ IPC */

static void socket_path(char *out, size_t n)
{
    const char *rt = getenv("XDG_RUNTIME_DIR");
    if (rt && *rt) snprintf(out, n, "%s/wallengine.sock", rt);
    else snprintf(out, n, "/tmp/wallengine-%d.sock", (int)getuid());
}

static int ipc_connect(void)
{
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    socket_path(a.sun_path, sizeof a.sun_path);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int ipc_send(const char *cmd, bool print)
{
    int fd = ipc_connect();
    if (fd < 0) {
        if (print) fprintf(stderr, "wallengine is not running\n");
        return 1;
    }
    char line[PATH_MAX + 64];
    snprintf(line, sizeof line, "%s\n", cmd);
    if (write(fd, line, strlen(line)) < 0) { close(fd); return 1; }
    shutdown(fd, SHUT_WR);
    char buf[4096];
    ssize_t r;
    while ((r = read(fd, buf, sizeof buf)) > 0)
        if (print) fwrite(buf, 1, r, stdout);
    close(fd);
    return 0;
}

static int ipc_listen(void)
{
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    socket_path(a.sun_path, sizeof a.sun_path);
    unlink(a.sun_path);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0 || listen(fd, 4) < 0) {
        logmsg("cannot create control socket %s: %s", a.sun_path, strerror(errno));
        close(fd);
        return -1;
    }
    chmod(a.sun_path, 0600);
    return fd;
}

static void apply_to_all(void (*fn)(Wallpaper *, int), int v)
{
    for (int i = 0; i < nmon; i++) {
        if (mons[i].cur) fn(mons[i].cur, v);
        if (mons[i].prev) fn(mons[i].prev, v);
    }
}
static void set_fx(Wallpaper *w, int v) { if (!w->own_fx) w->effect = v; }
static void set_vis(Wallpaper *w, int v) { if (!w->own_vis) w->vis = v; }

static void handle_cmd(char *line, FILE *out, double now)
{
    char *save, *argv[3] = { 0 };
    int argc = 0;
    char *cmd = strtok_r(line, " \t\r\n", &save);
    if (!cmd) return;
    argv[argc++] = cmd;
    char *rest = save ? trim(save) : NULL;
    if (rest && *rest) {
        /* "set [monitor] PATH": the path is everything after the optional monitor */
        char *sp = strpbrk(rest, " \t");
        if (sp) {
            char saved = *sp;
            *sp = 0;
            if (find_monitor(rest)) {
                argv[argc++] = rest;
                rest = trim(sp + 1);
            } else {
                *sp = saved;
            }
        }
        argv[argc++] = rest;
    }
    const char *c = argv[0];

    if (!strcmp(c, "next") || !strcmp(c, "prev")) {
        int dir = !strcmp(c, "next") ? 1 : -1;
        Monitor *m = argc >= 2 ? find_monitor(argv[1]) : NULL;
        for (int i = 0; i < nmon; i++)
            if (!m || m == &mons[i]) advance(&mons[i], dir, now);
        fprintf(out, "ok\n");
    } else if (!strcmp(c, "set") && argc >= 2) {
        Monitor *m = argc >= 3 ? find_monitor(argv[1]) : NULL;
        const char *raw = argc >= 3 ? argv[2] : argv[1];
        char path[PATH_MAX];
        expand_home(path, sizeof path, raw);
        int okc = 0;
        for (int i = 0; i < nmon; i++)
            if (!m || m == &mons[i]) okc += switch_to(&mons[i], path, now);
        fprintf(out, okc ? "ok\n" : "error: could not load %s\n", path);
    } else if (!strcmp(c, "pause")) {
        user_paused = true;
        fprintf(out, "paused\n");
    } else if (!strcmp(c, "resume") || !strcmp(c, "play")) {
        user_paused = false;
        fprintf(out, "playing\n");
    } else if (!strcmp(c, "toggle")) {
        user_paused = !user_paused;
        fprintf(out, user_paused ? "paused\n" : "playing\n");
    } else if (!strcmp(c, "effect") && argc >= 2 && parse_effect(argv[1]) >= 0) {
        cfg.effect = parse_effect(argv[1]);
        apply_to_all(set_fx, cfg.effect);
        fprintf(out, "ok\n");
    } else if (!strcmp(c, "visualizer") && argc >= 2 && parse_vis(argv[1]) >= 0) {
        cfg.vis = parse_vis(argv[1]);
        apply_to_all(set_vis, cfg.vis);
        fprintf(out, "ok\n");
    } else if (!strcmp(c, "fps") && argc >= 2 && atoi(argv[1]) > 0) {
        cfg.fps = atoi(argv[1]);
        fprintf(out, "ok\n");
    } else if (!strcmp(c, "reload")) {
        X.reconfigure = true;
        fprintf(out, "ok\n");
    } else if (!strcmp(c, "screenshot") && argc >= 2) {
        expand_home(pending_shot, sizeof pending_shot, argv[1]);
        fprintf(out, "ok\n");
    } else if (!strcmp(c, "status")) {
        fprintf(out, "wallengine %s  %s  fps=%d  effect=%s  visualizer=%s\n", WE_VERSION,
                user_paused ? "paused" : "playing", cfg.fps, effect_name(cfg.effect), vis_name(cfg.vis));
        for (int i = 0; i < nmon; i++)
            fprintf(out, "%d %-10s %dx%d+%d+%d %s%s\n", i, mons[i].name, mons[i].w, mons[i].h,
                    mons[i].x, mons[i].y, mons[i].cur ? mons[i].cur->path : "-",
                    mons[i].covered ? "  (covered, paused)" : "");
    } else if (!strcmp(c, "list")) {
        for (int i = 0; i < nplay; i++) fprintf(out, "%s\n", playlist[i]);
    } else if (!strcmp(c, "property") && argc >= 2) {
        /* "property NAME VALUE" (or NAME=VALUE) for Wallpaper Engine projects */
        char buf[PATH_MAX];
        snprintf(buf, sizeof buf, "%s", argv[1]);
        char *val = strchr(buf, '=');
        if (!val) val = strpbrk(buf, " \t");
        if (val) *val++ = 0;
        int n = 0;
        for (int i = 0; val && i < nmon; i++) {
            if (mons[i].cur && mons[i].cur->we) n += we_set_property(mons[i].cur->we, buf, trim(val));
        }
        if (n) fprintf(out, "ok (%d)\n", n);
        else fprintf(out, "error: no wallpaper has property '%s'\n", buf);
    } else if (!strcmp(c, "properties")) {
        for (int i = 0; i < nmon; i++)
            if (mons[i].cur && mons[i].cur->we) {
                fprintf(out, "[%s] %s\n", mons[i].name, we_title(mons[i].cur->we));
                we_list_properties(mons[i].cur->we, out);
            }
    } else if (!strcmp(c, "quit")) {
        quit_flag = 1;
        fprintf(out, "bye\n");
    } else {
        fprintf(out, "error: unknown command (try: next prev set pause resume toggle effect "
                     "visualizer fps reload screenshot status list property properties quit)\n");
    }
}

static void ipc_poll(int lfd, double now)
{
    if (lfd < 0) return;
    int c;
    while ((c = accept4(lfd, NULL, NULL, SOCK_CLOEXEC)) >= 0) {
        struct timeval tv = { 0, 200000 };
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        char buf[PATH_MAX + 64];
        ssize_t n = 0, r;
        while (n < (ssize_t)sizeof buf - 1 && (r = read(c, buf + n, sizeof buf - 1 - n)) > 0) {
            n += r;
            if (memchr(buf, '\n', n)) break;
        }
        buf[n > 0 ? n : 0] = 0;
        char *nl = strchr(buf, '\n');
        if (nl) *nl = 0;
        FILE *out = fdopen(c, "w");
        if (!out) { close(c); continue; }
        handle_cmd(buf, out, now);
        fclose(out);
    }
}

/* ------------------------------------------------------------------ main */

static void usage(void)
{
    printf(
"wallengine %s - live wallpaper engine for X11\n\n"
"usage: wallengine [options] [wallpaper|directory ...]\n"
"       wallengine ctl <command> [args]\n\n"
"wallpapers: GLSL shaders (.frag .glsl, Shadertoy-compatible), images (.jpg .png ...),\n"
"            videos (.mp4 .webm .mkv .gif ..., needs ffmpeg), scenes (.scene),\n"
"            Wallpaper Engine projects: a Steam workshop id or a folder with project.json\n\n"
"options:\n"
"  -m, --mode MODE         desktop (default) | root | window\n"
"  -f, --fps N             frame rate cap (default 30)\n"
"  -i, --interval SEC      rotate the playlist every SEC seconds\n"
"  -t, --transition SEC    crossfade duration (default 1.5, 0 = off)\n"
"  -s, --shuffle           shuffle the playlist\n"
"      --span              stretch one wallpaper across all monitors\n"
"      --different         show a different wallpaper on each monitor\n"
"      --monitor N=PATH    fixed wallpaper for monitor N\n"
"      --fit MODE          cover | contain | stretch | center\n"
"      --effect FX         none | snow | rain | fireflies | bokeh\n"
"      --vis VIS           audio visualizer: none | bars | wave | circle\n"
"      --parallax F        mouse parallax amount for images (default 0.02)\n"
"      --speed F           animation speed multiplier\n"
"      --no-audio          disable audio capture\n"
"      --sound             play the sound of video wallpapers (needs ffplay)\n"
"      --volume N          video sound volume 0-100\n"
"      --no-pause          keep running when a fullscreen app is focused\n"
"      --pause-maximized   also pause behind maximized windows\n"
"      --pause-battery     pause while running on battery\n"
"      --size WxH          window size for --mode window\n"
"  -c, --config FILE       config file (default ~/.config/wallengine/config.ini)\n"
"      --screenshot FILE   save a PNG of the output (with --duration)\n"
"      --duration SEC      exit after SEC seconds\n"
"      --replace           stop an already running instance first\n"
"      --list-monitors     print detected monitors and exit\n"
"\nWallpaper Engine (linux-wallpaperengine compatible):\n"
"  -r, --screen-root NAME  the following --bg/--scaling/--clamp/--playlist apply to screen NAME\n"
"  -b, --bg ID|PATH        wallpaper for the screen named by the previous --screen-root\n"
"      --screen-span A,B   span one wallpaper across the listed screens (then --bg)\n"
"      --scaling MODE      stretch | fit | fill | default\n"
"      --clamp MODE        clamp | border | repeat\n"
"      --playlist NAME     use a playlist from Wallpaper Engine's config.json\n"
"  -w, --window XxYxWxH    run in a window with this geometry\n"
"      --assets-dir DIR    Wallpaper Engine 'assets' folder (auto-detected from Steam)\n"
"      --silent            mute wallpaper sounds\n"
"      --wallpaper-volume N  wallpaper sound volume 0-128 (default 15)\n"
"      --noautomute        keep playing sounds while other apps play audio\n"
"      --no-audio-processing  disable audio-reactive features of wallpapers\n"
"      --disable-particles | --disable-mouse | --disable-parallax\n"
"  -l, --list-properties   list the wallpaper's user properties and exit\n"
"      --set-property N=V  override a user property (repeatable)\n"
"  -z, --dump-structure    print the scene structure\n"
"      --screenshot-delay N  frames to wait before --screenshot (default 5)\n"
"      --render-debug X    base-only | no-solid-final | pass-log | object=ID | skip-object=ID | skip-effect=ID\n"
"      --no-fullscreen-pause  same as --no-pause\n"
"  -v, --verbose\n"
"  -h, --help\n\n"
"control a running instance:\n"
"  wallengine ctl next|prev [monitor]     wallengine ctl set [monitor] PATH\n"
"  wallengine ctl pause|resume|toggle     wallengine ctl effect snow\n"
"  wallengine ctl visualizer bars         wallengine ctl fps 60\n"
"  wallengine ctl screenshot FILE         wallengine ctl status|list|reload|quit\n"
"  wallengine ctl property NAME VALUE     wallengine ctl properties\n",
        WE_VERSION);
}

int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "ctl")) {
        char cmd[PATH_MAX + 64] = "";
        for (int i = 2; i < argc; i++) {
            char abs[PATH_MAX];
            const char *a = argv[i];
            /* make relative paths absolute so the daemon can find them */
            if (i == argc - 1 && i > 2 && !strcmp(argv[2], "set") && a[0] != '/' && a[0] != '~' &&
                realpath(a, abs))
                a = abs;
            if (i == argc - 1 && !strcmp(argv[2], "screenshot") && i > 2 && a[0] != '/' && a[0] != '~') {
                char cwd[PATH_MAX];
                if (getcwd(cwd, sizeof cwd)) {
                    snprintf(abs, sizeof abs, "%s/%s", cwd, a);
                    a = abs;
                }
            }
            strncat(cmd, a, sizeof cmd - strlen(cmd) - 2);
            if (i < argc - 1) strcat(cmd, " ");
        }
        return ipc_send(cmd[0] ? cmd : "status", true);
    }

    /* config file first, command line overrides it */
    char conf[PATH_MAX];
    snprintf(conf, sizeof conf, "%s/.config/wallengine/config.ini", getenv("HOME"));
    for (int i = 1; i < argc - 1; i++)
        if (!strcmp(argv[i], "-c") || !strcmp(argv[i], "--config")) expand_home(conf, sizeof conf, argv[i + 1]);
    load_config(conf);

    bool replace = false, list_mon = false, list_props = false;
    int last_screen = -1;     /* -1 none, -2 span group, else index into cfg.screen_* */
    const char *default_bg = NULL;
    char *positional[256];
    int npos = 0;
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
#define ARG(s, l) (!strcmp(a, s) || !strcmp(a, l))
#define NEED() do { if (!v) { logmsg("%s needs a value", a); return 2; } i++; } while (0)
        if (ARG("-h", "--help")) { usage(); return 0; }
        else if (ARG("-v", "--verbose")) cfg.verbose = true;
        else if (ARG("-m", "--mode")) { NEED(); if (!set_option("mode", v)) { logmsg("bad mode %s", v); return 2; } }
        else if (ARG("-f", "--fps")) { NEED(); set_option("fps", v); }
        else if (ARG("-i", "--interval")) { NEED(); cfg.interval = atoi(v); }
        else if (ARG("-t", "--transition")) { NEED(); cfg.transition = atof(v); }
        else if (ARG("-s", "--shuffle")) cfg.shuffle = true;
        else if (ARG("-c", "--config")) { NEED(); }
        else if (!strcmp(a, "--span")) cfg.span = true;
        else if (!strcmp(a, "--different")) cfg.different = true;
        else if (!strcmp(a, "--fit")) { NEED(); if (!set_option("fit", v)) { logmsg("bad fit %s", v); return 2; } }
        else if (!strcmp(a, "--effect")) { NEED(); if (!set_option("effect", v)) { logmsg("bad effect %s", v); return 2; } }
        else if (!strcmp(a, "--vis")) { NEED(); if (!set_option("visualizer", v)) { logmsg("bad visualizer %s", v); return 2; } }
        else if (!strcmp(a, "--parallax")) { NEED(); cfg.parallax = atof(v); }
        else if (!strcmp(a, "--speed")) { NEED(); cfg.speed = atof(v); }
        else if (!strcmp(a, "--no-audio")) cfg.audio = false;
        else if (!strcmp(a, "--sound")) cfg.video_sound = true;
        else if (!strcmp(a, "--volume")) { NEED(); cfg.volume = atoi(v); }
        else if (!strcmp(a, "--no-pause")) cfg.pause_fullscreen = false;
        else if (!strcmp(a, "--pause-maximized")) cfg.pause_maximized = true;
        else if (!strcmp(a, "--pause-battery")) cfg.pause_battery = true;
        else if (!strcmp(a, "--size")) { NEED(); set_option("window_size", v); }
        else if (!strcmp(a, "--screenshot")) { NEED(); expand_home(cfg.screenshot, PATH_MAX, v); }
        else if (!strcmp(a, "--duration")) { NEED(); cfg.duration = atof(v); }
        else if (!strcmp(a, "--replace")) replace = true;
        else if (!strcmp(a, "--list-monitors")) list_mon = true;
        else if (!strcmp(a, "--monitor")) {
            NEED();
            char k[32];
            const char *eq = strchr(v, '=');
            if (!eq) { logmsg("--monitor expects N=PATH"); return 2; }
            snprintf(k, sizeof k, "monitor%.*s", (int)(eq - v), v);
            if (!set_option(k, eq + 1)) { logmsg("bad --monitor %s", v); return 2; }
        }
        else if (ARG("-r", "--screen-root")) {
            NEED();
            if (cfg.nscreens >= MAX_MONITORS) { logmsg("too many --screen-root"); return 2; }
            last_screen = cfg.nscreens++;
            snprintf(cfg.screen_name[last_screen], 64, "%s", v);
            cfg.screen_scaling[last_screen] = cfg.scaling;
            cfg.screen_clamp[last_screen] = cfg.clamp;
            if (cfg.mode == MODE_WINDOW) cfg.mode = MODE_DESKTOP;
        }
        else if (!strcmp(a, "--screen-span")) {
            NEED();
            snprintf(cfg.span_list, sizeof cfg.span_list, "%s", v);
            cfg.span_scaling = cfg.scaling;
            cfg.span_clamp = cfg.clamp;
            last_screen = -2;
        }
        else if (ARG("-b", "--bg")) {
            NEED();
            if (last_screen >= 0) snprintf(cfg.screen_bg[last_screen], PATH_MAX, "%s", v);
            else if (last_screen == -2) snprintf(cfg.span_bg, PATH_MAX, "%s", v);
            default_bg = v;
        }
        else if (!strcmp(a, "--scaling") || !strcmp(a, "--clamp")) {
            NEED();
            bool sc = !strcmp(a, "--scaling");
            int x = sc ? parse_scaling(v) : parse_clamp(v);
            if (x < 0) { logmsg("bad %s %s", a, v); return 2; }
            if (last_screen >= 0) *(sc ? &cfg.screen_scaling[last_screen] : &cfg.screen_clamp[last_screen]) = x;
            else if (last_screen == -2) *(sc ? &cfg.span_scaling : &cfg.span_clamp) = x;
            else *(sc ? &cfg.scaling : &cfg.clamp) = x;
        }
        else if (!strcmp(a, "--playlist")) {
            NEED();
            if (last_screen >= 0) {
                cfg.screen_playlist[last_screen] = strdup(v);
            } else {
                char **items = NULL;
                int delay = 60;
                bool random = false;
                int n = we_playlist(v, &items, &delay, &random);
                for (int k = 0; k < n && nplay < MAX_PLAYLIST; k++) playlist[nplay++] = items[k];
                free(items);
                if (n) {
                    cfg.interval = (delay > 0 ? delay : 1) * 60;
                    cfg.shuffle = cfg.shuffle || random;
                }
            }
        }
        else if (ARG("-w", "--window")) {
            NEED();
            int gx, gy, gw, gh;
            if (sscanf(v, "%dx%dx%dx%d", &gx, &gy, &gw, &gh) != 4) { logmsg("--window expects XxYxWxH"); return 2; }
            cfg.mode = MODE_WINDOW;
            cfg.win_x = gx;
            cfg.win_y = gy;
            cfg.win_w = gw;
            cfg.win_h = gh;
            cfg.win_pos = true;
        }
        else if (!strcmp(a, "--assets-dir")) { NEED(); char d[PATH_MAX]; expand_home(d, sizeof d, v); we_opt_assets(d); }
        else if (!strcmp(a, "--silent")) we_silent = true;
        else if (!strcmp(a, "--wallpaper-volume")) { NEED(); we_volume = atoi(v); }
        else if (!strcmp(a, "--noautomute")) we_automute = false;
        else if (!strcmp(a, "--no-audio-processing")) we_processing = false;
        else if (!strcmp(a, "--disable-particles")) we_opt_disable_particles(true);
        else if (!strcmp(a, "--disable-mouse")) we_opt_disable_mouse(true);
        else if (!strcmp(a, "--disable-parallax")) we_opt_disable_parallax(true);
        else if (ARG("-l", "--list-properties")) list_props = true;
        else if (!strcmp(a, "--set-property") || !strcmp(a, "--property")) {
            NEED();
            const char *eq = strchr(v, '=');
            char name[256];
            snprintf(name, sizeof name, "%.*s", eq ? (int)(eq - v) : (int)strlen(v), v);
            we_opt_property(name, eq ? eq + 1 : "1");
        }
        else if (ARG("-z", "--dump-structure")) we_opt_dump(true);
        else if (!strcmp(a, "--screenshot-delay")) { NEED(); cfg.screenshot_delay = atoi(v); }
        else if (!strcmp(a, "--render-debug")) { NEED(); we_opt_render_debug(v); }
        else if (!strcmp(a, "--no-fullscreen-pause")) cfg.pause_fullscreen = false;
        else if (!strcmp(a, "--layer") || !strcmp(a, "--fullscreen-pause-ignore-appid")) {
            NEED();
            logmsg("note: %s only applies to Wayland and is ignored on X11", a);
        }
        else if (!strcmp(a, "--fullscreen-pause-only-active")) { /* X11 already checks the active window */ }
        else if (a[0] == '-' && a[1]) { logmsg("unknown option %s (see --help)", a); return 2; }
        else if (npos < 256) positional[npos++] = argv[i];   /* options may follow wallpapers */
    }

    we_opt_audio(we_silent, we_volume, we_automute, we_processing);
    if (list_props) {
        int rc = 0, any = 0;
        for (int k = 0; k < npos; k++, any++) rc |= we_list_properties_path(positional[k]);
        if (default_bg) {
            rc |= we_list_properties_path(default_bg);
            any++;
        }
        for (int k = 0; k < cfg.nscreens; k++)
            if (cfg.screen_bg[k][0]) {
                rc |= we_list_properties_path(cfg.screen_bg[k]);
                any++;
            }
        if (!any) logmsg("--list-properties needs a wallpaper (workshop id or project folder)");
        return any ? rc : 2;
    }

    if (!list_mon) {
        int fd = ipc_connect();
        if (fd >= 0) {
            close(fd);
            if (!replace) {
                logmsg("already running (use --replace, or 'wallengine ctl ...')");
                return 1;
            }
            ipc_send("quit", false);
            for (int k = 0; k < 50 && (fd = ipc_connect()) >= 0; k++) {
                close(fd);
                usleep(100000);
            }
        }
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGHUP, on_signal);
    signal(SIGPIPE, SIG_IGN);

    if (!x_init(&X, cfg.mode, cfg.win_w, cfg.win_h)) return 1;
    if (list_mon) {
        nmon = x_monitors(&X, mons, MAX_MONITORS, MODE_DESKTOP, false);
        for (int k = 0; k < nmon; k++)
            printf("%d %s %dx%d+%d+%d\n", k, mons[k].name, mons[k].w, mons[k].h, mons[k].x, mons[k].y);
        x_shutdown(&X);
        return 0;
    }
    if (cfg.mode == MODE_DESKTOP && system("pgrep -x xfdesktop >/dev/null 2>&1") == 0)
        logmsg("note: xfdesktop is running. The live wallpaper is drawn above it, so desktop "
               "icons are hidden (but still clickable). To remove them: Settings > Desktop > "
               "Icons > Icon type: None, or run 'xfdesktop --quit'.");
    if (!render_init()) return 1;
    we_global_init();

    build_playlist(npos, positional, 0);
    if (nplay == 0 && default_bg) add_path(default_bg, 0);
    if (cfg.audio) audio_start(cfg.audio_cmd);
    else audio_start("true");  /* creates the (silent) audio texture only */

    int lfd = ipc_listen();
    double start = now_sec();
    setup_monitors(start);

    float mouse[4] = { 0 };
    bool first_mouse = true;
    double last_frame = start, last_check = 0, last_batt = 0;
    bool fs = false, batt = false;
    int fsr[4] = { 0 };
    long frames = 0;

    while (!quit_flag && !X.closed) {
        double now = now_sec();
        x_poll(&X);
        ipc_poll(lfd, now);
        if (X.reconfigure) {
            X.reconfigure = false;
            if (X.win != X.root && cfg.mode != MODE_WINDOW) {
                XMoveResizeWindow(X.dpy, X.win, 0, 0, X.sw, X.sh);
                X.ww = X.sw;
                X.wh = X.sh;
            } else if (X.win == X.root) {
                X.ww = X.sw;
                X.wh = X.sh;
            }
            setup_monitors(now);
        }

        if (now - last_check > 0.5) {
            last_check = now;
            fs = cfg.pause_fullscreen && x_fullscreen_rect(&X, cfg.pause_maximized, fsr);
        }
        if (cfg.pause_battery && now - last_batt > 5) {
            last_batt = now;
            batt = on_battery();
        }

        int active = 0;
        for (int k = 0; k < nmon; k++) {
            Monitor *m = &mons[k];
            m->covered = false;
            if (fs && cfg.mode != MODE_WINDOW) {
                int ix = (fsr[0] + fsr[2] < m->x + m->w ? fsr[0] + fsr[2] : m->x + m->w) -
                         (fsr[0] > m->x ? fsr[0] : m->x);
                int iy = (fsr[1] + fsr[3] < m->y + m->h ? fsr[1] + fsr[3] : m->y + m->h) -
                         (fsr[1] > m->y ? fsr[1] : m->y);
                m->covered = ix > 0 && iy > 0 && (double)ix * iy >= 0.9 * m->w * m->h;
            }
            bool p = user_paused || batt || m->covered;
            wp_pause(m->cur, p, now);
            wp_pause(m->prev, p, now);
            if (!p) active++;
        }

        if (!active && !pending_shot[0]) {
            usleep(100000);
            if (cfg.duration > 0 && now - start >= cfg.duration) break;
            continue;
        }

        audio_update();
        we_global_frame(now);

        int px = 0, py = 0;
        bool down = false;
        x_pointer(&X, &px, &py, &down);
        float dt = (float)(now - last_frame);
        float k = first_mouse ? 1.0f : fminf(1.0f, dt * 6.0f);
        first_mouse = false;
        mouse[0] += (px - mouse[0]) * k;
        mouse[1] += (py - mouse[1]) * k;
        mouse[2] = down;
        last_frame = now;

        glViewport(0, 0, X.ww, X.wh);
        for (int k2 = 0; k2 < nmon; k2++) {
            Monitor *m = &mons[k2];
            int interval = m->playlist ? m->interval : cfg.interval;
            int count = m->playlist ? m->nplaylist : nplay;
            if (interval > 0 && count > 1 && !m->covered && !user_paused &&
                !(k2 < MAX_MONITORS && cfg.monitor_wp[k2][0]) && now - m->last_switch >= interval)
                advance(m, 1, now);
            if (m->covered && !pending_shot[0]) continue;

            float tf = cfg.transition > 0 ? (float)((now - m->trans_start) / cfg.transition) : 1.0f;
            if (tf >= 1.0f && m->prev) {
                wp_free(m->prev);
                m->prev = NULL;
            }
            if (m->prev) {
                wp_tick(m->prev, now);
                wp_draw(m->prev, m, X.wh, now, 1.0f, mouse);
            }
            if (m->cur) {
                wp_tick(m->cur, now);
                float s = tf < 1 ? tf * tf * (3 - 2 * tf) : 1.0f;
                wp_draw(m->cur, m, X.wh, now, m->prev ? s : 1.0f, mouse);
            }
        }

        bool done = cfg.duration > 0 && now - start >= cfg.duration;
        /* --screenshot without --duration: take it after --screenshot-delay frames and keep running */
        frames++;
        if (cfg.screenshot[0] && cfg.duration <= 0 && frames == (cfg.screenshot_delay > 0 ? cfg.screenshot_delay : 5))
            snprintf(pending_shot, sizeof pending_shot, "%s", cfg.screenshot);
        if (pending_shot[0] || (done && cfg.screenshot[0])) {
            const char *f = pending_shot[0] ? pending_shot : cfg.screenshot;
            if (save_screenshot(f, X.ww, X.wh)) logmsg("saved %s", f);
            else logmsg("could not save %s", f);
            pending_shot[0] = 0;
        }
        x_swap(&X);
        if (done) break;

        double next = now + 1.0 / cfg.fps, wait = next - now_sec();
        if (wait > 0) usleep((useconds_t)(wait * 1e6));
    }

    logmsg("exiting");
    for (int k = 0; k < nmon; k++) {
        wp_free(mons[k].cur);
        wp_free(mons[k].prev);
    }
    we_global_shutdown();
    audio_stop();
    if (lfd >= 0) {
        char sp[108];
        socket_path(sp, sizeof sp);
        unlink(sp);
        close(lfd);
    }
    x_shutdown(&X);
    return 0;
}
