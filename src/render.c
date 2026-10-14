/* Wallpaper loading and drawing: Shadertoy-compatible GLSL wallpapers, and
 * image/video "scenes" with parallax, particle effects and audio visualizers. */
#include "we.h"

#include "third_party/stb_image.h"
#include "third_party/stb_image_write.h"
#include "we/wallpaper_engine.h"

#include <ctype.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static GLuint vao, scene_prog;

static const char *VS =
    "#version 330\n"
    "void main(){ vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);"
    " gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0); }\n";

static const char *TOY_HEADER =
    "#version 330\n"
    "uniform vec3 iResolution; uniform float iTime; uniform float iTimeDelta;\n"
    "uniform int iFrame; uniform float iFrameRate; uniform vec4 iMouse; uniform vec4 iDate;\n"
    "uniform sampler2D iChannel0; uniform sampler2D iChannel1;\n"
    "uniform vec3 iChannelResolution[4]; uniform float iSampleRate;\n"
    "uniform float iBass; uniform float iLevel; uniform vec2 uOffset;\n"
    "out vec4 we_FragColor;\n"
    "#line 1\n";

static const char *TOY_FOOTER =
    "\nvoid main(){ vec4 c = vec4(0.0, 0.0, 0.0, 1.0);"
    " mainImage(c, gl_FragCoord.xy - uOffset); we_FragColor = vec4(c.rgb, 1.0); }\n";

static const char *DEFAULT_TOY =
    "void mainImage(out vec4 o, in vec2 f){ vec2 uv = f / iResolution.xy;"
    " vec3 c = 0.5 + 0.5 * cos(iTime * 0.3 + uv.xyx * 3.0 + vec3(0, 2, 4));"
    " o = vec4(c * (0.6 + 0.4 * iBass), 1.0); }\n";

/* Built-in scene shader: image/video + parallax + particles + visualizer. */
static const char *SCENE_FS =
    "#version 330\n"
    "out vec4 fragColor;\n"
    "uniform sampler2D uTex; uniform sampler2D uAudio;\n"
    "uniform vec2 uTexSize, uOffset, uMouse; uniform vec3 iResolution;\n"
    "uniform float iTime, uParallax, uBass, uBright, uSat; uniform vec3 uTint;\n"
    "uniform int uFit, uEffect, uVis;\n"
    "float hash(vec2 p){ p = fract(p * vec2(123.34, 456.21)); p += dot(p, p + 45.32); return fract(p.x * p.y); }\n"
    "float spec(float x){ return texture(uAudio, vec2(x, 0.25)).r; }\n"
    "float wave(float x){ return texture(uAudio, vec2(x, 0.75)).r; }\n"
    "vec2 fitUV(vec2 uv){\n"
    "  float sa = iResolution.x / iResolution.y, ta = uTexSize.x / uTexSize.y;\n"
    "  vec2 c = uv - 0.5;\n"
    "  if (uFit == 0) c *= sa > ta ? vec2(1.0, ta / sa) : vec2(sa / ta, 1.0);\n"
    "  else if (uFit == 1) c *= sa > ta ? vec2(sa / ta, 1.0) : vec2(1.0, ta / sa);\n"
    "  else if (uFit == 3) c *= iResolution.xy / uTexSize;\n"
    "  return c + 0.5; }\n"
    "vec3 snow(vec2 uv, float a){ float s = 0.0;\n"
    "  for (int i = 0; i < 5; i++) { float fi = float(i), sc = 6.0 + fi * 5.0;\n"
    "    vec2 p = uv * vec2(a, 1.0) * sc; p.y += iTime * (0.6 + fi * 0.15); p.x += sin(iTime * 0.4 + fi) * 0.6 + uMouse.x * fi * 0.1;\n"
    "    vec2 id = floor(p), f = fract(p) - 0.5; float h = hash(id + fi * 13.1);\n"
    "    vec2 o = vec2(hash(id + 3.1), hash(id + 7.7)) - 0.5; o.x += sin(iTime * 2.0 + h * 6.28) * 0.15;\n"
    "    s += smoothstep(0.07 - fi * 0.008, 0.0, length(f - o * 0.6)) * step(0.55, h) * (1.0 - fi * 0.12); }\n"
    "  return vec3(s); }\n"
    "vec3 rain(vec2 uv, float a){ float s = 0.0;\n"
    "  for (int i = 0; i < 3; i++) { float fi = float(i);\n"
    "    vec2 p = uv * vec2(a * (50.0 + fi * 30.0), 3.0 + fi);\n"
    "    p.x += p.y * 0.08; float col = floor(p.x); float h = hash(vec2(col, fi));\n"
    "    float y = fract(p.y + iTime * (2.5 + h * 1.5 + fi * 0.5) + h * 10.0);\n"
    "    float x = abs(fract(p.x) - 0.5);\n"
    "    s += smoothstep(0.12, 0.0, x) * smoothstep(0.0, 0.25, y) * smoothstep(0.35, 0.25, y) * step(0.75, h) * 0.45; }\n"
    "  return vec3(0.75, 0.82, 0.95) * s; }\n"
    "vec3 fireflies(vec2 uv, float a){ vec3 c = vec3(0.0);\n"
    "  for (int i = 0; i < 48; i++) { float fi = float(i);\n"
    "    vec2 p = vec2(hash(vec2(fi, 1.0)), hash(vec2(fi, 2.0)));\n"
    "    p += 0.1 * vec2(sin(iTime * (0.2 + hash(vec2(fi, 3.0)) * 0.4) + fi), cos(iTime * (0.15 + hash(vec2(fi, 4.0)) * 0.4) + fi * 1.3));\n"
    "    p = fract(p + uMouse * 0.02 * hash(vec2(fi, 5.0)));\n"
    "    vec2 d = (uv - p) * vec2(a, 1.0);\n"
    "    float tw = 0.5 + 0.5 * sin(iTime * (1.5 + hash(vec2(fi, 6.0)) * 2.0) + fi * 3.0);\n"
    "    c += 0.00025 / (dot(d, d) + 0.0002) * tw * vec3(1.0, 0.8, 0.35); }\n"
    "  return c * (0.5 + uBass); }\n"
    "vec3 bokeh(vec2 uv, float a){ vec3 c = vec3(0.0);\n"
    "  for (int i = 0; i < 24; i++) { float fi = float(i);\n"
    "    vec2 p = vec2(hash(vec2(fi, 8.0)), fract(hash(vec2(fi, 9.0)) + iTime * (0.01 + 0.02 * hash(vec2(fi, 10.0)))));\n"
    "    p.x += sin(iTime * 0.2 + fi) * 0.03;\n"
    "    float r = 0.03 + 0.06 * hash(vec2(fi, 11.0));\n"
    "    float d = length((uv - p) * vec2(a, 1.0));\n"
    "    vec3 col = 0.5 + 0.5 * cos(fi + vec3(0.0, 2.0, 4.0));\n"
    "    c += col * smoothstep(r, r * 0.85, d) * 0.12 * (0.6 + 0.8 * uBass); }\n"
    "  return c; }\n"
    "vec4 visual(vec2 uv, float a){\n"
    "  vec3 rainbow = 0.5 + 0.5 * cos(6.28 * (uv.x + iTime * 0.05) + vec3(0.0, 2.0, 4.0));\n"
    "  if (uVis == 1) { float n = 96.0; float i = floor(uv.x * n), f = fract(uv.x * n);\n"
    "    float v = spec((i + 0.5) / n * 0.85); float h = v * 0.28;\n"
    "    float bar = step(0.12, f) * step(f, 0.88);\n"
    "    float on = bar * step(uv.y, h);\n"
    "    float glow = bar * smoothstep(h + 0.02, h, uv.y) * 0.25;\n"
    "    return vec4(rainbow, max(on * 0.85, glow)); }\n"
    "  if (uVis == 2) { float y = 0.18 + (wave(uv.x) - 0.5) * 0.25;\n"
    "    float d = abs(uv.y - y) * iResolution.y;\n"
    "    return vec4(rainbow, smoothstep(3.0, 0.0, d) + smoothstep(25.0, 0.0, d) * 0.3); }\n"
    "  if (uVis == 3) { vec2 p = (uv - 0.5) * vec2(a, 1.0); float ang = atan(p.y, p.x) / 6.2832 + 0.5;\n"
    "    float m = abs(ang * 2.0 - 1.0); float r0 = 0.16 + uBass * 0.03; float r1 = r0 + spec(m * 0.8) * 0.18;\n"
    "    float rr = length(p);\n"
    "    float ring = smoothstep(0.004, 0.0, abs(rr - r0));\n"
    "    float fill = step(r0, rr) * step(rr, r1) * step(0.3, fract(ang * 128.0));\n"
    "    vec3 c = 0.5 + 0.5 * cos(6.28 * (ang + iTime * 0.05) + vec3(0.0, 2.0, 4.0));\n"
    "    return vec4(c, max(ring, fill * 0.85)); }\n"
    "  return vec4(0.0); }\n"
    "void main(){\n"
    "  vec2 uv = (gl_FragCoord.xy - uOffset) / iResolution.xy;\n"
    "  float a = iResolution.x / iResolution.y;\n"
    "  vec2 t = fitUV(uv);\n"
    "  float p = uParallax;\n"
    "  float zoom = 1.0 + 2.0 * p + (uVis > 0 ? uBass * 0.015 : 0.0);\n"
    "  t = (t - 0.5) / zoom + 0.5 + uMouse * vec2(p, -p) / zoom * 0.999;\n"
    "  t.y = 1.0 - t.y;\n"
    "  vec3 col = (t.x < 0.0 || t.y < 0.0 || t.x > 1.0 || t.y > 1.0) ? vec3(0.0) : texture(uTex, t).rgb;\n"
    "  float l = dot(col, vec3(0.299, 0.587, 0.114));\n"
    "  col = mix(vec3(l), col, uSat) * uBright * uTint;\n"
    "  vec2 puv = uv + uMouse * p * 0.5;\n"
    "  if (uEffect == 1) col += snow(puv, a) * 0.9;\n"
    "  else if (uEffect == 2) col = col * 0.85 + rain(puv, a);\n"
    "  else if (uEffect == 3) col += fireflies(puv, a);\n"
    "  else if (uEffect == 4) col += bokeh(puv, a);\n"
    "  vec4 v = visual(uv, a); col = mix(col, v.rgb, clamp(v.a, 0.0, 1.0));\n"
    "  fragColor = vec4(col, 1.0); }\n";

static GLuint compile(GLenum type, const char *src, const char *what)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        logmsg("shader error in %s:\n%s", what, log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static GLuint link_program(const char *fs_src, const char *what)
{
    GLuint vs = compile(GL_VERTEX_SHADER, VS, "vertex shader");
    GLuint fs = compile(GL_FRAGMENT_SHADER, fs_src, what);
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return 0;
    }
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetProgramInfoLog(p, sizeof log, NULL, log);
        logmsg("link error in %s:\n%s", what, log);
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

bool render_init(void)
{
    const char *ver = (const char *)glGetString(GL_VERSION);
    dbg("OpenGL %s on %s", ver, glGetString(GL_RENDERER));
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    scene_prog = link_program(SCENE_FS, "built-in scene shader");
    return scene_prog != 0;
}

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = malloc(n + 1);
    if (fread(s, 1, n, f) != (size_t)n) n = 0;
    s[n] = 0;
    fclose(f);
    return s;
}

static const char *ext_of(const char *p)
{
    const char *d = strrchr(p, '.'), *s = strrchr(p, '/');
    return d && (!s || d > s) ? d + 1 : "";
}

static bool ext_in(const char *p, const char *list)
{
    const char *e = ext_of(p);
    size_t n = strlen(e);
    if (!n) return false;
    for (const char *l = list; *l;) {
        const char *c = strchr(l, ',');
        size_t len = c ? (size_t)(c - l) : strlen(l);
        if (len == n && strncasecmp(l, e, n) == 0) return true;
        if (!c) break;
        l = c + 1;
    }
    return false;
}

#define SHADER_EXT "frag,glsl,fs,shader"
#define SCENE_EXT  "scene"
#define IMAGE_EXT  "jpg,jpeg,png,bmp,tga,psd,hdr,pic,ppm,pgm,pnm"
#define VIDEO_EXT  "mp4,webm,mkv,mov,avi,m4v,gif,wmv,flv,ogv"

bool is_wallpaper_file(const char *path)
{
    if (we_is_project(path)) return true;
    return ext_in(path, SHADER_EXT) || ext_in(path, SCENE_EXT) || ext_in(path, IMAGE_EXT) ||
           ext_in(path, VIDEO_EXT);
}

static GLuint make_texture(int w, int h, const void *px, bool mip)
{
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    if (mip) glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mip ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

static bool load_image(Wallpaper *w, const char *path)
{
    int n;
    unsigned char *px = stbi_load(path, &w->tw, &w->th, &n, 4);
    if (!px) {
        logmsg("cannot load image %s: %s", path, stbi_failure_reason());
        return false;
    }
    w->tex = make_texture(w->tw, w->th, px, true);
    stbi_image_free(px);
    return true;
}

static bool build_shader(Wallpaper *w)
{
    char *src = strcmp(w->path, "builtin") == 0 ? strdup(DEFAULT_TOY) : read_file(w->path);
    if (!src) {
        logmsg("cannot read %s", w->path);
        return false;
    }
    char *full;
    if (strstr(src, "#version")) {
        full = src;  /* complete shader, used as-is */
    } else {
        size_t n = strlen(TOY_HEADER) + strlen(src) + strlen(TOY_FOOTER) + 1;
        full = malloc(n);
        snprintf(full, n, "%s%s%s", TOY_HEADER, src, TOY_FOOTER);
        free(src);
    }
    GLuint p = link_program(full, w->path);
    free(full);
    if (!p) return false;
    if (w->prog) glDeleteProgram(w->prog);
    w->prog = p;
    return true;
}

static char *trim(char *s)
{
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

/* A .scene file is a small ini describing a media file plus effects. */
static bool parse_scene(Wallpaper *w)
{
    FILE *f = fopen(w->path, "r");
    if (!f) return false;
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%s", w->path);
    char *d = dirname(dir);
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        char *s = trim(line);
        if (!*s || *s == '#' || *s == ';' || *s == '[') continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        char *hash = strstr(eq + 1, " #");
        if (hash) *hash = 0;   /* inline comment */
        char *k = trim(s), *v = trim(eq + 1);
        if (!strcmp(k, "source") || !strcmp(k, "image") || !strcmp(k, "video")) {
            if (*v == '/') snprintf(w->source, sizeof w->source, "%s", v);
            else snprintf(w->source, sizeof w->source, "%s/%s", d, v);
        } else if (!strcmp(k, "fit")) {
            int x = parse_fit(v);
            if (x >= 0) w->fit = x;
        } else if (!strcmp(k, "parallax")) {
            w->parallax = atof(v);
        } else if (!strcmp(k, "effect")) {
            int x = parse_effect(v);
            if (x >= 0) { w->effect = x; w->own_fx = true; }
        } else if (!strcmp(k, "visualizer")) {
            int x = parse_vis(v);
            if (x >= 0) { w->vis = x; w->own_vis = true; }
        } else if (!strcmp(k, "speed")) {
            w->speed = atof(v);
        } else if (!strcmp(k, "tint")) {
            sscanf(v, "%f %f %f", &w->tint[0], &w->tint[1], &w->tint[2]);
        } else if (!strcmp(k, "brightness")) {
            w->brightness = atof(v);
        } else if (!strcmp(k, "saturation")) {
            w->saturation = atof(v);
        }
    }
    fclose(f);
    if (!w->source[0]) {
        logmsg("%s: missing 'source = <image or video>'", w->path);
        return false;
    }
    return true;
}

Wallpaper *wp_load(const char *path, int mw, int mh, int scaling, int clamp)
{
    Wallpaper *w = calloc(1, sizeof *w);
    snprintf(w->path, sizeof w->path, "%s", path);
    snprintf(w->source, sizeof w->source, "%s", path);
    w->fit = cfg.fit;
    w->parallax = cfg.parallax;
    w->effect = cfg.effect;
    w->vis = cfg.vis;
    w->speed = cfg.speed;
    w->tint[0] = w->tint[1] = w->tint[2] = 1;
    w->brightness = w->saturation = 1;
    w->start = now_sec();

    bool ok = false;
    char dir[PATH_MAX];
    if (strcmp(path, "builtin") && we_resolve(path, dir, sizeof dir)) {
        w->kind = WP_WORKSHOP;
        w->we = we_open(dir, mw, mh, scaling, clamp);
        ok = w->we != NULL;
    } else if (!strcmp(path, "builtin") || ext_in(path, SHADER_EXT)) {
        w->kind = WP_SHADER;
        struct stat st;
        if (!stat(path, &st)) w->mtime = st.st_mtime;
        ok = build_shader(w);
        /* optional companion texture: foo.frag + foo.png/jpg -> iChannel1 */
        char side[PATH_MAX];
        const char *exts[] = { "png", "jpg", "jpeg" };
        for (int i = 0; ok && i < 3 && !w->tex; i++) {
            snprintf(side, sizeof side, "%.*s.%s", (int)(strlen(path) - strlen(ext_of(path)) - 1),
                     path, exts[i]);
            if (!access(side, R_OK)) load_image(w, side);
        }
    } else {
        if (ext_in(path, SCENE_EXT) && !parse_scene(w)) goto fail;
        if (ext_in(w->source, VIDEO_EXT)) {
            w->kind = WP_VIDEO;
            w->video = video_open(w->source, mw, mh, w->fit, cfg.fps);
            if (w->video) {
                video_size(w->video, &w->tw, &w->th);
                w->tex = make_texture(w->tw, w->th, NULL, false);
                w->fit = FIT_STRETCH;   /* ffmpeg already fitted the frames */
                ok = true;
            } else if (ext_in(w->source, "gif")) {
                w->kind = WP_IMAGE;    /* fall back to the first frame */
                ok = load_image(w, w->source);
            }
        } else {
            w->kind = WP_IMAGE;
            ok = load_image(w, w->source);
        }
    }
    if (ok) {
        dbg("loaded %s", path);
        return w;
    }
fail:
    wp_free(w);
    return NULL;
}

void wp_free(Wallpaper *w)
{
    if (!w) return;
    if (w->we) we_close(w->we);
    video_close(w->video);
    if (w->prog) glDeleteProgram(w->prog);
    if (w->tex) glDeleteTextures(1, &w->tex);
    free(w);
}

void wp_pause(Wallpaper *w, bool paused, double now)
{
    if (!w || w->paused == paused) return;
    w->paused = paused;
    if (paused) w->paused_at = now;
    else w->start += now - w->paused_at;
    if (w->we) we_pause(w->we, paused);
    if (w->video) video_pause(w->video, paused);
}

void wp_tick(Wallpaper *w, double now)
{
    if (w->video) video_update(w->video, w->tex);
    if (w->kind == WP_SHADER && strcmp(w->path, "builtin") && now - w->last_check > 1.0) {
        w->last_check = now;
        struct stat st;
        if (!stat(w->path, &st) && st.st_mtime != w->mtime) {
            w->mtime = st.st_mtime;
            if (build_shader(w)) logmsg("reloaded %s", w->path);
        }
    }
}

#define U(p, n) glGetUniformLocation(p, n)

void wp_draw(Wallpaper *w, const Monitor *m, int win_h, double now, float alpha, const float mouse[4])
{
    int vx = m->x, vy = win_h - (m->y + m->h);
    if (w->kind == WP_WORKSHOP) {
        float mx = (mouse[0] - m->x) / (float)m->w, my = (mouse[1] - m->y) / (float)m->h;
        we_draw(w->we, vx, vy, m->w, m->h, alpha, mx, my, mouse[2] > 0);
        glBindVertexArray(vao);
        return;
    }
    glViewport(vx, vy, m->w, m->h);
    glEnable(GL_SCISSOR_TEST);
    glScissor(vx, vy, m->w, m->h);
    if (alpha < 1.0f) {
        glEnable(GL_BLEND);
        glBlendColor(0, 0, 0, alpha);
        glBlendFunc(GL_CONSTANT_ALPHA, GL_ONE_MINUS_CONSTANT_ALPHA);
    } else {
        glDisable(GL_BLEND);
    }
    double t = ((w->paused ? w->paused_at : now) - w->start) * w->speed;

    if (w->kind == WP_SHADER) {
        GLuint p = w->prog;
        glUseProgram(p);
        time_t tt = time(NULL);
        struct tm lt;
        localtime_r(&tt, &lt);
        float secs = lt.tm_hour * 3600 + lt.tm_min * 60 + lt.tm_sec + (float)(now - (long)now);
        glUniform3f(U(p, "iResolution"), m->w, m->h, 1);
        glUniform1f(U(p, "iTime"), (float)t);
        glUniform1f(U(p, "iTimeDelta"), 1.0f / cfg.fps);
        glUniform1f(U(p, "iFrameRate"), cfg.fps);
        glUniform1i(U(p, "iFrame"), w->frame);
        /* mouse in monitor pixels, y up; zw = position while button held */
        float mx = mouse[0] - m->x, my = m->h - (mouse[1] - m->y);
        glUniform4f(U(p, "iMouse"), mx, my, mouse[2] > 0 ? mx : -mx, mouse[2] > 0 ? my : -my);
        glUniform4f(U(p, "iDate"), lt.tm_year + 1900, lt.tm_mon, lt.tm_mday, secs);
        glUniform1f(U(p, "iSampleRate"), 44100);
        glUniform1f(U(p, "iBass"), audio_bass());
        glUniform1f(U(p, "iLevel"), audio_level());
        glUniform2f(U(p, "uOffset"), vx, vy);
        float chres[12] = { AUDIO_BINS, 2, 1, w->tw, w->th, 1 };
        glUniform3fv(U(p, "iChannelResolution"), 4, chres);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, audio_texture());
        glUniform1i(U(p, "iChannel0"), 0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, w->tex);
        glUniform1i(U(p, "iChannel1"), 1);
    } else {
        GLuint p = scene_prog;
        glUseProgram(p);
        /* pointer relative to the monitor centre, -1..1 */
        float nx = (mouse[0] - m->x) / (float)m->w * 2 - 1;
        float ny = (mouse[1] - m->y) / (float)m->h * 2 - 1;
        nx = nx < -1 ? -1 : nx > 1 ? 1 : nx;
        ny = ny < -1 ? -1 : ny > 1 ? 1 : ny;
        glUniform3f(U(p, "iResolution"), m->w, m->h, 1);
        glUniform1f(U(p, "iTime"), (float)t);
        glUniform2f(U(p, "uOffset"), vx, vy);
        glUniform2f(U(p, "uTexSize"), w->tw, w->th);
        glUniform2f(U(p, "uMouse"), nx, -ny);
        glUniform1f(U(p, "uParallax"), w->parallax);
        glUniform1f(U(p, "uBass"), audio_bass());
        glUniform1f(U(p, "uBright"), w->brightness);
        glUniform1f(U(p, "uSat"), w->saturation);
        glUniform3fv(U(p, "uTint"), 1, w->tint);
        glUniform1i(U(p, "uFit"), w->fit);
        glUniform1i(U(p, "uEffect"), w->effect);
        glUniform1i(U(p, "uVis"), w->vis);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, w->tex);
        glUniform1i(U(p, "uTex"), 0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, audio_texture());
        glUniform1i(U(p, "uAudio"), 1);
    }
    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    w->frame++;
    glDisable(GL_SCISSOR_TEST);
}

bool save_screenshot(const char *path, int w, int h)
{
    unsigned char *px = malloc((size_t)w * h * 4);
    glReadBuffer(GL_BACK);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    stbi_flip_vertically_on_write(1);
    int ok = ext_in(path, "jpg,jpeg") ? stbi_write_jpg(path, w, h, 4, px, 92)
             : ext_in(path, "bmp")    ? stbi_write_bmp(path, w, h, 4, px)
                                      : stbi_write_png(path, w, h, 4, px, w * 4);
    free(px);
    return ok;
}
