/* Small column-major matrix/vector helpers (same conventions as glm). */
#ifndef WE_MATH_H
#define WE_MATH_H

#include <math.h>
#include <string.h>

typedef struct { float x, y; } vec2;
typedef struct { float x, y, z; } vec3;
typedef struct { float x, y, z, w; } vec4;
typedef struct { float m[16]; } mat4;   /* m[col * 4 + row] */
typedef struct { float m[9]; } mat3;

#define WE_PI 3.14159265358979323846f

static inline vec2 v2(float x, float y) { return (vec2){ x, y }; }
static inline vec3 v3(float x, float y, float z) { return (vec3){ x, y, z }; }
static inline vec4 v4(float x, float y, float z, float w) { return (vec4){ x, y, z, w }; }
static inline vec3 v3add(vec3 a, vec3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline vec3 v3sub(vec3 a, vec3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline vec3 v3mul(vec3 a, vec3 b) { return v3(a.x * b.x, a.y * b.y, a.z * b.z); }
static inline vec3 v3s(vec3 a, float s) { return v3(a.x * s, a.y * s, a.z * s); }
static inline float v3dot(vec3 a, vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline float v3len(vec3 a) { return sqrtf(v3dot(a, a)); }
static inline vec3 v3cross(vec3 a, vec3 b)
{
    return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
static inline vec3 v3norm(vec3 a)
{
    float l = v3len(a);
    return l > 0 ? v3s(a, 1.0f / l) : a;
}
static inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

static inline mat4 m4id(void)
{
    mat4 r = { { 0 } };
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1;
    return r;
}

static inline mat4 m4mul(mat4 a, mat4 b)
{
    mat4 r;
    for (int c = 0; c < 4; c++)
        for (int rr = 0; rr < 4; rr++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += a.m[k * 4 + rr] * b.m[c * 4 + k];
            r.m[c * 4 + rr] = s;
        }
    return r;
}

static inline vec4 m4v4(mat4 a, vec4 v)
{
    return v4(a.m[0] * v.x + a.m[4] * v.y + a.m[8] * v.z + a.m[12] * v.w,
              a.m[1] * v.x + a.m[5] * v.y + a.m[9] * v.z + a.m[13] * v.w,
              a.m[2] * v.x + a.m[6] * v.y + a.m[10] * v.z + a.m[14] * v.w,
              a.m[3] * v.x + a.m[7] * v.y + a.m[11] * v.z + a.m[15] * v.w);
}

static inline mat4 m4ortho(float l, float r, float b, float t, float n, float f)
{
    mat4 m = m4id();
    m.m[0] = 2 / (r - l);
    m.m[5] = 2 / (t - b);
    m.m[10] = -2 / (f - n);
    m.m[12] = -(r + l) / (r - l);
    m.m[13] = -(t + b) / (t - b);
    m.m[14] = -(f + n) / (f - n);
    return m;
}

static inline mat4 m4ortho2d(float l, float r, float b, float t)
{
    mat4 m = m4id();
    m.m[0] = 2 / (r - l);
    m.m[5] = 2 / (t - b);
    m.m[10] = -1;
    m.m[12] = -(r + l) / (r - l);
    m.m[13] = -(t + b) / (t - b);
    return m;
}

static inline mat4 m4perspective(float fovy, float aspect, float n, float f)
{
    float t = tanf(fovy / 2);
    mat4 m = { { 0 } };
    m.m[0] = 1 / (aspect * t);
    m.m[5] = 1 / t;
    m.m[10] = -(f + n) / (f - n);
    m.m[11] = -1;
    m.m[14] = -(2 * f * n) / (f - n);
    return m;
}

static inline mat4 m4translate(mat4 m, vec3 v)
{
    for (int r = 0; r < 4; r++)
        m.m[12 + r] = m.m[r] * v.x + m.m[4 + r] * v.y + m.m[8 + r] * v.z + m.m[12 + r];
    return m;
}

static inline mat4 m4scale(mat4 m, vec3 v)
{
    for (int r = 0; r < 4; r++) {
        m.m[r] *= v.x;
        m.m[4 + r] *= v.y;
        m.m[8 + r] *= v.z;
    }
    return m;
}

static inline mat4 m4rotation(float angle, vec3 axis)
{
    vec3 a = v3norm(axis);
    float c = cosf(angle), s = sinf(angle), t = 1 - c;
    mat4 r = m4id();
    r.m[0] = c + t * a.x * a.x;
    r.m[1] = t * a.x * a.y + s * a.z;
    r.m[2] = t * a.x * a.z - s * a.y;
    r.m[4] = t * a.x * a.y - s * a.z;
    r.m[5] = c + t * a.y * a.y;
    r.m[6] = t * a.y * a.z + s * a.x;
    r.m[8] = t * a.x * a.z + s * a.y;
    r.m[9] = t * a.y * a.z - s * a.x;
    r.m[10] = c + t * a.z * a.z;
    return r;
}

static inline mat4 m4rotate(mat4 m, float angle, vec3 axis) { return m4mul(m, m4rotation(angle, axis)); }

static inline vec3 m3v3_rot(mat4 r, vec3 v)
{
    return v3(r.m[0] * v.x + r.m[4] * v.y + r.m[8] * v.z, r.m[1] * v.x + r.m[5] * v.y + r.m[9] * v.z,
              r.m[2] * v.x + r.m[6] * v.y + r.m[10] * v.z);
}

static inline mat4 m4lookat(vec3 eye, vec3 center, vec3 up)
{
    vec3 f = v3norm(v3sub(center, eye));
    vec3 s = v3norm(v3cross(f, up));
    vec3 u = v3cross(s, f);
    mat4 m = m4id();
    m.m[0] = s.x; m.m[4] = s.y; m.m[8] = s.z;
    m.m[1] = u.x; m.m[5] = u.y; m.m[9] = u.z;
    m.m[2] = -f.x; m.m[6] = -f.y; m.m[10] = -f.z;
    m.m[12] = -v3dot(s, eye);
    m.m[13] = -v3dot(u, eye);
    m.m[14] = v3dot(f, eye);
    return m;
}

static inline mat4 m4inverse(mat4 a)
{
    const float *m = a.m;
    mat4 r;
    float *inv = r.m;
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] +
             m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] -
             m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] +
             m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] -
              m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] -
             m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] +
             m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] -
             m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] +
              m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] +
             m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] -
             m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] +
              m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] -
              m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] -
             m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] +
             m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] -
              m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] +
              m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (det == 0) return m4id();
    det = 1.0f / det;
    for (int i = 0; i < 16; i++) inv[i] *= det;
    return r;
}

static inline mat3 m3id(void)
{
    mat3 r = { { 1, 0, 0, 0, 1, 0, 0, 0, 1 } };
    return r;
}

#endif
