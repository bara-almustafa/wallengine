/* Web wallpapers: an offscreen WebKitGTK view rendered into a texture.
 * Built only when WebKitGTK development files are available (HAVE_WEBKIT). */
#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct WebView WebView;

#ifndef HAVE_WEBKIT

WebView *web_open(const char *html_path, int w, int h)
{
    (void)w;
    (void)h;
    we_err("%s: web wallpapers need WebKitGTK. Install libwebkit2gtk-4.1-dev and rebuild wallengine.", html_path);
    return NULL;
}
bool web_frame(WebView *v, GLuint tex, int *w, int *h, float mx, float my, bool down)
{
    (void)v; (void)tex; (void)w; (void)h; (void)mx; (void)my; (void)down;
    return false;
}
void web_pause(WebView *v, bool paused) { (void)v; (void)paused; }
void web_close(WebView *v) { (void)v; }
void web_set_properties(WebView *v, Properties *props) { (void)v; (void)props; }

#else

#include <gtk/gtk.h>
#include <webkit2/webkit2.h>

struct WebView {
    Properties    *props;
    bool           hooked, pushed;
    GtkWidget     *window;
    WebKitWebView *view;
    int            w, h;
    double         last_snap, last_audio;
    bool           loaded, paused;
};

static bool gtk_ready;

static void on_load(WebKitWebView *view, WebKitLoadEvent ev, gpointer user)
{
    WebView *v = user;
    if (ev != WEBKIT_LOAD_FINISHED) return;
    v->loaded = true;
    (void)view;
}

static void run_js(WebView *v, const char *js)
{
#if WEBKIT_CHECK_VERSION(2, 40, 0)
    webkit_web_view_evaluate_javascript(v->view, js, -1, NULL, NULL, NULL, NULL, NULL);
#else
    webkit_web_view_run_javascript(v->view, js, NULL, NULL, NULL);
#endif
}

static void push_properties(WebView *v, Properties *props)
{
    GString *js = g_string_new("(function(){ var p = {");
    for (int i = 0; props && i < props->list.n; i++) {
        Property *p = props->list.v[i];
        char buf[256];
        dv_to_string(&p->value, buf, sizeof buf);
        char *esc = g_strescape(p->type == PROP_COLOR ? buf : buf, NULL);
        if (p->type == PROP_COLOR) {
            char col[128];
            snprintf(col, sizeof col, "%g %g %g", p->value.v[0], p->value.v[1], p->value.v[2]);
            g_free(esc);
            esc = g_strescape(col, NULL);
        }
        if (p->type == PROP_SLIDER || p->type == PROP_BOOL)
            g_string_append_printf(js, "%s\"%s\": {\"value\": %s}", i ? "," : "", p->name,
                                   p->type == PROP_BOOL ? (p->value.b ? "true" : "false") : buf);
        else g_string_append_printf(js, "%s\"%s\": {\"value\": \"%s\"}", i ? "," : "", p->name, esc);
        g_free(esc);
    }
    g_string_append(js, "}; var l = window.wallpaperPropertyListener; if (l && l.applyUserProperties) "
                        "l.applyUserProperties(p); if (l && l.applyGeneralProperties) "
                        "l.applyGeneralProperties({fps: 30}); })();");
    run_js(v, js->str);
    g_string_free(js, TRUE);
}

WebView *web_open(const char *html_path, int w, int h)
{
    if (!gtk_ready) {
        if (!gtk_init_check(NULL, NULL)) {
            we_err("cannot initialise GTK for web wallpapers");
            return NULL;
        }
        gtk_ready = true;
    }
    WebView *v = calloc(1, sizeof *v);
    v->w = w;
    v->h = h;
    v->window = gtk_offscreen_window_new();
    gtk_window_set_default_size(GTK_WINDOW(v->window), w, h);
    v->view = WEBKIT_WEB_VIEW(webkit_web_view_new());
    WebKitSettings *s = webkit_web_view_get_settings(v->view);
    webkit_settings_set_enable_webgl(s, TRUE);
    webkit_settings_set_allow_file_access_from_file_urls(s, TRUE);
    webkit_settings_set_allow_universal_access_from_file_urls(s, TRUE);
    webkit_settings_set_hardware_acceleration_policy(s, WEBKIT_HARDWARE_ACCELERATION_POLICY_NEVER);
    webkit_settings_set_media_playback_requires_user_gesture(s, FALSE);
    g_signal_connect(v->view, "load-changed", G_CALLBACK(on_load), v);
    gtk_widget_set_size_request(GTK_WIDGET(v->view), w, h);
    gtk_container_add(GTK_CONTAINER(v->window), GTK_WIDGET(v->view));
    gtk_widget_show_all(v->window);
    char *uri = g_filename_to_uri(html_path, NULL, NULL);
    webkit_web_view_load_uri(v->view, uri);
    g_free(uri);
    return v;
}

static void push_audio(WebView *v)
{
    /* wallpaperRegisterAudioListener expects 128 values: 64 left + 64 right */
    GString *js = g_string_new("(function(){ var f = window.__weAudio; if (!f) return; f([");
    for (int k = 0; k < 2; k++)
        for (int i = 0; i < 64; i++) g_string_append_printf(js, "%s%.3f", k || i ? "," : "", g_Audio.audio64[i]);
    g_string_append(js, "]); })();");
    run_js(v, js->str);
    g_string_free(js, TRUE);
}

bool web_frame(WebView *v, GLuint tex, int *w, int *h, float mx, float my, bool down)
{
    (void)mx;
    (void)my;
    (void)down;
    while (g_main_context_iteration(NULL, FALSE)) {
    }
    double now = now_sec();
    if (v->loaded && !v->hooked) {
        v->hooked = true;
        run_js(v, "window.wallpaperRegisterAudioListener = function(cb){ window.__weAudio = cb; };"
                  "window.wallpaperRegisterMediaStatusListener = function(){};"
                  "window.wallpaperRegisterMediaPropertiesListener = function(){};"
                  "window.wallpaperRegisterMediaThumbnailListener = function(){};"
                  "window.wallpaperRegisterMediaPlaybackListener = function(){};"
                  "window.wallpaperRegisterMediaTimelineListener = function(){};");
    }
    if (v->loaded && v->hooked && !v->pushed) {
        v->pushed = true;
        push_properties(v, v->props);
    }
    if (v->loaded && now - v->last_audio > 1.0 / 30) {
        v->last_audio = now;
        push_audio(v);
    }
    if (now - v->last_snap < 1.0 / (cfg.fps > 0 ? cfg.fps : 30)) return false;
    v->last_snap = now;
    GdkPixbuf *pb = gtk_offscreen_window_get_pixbuf(GTK_OFFSCREEN_WINDOW(v->window));
    if (!pb) return false;
    int pw = gdk_pixbuf_get_width(pb), ph = gdk_pixbuf_get_height(pb), stride = gdk_pixbuf_get_rowstride(pb);
    int ch = gdk_pixbuf_get_n_channels(pb);
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, stride / ch);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, pw, ph, 0, ch == 4 ? GL_RGBA : GL_RGB, GL_UNSIGNED_BYTE,
                 gdk_pixbuf_get_pixels(pb));
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    g_object_unref(pb);
    *w = pw;
    *h = ph;
    return true;
}

void web_pause(WebView *v, bool paused) { v->paused = paused; }

void web_close(WebView *v)
{
    if (!v) return;
    gtk_widget_destroy(v->window);
    while (g_main_context_iteration(NULL, FALSE)) {
    }
    free(v);
}

void web_set_properties(WebView *v, Properties *props)
{
    v->props = props;
    v->pushed = false;   /* (re)sent on the next frame */
}

#endif
