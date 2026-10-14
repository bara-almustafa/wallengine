/* X11 window / GLX context, monitor detection, fullscreen detection. */
#include "we.h"

#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/extensions/shape.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Atom atom(Display *d, const char *name) { return XInternAtom(d, name, False); }

static void make_click_through(XState *x)
{
    /* An empty input shape lets every click fall through to whatever is below. */
    int ev, er;
    if (XShapeQueryExtension(x->dpy, &ev, &er))
        XShapeCombineRectangles(x->dpy, x->win, ShapeInput, 0, 0, NULL, 0, ShapeSet, Unsorted);
}

static void set_desktop_hints(XState *x)
{
    Display *d = x->dpy;
    Atom type = atom(d, "_NET_WM_WINDOW_TYPE_DESKTOP");
    XChangeProperty(d, x->win, atom(d, "_NET_WM_WINDOW_TYPE"), XA_ATOM, 32,
                    PropModeReplace, (unsigned char *)&type, 1);

    Atom states[] = {
        atom(d, "_NET_WM_STATE_BELOW"),        atom(d, "_NET_WM_STATE_STICKY"),
        atom(d, "_NET_WM_STATE_SKIP_TASKBAR"), atom(d, "_NET_WM_STATE_SKIP_PAGER"),
    };
    XChangeProperty(d, x->win, atom(d, "_NET_WM_STATE"), XA_ATOM, 32,
                    PropModeReplace, (unsigned char *)states, 4);

    long all_desktops = 0xFFFFFFFF;
    XChangeProperty(d, x->win, atom(d, "_NET_WM_DESKTOP"), XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char *)&all_desktops, 1);

    /* Motif hints: no decorations. */
    long mwm[5] = { 2, 0, 0, 0, 0 };
    Atom mh = atom(d, "_MOTIF_WM_HINTS");
    XChangeProperty(d, x->win, mh, mh, 32, PropModeReplace, (unsigned char *)mwm, 5);
}

bool x_init(XState *x, WinMode mode, int ww, int wh)
{
    memset(x, 0, sizeof *x);
    x->dpy = XOpenDisplay(NULL);
    if (!x->dpy) {
        logmsg("cannot open X display (is $DISPLAY set? Wayland is not supported)");
        return false;
    }
    x->scr  = DefaultScreen(x->dpy);
    x->root = RootWindow(x->dpy, x->scr);
    x->sw   = DisplayWidth(x->dpy, x->scr);
    x->sh   = DisplayHeight(x->dpy, x->scr);
    XSelectInput(x->dpy, x->root, StructureNotifyMask);

    int attrs[] = { GLX_RGBA, GLX_DOUBLEBUFFER, GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8,
                    GLX_BLUE_SIZE, 8, None };
    XVisualInfo *vi = NULL;

    if (mode == MODE_ROOT) {
        /* Must use the root window's own visual. */
        XVisualInfo tmpl = { .visualid = XVisualIDFromVisual(DefaultVisual(x->dpy, x->scr)) };
        int n = 0;
        vi = XGetVisualInfo(x->dpy, VisualIDMask, &tmpl, &n);
        x->win = x->root;
        x->ww = x->sw;
        x->wh = x->sh;
    } else {
        vi = glXChooseVisual(x->dpy, x->scr, attrs);
        if (!vi) {
            logmsg("no suitable GLX visual");
            return false;
        }
        XSetWindowAttributes swa = { 0 };
        swa.colormap         = XCreateColormap(x->dpy, x->root, vi->visual, AllocNone);
        swa.event_mask       = StructureNotifyMask | ExposureMask;
        swa.background_pixel = BlackPixel(x->dpy, x->scr);
        x->ww = mode == MODE_WINDOW ? ww : x->sw;
        x->wh = mode == MODE_WINDOW ? wh : x->sh;
        x->win = XCreateWindow(x->dpy, x->root, 0, 0, x->ww, x->wh, 0, vi->depth, InputOutput,
                               vi->visual, CWColormap | CWEventMask | CWBackPixel, &swa);

        XStoreName(x->dpy, x->win, "wallengine");
        XClassHint ch = { "wallengine", "Wallengine" };
        XSetClassHint(x->dpy, x->win, &ch);

        if (mode == MODE_DESKTOP) {
            set_desktop_hints(x);
            make_click_through(x);
        } else {
            x->wm_delete = atom(x->dpy, "WM_DELETE_WINDOW");
            XSetWMProtocols(x->dpy, x->win, &x->wm_delete, 1);
        }
        XMapWindow(x->dpy, x->win);
        if (mode == MODE_WINDOW && cfg.win_pos) XMoveWindow(x->dpy, x->win, cfg.win_x, cfg.win_y);
        if (mode == MODE_DESKTOP) {
            XMoveResizeWindow(x->dpy, x->win, 0, 0, x->sw, x->sh);
            XLowerWindow(x->dpy, x->win);
        }
    }
    if (!vi) {
        logmsg("cannot get visual for root window");
        return false;
    }

    x->ctx = glXCreateContext(x->dpy, vi, NULL, True);
    XFree(vi);
    if (!x->ctx || !glXMakeCurrent(x->dpy, x->win, x->ctx)) {
        logmsg("cannot create OpenGL context");
        return false;
    }
    XSync(x->dpy, False);
    return true;
}

void x_shutdown(XState *x)
{
    if (!x->dpy) return;
    glXMakeCurrent(x->dpy, None, NULL);
    if (x->ctx) glXDestroyContext(x->dpy, x->ctx);
    if (x->win && x->win != x->root) XDestroyWindow(x->dpy, x->win);
    else if (x->win == x->root) XClearWindow(x->dpy, x->root);
    XCloseDisplay(x->dpy);
    x->dpy = NULL;
}

void x_poll(XState *x)
{
    while (XPending(x->dpy)) {
        XEvent e;
        XNextEvent(x->dpy, &e);
        if (e.type == ClientMessage && (Atom)e.xclient.data.l[0] == x->wm_delete) {
            x->closed = true;
        } else if (e.type == ConfigureNotify) {
            if (e.xconfigure.window == x->root &&
                (e.xconfigure.width != x->sw || e.xconfigure.height != x->sh)) {
                x->sw = e.xconfigure.width;
                x->sh = e.xconfigure.height;
                x->reconfigure = true;
            } else if (e.xconfigure.window == x->win && x->win != x->root &&
                       (e.xconfigure.width != x->ww || e.xconfigure.height != x->wh)) {
                x->ww = e.xconfigure.width;
                x->wh = e.xconfigure.height;
                x->reconfigure = true;
            }
        }
    }
}

int x_monitors(XState *x, Monitor *out, int max, WinMode mode, bool span)
{
    int n = 0;
    if (mode == MODE_WINDOW || span) {
        out[0] = (Monitor){ .x = 0, .y = 0, .w = x->ww, .h = x->wh };
        snprintf(out[0].name, sizeof out[0].name, "%s", span ? "span" : "window");
        return 1;
    }
    FILE *p = popen("xrandr --listmonitors 2>/dev/null", "r");
    if (p) {
        char line[512];
        while (fgets(line, sizeof line, p) && n < max) {
            int w, h, mx, my;
            char name[64];
            char *c = strchr(line, ':');
            if (!c || strncmp(line, "Monitors", 8) == 0) continue;
            if (sscanf(c + 1, " %*s %d/%*dx%d/%*d+%d+%d %63s", &w, &h, &mx, &my, name) == 5) {
                out[n] = (Monitor){ .x = mx, .y = my, .w = w, .h = h };
                snprintf(out[n].name, sizeof out[n].name, "%s", name);
                n++;
            }
        }
        pclose(p);
    }
    if (n == 0) {
        out[0] = (Monitor){ .x = 0, .y = 0, .w = x->sw, .h = x->sh };
        snprintf(out[0].name, sizeof out[0].name, "screen");
        n = 1;
    }
    return n;
}

static unsigned char *get_prop(Display *d, Window w, Atom prop, Atom type, unsigned long *n)
{
    Atom rt;
    int fmt;
    unsigned long after;
    unsigned char *data = NULL;
    if (XGetWindowProperty(d, w, prop, 0, 1024, False, type, &rt, &fmt, n, &after, &data) != Success)
        return NULL;
    if (!data || *n == 0) {
        if (data) XFree(data);
        return NULL;
    }
    return data;
}

static int ignore_xerror(Display *d, XErrorEvent *e) { (void)d; (void)e; return 0; }

/* If the focused window is fullscreen (or maximized), return its rectangle. */
bool x_fullscreen_rect(XState *x, bool maximized_too, int r[4])
{
    Display *d = x->dpy;
    unsigned long n;
    unsigned char *data = get_prop(d, x->root, atom(d, "_NET_ACTIVE_WINDOW"), XA_WINDOW, &n);
    if (!data) return false;
    Window active = *(Window *)data;
    XFree(data);
    if (!active || active == x->win) return false;

    int (*old)(Display *, XErrorEvent *) = XSetErrorHandler(ignore_xerror);
    bool hit = false;
    data = get_prop(d, active, atom(d, "_NET_WM_STATE"), XA_ATOM, &n);
    if (data) {
        Atom *st = (Atom *)data;
        Atom fs = atom(d, "_NET_WM_STATE_FULLSCREEN");
        Atom mv = atom(d, "_NET_WM_STATE_MAXIMIZED_VERT");
        Atom mh = atom(d, "_NET_WM_STATE_MAXIMIZED_HORZ");
        Atom hidden = atom(d, "_NET_WM_STATE_HIDDEN");
        bool is_fs = false, v = false, h = false, hid = false;
        for (unsigned long i = 0; i < n; i++) {
            if (st[i] == fs) is_fs = true;
            if (st[i] == mv) v = true;
            if (st[i] == mh) h = true;
            if (st[i] == hidden) hid = true;
        }
        XFree(data);
        hit = !hid && (is_fs || (maximized_too && v && h));
    }
    if (hit) {
        XWindowAttributes wa;
        Window child;
        int rx, ry;
        if (XGetWindowAttributes(d, active, &wa) &&
            XTranslateCoordinates(d, active, x->root, 0, 0, &rx, &ry, &child)) {
            r[0] = rx; r[1] = ry; r[2] = wa.width; r[3] = wa.height;
        } else {
            hit = false;
        }
    }
    XSync(d, False);
    XSetErrorHandler(old);
    return hit;
}

void x_pointer(XState *x, int *px, int *py, bool *down)
{
    Window rr, cr;
    int rx, ry, wx, wy;
    unsigned int mask;
    if (XQueryPointer(x->dpy, x->win, &rr, &cr, &rx, &ry, &wx, &wy, &mask)) {
        *px = wx;   /* relative to our drawable */
        *py = wy;
        *down = mask & Button1Mask;
    }
}

void x_swap(XState *x) { glXSwapBuffers(x->dpy, x->win); }

bool on_battery(void)
{
    DIR *dir = opendir("/sys/class/power_supply");
    if (!dir) return false;
    bool have_mains = false, online = false;
    struct dirent *e;
    while ((e = readdir(dir))) {
        if (e->d_name[0] == '.') continue;
        char path[512], buf[32] = { 0 };
        snprintf(path, sizeof path, "/sys/class/power_supply/%s/type", e->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        bool mains = fgets(buf, sizeof buf, f) && strncmp(buf, "Mains", 5) == 0;
        fclose(f);
        if (!mains) continue;
        have_mains = true;
        snprintf(path, sizeof path, "/sys/class/power_supply/%s/online", e->d_name);
        if ((f = fopen(path, "r"))) {
            if (fgetc(f) == '1') online = true;
            fclose(f);
        }
    }
    closedir(dir);
    return have_mains && !online;
}
