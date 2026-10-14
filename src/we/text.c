/* Text layers (port of linux-wallpaperengine's CText, using stb_truetype). */
#include "internal.h"

#include "../third_party/stb_truetype.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *FONT_CANDIDATES[] = {
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
    "/usr/share/fonts/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
    "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
    "/usr/share/fonts/noto/NotoSans-Regular.ttf",
    "/usr/share/fonts/truetype/freefont/FreeSans.ttf",
};

static const char *VS =
    "#version 330 core\n"
    "layout(location = 0) in vec2 aPos;\n"
    "layout(location = 1) in vec2 aUV;\n"
    "uniform mat4 uMVP;\n"
    "out vec2 vUV;\n"
    "void main() { vUV = aUV; gl_Position = uMVP * vec4(aPos, 0.0, 1.0); }\n";
static const char *FS =
    "#version 330 core\n"
    "in vec2 vUV; uniform sampler2D uTexture; uniform vec4 uColor; out vec4 FragColor;\n"
    "void main() { float c = texture(uTexture, vUV).r; FragColor = vec4(uColor.rgb, uColor.a * c); }\n";

typedef struct {
    WeScene *scene;
    DynVal  *text, *point_size, *color, *alpha;
    unsigned char *font_data;
    stbtt_fontinfo font;
    GLuint   prog, vao, vbo, tex;
    GLint    u_mvp, u_color, u_tex;
    float    qw, qh;
    char    *last_text;
    unsigned last_px;
    bool     valid;
} Text;

static bool load_font(WeScene *s, Text *t, const char *font)
{
    if (font && *font && strncmp(font, "systemfont_", 11)) {
        size_t len;
        t->font_data = vfs_read(s->project->vfs, font, &len);
        if (t->font_data && stbtt_InitFont(&t->font, t->font_data, stbtt_GetFontOffsetForIndex(t->font_data, 0)))
            return true;
        if (t->font_data) we_err("cannot use font %s, falling back to a system font", font);
        free(t->font_data);
        t->font_data = NULL;
    }
    for (size_t i = 0; i < sizeof FONT_CANDIDATES / sizeof *FONT_CANDIDATES; i++) {
        FILE *f = fopen(FONT_CANDIDATES[i], "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        t->font_data = malloc(n);
        if (fread(t->font_data, 1, n, f) != (size_t)n) n = 0;
        fclose(f);
        if (n && stbtt_InitFont(&t->font, t->font_data, 0)) return true;
        free(t->font_data);
        t->font_data = NULL;
    }
    we_err("no usable font found for text layers");
    return false;
}

static unsigned pixel_size(SceneObj *o, Text *t)
{
    float avg = (o->scale->v[0] + o->scale->v[1]) * 0.5f;
    float comp = avg > 0 && avg < 1 ? fminf(1.0f / avg, 32.0f) : 1.0f;
    float px = t->point_size->f * comp;
    return px < 1 ? 1 : (unsigned)px;
}

/* decode one UTF-8 code point */
static int utf8(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    int c = *p++;
    if (c >= 0xF0 && p[0] && p[1] && p[2]) {
        c = (c & 7) << 18 | (p[0] & 63) << 12 | (p[1] & 63) << 6 | (p[2] & 63);
        p += 3;
    } else if (c >= 0xE0 && p[0] && p[1]) {
        c = (c & 15) << 12 | (p[0] & 63) << 6 | (p[1] & 63);
        p += 2;
    } else if (c >= 0xC0 && p[0]) {
        c = (c & 31) << 6 | (p[0] & 63);
        p += 1;
    }
    *s = (const char *)p;
    return c;
}

static void rebuild(Text *t, const char *text, unsigned px)
{
    float scale = stbtt_ScaleForPixelHeight(&t->font, px);
    int asc, desc, gap;
    stbtt_GetFontVMetrics(&t->font, &asc, &desc, &gap);
    int ascent = (int)ceilf(asc * scale), descent = (int)ceilf(-desc * scale);
    int width = 0;
    for (const char *s = text; *s;) {
        int cp = utf8(&s), adv, lsb;
        stbtt_GetCodepointHMetrics(&t->font, cp, &adv, &lsb);
        width += (int)ceilf(adv * scale);
    }
    if (width < 1) width = 1;
    int height = ascent + descent;
    if (height < 1) height = 1;
    unsigned char *pix = calloc((size_t)width * height, 1);
    int pen = 0;
    for (const char *s = text; *s;) {
        int cp = utf8(&s), adv, lsb, x0, y0, x1, y1;
        stbtt_GetCodepointHMetrics(&t->font, cp, &adv, &lsb);
        stbtt_GetCodepointBitmapBox(&t->font, cp, scale, scale, &x0, &y0, &x1, &y1);
        int gx = pen + x0, gy = ascent + y0, gw = x1 - x0, gh = y1 - y0;
        if (gw > 0 && gh > 0 && gx >= 0 && gy >= 0 && gx + gw <= width && gy + gh <= height)
            stbtt_MakeCodepointBitmap(&t->font, pix + gy * width + gx, gw, gh, width, scale, scale, cp);
        pen += (int)ceilf(adv * scale);
    }
    bool first = !t->tex;
    if (first) glGenTextures(1, &t->tex);
    glBindTexture(GL_TEXTURE_2D, t->tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, width, height, 0, GL_RED, GL_UNSIGNED_BYTE, pix);
    if (first) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    free(pix);
    t->qw = width;
    t->qh = height;
    free(t->last_text);
    t->last_text = strdup(text);
    float hx = t->qw / 2, hy = t->qh / 2;
    /* quad centered on the origin; v = 0 is the top of the glyph bitmap */
    float verts[] = { -hx, -hy, 0, 0, hx, -hy, 1, 0, hx, hy, 1, 1, -hx, -hy, 0, 0, hx, hy, 1, 1, -hx, hy, 0, 1 };
    bool firstq = !t->vao;
    if (firstq) {
        glGenVertexArrays(1, &t->vao);
        glGenBuffers(1, &t->vbo);
    }
    glBindVertexArray(t->vao);
    glBindBuffer(GL_ARRAY_BUFFER, t->vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof verts, verts, GL_DYNAMIC_DRAW);
    if (firstq) {
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)(2 * sizeof(float)));
    }
}

static void text_render(SceneObj *o)
{
    Text *t = o->impl;
    if (!t->valid || !o->visible->b) return;
    WeScene *s = t->scene;
    const char *cur = t->last_text;
    if (t->text->type == DV_STRING && t->text->s) cur = *t->text->s ? t->text->s : " ";
    else if (t->text->type != DV_NULL && t->text->type != DV_STRING) {
        static char num[64];
        dv_to_string(t->text, num, sizeof num);
        cur = num;
    }
    unsigned px = pixel_size(o, t);
    if (px != t->last_px || strcmp(cur, t->last_text)) {
        t->last_px = px;
        char *copy = strdup(cur);
        rebuild(t, copy, px);
        free(copy);
    }
    float sw = s->width, sh = s->height;
    /* same convention as image layers (y up on screen) */
    vec3 org = v3(o->origin->v[0] - sw * 0.5f, sh * 0.5f - o->origin->v[1], o->origin->v[2]);
    mat4 model = m4scale(m4translate(m4id(), org), v3(o->scale->v[0], o->scale->v[1], o->scale->v[2]));
    mat4 mvp = m4mul(m4mul(s->projection, s->lookat), model);
    glBindFramebuffer(GL_FRAMEBUFFER, s->scene_fbo->fbo);
    glViewport(0, 0, s->scene_fbo->width, s->scene_fbo->height);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(t->prog);
    glUniformMatrix4fv(t->u_mvp, 1, GL_FALSE, mvp.m);
    glUniform4f(t->u_color, t->color->v[0], t->color->v[1], t->color->v[2], t->color->v[3] * t->alpha->f);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, t->tex);
    glUniform1i(t->u_tex, 0);
    glBindVertexArray(t->vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(s->vao);
}

static void text_destroy(SceneObj *o)
{
    Text *t = o->impl;
    if (t->vbo) glDeleteBuffers(1, &t->vbo);
    if (t->vao) glDeleteVertexArrays(1, &t->vao);
    if (t->prog) glDeleteProgram(t->prog);
    if (t->tex) glDeleteTextures(1, &t->tex);
    free(t->font_data);
    free(t->last_text);
    free(t);
}

bool text_create(WeScene *s, SceneObj *o)
{
    WeProject *p = s->project;
    Arena *a = &p->arena;
    const cJSON *j = o->json;
    Text *t = calloc(1, sizeof *t);
    o->impl = t;
    o->render = text_render;
    o->destroy = text_destroy;
    t->scene = s;
    const cJSON *tj = js_get(j, "text");
    t->text = tj ? us_parse(a, tj, &p->props, false) : dv_new_float(a, 0);
    /* text values are strings even when they contain spaces (which would parse as vectors) */
    const cJSON *raw = tj && cJSON_IsObject(tj) ? cJSON_GetObjectItemCaseSensitive(tj, "value") : tj;
    if (raw && cJSON_IsString(raw) && !t->text->prop) dv_set_string(t->text, raw->valuestring);
    t->point_size = us_float(a, j, "pointsize", &p->props, 32);
    t->color = us_color(a, j, "color", &p->props, v4(1, 1, 1, 1));
    t->alpha = us_float(a, j, "alpha", &p->props, 1);
    sceneobj_register(o, "color", t->color);
    sceneobj_register(o, "alpha", t->alpha);
    sceneobj_register(o, "pointSize", t->point_size);
    bool scripted = t->text->script != NULL;
    const char *initial = t->text->type == DV_STRING && t->text->s ? t->text->s : "";
    if (!*initial && !scripted) return true;   /* nothing to draw */
    if (!load_font(s, t, js_str(j, "font", ""))) return false;
    t->prog = gl_program_from_glsl(VS, FS, "text");
    if (!t->prog) return false;
    t->u_mvp = glGetUniformLocation(t->prog, "uMVP");
    t->u_color = glGetUniformLocation(t->prog, "uColor");
    t->u_tex = glGetUniformLocation(t->prog, "uTexture");
    t->last_px = pixel_size(o, t);
    rebuild(t, *initial ? initial : " ", t->last_px);
    sceneobj_register(o, "text", t->text);   /* scripted text runs through the script engine */
    t->valid = t->tex && t->vao;
    return true;
}
