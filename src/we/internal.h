/* Wallpaper Engine (Steam workshop) compatibility layer - internal types.
 * Ported from linux-wallpaperengine (GPLv3) by Almamu and contributors. */
#ifndef WE_INTERNAL_H
#define WE_INTERNAL_H

#include "../we.h"
#include "../third_party/cJSON.h"
#include "wemath.h"
#include "wallpaper_engine.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* ------------------------------------------------------------------ arena */
typedef struct ArenaBlock ArenaBlock;
typedef struct DynVal DynVal;
typedef struct { ArenaBlock *head; DynVal **dvs; int ndv, capdv; } Arena;
void  arena_track(Arena *a, DynVal *d);            /* dv_free() on arena_free() */
void *arena_alloc(Arena *a, size_t n);           /* zeroed */
char *arena_strdup(Arena *a, const char *s);
void  arena_free(Arena *a);

/* ------------------------------------------------------------------ dynamic arrays */
#define VEC(T) struct { T *v; int n, cap; }
#define VPUSH(arr, x)                                                                              \
    do {                                                                                           \
        if ((arr).n == (arr).cap) {                                                                \
            (arr).cap = (arr).cap ? (arr).cap * 2 : 8;                                             \
            (arr).v = realloc((arr).v, sizeof(*(arr).v) * (arr).cap);                              \
        }                                                                                          \
        (arr).v[(arr).n++] = (x);                                                                  \
    } while (0)
/* arena-backed growth: old buffers stay in the arena and are released with it */
void *vec_grow(Arena *a, void *v, int n, int *cap, size_t elem);
#define AVPUSH(a, arr, x)                                                                          \
    do {                                                                                           \
        if ((arr).n == (arr).cap) (arr).v = vec_grow(a, (arr).v, (arr).n, &(arr).cap, sizeof *(arr).v); \
        (arr).v[(arr).n++] = (x);                                                                  \
    } while (0)
typedef VEC(int) IntVec;
#define VFREE(arr) do { free((arr).v); (arr).v = NULL; (arr).n = (arr).cap = 0; } while (0)

/* ------------------------------------------------------------------ logging */
void we_log(const char *fmt, ...);
void we_err(const char *fmt, ...);
void we_dbg(const char *fmt, ...);

/* ------------------------------------------------------------------ json helpers */
cJSON      *js_get(const cJSON *o, const char *key);            /* NULL for missing or null */
const char *js_str(const cJSON *o, const char *key, const char *def);
double      js_num(const cJSON *o, const char *key, double def);
int         js_int(const cJSON *o, const char *key, int def);
bool        js_bool(const cJSON *o, const char *key, bool def);
int         vec_parse(const char *s, float out[4]);              /* returns component count */
int         vec_count(const char *s);
vec3        js_vec3(const cJSON *o, const char *key, vec3 def);  /* string/number/array */
vec2        js_vec2(const cJSON *o, const char *key, vec2 def);
void        color_parse(const char *s, float alpha, float out[4]);

/* ------------------------------------------------------------------ virtual file system */
typedef struct Vfs Vfs;
Vfs           *vfs_new(void);
void           vfs_free(Vfs *v);
bool           vfs_mount(Vfs *v, const char *path);              /* directory or .pkg */
void           vfs_add(Vfs *v, const char *name, const void *data, size_t len);
unsigned char *vfs_read(Vfs *v, const char *name, size_t *len);  /* malloc'd, NUL terminated */
bool           vfs_exists(Vfs *v, const char *name);
bool           vfs_physical(Vfs *v, const char *name, char *out, size_t n);
char          *vfs_read_shader(Vfs *v, const char *name, const char *ext); /* shaders/<name>.<ext> */

/* ------------------------------------------------------------------ dynamic values / properties */
typedef enum { DV_NULL, DV_FLOAT, DV_INT, DV_BOOL, DV_VEC2, DV_VEC3, DV_VEC4, DV_STRING } DvType;

typedef struct Property Property;

typedef struct { char *name; DynVal *value; } ScriptProp;

struct DynVal {
    DvType    type;
    float     v[4];        /* vector view: getVec2/3/4 */
    float     f;
    int       i;
    bool      b;
    char     *s;           /* owned (malloc) */
    char     *cond;        /* "user": { "condition": ... } */
    Property *prop;
    char     *script;      /* SceneScript source, if any */
    VEC(ScriptProp) script_props;
};

typedef enum {
    PROP_SLIDER, PROP_BOOL, PROP_COLOR, PROP_COMBO, PROP_TEXT, PROP_SCENETEXTURE, PROP_FILE, PROP_TEXTINPUT
} PropType;

typedef struct { char *value, *label; } ComboOption;

struct Property {
    char    *name, *text;
    PropType type;
    DynVal   value;
    float    min, max, step;
    VEC(ComboOption) options;
    VEC(DynVal *) listeners;
};

typedef struct { VEC(Property *) list; } Properties;

void      dv_set_float(DynVal *d, float f);
void      dv_set_int(DynVal *d, int i);
void      dv_set_bool(DynVal *d, bool b);
void      dv_set_vec(DynVal *d, const float *v, int n);
void      dv_set_color(DynVal *d, const float c[4]);
void      dv_set_string(DynVal *d, const char *s);
void      dv_set_null(DynVal *d);
void      dv_copy(DynVal *dst, const DynVal *src);
void      dv_connect(DynVal *d, Property *p);
void      dv_free(DynVal *d);
char     *dv_to_string(const DynVal *d, char *buf, size_t n);
DynVal   *dv_new_float(Arena *a, float f);
DynVal   *dv_new_int(Arena *a, int i);
DynVal   *dv_new_bool(Arena *a, bool b);
DynVal   *dv_new_vec(Arena *a, const float *v, int n);
DynVal   *dv_new_color(Arena *a, const float c[4]);

/* Parse a value that may be a plain JSON value or a {"user":..,"value":..} object. */
DynVal   *us_parse(Arena *a, const cJSON *node, Properties *props, bool expect_color);
/* Equivalent of linux-wallpaperengine's json.user(key, props, default) helpers. */
DynVal   *us_float(Arena *a, const cJSON *o, const char *key, Properties *p, float def);
DynVal   *us_int(Arena *a, const cJSON *o, const char *key, Properties *p, int def);
DynVal   *us_bool(Arena *a, const cJSON *o, const char *key, Properties *p, bool def);
DynVal   *us_vec2(Arena *a, const cJSON *o, const char *key, Properties *p, vec2 def);
DynVal   *us_vec3(Arena *a, const cJSON *o, const char *key, Properties *p, vec3 def);
DynVal   *us_color(Arena *a, const cJSON *o, const char *key, Properties *p, vec4 def);

Property *prop_parse(const cJSON *node, const char *name);
void      prop_set(Property *p, const char *value);          /* --set-property */
void      prop_dump(const Property *p, FILE *f);
void      prop_free(Property *p);
Property *props_find(Properties *props, const char *name);

/* ------------------------------------------------------------------ materials, effects, models */
typedef struct { int idx; char *name; } TexEntry;
typedef VEC(TexEntry) TexMap;
typedef struct { char *name; int value; } Combo;
typedef VEC(Combo) ComboMap;
typedef struct { char *name; DynVal *value; } Constant;
typedef VEC(Constant) ConstMap;

void        texmap_set(Arena *a, TexMap *m, int idx, const char *name);   /* a may be NULL */
const char *texmap_get(const TexMap *m, int idx);
void        combo_set(Arena *a, ComboMap *m, const char *name, int value);
int         combo_find(const ComboMap *m, const char *name);   /* index or -1 */
int         const_find(const ConstMap *m, const char *name);

enum { BLEND_NORMAL = 1, BLEND_TRANSLUCENT = 2, BLEND_ADDITIVE = 3 };
enum { CULL_NORMAL = 1, CULL_NONE = 2 };

typedef struct {
    int      blending, cullmode;
    bool     depthtest, depthwrite;
    char    *shader;
    TexMap   textures, usertextures;
    ComboMap combos;
    ConstMap constants;
} MaterialPass;

typedef struct {
    char *filename;
    VEC(MaterialPass *) passes;
} Material;

typedef struct {
    int      id;
    ComboMap combos;
    ConstMap constants;
    TexMap   textures, usertextures;
    char    *shader_override;
} PassOverride;

typedef struct {
    Material *material;      /* may be NULL for command passes */
    TexMap    binds;
    int       command;       /* -1 none, 0 copy, 1 swap */
    char     *source, *target;
} EffectPass;

typedef struct { char *name, *format; float scale; bool unique; } EffectFbo;

typedef struct {
    char *name;
    VEC(EffectPass) passes;
    VEC(EffectFbo) fbos;
} WeEffect;

typedef struct {
    char     *filename;
    Material *material;
    bool      solidlayer, fullscreen, passthrough, autosize, nopadding;
    bool      has_width, has_height;
    int       width, height;
    char     *puppet;
} Model;

typedef struct {
    int     id;
    char   *name;
    DynVal *visible;
    VEC(PassOverride *) overrides;
    WeEffect *effect;
} ImageEffect;

typedef struct WeScene WeScene;
typedef struct WeProject WeProject;

Material *material_load(WeProject *p, const char *file);
Material *material_parse(WeProject *p, const cJSON *j, const char *file);
WeEffect *effect_load(WeProject *p, const char *file);
Model    *model_load(WeProject *p, const char *file);
PassOverride *override_parse(WeProject *p, const cJSON *j);
void      texmap_parse(Arena *a, TexMap *m, const cJSON *arr);
ConstMap  constants_parse(WeProject *p, const cJSON *obj);

/* ------------------------------------------------------------------ project */
enum { WE_TYPE_SCENE, WE_TYPE_VIDEO, WE_TYPE_WEB, WE_TYPE_APPLICATION, WE_TYPE_UNKNOWN };

struct WeProject {
    Arena      arena;
    char       dir[PATH_MAX];
    char      *title, *file, *workshop_id;
    int        type;
    bool       audio_processing;
    Properties props;
    Vfs       *vfs;
    cJSON     *json;
};

/* ------------------------------------------------------------------ textures */
typedef struct {
    uint32_t frame;
    float    frametime, x, y, width1, width2, height1, height2;
} TexFrame;

enum {
    TEXFMT_ARGB8888 = 0, TEXFMT_RGB888 = 1, TEXFMT_RGB565 = 2, TEXFMT_DXT5 = 4, TEXFMT_DXT3 = 6,
    TEXFMT_DXT1 = 7, TEXFMT_RG88 = 8, TEXFMT_R8 = 9, TEXFMT_RG1616F = 10, TEXFMT_R16F = 11,
    TEXFMT_BC7 = 12, TEXFMT_RGBA1010102 = 13, TEXFMT_RGBA16161616F = 14, TEXFMT_RGB161616F = 15
};
enum {
    TEXFLAG_NOINTERP = 1, TEXFLAG_CLAMP = 2, TEXFLAG_GIF = 4, TEXFLAG_CLAMPBORDER = 8, TEXFLAG_VIDEO = 32,
};
enum { TEXKIND_FILE, TEXKIND_FBO, TEXKIND_ALBUM, TEXKIND_DUMMY };

typedef struct Tex {
    int      kind;
    char    *name;
    GLuint  *ids;
    int      nids;
    int     *img_w, *img_h;           /* first mipmap size per image */
    int      tex_w, tex_h, width, height, gif_w, gif_h;
    float    res[4];                  /* g_TextureNResolution */
    int      format;
    uint32_t flags;
    bool     animated;
    VEC(TexFrame) frames;
    int      ss_cols, ss_rows, ss_frames;
    float    ss_duration;
    Video   *video;                   /* mp4 embedded in a .tex */
    char     video_tmp[PATH_MAX];
    /* fbo */
    GLuint   fbo;
    float    scale;
    int      refs;
} Tex;

Tex   *tex_load(WeProject *p, const char *name);              /* materials/<name>.tex */
Tex   *fbo_new(const char *name, int format, uint32_t flags, float scale, int rw, int rh, int tw, int th);
Tex   *tex_dummy(int w, int h);
void   tex_free(Tex *t);
GLuint tex_id(const Tex *t, uint32_t img);
int    tex_width(const Tex *t, uint32_t img);
int    tex_height(const Tex *t, uint32_t img);
int    tex_real_w(const Tex *t);
int    tex_real_h(const Tex *t);
void   tex_update(Tex *t);                                    /* video textures */

/* ------------------------------------------------------------------ fbo providers */
typedef struct FboProvider FboProvider;
struct FboProvider {
    FboProvider *parent;
    VEC(Tex *) fbos;          /* owned unless alias */
    VEC(char *) alias_names;
    VEC(Tex *) alias_targets;
};
Tex  *fbop_create(FboProvider *p, const char *name, int format, uint32_t flags, float scale, int rw, int rh,
                  int tw, int th);
void  fbop_alias(FboProvider *p, const char *alias, const char *original);
Tex  *fbop_find(const FboProvider *p, const char *name);
void  fbop_free(FboProvider *p);

/* ------------------------------------------------------------------ shaders */
enum { SV_FLOAT, SV_INT, SV_VEC2, SV_VEC3, SV_VEC4 };

typedef struct {
    int     type;
    char   *name;       /* uniform name */
    char   *material;   /* constant key */
    DynVal  def;
} ShaderParam;

typedef struct ShaderUnit {
    bool        vertex;
    char       *file;
    char       *preprocessed;
    ComboMap    discovered;
    VEC(ShaderParam) params;
    TexMap      textures;              /* default textures by index */
    const ComboMap *combos, *override_combos;
    const TexMap   *pass_textures, *override_textures;
    const ConstMap *constants;
    struct ShaderUnit *link;
} ShaderUnit;

typedef struct {
    ShaderUnit vertex, fragment;
    GLuint     program;
} Shader;

Shader *shader_build(WeProject *p, const char *name, const ComboMap *combos, const ComboMap *ovr_combos,
                     const TexMap *textures, const TexMap *ovr_textures, const ConstMap *constants);
void    shader_free(Shader *s);
const ShaderParam *shader_find_param(const Shader *s, const char *material);
GLuint  gl_program_from_glsl(const char *vs, const char *fs, const char *label);
bool    shaderc_init(void);

/* ------------------------------------------------------------------ passes */
enum { U_FLOAT, U_INT, U_VEC2, U_VEC3, U_VEC4, U_MAT3, U_MAT4 };

typedef struct {
    char        *name;
    GLint        loc;
    int          type, count;
    const void  *ptr;          /* value pointer, or */
    const void **pptr;         /* pointer to value pointer */
    float        own[16];      /* inline storage for copied values */
    int          own_i;
} Uniform;

typedef struct TexChain { Tex *tex; struct TexChain *next; } TexChain;

typedef struct Renderable Renderable;

typedef struct Pass {
    Renderable     *renderable;
    FboProvider    *fbos;
    const MaterialPass *mp;
    const TexMap   *binds;
    const PassOverride *ovr;
    char           *target;
    ComboMap        combos;
    Shader         *shader;
    GLuint          program, vao;
    VEC(Uniform)    uniforms;
    TexChain       *textures[16];
    int             blending;
    Tex            *draw_to, *input, *prev_input;
    const mat4     *mvp, *mvp_inv, *model, *viewproj;
    float           tex0_res[4];
    GLint           u_rot, u_trans, a_pos, a_tc;
    GLuint          pos_buf, tc_buf;
    /* custom geometry */
    void          (*geo_setup)(struct Pass *, void *);
    void          (*geo_draw)(struct Pass *, void *);
    void          (*geo_cleanup)(struct Pass *, void *);
    void           *geo_user;
    bool            ok;
} Pass;

Pass *pass_new(Renderable *r, FboProvider *fbos, const MaterialPass *mp, const PassOverride *ovr,
               const TexMap *binds, const char *target);
void  pass_free(Pass *p);
void  pass_render(Pass *p);
void  pass_uniform_ptr(Pass *p, const char *name, int type, const void *ptr, int count);
void  pass_uniform_pptr(Pass *p, const char *name, int type, const void **pptr);
void  pass_uniform_f(Pass *p, const char *name, float v);
void  pass_uniform_i(Pass *p, const char *name, int v);
void  pass_uniform_v(Pass *p, const char *name, int type, const float *v);

/* ------------------------------------------------------------------ scene objects */
enum { OBJ_NONE, OBJ_IMAGE, OBJ_SOUND, OBJ_PARTICLE, OBJ_TEXT, OBJ_GROUP };

typedef struct SceneObj SceneObj;

/* Shared header of image / particle (the "CRenderable"). */
struct Renderable {
    WeScene  *scene;
    SceneObj *obj;
    Material *material;
    Tex      *texture;
    double    anim_time;
    const float *brightness;   /* live values used by g_Brightness, g_Alpha, g_Color(4) */
    const float *alpha;
    const float *color;        /* vec4 */
    FboProvider fbos;
};

struct SceneObj {
    int       id;
    char     *name;
    int       kind;
    cJSON    *json;
    VEC(int)  deps;
    bool      has_parent;
    int       parent;
    DynVal   *origin, *scale, *angles, *visible;   /* generic transform */
    void     *impl;                                /* Image*, Particle*, ... */
    bool      created, ordered;
    void    (*render)(SceneObj *);
    void    (*destroy)(SceneObj *);
    /* scripting */
    VEC(char *)   prop_names;
    VEC(DynVal *) prop_values;
};

void sceneobj_register(SceneObj *o, const char *name, DynVal *v);

/* ------------------------------------------------------------------ scene */
typedef struct {
    float   audio16[16], audio32[32], audio64[64];
} AudioSpectrum;

struct WeScene {
    WeProject *project;
    Arena     *arena;
    cJSON     *json;
    /* camera */
    vec3       eye, center, up;
    mat4       projection, lookat;
    float      width, height;
    DynVal    *nearz, *farz, *fov;
    DynVal    *ambient, *skylight, *clear;
    DynVal    *fade;
    DynVal    *bloom, *bloom_strength, *bloom_threshold;
    DynVal    *parallax, *parallax_amount, *parallax_delay, *parallax_mouse;
    DynVal    *shake, *shake_amplitude, *shake_roughness, *shake_speed;
    /* objects */
    VEC(SceneObj *) objects;
    VEC(SceneObj *) order;
    SceneObj  *bloom_obj;
    cJSON     *bloom_json;
    /* rendering */
    FboProvider fbos;
    Tex       *scene_fbo;
    VEC(Tex *) textures;     /* texture cache */
    GLuint     vao;
    /* input */
    vec2       mouse, mouse_last, mouse_norm, parallax_disp;
    bool       mouse_down, mouse_down_last;
    double     time, time_last;
    /* scripting */
    void      *script;
    /* sounds */
    VEC(int)   sound_ids;
    bool       paused;
};

Tex  *scene_resolve_texture(WeScene *s, const char *name);
Tex  *scene_find_fbo(WeScene *s, const char *name);
SceneObj *scene_object(WeScene *s, int id);
SceneObj *scene_object_by_name(WeScene *s, const char *name);

/* globals (time, audio) shared by all wallpapers */
extern float g_Time, g_TimeLast, g_Daytime;
extern AudioSpectrum g_Audio;

/* object implementations */
bool image_create(WeScene *s, SceneObj *o);
bool particle_create(WeScene *s, SceneObj *o);
bool text_create(WeScene *s, SceneObj *o);
bool sound_create(WeScene *s, SceneObj *o);

/* ------------------------------------------------------------------ audio / sound */
void sound_system_init(void);
int  sound_play(const unsigned char *data, size_t len, const char *name, bool loop, float volume);
void sound_stop(int id);
void sound_pause(int id, bool paused);
void sound_pause_all(bool paused);
void sound_set_master(float volume, bool muted);
void sound_system_shutdown(void);
void sound_automute_tick(void);

/* ------------------------------------------------------------------ scripting */
void *script_new(WeScene *s);
void  script_free(void *e);
void  script_queue(void *e, SceneObj *o, const char *key, DynVal *v);
void  script_tick(void *e);
int   script_layer_new(void *e, DynVal *text);
void  script_layer_tick(void *e, int handle);
const char *script_layer_text(void *e, int handle);
void  script_layer_free(void *e, int handle);
void  script_user_properties_changed(void *e);

/* ------------------------------------------------------------------ media (MPRIS) */
typedef struct {
    int    state;          /* 0 stopped, 1 playing, 2 paused */
    char   title[256], artist[256], album[256], art_url[1024];
    double duration, position;
    unsigned serial;       /* increments on any change */
    unsigned art_serial;   /* increments when artwork changes */
} MediaInfo;
void       media_init(void);
void       media_update(void);
const MediaInfo *media_info(void);
void       media_shutdown(void);
Tex       *media_album_texture(bool previous);

/* ------------------------------------------------------------------ settings shared with main */
typedef struct {
    bool  disable_particles, disable_mouse, disable_parallax;
    bool  silent, no_audio_processing, automute;
    int   volume;                 /* 0..128 like linux-wallpaperengine */
    char  assets[PATH_MAX];
    bool  dump;
    bool  debug_base_only, debug_no_solid_final, debug_pass_log;
    int   debug_object;           /* -1 none */
    IntVec skip_objects, skip_effects;
    VEC(char *) prop_names;
    VEC(char *) prop_values;
} WeSettings;
extern WeSettings we_settings;

#endif
