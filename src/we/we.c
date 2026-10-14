/* Top level of the Wallpaper Engine layer: opening projects (scene / video /
 * web), global per-frame state, and presenting a wallpaper into a viewport with
 * Wallpaper Engine's scaling modes. */
#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

WeProject *project_load(const char *dir);
void       project_free(WeProject *p);
WeScene   *scene_new(WeProject *p, int out_w, int out_h, uint32_t clamp_flags);
void       scene_frame(WeScene *s, float mx, float my, bool down, const float uv[4]);
void       scene_free(WeScene *s);
void       scene_pause(WeScene *s, bool paused);

typedef struct WebView WebView;
WebView *web_open(const char *html_path, int w, int h);
bool     web_frame(WebView *v, GLuint tex, int *w, int *h, float mx, float my, bool down);
void     web_pause(WebView *v, bool paused);
void     web_close(WebView *v);
void     web_set_properties(WebView *v, Properties *props);

float g_Time, g_TimeLast, g_Daytime;
AudioSpectrum g_Audio;

struct WeWallpaper {
    WeProject *proj;
    WeScene   *scene;
    Video     *video;
    WebView   *web;
    GLuint     tex;          /* video / web texture */
    int        tw, th;
    int        scaling, clamp;
    double     last_frame;   /* g_Time of the last scene render */
    bool       rendered;
};

static GLuint blit_prog, blit_vao, blit_pos, blit_tc;
static double start_time = -1;

static const char *BLIT_VS =
    "#version 330\n"
    "in vec3 a_Position; in vec2 a_TexCoord; out vec2 v_TexCoord;\n"
    "void main(){ gl_Position = vec4(a_Position, 1.0); v_TexCoord = a_TexCoord; }\n";
static const char *BLIT_FS =
    "#version 330\n"
    "uniform sampler2D g_Texture0; in vec2 v_TexCoord; out vec4 out_FragColor;\n"
    "void main(){ out_FragColor = vec4(texture(g_Texture0, v_TexCoord).rgb, 1.0); }\n";

/* ------------------------------------------------------------------ options */
void we_opt_disable_particles(bool v) { we_settings.disable_particles = v; }
void we_opt_disable_mouse(bool v) { we_settings.disable_mouse = v; }
void we_opt_disable_parallax(bool v) { we_settings.disable_parallax = v; }
void we_opt_audio(bool silent, int volume, bool automute, bool processing)
{
    we_settings.silent = silent;
    we_settings.volume = volume < 0 ? 0 : volume > 128 ? 128 : volume;
    we_settings.automute = automute;
    we_settings.no_audio_processing = !processing;
}
void we_opt_assets(const char *dir) { snprintf(we_settings.assets, sizeof we_settings.assets, "%s", dir); }
void we_opt_property(const char *name, const char *value)
{
    VPUSH(we_settings.prop_names, strdup(name));
    VPUSH(we_settings.prop_values, strdup(value));
}
void we_opt_dump(bool v) { we_settings.dump = v; }
void we_opt_render_debug(const char *spec)
{
    if (!strcmp(spec, "base-only")) we_settings.debug_base_only = true;
    else if (!strcmp(spec, "no-solid-final")) we_settings.debug_no_solid_final = true;
    else if (!strcmp(spec, "pass-log")) we_settings.debug_pass_log = true;
    else if (!strncmp(spec, "object=", 7)) we_settings.debug_object = atoi(spec + 7);
    else if (!strncmp(spec, "skip-object=", 12)) VPUSH(we_settings.skip_objects, atoi(spec + 12));
    else if (!strncmp(spec, "skip-effect=", 12)) VPUSH(we_settings.skip_effects, atoi(spec + 12));
    else we_err("invalid --render-debug mode %s", spec);
}

/* ------------------------------------------------------------------ globals */
void we_global_init(void)
{
    blit_prog = gl_program_from_glsl(BLIT_VS, BLIT_FS, "wallpaper blit");
    glGenVertexArrays(1, &blit_vao);
    glGenBuffers(1, &blit_pos);
    glGenBuffers(1, &blit_tc);
    /* inverted positions so the final texture is shown the right way up */
    const float pos[] = { -1, 1, 0, 1, 1, 0, -1, -1, 0, -1, -1, 0, 1, 1, 0, 1, -1, 0 };
    glBindBuffer(GL_ARRAY_BUFFER, blit_pos);
    glBufferData(GL_ARRAY_BUFFER, sizeof pos, pos, GL_STATIC_DRAW);
    sound_system_init();
    media_init();
}

extern int audio_power(float *power);

static float move_towards(float cur, float target, float max)
{
    if (fabsf(target - cur) <= max) return target;
    return cur + (target > cur ? max : -max);
}

static void update_spectrum(void)
{
    static float dest16[16], dest32[32], dest64[64];
    for (int i = 0; i < 64; i++) {
        g_Audio.audio64[i] = move_towards(g_Audio.audio64[i], dest64[i], 0.3f);
        if (i < 32) g_Audio.audio32[i] = move_towards(g_Audio.audio32[i], dest32[i], 0.3f);
        if (i < 16) g_Audio.audio16[i] = move_towards(g_Audio.audio16[i], dest16[i], 0.3f);
    }
    if (we_settings.no_audio_processing) {
        memset(dest16, 0, sizeof dest16);
        memset(dest32, 0, sizeof dest32);
        memset(dest64, 0, sizeof dest64);
        return;
    }
    float power[512];
    if (audio_power(power) < 512) return;
    /* bands use bins 0,2,4..126 like the original */
    for (int band = 0; band < 64; band++) {
        float mag = power[band * 2];
        float f1 = mag > 0 ? 0.35f * log10f(mag) : 0;
        dest64[band] = fminf(1.0f, f1 * (2.0f - powf(M_E, (1.0f - band / 63.0f) * 1.0f - 0.5f)));
        dest32[band >> 1] = fminf(1.0f, f1 * (2.0f - powf(M_E, (1.0f - band / 31.0f) * 1.0f - 0.5f)));
        dest16[band >> 2] = fminf(1.0f, f1 * (2.0f - powf(M_E, (1.0f - band / 15.0f) * 1.0f - 0.5f)));
    }
}

void we_global_frame(double now)
{
    if (start_time < 0) start_time = now;
    g_TimeLast = g_Time;
    g_Time = (float)(now - start_time);
    time_t t = time(NULL);
    struct tm lt;
    localtime_r(&t, &lt);
    g_Daytime = (lt.tm_hour * 60 + lt.tm_min) / (24.0f * 60.0f);
    update_spectrum();
    media_update();
    sound_automute_tick();
}

void we_global_shutdown(void)
{
    media_shutdown();
    sound_system_shutdown();
    if (blit_prog) glDeleteProgram(blit_prog);
    if (blit_vao) glDeleteVertexArrays(1, &blit_vao);
    if (blit_pos) glDeleteBuffers(1, &blit_pos);
    if (blit_tc) glDeleteBuffers(1, &blit_tc);
}

/* ------------------------------------------------------------------ wallpapers */
WeWallpaper *we_open(const char *dir, int out_w, int out_h, int scaling, int clamp)
{
    WeProject *p = project_load(dir);
    if (!p) return NULL;
    WeWallpaper *w = calloc(1, sizeof *w);
    w->proj = p;
    w->scaling = scaling;
    w->clamp = clamp;
    we_apply_settings(w);
    if (p->type == WE_TYPE_SCENE) {
        uint32_t flags = clamp == WE_CLAMP_CLAMP ? TEXFLAG_CLAMP : clamp == WE_CLAMP_BORDER ? TEXFLAG_CLAMPBORDER : 0;
        w->scene = scene_new(p, out_w, out_h, flags);
        if (!w->scene) goto fail;
        if (we_settings.dump) we_dump(w);
    } else if (p->type == WE_TYPE_VIDEO) {
        char path[PATH_MAX];
        if (!vfs_physical(p->vfs, p->file, path, sizeof path)) {
            we_err("cannot find video file %s", p->file);
            goto fail;
        }
        int fit = scaling == WE_SCALE_STRETCH ? FIT_STRETCH : scaling == WE_SCALE_FIT ? FIT_CONTAIN : FIT_COVER;
        bool sound = cfg.video_sound;
        cfg.video_sound = !we_settings.silent;
        int vol = cfg.volume;
        cfg.volume = we_settings.volume * 100 / 128;
        w->video = video_open(path, out_w, out_h, fit, cfg.fps);
        cfg.video_sound = sound;
        cfg.volume = vol;
        if (!w->video) goto fail;
        video_size(w->video, &w->tw, &w->th);
        glGenTextures(1, &w->tex);
        glBindTexture(GL_TEXTURE_2D, w->tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w->tw, w->th, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    } else if (p->type == WE_TYPE_WEB) {
        char path[PATH_MAX];
        if (!vfs_physical(p->vfs, p->file, path, sizeof path)) {
            we_err("cannot find web page %s", p->file);
            goto fail;
        }
        w->web = web_open(path, out_w, out_h);
        if (!w->web) goto fail;
        web_set_properties(w->web, &p->props);
        glGenTextures(1, &w->tex);
        glBindTexture(GL_TEXTURE_2D, w->tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        w->tw = out_w;
        w->th = out_h;
    } else {
        we_err("%s: project type '%s' is not supported (application wallpapers are Windows programs)", dir,
               js_str(p->json, "type", "?"));
        goto fail;
    }
    we_log("loaded Wallpaper Engine %s '%s'", we_kind(w), p->title);
    return w;
fail:
    we_close(w);
    return NULL;
}

void we_close(WeWallpaper *w)
{
    if (!w) return;
    scene_free(w->scene);
    if (w->video) video_close(w->video);
    if (w->web) web_close(w->web);
    if (w->tex) glDeleteTextures(1, &w->tex);
    project_free(w->proj);
    free(w);
}

const char *we_title(const WeWallpaper *w) { return w->proj->title; }

const char *we_kind(const WeWallpaper *w)
{
    return w->scene ? "scene" : w->video ? "video" : w->web ? "web" : "?";
}

void we_pause(WeWallpaper *w, bool paused)
{
    if (w->scene) scene_pause(w->scene, paused);
    if (w->video) video_pause(w->video, paused);
    if (w->web) web_pause(w->web, paused);
}

bool we_set_property(WeWallpaper *w, const char *name, const char *value)
{
    Property *p = props_find(&w->proj->props, name);
    if (!p) return false;
    prop_set(p, value);
    if (w->scene) script_user_properties_changed(w->scene->script);
    if (w->web) web_set_properties(w->web, &w->proj->props);
    return true;
}

void we_apply_settings(WeWallpaper *w)
{
    for (int i = 0; i < we_settings.prop_names.n; i++) {
        Property *p = props_find(&w->proj->props, we_settings.prop_names.v[i]);
        if (!p) continue;
        we_dbg("applying override value for %s", p->name);
        prop_set(p, we_settings.prop_values.v[i]);
    }
}

void we_list_properties(const WeWallpaper *w, FILE *f)
{
    for (int i = 0; i < w->proj->props.list.n; i++) prop_dump(w->proj->props.list.v[i], f);
}

int we_list_properties_path(const char *path)
{
    char dir[PATH_MAX];
    if (!we_resolve(path, dir, sizeof dir)) {
        we_err("%s is not a Wallpaper Engine project or workshop id", path);
        return 1;
    }
    WeProject *p = project_load(dir);
    if (!p) return 1;
    WeWallpaper tmp = { .proj = p };
    we_apply_settings(&tmp);
    printf("%s (%s)\n", p->title, js_str(p->json, "type", "?"));
    we_list_properties(&tmp, stdout);
    project_free(p);
    return 0;
}

void we_dump(const WeWallpaper *w)
{
    printf("Project: %s\n  type: %s\n  workshop id: %s\n  file: %s\n  audio processing: %d\n", w->proj->title,
           we_kind(w), w->proj->workshop_id, w->proj->file, w->proj->audio_processing);
    if (!w->scene) return;
    WeScene *s = w->scene;
    printf("Scene %gx%g, eye %g %g %g, bloom %d, parallax %d\n", s->width, s->height, s->eye.x, s->eye.y, s->eye.z,
           s->bloom->b, s->parallax->b);
    static const char *kinds[] = { "none", "image", "sound", "particle", "text", "group" };
    for (int i = 0; i < s->objects.n; i++) {
        SceneObj *o = s->objects.v[i];
        printf("  [%d] %-8s %-30s origin %g %g %g%s%s\n", o->id, kinds[o->kind], o->name, o->origin->v[0],
               o->origin->v[1], o->origin->v[2], o->impl ? "" : "  (not rendered)", o->visible->b ? "" : " (hidden)");
        if (o->kind == OBJ_IMAGE) {
            printf("        model %s\n", js_str(o->json, "image", ""));
            const cJSON *e;
            cJSON_ArrayForEach(e, js_get(o->json, "effects")) printf("        effect %s\n", js_str(e, "file", ""));
        } else if (o->kind == OBJ_PARTICLE) {
            const cJSON *pj = js_get(o->json, "particle");
            printf("        particle %s\n", pj && cJSON_IsString(pj) ? pj->valuestring : "(inline)");
        }
    }
}

/* WallpaperState UV computation (vflip = true) */
static void update_us(float vw, float vh, int pw, int ph, float uv[4])
{
    int nw = (int)(vh / ph * pw);
    float nc = nw / 2.0f, vc = vw / 2.0f;
    uv[0] = (nc - vc) / nw;
    uv[1] = (nc + vc) / nw;
}

static void update_vs(float vw, float vh, int pw, int ph, float uv[4])
{
    int nh = (int)(vw / pw * ph);
    float nc = nh / 2.0f, vc = vh / 2.0f;
    uv[2] = (nc - vc) / nh;
    uv[3] = (nc + vc) / nh;
}

static void compute_uvs(int mode, int vw, int vh, int pw, int ph, float uv[4])
{
    uv[0] = 0;
    uv[1] = 1;
    uv[2] = 0;
    uv[3] = 1;
    if (pw <= 0 || ph <= 0 || vw <= 0 || vh <= 0) return;
    if (mode == WE_SCALE_STRETCH) return;
    if (mode == WE_SCALE_FILL || mode == WE_SCALE_FIT) {
        float m1 = (float)vw / pw, m2 = (float)vh / ph;
        float m = mode == WE_SCALE_FILL ? fmaxf(m1, m2) : fminf(m1, m2);
        int npw = pw * m, nph = ph * m;
        if (npw != vw) update_us(vw, vh, npw, nph, uv);
        else if (nph != vh) update_vs(vw, vh, npw, nph, uv);
        return;
    }
    if ((vh > vw && pw >= ph) || (vw > vh && ph > pw)) update_us(vw, vh, pw, ph, uv);
    if ((vw > vh && pw >= ph) || (vh > vw && ph > pw)) update_vs(vw, vh, pw, ph, uv);
}

void we_draw(WeWallpaper *w, int vx, int vy, int vw, int vh, float alpha, float mx, float my, bool down)
{
    float uv[4];
    GLuint tex = 0;
    if (w->scene) {
        compute_uvs(w->scaling, vw, vh, (int)w->scene->width, (int)w->scene->height, uv);
        /* render the scene once per frame even when drawn twice (crossfades) */
        if (!w->rendered || w->last_frame != g_Time) {
            scene_frame(w->scene, mx, my, down, uv);
            w->last_frame = g_Time;
            w->rendered = true;
        }
        tex = w->scene->scene_fbo->ids[0];
    } else if (w->video) {
        uv[0] = 0; uv[1] = 1; uv[2] = 0; uv[3] = 1;
        video_update(w->video, w->tex);
        tex = w->tex;
    } else if (w->web) {
        uv[0] = 0; uv[1] = 1; uv[2] = 0; uv[3] = 1;
        web_frame(w->web, w->tex, &w->tw, &w->th, mx, my, down);
        compute_uvs(w->scaling, vw, vh, w->tw, w->th, uv);
        tex = w->tex;
    } else {
        return;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(vx, vy, vw, vh);
    glEnable(GL_SCISSOR_TEST);
    glScissor(vx, vy, vw, vh);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glColorMask(true, true, true, true);
    if (alpha < 1.0f) {
        glEnable(GL_BLEND);
        glBlendColor(0, 0, 0, alpha);
        glBlendFunc(GL_CONSTANT_ALPHA, GL_ONE_MINUS_CONSTANT_ALPHA);
    } else {
        glDisable(GL_BLEND);
    }
    const float tc[] = { uv[0], uv[2], uv[1], uv[2], uv[0], uv[3], uv[0], uv[3], uv[1], uv[2], uv[1], uv[3] };
    glBindVertexArray(blit_vao);
    glUseProgram(blit_prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glUniform1i(glGetUniformLocation(blit_prog, "g_Texture0"), 0);
    GLint ap = glGetAttribLocation(blit_prog, "a_Position"), at = glGetAttribLocation(blit_prog, "a_TexCoord");
    glBindBuffer(GL_ARRAY_BUFFER, blit_tc);
    glBufferData(GL_ARRAY_BUFFER, sizeof tc, tc, GL_STREAM_DRAW);
    glEnableVertexAttribArray(at);
    glVertexAttribPointer(at, 2, GL_FLOAT, GL_FALSE, 0, NULL);
    glBindBuffer(GL_ARRAY_BUFFER, blit_pos);
    glEnableVertexAttribArray(ap);
    glVertexAttribPointer(ap, 3, GL_FLOAT, GL_FALSE, 0, NULL);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glDisableVertexAttribArray(at);
    glDisableVertexAttribArray(ap);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
}
