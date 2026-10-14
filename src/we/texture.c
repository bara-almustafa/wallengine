/* Wallpaper Engine .tex textures (TEXV0005 containers), framebuffers and
 * framebuffer providers. */
#include "internal.h"

#include "../third_party/stb_image.h"

#include <lz4.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT 0x83F1
#define GL_COMPRESSED_RGBA_S3TC_DXT3_EXT 0x83F2
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3
#endif
#ifndef GL_TEXTURE_MAX_ANISOTROPY
#define GL_TEXTURE_MAX_ANISOTROPY 0x84FE
#endif

typedef struct { const unsigned char *p, *end; bool bad; } Reader;

static uint32_t r_u32(Reader *r)
{
    if (r->p + 4 > r->end) {
        r->bad = true;
        return 0;
    }
    uint32_t v = r->p[0] | r->p[1] << 8 | r->p[2] << 16 | (uint32_t)r->p[3] << 24;
    r->p += 4;
    return v;
}

static float r_f32(Reader *r)
{
    uint32_t u = r_u32(r);
    float f;
    memcpy(&f, &u, 4);
    return f;
}

static const unsigned char *r_bytes(Reader *r, size_t n)
{
    if ((size_t)(r->end - r->p) < n) {
        r->bad = true;
        return NULL;
    }
    const unsigned char *p = r->p;
    r->p += n;
    return p;
}

static void r_skip_cstr(Reader *r)
{
    while (r->p < r->end && *r->p) r->p++;
    if (r->p < r->end) r->p++;
}

typedef struct {
    uint32_t w, h;
    unsigned char *data;   /* uncompressed */
    size_t size;
} Mip;

enum { CV_1 = 1, CV_2, CV_3, CV_4 };

static void set_params(Tex *t, int levels)
{
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, levels > 0 ? levels - 1 : 0);
    GLint wrap = t->flags & TEXFLAG_CLAMP ? GL_CLAMP_TO_EDGE
                 : t->flags & TEXFLAG_CLAMPBORDER ? GL_CLAMP_TO_BORDER
                                                  : GL_REPEAT;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
    if (t->flags & TEXFLAG_NOINTERP) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, levels > 1 ? GL_NEAREST_MIPMAP_NEAREST : GL_NEAREST);
    } else {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, levels > 1 ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    }
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY, 8.0f);
}

/* Upload a mip level in the texture's native format. */
static void upload_level(Tex *t, int level, Mip *m, bool fif)
{
    if (fif) {
        int w, h, n;
        unsigned char *px = stbi_load_from_memory(m->data, (int)m->size, &w, &h, &n, 4);
        if (!px) {
            we_err("cannot decode embedded image in %s: %s", t->name, stbi_failure_reason());
            return;
        }
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexImage2D(GL_TEXTURE_2D, level, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
        stbi_image_free(px);
        return;
    }
    GLenum ifmt, fmt = GL_RGBA, type = GL_UNSIGNED_BYTE;
    bool compressed = false;
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    switch (t->format) {
    case TEXFMT_DXT5: ifmt = GL_COMPRESSED_RGBA_S3TC_DXT5_EXT; compressed = true; break;
    case TEXFMT_DXT3: ifmt = GL_COMPRESSED_RGBA_S3TC_DXT3_EXT; compressed = true; break;
    case TEXFMT_DXT1: ifmt = GL_COMPRESSED_RGBA_S3TC_DXT1_EXT; compressed = true; break;
    case TEXFMT_BC7: ifmt = GL_COMPRESSED_RGBA_BPTC_UNORM; compressed = true; break;
    case TEXFMT_R8: ifmt = GL_R8; fmt = GL_RED; break;
    case TEXFMT_RG88: ifmt = GL_RG8; fmt = GL_RG; break;
    case TEXFMT_RGB888: ifmt = GL_RGB8; fmt = GL_RGB; break;
    case TEXFMT_RGB565: ifmt = GL_RGB565; fmt = GL_RGB; type = GL_UNSIGNED_SHORT_5_6_5; break;
    case TEXFMT_RG1616F: ifmt = GL_RG16F; fmt = GL_RG; type = GL_HALF_FLOAT; break;
    case TEXFMT_R16F: ifmt = GL_R16F; fmt = GL_RED; type = GL_HALF_FLOAT; break;
    case TEXFMT_RGBA16161616F: ifmt = GL_RGBA16F; fmt = GL_RGBA; type = GL_HALF_FLOAT; break;
    case TEXFMT_RGB161616F: ifmt = GL_RGB16F; fmt = GL_RGB; type = GL_HALF_FLOAT; break;
    case TEXFMT_RGBA1010102: ifmt = GL_RGB10_A2; fmt = GL_RGBA; type = GL_UNSIGNED_INT_2_10_10_10_REV; break;
    default: ifmt = GL_RGBA8; break;
    }
    if (compressed)
        glCompressedTexImage2D(GL_TEXTURE_2D, level, ifmt, m->w, m->h, 0, (GLsizei)m->size, m->data);
    else
        glTexImage2D(GL_TEXTURE_2D, level, ifmt, m->w, m->h, 0, fmt, type, m->data);
}

static bool read_mip(Reader *r, int cv, Mip *m)
{
    if (cv == CV_4) {
        r_u32(r);
        r_u32(r);
        r_skip_cstr(r);   /* json */
        r_u32(r);
    }
    m->w = r_u32(r);
    m->h = r_u32(r);
    uint32_t compression = 0;
    int uncompressed = 0;
    if (cv >= CV_2) {
        compression = r_u32(r);
        uncompressed = (int)r_u32(r);
    }
    int compressed = (int)r_u32(r);
    if (r->bad || compressed < 0) return false;
    if (compression == 0) uncompressed = compressed;
    const unsigned char *src = r_bytes(r, compressed);
    if (!src) return false;
    m->size = uncompressed;
    m->data = malloc(uncompressed > 0 ? uncompressed : 1);
    if (compression == 1) {
        int got = LZ4_decompress_safe((const char *)src, (char *)m->data, compressed, uncompressed);
        if (got < 0) {
            we_err("LZ4 decompression failed");
            return false;
        }
    } else {
        memcpy(m->data, src, uncompressed);
    }
    return true;
}

static void spritesheet_from_meta(WeProject *p, Tex *t, const char *name)
{
    char path[PATH_MAX];
    snprintf(path, sizeof path, "materials/%s.tex-json", name);
    char *data = (char *)vfs_read(p->vfs, path, NULL);
    if (!data) return;
    cJSON *j = cJSON_Parse(data);
    free(data);
    const cJSON *seq = js_get(j, "spritesheetsequences");
    const cJSON *first = seq ? cJSON_GetArrayItem(seq, 0) : NULL;
    if (first) {
        int frames = js_int(first, "frames", 0);
        float fw = js_num(first, "width", 0), fh = js_num(first, "height", 0);
        if (frames > 0 && fw > 0 && fh > 0 && t->width > 0 && t->height > 0) {
            t->ss_cols = (int)roundf(t->width / fw);
            t->ss_rows = (int)roundf(t->height / fh);
            t->ss_frames = frames;
            t->ss_duration = js_num(first, "duration", 1.0);
        }
    }
    cJSON_Delete(j);
}

static void parse_frames(Tex *t, Reader *r)
{
    char magic[9] = { 0 };
    const unsigned char *m = r_bytes(r, 9);
    if (!m) return;
    memcpy(magic, m, 9);
    int av = !strncmp(magic, "TEXS0001", 8) ? 1 : !strncmp(magic, "TEXS0002", 8) ? 2 : !strncmp(magic, "TEXS0003", 8) ? 3 : 0;
    if (!av) {
        we_err("%s: unknown animation block %.8s", t->name, magic);
        return;
    }
    uint32_t count = r_u32(r);
    if (av == 3) {
        t->gif_w = r_u32(r);
        t->gif_h = r_u32(r);
    }
    for (uint32_t i = 0; i < count && !r->bad; i++) {
        TexFrame f = { 0 };
        f.frame = r_u32(r);
        f.frametime = r_f32(r);
        if (av == 1) {
            f.x = r_u32(r);
            f.y = r_u32(r);
            f.width1 = r_u32(r);
            r_u32(r);
            r_u32(r);
            f.height1 = r_u32(r);
        } else {
            f.x = r_f32(r);
            f.y = r_f32(r);
            f.width1 = r_f32(r);
            f.width2 = r_f32(r);
            f.height2 = r_f32(r);
            f.height1 = r_f32(r);
        }
        VPUSH(t->frames, f);
    }
    if ((av == 1 || av == 2) && t->frames.n) {
        t->gif_w = (int)t->frames.v[0].width1;
        t->gif_h = (int)t->frames.v[0].height1;
    }
    if (t->frames.n && t->width > 0 && t->height > 0) {
        float fw = t->frames.v[0].width1, fh = t->frames.v[0].height1;
        if (fw > 0 && fh > 0) {
            int cols = (int)round((double)t->width / fw), rows = (int)round((double)t->height / fh);
            if (cols > 0 && rows > 0 && cols * rows >= t->frames.n) {
                t->ss_cols = cols;
                t->ss_rows = rows;
                t->ss_frames = t->frames.n;
                t->ss_duration = 0;
                for (int i = 0; i < t->frames.n; i++) t->ss_duration += t->frames.v[i].frametime;
            }
        }
    }
}

Tex *tex_load(WeProject *p, const char *name)
{
    char path[PATH_MAX];
    snprintf(path, sizeof path, "materials/%s.tex", name);
    size_t len;
    unsigned char *data = vfs_read(p->vfs, path, &len);
    if (!data) return NULL;
    Reader r = { data, data + len, false };
    Tex *t = calloc(1, sizeof *t);
    t->kind = TEXKIND_FILE;
    t->name = strdup(name);
    t->refs = 1;
    const unsigned char *m1 = r_bytes(&r, 9), *m2 = r_bytes(&r, 9);
    if (!m1 || !m2 || strncmp((const char *)m1, "TEXV0005", 8) || strncmp((const char *)m2, "TEXI0001", 8)) {
        we_err("%s: not a TEXV0005/TEXI0001 texture", path);
        free(data);
        tex_free(t);
        return NULL;
    }
    t->format = r_u32(&r);
    t->flags = r_u32(&r);
    t->tex_w = r_u32(&r);
    t->tex_h = r_u32(&r);
    t->width = r_u32(&r);
    t->height = r_u32(&r);
    r_u32(&r);
    const unsigned char *cm = r_bytes(&r, 9);
    if (!cm) goto bad;
    uint32_t image_count = r_u32(&r);
    int cv = 0, fif = -1;
    bool mp4 = false;
    if (!strncmp((const char *)cm, "TEXB0004", 8)) {
        fif = (int)r_u32(&r);
        mp4 = r_u32(&r) == 1;
        if (fif == -1 && mp4) fif = 35;
        /* TEXB0004 mips carry an extra header only for mp4 data; otherwise it is TEXB0003 */
        cv = fif == 35 ? CV_4 : CV_3;
    } else if (!strncmp((const char *)cm, "TEXB0003", 8)) {
        cv = CV_3;
        fif = (int)r_u32(&r);
    } else if (!strncmp((const char *)cm, "TEXB0002", 8)) {
        cv = CV_2;
    } else if (!strncmp((const char *)cm, "TEXB0001", 8)) {
        cv = CV_1;
    } else {
        we_err("%s: unknown container %.8s", path, cm);
        goto bad;
    }
    bool is_video = mp4 || (t->flags & TEXFLAG_VIDEO);
    t->animated = (t->flags & TEXFLAG_GIF) != 0;

    Mip **images = calloc(image_count ? image_count : 1, sizeof(Mip *));
    int *levels = calloc(image_count ? image_count : 1, sizeof(int));
    for (uint32_t i = 0; i < image_count && !r.bad; i++) {
        uint32_t mc = r_u32(&r);
        if (mc > 32) {
            r.bad = true;
            break;
        }
        images[i] = calloc(mc ? mc : 1, sizeof(Mip));
        for (uint32_t m = 0; m < mc; m++) {
            if (!read_mip(&r, cv, &images[i][m])) {
                r.bad = true;
                break;
            }
            levels[i]++;
        }
    }
    if (t->animated && !r.bad) parse_frames(t, &r);
    spritesheet_from_meta(p, t, name);

    t->nids = image_count;
    t->ids = calloc(image_count ? image_count : 1, sizeof(GLuint));
    t->img_w = calloc(image_count ? image_count : 1, sizeof(int));
    t->img_h = calloc(image_count ? image_count : 1, sizeof(int));
    for (uint32_t i = 0; i < image_count; i++) {
        if (levels[i]) {
            t->img_w[i] = images[i][0].w;
            t->img_h[i] = images[i][0].h;
        }
    }

    if (is_video && image_count && levels[0]) {
        /* the mipmap holds an mp4 file: decode it with the video pipeline */
        const char *rt = getenv("XDG_RUNTIME_DIR");
        snprintf(t->video_tmp, sizeof t->video_tmp, "%s/wallengine-tex-%d-%p.mp4", rt && *rt ? rt : "/tmp",
                 (int)getpid(), (void *)t);
        FILE *f = fopen(t->video_tmp, "wb");
        if (f) {
            fwrite(images[0][0].data, 1, images[0][0].size, f);
            fclose(f);
        }
        int vw = t->tex_w > 0 ? t->tex_w : 512, vh = t->tex_h > 0 ? t->tex_h : 512;
        t->video = video_open(t->video_tmp, vw, vh, FIT_STRETCH, cfg.fps);
        glGenTextures(1, &t->ids[0]);
        glBindTexture(GL_TEXTURE_2D, t->ids[0]);
        int w = vw, h = vh;
        if (t->video) video_size(t->video, &w, &h);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        set_params(t, 1);
        t->img_w[0] = t->tex_w = w;
        t->img_h[0] = t->tex_h = h;
        t->res[0] = t->res[2] = w;
        t->res[1] = t->res[3] = h;
        t->width = w;
        t->height = h;
        t->format = TEXFMT_ARGB8888;
    } else {
        glGenTextures(image_count, t->ids);
        for (uint32_t i = 0; i < image_count; i++) {
            glBindTexture(GL_TEXTURE_2D, t->ids[i]);
            set_params(t, levels[i]);
            for (int l = 0; l < levels[i]; l++) upload_level(t, l, &images[i][l], fif != -1);
        }
        if (t->animated) {
            t->res[0] = t->tex_w;
            t->res[1] = t->tex_h;
            t->res[2] = t->gif_w;
            t->res[3] = t->gif_h;
        } else if (fif != -1 && image_count && levels[0]) {
            t->res[0] = images[0][0].w;
            t->res[1] = images[0][0].h;
            t->res[2] = t->width;
            t->res[3] = t->height;
        } else {
            t->res[0] = t->tex_w;
            t->res[1] = t->tex_h;
            t->res[2] = t->width;
            t->res[3] = t->height;
        }
        if (fif != -1) t->format = TEXFMT_ARGB8888;
    }
    for (uint32_t i = 0; i < image_count; i++) {
        for (int l = 0; l < levels[i]; l++) free(images[i][l].data);
        free(images[i]);
    }
    free(images);
    free(levels);
    free(data);
    if (r.bad) we_err("%s: truncated texture data", path);
    we_dbg("texture %s: %dx%d (%dx%d) fmt=%d flags=%u images=%u%s", name, t->width, t->height, t->tex_w,
           t->tex_h, t->format, t->flags, image_count, t->animated ? " animated" : "");
    return t;
bad:
    free(data);
    tex_free(t);
    return NULL;
}

Tex *fbo_new(const char *name, int format, uint32_t flags, float scale, int rw, int rh, int tw, int th)
{
    Tex *t = calloc(1, sizeof *t);
    t->kind = TEXKIND_FBO;
    t->name = strdup(name ? name : "");
    t->format = format;
    t->flags = flags;
    t->scale = scale;
    t->refs = 1;
    if (tw < 1) tw = 1;
    if (th < 1) th = 1;
    if (rw < 1) rw = 1;
    if (rh < 1) rh = 1;
    t->nids = 1;
    t->ids = calloc(1, sizeof(GLuint));
    t->img_w = calloc(1, sizeof(int));
    t->img_h = calloc(1, sizeof(int));
    t->img_w[0] = t->tex_w = tw;
    t->img_h[0] = t->tex_h = th;
    t->width = rw;
    t->height = rh;
    t->res[0] = tw;
    t->res[1] = th;
    t->res[2] = rw;
    t->res[3] = rh;
    TexFrame f = { 0, 0, 0, 0, (float)tw, (float)rw, (float)th, (float)rh };
    VPUSH(t->frames, f);

    glGenFramebuffers(1, &t->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    glGenTextures(1, &t->ids[0]);
    glBindTexture(GL_TEXTURE_2D, t->ids[0]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    GLint wrap = flags & TEXFLAG_CLAMP ? GL_CLAMP_TO_EDGE : flags & TEXFLAG_CLAMPBORDER ? GL_CLAMP_TO_BORDER : GL_REPEAT;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
    GLint filter = flags & TEXFLAG_NOINTERP ? GL_NEAREST : GL_LINEAR;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY, 8.0f);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->ids[0], 0);
    GLenum db = GL_COLOR_ATTACHMENT0;
    glDrawBuffers(1, &db);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        we_err("framebuffer %s (%dx%d) is incomplete", t->name, tw, th);
    GLfloat prev[4];
    glGetFloatv(GL_COLOR_CLEAR_VALUE, prev);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glClearColor(prev[0], prev[1], prev[2], prev[3]);
    return t;
}

Tex *tex_dummy(int w, int h)
{
    Tex *t = fbo_new("", TEXFMT_ARGB8888, 0, 1, w, h, w, h);
    return t;
}

void tex_free(Tex *t)
{
    if (!t) return;
    if (--t->refs > 0) return;
    if (t->video) video_close(t->video);
    if (t->video_tmp[0]) unlink(t->video_tmp);
    if (t->nids && t->ids) glDeleteTextures(t->nids, t->ids);
    if (t->fbo) glDeleteFramebuffers(1, &t->fbo);
    free(t->ids);
    free(t->img_w);
    free(t->img_h);
    VFREE(t->frames);
    free(t->name);
    free(t);
}

GLuint tex_id(const Tex *t, uint32_t img)
{
    if (!t || !t->nids) return 0;
    return img < (uint32_t)t->nids ? t->ids[img] : t->ids[0];
}

int tex_width(const Tex *t, uint32_t img)
{
    if (t->kind == TEXKIND_FBO) return t->tex_w;
    return img < (uint32_t)t->nids ? t->img_w[img] : t->tex_w;
}

int tex_height(const Tex *t, uint32_t img)
{
    if (t->kind == TEXKIND_FBO) return t->tex_h;
    return img < (uint32_t)t->nids ? t->img_h[img] : t->tex_h;
}

int tex_real_w(const Tex *t) { return t->animated ? t->gif_w : t->width; }
int tex_real_h(const Tex *t) { return t->animated ? t->gif_h : t->height; }

void tex_update(Tex *t)
{
    if (t && t->video) video_update(t->video, t->ids[0]);
}

/* ------------------------------------------------------------------ fbo providers */
Tex *fbop_create(FboProvider *p, const char *name, int format, uint32_t flags, float scale, int rw, int rh,
                 int tw, int th)
{
    for (int i = 0; i < p->fbos.n; i++)
        if (!strcmp(p->fbos.v[i]->name, name)) {
            tex_free(p->fbos.v[i]);
            p->fbos.v[i] = fbo_new(name, format, flags, scale, rw, rh, tw, th);
            return p->fbos.v[i];
        }
    Tex *t = fbo_new(name, format, flags, scale, rw, rh, tw, th);
    VPUSH(p->fbos, t);
    return t;
}

void fbop_alias(FboProvider *p, const char *alias, const char *original)
{
    Tex *t = fbop_find(p, original);
    if (!t) return;
    VPUSH(p->alias_names, strdup(alias));
    VPUSH(p->alias_targets, t);
}

Tex *fbop_find(const FboProvider *p, const char *name)
{
    for (; p; p = p->parent) {
        for (int i = p->fbos.n - 1; i >= 0; i--)
            if (!strcmp(p->fbos.v[i]->name, name)) return p->fbos.v[i];
        for (int i = 0; i < p->alias_names.n; i++)
            if (!strcmp(p->alias_names.v[i], name)) return p->alias_targets.v[i];
    }
    return NULL;
}

void fbop_free(FboProvider *p)
{
    for (int i = 0; i < p->fbos.n; i++) tex_free(p->fbos.v[i]);
    VFREE(p->fbos);
    for (int i = 0; i < p->alias_names.n; i++) free(p->alias_names.v[i]);
    VFREE(p->alias_names);
    VFREE(p->alias_targets);
}
