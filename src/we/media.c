/* Now-playing information from MPRIS media players over D-Bus, plus album art
 * textures ($mediaThumbnail / $mediaPreviousThumbnail). */
#include "internal.h"

#include "../third_party/stb_image.h"

#include <dbus/dbus.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static DBusConnection *conn;
static MediaInfo       info;
static char            player[256];
static double          last_poll;
static Tex            *album[2];
static unsigned        album_serial;

static void parse_metadata(DBusMessageIter *variant)
{
    DBusMessageIter dict;
    dbus_message_iter_recurse(variant, &dict);
    bool changed = false, art = false;
    while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter entry, value;
        dbus_message_iter_recurse(&dict, &entry);
        const char *key = NULL;
        dbus_message_iter_get_basic(&entry, &key);
        dbus_message_iter_next(&entry);
        dbus_message_iter_recurse(&entry, &value);
        int t = dbus_message_iter_get_arg_type(&value);
        const char *str = NULL;
        if (t == DBUS_TYPE_STRING || t == DBUS_TYPE_OBJECT_PATH) dbus_message_iter_get_basic(&value, &str);
        if (key && !strcmp(key, "xesam:title") && str) {
            if (strcmp(info.title, str)) changed = true;
            snprintf(info.title, sizeof info.title, "%s", str);
        } else if (key && !strcmp(key, "xesam:album") && str) {
            if (strcmp(info.album, str)) changed = true;
            snprintf(info.album, sizeof info.album, "%s", str);
        } else if (key && !strcmp(key, "mpris:artUrl") && str) {
            if (strcmp(info.art_url, str)) {
                art = true;
                snprintf(info.art_url, sizeof info.art_url, "%s", str);
            }
        } else if (key && !strcmp(key, "xesam:artist") && t == DBUS_TYPE_ARRAY) {
            DBusMessageIter arr;
            dbus_message_iter_recurse(&value, &arr);
            if (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_STRING) {
                dbus_message_iter_get_basic(&arr, &str);
                if (strcmp(info.artist, str)) changed = true;
                snprintf(info.artist, sizeof info.artist, "%s", str);
            }
        } else if (key && !strcmp(key, "mpris:length")) {
            int64_t len = 0;
            if (t == DBUS_TYPE_INT64 || t == DBUS_TYPE_UINT64) dbus_message_iter_get_basic(&value, &len);
            if (info.duration != len / 1e6) changed = true;
            info.duration = len / 1e6;
        }
        dbus_message_iter_next(&dict);
    }
    if (changed || art) info.serial++;
    if (art) info.art_serial++;
}

static void parse_status(DBusMessageIter *variant)
{
    const char *st = NULL;
    if (dbus_message_iter_get_arg_type(variant) != DBUS_TYPE_STRING) return;
    dbus_message_iter_get_basic(variant, &st);
    int s = !strcmp(st, "Playing") ? 1 : !strcmp(st, "Paused") ? 2 : 0;
    if (s != info.state) {
        info.state = s;
        info.serial++;
    }
}

static DBusHandlerResult filter(DBusConnection *c, DBusMessage *m, void *user)
{
    (void)c;
    (void)user;
    if (!dbus_message_is_signal(m, "org.freedesktop.DBus.Properties", "PropertiesChanged"))
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    DBusMessageIter it, changed;
    if (!dbus_message_iter_init(m, &it)) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    const char *iface = NULL;
    dbus_message_iter_get_basic(&it, &iface);
    if (!iface || strcmp(iface, "org.mpris.MediaPlayer2.Player")) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    dbus_message_iter_next(&it);
    dbus_message_iter_recurse(&it, &changed);
    while (dbus_message_iter_get_arg_type(&changed) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter entry, value;
        dbus_message_iter_recurse(&changed, &entry);
        const char *key = NULL;
        dbus_message_iter_get_basic(&entry, &key);
        dbus_message_iter_next(&entry);
        dbus_message_iter_recurse(&entry, &value);
        if (key && !strcmp(key, "Metadata")) parse_metadata(&value);
        else if (key && !strcmp(key, "PlaybackStatus")) {
            parse_status(&value);
            const char *sender = dbus_message_get_sender(m);
            if (info.state == 1 && sender) snprintf(player, sizeof player, "%s", sender);
        }
        dbus_message_iter_next(&changed);
    }
    return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusMessage *get_prop(const char *dest, const char *prop)
{
    DBusMessage *msg = dbus_message_new_method_call(dest, "/org/mpris/MediaPlayer2",
                                                    "org.freedesktop.DBus.Properties", "Get");
    const char *iface = "org.mpris.MediaPlayer2.Player";
    dbus_message_append_args(msg, DBUS_TYPE_STRING, &iface, DBUS_TYPE_STRING, &prop, DBUS_TYPE_INVALID);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(conn, msg, 300, NULL);
    dbus_message_unref(msg);
    return reply;
}

static void detect_player(void)
{
    DBusMessage *msg = dbus_message_new_method_call("org.freedesktop.DBus", "/org/freedesktop/DBus",
                                                    "org.freedesktop.DBus", "ListNames");
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(conn, msg, 500, NULL);
    dbus_message_unref(msg);
    if (!reply) return;
    DBusMessageIter it, arr;
    char best[256] = "";
    if (dbus_message_iter_init(reply, &it) && dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_ARRAY) {
        dbus_message_iter_recurse(&it, &arr);
        while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_STRING) {
            const char *name;
            dbus_message_iter_get_basic(&arr, &name);
            if (!strncmp(name, "org.mpris.MediaPlayer2.", 23)) {
                if (!best[0]) snprintf(best, sizeof best, "%s", name);
                DBusMessage *st = get_prop(name, "PlaybackStatus");
                if (st) {
                    DBusMessageIter si, var;
                    if (dbus_message_iter_init(st, &si)) {
                        dbus_message_iter_recurse(&si, &var);
                        const char *s = NULL;
                        if (dbus_message_iter_get_arg_type(&var) == DBUS_TYPE_STRING) {
                            dbus_message_iter_get_basic(&var, &s);
                            if (s && !strcmp(s, "Playing")) snprintf(best, sizeof best, "%s", name);
                        }
                    }
                    dbus_message_unref(st);
                }
            }
            dbus_message_iter_next(&arr);
        }
    }
    dbus_message_unref(reply);
    if (!best[0]) return;
    snprintf(player, sizeof player, "%s", best);
    DBusMessage *md = get_prop(player, "Metadata");
    if (md) {
        DBusMessageIter mi, var;
        if (dbus_message_iter_init(md, &mi)) {
            dbus_message_iter_recurse(&mi, &var);
            if (dbus_message_iter_get_arg_type(&var) == DBUS_TYPE_ARRAY) parse_metadata(&var);
        }
        dbus_message_unref(md);
    }
    DBusMessage *st = get_prop(player, "PlaybackStatus");
    if (st) {
        DBusMessageIter si, var;
        if (dbus_message_iter_init(st, &si)) {
            dbus_message_iter_recurse(&si, &var);
            parse_status(&var);
        }
        dbus_message_unref(st);
    }
}

static void poll_position(void)
{
    if (!player[0]) return;
    DBusMessage *r = get_prop(player, "Position");
    if (!r) return;
    DBusMessageIter it, var;
    if (dbus_message_iter_init(r, &it)) {
        dbus_message_iter_recurse(&it, &var);
        int t = dbus_message_iter_get_arg_type(&var);
        if (t == DBUS_TYPE_INT64 || t == DBUS_TYPE_UINT64) {
            int64_t pos;
            dbus_message_iter_get_basic(&var, &pos);
            info.position = pos / 1e6;
        }
    }
    dbus_message_unref(r);
}

void media_init(void)
{
    DBusError err;
    dbus_error_init(&err);
    conn = dbus_bus_get_private(DBUS_BUS_SESSION, &err);
    if (!conn) {
        we_dbg("media info disabled: %s", err.message ? err.message : "no session bus");
        dbus_error_free(&err);
        return;
    }
    dbus_connection_set_exit_on_disconnect(conn, FALSE);
    dbus_connection_add_filter(conn, filter, NULL, NULL);
    dbus_bus_add_match(conn,
                       "type='signal',interface='org.freedesktop.DBus.Properties',member='PropertiesChanged',"
                       "path='/org/mpris/MediaPlayer2'",
                       NULL);
    dbus_connection_flush(conn);
    detect_player();
}

void media_update(void)
{
    if (!conn) return;
    dbus_connection_read_write(conn, 0);
    while (dbus_connection_dispatch(conn) == DBUS_DISPATCH_DATA_REMAINS) {
    }
    double now = now_sec();
    if (now - last_poll > 2.0) {
        last_poll = now;
        if (!player[0]) detect_player();
        poll_position();
    }
}

const MediaInfo *media_info(void) { return &info; }

void media_shutdown(void)
{
    if (conn) {
        dbus_connection_close(conn);
        dbus_connection_unref(conn);
        conn = NULL;
    }
    tex_free(album[0]);
    tex_free(album[1]);
    album[0] = album[1] = NULL;
}

static unsigned char *fetch_art(const char *url, int *w, int *h)
{
    int n;
    if (!strncmp(url, "file://", 7)) {
        char path[PATH_MAX];
        /* percent-decode */
        const char *s = url + 7;
        size_t o = 0;
        while (*s && o < sizeof path - 1) {
            if (*s == '%' && s[1] && s[2]) {
                char hex[3] = { s[1], s[2], 0 };
                path[o++] = (char)strtol(hex, NULL, 16);
                s += 3;
            } else {
                path[o++] = *s++;
            }
        }
        path[o] = 0;
        return stbi_load(path, w, h, &n, 4);
    }
    if (!strncmp(url, "http://", 7) || !strncmp(url, "https://", 8)) {
        const char *rt = getenv("XDG_RUNTIME_DIR");
        char tmp[PATH_MAX];
        snprintf(tmp, sizeof tmp, "%s/wallengine-art-%d", rt && *rt ? rt : "/tmp", (int)getpid());
        char cmd[PATH_MAX * 2];
        /* single quotes in the URL are percent-encoded to keep the shell command safe */
        char safe[2048];
        size_t o = 0;
        for (const char *s = url; *s && o < sizeof safe - 4; s++) {
            if (*s == '\'') {
                memcpy(safe + o, "%27", 3);
                o += 3;
            } else {
                safe[o++] = *s;
            }
        }
        safe[o] = 0;
        snprintf(cmd, sizeof cmd, "curl -sfL --max-time 5 -o '%s' '%s'", tmp, safe);
        if (system(cmd) != 0) return NULL;
        unsigned char *px = stbi_load(tmp, w, h, &n, 4);
        unlink(tmp);
        return px;
    }
    return NULL;
}

static Tex *make_album(void)
{
    Tex *t = calloc(1, sizeof *t);
    t->kind = TEXKIND_ALBUM;
    t->name = strdup("$mediaThumbnail");
    t->refs = 1;
    t->nids = 1;
    t->ids = calloc(1, sizeof(GLuint));
    t->img_w = calloc(1, sizeof(int));
    t->img_h = calloc(1, sizeof(int));
    glGenTextures(1, &t->ids[0]);
    glBindTexture(GL_TEXTURE_2D, t->ids[0]);
    unsigned char black[4] = { 0, 0, 0, 255 };
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, black);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    t->tex_w = t->tex_h = t->width = t->height = 1;
    t->img_w[0] = t->img_h[0] = 1;
    t->res[0] = t->res[1] = t->res[2] = t->res[3] = 1;
    return t;
}

Tex *media_album_texture(bool previous)
{
    if (!album[0]) {
        album[0] = make_album();
        album[1] = make_album();
        album[1]->name[1] = 'P';
    }
    if (album_serial != info.art_serial) {
        album_serial = info.art_serial;
        /* rotate current -> previous, then load the new artwork */
        Tex *tmp = album[1];
        album[1] = album[0];
        album[0] = tmp;
        int w, h;
        unsigned char *px = info.art_url[0] ? fetch_art(info.art_url, &w, &h) : NULL;
        if (px) {
            glBindTexture(GL_TEXTURE_2D, album[0]->ids[0]);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
            stbi_image_free(px);
            album[0]->tex_w = album[0]->width = album[0]->img_w[0] = w;
            album[0]->tex_h = album[0]->height = album[0]->img_h[0] = h;
            album[0]->res[0] = album[0]->res[2] = w;
            album[0]->res[1] = album[0]->res[3] = h;
        }
    }
    return album[previous ? 1 : 0];
}
