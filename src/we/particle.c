/* Particle systems (port of linux-wallpaperengine's CParticle). */
#include "internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_PARTICLES_DEFAULT 1000
#define SPRITE_FLOATS 17
#define ROPE_FLOATS   26

typedef struct {
    vec3  pos, vel, rot, angvel;
    vec3  color;
    float alpha, size, frame, lifetime, age;
    struct { float freq, scale, phase, base; bool init; } osc_alpha, osc_size;
    struct { vec3 freq, scale, phase; bool init; } osc_pos;
    struct { vec3 color; float alpha, size, lifetime; } initial;
    bool  alive;
} Pt;

typedef struct { vec3 position, offset; bool link_mouse, world; } CtrlPt;

/* ---- definitions parsed from the particle JSON ---- */
enum { EM_BOX, EM_SPHERE };
typedef struct {
    int   kind;
    vec3  directions, dist_min, dist_max, origin;
    int   sign[3];
    uint32_t instantaneous, flags;
    float speed_min, speed_max, rate, cone, delay, duration;
    int   control_point;
    float min_pdelay, max_pdelay, min_pdur, max_pdur;
    /* runtime */
    vec3  t_origin, t_dirs;
    float rate_eff, timer, delay_timer, dur_timer, ptimer, pdur, pdelay;
    bool  emitting, inst_done;
    uint32_t remaining;
} Emitter;

enum {
    IN_COLOR, IN_SIZE, IN_ALPHA, IN_LIFETIME, IN_VELOCITY, IN_ROTATION, IN_ANGVEL, IN_TURBVEL, IN_MAPSEQ
};
typedef struct { int kind; DynVal *a, *b, *c, *d, *e, *f, *g, *h, *i; int seq; } Init;

enum {
    OP_MOVEMENT, OP_ANGMOVE, OP_ALPHAFADE, OP_SIZECHANGE, OP_ALPHACHANGE, OP_COLORCHANGE, OP_TURBULENCE, OP_VORTEX,
    OP_CPATTRACT, OP_OSCALPHA, OP_OSCSIZE, OP_OSCPOS
};
typedef struct {
    int     kind, cp, flags;
    DynVal *v[16];
    float   phase, speed;   /* turbulence: randomized once */
} Op;

typedef struct {
    Renderable r;
    DynVal  *parallax_depth;
    /* instance override */
    DynVal  *io_alpha, *io_size, *io_lifetime, *io_rate, *io_speed, *io_count, *io_color, *io_colorn;
    char    *anim_mode;
    float    seq_mult;
    uint32_t max_count, flags;
    VEC(Emitter) emitters;
    VEC(Init) inits;
    VEC(Op) ops;
    CtrlPt   cps[8];
    Pt      *pts;
    uint32_t count, cap;
    float   *verts;
    uint32_t *idx;
    double   time;
    Pass    *pass;
    PassOverride *ovr;
    TexMap   binds;
    FboProvider pfbos;
    bool     refract;
    Tex     *refract_fbo;
    GLuint   vao, vbo, ebo;
    GLint    prev_vao;
    GLsizei  active_idx;
    mat4     model, model_inv, mvp, mvp_inv, viewproj;
    vec3     up, right, fwd, view_up, view_right, eye;
    vec4     rv0, rv1;
    int      ss_cols, ss_rows, ss_frames;
    float    ss_duration;
    float    overbright, refract_amount;
    bool     trail, rope, uniform_lifetimes, rope_uv_scroll, rope_uv_smooth;
    float    trail_len, trail_max, trail_min, rope_uv_scale;
    int      rope_subdiv, rope_segments;
    vec3     t_origin;
    float    last_w, last_h;
    uint64_t rng;
    bool     initialized;
    float    white[4];
} Particle;

/* ------------------------------------------------------------------ random / noise */
static float rnd(Particle *p)
{
    p->rng ^= p->rng << 13;
    p->rng ^= p->rng >> 7;
    p->rng ^= p->rng << 17;
    return (p->rng >> 11) * (1.0 / 9007199254740992.0);
}

static float rndf(Particle *p, float a, float b)
{
    if (b < a) {
        float t = a;
        a = b;
        b = t;
    }
    return a + (b - a) * rnd(p);
}

static vec3 rndv(Particle *p, const float *a, const float *b)
{
    return v3(rndf(p, a[0], b[0]), rndf(p, a[1], b[1]), rndf(p, a[2], b[2]));
}

static const unsigned char PERM[512] = {
    151, 160, 137, 91, 90, 15, 131, 13, 201, 95, 96, 53, 194, 233, 7, 225, 140, 36, 103, 30, 69, 142, 8, 99, 37,
    240, 21, 10, 23, 190, 6, 148, 247, 120, 234, 75, 0, 26, 197, 62, 94, 252, 219, 203, 117, 35, 11, 32, 57, 177,
    33, 88, 237, 149, 56, 87, 174, 20, 125, 136, 171, 168, 68, 175, 74, 165, 71, 134, 139, 48, 27, 166, 77, 146,
    158, 231, 83, 111, 229, 122, 60, 211, 133, 230, 220, 105, 92, 41, 55, 46, 245, 40, 244, 102, 143, 54, 65, 25,
    63, 161, 1, 216, 80, 73, 209, 76, 132, 187, 208, 89, 18, 169, 200, 196, 135, 130, 116, 188, 159, 86, 164, 100,
    109, 198, 173, 186, 3, 64, 52, 217, 226, 250, 124, 123, 5, 202, 38, 147, 118, 126, 255, 82, 85, 212, 207, 206,
    59, 227, 47, 16, 58, 17, 182, 189, 28, 42, 223, 183, 170, 213, 119, 248, 152, 2, 44, 154, 163, 70, 221, 153,
    101, 155, 167, 43, 172, 9, 129, 22, 39, 253, 19, 98, 108, 110, 79, 113, 224, 232, 178, 185, 112, 104, 218, 246,
    97, 228, 251, 34, 242, 193, 238, 210, 144, 12, 191, 179, 162, 241, 81, 51, 145, 235, 249, 14, 239, 107, 49, 192,
    214, 31, 181, 199, 106, 157, 184, 84, 204, 176, 115, 121, 50, 45, 127, 4, 150, 254, 138, 236, 205, 93, 222, 114,
    67, 29, 24, 72, 243, 141, 128, 195, 78, 66, 215, 61, 156, 180,
    151, 160, 137, 91, 90, 15, 131, 13, 201, 95, 96, 53, 194, 233, 7, 225, 140, 36, 103, 30, 69, 142, 8, 99, 37,
    240, 21, 10, 23, 190, 6, 148, 247, 120, 234, 75, 0, 26, 197, 62, 94, 252, 219, 203, 117, 35, 11, 32, 57, 177,
    33, 88, 237, 149, 56, 87, 174, 20, 125, 136, 171, 168, 68, 175, 74, 165, 71, 134, 139, 48, 27, 166, 77, 146,
    158, 231, 83, 111, 229, 122, 60, 211, 133, 230, 220, 105, 92, 41, 55, 46, 245, 40, 244, 102, 143, 54, 65, 25,
    63, 161, 1, 216, 80, 73, 209, 76, 132, 187, 208, 89, 18, 169, 200, 196, 135, 130, 116, 188, 159, 86, 164, 100,
    109, 198, 173, 186, 3, 64, 52, 217, 226, 250, 124, 123, 5, 202, 38, 147, 118, 126, 255, 82, 85, 212, 207, 206,
    59, 227, 47, 16, 58, 17, 182, 189, 28, 42, 223, 183, 170, 213, 119, 248, 152, 2, 44, 154, 163, 70, 221, 153,
    101, 155, 167, 43, 172, 9, 129, 22, 39, 253, 19, 98, 108, 110, 79, 113, 224, 232, 178, 185, 112, 104, 218, 246,
    97, 228, 251, 34, 242, 193, 238, 210, 144, 12, 191, 179, 162, 241, 81, 51, 145, 235, 249, 14, 239, 107, 49, 192,
    214, 31, 181, 199, 106, 157, 184, 84, 204, 176, 115, 121, 50, 45, 127, 4, 150, 254, 138, 236, 205, 93, 222, 114,
    67, 29, 24, 72, 243, 141, 128, 195, 78, 66, 215, 61, 156, 180
};

static double grad(int h, double x, double y, double z)
{
    switch (h & 15) {
    case 0: return x + y;
    case 1: return -x + y;
    case 2: return x - y;
    case 3: return -x - y;
    case 4: return x + z;
    case 5: return -x + z;
    case 6: return x - z;
    case 7: return -x - z;
    case 8: return y + z;
    case 9: return -y + z;
    case 10: return y - z;
    case 11: return -y - z;
    case 12: return y + x;
    case 13: return -y + z;
    case 14: return y - x;
    default: return -y - z;
    }
}

static double fade(double t) { return t * t * t * (t * (t * 6 - 15) + 10); }
static double lrp(double t, double a, double b) { return a + t * (b - a); }

static double perlin(double x, double y, double z)
{
    int X = (int)floor(x) & 255, Y = (int)floor(y) & 255, Z = (int)floor(z) & 255;
    x -= floor(x);
    y -= floor(y);
    z -= floor(z);
    double u = fade(x), v = fade(y), w = fade(z);
    int A = PERM[X] + Y, AA = PERM[A] + Z, AB = PERM[A + 1] + Z, B = PERM[X + 1] + Y, BA = PERM[B] + Z,
        BB = PERM[B + 1] + Z;
    return lrp(w,
               lrp(v, lrp(u, grad(PERM[AA], x, y, z), grad(PERM[BA], x - 1, y, z)),
                   lrp(u, grad(PERM[AB], x, y - 1, z), grad(PERM[BB], x - 1, y - 1, z))),
               lrp(v, lrp(u, grad(PERM[AA + 1], x, y, z - 1), grad(PERM[BA + 1], x - 1, y, z - 1)),
                   lrp(u, grad(PERM[AB + 1], x, y - 1, z - 1), grad(PERM[BB + 1], x - 1, y - 1, z - 1))));
}

static vec3 pnv(vec3 p)
{
    return v3(perlin(p.x, p.y, p.z), perlin(p.x + 89.2, p.y + 33.1, p.z + 57.3),
              perlin(p.x + 100.3, p.y + 120.1, p.z + 142.2));
}

static vec3 curl(vec3 p)
{
    const float e = 1e-4f;
    vec3 x0 = pnv(v3(p.x - e, p.y, p.z)), x1 = pnv(v3(p.x + e, p.y, p.z));
    vec3 y0 = pnv(v3(p.x, p.y - e, p.z)), y1 = pnv(v3(p.x, p.y + e, p.z));
    vec3 z0 = pnv(v3(p.x, p.y, p.z - e)), z1 = pnv(v3(p.x, p.y, p.z + e));
    return v3s(v3((y1.z - y0.z) - (z1.y - z0.y), (z1.x - z0.x) - (x1.z - x0.z), (x1.y - x0.y) - (y1.x - y0.x)),
               1.0f / (2 * e));
}

static float fade_value(float life, float st, float et, float sv, float ev)
{
    if (life <= st) return sv;
    if (life >= et) return ev;
    return sv + (life - st) / (et - st) * (ev - sv);
}

static float life_pos(const Pt *p) { return p->lifetime > 0 ? p->age / p->lifetime : 1.0f; }

/* ------------------------------------------------------------------ parsing */
static vec3 em_vec3(const cJSON *j, const char *k, vec3 def) { return js_vec3(j, k, def); }

static void parse_emitter(Particle *P, const cJSON *j)
{
    const char *name = js_str(j, "name", "");
    Emitter e = { 0 };
    if (!strcmp(name, "boxrandom")) e.kind = EM_BOX;
    else if (!strcmp(name, "sphererandom")) e.kind = EM_SPHERE;
    else {
        we_dbg("unknown particle emitter %s", name);
        return;
    }
    e.directions = em_vec3(j, "directions", v3(1, 1, 0));
    e.dist_min = em_vec3(j, "distancemin", v3(0, 0, 0));
    e.dist_max = em_vec3(j, "distancemax", v3(256, 256, 0));
    e.origin = em_vec3(j, "origin", v3(0, 0, 0));
    const cJSON *sign = js_get(j, "sign");
    if (sign && cJSON_IsArray(sign) && cJSON_GetArraySize(sign) >= 3)
        for (int i = 0; i < 3; i++) e.sign[i] = cJSON_GetArrayItem(sign, i)->valueint;
    e.instantaneous = js_int(j, "instantaneous", 0);
    e.speed_min = js_num(j, "speedmin", 0);
    e.speed_max = js_num(j, "speedmax", 0);
    e.rate = js_num(j, "rate", 10);
    e.control_point = js_int(j, "controlpoint", 0);
    e.flags = js_int(j, "flags", 0);
    e.cone = js_num(j, "cone", 0);
    e.delay = js_num(j, "delay", 0);
    e.duration = js_num(j, "duration", 0);
    e.min_pdelay = js_num(j, "minperiodicdelay", 1);
    e.max_pdelay = js_num(j, "maxperiodicdelay", 2);
    e.min_pdur = js_num(j, "minperiodicduration", 2);
    e.max_pdur = js_num(j, "maxperiodicduration", 3);
    VPUSH(P->emitters, e);
}

#define UF(k, d) us_float(a, j, k, pr, d)
#define UI(k, d) us_int(a, j, k, pr, d)
#define UV3(k, x, y, z) us_vec3(a, j, k, pr, v3(x, y, z))
#define UV2(k, x, y) us_vec2(a, j, k, pr, v2(x, y))

static void parse_init(Particle *P, WeProject *p, const cJSON *j)
{
    Arena *a = &p->arena;
    Properties *pr = &p->props;
    const char *n = js_str(j, "name", "");
    Init in = { 0 };
    if (!strcmp(n, "colorrandom")) {
        in.kind = IN_COLOR;
        in.a = us_color(a, j, "min", pr, v4(0, 0, 0, 1));
        in.b = us_color(a, j, "max", pr, v4(1, 1, 1, 1));
    } else if (!strcmp(n, "sizerandom")) {
        in.kind = IN_SIZE;
        in.a = UF("min", 0);
        in.b = UF("max", 20);
        in.c = UF("exponent", 1);
    } else if (!strcmp(n, "alpharandom")) {
        in.kind = IN_ALPHA;
        in.a = UF("min", 0.05f);
        in.b = UF("max", 1);
    } else if (!strcmp(n, "lifetimerandom")) {
        in.kind = IN_LIFETIME;
        in.a = UF("min", 0);
        in.b = UF("max", 1);
        P->uniform_lifetimes = in.a->f == in.b->f;
    } else if (!strcmp(n, "velocityrandom")) {
        in.kind = IN_VELOCITY;
        in.a = UV3("min", -32, -32, -32);
        in.b = UV3("max", 32, 32, 32);
    } else if (!strcmp(n, "rotationrandom")) {
        in.kind = IN_ROTATION;
        in.a = UV3("min", 0, 0, 0);
        in.b = UV3("max", 0, 0, 2 * WE_PI);
    } else if (!strcmp(n, "angularvelocityrandom")) {
        in.kind = IN_ANGVEL;
        in.a = UV3("min", 0, 0, -5);
        in.b = UV3("max", 0, 0, 5);
        in.c = UF("exponent", 1);
    } else if (!strcmp(n, "turbulentvelocityrandom")) {
        in.kind = IN_TURBVEL;
        in.a = UF("speedmin", 100);
        in.b = UF("speedmax", 250);
        in.c = UF("scale", 1);
        in.d = UF("offset", 0);
        in.e = UV3("forward", 0, 1, 0);
        in.f = UF("timescale", 1);
        in.g = UF("phasemin", 0);
        in.h = UF("phasemax", 0.1f);
        in.i = UV3("right", 0, 0, 1);
    } else if (!strcmp(n, "mapsequencearoundcontrolpoint")) {
        in.kind = IN_MAPSEQ;
        in.a = UI("controlpoint", 0);
        in.b = UI("count", 1);
        in.c = UV3("speedmin", 0, 0, 0);
        in.d = UV3("speedmax", 100, 100, 100);
    } else {
        we_dbg("unknown particle initializer %s", n);
        return;
    }
    VPUSH(P->inits, in);
}

static void parse_op(Particle *P, WeProject *p, const cJSON *j)
{
    Arena *a = &p->arena;
    Properties *pr = &p->props;
    const char *n = js_str(j, "name", "");
    Op o = { 0 };
    DynVal **v = o.v;
    if (!strcmp(n, "movement")) {
        o.kind = OP_MOVEMENT;
        v[0] = UF("drag", 0);
        v[1] = UV3("gravity", 0, 0, 0);
    } else if (!strcmp(n, "angularmovement")) {
        o.kind = OP_ANGMOVE;
        v[0] = UF("drag", 0);
        v[1] = UV3("force", 0, 0, 0);
    } else if (!strcmp(n, "alphafade")) {
        o.kind = OP_ALPHAFADE;
        v[0] = UF("fadeintime", 0.5f);
        v[1] = UF("fadeouttime", 0.5f);
    } else if (!strcmp(n, "sizechange") || !strcmp(n, "alphachange")) {
        o.kind = n[0] == 's' ? OP_SIZECHANGE : OP_ALPHACHANGE;
        v[0] = UF("starttime", 0);
        v[1] = UF("endtime", 1);
        v[2] = UF("startvalue", 1);
        v[3] = UF("endvalue", 0);
    } else if (!strcmp(n, "colorchange")) {
        o.kind = OP_COLORCHANGE;
        v[0] = UF("starttime", 0);
        v[1] = UF("endtime", 1);
        v[2] = UV3("startvalue", 1, 1, 1);
        v[3] = UV3("endvalue", 1, 1, 1);
    } else if (!strcmp(n, "turbulence")) {
        o.kind = OP_TURBULENCE;
        v[0] = UF("scale", 0.005f);
        v[1] = UF("speedmin", 500);
        v[2] = UF("speedmax", 1000);
        v[3] = UF("timescale", 0.01f);
        v[4] = UV3("mask", 1, 1, 0);
        v[5] = UF("phasemin", 0);
        v[6] = UF("phasemax", 0);
        v[7] = UI("audioprocessingmode", 0);
        v[8] = UV2("audioprocessingbounds", 0, 1);
        o.phase = rndf(P, v[5]->f, v[6]->f);
        o.speed = rndf(P, v[1]->f, v[2]->f);
    } else if (!strcmp(n, "vortex") || !strcmp(n, "vortex_v2")) {
        o.kind = OP_VORTEX;
        o.cp = js_int(j, "controlpoint", 0);
        o.flags = js_int(j, "flags", 0);
        v[0] = UV3("axis", 0, 0, 1);
        v[1] = UV3("offset", 0, 0, 0);
        v[2] = UF("distanceinner", 500);
        v[3] = UF("distanceouter", 650);
        v[4] = UF("speedinner", 2500);
        v[5] = UF("speedouter", 0);
        v[6] = UF("centerforce", 1);
        v[7] = UF("ringradius", 300);
        v[8] = UF("ringwidth", 50);
        v[9] = UF("ringpulldistance", 50);
        v[10] = UF("ringpullforce", 10);
        v[11] = UI("audioprocessingmode", 0);
        v[12] = UV2("audioprocessingbounds", 0, 1);
    } else if (!strcmp(n, "controlpointattract")) {
        o.kind = OP_CPATTRACT;
        o.cp = js_int(j, "controlpoint", 0);
        v[0] = UV3("origin", 0, 0, 0);
        v[1] = UF("scale", 100);
        v[2] = UF("threshold", 1000);
    } else if (!strcmp(n, "oscillatealpha") || !strcmp(n, "oscillatesize") || !strcmp(n, "oscillateposition")) {
        bool pos = !strcmp(n, "oscillateposition");
        bool alpha = !strcmp(n, "oscillatealpha");
        o.kind = pos ? OP_OSCPOS : alpha ? OP_OSCALPHA : OP_OSCSIZE;
        v[0] = UF("frequencymin", 0);
        v[1] = UF("frequencymax", pos ? 5 : 10);
        v[2] = UF("scalemin", alpha || pos ? 0 : 0.8f);
        v[3] = UF("scalemax", pos ? 10 : alpha ? 1 : 1.2f);
        v[4] = UF("phasemin", 0);
        v[5] = UF("phasemax", 2 * WE_PI);
        if (pos) v[6] = UV3("mask", 1, 1, 0);
    } else {
        we_dbg("unknown particle operator %s", n);
        return;
    }
    VPUSH(P->ops, o);
}

/* ------------------------------------------------------------------ simulation */
static void apply_inits(Particle *P, Pt *p)
{
    for (int k = 0; k < P->inits.n; k++) {
        Init *in = &P->inits.v[k];
        switch (in->kind) {
        case IN_COLOR:
            p->color = v3mul(rndv(P, in->a->v, in->b->v), v3(P->io_colorn->v[0], P->io_colorn->v[1], P->io_colorn->v[2]));
            p->initial.color = p->color;
            break;
        case IN_SIZE: {
            float t = powf(rnd(P), in->c->f);
            p->size = (in->a->f + t * (in->b->f - in->a->f)) * P->io_size->f / 2.0f;
            p->initial.size = p->size;
            break;
        }
        case IN_ALPHA:
            p->alpha = rndf(P, in->a->f, in->b->f) * P->io_alpha->f;
            p->initial.alpha = p->alpha;
            break;
        case IN_LIFETIME:
            p->lifetime = rndf(P, in->a->f, in->b->f) * P->io_lifetime->f;
            p->initial.lifetime = p->lifetime;
            break;
        case IN_VELOCITY: {
            vec3 v = v3s(rndv(P, in->a->v, in->b->v), P->io_speed->f);
            v.y = -v.y;
            p->vel = v3add(p->vel, v);
            break;
        }
        case IN_ROTATION: p->rot = v3s(rndv(P, in->a->v, in->b->v), P->io_speed->f); break;
        case IN_ANGVEL: {
            vec3 r;
            float *rr = &r.x;
            for (int i = 0; i < 3; i++) {
                float t = powf(rnd(P), in->c->f);
                rr[i] = in->a->v[i] + t * (in->b->v[i] - in->a->v[i]);
            }
            p->angvel = v3s(r, P->io_speed->f);
            break;
        }
        case IN_TURBVEL: {
            vec3 fwd = v3(in->e->v[0], -in->e->v[1], in->e->v[2]);
            vec3 right = v3(in->i->v[0], -in->i->v[1], in->i->v[2]);
            fwd = v3len(fwd) > 0.0001f ? v3norm(fwd) : v3(0, 1, 0);
            right = v3len(right) > 0.0001f ? v3norm(right) : v3(1, 0, 0);
            float speed = rndf(P, in->a->f, in->b->f), scale = in->c->f, offset = in->d->f;
            vec3 np = v3add(v3s(p->pos, 0.1f), v3((float)P->time * in->f->f, (float)P->time * in->f->f,
                                                    (float)P->time * in->f->f));
            float ph = rndf(P, in->g->f, in->h->f);
            vec3 r = curl(v3add(np, v3(ph, ph * 0.7f, ph * 1.3f)));
            float l = v3len(r);
            r = l < 0.0001f ? fwd : v3s(r, 1 / l);
            if (scale < 2.0f) {
                float ang = acosf(clampf(v3dot(r, fwd), -1, 1)) / WE_PI, maxa = scale / 2;
                if (ang > maxa && maxa > 0.0001f) {
                    vec3 axis = v3cross(r, fwd);
                    float al = v3len(axis);
                    if (al > 0.0001f) r = m3v3_rot(m4rotation((ang - maxa) * WE_PI, v3s(axis, 1 / al)), r);
                }
            }
            if (fabsf(offset) > 0.0001f) r = m3v3_rot(m4rotation(-offset, right), r);
            if (!(P->flags & 4)) {
                r.z = 0;
                float l2 = v3len(r);
                if (l2 > 0.0001f) r = v3s(r, 1 / l2);
            }
            p->vel = v3add(p->vel, v3s(r, speed * P->io_speed->f));
            break;
        }
        case IN_MAPSEQ: {
            int cp = in->a->i, count = in->b->i > 0 ? in->b->i : 1;
            float ang = (float)in->seq / count * 2 * WE_PI;
            in->seq = (in->seq + 1) % count;
            p->pos = cp >= 0 && cp < 8 ? P->cps[cp].position : v3(0, 0, 0);
            vec3 sp = rndv(P, in->c->v, in->d->v);
            sp.y = -sp.y;
            float c = cosf(ang), s = sinf(ang);
            /* glm::mat3 is column-major: rotationMatrix * speed */
            vec3 rs = v3(c * sp.x + s * sp.y, -s * sp.x + c * sp.y, sp.z);
            p->vel = v3s(rs, P->io_speed->f);
            break;
        }
        }
    }
}

static void spawn(Particle *P, Pt *p, vec3 pos, vec3 vel, float lifetime)
{
    memset(p, 0, sizeof *p);
    p->pos = pos;
    p->vel = vel;
    p->color = v3(P->io_colorn->v[0], P->io_colorn->v[1], P->io_colorn->v[2]);
    p->alpha = P->io_alpha->f;
    p->size = 20.0f * P->io_size->f;
    p->lifetime = lifetime;
    p->alive = true;
    p->frame = -1;
    p->initial.color = p->color;
    p->initial.alpha = p->alpha;
    p->initial.size = p->size;
    p->initial.lifetime = p->lifetime;
    p->osc_alpha.base = 1;
    p->osc_size.base = 1;
    p->osc_pos.scale = v3(1, 1, 1);
    apply_inits(P, p);
}

static void emit(Particle *P, Emitter *e, float dt)
{
    if (P->count >= P->cap) return;
    uint32_t n = 0;
    if (e->kind == EM_BOX) {
        if (e->delay_timer > 0) {
            e->delay_timer -= dt;
            return;
        }
        if (e->duration > 0) {
            e->dur_timer += dt;
            if (e->dur_timer >= e->duration) return;
        }
        if (e->flags & 4) {
            e->ptimer += dt;
            if (!e->emitting) {
                if (e->ptimer >= e->pdelay) {
                    e->emitting = true;
                    e->ptimer = 0;
                    e->pdur = rndf(P, e->min_pdur, e->max_pdur);
                } else {
                    return;
                }
            } else if (e->ptimer >= e->pdur) {
                e->emitting = false;
                e->ptimer = 0;
                e->pdelay = rndf(P, e->min_pdelay, e->max_pdelay);
                return;
            }
        }
        if (e->instantaneous > 0 && !e->inst_done) {
            n = e->instantaneous;
            e->inst_done = true;
        }
        if (e->rate > 0) {
            e->timer += dt * e->rate_eff;
            uint32_t r = (uint32_t)e->timer;
            e->timer -= r;
            if ((e->flags & 2) && r > 1) r = 1;
            n += r;
        }
        for (uint32_t i = 0; i < n && P->count < P->cap; i++) {
            vec3 o = e->t_origin;
            if (e->control_point >= 0 && e->control_point < 8) o = v3add(o, P->cps[e->control_point].position);
            float rp[3];
            const float *mn = &e->dist_min.x, *mx = &e->dist_max.x;
            for (int ax = 0; ax < 3; ax++) {
                float d = rndf(P, mn[ax], mx[ax]);
                if (rnd(P) < 0.5f) d = -d;
                rp[ax] = d;
            }
            vec3 pos = v3add(o, v3mul(v3(rp[0], rp[1], rp[2]), e->t_dirs));
            spawn(P, &P->pts[P->count], pos, v3(0, 0, 0), P->io_lifetime->f);
            P->count++;
        }
    } else {
        e->timer += dt * e->rate_eff;
        n = (uint32_t)e->timer;
        e->timer -= n;
        if ((e->flags & 2) && n > 1) n = 1;
        if (e->remaining > 0) {
            n = e->remaining;
            e->remaining = 0;
        }
        for (uint32_t i = 0; i < n && P->count < P->cap; i++) {
            vec3 o = e->t_origin;
            if (e->control_point >= 0 && e->control_point < 8) o = v3add(o, P->cps[e->control_point].position);
            vec3 rp;
            if (!(P->flags & 4)) {
                float ang = rndf(P, 0, 2 * WE_PI), mnr = e->dist_min.x, mxr = e->dist_max.x;
                float r = sqrtf(rndf(P, mnr * mnr, mxr * mxr));
                rp = v3(r * cosf(ang), r * sinf(ang), rndf(P, -mxr, mxr));
            } else {
                float th = rndf(P, 0, 2 * WE_PI), ct = rndf(P, -1, 1), st = sqrtf(1 - ct * ct);
                rp = v3(st * cosf(th), st * sinf(th), ct);
                float mnr = e->dist_min.x, mxr = e->dist_max.x;
                rp = v3s(rp, cbrtf(rndf(P, mnr * mnr * mnr, mxr * mxr * mxr)));
            }
            rp = v3mul(rp, e->directions);
            float *c = &rp.x;
            for (int k = 0; k < 3; k++) {
                if (e->sign[k] == 1) c[k] = fabsf(c[k]);
                else if (e->sign[k] == -1) c[k] = -fabsf(c[k]);
            }
            vec3 vel = v3(0, 0, 0);
            if (e->speed_max > 0 || e->speed_min != 0) {
                vec3 dir = v3len(rp) > 0 ? v3norm(rp) : v3(0, 1, 0);
                vel = v3s(dir, rndf(P, e->speed_min, e->speed_max));
            }
            spawn(P, &P->pts[P->count], v3add(o, rp), vel, P->io_lifetime->f);
            P->count++;
        }
    }
}

static void run_op(Particle *P, Op *o, float t, float dt)
{
    DynVal **v = o->v;
    float speed = P->io_speed->f;
    for (uint32_t i = 0; i < P->count; i++) {
        Pt *p = &P->pts[i];
        if (!p->alive) continue;
        float life = life_pos(p);
        switch (o->kind) {
        case OP_MOVEMENT: {
            vec3 g = v3(v[1]->v[0], -v[1]->v[1], v[1]->v[2]);
            p->pos = v3add(p->pos, v3s(p->vel, dt));
            p->vel = v3add(p->vel, v3s(g, dt * speed));
            float df = 1 - v[0]->f * dt;
            p->vel = v3s(p->vel, df < 0 ? 0 : df);
            break;
        }
        case OP_ANGMOVE: {
            vec3 f = v3(v[1]->v[0], v[1]->v[1], v[1]->v[2]);
            p->rot = v3add(p->rot, v3s(p->angvel, dt * speed));
            p->angvel = v3add(p->angvel, v3s(f, dt * speed));
            float df = 1 - v[0]->f * dt;
            p->angvel = v3s(p->angvel, df < 0 ? 0 : df);
            float *r = &p->rot.x;
            for (int k = 0; k < 3; k++) {
                while (r[k] > WE_PI) r[k] -= 2 * WE_PI;
                while (r[k] < -WE_PI) r[k] += 2 * WE_PI;
            }
            break;
        }
        case OP_ALPHAFADE: {
            float fi = v[0]->f, fo = v[1]->f;
            if (life <= fi) p->alpha = p->initial.alpha * fade_value(life, 0, fi, 0, 1);
            else if (life > fo) p->alpha = p->initial.alpha * (1 - fade_value(life, fo, 1, 0, 1));
            else p->alpha = p->initial.alpha;
            p->osc_alpha.base = p->alpha;
            break;
        }
        case OP_SIZECHANGE:
            p->size = p->initial.size * fade_value(life, v[0]->f, v[1]->f, v[2]->f, v[3]->f);
            p->osc_size.base = p->size;
            break;
        case OP_ALPHACHANGE:
            p->alpha = p->initial.alpha * fade_value(life, v[0]->f, v[1]->f, v[2]->f, v[3]->f);
            p->osc_alpha.base = p->alpha;
            break;
        case OP_COLORCHANGE: {
            vec3 c = v3(fade_value(life, v[0]->f, v[1]->f, v[2]->v[0], v[3]->v[0]),
                        fade_value(life, v[0]->f, v[1]->f, v[2]->v[1], v[3]->v[1]),
                        fade_value(life, v[0]->f, v[1]->f, v[2]->v[2], v[3]->v[2]));
            p->color = v3mul(p->initial.color, c);
            break;
        }
        case OP_TURBULENCE: {
            if (o->speed <= 0.0001f) return;
            vec3 np = p->pos;
            np.x += o->phase + v[3]->f * t;
            np = v3s(np, v[0]->f * 2);
            vec3 c = curl(np);
            float l = v3len(c);
            if (l > 0.0001f) c = v3s(c, o->speed / l);
            c = v3mul(c, v3(v[4]->v[0], v[4]->v[1], v[4]->v[2]));
            p->vel = v3add(p->vel, v3s(c, dt * speed));
            break;
        }
        case OP_VORTEX: {
            float amp = 0;
            if (v[11]->i > 0) {
                /* audio-driven vortices react to the bass level */
                amp = g_Audio.audio16[0] + g_Audio.audio16[1];
                if (amp == 0) return;
            }
            vec3 axis = v3(v[0]->v[0], v[0]->v[1], v[0]->v[2]);
            axis = v3len(axis) > 0 ? v3norm(axis) : v3(0, 0, 1);
            vec3 off = v3(v[1]->v[0], v[1]->v[1], v[1]->v[2]);
            vec3 center = o->cp >= 0 && o->cp < 8 ? v3add(P->cps[o->cp].position, off) : off;
            float si = v[4]->f, so = v[5]->f;
            if (v[11]->i > 0) {
                si *= 1 + amp;
                so *= 1 + amp;
            }
            vec3 tp = v3sub(p->pos, center), radial = tp;
            if (o->flags & 1) radial = v3sub(tp, v3s(axis, v3dot(tp, axis)));
            float dist = v3len(radial);
            vec3 tan = v3cross(axis, radial);
            if (v3len(tan) <= 0.001f) continue;
            tan = v3norm(tan);
            float sp = 0;
            vec3 rf = v3(0, 0, 0);
            if (o->flags & 4) {
                float rr = v[7]->f, rw = v[8]->f, ri = rr - rw * 0.5f, ro = rr + rw * 0.5f;
                if (dist < ri) sp = 0;
                else if (dist <= ro) sp = lerpf(si, so, (dist - ri) / rw);
                else if (dist <= ro + v[9]->f) {
                    float pt = (dist - ro) / v[9]->f;
                    sp = so * (1 - pt);
                    if (dist > 0.001f) rf = v3s(v3norm(radial), -v[10]->f * pt);
                }
            } else {
                float di = v[2]->f, dd = v[3]->f, mid = dd - di + 0.1f;
                if (mid < 0 || dist < di) sp = si;
                else if (dist > dd) sp = so;
                else sp = lerpf(si, so, (dist - di) / mid);
            }
            p->vel = v3add(p->vel, v3s(tan, sp * dt * speed));
            p->vel = v3add(p->vel, v3s(rf, dt * speed));
            if ((o->flags & 2) && dist > 0.001f) p->vel = v3add(p->vel, v3s(v3norm(radial), -v[6]->f * dt * speed));
            break;
        }
        case OP_CPATTRACT: {
            if (o->cp < 0 || o->cp >= 8) return;
            vec3 c = v3add(P->cps[o->cp].position, v3(v[0]->v[0], v[0]->v[1], v[0]->v[2]));
            vec3 to = v3sub(c, p->pos);
            float d = v3len(to);
            if (d > 0.001f && d < v[2]->f / 2) p->vel = v3add(p->vel, v3s(to, v[1]->f * dt * speed / d));
            break;
        }
        case OP_OSCALPHA:
        case OP_OSCSIZE: {
            bool al = o->kind == OP_OSCALPHA;
            typeof(p->osc_alpha) *os = al ? &p->osc_alpha : &p->osc_size;
            if (!os->init) {
                os->freq = rndf(P, v[0]->f, v[1]->f);
                os->scale = rndf(P, v[2]->f, v[3]->f);
                os->phase = rndf(P, v[4]->f, v[5]->f + 2 * WE_PI);
                os->base = al ? p->alpha : p->size;
                os->init = true;
            }
            float cv = (cosf(os->freq * p->age + os->phase) + 1) * 0.5f;
            float m = lerpf(v[2]->f, v[3]->f, cv);
            if (al) p->alpha = os->base * m;
            else p->size = os->base * m;
            break;
        }
        case OP_OSCPOS: {
            float *fq = &p->osc_pos.freq.x, *sc = &p->osc_pos.scale.x, *ph = &p->osc_pos.phase.x;
            if (!p->osc_pos.init) {
                for (int k = 0; k < 3; k++) {
                    fq[k] = rndf(P, v[0]->f, v[1]->f);
                    sc[k] = rndf(P, v[2]->f, v[3]->f);
                    ph[k] = rndf(P, v[4]->f, v[5]->f + 2 * WE_PI);
                }
                p->osc_pos.init = true;
            }
            float d[3];
            for (int k = 0; k < 3; k++) {
                float w = fq[k];
                d[k] = -sc[k] * w * sinf(w * p->age + ph[k]) * dt * v[6]->v[k] * speed;
            }
            p->pos = v3add(p->pos, v3(d[0], d[1], d[2]));
            break;
        }
        }
    }
}

static void update(Particle *P, float dt)
{
    WeScene *s = P->r.scene;
    float sw = s->width, sh = s->height;
    if (sw != P->last_w || sh != P->last_h) {
        P->t_origin = v3(P->r.obj->origin->v[0] - sw / 2, sh / 2 - P->r.obj->origin->v[1], P->r.obj->origin->v[2]);
        for (int i = 0; i < 8; i++)
            if (!P->cps[i].link_mouse && P->cps[i].world) P->cps[i].position = v3sub(P->cps[i].offset, P->t_origin);
        P->last_w = sw;
        P->last_h = sh;
    }
    for (int i = 0; i < 8; i++)
        if (P->cps[i].link_mouse) {
            vec3 pos = v3(s->mouse_norm.x * sw - sw / 2, sh / 2 - s->mouse_norm.y * sh, 0);
            P->cps[i].position = v3sub(v3add(pos, P->cps[i].offset), P->t_origin);
        }
    for (int i = 0; i < P->emitters.n; i++) emit(P, &P->emitters.v[i], dt);
    for (uint32_t i = 0; i < P->count; i++) P->pts[i].age += dt;
    for (int i = 0; i < P->ops.n; i++) run_op(P, &P->ops.v[i], (float)P->time, dt);
    for (uint32_t i = 0; i < P->count; i++) {
        Pt *p = &P->pts[i];
        if (P->ss_frames <= 0) continue;
        float lp = life_pos(p), sp = P->seq_mult > 0 ? P->seq_mult : 1;
        if (!strcmp(P->anim_mode, "randomframe")) {
            if (p->frame < 0) p->frame = (float)(int)(rnd(P) * P->ss_frames);
        } else if (!strcmp(P->anim_mode, "once")) {
            p->frame = fminf(lp * P->ss_frames * sp, P->ss_frames - 1);
        } else if (P->ss_duration > 0) {
            float cyc = fmodf(p->age * sp, P->ss_duration) / P->ss_duration;
            p->frame = fmodf(cyc * P->ss_frames, P->ss_frames);
        } else {
            p->frame = fmodf(lp * P->ss_frames * sp, P->ss_frames);
        }
    }
    uint32_t w = 0;
    for (uint32_t r = 0; r < P->count; r++)
        if (P->pts[r].alive && P->pts[r].age < P->pts[r].lifetime) {
            if (w != r) P->pts[w] = P->pts[r];
            w++;
        }
    P->count = w;
}

/* ------------------------------------------------------------------ rendering */
static void update_matrices(Particle *P)
{
    WeScene *s = P->r.scene;
    SceneObj *o = P->r.obj;
    mat4 m = m4translate(m4id(), P->t_origin);
    if (s->parallax->b && !we_settings.disable_parallax) {
        float amount = s->parallax_amount->f;
        float dx = P->parallax_depth->v[0], dy = P->parallax_depth->v[1];
        if (fabsf(dx) < 0.65f) dx = dx < 0 ? -0.65f : 0.65f;
        if (fabsf(dy) < 0.65f) dy = dy < 0 ? -0.65f : 0.65f;
        m = m4translate(m, v3((dx + amount) * s->parallax_disp.x * s->width,
                              (dy + amount) * s->parallax_disp.y * s->width, 0));
    }
    m = m4rotate(m, -o->angles->v[2], v3(0, 0, 1));
    m = m4rotate(m, o->angles->v[1], v3(0, 1, 0));
    m = m4rotate(m, -o->angles->v[0], v3(1, 0, 0));
    m = m4scale(m, v3(o->scale->v[0], o->scale->v[1], o->scale->v[2]));
    P->model = m;
    P->model_inv = m4inverse(m);
    if (P->flags & 4) {
        float aspect = s->width / s->height;
        P->viewproj = m4mul(m4perspective(s->fov->f * WE_PI / 180, aspect, s->nearz->f, s->farz->f),
                            m4lookat(v3(0, 0, 1000), v3(0, 0, 0), v3(0, 1, 0)));
    } else {
        P->viewproj = m4mul(s->projection, s->lookat);
    }
    P->eye = v3(0, 0, 1000);
    P->mvp = m4mul(P->viewproj, P->model);
    P->mvp_inv = m4inverse(P->mvp);
    P->up = v3(0, 1, 0);
    P->right = v3(1, 0, 0);
    P->fwd = v3(0, 0, 1);
    P->view_up = v3(0, 1, 0);
    P->view_right = v3(1, 0, 0);
    P->rv0 = v4(P->trail_len, P->trail_max, P->trail_min, 0);
    Tex *t = P->r.texture;
    if (P->ss_frames > 0 && P->ss_cols > 0 && P->ss_rows > 0) {
        float fw = 1.0f / P->ss_cols, fh = 1.0f / P->ss_rows, ratio = 1;
        if (t && t->res[0] > 0) ratio = (t->res[1] * fh) / (t->res[0] * fw);
        P->rv1 = v4(fw, fh, P->ss_frames, ratio);
    } else {
        float ratio = 1;
        if (t && tex_real_w(t) > 0) ratio = (float)tex_real_h(t) / tex_real_w(t);
        P->rv1 = v4(0, 0, 0, ratio);
    }
}

static void geo_setup(Pass *p, void *u)
{
    Particle *P = u;
    (void)p;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &P->prev_vao);
    glBindVertexArray(P->vao);
}

static void geo_draw(Pass *p, void *u)
{
    Particle *P = u;
    (void)p;
    glDrawElements(GL_TRIANGLES, P->active_idx, GL_UNSIGNED_INT, NULL);
}

static void geo_cleanup(Pass *p, void *u)
{
    Particle *P = u;
    (void)p;
    glBindVertexArray(P->prev_vao);
}

static void draw_pass(Particle *P, uint32_t nverts, int floats)
{
    glBindBuffer(GL_ARRAY_BUFFER, P->vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)nverts * floats * sizeof(float), P->verts, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, P->ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)P->active_idx * sizeof(uint32_t), P->idx, GL_DYNAMIC_DRAW);
    update_matrices(P);
    if (P->refract && P->refract_fbo) {
        Tex *sf = P->r.scene->scene_fbo;
        glBindFramebuffer(GL_READ_FRAMEBUFFER, sf->fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, P->refract_fbo->fbo);
        glBlitFramebuffer(0, 0, sf->width, sf->height, 0, 0, sf->width, sf->height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }
    glEnable(GL_DEPTH_CLAMP);
    pass_render(P->pass);
    glDisable(GL_DEPTH_CLAMP);
}

static void render_sprites(Particle *P)
{
    uint32_t vi = 0, ii = 0;
    for (uint32_t i = 0; i < P->count; i++) {
        Pt *p = &P->pts[i];
        if (!p->alive || !isfinite(p->pos.x) || !isfinite(p->pos.y) || !isfinite(p->pos.z) || !isfinite(p->size) ||
            p->size <= 0 || p->size > 10000)
            continue;
        float lt = life_pos(p);
        if (P->ss_frames > 0 && p->frame >= 0) {
            if (!strcmp(P->anim_mode, "randomframe")) lt = (p->frame + 0.5f) / P->ss_frames;
            else lt = p->frame / P->ss_frames;
        }
        static const float corners[4][2] = { { 0, 1 }, { 1, 1 }, { 1, 0 }, { 0, 0 } };
        uint32_t base = vi;
        for (int c = 0; c < 4; c++) {
            float *v = &P->verts[vi * SPRITE_FLOATS];
            v[0] = p->pos.x; v[1] = p->pos.y; v[2] = p->pos.z;
            v[3] = corners[c][0]; v[4] = corners[c][1]; v[5] = p->rot.z; v[6] = p->size;
            v[7] = p->color.x; v[8] = p->color.y; v[9] = p->color.z; v[10] = p->alpha;
            v[11] = p->vel.x; v[12] = p->vel.y; v[13] = p->vel.z; v[14] = lt;
            v[15] = p->rot.x; v[16] = p->rot.y;
            vi++;
        }
        uint32_t q[6] = { base, base + 1, base + 2, base + 2, base + 3, base };
        memcpy(&P->idx[ii], q, sizeof q);
        ii += 6;
    }
    P->active_idx = ii;
    if (ii) draw_pass(P, vi, SPRITE_FLOATS);
}

static vec3 catmull(vec3 p0, vec3 p1, vec3 p2, vec3 p3, float t)
{
    float t2 = t * t, t3 = t2 * t;
    vec3 r;
    float *R = &r.x;
    const float *a = &p0.x, *b = &p1.x, *c = &p2.x, *d = &p3.x;
    for (int k = 0; k < 3; k++)
        R[k] = 0.5f * (2 * b[k] + (-a[k] + c[k]) * t + (2 * a[k] - 5 * b[k] + 4 * c[k] - d[k]) * t2 +
                       (-a[k] + 3 * b[k] - 3 * c[k] + d[k]) * t3);
    return r;
}

static void render_rope(Particle *P)
{
    if (P->count < 2) return;
    uint32_t alive = P->count, segs = alive - 1;
    int sub = P->rope_subdiv > 0 ? P->rope_subdiv : 1;
    uint32_t total = segs * sub + 1;
    vec3 *sp = malloc(sizeof(vec3) * total);
    float *ss = malloc(sizeof(float) * total);
    vec4 *sc = malloc(sizeof(vec4) * total);
    for (uint32_t i = 0; i < segs; i++) {
        Pt *p1 = &P->pts[i], *p2 = &P->pts[i + 1];
        Pt *p0 = i > 0 ? &P->pts[i - 1] : p1, *p3 = i + 2 < alive ? &P->pts[i + 2] : p2;
        for (int k = 0; k < sub; k++) {
            float t = (float)k / sub;
            uint32_t idx = i * sub + k;
            sp[idx] = catmull(p0->pos, p1->pos, p2->pos, p3->pos, t);
            ss[idx] = lerpf(p1->size, p2->size, t);
            sc[idx] = v4(lerpf(p1->color.x, p2->color.x, t), lerpf(p1->color.y, p2->color.y, t),
                         lerpf(p1->color.z, p2->color.z, t), lerpf(p1->alpha, p2->alpha, t));
        }
    }
    Pt *pl = &P->pts[alive - 1];
    sp[total - 1] = pl->pos;
    ss[total - 1] = pl->size;
    sc[total - 1] = v4(pl->color.x, pl->color.y, pl->color.z, pl->alpha);
    uint32_t tsub = total - 1;
    float uvs = P->rope_uv_scale > 0 ? P->rope_uv_scale : 1;
    float tlen = (float)tsub / uvs + 1, usable = tlen - 1;
    bool smooth = P->rope_uv_smooth && P->uniform_lifetimes && !P->rope_uv_scroll;
    float *cum = NULL, totlen = 0;
    if (smooth) {
        cum = calloc(total, sizeof(float));
        for (uint32_t i = 1; i < total; i++) {
            totlen += v3len(v3sub(sp[i], sp[i - 1]));
            cum[i] = totlen;
        }
    }
    float scroll = P->rope_uv_scroll && usable > 0 ? fmodf(g_Time, 10000.0f) * usable : 0;
    uint32_t vi = 0, ii = 0;
    for (uint32_t s = 0; s < tsub; s++) {
        vec3 a = sp[s], b = sp[s + 1], prev = s > 0 ? sp[s - 1] : a, after = s + 2 < total ? sp[s + 2] : b;
        float tp = smooth && totlen > 0 ? cum[s] / totlen * tsub : (float)s;
        tp += scroll;
        static const float uv[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
        uint32_t base = vi;
        for (int c = 0; c < 4; c++) {
            float *v = &P->verts[vi * ROPE_FLOATS];
            v[0] = a.x; v[1] = a.y; v[2] = a.z; v[3] = ss[s];
            v[4] = b.x; v[5] = b.y; v[6] = b.z; v[7] = tlen;
            v[8] = prev.x; v[9] = prev.y; v[10] = prev.z; v[11] = tp;
            v[12] = after.x; v[13] = after.y; v[14] = after.z; v[15] = ss[s + 1];
            v[16] = sc[s + 1].x; v[17] = sc[s + 1].y; v[18] = sc[s + 1].z; v[19] = sc[s + 1].w;
            v[20] = uv[c][0]; v[21] = uv[c][1];
            v[22] = sc[s].x; v[23] = sc[s].y; v[24] = sc[s].z; v[25] = sc[s].w;
            vi++;
        }
        uint32_t q[6] = { base, base + 1, base + 2, base + 2, base + 3, base };
        memcpy(&P->idx[ii], q, sizeof q);
        ii += 6;
    }
    free(sp);
    free(ss);
    free(sc);
    free(cum);
    P->active_idx = ii;
    if (ii) draw_pass(P, vi, ROPE_FLOATS);
}

static void particle_render(SceneObj *o)
{
    Particle *P = o->impl;
    if (!P->initialized || !o->visible->b || !P->pass) return;
    if (P->time == 0) {
        P->time = g_Time;
        return;
    }
    float dt = g_Time - (float)P->time;
    P->time = g_Time;
    if (dt > 0) update(P, fminf(dt, 0.1f));
    if (P->count) {
        if (P->rope) render_rope(P);
        else render_sprites(P);
    }
}

static void particle_destroy(SceneObj *o)
{
    Particle *P = o->impl;
    pass_free(P->pass);
    if (P->vao) glDeleteVertexArrays(1, &P->vao);
    if (P->vbo) glDeleteBuffers(1, &P->vbo);
    if (P->ebo) glDeleteBuffers(1, &P->ebo);
    fbop_free(&P->pfbos);
    fbop_free(&P->r.fbos);
    if (P->r.texture && P->r.texture->kind == TEXKIND_FILE) tex_free(P->r.texture);
    VFREE(P->emitters);
    VFREE(P->inits);
    VFREE(P->ops);
    free(P->pts);
    free(P->verts);
    free(P->idx);
    free(P);
}

static void attrib(GLuint prog, const char *name, int n, int stride, int off)
{
    GLint l = glGetAttribLocation(prog, name);
    if (l < 0) return;
    glEnableVertexAttribArray(l);
    glVertexAttribPointer(l, n, GL_FLOAT, GL_FALSE, stride * sizeof(float), (void *)(sizeof(float) * off));
}

bool particle_create(WeScene *s, SceneObj *o)
{
    WeProject *proj = s->project;
    Arena *a = &proj->arena;
    Properties *pr = &proj->props;
    const cJSON *j = o->json;
    cJSON *pj = NULL;
    const cJSON *pnode = js_get(j, "particle");
    if (pnode && cJSON_IsString(pnode)) {
        char *data = (char *)vfs_read(proj->vfs, pnode->valuestring, NULL);
        if (!data) {
            we_err("cannot load particle file %s", pnode->valuestring);
            return false;
        }
        pj = cJSON_Parse(data);
        free(data);
    } else if (pnode && cJSON_IsObject(pnode)) {
        pj = cJSON_Duplicate(pnode, 1);
    }
    if (!pj) return false;

    Particle *P = calloc(1, sizeof *P);
    o->impl = P;
    o->render = particle_render;
    o->destroy = particle_destroy;
    P->r.scene = s;
    P->r.obj = o;
    P->r.fbos.parent = &s->fbos;
    P->rng = 0x9E3779B97F4A7C15ull ^ (uint64_t)(uintptr_t)P ^ (uint64_t)(g_Time * 1000003);
    P->parallax_depth = us_vec2(a, j, "parallaxDepth", pr, v2(0, 0));
    sceneobj_register(o, "parallaxDepth", P->parallax_depth);

    const cJSON *io = js_get(j, "instanceoverride");
    if (io) {
        const cJSON *jj = io;
        P->io_alpha = us_float(a, jj, "alpha", pr, 1);
        P->io_size = us_float(a, jj, "size", pr, 1);
        P->io_lifetime = us_float(a, jj, "lifetime", pr, 1);
        P->io_rate = us_float(a, jj, "rate", pr, 1);
        P->io_speed = us_float(a, jj, "speed", pr, 1);
        P->io_count = us_float(a, jj, "count", pr, 1);
        P->io_color = us_vec3(a, jj, "color", pr, v3(1, 1, 1));
        P->io_colorn = us_vec3(a, jj, "colorn", pr, v3(1, 1, 1));
    } else {
        P->io_alpha = dv_new_float(a, 1);
        P->io_size = dv_new_float(a, 1);
        P->io_lifetime = dv_new_float(a, 1);
        P->io_rate = dv_new_float(a, 1);
        P->io_speed = dv_new_float(a, 1);
        P->io_count = dv_new_float(a, 1);
        P->io_color = dv_new_float(a, 1);
        P->io_colorn = dv_new_float(a, 1);
    }

    P->anim_mode = arena_strdup(a, js_str(pj, "animationmode", "sequence"));
    P->seq_mult = js_num(pj, "sequencemultiplier", 1);
    P->max_count = js_int(pj, "maxcount", 100);
    P->flags = js_int(pj, "flags", 0);

    const cJSON *it;
    cJSON_ArrayForEach(it, js_get(pj, "emitter")) parse_emitter(P, it);
    cJSON_ArrayForEach(it, js_get(pj, "initializer")) parse_init(P, proj, it);
    cJSON_ArrayForEach(it, js_get(pj, "operator")) parse_op(P, proj, it);

    const cJSON *rend = cJSON_GetArrayItem(js_get(pj, "renderer"), 0);
    const char *rname = rend ? js_str(rend, "name", "sprite") : "sprite";
    if (!strcmp(rname, "rope") || !strcmp(rname, "ropetrail")) {
        P->rope = true;
        P->rope_subdiv = (int)js_num(rend, "subdivision", !strcmp(rname, "rope") ? 4 : 1);
        if (P->rope_subdiv < 0) P->rope_subdiv = 0;
        P->rope_uv_scale = js_num(rend, "uvscale", 1);
        P->rope_uv_scroll = js_bool(rend, "uvscrolling", false);
        P->rope_uv_smooth = js_bool(rend, "uvsmoothing", true);
        if (!strcmp(rname, "ropetrail")) {
            P->trail = true;
            P->trail_len = js_num(rend, "length", 1);
            P->rope_segments = (int)js_num(rend, "segments", 4);
            if (P->rope_segments < 2) P->rope_segments = 2;
        }
    } else if (!strcmp(rname, "spritetrail")) {
        P->trail = true;
        P->trail_len = js_num(rend, "length", 0.05);
        P->trail_max = js_num(rend, "maxlength", 10);
        P->trail_min = js_num(rend, "minlength", 0);
    }
    if (!P->trail_max) P->trail_max = 10;

    const char *matfile = js_str(pj, "material", NULL);
    Material *mat = matfile ? material_load(proj, matfile) : NULL;
    if (!mat || !mat->passes.n) {
        we_err("particle %s has no usable material", o->name);
        cJSON_Delete(pj);
        return false;
    }
    P->r.material = mat;

    uint32_t mc = (uint32_t)(P->max_count * P->io_count->f);
    P->cap = mc > 0 ? mc : MAX_PARTICLES_DEFAULT;
    P->pts = calloc(P->cap, sizeof(Pt));
    if (P->rope) {
        int sub = P->rope_subdiv > 0 ? P->rope_subdiv : 1;
        int maxsub = (P->cap > 1 ? P->cap - 1 : 1) * sub;
        P->verts = calloc((size_t)maxsub * 4 * ROPE_FLOATS, sizeof(float));
        P->idx = calloc((size_t)maxsub * 6, sizeof(uint32_t));
    } else {
        P->verts = calloc((size_t)P->cap * 4 * SPRITE_FLOATS, sizeof(float));
        P->idx = calloc((size_t)P->cap * 6, sizeof(uint32_t));
    }

    /* texture */
    MaterialPass *fp = mat->passes.v[0];
    const char *tname = texmap_get(&fp->textures, 0);
    if (tname) {
        if (!strncmp(tname, "_rt_", 4)) P->r.texture = scene_find_fbo(s, tname);
        else {
            P->r.texture = scene_resolve_texture(s, tname);
            if (P->r.texture) P->r.texture->refs++;
        }
    }
    if (!P->r.texture) {
        we_err("particle %s: missing texture", o->name);
        cJSON_Delete(pj);
        return false;
    }
    P->ss_cols = P->r.texture->ss_cols;
    P->ss_rows = P->r.texture->ss_rows;
    P->ss_frames = P->r.texture->ss_frames;
    P->ss_duration = P->r.texture->ss_duration;
    P->overbright = 1;
    int oi = const_find(&fp->constants, "ui_editor_properties_overbright");
    if (oi >= 0) P->overbright = fp->constants.v[oi].value->f;
    P->refract_amount = 0.05f;
    P->r.brightness = &P->overbright;
    P->r.alpha = &P->io_alpha->f;
    P->white[0] = P->white[1] = P->white[2] = P->white[3] = 1;
    P->r.color = P->io_color->v;
    for (int i = 0; i < P->inits.n; i++) (void)0;

    /* setup (CParticle::setup) */
    P->last_w = s->width;
    P->last_h = s->height;
    P->t_origin = v3(o->origin->v[0] - s->width / 2, s->height / 2 - o->origin->v[1], o->origin->v[2]);
    for (int i = 0; i < P->emitters.n; i++) {
        Emitter *e = &P->emitters.v[i];
        e->rate_eff = e->rate * P->io_rate->f;
        e->t_origin = v3(e->origin.x, -e->origin.y, e->origin.z);
        e->t_dirs = v3(e->directions.x, -e->directions.y, e->directions.z);
        e->delay_timer = e->delay;
        e->remaining = e->kind == EM_SPHERE ? e->instantaneous : 0;
        if (e->control_point == -1) {
            const cJSON *cp0 = cJSON_GetArrayItem(js_get(pj, "controlpoint"), 0);
            if (cp0 && (js_int(cp0, "flags", 0) & 1)) e->control_point = 0;
        }
    }
    cJSON_ArrayForEach(it, js_get(pj, "controlpoint"))
    {
        int id = js_int(it, "id", -1);
        if (id < 0 || id >= 8) continue;
        int fl = js_int(it, "flags", 0);
        P->cps[id].offset = js_vec3(it, "offset", v3(0, 0, 0));
        P->cps[id].link_mouse = (fl & 1) != 0;
        P->cps[id].world = (fl & 2) != 0;
        if (!P->cps[id].link_mouse)
            P->cps[id].position = P->cps[id].world ? v3sub(P->cps[id].offset, P->t_origin) : P->cps[id].offset;
    }

    P->ovr = arena_alloc(a, sizeof *P->ovr);
    P->ovr->id = -1;
    combo_set(a, &P->ovr->combos, "THICKFORMAT", 1);
    if (P->rope) P->ovr->shader_override = "genericropeparticle";
    if (P->ss_frames > 0) combo_set(a, &P->ovr->combos, "SPRITESHEET", 1);
    if (P->trail) combo_set(a, &P->ovr->combos, "TRAILRENDERER", 1);
    texmap_set(a, &P->binds, 0, "previous");
    int ri = combo_find(&fp->combos, "REFRACT");
    P->refract = ri >= 0 && fp->combos.v[ri].value != 0;
    P->pfbos.parent = &P->r.fbos;
    if (P->refract)
        P->refract_fbo = fbop_create(&P->pfbos, "_rt_FullFrameBuffer", TEXFMT_ARGB8888, TEXFLAG_CLAMP, 1,
                                     s->scene_fbo->width, s->scene_fbo->height, s->scene_fbo->width,
                                     s->scene_fbo->height);
    P->pass = pass_new(&P->r, &P->pfbos, fp, P->ovr, &P->binds, NULL);
    cJSON_Delete(pj);
    if (!P->pass || !P->pass->ok) {
        we_err("particle %s: shader setup failed", o->name);
        return false;
    }
    Pass *ps = P->pass;
    ps->draw_to = s->scene_fbo;
    ps->input = P->r.texture;
    ps->mvp = &P->mvp;
    ps->mvp_inv = &P->mvp_inv;
    ps->model = &P->model;
    ps->viewproj = &P->viewproj;

    GLint prev;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prev);
    glGenVertexArrays(1, &P->vao);
    glGenBuffers(1, &P->vbo);
    glGenBuffers(1, &P->ebo);
    glBindVertexArray(P->vao);
    glBindBuffer(GL_ARRAY_BUFFER, P->vbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, P->ebo);
    GLuint prog = ps->program;
    if (P->rope) {
        attrib(prog, "a_PositionVec4", 4, ROPE_FLOATS, 0);
        attrib(prog, "a_TexCoordVec4", 4, ROPE_FLOATS, 4);
        attrib(prog, "a_TexCoordVec4C1", 4, ROPE_FLOATS, 8);
        attrib(prog, "a_TexCoordVec4C2", 4, ROPE_FLOATS, 12);
        attrib(prog, "a_TexCoordVec4C3", 4, ROPE_FLOATS, 16);
        attrib(prog, "a_TexCoordC4", 2, ROPE_FLOATS, 20);
        attrib(prog, "a_Color", 4, ROPE_FLOATS, 22);
    } else {
        attrib(prog, "a_Position", 3, SPRITE_FLOATS, 0);
        attrib(prog, "a_TexCoordVec4", 4, SPRITE_FLOATS, 3);
        attrib(prog, "a_Color", 4, SPRITE_FLOATS, 7);
        attrib(prog, "a_TexCoordVec4C1", 4, SPRITE_FLOATS, 11);
        attrib(prog, "a_TexCoordC2", 2, SPRITE_FLOATS, 15);
    }
    glBindVertexArray(prev);
    ps->geo_setup = geo_setup;
    ps->geo_draw = geo_draw;
    ps->geo_cleanup = geo_cleanup;
    ps->geo_user = P;
    pass_uniform_ptr(ps, "g_ModelMatrixInverse", U_MAT4, &P->model_inv, 1);
    pass_uniform_ptr(ps, "g_OrientationUp", U_VEC3, &P->up, 1);
    pass_uniform_ptr(ps, "g_OrientationRight", U_VEC3, &P->right, 1);
    pass_uniform_ptr(ps, "g_OrientationForward", U_VEC3, &P->fwd, 1);
    pass_uniform_ptr(ps, "g_ViewUp", U_VEC3, &P->view_up, 1);
    pass_uniform_ptr(ps, "g_ViewRight", U_VEC3, &P->view_right, 1);
    pass_uniform_ptr(ps, "g_EyePosition", U_VEC3, &P->eye, 1);
    pass_uniform_ptr(ps, "g_RenderVar0", U_VEC4, &P->rv0, 1);
    pass_uniform_ptr(ps, "g_RenderVar1", U_VEC4, &P->rv1, 1);
    if (P->refract) pass_uniform_ptr(ps, "g_RefractAmount", U_FLOAT, &P->refract_amount, 1);
    P->initialized = true;
    return true;
}
