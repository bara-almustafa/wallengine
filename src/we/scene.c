/* Scene wallpapers (port of linux-wallpaperengine's CScene / Camera). */
#include "internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

WeScene *scene_new(WeProject *p, int out_w, int out_h, uint32_t clamp_flags);
void     scene_frame(WeScene *s, float mx, float my, bool down, const float uv[4]);
void     scene_free(WeScene *s);
void     scene_pause(WeScene *s, bool paused);

SceneObj *scene_object(WeScene *s, int id)
{
    for (int i = 0; i < s->objects.n; i++)
        if (s->objects.v[i]->id == id) return s->objects.v[i];
    return NULL;
}

SceneObj *scene_object_by_name(WeScene *s, const char *name)
{
    for (int i = 0; i < s->order.n; i++)
        if (!strcmp(s->order.v[i]->name, name)) return s->order.v[i];
    for (int i = 0; i < s->objects.n; i++)
        if (!strcmp(s->objects.v[i]->name, name)) return s->objects.v[i];
    return NULL;
}

Tex *scene_find_fbo(WeScene *s, const char *name) { return fbop_find(&s->fbos, name); }

Tex *scene_resolve_texture(WeScene *s, const char *name)
{
    if (!strcmp(name, "$mediaThumbnail")) return media_album_texture(false);
    if (!strcmp(name, "$mediaPreviousThumbnail")) return media_album_texture(true);
    for (int i = 0; i < s->textures.n; i++)
        if (!strcmp(s->textures.v[i]->name, name)) return s->textures.v[i];
    Tex *t = tex_load(s->project, name);
    if (t) VPUSH(s->textures, t);
    return t;
}

void sceneobj_register(SceneObj *o, const char *name, DynVal *v)
{
    for (int i = 0; i < o->prop_names.n; i++)
        if (!strcmp(o->prop_names.v[i], name)) return;
    VPUSH(o->prop_names, strdup(name));
    VPUSH(o->prop_values, v);
}

static SceneObj *parse_object(WeScene *s, cJSON *j)
{
    WeProject *p = s->project;
    Arena *a = &p->arena;
    SceneObj *o = calloc(1, sizeof *o);
    o->json = j;
    const cJSON *id = js_get(j, "id");
    o->id = id && cJSON_IsNumber(id) ? (int)id->valuedouble : -1;
    const cJSON *name = js_get(j, "name");
    char buf[64];
    if (name && cJSON_IsString(name)) o->name = arena_strdup(a, name->valuestring);
    else if (name && cJSON_IsNumber(name)) {
        snprintf(buf, sizeof buf, "%d", (int)name->valuedouble);
        o->name = arena_strdup(a, buf);
    } else {
        o->name = "unknown";
    }
    const cJSON *d;
    cJSON_ArrayForEach(d, js_get(j, "dependencies"))
    {
        if (cJSON_IsNumber(d)) VPUSH(o->deps, (int)d->valuedouble);
    }
    const cJSON *parent = js_get(j, "parent");
    if (parent && cJSON_IsNumber(parent)) {
        o->has_parent = true;
        o->parent = (int)parent->valuedouble;
    }
    o->origin = us_vec3(a, j, "origin", &p->props, v3(0, 0, 0));
    o->scale = us_vec3(a, j, "scale", &p->props, v3(1, 1, 1));
    o->angles = us_vec3(a, j, "angles", &p->props, v3(0, 0, 0));
    o->visible = us_bool(a, j, "visible", &p->props, true);
    const cJSON *image = js_get(j, "image");
    const cJSON *sound = js_get(j, "sound");
    if (image && cJSON_IsString(image)) o->kind = OBJ_IMAGE;
    else if (sound && cJSON_IsArray(sound)) o->kind = OBJ_SOUND;
    else if (js_get(j, "particle")) o->kind = OBJ_PARTICLE;
    else if (js_get(j, "text")) o->kind = OBJ_TEXT;
    else if (js_get(j, "light")) {
        o->kind = OBJ_GROUP;
        we_dbg("light objects are not supported yet (%s)", o->name);
    } else if (js_get(j, "shape")) {
        o->kind = OBJ_GROUP;
        we_dbg("volume light objects are not supported yet (%s)", o->name);
    } else {
        o->kind = OBJ_GROUP;
    }
    sceneobj_register(o, "origin", o->origin);
    sceneobj_register(o, "scale", o->scale);
    sceneobj_register(o, "angles", o->angles);
    sceneobj_register(o, "visible", o->visible);
    return o;
}

static bool in_list(const IntVec *l, int id)
{
    for (int i = 0; i < l->n; i++)
        if (l->v[i] == id) return true;
    return false;
}

static void create_object(WeScene *s, SceneObj *o, int depth)
{
    if (o->created || depth > 64) return;
    o->created = true;   /* guards against dependency cycles */
    for (int i = 0; i < o->deps.n; i++) {
        if (o->deps.v[i] == o->id) continue;
        SceneObj *d = scene_object(s, o->deps.v[i]);
        if (d) create_object(s, d, depth + 1);
    }
    if (o->has_parent) {
        SceneObj *p = scene_object(s, o->parent);
        if (p) create_object(s, p, depth + 1);
        else we_err("cannot find parent %d of object %d", o->parent, o->id);
    }
    bool ok = true;
    switch (o->kind) {
    case OBJ_IMAGE: ok = image_create(s, o); break;
    case OBJ_SOUND: ok = we_settings.silent ? false : sound_create(s, o); break;
    case OBJ_TEXT: ok = text_create(s, o); break;
    case OBJ_PARTICLE:
        if (we_settings.disable_particles) ok = false;
        else ok = particle_create(s, o);
        break;
    default: break;
    }
    if (!ok) {
        if (o->kind == OBJ_IMAGE || o->kind == OBJ_PARTICLE || o->kind == OBJ_TEXT)
            we_err("failed to set up object %d (%s)", o->id, o->name);
        if (o->destroy) o->destroy(o);
        o->impl = NULL;
        o->render = NULL;
        o->destroy = NULL;
    }
    /* register the object's properties with the script engine */
    for (int i = 0; i < o->prop_names.n; i++) {
        char key[160];
        snprintf(key, sizeof key, "%s_%d", o->prop_names.v[i], o->id);
        if (o->prop_values.v[i]->script) script_queue(s->script, o, key, o->prop_values.v[i]);
    }
}

static void order_object(WeScene *s, SceneObj *o, int depth)
{
    if (o->ordered || depth > 64) return;
    for (int i = 0; i < o->deps.n; i++) {
        if (o->deps.v[i] == o->id) continue;
        SceneObj *d = scene_object(s, o->deps.v[i]);
        if (d) order_object(s, d, depth + 1);
        else we_err("cannot find dependency %d of object %d", o->deps.v[i], o->id);
    }
    if (o->ordered) return;
    o->ordered = true;
    if (o->impl) VPUSH(s->order, o);
}

static cJSON *bloom_json(WeScene *s)
{
    char buf[128];
    cJSON *b = cJSON_CreateObject();
    cJSON_AddStringToObject(b, "image", "models/wpenginelinux.json");
    cJSON_AddStringToObject(b, "name", "bloomimagewpenginelinux");
    cJSON_AddBoolToObject(b, "visible", 1);
    cJSON_AddStringToObject(b, "scale", "1.0 1.0 1.0");
    cJSON_AddStringToObject(b, "angles", "0.0 0.0 0.0");
    snprintf(buf, sizeof buf, "%f %f 0.0", s->width / 2, s->height / 2);
    cJSON_AddStringToObject(b, "origin", buf);
    snprintf(buf, sizeof buf, "%f %f", s->width, s->height);
    cJSON_AddStringToObject(b, "size", buf);
    cJSON_AddNumberToObject(b, "id", -1);
    cJSON *effects = cJSON_AddArrayToObject(b, "effects");
    cJSON *e = cJSON_CreateObject();
    cJSON_AddStringToObject(e, "file", "effects/wpenginelinux/bloomeffect.json");
    cJSON_AddNumberToObject(e, "id", 15242000);
    cJSON_AddStringToObject(e, "name", "");
    cJSON *passes = cJSON_AddArrayToObject(e, "passes");
    for (int i = 0; i < 3; i++) {
        cJSON *ps = cJSON_CreateObject();
        cJSON *c = cJSON_AddObjectToObject(ps, "constantshadervalues");
        cJSON_AddNumberToObject(c, "bloomstrength", s->bloom_strength->f);
        cJSON_AddNumberToObject(c, "bloomthreshold", s->bloom_threshold->f);
        cJSON_AddItemToArray(passes, ps);
    }
    cJSON_AddItemToArray(effects, e);
    return b;
}

/* the bloom layer is created on first use so the "bloom" property can be toggled live */
static void scene_enable_bloom(WeScene *s)
{
    if (s->bloom_obj) return;
    s->bloom_json = bloom_json(s);
    SceneObj *b = parse_object(s, s->bloom_json);
    s->bloom_obj = b;
    b->kind = OBJ_IMAGE;
    if (image_create(s, b)) {
        b->created = true;
    } else {
        we_err("bloom requires the Wallpaper Engine assets (materials/util/*.json)");
        if (b->destroy) b->destroy(b);
        b->impl = NULL;
        b->render = NULL;
        b->destroy = NULL;
    }
}

WeScene *scene_new(WeProject *p, int out_w, int out_h, uint32_t clamp_flags)
{
    char *data = (char *)vfs_read(p->vfs, p->file, NULL);
    if (!data) {
        we_err("cannot read scene file %s", p->file);
        return NULL;
    }
    cJSON *j = cJSON_Parse(data);
    free(data);
    if (!j) {
        we_err("cannot parse %s", p->file);
        return NULL;
    }
    WeScene *s = calloc(1, sizeof *s);
    s->project = p;
    s->arena = &p->arena;
    s->json = j;
    Arena *a = &p->arena;
    Properties *pr = &p->props;
    const cJSON *camera = js_get(j, "camera");
    const cJSON *general = js_get(j, "general");
    const cJSON *proj = js_get(general, "orthogonalprojection");

    s->eye = js_vec3(camera, "eye", v3(0, 0, 1));
    s->center = js_vec3(camera, "center", v3(0, 0, 0));
    s->up = js_vec3(camera, "up", v3(0, 1, 0));
    s->nearz = us_float(a, camera, "nearz", pr, 0.0f);
    s->farz = us_float(a, camera, "farz", pr, 1000.0f);
    s->fov = us_float(a, camera, "fov", pr, 50.0f);
    s->ambient = us_vec3(a, general, "ambientcolor", pr, v3(0, 0, 0));
    s->skylight = us_vec3(a, general, "skylightcolor", pr, v3(0, 0, 0));
    s->clear = us_vec3(a, general, "clearcolor", pr, v3(1, 1, 1));
    s->fade = us_bool(a, general, "camerafade", pr, false);
    s->bloom = us_bool(a, general, "bloom", pr, false);
    s->bloom_strength = us_float(a, general, "bloomstrength", pr, 0);
    s->bloom_threshold = us_float(a, general, "bloomthreshold", pr, 0);
    s->parallax = us_bool(a, general, "cameraparallax", pr, false);
    s->parallax_amount = us_float(a, general, "cameraparallaxamount", pr, 1);
    s->parallax_delay = us_float(a, general, "cameraparallaxdelay", pr, 0);
    s->parallax_mouse = us_float(a, general, "cameraparallaxmouseinfluence", pr, 1);
    s->shake = us_bool(a, general, "camerashake", pr, false);
    s->shake_amplitude = us_float(a, general, "camerashakeamplitude", pr, 0);
    s->shake_roughness = us_float(a, general, "camerashakeroughness", pr, 0);
    s->shake_speed = us_float(a, general, "camerashakespeed", pr, 0);
    s->lookat = m4lookat(s->eye, s->center, s->up);

    s->script = script_new(s);

    cJSON *oj;
    cJSON_ArrayForEach(oj, js_get(j, "objects")) VPUSH(s->objects, parse_object(s, oj));

    float w = js_num(proj, "width", 0), h = js_num(proj, "height", 0);
    if (js_bool(proj, "auto", false) || w <= 0 || h <= 0) {
        float mx = 0, my = 0;
        for (int i = 0; i < s->objects.n; i++) {
            SceneObj *o = s->objects.v[i];
            if (o->kind != OBJ_IMAGE) continue;
            vec2 size = js_vec2(o->json, "size", v2(0, 0));
            mx = fmaxf(mx, fabsf(o->origin->v[0]) + size.x / 2);
            my = fmaxf(my, fabsf(o->origin->v[1]) + size.y / 2);
        }
        if (mx > 0 && my > 0) {
            w = mx * 2;
            h = my * 2;
        } else {
            w = out_w;
            h = out_h;
        }
    }
    s->width = w;
    s->height = h;
    s->projection = m4translate(m4ortho(-w / 2, w / 2, -h / 2, h / 2, s->nearz->f, s->farz->f), s->eye);

    glGenVertexArrays(1, &s->vao);
    glBindVertexArray(s->vao);
    s->scene_fbo = fbop_create(&s->fbos, "_rt_FullFrameBuffer", TEXFMT_ARGB8888, clamp_flags, 1, w, h, w, h);
    fbop_alias(&s->fbos, "_rt_MipMappedFrameBuffer", "_rt_FullFrameBuffer");
    fbop_create(&s->fbos, "_rt_shadowAtlas", TEXFMT_ARGB8888, TEXFLAG_CLAMP, 1, w, h, w, h);
    fbop_alias(&s->fbos, "_alias_lightCookie", "_rt_shadowAtlas");
    /* the bloom framebuffers must exist before objects that sample them */
    fbop_create(&s->fbos, "_rt_4FrameBuffer", TEXFMT_ARGB8888, TEXFLAG_CLAMP, 1, w / 4, h / 4, w / 4, h / 4);
    fbop_create(&s->fbos, "_rt_8FrameBuffer", TEXFMT_ARGB8888, TEXFLAG_CLAMP, 1, w / 8, h / 8, w / 8, h / 8);
    fbop_create(&s->fbos, "_rt_Bloom", TEXFMT_ARGB8888, TEXFLAG_CLAMP, 1, w / 8, h / 8, w / 8, h / 8);

    for (int i = 0; i < s->objects.n; i++) create_object(s, s->objects.v[i], 0);
    for (int i = 0; i < s->objects.n; i++) order_object(s, s->objects.v[i], 0);

    if (s->bloom->b) scene_enable_bloom(s);
    we_dbg("scene %s: %gx%g, %d objects (%d rendered)", p->title, w, h, s->objects.n, s->order.n);
    return s;
}

static float hash1(float n) { return fmodf(sinf(n) * 43758.5453f, 1.0f); }

static float noise1(float x)
{
    float i = floorf(x), f = x - i;
    f = f * f * (3 - 2 * f);
    return lerpf(hash1(i), hash1(i + 1), f) * 2 - 1;
}

void scene_frame(WeScene *s, float mx, float my, bool down, const float uv[4])
{
    /* mouse in scene space (CScene::updateMouse) */
    s->mouse_last = s->mouse;
    s->mouse_down_last = s->mouse_down;
    s->mouse_down = down && !we_settings.disable_mouse;
    if (!we_settings.disable_mouse) {
        mx = clampf(mx, 0, 1);
        my = clampf(my, 0, 1);
        s->mouse_norm.x = uv[0] + mx * (uv[1] - uv[0]);
        s->mouse_norm.y = uv[2] + my * (uv[3] - uv[2]);
        s->mouse.x = s->mouse_norm.x;
        s->mouse.y = uv[2] + (1 - my) * (uv[3] - uv[2]);
    }
    float dt = g_Time - g_TimeLast;
    if (s->parallax->b && !we_settings.disable_parallax) {
        float infl = s->parallax_mouse->f, amount = s->parallax_amount->f;
        float delay = clampf(s->parallax_delay->f * dt, 0, 1);
        vec2 c = v2(s->mouse.x - 0.5f, s->mouse.y - 0.5f);
        s->parallax_disp.x = lerpf(s->parallax_disp.x, c.x * amount * infl, delay);
        s->parallax_disp.y = lerpf(s->parallax_disp.y, c.y * amount * infl, delay);
    }
    s->lookat = m4lookat(s->eye, s->center, s->up);
    if (s->shake->b && s->shake_amplitude->f > 0) {
        float t = g_Time * fmaxf(s->shake_speed->f, 0.01f);
        float r = 1 + s->shake_roughness->f * 4;
        float a = s->shake_amplitude->f * 10;
        vec3 off = v3(noise1(t * r) * a, noise1(t * r + 71.3f) * a, 0);
        s->lookat = m4translate(s->lookat, off);
    }
    script_tick(s->script);

    glBindVertexArray(s->vao);
    glBindFramebuffer(GL_FRAMEBUFFER, s->scene_fbo->fbo);
    glViewport(0, 0, s->scene_fbo->width, s->scene_fbo->height);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(s->clear->v[0], s->clear->v[1], s->clear->v[2], 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    for (int i = 0; i < s->order.n; i++) {
        SceneObj *o = s->order.v[i];
        if (we_settings.debug_object >= 0 && o->id != we_settings.debug_object) continue;
        if (in_list(&we_settings.skip_objects, o->id)) continue;
        if (o->render) o->render(o);
    }
    if (s->bloom->b) {
        scene_enable_bloom(s);
        if (s->bloom_obj && s->bloom_obj->render) s->bloom_obj->render(s->bloom_obj);
    }
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDepthMask(GL_TRUE);
    glColorMask(true, true, true, true);
}

void scene_pause(WeScene *s, bool paused)
{
    s->paused = paused;
    for (int i = 0; i < s->sound_ids.n; i++) sound_pause(s->sound_ids.v[i], paused);
}

void scene_free(WeScene *s)
{
    if (!s) return;
    for (int i = 0; i < s->sound_ids.n; i++) sound_stop(s->sound_ids.v[i]);
    VFREE(s->sound_ids);
    /* destroy in reverse creation order */
    for (int i = s->objects.n - 1; i >= 0; i--) {
        SceneObj *o = s->objects.v[i];
        if (o->destroy) o->destroy(o);
    }
    if (s->bloom_obj && s->bloom_obj->destroy) s->bloom_obj->destroy(s->bloom_obj);
    script_free(s->script);
    for (int i = 0; i < s->objects.n; i++) {
        SceneObj *o = s->objects.v[i];
        for (int k = 0; k < o->prop_names.n; k++) free(o->prop_names.v[k]);
        VFREE(o->prop_names);
        VFREE(o->prop_values);
        VFREE(o->deps);
        free(o);
    }
    if (s->bloom_obj) {
        for (int k = 0; k < s->bloom_obj->prop_names.n; k++) free(s->bloom_obj->prop_names.v[k]);
        VFREE(s->bloom_obj->prop_names);
        VFREE(s->bloom_obj->prop_values);
        free(s->bloom_obj);
    }
    VFREE(s->objects);
    VFREE(s->order);
    for (int i = 0; i < s->textures.n; i++) tex_free(s->textures.v[i]);
    VFREE(s->textures);
    fbop_free(&s->fbos);
    if (s->vao) glDeleteVertexArrays(1, &s->vao);
    cJSON_Delete(s->bloom_json);
    cJSON_Delete(s->json);
    free(s);
}
