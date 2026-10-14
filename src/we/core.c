/* Core helpers for the Wallpaper Engine layer: arena, logging, JSON helpers,
 * virtual file system (directories, scene.pkg containers, virtual files). */
#include "internal.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

WeSettings we_settings = { .volume = 15, .automute = true, .debug_object = -1 };

/* ------------------------------------------------------------------ arena */
struct ArenaBlock {
    ArenaBlock *next;
    size_t      used, size;
    max_align_t data[];
};

void *arena_alloc(Arena *a, size_t n)
{
    n = (n + 15) & ~(size_t)15;
    ArenaBlock *b = a->head;
    if (!b || b->used + n > b->size) {
        size_t size = n > 65536 ? n : 65536;
        b = calloc(1, sizeof *b + size);
        b->size = size;
        b->next = a->head;
        a->head = b;
    }
    void *p = (char *)b->data + b->used;
    b->used += n;
    memset(p, 0, n);
    return p;
}

char *arena_strdup(Arena *a, const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *d = arena_alloc(a, n);
    memcpy(d, s, n);
    return d;
}

void *vec_grow(Arena *a, void *v, int n, int *cap, size_t elem)
{
    int nc = *cap ? *cap * 2 : 8;
    void *nv;
    if (a) {
        nv = arena_alloc(a, elem * nc);
        if (n) memcpy(nv, v, elem * n);
    } else {
        nv = realloc(v, elem * nc);
    }
    *cap = nc;
    return nv;
}

void arena_track(Arena *a, DynVal *d)
{
    if (a->ndv == a->capdv) {
        a->capdv = a->capdv ? a->capdv * 2 : 64;
        a->dvs = realloc(a->dvs, sizeof(DynVal *) * a->capdv);
    }
    a->dvs[a->ndv++] = d;
}

void arena_free(Arena *a)
{
    for (int i = 0; i < a->ndv; i++) dv_free(a->dvs[i]);
    free(a->dvs);
    a->dvs = NULL;
    a->ndv = a->capdv = 0;
    while (a->head) {
        ArenaBlock *n = a->head->next;
        free(a->head);
        a->head = n;
    }
}

/* ------------------------------------------------------------------ logging */
void we_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("wallengine: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

void we_err(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("wallengine: error: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

void we_dbg(const char *fmt, ...)
{
    if (!cfg.verbose) return;
    va_list ap;
    va_start(ap, fmt);
    fputs("wallengine: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

/* ------------------------------------------------------------------ json */
cJSON *js_get(const cJSON *o, const char *key)
{
    if (!o || !cJSON_IsObject(o)) return NULL;
    cJSON *c = cJSON_GetObjectItemCaseSensitive(o, key);
    return c && !cJSON_IsNull(c) ? c : NULL;
}

const char *js_str(const cJSON *o, const char *key, const char *def)
{
    cJSON *c = js_get(o, key);
    return c && cJSON_IsString(c) ? c->valuestring : def;
}

double js_num(const cJSON *o, const char *key, double def)
{
    cJSON *c = js_get(o, key);
    if (!c) return def;
    if (cJSON_IsNumber(c)) return c->valuedouble;
    if (cJSON_IsBool(c)) return cJSON_IsTrue(c);
    if (cJSON_IsString(c)) {
        char *end;
        double d = strtod(c->valuestring, &end);
        return end != c->valuestring ? d : def;
    }
    if (cJSON_IsObject(c)) return js_num(c, "value", def);
    return def;
}

int js_int(const cJSON *o, const char *key, int def) { return (int)js_num(o, key, def); }

bool js_bool(const cJSON *o, const char *key, bool def)
{
    cJSON *c = js_get(o, key);
    if (!c) return def;
    if (cJSON_IsBool(c)) return cJSON_IsTrue(c);
    if (cJSON_IsNumber(c)) return c->valuedouble != 0;
    if (cJSON_IsString(c)) return !strcmp(c->valuestring, "true") || !strcmp(c->valuestring, "1");
    if (cJSON_IsObject(c)) return js_bool(c, "value", def);
    return def;
}

int vec_count(const char *s)
{
    /* same rule as VectorBuilder::preparseSize: count space separators */
    const char *a = strchr(s, ' ');
    if (!a) return 1;
    const char *b = strchr(a + 1, ' ');
    if (!b) return 2;
    const char *c = strchr(b + 1, ' ');
    if (!c) return 3;
    return 4;
}

int vec_parse(const char *s, float out[4])
{
    int n = 0;
    const char *p = s;
    while (*p && n < 4) {
        while (*p == ' ' || *p == ',' || *p == '\t') p++;
        if (!*p) break;
        char *end;
        float v = strtof(p, &end);
        if (end == p) break;
        out[n++] = v;
        p = end;
    }
    for (int i = n; i < 4; i++) out[i] = 0;
    return n;
}

vec3 js_vec3(const cJSON *o, const char *key, vec3 def)
{
    cJSON *c = js_get(o, key);
    if (!c) return def;
    if (cJSON_IsObject(c)) c = js_get(c, "value");
    if (!c) return def;
    if (cJSON_IsNumber(c)) return v3(c->valuedouble, c->valuedouble, c->valuedouble);
    if (cJSON_IsString(c)) {
        float v[4];
        int n = vec_parse(c->valuestring, v);
        if (n == 1) return v3(v[0], v[0], v[0]);
        return v3(v[0], v[1], v[2]);
    }
    if (cJSON_IsArray(c) && cJSON_GetArraySize(c) >= 3)
        return v3(cJSON_GetArrayItem(c, 0)->valuedouble, cJSON_GetArrayItem(c, 1)->valuedouble,
                  cJSON_GetArrayItem(c, 2)->valuedouble);
    return def;
}

vec2 js_vec2(const cJSON *o, const char *key, vec2 def)
{
    cJSON *c = js_get(o, key);
    if (!c) return def;
    if (cJSON_IsObject(c)) c = js_get(c, "value");
    if (!c) return def;
    if (cJSON_IsNumber(c)) return v2(c->valuedouble, c->valuedouble);
    if (cJSON_IsString(c)) {
        float v[4];
        vec_parse(c->valuestring, v);
        return v2(v[0], v[1]);
    }
    if (cJSON_IsArray(c) && cJSON_GetArraySize(c) >= 2)
        return v2(cJSON_GetArrayItem(c, 0)->valuedouble, cJSON_GetArrayItem(c, 1)->valuedouble);
    return def;
}

void color_parse(const char *s, float alpha, float out[4])
{
    out[0] = out[1] = out[2] = 0;
    out[3] = alpha;
    while (isspace((unsigned char)*s)) s++;
    if (*s == '#') {
        char hex[9] = { 0 };
        size_t n = strlen(s + 1);
        if (n == 3) {
            snprintf(hex, sizeof hex, "%c%c%c%c%c%c%02x", s[1], s[1], s[2], s[2], s[3], s[3], (int)(alpha * 255));
        } else if (n == 4) {
            snprintf(hex, sizeof hex, "%c%c%c%c%c%c%c%c", s[1], s[1], s[2], s[2], s[3], s[3], s[4], s[4]);
        } else if (n == 6) {
            snprintf(hex, sizeof hex, "%.6s%02x", s + 1, (int)(alpha * 255));
        } else if (n == 8) {
            snprintf(hex, sizeof hex, "%.8s", s + 1);
        } else {
            return;
        }
        unsigned long c = strtoul(hex, NULL, 16);
        out[0] = ((c >> 24) & 0xff) / 255.0f;
        out[1] = ((c >> 16) & 0xff) / 255.0f;
        out[2] = ((c >> 8) & 0xff) / 255.0f;
        out[3] = (c & 0xff) / 255.0f;
        return;
    }
    char copy[256];
    snprintf(copy, sizeof copy, "%s", s);
    for (char *c = copy; *c; c++)
        if (*c == ',') *c = ' ';
    float v[4];
    int n = vec_parse(copy, v);
    if (n < 3) {
        if (n == 1) out[0] = out[1] = out[2] = v[0];
        return;
    }
    bool big = v[0] > 1 || v[1] > 1 || v[2] > 1 || (n == 4 && v[3] > 1);
    if (!strchr(copy, '.') && big) {
        /* integer notation 0..255 ("1 1 1" is treated as white, not 1/255) */
        out[0] = v[0] / 255.0f;
        out[1] = v[1] / 255.0f;
        out[2] = v[2] / 255.0f;
        out[3] = n == 4 ? v[3] / 255.0f : alpha;
    } else {
        out[0] = v[0];
        out[1] = v[1];
        out[2] = v[2];
        out[3] = n == 4 ? v[3] : alpha;
    }
}

/* ------------------------------------------------------------------ vfs */
typedef struct { char *name; uint32_t offset, length; } PkgEntry;
typedef struct { char *name; unsigned char *data; size_t len; } VFile;

typedef struct {
    int       kind;          /* 0 virtual, 1 directory, 2 package */
    char      path[PATH_MAX];
    PkgEntry *entries;
    int       nentries;
    size_t    base;
    VFile    *files;
    int       nfiles;
} Mount;

struct Vfs { Mount *m; int n; };

static void normalize(const char *in, char *out, size_t n)
{
    /* lexically normalize, drop leading slashes and "./", resolve ".." */
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s", in);
    for (char *c = tmp; *c; c++)
        if (*c == '\\') *c = '/';
    char *parts[256];
    int np = 0;
    char *save;
    for (char *t = strtok_r(tmp, "/", &save); t; t = strtok_r(NULL, "/", &save)) {
        if (!strcmp(t, ".")) continue;
        if (!strcmp(t, "..")) {
            if (np) np--;
            continue;
        }
        if (np < 256) parts[np++] = t;
    }
    out[0] = 0;
    size_t len = 0;
    for (int i = 0; i < np; i++) {
        len += snprintf(out + len, n > len ? n - len : 0, "%s%s", i ? "/" : "", parts[i]);
    }
}

Vfs *vfs_new(void)
{
    Vfs *v = calloc(1, sizeof *v);
    v->m = calloc(1, sizeof(Mount));
    v->n = 1;   /* mount 0: virtual files, always searched first */
    return v;
}

void vfs_free(Vfs *v)
{
    if (!v) return;
    for (int i = 0; i < v->n; i++) {
        Mount *m = &v->m[i];
        for (int j = 0; j < m->nentries; j++) free(m->entries[j].name);
        free(m->entries);
        for (int j = 0; j < m->nfiles; j++) {
            free(m->files[j].name);
            free(m->files[j].data);
        }
        free(m->files);
    }
    free(v->m);
    free(v);
}

static uint32_t rd32(FILE *f)
{
    unsigned char b[4] = { 0 };
    if (fread(b, 1, 4, f) != 4) return 0;
    return b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24;
}

static char *rdsized(FILE *f)
{
    uint32_t n = rd32(f);
    if (n > 65536) return NULL;
    char *s = malloc(n + 1);
    if (fread(s, 1, n, f) != n) {
        free(s);
        return NULL;
    }
    s[n] = 0;
    return s;
}

bool vfs_mount(Vfs *v, const char *path)
{
    struct stat st;
    if (stat(path, &st) < 0) return false;
    Mount m = { 0 };
    if (!realpath(path, m.path)) snprintf(m.path, sizeof m.path, "%s", path);
    if (S_ISDIR(st.st_mode)) {
        m.kind = 1;
    } else {
        FILE *f = fopen(path, "rb");
        if (!f) return false;
        char *hdr = rdsized(f);
        if (!hdr || strncmp(hdr, "PKGV", 4)) {
            we_err("%s is not a Wallpaper Engine package", path);
            free(hdr);
            fclose(f);
            return false;
        }
        free(hdr);
        uint32_t count = rd32(f);
        if (count > 1000000) {
            fclose(f);
            return false;
        }
        m.kind = 2;
        m.entries = calloc(count ? count : 1, sizeof(PkgEntry));
        for (uint32_t i = 0; i < count; i++) {
            m.entries[i].name = rdsized(f);
            m.entries[i].offset = rd32(f);
            m.entries[i].length = rd32(f);
            if (!m.entries[i].name) break;
            m.nentries++;
        }
        m.base = ftell(f);
        fclose(f);
        we_dbg("mounted %s (%d files)", path, m.nentries);
    }
    v->m = realloc(v->m, sizeof(Mount) * (v->n + 1));
    v->m[v->n++] = m;
    return true;
}

void vfs_add(Vfs *v, const char *name, const void *data, size_t len)
{
    Mount *m = &v->m[0];
    char norm[PATH_MAX];
    normalize(name, norm, sizeof norm);
    for (int i = 0; i < m->nfiles; i++)
        if (!strcmp(m->files[i].name, norm)) {
            free(m->files[i].data);
            m->files[i].data = malloc(len + 1);
            memcpy(m->files[i].data, data, len);
            m->files[i].data[len] = 0;
            m->files[i].len = len;
            return;
        }
    m->files = realloc(m->files, sizeof(VFile) * (m->nfiles + 1));
    VFile *f = &m->files[m->nfiles++];
    f->name = strdup(norm);
    f->data = malloc(len + 1);
    memcpy(f->data, data, len);
    f->data[len] = 0;
    f->len = len;
}

static unsigned char *read_whole(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    struct stat st;
    if (fstat(fileno(f), &st) < 0 || !S_ISREG(st.st_mode)) {
        fclose(f);
        return NULL;
    }
    unsigned char *buf = malloc(st.st_size + 1);
    size_t n = fread(buf, 1, st.st_size, f);
    fclose(f);
    buf[n] = 0;
    if (len) *len = n;
    return buf;
}

static int lookup(Vfs *v, const char *norm, int *entry)
{
    for (int i = 0; i < v->n; i++) {
        Mount *m = &v->m[i];
        if (m->kind == 0) {
            for (int j = 0; j < m->nfiles; j++)
                if (!strcmp(m->files[j].name, norm)) {
                    *entry = j;
                    return i;
                }
        } else if (m->kind == 1) {
            char full[PATH_MAX * 2];
            snprintf(full, sizeof full, "%s/%s", m->path, norm);
            struct stat st;
            if (!stat(full, &st) && S_ISREG(st.st_mode)) {
                *entry = -1;
                return i;
            }
        } else {
            for (int j = 0; j < m->nentries; j++)
                if (!strcmp(m->entries[j].name, norm)) {
                    *entry = j;
                    return i;
                }
        }
    }
    return -1;
}

bool vfs_exists(Vfs *v, const char *name)
{
    char norm[PATH_MAX];
    normalize(name, norm, sizeof norm);
    int e;
    return lookup(v, norm, &e) >= 0;
}

unsigned char *vfs_read(Vfs *v, const char *name, size_t *len)
{
    char norm[PATH_MAX];
    normalize(name, norm, sizeof norm);
    int e;
    int mi = lookup(v, norm, &e);
    if (mi < 0) return NULL;
    Mount *m = &v->m[mi];
    if (m->kind == 0) {
        VFile *f = &m->files[e];
        unsigned char *d = malloc(f->len + 1);
        memcpy(d, f->data, f->len + 1);
        if (len) *len = f->len;
        return d;
    }
    if (m->kind == 1) {
        char full[PATH_MAX * 2];
        snprintf(full, sizeof full, "%s/%s", m->path, norm);
        return read_whole(full, len);
    }
    FILE *f = fopen(m->path, "rb");
    if (!f) return NULL;
    PkgEntry *pe = &m->entries[e];
    unsigned char *d = malloc((size_t)pe->length + 1);
    fseek(f, m->base + pe->offset, SEEK_SET);
    size_t n = fread(d, 1, pe->length, f);
    fclose(f);
    d[n] = 0;
    if (len) *len = n;
    return d;
}

bool vfs_physical(Vfs *v, const char *name, char *out, size_t n)
{
    char norm[PATH_MAX];
    normalize(name, norm, sizeof norm);
    int e;
    int mi = lookup(v, norm, &e);
    if (mi < 0 || v->m[mi].kind != 1) return false;
    snprintf(out, n, "%s/%s", v->m[mi].path, norm);
    return true;
}

static void replace_ext(const char *name, const char *ext, char *out, size_t n)
{
    const char *slash = strrchr(name, '/');
    const char *dot = strrchr(name, '.');
    if (dot && (!slash || dot > slash)) snprintf(out, n, "%.*s.%s", (int)(dot - name), name, ext);
    else snprintf(out, n, "%s.%s", name, ext);
}

char *vfs_read_shader(Vfs *v, const char *name, const char *ext)
{
    char file[PATH_MAX], path[PATH_MAX + 32];
    replace_ext(name, ext, file, sizeof file);
    /* workshop/<id>/<file> shaders may have a compatibility replacement */
    if (!strncmp(file, "workshop/", 9)) {
        const char *id = file + 9;
        const char *slash = strchr(id, '/');
        if (slash) {
            snprintf(path, sizeof path, "zcompat/scene/shaders/%.*s/%s", (int)(slash - id), id, slash + 1);
            unsigned char *d = vfs_read(v, path, NULL);
            if (d) {
                we_dbg("replaced shader %s with compat %s", file, path);
                return (char *)d;
            }
        }
    }
    snprintf(path, sizeof path, "shaders/%s", file);
    return (char *)vfs_read(v, path, NULL);
}

/* ------------------------------------------------------------------ dynamic values */
static void dv_reset(DynVal *d)
{
    free(d->s);
    d->s = NULL;
}

void dv_set_float(DynVal *d, float f)
{
    dv_reset(d);
    d->v[0] = d->v[1] = d->v[2] = d->v[3] = f;
    d->f = f;
    d->i = (int)f;
    d->b = (int)f != 0;
    d->type = DV_FLOAT;
}

void dv_set_int(DynVal *d, int i)
{
    dv_reset(d);
    d->v[0] = d->v[1] = d->v[2] = d->v[3] = (float)i;
    d->f = (float)i;
    d->i = i;
    d->b = i != 0;
    d->type = DV_INT;
}

void dv_set_bool(DynVal *d, bool b)
{
    dv_reset(d);
    d->v[0] = d->v[1] = d->v[2] = d->v[3] = b;
    d->f = b;
    d->i = b;
    d->b = b;
    d->type = DV_BOOL;
}

void dv_set_vec(DynVal *d, const float *v, int n)
{
    dv_reset(d);
    d->v[0] = v[0];
    d->v[1] = n > 1 ? v[1] : 0;
    d->v[2] = n > 2 ? v[2] : 0;
    d->v[3] = n > 3 ? v[3] : 0;
    d->f = v[0];
    d->i = (int)v[0];
    d->b = v[0] != 0;
    d->type = n <= 2 ? DV_VEC2 : n == 3 ? DV_VEC3 : DV_VEC4;
}

void dv_set_color(DynVal *d, const float c[4])
{
    dv_reset(d);
    memcpy(d->v, c, sizeof d->v);
    d->f = c[0];
    d->i = (int)(255 * c[0]);
    d->b = c[3] != 0;
    d->type = DV_VEC4;
}

void dv_set_null(DynVal *d)
{
    dv_reset(d);
    memset(d->v, 0, sizeof d->v);
    d->f = 0;
    d->i = 0;
    d->b = false;
    d->type = DV_NULL;
}

static void apply_condition(DynVal *d, const char *s)
{
    bool b = !strcmp(d->cond, s);
    d->v[0] = d->v[1] = d->v[2] = d->v[3] = b;
    d->f = b;
    d->i = b;
    d->b = b;
    d->type = DV_BOOL;
}

void dv_set_string(DynVal *d, const char *s)
{
    char *copy = strdup(s ? s : "");
    dv_reset(d);
    memset(d->v, 0, sizeof d->v);
    d->f = 0;
    d->i = 0;
    d->b = false;
    d->s = copy;
    d->type = DV_STRING;
    if (d->cond) apply_condition(d, copy);
}

void dv_copy(DynVal *dst, const DynVal *src)
{
    if (src->type == DV_NULL) {
        dv_set_null(dst);
        return;
    }
    char *s = src->s ? strdup(src->s) : NULL;
    dv_reset(dst);
    memcpy(dst->v, src->v, sizeof dst->v);
    dst->f = src->f;
    dst->i = src->i;
    dst->b = src->b;
    dst->s = s;
    dst->type = src->type;
    if (dst->cond && src->type == DV_STRING) apply_condition(dst, s ? s : "");
}

void dv_connect(DynVal *d, Property *p)
{
    d->prop = p;
    VPUSH(p->listeners, d);
    dv_copy(d, &p->value);
}

void dv_free(DynVal *d)
{
    free(d->s);
    d->s = NULL;
}

char *dv_to_string(const DynVal *d, char *buf, size_t n)
{
    switch (d->type) {
    case DV_FLOAT: snprintf(buf, n, "%g", d->f); break;
    case DV_INT: snprintf(buf, n, "%d", d->i); break;
    case DV_BOOL: snprintf(buf, n, "%d", d->b); break;
    case DV_VEC2: snprintf(buf, n, "%g, %g", d->v[0], d->v[1]); break;
    case DV_VEC3: snprintf(buf, n, "%g, %g, %g", d->v[0], d->v[1], d->v[2]); break;
    case DV_VEC4: snprintf(buf, n, "%g, %g, %g, %g", d->v[0], d->v[1], d->v[2], d->v[3]); break;
    case DV_STRING: snprintf(buf, n, "%s", d->s ? d->s : ""); break;
    default: snprintf(buf, n, "null");
    }
    return buf;
}

static DynVal *dv_alloc(Arena *a)
{
    DynVal *d = arena_alloc(a, sizeof *d);
    arena_track(a, d);
    return d;
}

DynVal *dv_new_float(Arena *a, float f)
{
    DynVal *d = dv_alloc(a);
    dv_set_float(d, f);
    return d;
}
DynVal *dv_new_int(Arena *a, int i)
{
    DynVal *d = dv_alloc(a);
    dv_set_int(d, i);
    return d;
}
DynVal *dv_new_bool(Arena *a, bool b)
{
    DynVal *d = dv_alloc(a);
    dv_set_bool(d, b);
    return d;
}
DynVal *dv_new_vec(Arena *a, const float *v, int n)
{
    DynVal *d = dv_alloc(a);
    dv_set_vec(d, v, n);
    return d;
}
DynVal *dv_new_color(Arena *a, const float c[4])
{
    DynVal *d = dv_alloc(a);
    dv_set_color(d, c);
    return d;
}

static void dv_parse_value(DynVal *d, const cJSON *v, bool expect_color)
{
    if (!v || cJSON_IsNull(v)) {
        dv_set_null(d);
    } else if (cJSON_IsString(v)) {
        if (expect_color) {
            float c[4];
            color_parse(v->valuestring, 1.0f, c);
            dv_set_color(d, c);
        } else {
            int n = vec_count(v->valuestring);
            if (n == 1) {
                char *end;
                float f = strtof(v->valuestring, &end);
                if (end != v->valuestring && *end == 0) dv_set_float(d, f);
                else dv_set_string(d, v->valuestring);
            } else {
                float vv[4];
                vec_parse(v->valuestring, vv);
                dv_set_vec(d, vv, n);
            }
        }
    } else if (cJSON_IsBool(v)) {
        dv_set_bool(d, cJSON_IsTrue(v));
    } else if (cJSON_IsNumber(v)) {
        double x = v->valuedouble;
        /* nlohmann distinguishes 1 from 1.0; approximate with the source text */
        if (x == (double)(int)x && v->valueint == (int)x) dv_set_int(d, (int)x);
        else dv_set_float(d, (float)x);
    } else {
        dv_set_null(d);
    }
}

DynVal *us_parse(Arena *a, const cJSON *node, Properties *props, bool expect_color)
{
    DynVal *d = dv_alloc(a);
    const cJSON *value = node;
    const cJSON *user = NULL;
    if (cJSON_IsObject(node)) {
        user = js_get(node, "user");
        value = cJSON_GetObjectItemCaseSensitive(node, "value");
        const char *script = js_str(node, "script", NULL);
        if (script) {
            d->script = arena_strdup(a, script);
            const cJSON *sp = js_get(node, "scriptproperties");
            const cJSON *it;
            cJSON_ArrayForEach(it, sp)
            {
                ScriptProp s = { .name = arena_strdup(a, it->string), .value = us_parse(a, it, props, false) };
                AVPUSH(a, d->script_props, s);
            }
        }
    }
    dv_parse_value(d, value, expect_color);
    if (user && props) {
        const char *source = NULL;
        if (cJSON_IsString(user)) {
            source = user->valuestring;
        } else if (cJSON_IsObject(user)) {
            source = js_str(user, "name", NULL);
            const cJSON *cond = js_get(user, "condition");
            if (cond) {
                char buf[64];
                if (cJSON_IsString(cond)) d->cond = arena_strdup(a, cond->valuestring);
                else if (cJSON_IsNumber(cond)) {
                    snprintf(buf, sizeof buf, "%d", (int)cond->valuedouble);
                    d->cond = arena_strdup(a, buf);
                }
            }
        }
        Property *p = source ? props_find(props, source) : NULL;
        if (p) dv_connect(d, p);
    }
    return d;
}

DynVal *us_float(Arena *a, const cJSON *o, const char *key, Properties *p, float def)
{
    const cJSON *n = js_get(o, key);
    return n ? us_parse(a, n, p, false) : dv_new_float(a, def);
}

DynVal *us_int(Arena *a, const cJSON *o, const char *key, Properties *p, int def)
{
    const cJSON *n = js_get(o, key);
    return n ? us_parse(a, n, p, false) : dv_new_int(a, def);
}

DynVal *us_bool(Arena *a, const cJSON *o, const char *key, Properties *p, bool def)
{
    const cJSON *n = js_get(o, key);
    return n ? us_parse(a, n, p, false) : dv_new_bool(a, def);
}

DynVal *us_vec2(Arena *a, const cJSON *o, const char *key, Properties *p, vec2 def)
{
    const cJSON *n = js_get(o, key);
    float v[2] = { def.x, def.y };
    return n ? us_parse(a, n, p, false) : dv_new_vec(a, v, 2);
}

DynVal *us_vec3(Arena *a, const cJSON *o, const char *key, Properties *p, vec3 def)
{
    const cJSON *n = js_get(o, key);
    float v[3] = { def.x, def.y, def.z };
    return n ? us_parse(a, n, p, false) : dv_new_vec(a, v, 3);
}

DynVal *us_color(Arena *a, const cJSON *o, const char *key, Properties *p, vec4 def)
{
    const cJSON *n = js_get(o, key);
    float v[4] = { def.x, def.y, def.z, def.w };
    return n ? us_parse(a, n, p, true) : dv_new_color(a, v);
}

/* ------------------------------------------------------------------ properties */
Property *props_find(Properties *props, const char *name)
{
    for (int i = 0; i < props->list.n; i++)
        if (!strcmp(props->list.v[i]->name, name)) return props->list.v[i];
    return NULL;
}

static void prop_notify(Property *p)
{
    for (int i = 0; i < p->listeners.n; i++) {
        DynVal *d = p->listeners.v[i];
        if (p->value.type == DV_NULL) dv_set_null(d);
        else dv_copy(d, &p->value);
    }
}

Property *prop_parse(const cJSON *node, const char *name)
{
    const char *type = js_str(node, "type", NULL);
    if (!type || !strcmp(type, "group")) return NULL;
    Property *p = calloc(1, sizeof *p);
    p->name = strdup(name);
    p->text = strdup(js_str(node, "text", ""));
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(node, "value");
    if (!strcmp(type, "color")) {
        p->type = PROP_COLOR;
        float c[4];
        color_parse(value && cJSON_IsString(value) ? value->valuestring : "1 1 1", 1, c);
        dv_set_color(&p->value, c);
    } else if (!strcmp(type, "bool")) {
        p->type = PROP_BOOL;
        dv_set_bool(&p->value, js_bool(node, "value", false));
    } else if (!strcmp(type, "slider")) {
        p->type = PROP_SLIDER;
        p->min = js_num(node, "min", 0);
        p->max = js_num(node, "max", 0);
        p->step = js_num(node, "step", 0);
        dv_set_float(&p->value, js_num(node, "value", 0));
    } else if (!strcmp(type, "combo")) {
        p->type = PROP_COMBO;
        const cJSON *opt;
        cJSON_ArrayForEach(opt, js_get(node, "options"))
        {
            if (!cJSON_IsObject(opt)) continue;
            const cJSON *v = js_get(opt, "value");
            char buf[64];
            ComboOption o;
            if (v && cJSON_IsNumber(v)) {
                snprintf(buf, sizeof buf, "%d", (int)v->valuedouble);
                o.value = strdup(buf);
            } else {
                o.value = strdup(v && cJSON_IsString(v) ? v->valuestring : "");
            }
            o.label = strdup(js_str(opt, "label", ""));
            VPUSH(p->options, o);
        }
        char buf[64];
        if (value && cJSON_IsNumber(value)) {
            snprintf(buf, sizeof buf, "%d", (int)value->valuedouble);
            dv_set_string(&p->value, buf);
        } else {
            dv_set_string(&p->value, value && cJSON_IsString(value) ? value->valuestring : "");
        }
    } else if (!strcmp(type, "text")) {
        p->type = PROP_TEXT;
        dv_set_null(&p->value);
    } else if (!strcmp(type, "scenetexture")) {
        p->type = PROP_SCENETEXTURE;
        dv_set_string(&p->value, value && cJSON_IsString(value) ? value->valuestring : "");
    } else if (!strcmp(type, "file") || !strcmp(type, "directory")) {
        p->type = PROP_FILE;
        dv_set_string(&p->value, js_str(node, "value", ""));
    } else if (!strcmp(type, "textinput") || !strcmp(type, "usershortcut")) {
        p->type = PROP_TEXTINPUT;
        char *dump = value ? cJSON_PrintUnformatted(value) : strdup("");
        dv_set_string(&p->value, dump);
        free(dump);
    } else {
        we_err("unexpected property type '%s' for %s", type, name);
        free(p->name);
        free(p->text);
        free(p);
        return NULL;
    }
    return p;
}

void prop_set(Property *p, const char *value)
{
    switch (p->type) {
    case PROP_SLIDER: dv_set_float(&p->value, atof(value)); break;
    case PROP_BOOL: dv_set_bool(&p->value, !strcmp(value, "true") || !strcmp(value, "1")); break;
    case PROP_COLOR: {
        float c[4];
        color_parse(value, 1, c);
        dv_set_color(&p->value, c);
        break;
    }
    case PROP_COMBO: {
        bool found = false;
        for (int i = 0; i < p->options.n; i++)
            if (!strcmp(p->options.v[i].value, value)) found = true;
        if (!found) {
            we_err("combo value '%s' is not an option of %s", value, p->name);
            return;
        }
        dv_set_string(&p->value, value);
        break;
    }
    default: dv_set_string(&p->value, value);
    }
    prop_notify(p);
}

void prop_dump(const Property *p, FILE *f)
{
    static const char *names[] = { "slider", "boolean", "color", "combo", "text", "scene texture", "file",
                                   "textinput" };
    char buf[256];
    fprintf(f, "%s - %s\n\tText: %s\n", p->name, names[p->type], p->text);
    if (p->type == PROP_SLIDER) fprintf(f, "\tMin: %g\n\tMax: %g\n\tStep: %g\n", p->min, p->max, p->step);
    fprintf(f, "\tValue: %s\n", p->type == PROP_TEXT ? p->text : dv_to_string(&p->value, buf, sizeof buf));
    if (p->type == PROP_COMBO) {
        fprintf(f, "\tValues:\n");
        for (int i = 0; i < p->options.n; i++)
            fprintf(f, "\t\t%s = %s\n", p->options.v[i].value, p->options.v[i].label);
    }
}

void prop_free(Property *p)
{
    free(p->name);
    free(p->text);
    dv_free(&p->value);
    for (int i = 0; i < p->options.n; i++) {
        free(p->options.v[i].value);
        free(p->options.v[i].label);
    }
    VFREE(p->options);
    VFREE(p->listeners);
    free(p);
}

/* ------------------------------------------------------------------ maps */
void texmap_set(Arena *a, TexMap *m, int idx, const char *name)
{
    for (int i = 0; i < m->n; i++)
        if (m->v[i].idx == idx) {
            m->v[i].name = (char *)name;
            return;
        }
    TexEntry e = { idx, (char *)name };
    AVPUSH(a, *m, e);
}

const char *texmap_get(const TexMap *m, int idx)
{
    for (int i = 0; i < m->n; i++)
        if (m->v[i].idx == idx) return m->v[i].name;
    return NULL;
}

void combo_set(Arena *a, ComboMap *m, const char *name, int value)
{
    for (int i = 0; i < m->n; i++)
        if (!strcmp(m->v[i].name, name)) {
            m->v[i].value = value;
            return;
        }
    Combo c = { (char *)name, value };
    AVPUSH(a, *m, c);
}

int combo_find(const ComboMap *m, const char *name)
{
    for (int i = 0; i < m->n; i++)
        if (!strcmp(m->v[i].name, name)) return i;
    return -1;
}

int const_find(const ConstMap *m, const char *name)
{
    for (int i = 0; i < m->n; i++)
        if (!strcmp(m->v[i].name, name)) return i;
    return -1;
}
