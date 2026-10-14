/* Wallpaper Engine shader units: preprocessing (includes, combos, annotated
 * uniforms) and compilation. Wallpaper Engine shaders rely on HLSL-like
 * implicit conversions, so they go through a patched glslang (to SPIR-V) and
 * SPIRV-Cross (back to GLSL 330) before reaching the driver. */
#include "internal.h"

#include "../third_party/glslang/glslang/Include/glslang_c_interface.h"
#include "../third_party/glslang/glslang/Public/resource_limits_c.h"
#include "../third_party/spirv-cross/spirv_cross_c.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ string builder */
typedef struct { char *s; size_t n, cap; } Str;

static void sb_putn(Str *b, const char *s, size_t n)
{
    if (b->n + n + 1 > b->cap) {
        b->cap = (b->n + n + 1) * 2;
        b->s = realloc(b->s, b->cap);
    }
    memcpy(b->s + b->n, s, n);
    b->n += n;
    b->s[b->n] = 0;
}
static void sb_put(Str *b, const char *s) { sb_putn(b, s, strlen(s)); }
static void sb_printf(Str *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void sb_printf(Str *b, const char *fmt, ...)
{
    char tmp[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n >= (int)sizeof tmp) n = sizeof tmp - 1;
    sb_putn(b, tmp, n);
}

/* replace [pos, pos+len) of *s with rep */
static void str_splice(char **s, size_t pos, size_t len, const char *rep)
{
    size_t sl = strlen(*s), rl = strlen(rep);
    char *n = malloc(sl - len + rl + 1);
    memcpy(n, *s, pos);
    memcpy(n + pos, rep, rl);
    memcpy(n + pos + rl, *s + pos + len, sl - pos - len + 1);
    free(*s);
    *s = n;
}

static long str_find(const char *s, const char *needle, long from)
{
    if (from < 0) from = 0;
    if ((size_t)from > strlen(s)) return -1;
    const char *p = strstr(s + from, needle);
    return p ? p - s : -1;
}

/* last occurrence of needle starting at or before 'before' */
static long str_rfind(const char *s, const char *needle, long before)
{
    long best = -1, nl = strlen(needle);
    for (const char *p = strstr(s, needle); p && p - s <= before; p = strstr(p + 1, needle)) best = p - s;
    (void)nl;
    return best;
}

static long str_rfind_char(const char *s, char c, long before)
{
    for (long i = before; i >= 0; i--)
        if (s[i] == c) return i;
    return -1;
}

static bool is_ident(char c) { return isalnum((unsigned char)c) || c == '_'; }

/* ------------------------------------------------------------------ preprocessing */
static char *include_content(Vfs *vfs, const char *file)
{
    Str b = { 0 };
    char *inc = vfs_read_shader(vfs, file, "h");
    if (inc) {
        sb_printf(&b, "// begin of include from file %s\n", file);
        sb_put(&b, inc);
        sb_printf(&b, "\n// end of included from file %s\n", file);
        free(inc);
    } else {
        sb_printf(&b, "// tried including file %s but was not found\n", file);
    }
    return b.s;
}

static bool extract_quoted(const char *s, long start, char *out, size_t n, long *qend)
{
    const char *a = strchr(s + start, '"');
    if (!a) return false;
    const char *e = strchr(a + 1, '"');
    if (!e) return false;
    snprintf(out, n, "%.*s", (int)(e - a - 1), a + 1);
    if (qend) *qend = e - s;
    return true;
}

static void preprocess_includes(ShaderUnit *u, Vfs *vfs)
{
    Str includes = { 0 };
    sb_put(&includes, "");
    long start, end = 0;
    char file[PATH_MAX];
    while ((start = str_find(u->preprocessed, "#include", end)) >= 0) {
        if (extract_quoted(u->preprocessed, start, file, sizeof file, NULL)) {
            char *c = include_content(vfs, file);
            sb_put(&includes, c);
            free(c);
        }
        u->preprocessed[start] = '/';
        u->preprocessed[start + 1] = '/';
        end = start;
    }
    /* nested includes are expanded in place */
    end = 0;
    while ((start = str_find(includes.s, "#include", end)) >= 0) {
        long line_end = str_find(includes.s, "\n", start);
        if (line_end < 0) line_end = strlen(includes.s);
        char *c;
        if (extract_quoted(includes.s, start, file, sizeof file, NULL) && strlen(file))
            c = include_content(vfs, file);
        else
            c = strdup("");
        str_splice(&includes.s, start, line_end - start, c);
        free(c);
        end = start;
        if (end > 4 * 1024 * 1024) break;
    }

    /* place the includes before main(), outside of any #if block */
    end = 0;
    bool added = false;
    const char *s;
    while ((start = str_find(u->preprocessed, " main", end)) >= 0) {
        s = u->preprocessed;
        char v = s[start + 5];
        end = start + 5;
        if (v != ' ' && v != '(') continue;
        long la = str_rfind(s, "attribute", start), lv = str_rfind(s, "varying", start),
             lu = str_rfind(s, "uniform", start);
        long latest = la;
        if (latest < 0) latest = lv;
        else if (lv >= 0 && latest < lv) latest = lv;
        if (latest < 0) latest = lu;
        else if (lu >= 0 && latest < lu) latest = lu;
        if (latest >= 0 && latest < start) {
            long nl = str_find(s, "\n", latest);
            latest = nl >= 0 ? nl : (long)strlen(s);
        } else {
            latest = str_rfind_char(s, '\n', start);
        }
        start = str_rfind_char(s, '\n', start);
        /* walk #if/#endif pairs */
        long stack[256];
        int sp = 0;
        for (long cur = 0; s[cur]; cur++) {
            if (s[cur] != '#') continue;
            if (!strncmp(s + cur, "#if", 3)) {
                if (sp < 256) stack[sp++] = cur;
            } else if (!strncmp(s + cur, "#endif", 6)) {
                if (!sp) continue;
                long ss = stack[--sp];
                if (latest > ss && latest <= cur) {
                    long before = str_rfind_char(s, '\n', ss);
                    latest = before >= 0 ? before : 0;
                }
            }
        }
        if (start >= 0 && (latest < 0 || start < latest)) latest = start;
        Str ins = { 0 };
        sb_put(&ins, includes.s);
        sb_put(&ins, "\n");
        long at = latest + 1;
        if (at < 0) at = 0;
        str_splice(&u->preprocessed, at, 0, ins.s);
        free(ins.s);
        added = true;
        break;
    }
    if (!added) we_err("could not find where to place includes in shader %s", u->file);
    free(includes.s);
}

static void preprocess_requires(ShaderUnit *u)
{
    long start, end = 0;
    while ((start = str_find(u->preprocessed, "#require", end)) >= 0) {
        long line_end = str_find(u->preprocessed, "\n", start);
        if (line_end < 0) line_end = strlen(u->preprocessed);
        char name[128];
        long ns = start + 9;
        snprintf(name, sizeof name, "%.*s", ns < line_end ? (int)(line_end - ns) : 0, u->preprocessed + ns);
        for (int i = strlen(name) - 1; i >= 0 && (name[i] == ' ' || name[i] == '\r'); i--) name[i] = 0;
        u->preprocessed[start] = '/';
        u->preprocessed[start + 1] = '/';
        if (!strcmp(name, "LightingV1")) {
            const char *code =
                "// begin of generated module LightingV1\n"
                "vec3 PerformLighting_V1(vec3 worldPos, vec3 albedo, vec3 normal, vec3 viewDir,\n"
                "    vec3 specularTint, vec3 baseReflectance, float roughness, float metallic)\n"
                "{\n"
                "    return vec3(0.0);\n"
                "}\n"
                "// end of generated module LightingV1\n";
            str_splice(&u->preprocessed, start, 0, code);
            end = start + strlen(code) + 2;
        } else {
            we_err("unknown #require module '%s' in shader %s", name, u->file);
            end = line_end;
        }
    }
}

static void parse_combo(ShaderUnit *u, Arena *a, const char *json)
{
    cJSON *j = cJSON_Parse(json);
    if (!j) {
        we_err("cannot parse combo metadata in %s: %s", u->file, json);
        return;
    }
    const char *combo = js_str(j, "combo", NULL);
    if (combo && combo_find(u->combos, combo) < 0 && combo_find(u->override_combos, combo) < 0 &&
        combo_find(&u->discovered, combo) < 0) {
        const cJSON *def = js_get(j, "default");
        int v = def && cJSON_IsNumber(def) ? (int)def->valuedouble : 0;
        combo_set(a, &u->discovered, arena_strdup(a, combo), v);
    }
    cJSON_Delete(j);
}

static int json_int(const cJSON *c)
{
    if (!c) return 0;
    if (cJSON_IsNumber(c)) return (int)c->valuedouble;
    if (cJSON_IsString(c)) return atoi(c->valuestring);
    if (cJSON_IsBool(c)) return cJSON_IsTrue(c);
    return 0;
}

static void parse_param(ShaderUnit *u, Arena *a, const char *type, const char *name, const char *json)
{
    cJSON *j = cJSON_Parse(json);
    if (!j) {
        we_err("cannot parse annotation for %s in %s", name, u->file);
        return;
    }
    const char *material = js_str(j, "material", NULL);
    const cJSON *def = js_get(j, "default");
    const char *combo = js_str(j, "combo", NULL);

    if (!strcmp(type, "sampler2D") || !strcmp(type, "sampler2DComparison")) {
        int index = strlen(name) > 9 ? name[9] - '0' : 0;
        if (index < 0 || index > 15) index = 0;
        if (combo) {
            bool used = texmap_get(u->pass_textures, index) || texmap_get(u->override_textures, index);
            bool required = false;
            int value = 1;
            const cJSON *require = js_get(j, "require");
            if (used) {
                required = true;
            } else if (require) {
                if (js_bool(j, "requireany", false)) {
                    const cJSON *it;
                    cJSON_ArrayForEach(it, require)
                    {
                        int ci = combo_find(u->combos, it->string);
                        if (ci < 0 || combo_find(u->override_combos, it->string) >= 0 ||
                            u->combos->v[ci].value != json_int(it)) {
                            required = true;
                            break;
                        }
                    }
                } else {
                    required = true;
                    const cJSON *it;
                    cJSON_ArrayForEach(it, require)
                    {
                        int ci = combo_find(u->combos, it->string);
                        bool exists = ci >= 0 || combo_find(u->override_combos, it->string) >= 0;
                        if (exists && ci >= 0 && u->combos->v[ci].value == json_int(it)) {
                            required = false;
                            break;
                        }
                    }
                }
            }
            if (required && !used) {
                if (!def) {
                    required = false;
                } else if (combo_find(u->combos, combo) >= 0 || combo_find(u->override_combos, combo) >= 0) {
                    required = false;
                } else {
                    value = json_int(def);
                }
            }
            if (required && combo_find(&u->discovered, combo) < 0)
                combo_set(a, &u->discovered, arena_strdup(a, combo), value);
        }
        if (def && cJSON_IsString(def) && !texmap_get(&u->textures, index))
            texmap_set(a, &u->textures, index, arena_strdup(a, def->valuestring));
        cJSON_Delete(j);
        return;
    }

    int t;
    if (!strcmp(type, "float")) t = SV_FLOAT;
    else if (!strcmp(type, "int")) t = SV_INT;
    else if (!strcmp(type, "vec2")) t = SV_VEC2;
    else if (!strcmp(type, "vec3")) t = SV_VEC3;
    else if (!strcmp(type, "vec4")) t = SV_VEC4;
    else {
        we_dbg("unknown parameter type %s for %s in %s", type, name, u->file);
        cJSON_Delete(j);
        return;
    }
    if (!def && !(material && u->constants && const_find(u->constants, material) >= 0)) {
        cJSON_Delete(j);
        return;
    }
    ShaderParam p = { .type = t, .name = arena_strdup(a, name), .material = arena_strdup(a, material) };
    float v[4] = { 0 };
    if (def && cJSON_IsString(def)) {
        int n = vec_parse(def->valuestring, v);
        if (n == 1) v[1] = v[2] = v[3] = v[0];
    } else if (def && cJSON_IsNumber(def)) {
        v[0] = v[1] = v[2] = v[3] = def->valuedouble;
    } else if (def && cJSON_IsBool(def)) {
        v[0] = cJSON_IsTrue(def);
    }
    switch (t) {
    case SV_FLOAT: dv_set_float(&p.def, v[0]); break;
    case SV_INT: dv_set_int(&p.def, (int)v[0]); break;
    case SV_VEC2: dv_set_vec(&p.def, v, 2); break;
    case SV_VEC3: dv_set_vec(&p.def, v, 3); break;
    default: dv_set_vec(&p.def, v, 4);
    }
    AVPUSH(a, u->params, p);
    cJSON_Delete(j);
}

static void preprocess_variables(ShaderUnit *u, Arena *a)
{
    const char *s = u->preprocessed;
    const char *line = s;
    char buf[4096];
    while (1) {
        const char *nl = strchr(line, '\n');
        if (!nl) break;
        size_t len = nl - line;
        if (len >= sizeof buf) len = sizeof buf - 1;
        memcpy(buf, line, len);
        buf[len] = 0;
        char *combo = strstr(buf, "// [COMBO] ");
        char *uniform = strstr(buf, "uniform ");
        char *comment = strstr(buf, "// ");
        char *semi = strchr(buf, ';');
        if (combo) {
            parse_combo(u, a, combo + 11);
        } else if (uniform && comment && semi && semi < comment) {
            char *last = NULL, *prev = NULL;
            for (char *c = buf; c <= semi; c++)
                if (*c == ' ') last = c;
            if (last)
                for (char *c = buf; c < last; c++)
                    if (*c == ' ') prev = c;
            if (last && prev) {
                char type[64], name[128];
                snprintf(type, sizeof type, "%.*s", (int)(last - prev - 1), prev + 1);
                snprintf(name, sizeof name, "%.*s", (int)(semi - last - 1), last + 1);
                parse_param(u, a, type, name, comment + 2);
            }
        }
        line = nl + 1;
    }
}

static void replace_all(char **s, const char *from, const char *to)
{
    long pos = 0;
    size_t fl = strlen(from), tl = strlen(to);
    while ((pos = str_find(*s, from, pos)) >= 0) {
        str_splice(s, pos, fl, to);
        pos += tl;
    }
}

static void unit_init(ShaderUnit *u, Arena *a, Vfs *vfs, bool vertex, const char *file, char *content)
{
    u->vertex = vertex;
    u->file = arena_strdup(a, file);
    u->preprocessed = content;
    preprocess_includes(u, vfs);
    preprocess_requires(u);
    preprocess_variables(u, a);
    replace_all(&u->preprocessed, "gl_FragColor", "out_FragColor");
}

/* "varying vecN NAME;" with any whitespace */
static bool find_varying(const char *s, const char *type, const char *name, long *pos, long *len)
{
    for (const char *p = strstr(s, "varying"); p; p = strstr(p + 1, "varying")) {
        if (p > s && is_ident(p[-1])) continue;
        const char *q = p + 7;
        if (!isspace((unsigned char)*q)) continue;
        while (isspace((unsigned char)*q)) q++;
        size_t tl = strlen(type);
        if (strncmp(q, type, tl) || is_ident(q[tl])) continue;
        q += tl;
        if (!isspace((unsigned char)*q)) continue;
        while (isspace((unsigned char)*q)) q++;
        size_t nl = strlen(name);
        if (name[0] && (strncmp(q, name, nl) || is_ident(q[nl]))) continue;
        const char *r = q + (name[0] ? nl : 0);
        if (!name[0]) {
            while (is_ident(*r)) r++;
        }
        while (isspace((unsigned char)*r)) r++;
        if (*r != ';') continue;
        *pos = p - s;
        *len = r + 1 - p;
        return true;
    }
    return false;
}

static char *linked_varying_compat(const ShaderUnit *u, char *src)
{
    if (!u->vertex || !u->link) return src;
    const char *frag = u->link->preprocessed;
    for (const char *p = strstr(frag, "varying"); p; p = strstr(p + 1, "varying")) {
        long pos, len;
        if (!find_varying(p, "vec4", "", &pos, &len) || pos != 0) continue;
        /* extract the name */
        const char *q = p + 7;
        while (isspace((unsigned char)*q)) q++;
        q += 4;
        while (isspace((unsigned char)*q)) q++;
        char name[128];
        int n = 0;
        while (is_ident(q[n]) && n < 127) {
            name[n] = q[n];
            n++;
        }
        name[n] = 0;
        long vpos, vlen;
        if (!find_varying(src, "vec2", name, &vpos, &vlen)) continue;
        char decl[192];
        snprintf(decl, sizeof decl, "varying vec4 %s;", name);
        str_splice(&src, vpos, vlen, decl);
        /* rewrite "NAME = expr;" at line starts */
        long off = 0;
        while (1) {
            long at = str_find(src, name, off);
            if (at < 0) break;
            off = at + strlen(name);
            long ls = at;
            while (ls > 0 && (src[ls - 1] == ' ' || src[ls - 1] == '\t')) ls--;
            if (ls > 0 && src[ls - 1] != '\n') continue;
            const char *e = src + at + strlen(name);
            while (isspace((unsigned char)*e) && *e != '\n') e++;
            if (*e != '=' || e[1] == '=') continue;
            e++;
            while (isspace((unsigned char)*e) && *e != '\n') e++;
            const char *semi = e;
            while (*semi && *semi != ';' && *semi != '\n') semi++;
            if (*semi != ';') continue;
            Str rep = { 0 };
            sb_printf(&rep, "%s = vec4(%.*s, 0.0, 1.0);", name, (int)(semi - e), e);
            str_splice(&src, at, semi + 1 - (src + at), rep.s);
            off = at + rep.n;
            free(rep.s);
        }
    }
    return src;
}

static char *texcoord_compat(const ShaderUnit *u, char *src)
{
    if (u->vertex) return src;
    long p, l;
    if (!find_varying(src, "vec3", "v_TexCoord", &p, &l) && !find_varying(src, "vec4", "v_TexCoord", &p, &l))
        return src;
    bool changed = false;
    long off = 0;
    while (1) {
        long at = str_find(src, "v_TexCoord", off);
        if (at < 0) break;
        off = at + 10;
        if ((at > 0 && is_ident(src[at - 1])) || is_ident(src[at + 10]) || src[at + 10] == '.') continue;
        /* v_TexCoord <op> CAST2( */
        const char *e = src + at + 10;
        while (*e == ' ' || *e == '\t') e++;
        bool after = (*e == '-' || *e == '+' || *e == '*' || *e == '/');
        if (after) {
            e++;
            while (*e == ' ' || *e == '\t') e++;
            after = !strncmp(e, "CAST2", 5);
        }
        /* CAST2(...) <op> v_TexCoord */
        bool before = false;
        long b = at - 1;
        while (b >= 0 && (src[b] == ' ' || src[b] == '\t')) b--;
        if (b >= 0 && (src[b] == '-' || src[b] == '+' || src[b] == '*' || src[b] == '/')) {
            b--;
            while (b >= 0 && (src[b] == ' ' || src[b] == '\t')) b--;
            if (b >= 0 && src[b] == ')') {
                int depth = 0;
                long k = b;
                for (; k >= 0; k--) {
                    if (src[k] == ')') depth++;
                    else if (src[k] == '(' && --depth == 0) break;
                }
                long m = k - 1;
                while (m >= 0 && (src[m] == ' ' || src[m] == '\t')) m--;
                before = m >= 4 && !strncmp(src + m - 4, "CAST2", 5);
            }
        }
        if (after || before) {
            str_splice(&src, at + 10, 0, ".xy");
            off = at + 13;
            changed = true;
        }
    }
    if (changed) we_dbg("applied fragment TexCoord vec2 compatibility in %s", u->file);
    return src;
}

static void add_combos(Str *b, const ComboMap *m, ComboMap *added, Arena *a)
{
    if (!m) return;
    for (int i = 0; i < m->n; i++) {
        char up[128];
        snprintf(up, sizeof up, "%s", m->v[i].name);
        for (char *c = up; *c; c++) *c = toupper((unsigned char)*c);
        if (combo_find(added, up) >= 0) continue;
        combo_set(a, added, arena_strdup(a, up), 1);
        sb_printf(b, "#define %s %d\n", up, m->v[i].value);
    }
}

static char *unit_compile(ShaderUnit *u, Arena *a)
{
    Str b = { 0 };
    sb_printf(&b,
              "#version 330\n"
              "// ======================================================\n"
              "// Processed shader %s\n"
              "// ======================================================\n"
              "precision highp float;\n"
              "#define mul(x, y) ((y) * (x))\n"
              "#define max(x, y) max (y, x)\n"
              "#define lerp mix\n"
              "#define frac fract\n"
              "#define CAST2(x) (vec2(x))\n"
              "#define CAST3(x) (vec3(x))\n"
              "#define CAST4(x) (vec4(x))\n"
              "#define CAST3X3(x) (mat3(x))\n"
              "#define float2 vec2\n"
              "#define float3 vec3\n"
              "#define float4 vec4\n"
              "#define int2 ivec2\n"
              "#define int3 ivec3\n"
              "#define int4 ivec4\n"
              "#define saturate(x) (clamp(x, 0.0, 1.0))\n"
              "#define texSample2D texture\n"
              "#define texSample2DLod textureLod\n"
              "#define log10(x) (log2(x) * 0.301029995663981)\n"
              "#define atan2 atan\n"
              "#define fmod(x, y) ((x)-(y)*trunc((x)/(y)))\n"
              "#define ddx dFdx\n"
              "#define ddy(x) dFdy(-(x))\n"
              "#define GLSL 1\n\n",
              u->file);
    if (u->vertex) sb_put(&b, "#define attribute in\n#define varying out\n");
    else sb_put(&b, "out vec4 out_FragColor;\n#define varying in\n");
    ComboMap added = { 0 };
    add_combos(&b, u->override_combos, &added, a);
    add_combos(&b, u->combos, &added, a);
    add_combos(&b, &u->discovered, &added, a);
    if (u->link) {
        add_combos(&b, u->link->combos, &added, a);
        add_combos(&b, &u->link->discovered, &added, a);
    }
    char *body = strdup(u->preprocessed);
    body = linked_varying_compat(u, body);
    body = texcoord_compat(u, body);
    sb_put(&b, body);
    free(body);
    return b.s;
}

/* ------------------------------------------------------------------ glslang / SPIRV-Cross */
static bool glslang_ready;

bool shaderc_init(void)
{
    if (!glslang_ready) glslang_ready = glslang_initialize_process();
    return glslang_ready;
}

static glslang_shader_t *gs_parse(glslang_stage_t stage, const char *src, const char *file)
{
    glslang_input_t in = {
        .language = GLSLANG_SOURCE_GLSL,
        .stage = stage,
        .client = GLSLANG_CLIENT_OPENGL,
        .client_version = GLSLANG_TARGET_OPENGL_450,
        .target_language = GLSLANG_TARGET_SPV,
        .target_language_version = GLSLANG_TARGET_SPV_1_5,
        .code = src,
        .default_version = 330,
        .default_profile = GLSLANG_NO_PROFILE,
        .messages = GLSLANG_MSG_DEFAULT_BIT,
        .resource = glslang_default_resource(),
    };
    glslang_shader_t *s = glslang_shader_create(&in);
    glslang_shader_set_options(s, GLSLANG_SHADER_AUTO_MAP_LOCATIONS | GLSLANG_SHADER_AUTO_MAP_BINDINGS);
    if (!glslang_shader_preprocess(s, &in) || !glslang_shader_parse(s, &in)) {
        we_err("%s %s unit failed to parse:\n%s", file, stage == GLSLANG_STAGE_VERTEX ? "vertex" : "fragment",
               glslang_shader_get_info_log(s));
        if (cfg.verbose) fprintf(stderr, "---- source ----\n%s\n----------------\n", src);
        glslang_shader_delete(s);
        return NULL;
    }
    return s;
}

static char *spirv_to_glsl(glslang_program_t *prog, glslang_stage_t stage)
{
    glslang_program_SPIRV_generate(prog, stage);
    spvc_context ctx = NULL;
    spvc_parsed_ir ir;
    spvc_compiler comp;
    spvc_compiler_options opt;
    const char *res = NULL;
    char *out = NULL;
    if (spvc_context_create(&ctx) != SPVC_SUCCESS) return NULL;
    if (spvc_context_parse_spirv(ctx, glslang_program_SPIRV_get_ptr(prog), glslang_program_SPIRV_get_size(prog),
                                 &ir) == SPVC_SUCCESS &&
        spvc_context_create_compiler(ctx, SPVC_BACKEND_GLSL, ir, SPVC_CAPTURE_MODE_TAKE_OWNERSHIP, &comp) ==
            SPVC_SUCCESS) {
        spvc_compiler_create_compiler_options(comp, &opt);
        spvc_compiler_options_set_uint(opt, SPVC_COMPILER_OPTION_GLSL_VERSION, 330);
        spvc_compiler_options_set_bool(opt, SPVC_COMPILER_OPTION_GLSL_ES, SPVC_FALSE);
        spvc_compiler_install_compiler_options(comp, opt);
        if (spvc_compiler_compile(comp, &res) == SPVC_SUCCESS) out = strdup(res);
        else we_err("SPIRV-Cross: %s", spvc_context_get_last_error_string(ctx));
    }
    spvc_context_destroy(ctx);
    return out;
}

static bool to_glsl(const char *vs, const char *fs, const char *file, char **ovs, char **ofs)
{
    if (!shaderc_init()) return false;
    glslang_shader_t *v = gs_parse(GLSLANG_STAGE_VERTEX, vs, file);
    glslang_shader_t *f = v ? gs_parse(GLSLANG_STAGE_FRAGMENT, fs, file) : NULL;
    bool ok = false;
    if (v && f) {
        glslang_program_t *p = glslang_program_create();
        glslang_program_add_shader(p, v);
        glslang_program_add_shader(p, f);
        if (glslang_program_link(p, GLSLANG_MSG_DEFAULT_BIT)) {
            *ovs = spirv_to_glsl(p, GLSLANG_STAGE_VERTEX);
            *ofs = spirv_to_glsl(p, GLSLANG_STAGE_FRAGMENT);
            ok = *ovs && *ofs;
        } else {
            we_err("%s: link failed: %s", file, glslang_program_get_info_log(p));
        }
        glslang_program_delete(p);
    }
    if (v) glslang_shader_delete(v);
    if (f) glslang_shader_delete(f);
    return ok;
}

static GLuint gl_compile(GLenum type, const char *src, const char *label)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        we_err("GL %s shader %s:\n%s", type == GL_VERTEX_SHADER ? "vertex" : "fragment", label, log);
        if (cfg.verbose) fprintf(stderr, "---- source ----\n%s\n----------------\n", src);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

GLuint gl_program_from_glsl(const char *vs, const char *fs, const char *label)
{
    GLuint v = gl_compile(GL_VERTEX_SHADER, vs, label), f = v ? gl_compile(GL_FRAGMENT_SHADER, fs, label) : 0;
    if (!v || !f) {
        if (v) glDeleteShader(v);
        return 0;
    }
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    glDetachShader(p, v);
    glDetachShader(p, f);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint ok;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetProgramInfoLog(p, sizeof log, NULL, log);
        we_err("GL link %s:\n%s", label, log);
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

/* program cache: identical processed sources share one GL program */
typedef struct { char *vs, *fs; GLuint program; int refs; } CacheEntry;
static VEC(CacheEntry) cache;

static GLuint cached_program(const char *vs, const char *fs, const char *file)
{
    for (int i = 0; i < cache.n; i++)
        if (!strcmp(cache.v[i].vs, vs) && !strcmp(cache.v[i].fs, fs)) {
            cache.v[i].refs++;
            return cache.v[i].program;
        }
    char *gvs = NULL, *gfs = NULL;
    GLuint prog = 0;
    if (to_glsl(vs, fs, file, &gvs, &gfs)) prog = gl_program_from_glsl(gvs, gfs, file);
    free(gvs);
    free(gfs);
    if (!prog) return 0;
    CacheEntry e = { strdup(vs), strdup(fs), prog, 1 };
    VPUSH(cache, e);
    return prog;
}

static void release_program(GLuint prog)
{
    for (int i = 0; i < cache.n; i++)
        if (cache.v[i].program == prog) {
            if (--cache.v[i].refs > 0) return;
            glDeleteProgram(prog);
            free(cache.v[i].vs);
            free(cache.v[i].fs);
            cache.v[i] = cache.v[--cache.n];
            return;
        }
}

Shader *shader_build(WeProject *p, const char *name, const ComboMap *combos, const ComboMap *ovr_combos,
                     const TexMap *textures, const TexMap *ovr_textures, const ConstMap *constants)
{
    char *vsrc = vfs_read_shader(p->vfs, name, "vert");
    char *fsrc = vfs_read_shader(p->vfs, name, "frag");
    if (!vsrc || !fsrc) {
        we_err("cannot find shader %s (shaders/%s.vert/.frag). Are the Wallpaper Engine assets installed? "
               "(--assets-dir)",
               name, name);
        free(vsrc);
        free(fsrc);
        return NULL;
    }
    Arena *a = &p->arena;
    Shader *s = calloc(1, sizeof *s);
    static const ComboMap empty_c;
    static const TexMap empty_t;
    for (int i = 0; i < 2; i++) {
        ShaderUnit *u = i ? &s->fragment : &s->vertex;
        u->combos = combos ? combos : &empty_c;
        u->override_combos = ovr_combos ? ovr_combos : &empty_c;
        u->pass_textures = textures ? textures : &empty_t;
        u->override_textures = ovr_textures ? ovr_textures : &empty_t;
        u->constants = constants;
    }
    unit_init(&s->vertex, a, p->vfs, true, name, vsrc);
    unit_init(&s->fragment, a, p->vfs, false, name, fsrc);
    s->vertex.link = &s->fragment;
    s->fragment.link = &s->vertex;
    char *vs = unit_compile(&s->vertex, a);
    char *fs = unit_compile(&s->fragment, a);
    s->program = cached_program(vs, fs, name);
    free(vs);
    free(fs);
    if (!s->program) {
        shader_free(s);
        return NULL;
    }
    return s;
}

void shader_free(Shader *s)
{
    if (!s) return;
    if (s->program) release_program(s->program);
    free(s->vertex.preprocessed);
    free(s->fragment.preprocessed);
    for (int i = 0; i < s->vertex.params.n; i++) dv_free(&s->vertex.params.v[i].def);
    for (int i = 0; i < s->fragment.params.n; i++) dv_free(&s->fragment.params.v[i].def);
    free(s);
}

const ShaderParam *shader_find_param(const Shader *s, const char *material)
{
    for (int k = 0; k < 2; k++) {
        const ShaderUnit *u = k ? &s->fragment : &s->vertex;
        for (int i = 0; i < u->params.n; i++)
            if (u->params.v[i].material && !strcmp(u->params.v[i].material, material)) return &u->params.v[i];
    }
    return NULL;
}
