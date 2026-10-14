/* SceneScript: Wallpaper Engine's JavaScript property scripts, on QuickJS.
 * Each scripted value is an ES module exporting update(value) and optional
 * init, destroy, cursor and media event handlers and applyUserProperties. */
#include "internal.h"

#include "../third_party/quickjs/quickjs.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

bool image_hit(SceneObj *o, float x, float y);

typedef struct {
    char      *key;
    SceneObj  *obj;
    DynVal    *value;
    JSValue    ns;          /* module namespace */
    bool       ok, inited, hover, pressed;
} Script;

typedef struct {
    WeScene   *scene;
    JSRuntime *rt;
    JSContext *ctx;
    VEC(Script) scripts;
    unsigned   media_serial;
    bool       down_last;
    vec2       mouse_last;
    bool       props_dirty;
} Engine;

/* ------------------------------------------------------------------ JS prelude */
static const char *PRELUDE =
    "'use strict';\n"
    "class __VecBase {\n"
    "  _c(){ return [] }\n"
    "  _make(a){ return __vec(a) }\n"
    "  _op(o, f){ const a = this._c(); const b = (typeof o === 'number') ? a.map(()=>o) : a.map((_,i)=>o[['x','y','z','w'][i]] ?? 0); return this._make(a.map((v,i)=>f(v,b[i]))) }\n"
    "  add(o){ return this._op(o,(a,b)=>a+b) } subtract(o){ return this._op(o,(a,b)=>a-b) }\n"
    "  multiply(o){ return this._op(o,(a,b)=>a*b) } divide(o){ return this._op(o,(a,b)=>a/b) }\n"
    "  min(o){ return this._op(o,Math.min) } max(o){ return this._op(o,Math.max) }\n"
    "  lengthSqr(){ return this._c().reduce((s,v)=>s+v*v,0) } length(){ return Math.sqrt(this.lengthSqr()) }\n"
    "  normalize(){ const l = this.length(); return l > 0 ? this.divide(l) : this.copy() }\n"
    "  dot(o){ return this._c().reduce((s,v,i)=>s+v*(o[['x','y','z','w'][i]] ?? 0),0) }\n"
    "  mix(o,t){ return this._op(o,(a,b)=>a+(b-a)*t) }\n"
    "  abs(){ return this._make(this._c().map(Math.abs)) } sign(){ return this._make(this._c().map(Math.sign)) }\n"
    "  floor(){ return this._make(this._c().map(Math.floor)) } ceil(){ return this._make(this._c().map(Math.ceil)) }\n"
    "  round(){ return this._make(this._c().map(Math.round)) } copy(){ return this._make(this._c()) }\n"
    "  equals(o){ return this._c().every((v,i)=>v === (o[['x','y','z','w'][i]] ?? 0)) }\n"
    "  toString(){ return this._c().join(' ') }\n"
    "}\n"
    "class Vec2 extends __VecBase { constructor(x=0,y){ super(); if (typeof x === 'string') { const p = x.split(/[ ,]+/).map(Number); x = p[0]; y = p[1]; } if (typeof x === 'object' && x) { y = x.y; x = x.x; } this.x = x; this.y = (y === undefined ? x : y); } _c(){ return [this.x,this.y] } perpendicular(){ return new Vec2(-this.y, this.x) } angle(){ return Math.atan2(this.y, this.x) } }\n"
    "class Vec3 extends __VecBase { constructor(x=0,y,z){ super(); if (typeof x === 'string') { const p = x.split(/[ ,]+/).map(Number); x = p[0]; y = p[1]; z = p[2]; } if (typeof x === 'object' && x) { z = x.z ?? 0; y = x.y; x = x.x; } this.x = x; this.y = (y === undefined ? x : y); this.z = (z === undefined ? (y === undefined ? x : 0) : z); } _c(){ return [this.x,this.y,this.z] }\n"
    "  cross(o){ return new Vec3(this.y*o.z-this.z*o.y, this.z*o.x-this.x*o.z, this.x*o.y-this.y*o.x) } }\n"
    "class Vec4 extends __VecBase { constructor(x=0,y,z,w){ super(); if (typeof x === 'string') { const p = x.split(/[ ,]+/).map(Number); x = p[0]; y = p[1]; z = p[2]; w = p[3]; } if (typeof x === 'object' && x) { w = x.w ?? 0; z = x.z ?? 0; y = x.y; x = x.x; } this.x = x; this.y = (y === undefined ? x : y); this.z = (z === undefined ? (y === undefined ? x : 0) : z); this.w = (w === undefined ? (y === undefined ? x : 0) : w); } _c(){ return [this.x,this.y,this.z,this.w] } }\n"
    "function __vec(a){ return a.length === 2 ? new Vec2(a[0],a[1]) : a.length === 3 ? new Vec3(a[0],a[1],a[2]) : new Vec4(a[0],a[1],a[2],a[3]) }\n"
    "globalThis.Vec2 = Vec2; globalThis.Vec3 = Vec3; globalThis.Vec4 = Vec4; globalThis.__vec = __vec;\n"
    "globalThis.localStorage = { __d: Object.create(null), get(k){ k = String(k); return k in this.__d ? this.__d[k] : null }, set(k,v){ this.__d[String(k)] = v }, remove(k){ delete this.__d[String(k)] }, clear(){ this.__d = Object.create(null) } };\n"
    "globalThis.MediaPlaybackEvent = { PLAYBACK_STOPPED: 0, PLAYBACK_PLAYING: 1, PLAYBACK_PAUSED: 2 };\n"
    "globalThis.shared = {};\n"
    "globalThis.console = { log: (...a) => __log(0, a.map(String).join(' ')), warn: (...a) => __log(1, a.map(String).join(' ')), error: (...a) => __log(1, a.map(String).join(' ')), info: (...a) => __log(0, a.map(String).join(' ')) };\n"
    "const __timers = new Map(); let __timerId = 0;\n"
    "function __addTimer(cb, ms, repeat){ const id = ++__timerId; __timers.set(id, { cb, ms: Math.max(0, ms|0), next: engine.runtime * 1000 + (ms|0), repeat }); const stop = () => __timers.delete(id); return stop; }\n"
    "globalThis.__tickTimers = function(){ const now = engine.runtime * 1000; for (const [id, t] of [...__timers]) { if (t.next > now) continue; try { t.cb() } catch (e) { console.error(e) } if (t.repeat) t.next = now + t.ms; else __timers.delete(id) } };\n"
    "globalThis.engine = {\n"
    "  get frametime(){ return __num(0) }, get runtime(){ return __num(1) }, get timeOfDay(){ return __num(2) },\n"
    "  get userProperties(){ return __userProps() },\n"
    "  AUDIO_RESOLUTION_16: 16, AUDIO_RESOLUTION_32: 32, AUDIO_RESOLUTION_64: 64,\n"
    "  registerAudioBuffers(res){ const r = res|0 || 16; return { get left(){ return __audio(r) }, get right(){ return __audio(r) }, get average(){ return __audio(r) }, resolution: r } },\n"
    "  setInterval(cb, ms){ return __addTimer(cb, ms, true) }, setTimeout(cb, ms){ return __addTimer(cb, ms, false) },\n"
    "  clearInterval(f){ if (typeof f === 'function') f() }, clearTimeout(f){ if (typeof f === 'function') f() },\n"
    "  isDesktopDevice(){ return true }, isMobileDevice(){ return false }, isTabletDevice(){ return false },\n"
    "  isWallpaper(){ return true }, isScreensaver(){ return false }, isRunningInEditor(){ return false },\n"
    "  openUserShortcut(){}, screenResolution: null, canvasSize: null\n"
    "};\n"
    "globalThis.input = { get cursorWorldPosition(){ const c = __cursor(); return new Vec3(c[3], c[4], 0) }, get cursorScreenPosition(){ const c = __cursor(); return new Vec2(c[0], c[1]) }, get cursorLeftDown(){ return __cursor()[2] === 1 } };\n"
    "function __makeLayer(id){ if (id === undefined || id < -1000000) return undefined; const info = __layerInfo(id); if (!info) return undefined;\n"
    "  const layer = { get id(){ return id }, get name(){ return info.name }, get debug(){ return false },\n"
    "    getTextureAnimation(){ return { play(){}, stop(){}, pause(){}, isPlaying(){ return true }, setFrame(){}, getFrame(){ return 0 }, frameCount: 1, rate: 1 } },\n"
    "    getAnimation(){ return undefined }, getEffect(){ return { visible: true } }, getEffectCount(){ return 0 },\n"
    "    play(){}, stop(){}, pause(){}, isPlaying(){ return true }, getParent(){ return __makeLayer(info.parent) } };\n"
    "  for (const p of info.props) Object.defineProperty(layer, p, { enumerable: true, get(){ return __getProp(id, p) }, set(v){ __setProp(id, p, v) } });\n"
    "  return layer; }\n"
    "globalThis.__makeLayer = __makeLayer;\n"
    "globalThis.thisScene = {\n"
    "  getLayer(n){ const id = __findLayer(n); return id === undefined ? undefined : __makeLayer(id) },\n"
    "  getLayerCount(){ return __layerIds().length }, enumerateLayers(){ return __layerIds().map(__makeLayer) },\n"
    "  getLayerIndex(l){ return __layerIds().indexOf(l.id) }, sortLayer(){}, createLayer(){ return undefined }, destroyLayer(){},\n"
    "  getInitialLayerConfig(){ return undefined }, getCameraTransforms(){ return { eye: new Vec3(0,0,1), center: new Vec3(0,0,0), up: new Vec3(0,1,0) } }, setCameraTransforms(){},\n"
    "  get bloom(){ return __sceneProp('bloom') }, get bloomstrength(){ return __sceneProp('bloomstrength') }, get bloomthreshold(){ return __sceneProp('bloomthreshold') },\n"
    "  get clearenabled(){ return true }, get clearcolor(){ return __sceneProp('clearcolor') }, get ambientcolor(){ return __sceneProp('ambientcolor') }, get skylightcolor(){ return __sceneProp('skylightcolor') },\n"
    "  get fov(){ return __sceneProp('fov') }, get nearz(){ return __sceneProp('nearz') }, get farz(){ return __sceneProp('farz') },\n"
    "  get camerafade(){ return __sceneProp('camerafade') }, get camerashake(){ return __sceneProp('camerashake') }, get camerashakespeed(){ return __sceneProp('camerashakespeed') },\n"
    "  get camerashakeamplitude(){ return __sceneProp('camerashakeamplitude') }, get camerashakeroughness(){ return __sceneProp('camerashakeroughness') },\n"
    "  get cameraparallax(){ return __sceneProp('cameraparallax') }, get cameraparallaxamount(){ return __sceneProp('cameraparallaxamount') }, get cameraparallaxdelay(){ return __sceneProp('cameraparallaxdelay') }, get cameraparallaxmouseinfluence(){ return __sceneProp('cameraparallaxmouseinfluence') },\n"
    "  get time(){ return engine.runtime }, get currentTime(){ return engine.runtime }, get dt(){ return engine.frametime }, get fps(){ const f = engine.frametime; return f > 0 ? 1 / f : 60 }\n"
    "};\n"
    "globalThis.createScriptProperties = function(){ const props = Object.assign({}, globalThis.__seedProps || {}); const add = o => { if (o && o.name !== undefined && !(o.name in props)) props[o.name] = o.value; return b; };\n"
    "  const b = { addSlider: add, addCheckbox: add, addCombo: add, addColor: add, addText: add, addTextInput: add, addFile: add, addDirectory: add, finish(){ return props } }; return b; };\n";

static const char *MOD_WEMATH =
    "export const PI = Math.PI;\n"
    "export function deg2rad(d){ return d * Math.PI / 180 }\n"
    "export function rad2deg(r){ return r * 180 / Math.PI }\n"
    "export function mix(a, b, t){ return (typeof a === 'object') ? a.mix(b, t) : a + (b - a) * t }\n"
    "export function clamp(v, lo, hi){ return Math.min(Math.max(v, lo), hi) }\n"
    "export function smoothStep(e0, e1, x){ const t = Math.min(Math.max((x - e0) / (e1 - e0), 0), 1); return t * t * (3 - 2 * t) }\n"
    "export const smoothstep = smoothStep;\n"
    "export function fract(x){ return x - Math.floor(x) }\n"
    "export function sign(x){ return Math.sign(x) }\n";

static const char *MOD_WECOLOR =
    "export function rgb2hsv(c){ const r = c.x, g = c.y, b = c.z, mx = Math.max(r,g,b), mn = Math.min(r,g,b), d = mx - mn; let h = 0;\n"
    "  if (d > 0) { if (mx === r) h = ((g - b) / d) % 6; else if (mx === g) h = (b - r) / d + 2; else h = (r - g) / d + 4; h /= 6; if (h < 0) h += 1; }\n"
    "  return new Vec3(h, mx === 0 ? 0 : d / mx, mx) }\n"
    "export function hsv2rgb(c){ const h = c.x * 6, s = c.y, v = c.z, i = Math.floor(h) % 6, f = h - Math.floor(h), p = v * (1 - s), q = v * (1 - f * s), t = v * (1 - (1 - f) * s);\n"
    "  return [new Vec3(v,t,p), new Vec3(q,v,p), new Vec3(p,v,t), new Vec3(p,q,v), new Vec3(t,p,v), new Vec3(v,p,q)][(i + 6) % 6] }\n"
    "export function normalizeColor(c){ return new Vec3(c.x / 255, c.y / 255, c.z / 255) }\n"
    "export function expandColor(c){ return new Vec3(c.x * 255, c.y * 255, c.z * 255) }\n";

/* ------------------------------------------------------------------ helpers */
static Engine *eng(JSContext *ctx) { return JS_GetContextOpaque(ctx); }

static void log_exception(JSContext *ctx, const char *where)
{
    JSValue e = JS_GetException(ctx);
    const char *s = JS_ToCString(ctx, e);
    JSValue stack = JS_GetPropertyStr(ctx, e, "stack");
    const char *st = JS_IsUndefined(stack) ? NULL : JS_ToCString(ctx, stack);
    we_err("script %s: %s%s%s", where, s ? s : "exception", st ? "\n" : "", st ? st : "");
    if (s) JS_FreeCString(ctx, s);
    if (st) JS_FreeCString(ctx, st);
    JS_FreeValue(ctx, stack);
    JS_FreeValue(ctx, e);
}

static JSValue call_global(JSContext *ctx, const char *fn, int argc, JSValue *argv)
{
    JSValue g = JS_GetGlobalObject(ctx);
    JSValue f = JS_GetPropertyStr(ctx, g, fn);
    JSValue r = JS_UNDEFINED;
    if (JS_IsFunction(ctx, f)) r = JS_Call(ctx, f, g, argc, argv);
    JS_FreeValue(ctx, f);
    JS_FreeValue(ctx, g);
    return r;
}

static JSValue dv_to_js(JSContext *ctx, const DynVal *d)
{
    switch (d->type) {
    case DV_FLOAT: return JS_NewFloat64(ctx, d->f);
    case DV_INT: return JS_NewInt32(ctx, d->i);
    case DV_BOOL: return JS_NewBool(ctx, d->b);
    case DV_STRING: return JS_NewString(ctx, d->s ? d->s : "");
    case DV_VEC2:
    case DV_VEC3:
    case DV_VEC4: {
        int n = d->type == DV_VEC2 ? 2 : d->type == DV_VEC3 ? 3 : 4;
        JSValue arr = JS_NewArray(ctx);
        for (int i = 0; i < n; i++) JS_SetPropertyUint32(ctx, arr, i, JS_NewFloat64(ctx, d->v[i]));
        JSValue r = call_global(ctx, "__vec", 1, &arr);
        JS_FreeValue(ctx, arr);
        return r;
    }
    default: return JS_NULL;
    }
}

static void js_to_dv(JSContext *ctx, JSValue v, DynVal *d)
{
    if (JS_IsException(v) || JS_IsUndefined(v)) return;
    int tag = JS_VALUE_GET_TAG(v);
    if (JS_IsNull(v)) {
        dv_set_null(d);
    } else if (tag == JS_TAG_INT) {
        if (d->type == DV_FLOAT) dv_set_float(d, JS_VALUE_GET_INT(v));
        else dv_set_int(d, JS_VALUE_GET_INT(v));
    } else if (JS_IsBool(v)) {
        dv_set_bool(d, JS_ToBool(ctx, v));
    } else if (JS_IsNumber(v)) {
        double x;
        JS_ToFloat64(ctx, &x, v);
        dv_set_float(d, (float)x);
    } else if (JS_IsString(v)) {
        const char *s = JS_ToCString(ctx, v);
        dv_set_string(d, s ? s : "");
        if (s) JS_FreeCString(ctx, s);
    } else if (JS_IsObject(v)) {
        const char *names[] = { "x", "y", "z", "w" };
        float c[4] = { 0 };
        int n = 0;
        for (int i = 0; i < 4; i++) {
            JSValue p = JS_GetPropertyStr(ctx, v, names[i]);
            if (JS_IsNumber(p)) {
                double x;
                JS_ToFloat64(ctx, &x, p);
                c[i] = (float)x;
                n = i + 1;
            }
            JS_FreeValue(ctx, p);
            if (n != i + 1) break;
        }
        if (n >= 2) {
            float keep_w = d->v[3];
            bool was4 = d->type == DV_VEC4;
            dv_set_vec(d, c, n);
            if (n == 3 && was4) {
                d->v[3] = keep_w;
                d->type = DV_VEC4;
            }
        }
    }
}

/* ------------------------------------------------------------------ native bindings */
static JSValue js_log(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    int lvl = 0;
    if (argc > 0) JS_ToInt32(ctx, &lvl, argv[0]);
    const char *s = argc > 1 ? JS_ToCString(ctx, argv[1]) : NULL;
    if (lvl) we_err("[script] %s", s ? s : "");
    else we_log("[script] %s", s ? s : "");
    if (s) JS_FreeCString(ctx, s);
    return JS_UNDEFINED;
}

static JSValue js_engine_num(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    int w = 0;
    if (argc > 0) JS_ToInt32(ctx, &w, argv[0]);
    double v = w == 0 ? g_Time - g_TimeLast : w == 1 ? g_Time : g_Daytime;
    return JS_NewFloat64(ctx, v);
}

static JSValue js_audio(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    int r = 16;
    if (argc > 0) JS_ToInt32(ctx, &r, argv[0]);
    const float *src = r >= 64 ? g_Audio.audio64 : r >= 32 ? g_Audio.audio32 : g_Audio.audio16;
    int n = r >= 64 ? 64 : r >= 32 ? 32 : 16;
    JSValue arr = JS_NewArray(ctx);
    for (int i = 0; i < n; i++) JS_SetPropertyUint32(ctx, arr, i, JS_NewFloat64(ctx, src[i]));
    return arr;
}

static JSValue js_cursor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;
    WeScene *s = eng(ctx)->scene;
    JSValue arr = JS_NewArray(ctx);
    float sx = s->mouse.x * s->width, sy = s->mouse.y * s->height;
    JS_SetPropertyUint32(ctx, arr, 0, JS_NewFloat64(ctx, s->mouse_norm.x * s->width));
    JS_SetPropertyUint32(ctx, arr, 1, JS_NewFloat64(ctx, s->mouse_norm.y * s->height));
    JS_SetPropertyUint32(ctx, arr, 2, JS_NewInt32(ctx, s->mouse_down));
    JS_SetPropertyUint32(ctx, arr, 3, JS_NewFloat64(ctx, sx));
    JS_SetPropertyUint32(ctx, arr, 4, JS_NewFloat64(ctx, sy));
    return arr;
}

static SceneObj *obj_arg(JSContext *ctx, JSValueConst v)
{
    int id;
    if (JS_ToInt32(ctx, &id, v)) return NULL;
    return scene_object(eng(ctx)->scene, id);
}

static DynVal *obj_prop(SceneObj *o, const char *name)
{
    for (int i = 0; i < o->prop_names.n; i++)
        if (!strcasecmp(o->prop_names.v[i], name)) return o->prop_values.v[i];
    return NULL;
}

static JSValue js_get_prop(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 2) return JS_UNDEFINED;
    SceneObj *o = obj_arg(ctx, argv[0]);
    const char *n = JS_ToCString(ctx, argv[1]);
    DynVal *d = o && n ? obj_prop(o, n) : NULL;
    if (n) JS_FreeCString(ctx, n);
    return d ? dv_to_js(ctx, d) : JS_UNDEFINED;
}

static JSValue js_set_prop(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 3) return JS_UNDEFINED;
    SceneObj *o = obj_arg(ctx, argv[0]);
    const char *n = JS_ToCString(ctx, argv[1]);
    DynVal *d = o && n ? obj_prop(o, n) : NULL;
    if (n) JS_FreeCString(ctx, n);
    if (d) js_to_dv(ctx, argv[2], d);
    return JS_UNDEFINED;
}

static JSValue js_layer_info(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) return JS_NULL;
    SceneObj *o = obj_arg(ctx, argv[0]);
    if (!o) return JS_NULL;
    JSValue info = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, info, "name", JS_NewString(ctx, o->name));
    JS_SetPropertyStr(ctx, info, "parent", o->has_parent ? JS_NewInt32(ctx, o->parent) : JS_UNDEFINED);
    JSValue props = JS_NewArray(ctx);
    for (int i = 0; i < o->prop_names.n; i++)
        JS_SetPropertyUint32(ctx, props, i, JS_NewString(ctx, o->prop_names.v[i]));
    JS_SetPropertyStr(ctx, info, "props", props);
    return info;
}

static JSValue js_find_layer(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) return JS_UNDEFINED;
    WeScene *s = eng(ctx)->scene;
    if (JS_IsNumber(argv[0])) {
        SceneObj *o = obj_arg(ctx, argv[0]);
        return o ? JS_NewInt32(ctx, o->id) : JS_UNDEFINED;
    }
    const char *n = JS_ToCString(ctx, argv[0]);
    SceneObj *o = n ? scene_object_by_name(s, n) : NULL;
    if (n) JS_FreeCString(ctx, n);
    return o ? JS_NewInt32(ctx, o->id) : JS_UNDEFINED;
}

static JSValue js_layer_ids(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;
    WeScene *s = eng(ctx)->scene;
    JSValue arr = JS_NewArray(ctx);
    for (int i = 0; i < s->objects.n; i++) JS_SetPropertyUint32(ctx, arr, i, JS_NewInt32(ctx, s->objects.v[i]->id));
    return arr;
}

static JSValue js_scene_prop(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) return JS_UNDEFINED;
    WeScene *s = eng(ctx)->scene;
    const char *n = JS_ToCString(ctx, argv[0]);
    if (!n) return JS_UNDEFINED;
    struct { const char *name; DynVal *v; } map[] = {
        { "bloom", s->bloom }, { "bloomstrength", s->bloom_strength }, { "bloomthreshold", s->bloom_threshold },
        { "clearcolor", s->clear }, { "ambientcolor", s->ambient }, { "skylightcolor", s->skylight },
        { "fov", s->fov }, { "nearz", s->nearz }, { "farz", s->farz }, { "camerafade", s->fade },
        { "camerashake", s->shake }, { "camerashakespeed", s->shake_speed },
        { "camerashakeamplitude", s->shake_amplitude }, { "camerashakeroughness", s->shake_roughness },
        { "cameraparallax", s->parallax }, { "cameraparallaxamount", s->parallax_amount },
        { "cameraparallaxdelay", s->parallax_delay }, { "cameraparallaxmouseinfluence", s->parallax_mouse },
    };
    JSValue r = JS_UNDEFINED;
    for (size_t i = 0; i < sizeof map / sizeof *map; i++)
        if (!strcmp(map[i].name, n)) r = dv_to_js(ctx, map[i].v);
    JS_FreeCString(ctx, n);
    return r;
}

static JSValue js_user_props(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;
    Properties *pr = &eng(ctx)->scene->project->props;
    JSValue o = JS_NewObject(ctx);
    for (int i = 0; i < pr->list.n; i++) JS_SetPropertyStr(ctx, o, pr->list.v[i]->name, dv_to_js(ctx, &pr->list.v[i]->value));
    return o;
}

static JSModuleDef *module_loader(JSContext *ctx, const char *name, void *opaque)
{
    (void)opaque;
    const char *src = !strcmp(name, "WEMath") ? MOD_WEMATH : !strcmp(name, "WEColor") ? MOD_WECOLOR : NULL;
    if (!src) {
        JS_ThrowReferenceError(ctx, "module '%s' is not available", name);
        return NULL;
    }
    JSValue f = JS_Eval(ctx, src, strlen(src), name, JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(f)) return NULL;
    JSModuleDef *m = JS_VALUE_GET_PTR(f);
    JS_FreeValue(ctx, f);
    return m;
}

static void run_jobs(Engine *e)
{
    JSContext *c;
    while (JS_ExecutePendingJob(e->rt, &c) > 0) {
    }
}

void *script_new(WeScene *s)
{
    Engine *e = calloc(1, sizeof *e);
    e->scene = s;
    e->rt = JS_NewRuntime();
    e->ctx = JS_NewContext(e->rt);
    JS_SetContextOpaque(e->ctx, e);
    JS_SetModuleLoaderFunc(e->rt, NULL, module_loader, e);
    JSValue g = JS_GetGlobalObject(e->ctx);
    struct { const char *n; JSCFunction *f; int a; } fns[] = {
        { "__log", js_log, 2 },           { "__num", js_engine_num, 1 },          { "__audio", js_audio, 1 },
        { "__cursor", js_cursor, 0 },     { "__getProp", js_get_prop, 2 }, { "__setProp", js_set_prop, 3 },
        { "__layerInfo", js_layer_info, 1 }, { "__findLayer", js_find_layer, 1 },
        { "__layerIds", js_layer_ids, 0 }, { "__sceneProp", js_scene_prop, 1 },
        { "__userProps", js_user_props, 0 },
    };
    for (size_t i = 0; i < sizeof fns / sizeof *fns; i++)
        JS_SetPropertyStr(e->ctx, g, fns[i].n, JS_NewCFunction(e->ctx, fns[i].f, fns[i].n, fns[i].a));
    JS_FreeValue(e->ctx, g);
    JSValue r = JS_Eval(e->ctx, PRELUDE, strlen(PRELUDE), "<scenescript-prelude>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) log_exception(e->ctx, "prelude");
    JS_FreeValue(e->ctx, r);
    return e;
}

static void set_this_layer(Engine *e, Script *s)
{
    JSValue id = JS_NewInt32(e->ctx, s->obj ? s->obj->id : -2000000);
    JSValue layer = call_global(e->ctx, "__makeLayer", 1, &id);
    JSValue g = JS_GetGlobalObject(e->ctx);
    JS_SetPropertyStr(e->ctx, g, "thisLayer", JS_DupValue(e->ctx, layer));
    JS_SetPropertyStr(e->ctx, g, "thisObject", layer);
    JS_FreeValue(e->ctx, g);
}

static JSValue call_export(Engine *e, Script *s, const char *name, int argc, JSValue *argv, bool *found)
{
    JSValue f = JS_GetPropertyStr(e->ctx, s->ns, name);
    JSValue r = JS_UNDEFINED;
    if (found) *found = JS_IsFunction(e->ctx, f);
    if (JS_IsFunction(e->ctx, f)) {
        set_this_layer(e, s);
        r = JS_Call(e->ctx, f, JS_UNDEFINED, argc, argv);
        if (JS_IsException(r)) {
            log_exception(e->ctx, s->key);
            r = JS_UNDEFINED;
        }
    }
    JS_FreeValue(e->ctx, f);
    return r;
}

/* keep the exported scriptProperties object in sync with user-bound values */
static void sync_script_props(Engine *e, Script *s)
{
    if (!s->value->script_props.n) return;
    JSValue sp = JS_GetPropertyStr(e->ctx, s->ns, "scriptProperties");
    if (JS_IsObject(sp))
        for (int i = 0; i < s->value->script_props.n; i++) {
            ScriptProp *p = &s->value->script_props.v[i];
            if (p->value->prop) JS_SetPropertyStr(e->ctx, sp, p->name, dv_to_js(e->ctx, p->value));
        }
    JS_FreeValue(e->ctx, sp);
}

void script_queue(void *ep, SceneObj *o, const char *key, DynVal *v)
{
    Engine *e = ep;
    if (!e || !v->script) return;
    for (int i = 0; i < e->scripts.n; i++)
        if (!strcmp(e->scripts.v[i].key, key)) return;
    Script s = { .key = strdup(key), .obj = o, .value = v, .ns = JS_UNDEFINED };
    JSContext *ctx = e->ctx;
    /* seed createScriptProperties() with the scene's values */
    JSValue seed = JS_NewObject(ctx);
    for (int i = 0; i < v->script_props.n; i++)
        JS_SetPropertyStr(ctx, seed, v->script_props.v[i].name, dv_to_js(ctx, v->script_props.v[i].value));
    JSValue g = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, g, "__seedProps", seed);
    JS_FreeValue(ctx, g);
    set_this_layer(e, &s);
    JSValue f = JS_Eval(ctx, v->script, strlen(v->script), key, JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(f)) {
        log_exception(ctx, key);
    } else {
        JSModuleDef *m = JS_VALUE_GET_PTR(f);
        JSValue r = JS_EvalFunction(ctx, f);
        run_jobs(e);
        if (JS_IsException(r)) {
            log_exception(ctx, key);
        } else {
            s.ns = JS_GetModuleNamespace(ctx, m);
            s.ok = !JS_IsException(s.ns);
        }
        JS_FreeValue(ctx, r);
    }
    VPUSH(e->scripts, s);
    if (!s.ok) return;
    Script *sp = &e->scripts.v[e->scripts.n - 1];
    JSValue arg = dv_to_js(ctx, v);
    JSValue r = call_export(e, sp, "init", 1, &arg, NULL);
    js_to_dv(ctx, r, v);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, arg);
    sp->inited = true;
    arg = dv_to_js(ctx, v);
    r = call_export(e, sp, "update", 1, &arg, NULL);
    js_to_dv(ctx, r, v);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, arg);
    we_dbg("script %s loaded", key);
}

static JSValue cursor_event(Engine *e)
{
    WeScene *s = e->scene;
    JSValue ev = JS_NewObject(e->ctx);
    float c[3] = { s->mouse.x * s->width, s->mouse.y * s->height, 0 };
    JSValue arr = JS_NewArray(e->ctx);
    for (int i = 0; i < 3; i++) JS_SetPropertyUint32(e->ctx, arr, i, JS_NewFloat64(e->ctx, c[i]));
    JS_SetPropertyStr(e->ctx, ev, "worldPosition", call_global(e->ctx, "__vec", 1, &arr));
    JS_FreeValue(e->ctx, arr);
    return ev;
}

static void media_events(Engine *e)
{
    const MediaInfo *m = media_info();
    if (m->serial == e->media_serial) return;
    e->media_serial = m->serial;
    JSContext *ctx = e->ctx;
    JSValue props = JS_NewObject(ctx), playback = JS_NewObject(ctx), timeline = JS_NewObject(ctx),
            thumb = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, props, "title", JS_NewString(ctx, m->title));
    JS_SetPropertyStr(ctx, props, "artist", JS_NewString(ctx, m->artist));
    JS_SetPropertyStr(ctx, props, "albumTitle", JS_NewString(ctx, m->album));
    JS_SetPropertyStr(ctx, playback, "state", JS_NewInt32(ctx, m->state));
    JS_SetPropertyStr(ctx, timeline, "position", JS_NewFloat64(ctx, m->position));
    JS_SetPropertyStr(ctx, timeline, "duration", JS_NewFloat64(ctx, m->duration));
    JS_SetPropertyStr(ctx, thumb, "hasThumbnail", JS_NewBool(ctx, m->art_url[0] != 0));
    const float cols[4][3] = { { 0.12f, 0.12f, 0.12f }, { 0, 0, 0 }, { 0.25f, 0.25f, 0.25f }, { 1, 1, 1 } };
    const char *cn[4] = { "primaryColor", "secondaryColor", "tertiaryColor", "highContrastColor" };
    for (int i = 0; i < 4; i++) {
        JSValue arr = JS_NewArray(ctx);
        for (int k = 0; k < 3; k++) JS_SetPropertyUint32(ctx, arr, k, JS_NewFloat64(ctx, cols[i][k]));
        JS_SetPropertyStr(ctx, thumb, cn[i], call_global(ctx, "__vec", 1, &arr));
        JS_FreeValue(ctx, arr);
    }
    for (int i = 0; i < e->scripts.n; i++) {
        Script *s = &e->scripts.v[i];
        if (!s->ok) continue;
        JSValue r;
        r = call_export(e, s, "mediaPropertiesChanged", 1, &props, NULL);
        JS_FreeValue(ctx, r);
        r = call_export(e, s, "mediaPlaybackChanged", 1, &playback, NULL);
        JS_FreeValue(ctx, r);
        r = call_export(e, s, "mediaTimelineChanged", 1, &timeline, NULL);
        JS_FreeValue(ctx, r);
        r = call_export(e, s, "mediaThumbnailChanged", 1, &thumb, NULL);
        JS_FreeValue(ctx, r);
    }
    JS_FreeValue(ctx, props);
    JS_FreeValue(ctx, playback);
    JS_FreeValue(ctx, timeline);
    JS_FreeValue(ctx, thumb);
}

static void cursor_events(Engine *e)
{
    WeScene *s = e->scene;
    bool down = s->mouse_down, moved = s->mouse.x != e->mouse_last.x || s->mouse.y != e->mouse_last.y;
    bool pressed = down && !e->down_last, released = !down && e->down_last;
    e->down_last = down;
    e->mouse_last = s->mouse;
    if (!pressed && !released && !moved) return;
    float x = s->mouse.x * s->width - s->width / 2, y = s->mouse.y * s->height - s->height / 2;
    for (int i = 0; i < e->scripts.n; i++) {
        Script *sc = &e->scripts.v[i];
        if (!sc->ok || !sc->obj) continue;
        bool hit = image_hit(sc->obj, x, y);
        JSValue ev = cursor_event(e), r;
        if (hit && !sc->hover) {
            r = call_export(e, sc, "cursorEnter", 1, &ev, NULL);
            JS_FreeValue(e->ctx, r);
        } else if (!hit && sc->hover) {
            r = call_export(e, sc, "cursorLeave", 1, &ev, NULL);
            JS_FreeValue(e->ctx, r);
        }
        sc->hover = hit;
        if (moved && hit) {
            r = call_export(e, sc, "cursorMove", 1, &ev, NULL);
            JS_FreeValue(e->ctx, r);
        }
        if (pressed && hit) {
            sc->pressed = true;
            r = call_export(e, sc, "cursorDown", 1, &ev, NULL);
            JS_FreeValue(e->ctx, r);
        }
        if (released) {
            if (hit) {
                r = call_export(e, sc, "cursorUp", 1, &ev, NULL);
                JS_FreeValue(e->ctx, r);
                if (sc->pressed) {
                    r = call_export(e, sc, "cursorClick", 1, &ev, NULL);
                    JS_FreeValue(e->ctx, r);
                }
            }
            sc->pressed = false;
        }
        JS_FreeValue(e->ctx, ev);
    }
}

void script_tick(void *ep)
{
    Engine *e = ep;
    if (!e) return;
    JSValue r = call_global(e->ctx, "__tickTimers", 0, NULL);
    if (JS_IsException(r)) log_exception(e->ctx, "timers");
    JS_FreeValue(e->ctx, r);
    media_events(e);
    cursor_events(e);
    if (e->props_dirty) {
        e->props_dirty = false;
        JSValue up = js_user_props(e->ctx, JS_UNDEFINED, 0, NULL);
        for (int i = 0; i < e->scripts.n; i++)
            if (e->scripts.v[i].ok) {
                JSValue rr = call_export(e, &e->scripts.v[i], "applyUserProperties", 1, &up, NULL);
                JS_FreeValue(e->ctx, rr);
            }
        JS_FreeValue(e->ctx, up);
    }
    for (int i = 0; i < e->scripts.n; i++) {
        Script *s = &e->scripts.v[i];
        if (!s->ok) continue;
        sync_script_props(e, s);
        JSValue arg = dv_to_js(e->ctx, s->value);
        JSValue res = call_export(e, s, "update", 1, &arg, NULL);
        js_to_dv(e->ctx, res, s->value);
        JS_FreeValue(e->ctx, res);
        JS_FreeValue(e->ctx, arg);
    }
    run_jobs(e);
}

void script_user_properties_changed(void *ep)
{
    Engine *e = ep;
    if (e) e->props_dirty = true;
}

/* text layers use the regular property-script mechanism */
int script_layer_new(void *e, DynVal *text)
{
    (void)e;
    (void)text;
    return 0;
}
void script_layer_tick(void *e, int handle) { (void)e; (void)handle; }
const char *script_layer_text(void *e, int handle)
{
    (void)e;
    (void)handle;
    return NULL;
}
void script_layer_free(void *e, int handle) { (void)e; (void)handle; }

void script_free(void *ep)
{
    Engine *e = ep;
    if (!e) return;
    for (int i = 0; i < e->scripts.n; i++) {
        Script *s = &e->scripts.v[i];
        if (s->ok) {
            JSValue r = call_export(e, s, "destroy", 0, NULL, NULL);
            JS_FreeValue(e->ctx, r);
        }
        JS_FreeValue(e->ctx, s->ns);
        free(s->key);
    }
    VFREE(e->scripts);
    JSValue g = JS_GetGlobalObject(e->ctx);
    JS_SetPropertyStr(e->ctx, g, "thisLayer", JS_UNDEFINED);
    JS_SetPropertyStr(e->ctx, g, "thisObject", JS_UNDEFINED);
    JS_FreeValue(e->ctx, g);
    JS_FreeContext(e->ctx);
    JS_FreeRuntime(e->rt);
    free(e);
}
