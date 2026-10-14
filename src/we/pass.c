/* A single material/effect render pass (port of linux-wallpaperengine's CPass). */
#include "internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const TexMap EMPTY_BINDS;
static const PassOverride EMPTY_OVERRIDE = { .id = -1 };

static bool tex_ready(const Tex *t) { return t && t->nids > 0; }

static Uniform *find_uniform(Pass *p, const char *name)
{
    for (int i = 0; i < p->uniforms.n; i++)
        if (!strcmp(p->uniforms.v[i].name, name)) return &p->uniforms.v[i];
    return NULL;
}

static Uniform *new_uniform(Pass *p, const char *name, int type)
{
    GLint loc = glGetUniformLocation(p->program, name);
    if (loc < 0) return NULL;
    Uniform *u = find_uniform(p, name);
    if (!u) {
        Uniform nu = { 0 };
        nu.name = strdup(name);
        VPUSH(p->uniforms, nu);
        u = &p->uniforms.v[p->uniforms.n - 1];
    }
    char *n = u->name;
    memset(u, 0, sizeof *u);
    u->name = n;
    u->loc = loc;
    u->type = type;
    u->count = 1;
    return u;
}

void pass_uniform_ptr(Pass *p, const char *name, int type, const void *ptr, int count)
{
    Uniform *u = new_uniform(p, name, type);
    if (!u) return;
    u->ptr = ptr;
    u->count = count;
}

void pass_uniform_pptr(Pass *p, const char *name, int type, const void **pptr)
{
    Uniform *u = new_uniform(p, name, type);
    if (u) u->pptr = pptr;
}

void pass_uniform_f(Pass *p, const char *name, float v)
{
    Uniform *u = new_uniform(p, name, U_FLOAT);
    if (u) u->own[0] = v;
}

void pass_uniform_i(Pass *p, const char *name, int v)
{
    Uniform *u = new_uniform(p, name, U_INT);
    if (u) u->own_i = v;
}

void pass_uniform_v(Pass *p, const char *name, int type, const float *v)
{
    Uniform *u = new_uniform(p, name, type);
    if (!u) return;
    int n = type == U_VEC2 ? 2 : type == U_VEC3 ? 3 : type == U_VEC4 ? 4 : type == U_MAT3 ? 9 : type == U_MAT4 ? 16 : 1;
    memcpy(u->own, v, sizeof(float) * n);
}

static void apply_uniform(const Uniform *u)
{
    const void *ptr = u->pptr ? *u->pptr : u->ptr;
    if (!ptr) ptr = u->type == U_INT ? (const void *)&u->own_i : (const void *)u->own;
    switch (u->type) {
    case U_FLOAT: glUniform1fv(u->loc, u->count, ptr); break;
    case U_INT: glUniform1iv(u->loc, u->count, ptr); break;
    case U_VEC2: glUniform2fv(u->loc, 1, ptr); break;
    case U_VEC3: glUniform3fv(u->loc, 1, ptr); break;
    case U_VEC4: glUniform4fv(u->loc, 1, ptr); break;
    case U_MAT3: glUniformMatrix3fv(u->loc, 1, GL_FALSE, ptr); break;
    case U_MAT4: glUniformMatrix4fv(u->loc, 1, GL_FALSE, ptr); break;
    }
}

static void uniform_from_param(Pass *p, const ShaderParam *sp, const DynVal *v)
{
    switch (sp->type) {
    case SV_FLOAT: pass_uniform_ptr(p, sp->name, U_FLOAT, &v->f, 1); break;
    case SV_INT: pass_uniform_ptr(p, sp->name, U_INT, &v->i, 1); break;
    case SV_VEC2: pass_uniform_ptr(p, sp->name, U_VEC2, v->v, 1); break;
    case SV_VEC3: pass_uniform_ptr(p, sp->name, U_VEC3, v->v, 1); break;
    case SV_VEC4: pass_uniform_ptr(p, sp->name, U_VEC4, v->v, 1); break;
    }
}

static Tex *resolve_named(Pass *p, const char *name)
{
    if (!strncmp(name, "_rt_", 4) || !strncmp(name, "_alias_", 7)) {
        Tex *t = fbop_find(p->fbos, name);
        if (!t) we_err("cannot resolve framebuffer %s", name);
        return t;
    }
    Tex *t = scene_resolve_texture(p->renderable->scene, name);
    if (!t) we_err("cannot resolve texture %s", name);
    return t;
}

static void chain_push(Pass *p, int idx, Tex *t)
{
    if (idx < 0 || idx > 15) return;
    TexChain *c = calloc(1, sizeof *c);
    c->tex = t;
    c->next = p->textures[idx];
    p->textures[idx] = c;
}

static Tex *resolve_texture(Pass *p, Tex *expected, int index, Tex *previous)
{
    const char *bind = texmap_get(p->binds, index);
    if (!bind) return expected;
    if (!strcmp(bind, "previous")) return p->prev_input ? p->prev_input : previous ? previous : expected;
    return fbop_find(p->fbos, bind);
}

static void setup_texture_uniforms(Pass *p)
{
    const TexMap *maps[] = { &p->shader->vertex.textures, &p->shader->fragment.textures, &p->mp->textures,
                             &p->mp->usertextures, &p->ovr->textures, &p->ovr->usertextures };
    for (int m = 0; m < 6; m++) {
        for (int i = 0; i < maps[m]->n; i++) {
            Tex *t = resolve_named(p, maps[m]->v[i].name);
            if (!t) continue;
            if (m == 0) {
                /* the vertex defaults start a fresh chain */
                int idx = maps[m]->v[i].idx;
                if (idx >= 0 && idx < 16) {
                    while (p->textures[idx]) {
                        TexChain *n = p->textures[idx]->next;
                        free(p->textures[idx]);
                        p->textures[idx] = n;
                    }
                }
            }
            chain_push(p, maps[m]->v[i].idx, t);
        }
    }
    for (int i = 0; i < p->binds->n; i++) {
        const char *b = p->binds->v[i].name;
        Tex *t = !strcmp(b, "previous") ? NULL : fbop_find(p->fbos, b);
        if (strcmp(b, "previous") && !t) we_err("cannot resolve bound framebuffer %s", b);
        chain_push(p, p->binds->v[i].idx, t);
    }

    Tex *tex = resolve_texture(p, p->renderable->texture, 0, NULL);
    char name[32];
    for (int i = 0; i < 8; i++) {
        snprintf(name, sizeof name, "g_Texture%d", i);
        pass_uniform_i(p, name, i);
    }
    pass_uniform_f(p, "g_TextureReductionScale", 1.0f);
    if (tex) memcpy(p->tex0_res, tex->res, sizeof p->tex0_res);
    for (int idx = 0; idx < 16; idx++) {
        if (!p->textures[idx]) continue;
        Tex *t = resolve_texture(p, p->textures[idx]->tex, idx, tex);
        if (t) tex = t;
        if (!tex) continue;
        snprintf(name, sizeof name, "g_Texture%dResolution", idx);
        pass_uniform_ptr(p, name, U_VEC4, tex->res, 1);
    }
    pass_uniform_ptr(p, "g_Texture0Resolution", U_VEC4, p->tex0_res, 1);
}

static const float IDENTITY4[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
static const float IDENTITY3[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };

static void setup_uniforms(Pass *p)
{
    setup_texture_uniforms(p);
    Renderable *r = p->renderable;
    WeScene *s = r->scene;
    pass_uniform_ptr(p, "g_LightAmbientColor", U_VEC3, s->ambient->v, 1);
    pass_uniform_ptr(p, "g_LightSkylightColor", U_VEC3, s->skylight->v, 1);
    static const float one = 1.0f;
    static const float white[4] = { 1, 1, 1, 1 };
    const float *color = r->color ? r->color : white;
    pass_uniform_ptr(p, "g_Brightness", U_FLOAT, r->brightness ? r->brightness : &one, 1);
    pass_uniform_ptr(p, "g_UserAlpha", U_FLOAT, r->alpha ? r->alpha : &one, 1);
    pass_uniform_ptr(p, "g_Alpha", U_FLOAT, r->alpha ? r->alpha : &one, 1);
    pass_uniform_ptr(p, "g_Color", U_VEC3, color, 1);
    pass_uniform_ptr(p, "g_Color4", U_VEC4, color, 1);
    if (!find_uniform(p, "g_CompositeColor")) pass_uniform_ptr(p, "g_CompositeColor", U_VEC3, color, 1);
    pass_uniform_ptr(p, "g_Time", U_FLOAT, &g_Time, 1);
    pass_uniform_ptr(p, "g_Daytime", U_FLOAT, &g_Daytime, 1);
    pass_uniform_pptr(p, "g_ModelViewProjectionMatrixInverse", U_MAT4, (const void **)&p->mvp_inv);
    pass_uniform_pptr(p, "g_ModelViewProjectionMatrix", U_MAT4, (const void **)&p->mvp);
    pass_uniform_pptr(p, "g_EffectModelViewProjectionMatrix", U_MAT4, (const void **)&p->mvp);
    pass_uniform_pptr(p, "g_ModelMatrix", U_MAT4, (const void **)&p->model);
    pass_uniform_pptr(p, "g_EffectModelMatrix", U_MAT4, (const void **)&p->model);
    pass_uniform_v(p, "g_NormalModelMatrix", U_MAT3, IDENTITY3);
    pass_uniform_pptr(p, "g_ViewProjectionMatrix", U_MAT4, (const void **)&p->viewproj);
    pass_uniform_ptr(p, "g_PointerPosition", U_VEC2, &s->mouse, 1);
    pass_uniform_ptr(p, "g_PointerPositionLast", U_VEC2, &s->mouse_last, 1);
    pass_uniform_v(p, "g_EffectTextureProjectionMatrix", U_MAT4, IDENTITY4);
    pass_uniform_v(p, "g_EffectTextureProjectionMatrixInverse", U_MAT4, IDENTITY4);
    float ts[2] = { 1.0f / s->width, 1.0f / s->height }, tsh[2] = { 0.5f / s->width, 0.5f / s->height };
    pass_uniform_v(p, "g_TexelSize", U_VEC2, ts);
    pass_uniform_v(p, "g_TexelSizeHalf", U_VEC2, tsh);
    pass_uniform_ptr(p, "g_AudioSpectrum16Left", U_FLOAT, g_Audio.audio16, 16);
    pass_uniform_ptr(p, "g_AudioSpectrum16Right", U_FLOAT, g_Audio.audio16, 16);
    pass_uniform_ptr(p, "g_AudioSpectrum32Left", U_FLOAT, g_Audio.audio32, 32);
    pass_uniform_ptr(p, "g_AudioSpectrum32Right", U_FLOAT, g_Audio.audio32, 32);
    pass_uniform_ptr(p, "g_AudioSpectrum64Left", U_FLOAT, g_Audio.audio64, 64);
    pass_uniform_ptr(p, "g_AudioSpectrum64Right", U_FLOAT, g_Audio.audio64, 64);
}

static void setup_shader_variables(Pass *p)
{
    for (int k = 0; k < 2; k++) {
        const ShaderUnit *u = k ? &p->shader->fragment : &p->shader->vertex;
        for (int i = 0; i < u->params.n; i++)
            if (!find_uniform(p, u->params.v[i].name)) uniform_from_param(p, &u->params.v[i], &u->params.v[i].def);
    }
    const ConstMap *cms[] = { &p->mp->constants, &p->ovr->constants };
    for (int k = 0; k < 2; k++)
        for (int i = 0; i < cms[k]->n; i++) {
            const ShaderParam *sp = shader_find_param(p->shader, cms[k]->v[i].name);
            if (sp) uniform_from_param(p, sp, cms[k]->v[i].value);
        }
}

Pass *pass_new(Renderable *r, FboProvider *fbos, const MaterialPass *mp, const PassOverride *ovr,
               const TexMap *binds, const char *target)
{
    Pass *p = calloc(1, sizeof *p);
    p->renderable = r;
    p->fbos = fbos;
    p->mp = mp;
    p->ovr = ovr ? ovr : &EMPTY_OVERRIDE;
    p->binds = binds ? binds : &EMPTY_BINDS;
    p->target = target ? strdup(target) : NULL;
    p->blending = mp->blending;
    WeProject *proj = r->scene->project;
    Arena *a = &proj->arena;
    for (int i = 0; i < mp->combos.n; i++) combo_set(a, &p->combos, mp->combos.v[i].name, mp->combos.v[i].value);
    if (r->texture) {
        if (r->texture->format == TEXFMT_RG88) combo_set(a, &p->combos, "TEX0FORMAT", 8);
        else if (r->texture->format == TEXFMT_R8) combo_set(a, &p->combos, "TEX0FORMAT", 9);
    }
    const char *shader_name = p->ovr->shader_override ? p->ovr->shader_override : mp->shader;
    TexMap pass_textures = { 0 };
    for (int i = 0; i < mp->textures.n; i++) texmap_set(a, &pass_textures, mp->textures.v[i].idx, mp->textures.v[i].name);
    for (int i = 0; i < mp->usertextures.n; i++)
        texmap_set(a, &pass_textures, mp->usertextures.v[i].idx, mp->usertextures.v[i].name);
    ConstMap consts = { 0 };
    for (int i = 0; i < mp->constants.n; i++) AVPUSH(a, consts, mp->constants.v[i]);
    for (int i = 0; i < p->ovr->constants.n; i++) AVPUSH(a, consts, p->ovr->constants.v[i]);

    p->shader = shader_build(proj, shader_name, &p->combos, &p->ovr->combos, &pass_textures, &p->ovr->textures,
                             &consts);
    if (!p->shader) return p;   /* ok == false: renders nothing */
    p->program = p->shader->program;
    glGenVertexArrays(1, &p->vao);
    setup_shader_variables(p);
    setup_uniforms(p);
    p->a_tc = glGetAttribLocation(p->program, "a_TexCoord");
    p->a_pos = glGetAttribLocation(p->program, "a_Position");
    p->u_rot = glGetUniformLocation(p->program, "g_Texture0Rotation");
    p->u_trans = glGetUniformLocation(p->program, "g_Texture0Translation");
    p->ok = true;
    return p;
}

void pass_free(Pass *p)
{
    if (!p) return;
    if (p->vao) glDeleteVertexArrays(1, &p->vao);
    shader_free(p->shader);
    for (int i = 0; i < p->uniforms.n; i++) free(p->uniforms.v[i].name);
    VFREE(p->uniforms);
    for (int i = 0; i < 16; i++)
        while (p->textures[i]) {
            TexChain *n = p->textures[i]->next;
            free(p->textures[i]);
            p->textures[i] = n;
        }
    free(p->target);
    free(p);
}

static Tex *resolve_texture0(Pass *p)
{
    Tex *t0 = resolve_texture(p, p->input, 0, p->input);
    TexChain *c = p->textures[0];
    if (!c) return t0;
    for (; c; c = c->next) {
        if (!c->tex) {
            if (tex_ready(p->prev_input)) return p->prev_input;
            if (tex_ready(p->input)) return p->input;
        } else if (tex_ready(c->tex)) {
            return c->tex;
        }
    }
    if (tex_ready(p->prev_input)) return p->prev_input;
    return p->input;
}

static void bind_unit(int index, const Tex *t, uint32_t frame)
{
    if (!t) return;
    glActiveTexture(GL_TEXTURE0 + index);
    glBindTexture(GL_TEXTURE_2D, tex_id(t, frame));
}

void pass_render(Pass *p)
{
    if (!p || !p->ok) return;
    if (we_settings.debug_pass_log)
        we_log("render pass object=%d shader=%s target=%s drawTo=%s", p->renderable->obj->id, p->mp->shader,
               p->target ? p->target : "<screen/local>", p->draw_to ? p->draw_to->name : "<null>");
    if (!p->draw_to || !p->input) return;
    glBindVertexArray(p->vao);

    /* framebuffer + state */
    glBindFramebuffer(GL_FRAMEBUFFER, p->draw_to->fbo);
    glViewport(0, 0, p->draw_to->width, p->draw_to->height);
    switch (p->blending) {
    case BLEND_TRANSLUCENT:
        glEnable(GL_BLEND);
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        break;
    case BLEND_ADDITIVE:
        glEnable(GL_BLEND);
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_SRC_ALPHA, GL_ONE);
        break;
    case BLEND_NORMAL:
        glEnable(GL_BLEND);
        glBlendFuncSeparate(GL_ONE, GL_ZERO, GL_ONE, GL_ZERO);
        break;
    default: glDisable(GL_BLEND);
    }
    if (p->mp->depthtest) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
    } else {
        glDisable(GL_DEPTH_TEST);
    }
    if (p->mp->cullmode == CULL_NORMAL) glEnable(GL_CULL_FACE);
    else glDisable(GL_CULL_FACE);
    glDepthMask(p->mp->depthwrite);

    /* textures */
    glUseProgram(p->program);
    Tex *t0 = resolve_texture0(p);
    uint32_t cur = 0;
    float trans[2] = { 0, 0 }, rot[4] = { 0, 0, 0, 0 };
    if (t0 && t0->animated && t0->frames.n && p->renderable->anim_time > 0) {
        double t = fmod((double)g_Time, p->renderable->anim_time);
        for (int i = 0; i < t0->frames.n; i++) {
            const TexFrame *f = &t0->frames.v[i];
            t -= f->frametime;
            if (t > 0) continue;
            cur = f->frame;
            float tw = tex_width(t0, cur), th = tex_height(t0, cur);
            trans[0] = f->x / tw;
            trans[1] = f->y / th;
            rot[0] = f->width1 / tw;
            rot[1] = f->width2 / tw;
            rot[2] = f->height2 / th;
            rot[3] = f->height1 / th;
            break;
        }
    }
    tex_update(t0);
    bind_unit(0, t0, cur);
    for (int idx = 0; idx < 16; idx++) {
        TexChain *c = p->textures[idx];
        if (!c) continue;
        Tex *e = c->tex;
        while (c) {
            if (!e) {
                if (tex_ready(p->prev_input)) {
                    e = p->prev_input;
                    break;
                }
                if (tex_ready(p->input)) {
                    e = p->input;
                    break;
                }
            } else if (tex_ready(e)) {
                break;
            }
            c = c->next;
            e = c ? c->tex : NULL;
        }
        if (!e && tex_ready(p->prev_input)) e = p->prev_input;
        if (!e) e = p->input;
        tex_update(e);
        bind_unit(idx, e, idx == 0 ? cur : 0);
        if (idx == 0) t0 = e;
    }
    if (t0) memcpy(p->tex0_res, t0->res, sizeof p->tex0_res);
    if (p->u_rot >= 0) glUniform4fv(p->u_rot, 1, rot);
    if (p->u_trans >= 0) glUniform2fv(p->u_trans, 1, trans);

    for (int i = 0; i < p->uniforms.n; i++) apply_uniform(&p->uniforms.v[i]);

    /* geometry */
    if (p->geo_setup) {
        p->geo_setup(p, p->geo_user);
    } else {
        if (p->a_tc >= 0) {
            glEnableVertexAttribArray(p->a_tc);
            glBindBuffer(GL_ARRAY_BUFFER, p->tc_buf);
            glVertexAttribPointer(p->a_tc, 2, GL_FLOAT, GL_FALSE, 0, NULL);
        }
        if (p->a_pos >= 0) {
            glEnableVertexAttribArray(p->a_pos);
            glBindBuffer(GL_ARRAY_BUFFER, p->pos_buf);
            glVertexAttribPointer(p->a_pos, 3, GL_FLOAT, GL_FALSE, 0, NULL);
        }
    }
    if (p->geo_draw) p->geo_draw(p, p->geo_user);
    else glDrawArrays(GL_TRIANGLES, 0, 6);
    if (p->geo_cleanup) {
        p->geo_cleanup(p, p->geo_user);
    } else {
        if (p->a_tc >= 0) glDisableVertexAttribArray(p->a_tc);
        if (p->a_pos >= 0) glDisableVertexAttribArray(p->a_pos);
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    for (int idx = 0; idx < 16; idx++)
        if (p->textures[idx]) {
            glActiveTexture(GL_TEXTURE0 + idx);
            glBindTexture(GL_TEXTURE_2D, 0);
        }
    glActiveTexture(GL_TEXTURE0);
}
