/* Image layers (port of linux-wallpaperengine's CImage). */
#include "internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    Renderable r;
    DynVal    *alpha, *color, *parallax_depth, *blend_mode, *brightness;
    char      *alignment;
    vec2       json_size;
    Model     *model;
    VEC(ImageEffect *) effects;

    GLuint     scene_pos, copy_pos, pass_pos, tc_copy, tc_pass;
    GLuint     puppet_pos, puppet_tc, puppet_idx;
    GLsizei    puppet_count;
    bool       has_puppet;
    float     *puppet_raw;
    int        puppet_nraw;

    mat4       mvp_screen, mvp_pass, mvp_copy, mvp_screen_inv, mvp_pass_inv, mvp_copy_inv, model_m, viewproj;
    Tex       *main_fbo, *sub_fbo, *cur_main, *cur_sub;
    VEC(Pass *) passes;
    VEC(FboProvider *) providers;
    vec4       pos;
    vec3       center;
    vec2       size;
    bool       initialized;
    Tex       *dummy;
} Image;

typedef struct { vec3 origin, scale; float angle; } Xform;

static vec2 rot2(vec2 v, float a)
{
    float c = cosf(a), s = sinf(a);
    return v2(v.x * c - v.y * s, v.x * s + v.y * c);
}

static Xform local_xform(SceneObj *o)
{
    Xform x;
    x.origin = v3(o->origin->v[0], o->origin->v[1], o->origin->v[2]);
    x.scale = v3(o->scale->v[0], o->scale->v[1], o->scale->v[2]);
    x.angle = o->angles->v[2];
    return x;
}

static Xform resolve_xform(WeScene *s, SceneObj *o)
{
    SceneObj *chain[33];
    int n = 0;
    chain[n++] = o;
    SceneObj *cur = o;
    while (cur->has_parent && n <= 32) {
        SceneObj *p = scene_object(s, cur->parent);
        if (!p) break;
        cur = p;
        chain[n++] = cur;
    }
    Xform r = local_xform(chain[n - 1]);
    for (int i = n - 2; i >= 0; i--) {
        Xform l = local_xform(chain[i]);
        vec2 off = rot2(v2(l.origin.x * r.scale.x, l.origin.y * r.scale.y), r.angle);
        l.origin.x = r.origin.x + off.x;
        l.origin.y = r.origin.y + off.y;
        l.origin.z = r.origin.z + l.origin.z * r.scale.z;
        r = (Xform){ l.origin, v3mul(l.scale, r.scale), l.angle + r.angle };
    }
    return r;
}

static vec2 image_size(Image *im)
{
    if (!im->r.texture) return im->json_size;
    return v2(tex_real_w(im->r.texture), tex_real_h(im->r.texture));
}

static GLuint make_buffer(const float *data, size_t bytes)
{
    GLuint b;
    glGenBuffers(1, &b);
    glBindBuffer(GL_ARRAY_BUFFER, b);
    glBufferData(GL_ARRAY_BUFFER, bytes, data, GL_STATIC_DRAW);
    return b;
}

static void scene_position(Image *im, vec3 origin, vec2 size, vec3 scale, float sw, float sh)
{
    vec2 ss = v2(size.x * scale.x, size.y * scale.y);
    im->pos.x = origin.x - ss.x / 2;
    im->pos.w = origin.y + ss.y / 2;
    im->pos.z = origin.x + ss.x / 2;
    im->pos.y = origin.y - ss.y / 2;
    if (strstr(im->alignment, "top")) {
        im->pos.y -= ss.y / 2;
        im->pos.w -= ss.y / 2;
    } else if (strstr(im->alignment, "bottom")) {
        im->pos.y += ss.y / 2;
        im->pos.w += ss.y / 2;
    }
    if (strstr(im->alignment, "left")) {
        im->pos.x += ss.x / 2;
        im->pos.z += ss.x / 2;
    } else if (strstr(im->alignment, "right")) {
        im->pos.x -= ss.x / 2;
        im->pos.z -= ss.x / 2;
    }
    im->pos.x -= sw / 2;
    im->pos.y = sh / 2 - im->pos.y;
    im->pos.z -= sw / 2;
    im->pos.w = sh / 2 - im->pos.w;
}

static void copy_geometry(Image *im, vec2 size, float out_tc[12], float out_pos[18])
{
    float w = 1, h = 1, x = 0, y = 0;
    Tex *t = im->r.texture;
    if (t && !t->animated && (tex_width(t, 0) != tex_real_w(t) || tex_height(t, 0) != tex_real_h(t))) {
        w = (float)tex_real_w(t) / tex_width(t, 0);
        h = (float)tex_real_h(t) / tex_height(t, 0);
    }
    float rw = size.x, rh = size.y, rx = 0, ry = 0;
    if (im->model->passthrough) {
        w = h = 1;
        rx = im->pos.x;
        ry = im->pos.w;
        rw = im->pos.z;
        rh = im->pos.y;
        if (im->model->fullscreen) {
            rx = ry = -1;
            rw = rh = 1;
        }
    }
    float tc[12] = { x, h, x, y, w, h, w, h, x, y, w, y };
    float pos[18] = { rx, rh, 0, rx, ry, 0, rw, rh, 0, rw, rh, 0, rx, ry, 0, rw, ry, 0 };
    memcpy(out_tc, tc, sizeof tc);
    memcpy(out_pos, pos, sizeof pos);
}

static void puppet_positions(Image *im, vec2 size)
{
    if (!im->puppet_raw) return;
    float *pos = malloc(sizeof(float) * im->puppet_nraw);
    for (int i = 0; i + 2 < im->puppet_nraw; i += 3) {
        pos[i] = size.x / 2 + im->puppet_raw[i];
        pos[i + 1] = size.y / 2 - im->puppet_raw[i + 1];
        pos[i + 2] = im->puppet_raw[i + 2];
    }
    if (!im->puppet_pos) glGenBuffers(1, &im->puppet_pos);
    glBindBuffer(GL_ARRAY_BUFFER, im->puppet_pos);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) * im->puppet_nraw, pos, GL_DYNAMIC_DRAW);
    free(pos);
}

static uint32_t rd32le(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static bool load_puppet(Image *im, vec2 size)
{
    if (!im->model->puppet) return false;
    size_t len;
    unsigned char *d = vfs_read(im->r.scene->project->vfs, im->model->puppet, &len);
    if (!d) {
        we_err("cannot read puppet %s", im->model->puppet);
        return false;
    }
    bool ok = false;
    const size_t marker = 9, header = 8, stride = 80, uv_off = 72;
    if (len < marker || (strncmp((char *)d, "MDLV0021", 8) && strncmp((char *)d, "MDLV0023", 8))) {
        we_err("unsupported puppet model %s", im->model->puppet);
        goto done;
    }
    size_t mdls = len;
    for (size_t o = marker; o + 4 < len; o++)
        if (!memcmp(d + o, "MDLS", 4)) {
            mdls = o;
            break;
        }
    size_t hdr = 0;
    uint32_t vbytes = 0, ibytes = 0;
    bool found = false;
    for (size_t o = marker; o + header + 4 < mdls; o++) {
        uint32_t vb = rd32le(d + o + 4);
        size_t voff = o + header, ilen_off = voff + vb;
        if (!vb || vb % stride || ilen_off + 4 > mdls) continue;
        uint32_t ib = rd32le(d + ilen_off);
        if (!ib || ib % 6 || ilen_off + 4 + ib > mdls) continue;
        hdr = o;
        vbytes = vb;
        ibytes = ib;
        found = true;
        break;
    }
    if (!found) {
        we_err("no usable mesh block in puppet %s", im->model->puppet);
        goto done;
    }
    size_t nv = vbytes / stride, voff = hdr + header, ioff = voff + vbytes + 4, ni = ibytes / 2;
    im->puppet_nraw = nv * 3;
    im->puppet_raw = malloc(sizeof(float) * im->puppet_nraw);
    float *tc = malloc(sizeof(float) * nv * 2);
    for (size_t i = 0; i < nv; i++) {
        const unsigned char *v = d + voff + i * stride;
        memcpy(&im->puppet_raw[i * 3], v, 12);
        memcpy(&tc[i * 2], v + uv_off, 8);
    }
    unsigned short *idx = malloc(ibytes);
    memcpy(idx, d + ioff, ibytes);
    for (size_t i = 0; i < ni; i++)
        if (idx[i] >= nv) {
            we_err("invalid puppet index in %s", im->model->puppet);
            free(tc);
            free(idx);
            free(im->puppet_raw);
            im->puppet_raw = NULL;
            goto done;
        }
    puppet_positions(im, size);
    im->puppet_tc = make_buffer(tc, sizeof(float) * nv * 2);
    glGenBuffers(1, &im->puppet_idx);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, im->puppet_idx);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, ibytes, idx, GL_STATIC_DRAW);
    im->puppet_count = ni;
    free(tc);
    free(idx);
    we_dbg("loaded puppet mesh %s: %zu vertices, %zu indices", im->model->puppet, nv, ni);
    ok = true;
done:
    free(d);
    return ok;
}

static void puppet_setup(Pass *p, void *user)
{
    Image *im = user;
    if (p->a_pos >= 0) {
        glEnableVertexAttribArray(p->a_pos);
        glBindBuffer(GL_ARRAY_BUFFER, im->puppet_pos);
        glVertexAttribPointer(p->a_pos, 3, GL_FLOAT, GL_FALSE, 0, NULL);
    }
    if (p->a_tc >= 0) {
        glEnableVertexAttribArray(p->a_tc);
        glBindBuffer(GL_ARRAY_BUFFER, im->puppet_tc);
        glVertexAttribPointer(p->a_tc, 2, GL_FLOAT, GL_FALSE, 0, NULL);
    }
}

static void puppet_draw(Pass *p, void *user)
{
    Image *im = user;
    GLint cur = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &cur);
    if ((GLuint)cur != im->r.scene->scene_fbo->fbo) {
        GLfloat prev[4];
        glGetFloatv(GL_COLOR_CLEAR_VALUE, prev);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        glClearColor(prev[0], prev[1], prev[2], prev[3]);
    }
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, im->puppet_idx);
    glDrawElements(GL_TRIANGLES, im->puppet_count, GL_UNSIGNED_SHORT, NULL);
    (void)p;
}

static void puppet_cleanup(Pass *p, void *user)
{
    (void)user;
    if (p->a_pos >= 0) glDisableVertexAttribArray(p->a_pos);
    if (p->a_tc >= 0) glDisableVertexAttribArray(p->a_tc);
}

static FboProvider *new_provider(Image *im)
{
    FboProvider *fp = calloc(1, sizeof *fp);
    fp->parent = &im->r.fbos;
    VPUSH(im->providers, fp);
    return fp;
}

static void add_pass(Image *im, Pass *p)
{
    if (p) VPUSH(im->passes, p);
}

static bool skipped(const IntVec *list, int id)
{
    for (int i = 0; i < list->n; i++)
        if (list->v[i] == id) return true;
    return false;
}

static bool magenta(const float *c) { return c[0] > 0.55f && c[1] < 0.25f && c[2] > 0.45f; }

static void image_setup(Image *im)
{
    WeScene *s = im->r.scene;
    WeProject *proj = s->project;
    Arena *a = &proj->arena;
    SceneObj *o = im->r.obj;
    if (im->model->passthrough) {
        bool any = false;
        for (int i = 0; i < im->effects.n; i++)
            if (im->effects.v[i]->visible->b) any = true;
        if (!any) return;
    }
    Material *mat = im->model->material;
    for (int i = 0; i < mat->passes.n; i++)
        add_pass(im, pass_new(&im->r, new_provider(im), mat->passes.v[i], NULL, NULL, NULL));

    if (!we_settings.debug_base_only) {
        for (int e = 0; e < im->effects.n; e++) {
            ImageEffect *ie = im->effects.v[e];
            if (skipped(&we_settings.skip_effects, ie->id) || !ie->visible->b || !ie->effect) continue;
            FboProvider *fp = new_provider(im);
            vec2 sz = image_size(im);
            for (int f = 0; f < ie->effect->fbos.n; f++) {
                EffectFbo *ef = &ie->effect->fbos.v[f];
                float sc = ef->scale > 0 ? ef->scale : 1;
                fbop_create(fp, ef->name, TEXFMT_ARGB8888, im->r.texture->flags, sc, sz.x / sc, sz.y / sc, sz.x / sc,
                            sz.y / sc);
            }
            int ovr = 0;
            for (int pi = 0; pi < ie->effect->passes.n; pi++) {
                EffectPass *ep = &ie->effect->passes.v[pi];
                if (!ep->material) {
                    if (ep->command != 0 || !ep->source || !ep->target) {
                        we_err("effect pass without material is only supported for copy commands");
                        continue;
                    }
                    MaterialPass *vp = arena_alloc(a, sizeof *vp);
                    vp->blending = BLEND_NORMAL;
                    vp->cullmode = CULL_NONE;
                    vp->shader = "commands/copy";
                    texmap_set(a, &vp->textures, 0, ep->source);
                    add_pass(im, pass_new(&im->r, fp, vp, NULL, NULL, ep->target));
                } else {
                    PassOverride *po = ovr < ie->overrides.n ? ie->overrides.v[ovr] : NULL;
                    for (int mp = 0; mp < ep->material->passes.n; mp++)
                        add_pass(im, pass_new(&im->r, fp, ep->material->passes.v[mp], po, &ep->binds, ep->target));
                    if (ovr < ie->overrides.n) ovr++;
                }
            }
        }

        /* linux-wallpaperengine compatibility tint for magenta composite layers */
        for (int e = 0; e < im->effects.n; e++) {
            ImageEffect *ie = im->effects.v[e];
            if (skipped(&we_settings.skip_effects, ie->id) || !ie->visible->b) continue;
            for (int k = 0; k < ie->overrides.n; k++) {
                PassOverride *po = ie->overrides.v[k];
                int ci = combo_find(&po->combos, "COMPOSITE");
                int cc = const_find(&po->constants, "compositecolor");
                if (ci < 0 || po->combos.v[ci].value != 2 || cc < 0) continue;
                if (!magenta(po->constants.v[cc].value->v)) continue;
                Material *tint = material_load(proj, "materials/effects/tint.json");
                if (!tint || !tint->passes.n) continue;
                PassOverride *to = arena_alloc(a, sizeof *to);
                to->id = -1;
                combo_set(a, &to->combos, "BLENDMODE", 30);
                Constant c1 = { "color", dv_new_vec(a, po->constants.v[cc].value->v, 3) };
                Constant c2 = { "alpha", dv_new_float(a, 1.0f) };
                AVPUSH(a, to->constants, c1);
                AVPUSH(a, to->constants, c2);
                add_pass(im, pass_new(&im->r, new_provider(im), tint->passes.v[0], to, NULL, NULL));
                goto tinted;
            }
        }
    tinted:
        if (im->blend_mode->i > 0) {
            Material *pt = material_load(proj, "materials/util/effectpassthrough.json");
            if (pt && pt->passes.n) {
                PassOverride *bo = arena_alloc(a, sizeof *bo);
                bo->id = -1;
                combo_set(a, &bo->combos, "BLENDMODE", im->blend_mode->i);
                add_pass(im, pass_new(&im->r, new_provider(im), pt->passes.v[0], bo, NULL, NULL));
            }
        }
    }
    if (im->passes.n > 1) {
        im->passes.v[im->passes.n - 1]->blending = im->passes.v[0]->blending;
        im->passes.v[0]->blending = BLEND_NORMAL;
    }
    im->r.anim_time = 0;
    for (int i = 0; i < im->r.texture->frames.n; i++) im->r.anim_time += im->r.texture->frames.v[i].frametime;

    /* wire inputs/outputs (CImage::setupPasses) */
    Tex *draw_to = im->cur_main, *as_input = im->r.texture, *effect_input = NULL;
    GLuint tc = im->tc_copy;
    bool first = true, in_target = false;
    for (int i = 0; i < im->passes.n; i++) {
        Pass *p = im->passes.v[i];
        Tex *prev_draw = draw_to;
        bool writes_target = false;
        bool is_first = first;
        GLuint space = is_first ? (im->has_puppet ? im->puppet_pos : im->copy_pos) : im->pass_pos;
        const mat4 *proj_m = is_first ? &im->mvp_copy : &im->mvp_pass;
        const mat4 *inv_m = is_first ? &im->mvp_copy_inv : &im->mvp_pass_inv;
        first = false;
        if (is_first && im->has_puppet) {
            p->blending = BLEND_TRANSLUCENT;
            p->geo_setup = puppet_setup;
            p->geo_draw = puppet_draw;
            p->geo_cleanup = puppet_cleanup;
            p->geo_user = im;
        }
        p->model = &im->model_m;
        p->viewproj = &im->viewproj;
        if (p->target) {
            Tex *res = fbop_find(p->fbos, p->target);
            if (!res) res = scene_find_fbo(s, p->target);
            if (!res) {
                we_err("pass target '%s' could not be resolved for object %d", p->target, o->id);
            } else {
                if (!in_target) {
                    effect_input = as_input;
                    in_target = true;
                }
                draw_to = res;
                writes_target = true;
            }
        }
        bool last = i == im->passes.n - 1;
        if (!writes_target && last && im->r.obj->visible->b &&
            !(we_settings.debug_no_solid_final && im->model->solidlayer)) {
            space = im->scene_pos;
            draw_to = s->scene_fbo;
            proj_m = &im->mvp_screen;
            inv_m = &im->mvp_screen_inv;
        }
        p->draw_to = draw_to;
        p->input = as_input;
        p->prev_input = in_target ? effect_input : NULL;
        p->pos_buf = space;
        p->tc_buf = tc;
        p->mvp = proj_m;
        p->mvp_inv = inv_m;
        tc = im->tc_pass;
        if (writes_target) {
            as_input = draw_to;
            draw_to = prev_draw;
        } else {
            draw_to = prev_draw;
            /* ping-pong */
            Tex *m = im->cur_main, *sub = im->cur_sub;
            draw_to = sub;
            as_input = m;
            im->cur_main = sub;
            im->cur_sub = m;
            in_target = false;
            effect_input = NULL;
        }
    }
    im->initialized = true;
}

static void update_geometry(Image *im, Xform *out)
{
    WeScene *s = im->r.scene;
    float sw = s->width, sh = s->height;
    Xform x = resolve_xform(s, im->r.obj);
    vec3 origin = x.origin;
    vec2 size = image_size(im);
    if ((size.x == 0 || size.y == 0) && im->model->has_width && im->model->has_height)
        size = v2(im->model->width, im->model->height);
    if (im->model->fullscreen) {
        size = v2(sw, sh);
        origin = v3(sw / 2, sh / 2, 0);
    }
    vec2 prev = im->size;
    im->size = size;
    if (im->has_puppet && (size.x != prev.x || size.y != prev.y)) puppet_positions(im, size);
    scene_position(im, origin, size, x.scale, sw, sh);
    float scene_pos[18] = { im->pos.x, im->pos.y, 0, im->pos.x, im->pos.w, 0, im->pos.z, im->pos.y, 0,
                            im->pos.z, im->pos.y, 0, im->pos.x, im->pos.w, 0, im->pos.z, im->pos.w, 0 };
    float tc[12], cp[18];
    copy_geometry(im, size, tc, cp);
    glBindBuffer(GL_ARRAY_BUFFER, im->scene_pos);
    glBufferData(GL_ARRAY_BUFFER, sizeof scene_pos, scene_pos, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, im->copy_pos);
    glBufferData(GL_ARRAY_BUFFER, sizeof cp, cp, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, im->tc_copy);
    glBufferData(GL_ARRAY_BUFFER, sizeof tc, tc, GL_DYNAMIC_DRAW);
    im->center = v3((im->pos.x + im->pos.z) / 2, (im->pos.y + im->pos.w) / 2, 0);
    im->mvp_copy = im->model->passthrough ? im->mvp_screen : m4ortho2d(0, size.x, 0, size.y);
    im->mvp_copy_inv = m4inverse(im->mvp_copy);
    im->model_m = m4ortho2d(0, size.x, 0, size.y);
    *out = x;
}

static void image_render(SceneObj *o)
{
    Image *im = o->impl;
    if (!im->initialized || !o->visible->b) return;
    WeScene *s = im->r.scene;
    glColorMask(true, true, true, true);
    Xform x;
    update_geometry(im, &x);
    mat4 rot = m4id();
    if (x.angle != 0) {
        rot = m4translate(rot, im->center);
        rot = m4rotate(rot, -x.angle, v3(0, 0, 1));
        rot = m4translate(rot, v3s(im->center, -1));
    }
    mat4 mvp = m4mul(m4mul(s->projection, s->lookat), rot);
    if (s->parallax->b && !we_settings.disable_parallax) {
        float amount = s->parallax_amount->f;
        float ref = s->width;
        float dx = (im->parallax_depth->v[0] + amount) * s->parallax_disp.x * ref;
        float dy = (im->parallax_depth->v[1] + amount) * s->parallax_disp.y * ref;
        mvp = m4translate(mvp, v3(dx, dy, 0));
    }
    im->mvp_screen = mvp;
    im->mvp_screen_inv = m4inverse(mvp);
    if (im->model->passthrough) {
        im->mvp_copy = im->mvp_screen;
        im->mvp_copy_inv = im->mvp_screen_inv;
    }
    tex_update(im->r.texture);
    for (int i = 0; i < im->passes.n; i++) {
        if (i == im->passes.n - 1) glColorMask(true, true, true, false);
        pass_render(im->passes.v[i]);
    }
    glColorMask(true, true, true, true);
}

static void image_destroy(SceneObj *o)
{
    Image *im = o->impl;
    for (int i = 0; i < im->passes.n; i++) pass_free(im->passes.v[i]);
    VFREE(im->passes);
    for (int i = 0; i < im->providers.n; i++) {
        fbop_free(im->providers.v[i]);
        free(im->providers.v[i]);
    }
    VFREE(im->providers);
    GLuint bufs[] = { im->scene_pos, im->copy_pos, im->pass_pos, im->tc_copy, im->tc_pass, im->puppet_pos,
                      im->puppet_tc, im->puppet_idx };
    for (size_t i = 0; i < sizeof bufs / sizeof *bufs; i++)
        if (bufs[i]) glDeleteBuffers(1, &bufs[i]);
    free(im->puppet_raw);
    if (im->r.texture && im->r.texture->kind == TEXKIND_FILE) tex_free(im->r.texture);
    if (im->dummy) tex_free(im->dummy);
    fbop_free(&im->r.fbos);
    free(im);
}

static ImageEffect *parse_image_effect(WeProject *p, const cJSON *j)
{
    Arena *a = &p->arena;
    ImageEffect *e = arena_alloc(a, sizeof *e);
    e->id = js_int(j, "id", -1);
    e->name = arena_strdup(a, js_str(j, "name", "Effect without name"));
    e->visible = us_bool(a, j, "visible", &p->props, true);
    const cJSON *it;
    cJSON_ArrayForEach(it, js_get(j, "passes"))
    {
        AVPUSH(a, e->overrides, override_parse(p, it));
    }
    const char *file = js_str(j, "file", NULL);
    e->effect = file ? effect_load(p, file) : NULL;
    if (!e->effect) we_err("cannot load effect %s", file ? file : "(none)");
    return e;
}

/* textures referenced by a material: "_rt_*" framebuffers or .tex files */
static Tex *detect_texture(Image *im)
{
    Material *m = im->model->material;
    if (!m->passes.n || !m->passes.v[0]->textures.n) return NULL;
    const char *name = texmap_get(&m->passes.v[0]->textures, 0);
    if (!name) {
        /* first entry in index order */
        int best = 1 << 30;
        for (int i = 0; i < m->passes.v[0]->textures.n; i++)
            if (m->passes.v[0]->textures.v[i].idx < best) {
                best = m->passes.v[0]->textures.v[i].idx;
                name = m->passes.v[0]->textures.v[i].name;
            }
    }
    if (!name) return NULL;
    if (!strncmp(name, "_rt_", 4) || !strncmp(name, "_alias_", 7)) return scene_find_fbo(im->r.scene, name);
    Tex *t = scene_resolve_texture(im->r.scene, name);
    if (t) t->refs++;
    return t;
}

bool image_create(WeScene *s, SceneObj *o)
{
    WeProject *p = s->project;
    Arena *a = &p->arena;
    const cJSON *j = o->json;
    Image *im = calloc(1, sizeof *im);
    im->r.scene = s;
    im->r.obj = o;
    im->r.fbos.parent = &s->fbos;
    o->impl = im;
    o->render = image_render;
    o->destroy = image_destroy;

    im->alpha = us_float(a, j, "alpha", &p->props, 1.0f);
    im->color = us_color(a, j, "color", &p->props, v4(1, 1, 1, 1));
    const char *align = js_str(j, "horizontalalign", js_str(j, "alignment", "center"));
    im->alignment = arena_strdup(a, align);
    DynVal *size = us_vec2(a, j, "size", &p->props, v2(0, 0));
    im->json_size = v2(size->v[0], size->v[1]);
    im->parallax_depth = us_vec2(a, j, "parallaxDepth", &p->props, v2(0, 0));
    im->blend_mode = us_int(a, j, "colorBlendMode", &p->props, 0);
    im->brightness = us_float(a, j, "brightness", &p->props, 1.0f);
    im->model = model_load(p, js_str(j, "image", ""));
    if (!im->model) return false;
    const cJSON *it;
    cJSON_ArrayForEach(it, js_get(j, "effects"))
    {
        AVPUSH(a, im->effects, parse_image_effect(p, it));
    }
    const cJSON *instance = js_get(j, "instance");
    if (instance && im->model->material->passes.n) {
        MaterialPass *fp = im->model->material->passes.v[0];
        TexMap tmp = { 0 };
        texmap_parse(a, &tmp, js_get(instance, "textures"));
        for (int i = 0; i < tmp.n; i++)
            if (!texmap_get(&fp->textures, tmp.v[i].idx)) texmap_set(a, &fp->textures, tmp.v[i].idx, tmp.v[i].name);
        TexMap tmpu = { 0 };
        texmap_parse(a, &tmpu, js_get(instance, "usertextures"));
        for (int i = 0; i < tmpu.n; i++)
            if (!texmap_get(&fp->usertextures, tmpu.v[i].idx))
                texmap_set(a, &fp->usertextures, tmpu.v[i].idx, tmpu.v[i].name);
    }
    im->r.material = im->model->material;
    im->r.brightness = &im->brightness->f;
    im->r.alpha = &im->alpha->f;
    im->r.color = im->color->v;

    sceneobj_register(o, "alpha", im->alpha);
    sceneobj_register(o, "color", im->color);
    sceneobj_register(o, "parallaxDepth", im->parallax_depth);

    float sw = s->width, sh = s->height;
    Xform x = resolve_xform(s, o);
    vec3 origin = x.origin;
    vec2 sz = im->json_size;
    im->r.texture = detect_texture(im);
    if (!im->r.texture) {
        if (im->model->solidlayer && sz.x == 0 && sz.y == 0) sz = v2(sw, sh);
        im->dummy = tex_dummy(sz.x, sz.y);
        im->r.texture = im->dummy;
        im->r.texture->kind = TEXKIND_DUMMY;
    }
    if ((sz.x == 0 || sz.y == 0) && im->r.texture) {
        sz = v2(tex_real_w(im->r.texture), tex_real_h(im->r.texture));
    } else if ((sz.x == 0 || sz.y == 0) && im->model->has_width && im->model->has_height) {
        sz = v2(im->model->width, im->model->height);
    }
    if (im->model->fullscreen) {
        sz = v2(sw, sh);
        origin = v3(sw / 2, sh / 2, 0);
    }
    im->size = sz;
    scene_position(im, origin, sz, x.scale, sw, sh);

    char na[64], nb[64];
    snprintf(na, sizeof na, "_rt_imageLayerComposite_%d_a", o->id);
    snprintf(nb, sizeof nb, "_rt_imageLayerComposite_%d_b", o->id);
    uint32_t flags = im->r.texture->flags;
    im->cur_main = im->main_fbo = fbop_create(&s->fbos, na, TEXFMT_ARGB8888, flags, 1, sz.x, sz.y, sz.x, sz.y);
    im->cur_sub = im->sub_fbo = fbop_create(&s->fbos, nb, TEXFMT_ARGB8888, flags, 1, sz.x, sz.y, sz.x, sz.y);

    float scene_pos[18] = { im->pos.x, im->pos.y, 0, im->pos.x, im->pos.w, 0, im->pos.z, im->pos.y, 0,
                            im->pos.z, im->pos.y, 0, im->pos.x, im->pos.w, 0, im->pos.z, im->pos.w, 0 };
    float tc[12], cp[18];
    copy_geometry(im, sz, tc, cp);
    if (im->r.texture->animated) {
        float t2[12] = { 0, 1, 0, 0, 1, 1, 1, 1, 0, 0, 1, 0 };
        memcpy(tc, t2, sizeof tc);
    }
    float tc_pass[12] = { 0, 1, 0, 0, 1, 1, 1, 1, 0, 0, 1, 0 };
    float pass_pos[18] = { -1, 1, 0, -1, -1, 0, 1, 1, 0, 1, 1, 0, -1, -1, 0, 1, -1, 0 };
    im->scene_pos = make_buffer(scene_pos, sizeof scene_pos);
    im->copy_pos = make_buffer(cp, sizeof cp);
    im->pass_pos = make_buffer(pass_pos, sizeof pass_pos);
    im->tc_copy = make_buffer(tc, sizeof tc);
    im->tc_pass = make_buffer(tc_pass, sizeof tc_pass);
    im->has_puppet = load_puppet(im, sz);
    im->center = v3((im->pos.x + im->pos.z) / 2, (im->pos.y + im->pos.w) / 2, 0);
    im->mvp_screen = m4mul(s->projection, s->lookat);
    im->mvp_pass = m4id();
    im->mvp_pass_inv = m4id();
    im->mvp_copy = im->model->passthrough ? im->mvp_screen : m4ortho2d(0, sz.x, 0, sz.y);
    im->mvp_copy_inv = m4inverse(im->mvp_copy);
    im->model_m = m4ortho2d(0, sz.x, 0, sz.y);
    im->viewproj = m4id();

    image_setup(im);
    return true;
}

/* hit test in scene space (centered coordinates, y up) for cursor events */
bool image_hit(SceneObj *o, float x, float y)
{
    if (!o || o->kind != OBJ_IMAGE || !o->impl) return false;
    Image *im = o->impl;
    float x0 = fminf(im->pos.x, im->pos.z), x1 = fmaxf(im->pos.x, im->pos.z);
    float y0 = fminf(im->pos.y, im->pos.w), y1 = fmaxf(im->pos.y, im->pos.w);
    return x >= x0 && x <= x1 && y >= y0 && y <= y1;
}
