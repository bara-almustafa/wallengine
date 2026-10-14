/* Project loading: Steam paths, project.json, properties, and the material /
 * effect / model definition files. */
#include "internal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define WORKSHOP_APP_ID "431960"
#define APP_DIRECTORY   "wallpaper_engine"

static const char *app_dirs[] = {
    ".steam/steam/steamapps/common",
    ".local/share/Steam/steamapps/common",
    ".var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps/common",
    "snap/steam/common/.local/share/Steam/steamapps/common",
};

static const char *workshop_dirs[] = {
    ".local/share/Steam/steamapps/workshop/content",
    ".steam/steam/steamapps/workshop/content",
    ".var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps/workshop/content",
    "snap/steam/common/.local/share/Steam/steamapps/workshop/content",
};

static bool is_dir(const char *p)
{
    struct stat st;
    return !stat(p, &st) && S_ISDIR(st.st_mode);
}

static bool is_file(const char *p)
{
    struct stat st;
    return !stat(p, &st) && S_ISREG(st.st_mode);
}

bool we_find_install(char *out, size_t n)
{
    const char *home = getenv("HOME");
    for (size_t i = 0; home && i < sizeof app_dirs / sizeof *app_dirs; i++) {
        snprintf(out, n, "%s/%s/%s", home, app_dirs[i], APP_DIRECTORY);
        if (is_dir(out)) return true;
    }
    return false;
}

bool we_find_assets(char *out, size_t n)
{
    if (we_settings.assets[0]) {
        snprintf(out, n, "%s", we_settings.assets);
        return is_dir(out);
    }
    char base[PATH_MAX];
    if (we_find_install(base, sizeof base)) {
        snprintf(out, n, "%s/assets", base);
        if (is_dir(out)) return true;
    }
    /* next to the executable, or in the config directory */
    char exe[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (len > 0) {
        exe[len] = 0;
        char *slash = strrchr(exe, '/');
        if (slash) *slash = 0;
        snprintf(out, n, "%s/assets", exe);
        if (is_dir(out)) return true;
    }
    const char *home = getenv("HOME");
    if (home) {
        snprintf(out, n, "%s/.config/wallengine/assets", home);
        if (is_dir(out)) return true;
    }
    return false;
}

static bool all_digits(const char *s)
{
    if (!*s) return false;
    for (; *s; s++)
        if (!isdigit((unsigned char)*s)) return false;
    return true;
}

bool we_resolve(const char *id, char *out, size_t n)
{
    if (!id || !*id) return false;
    if (!strchr(id, '/') && all_digits(id)) {
        const char *home = getenv("HOME");
        for (size_t i = 0; home && i < sizeof workshop_dirs / sizeof *workshop_dirs; i++) {
            snprintf(out, n, "%s/%s/%s/%s", home, workshop_dirs[i], WORKSHOP_APP_ID, id);
            if (is_dir(out)) return true;
        }
        return false;
    }
    char path[PATH_MAX];
    if (!realpath(id, path)) return false;
    if (is_file(path)) {
        const char *base = strrchr(path, '/');
        if (!base || strcmp(base + 1, "project.json")) return false;
        *strrchr(path, '/') = 0;
    }
    char pj[PATH_MAX + 16];
    snprintf(pj, sizeof pj, "%s/project.json", path);
    if (!is_file(pj)) return false;
    snprintf(out, n, "%s", path);
    return true;
}

bool we_is_project(const char *id)
{
    char tmp[PATH_MAX];
    return we_resolve(id, tmp, sizeof tmp);
}

/* ------------------------------------------------------------------ config.json playlists */
static void resolve_playlist_item(const char *raw, char *out, size_t n)
{
    char tmp[PATH_MAX];
    const char *s = raw;
    if (!strncmp(s, "\\\\?\\", 4)) s += 4;
    snprintf(tmp, sizeof tmp, "%s", s);
    for (char *c = tmp; *c; c++)
        if (*c == '\\') *c = '/';
    const char *p = tmp;
    if (strlen(p) > 1 && p[1] == ':') p += 2;
    snprintf(out, n, "%s%s", *p == '/' ? "" : "/", p);
    if (is_file(out)) {
        char *slash = strrchr(out, '/');
        if (slash) *slash = 0;
    }
}

static int add_playlist_items(const cJSON *pl, char ***items)
{
    int count = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, js_get(pl, "items"))
    {
        if (!cJSON_IsString(it)) continue;
        char path[PATH_MAX];
        resolve_playlist_item(it->valuestring, path, sizeof path);
        if (!is_dir(path) && !is_file(path)) {
            /* paths written by Windows Wallpaper Engine: map workshop items and local projects */
            const char *ws = strstr(path, "/431960/");
            const char *mp = strstr(path, "/wallpaper_engine/");
            char id[64] = "", base[PATH_MAX], alt[PATH_MAX * 2];
            if (ws) {
                sscanf(ws + 8, "%63[^/]", id);
                if (we_resolve(id, alt, sizeof alt)) snprintf(path, sizeof path, "%s", alt);
            } else if (mp && we_find_install(base, sizeof base)) {
                snprintf(alt, sizeof alt, "%s/%s", base, mp + 18);
                char *pj = strstr(alt, "/project.json");
                if (pj) *pj = 0;
                if (is_dir(alt) || is_file(alt)) snprintf(path, sizeof path, "%s", alt);
            }
        }
        if (!is_dir(path) && !is_file(path)) {
            we_err("playlist item not found: %s", path);
            continue;
        }
        *items = realloc(*items, sizeof(char *) * (count + 1));
        (*items)[count++] = strdup(path);
    }
    return count;
}

int we_playlist(const char *name, char ***items, int *delay_minutes, bool *random)
{
    char base[PATH_MAX], path[PATH_MAX + 16];
    *items = NULL;
    if (!we_find_install(base, sizeof base)) {
        we_err("cannot locate the Wallpaper Engine installation to read config.json");
        return 0;
    }
    snprintf(path, sizeof path, "%s/config.json", base);
    size_t len;
    FILE *f = fopen(path, "rb");
    if (!f) {
        we_err("cannot open %s", path);
        return 0;
    }
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(len + 1);
    len = fread(buf, 1, len, f);
    buf[len] = 0;
    fclose(f);
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        we_err("cannot parse %s", path);
        return 0;
    }
    cJSON *user = js_get(root, "steamuser");
    const cJSON *found = NULL;
    const cJSON *pl;
    cJSON_ArrayForEach(pl, js_get(js_get(user, "general"), "playlists"))
    {
        if (!strcmp(js_str(pl, "name", ""), name)) found = pl;
    }
    if (!found) {
        const cJSON *entry;
        cJSON_ArrayForEach(entry, js_get(js_get(user, "wallpaperconfig"), "selectedwallpapers"))
        {
            const cJSON *p = js_get(entry, "playlist");
            if (p && (!strcmp(js_str(p, "name", ""), name) || !strcmp(entry->string, name))) found = p;
        }
    }
    int count = 0;
    if (found) {
        const cJSON *settings = js_get(found, "settings");
        *delay_minutes = js_int(settings, "delay", 60);
        *random = !strcmp(js_str(settings, "order", "sequential"), "random");
        count = add_playlist_items(found, items);
    } else {
        we_err("playlist '%s' not found in %s", name, path);
    }
    cJSON_Delete(root);
    return count;
}

/* ------------------------------------------------------------------ project.json */
static const char *VIRTUAL_BLOOM_EFFECT =
    "{\"name\":\"camerabloom_wpengine_linux\",\"group\":\"wpengine_linux_camera\",\"dependencies\":[],"
    "\"passes\":["
    "{\"material\":\"materials/util/downsample_quarter_bloom.json\",\"target\":\"_rt_4FrameBuffer\","
    "\"bind\":[{\"name\":\"_rt_FullFrameBuffer\",\"index\":0}]},"
    "{\"material\":\"materials/util/downsample_eighth_blur_v.json\",\"target\":\"_rt_8FrameBuffer\","
    "\"bind\":[{\"name\":\"_rt_4FrameBuffer\",\"index\":0}]},"
    "{\"material\":\"materials/util/blur_h_bloom.json\",\"target\":\"_rt_Bloom\","
    "\"bind\":[{\"name\":\"_rt_8FrameBuffer\",\"index\":0}]},"
    "{\"material\":\"materials/util/combine.json\",\"target\":\"_rt_FullFrameBuffer\","
    "\"bind\":[{\"name\":\"_rt_imageLayerComposite_-1_a\",\"index\":0},{\"name\":\"_rt_Bloom\",\"index\":1}]}"
    "]}";

static const char *VIRTUAL_MODEL = "{\"material\":\"materials/wpenginelinux.json\"}";
static const char *VIRTUAL_MATERIAL =
    "{\"passes\":[{\"blending\":\"normal\",\"cullmode\":\"nocull\",\"depthtest\":\"disabled\","
    "\"depthwrite\":\"disabled\",\"shader\":\"genericimage2\",\"textures\":[\"_rt_FullFrameBuffer\"]}]}";
static const char *VIRTUAL_COPY_FRAG =
    "uniform sampler2D g_Texture0;\n"
    "in vec2 v_TexCoord;\n"
    "void main () {\n"
    "out_FragColor = texture (g_Texture0, v_TexCoord);\n"
    "}";
static const char *VIRTUAL_COPY_VERT =
    "in vec3 a_Position;\n"
    "in vec2 a_TexCoord;\n"
    "out vec2 v_TexCoord;\n"
    "void main () {\n"
    "gl_Position = vec4 (a_Position, 1.0);\n"
    "v_TexCoord = a_TexCoord;\n"
    "}";

WeProject *project_load(const char *dir);
void       project_free(WeProject *p);

WeProject *project_load(const char *dir)
{
    WeProject *p = calloc(1, sizeof *p);
    snprintf(p->dir, sizeof p->dir, "%s", dir);
    p->vfs = vfs_new();
    vfs_mount(p->vfs, dir);
    char path[PATH_MAX + 32];
    snprintf(path, sizeof path, "%s/scene.pkg", dir);
    if (is_file(path)) vfs_mount(p->vfs, path);
    snprintf(path, sizeof path, "%s/gifscene.pkg", dir);
    if (is_file(path)) vfs_mount(p->vfs, path);
    char assets[PATH_MAX];
    static bool warned;
    if (we_find_assets(assets, sizeof assets)) {
        vfs_mount(p->vfs, assets);
        we_dbg("using Wallpaper Engine assets at %s", assets);
    } else if (!warned) {
        warned = true;
        we_err("Wallpaper Engine's 'assets' folder was not found. Most scene wallpapers need it: install "
               "Wallpaper Engine through Steam (Linux/Proton is fine), or copy its assets folder and pass "
               "--assets-dir PATH");
    }
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof cwd)) vfs_mount(p->vfs, cwd);

    vfs_add(p->vfs, "effects/wpenginelinux/bloomeffect.json", VIRTUAL_BLOOM_EFFECT, strlen(VIRTUAL_BLOOM_EFFECT));
    vfs_add(p->vfs, "models/wpenginelinux.json", VIRTUAL_MODEL, strlen(VIRTUAL_MODEL));
    vfs_add(p->vfs, "materials/wpenginelinux.json", VIRTUAL_MATERIAL, strlen(VIRTUAL_MATERIAL));
    vfs_add(p->vfs, "shaders/commands/copy.frag", VIRTUAL_COPY_FRAG, strlen(VIRTUAL_COPY_FRAG));
    vfs_add(p->vfs, "shaders/commands/copy.vert", VIRTUAL_COPY_VERT, strlen(VIRTUAL_COPY_VERT));

    char *json = (char *)vfs_read(p->vfs, "project.json", NULL);
    if (!json) {
        we_err("%s: no project.json", dir);
        project_free(p);
        return NULL;
    }
    p->json = cJSON_Parse(json);
    free(json);
    if (!p->json) {
        we_err("%s: cannot parse project.json", dir);
        project_free(p);
        return NULL;
    }
    char type[32];
    snprintf(type, sizeof type, "%s", js_str(p->json, "type", ""));
    for (char *c = type; *c; c++) *c = tolower((unsigned char)*c);
    p->type = !strcmp(type, "scene") ? WE_TYPE_SCENE
              : !strcmp(type, "video") ? WE_TYPE_VIDEO
              : !strcmp(type, "web") ? WE_TYPE_WEB
              : !strcmp(type, "application") ? WE_TYPE_APPLICATION
                                             : WE_TYPE_UNKNOWN;
    p->title = arena_strdup(&p->arena, js_str(p->json, "title", "untitled"));
    p->file = arena_strdup(&p->arena, js_str(p->json, "file", ""));
    const cJSON *wid = js_get(p->json, "workshopid");
    char idbuf[64] = "-1";
    if (wid && cJSON_IsNumber(wid)) snprintf(idbuf, sizeof idbuf, "%.0f", wid->valuedouble);
    else if (wid && cJSON_IsString(wid)) snprintf(idbuf, sizeof idbuf, "%s", wid->valuestring);
    p->workshop_id = arena_strdup(&p->arena, idbuf);
    const cJSON *general = js_get(p->json, "general");
    p->audio_processing = js_bool(general, "supportsaudioprocessing", false);
    const cJSON *pr;
    cJSON_ArrayForEach(pr, js_get(general, "properties"))
    {
        Property *prop = prop_parse(pr, pr->string);
        if (prop) VPUSH(p->props.list, prop);
    }
    return p;
}

void project_free(WeProject *p)
{
    if (!p) return;
    for (int i = 0; i < p->props.list.n; i++) prop_free(p->props.list.v[i]);
    VFREE(p->props.list);
    arena_free(&p->arena);
    cJSON_Delete(p->json);
    vfs_free(p->vfs);
    free(p);
}

static cJSON *read_json(WeProject *p, const char *file)
{
    char *data = (char *)vfs_read(p->vfs, file, NULL);
    if (!data) {
        we_err("cannot find %s", file);
        return NULL;
    }
    cJSON *j = cJSON_Parse(data);
    if (!j) we_err("cannot parse %s", file);
    free(data);
    return j;
}

/* ------------------------------------------------------------------ materials */
void texmap_parse(Arena *a, TexMap *m, const cJSON *arr)
{
    int idx = -1;
    const cJSON *it;
    cJSON_ArrayForEach(it, arr)
    {
        idx++;
        if (cJSON_IsNull(it)) continue;
        if (cJSON_IsObject(it)) {
            const char *n = js_str(it, "name", NULL);
            if (n) texmap_set(a, m, idx, arena_strdup(a, n));
        } else if (cJSON_IsString(it) && *it->valuestring) {
            texmap_set(a, m, idx, arena_strdup(a, it->valuestring));
        }
    }
}

ConstMap constants_parse(WeProject *p, const cJSON *obj)
{
    ConstMap m = { 0 };
    const cJSON *it;
    cJSON_ArrayForEach(it, obj)
    {
        Constant c = { arena_strdup(&p->arena, it->string), us_parse(&p->arena, it, &p->props, false) };
        AVPUSH(&p->arena, m, c);
    }
    return m;
}

static void combos_parse(Arena *a, ComboMap *m, const cJSON *obj)
{
    const cJSON *it;
    cJSON_ArrayForEach(it, obj)
    {
        combo_set(a, m, arena_strdup(a, it->string), (int)(cJSON_IsNumber(it) ? it->valuedouble : js_num(it, "value", 0)));
    }
}

Material *material_parse(WeProject *p, const cJSON *j, const char *file)
{
    Arena *a = &p->arena;
    Material *m = arena_alloc(a, sizeof *m);
    m->filename = arena_strdup(a, file);
    const cJSON *pj;
    cJSON_ArrayForEach(pj, js_get(j, "passes"))
    {
        MaterialPass *mp = arena_alloc(a, sizeof *mp);
        const char *bl = js_str(pj, "blending", "normal");
        mp->blending = !strcmp(bl, "additive") ? BLEND_ADDITIVE : !strcmp(bl, "translucent") ? BLEND_TRANSLUCENT
                                                                                             : BLEND_NORMAL;
        mp->cullmode = !strcmp(js_str(pj, "cullmode", "nocull"), "normal") ? CULL_NORMAL : CULL_NONE;
        mp->depthtest = !strcmp(js_str(pj, "depthtest", "disabled"), "enabled");
        mp->depthwrite = !strcmp(js_str(pj, "depthwrite", "disabled"), "enabled");
        mp->shader = arena_strdup(a, js_str(pj, "shader", ""));
        texmap_parse(a, &mp->textures, js_get(pj, "textures"));
        texmap_parse(a, &mp->usertextures, js_get(pj, "usertextures"));
        combos_parse(a, &mp->combos, js_get(pj, "combos"));
        mp->constants = constants_parse(p, js_get(pj, "constantshadervalues"));
        AVPUSH(a, m->passes, mp);
    }
    return m;
}

Material *material_load(WeProject *p, const char *file)
{
    cJSON *j = read_json(p, file);
    if (!j) return NULL;
    Material *m = material_parse(p, j, file);
    cJSON_Delete(j);
    return m;
}

WeEffect *effect_load(WeProject *p, const char *file)
{
    cJSON *j = read_json(p, file);
    if (!j) return NULL;
    Arena *a = &p->arena;
    WeEffect *e = arena_alloc(a, sizeof *e);
    e->name = arena_strdup(a, js_str(j, "name", ""));
    const cJSON *pj;
    cJSON_ArrayForEach(pj, js_get(j, "passes"))
    {
        EffectPass ep = { .command = -1 };
        const char *mat = js_str(pj, "material", NULL);
        if (mat) ep.material = material_load(p, mat);
        const cJSON *b;
        cJSON_ArrayForEach(b, js_get(pj, "bind"))
        {
            texmap_set(a, &ep.binds, js_int(b, "index", 0), arena_strdup(a, js_str(b, "name", "")));
        }
        const char *cmd = js_str(pj, "command", NULL);
        if (cmd) ep.command = !strcmp(cmd, "copy") ? 0 : 1;
        ep.source = arena_strdup(a, js_str(pj, "source", NULL));
        ep.target = arena_strdup(a, js_str(pj, "target", NULL));
        AVPUSH(a, e->passes, ep);
    }
    cJSON_ArrayForEach(pj, js_get(j, "fbos"))
    {
        EffectFbo f = { arena_strdup(a, js_str(pj, "name", "")), arena_strdup(a, js_str(pj, "format", "rgba8888")),
                        js_num(pj, "scale", 1.0), js_bool(pj, "unique", false) };
        AVPUSH(a, e->fbos, f);
    }
    cJSON_Delete(j);
    return e;
}

Model *model_load(WeProject *p, const char *file)
{
    cJSON *j = read_json(p, file);
    if (!j) return NULL;
    Arena *a = &p->arena;
    Model *m = arena_alloc(a, sizeof *m);
    m->filename = arena_strdup(a, file);
    const char *mat = js_str(j, "material", NULL);
    m->material = mat ? material_load(p, mat) : NULL;
    if (!m->material) {
        we_err("model %s has no usable material", file);
        cJSON_Delete(j);
        return NULL;
    }
    m->solidlayer = js_bool(j, "solidlayer", false);
    m->fullscreen = js_bool(j, "fullscreen", false);
    m->passthrough = js_bool(j, "passthrough", false);
    m->autosize = js_bool(j, "autosize", false);
    m->nopadding = js_bool(j, "nopadding", false);
    m->has_width = js_get(j, "width") != NULL;
    m->has_height = js_get(j, "height") != NULL;
    m->width = js_int(j, "width", 0);
    m->height = js_int(j, "height", 0);
    m->puppet = arena_strdup(a, js_str(j, "puppet", NULL));
    cJSON_Delete(j);
    return m;
}

PassOverride *override_parse(WeProject *p, const cJSON *j)
{
    Arena *a = &p->arena;
    PassOverride *o = arena_alloc(a, sizeof *o);
    o->id = js_int(j, "id", -1);
    combos_parse(a, &o->combos, js_get(j, "combos"));
    o->constants = constants_parse(p, js_get(j, "constantshadervalues"));
    texmap_parse(a, &o->textures, js_get(j, "textures"));
    texmap_parse(a, &o->usertextures, js_get(j, "usertextures"));
    return o;
}
