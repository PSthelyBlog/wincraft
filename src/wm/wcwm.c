/*
 * wcwm - the WinCraft window manager.
 *
 * Windows programs draw their own title bars and borders and move and size
 * themselves, so this window manager draws nothing at all. It does only what
 * the display server needs someone to decide:
 *
 *   - stacking, in layers:  menus/tooltips > active full-screen window >
 *                           taskbar and always-on-top windows >
 *                           ordinary windows > desktop
 *   - keyboard focus (click to focus, or focus follows the mouse when
 *     Windows' ActiveWindowTracking is on; new windows take the focus)
 *   - minimising (ICCCM iconify) and restoring
 *   - the work area: maximised windows stop above the taskbar
 *   - moving and sizing with the mouse once the program hands it over, and
 *     snapping: a window dragged to the left or right edge of the screen
 *     fills that half of the work area, to a corner that quarter, to the
 *     top edge it is maximised; Windows+Left/Right/Up/Down do the same from
 *     the keyboard
 *
 * It speaks just enough ICCCM/EWMH for the Windows runtime to run in its
 * normal "managed" mode. The WinCraft shell's own desktop and taskbar are
 * recognised by window class and title.
 *
 *   usage: wcwm [taskbar-height]       (default: $WINCRAFT_TASKBAR or 44)
 *
 * The shell passes on the preferences that concern it in
 * $XDG_RUNTIME_DIR/wcwm.conf ("key=value" lines: taskbar, position (top or
 * bottom), autohide, focus_follows_mouse, focus_delay), read again whenever it
 * changes.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/Xproto.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <X11/extensions/XInput2.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#define SHELL_CLASS     "wcshell.exe"
#define MAX_WINDOWS     1024

struct client {
    struct client *next;        /* stacking order: head of the list is on top */
    Window win;
    Window transient_for;
    int iconic;
    int ignore_unmap;           /* unmaps we caused ourselves */
    int input, take_focus;      /* ICCCM focus model */
    int above, fullscreen, maximized;   /* _NET_WM_STATE */
    int tray;                   /* the runtime's notification-area window */
    int has_saved;              /* geometry to return to when restored */
    int saved_x, saved_y, saved_w, saved_h;
    int functions;              /* what the program allows: MWM_FUNC_* */
    int snapped;                /* SNAP_*: the half or quarter of the work area it fills */
    int snap_x, snap_y, snap_w, snap_h;     /* where it was before */
};

/* Wine says in _MOTIF_WM_HINTS whether a window can be resized, minimised
 * and maximised. */
#define MWM_HINTS_FUNCTIONS 1
#define MWM_FUNC_ALL        1
#define MWM_FUNC_RESIZE     2
#define MWM_FUNC_MOVE       4
#define MWM_FUNC_MINIMIZE   8
#define MWM_FUNC_MAXIMIZE   16

enum {
    SNAP_NONE, SNAP_LEFT, SNAP_RIGHT,
    SNAP_TOPLEFT, SNAP_TOPRIGHT, SNAP_BOTTOMLEFT, SNAP_BOTTOMRIGHT,
    SNAP_MAXIMIZE
};

enum {
    WM_PROTOCOLS, WM_TAKE_FOCUS, WM_STATE, WM_CHANGE_STATE,
    NET_SUPPORTED, NET_SUPPORTING_WM_CHECK, NET_WM_NAME, NET_ACTIVE_WINDOW,
    NET_WORKAREA, NET_WM_STATE, NET_WM_STATE_FULLSCREEN, NET_WM_STATE_ABOVE,
    NET_WM_STATE_MAXIMIZED_VERT, NET_WM_STATE_MAXIMIZED_HORZ,
    NET_CLIENT_LIST, NET_NUMBER_OF_DESKTOPS, NET_CURRENT_DESKTOP,
    NET_DESKTOP_GEOMETRY, NET_DESKTOP_VIEWPORT, NET_WM_USER_TIME, NET_WM_MOVERESIZE,
    MOTIF_WM_HINTS, UTF8_STRING,
    NATOMS
};

static const char *atom_names[NATOMS] = {
    "WM_PROTOCOLS", "WM_TAKE_FOCUS", "WM_STATE", "WM_CHANGE_STATE",
    "_NET_SUPPORTED", "_NET_SUPPORTING_WM_CHECK", "_NET_WM_NAME", "_NET_ACTIVE_WINDOW",
    "_NET_WORKAREA", "_NET_WM_STATE", "_NET_WM_STATE_FULLSCREEN", "_NET_WM_STATE_ABOVE",
    "_NET_WM_STATE_MAXIMIZED_VERT", "_NET_WM_STATE_MAXIMIZED_HORZ",
    "_NET_CLIENT_LIST", "_NET_NUMBER_OF_DESKTOPS", "_NET_CURRENT_DESKTOP",
    "_NET_DESKTOP_GEOMETRY", "_NET_DESKTOP_VIEWPORT", "_NET_WM_USER_TIME", "_NET_WM_MOVERESIZE",
    "_MOTIF_WM_HINTS", "UTF8_STRING",
};

static Display *dpy;
static Window root, check_win;
static Atom atoms[NATOMS];
static struct client *clients, *focused;
static Window taskbar_win, desktop_win;

static int adopt_shell_window(Window win);
static Time last_time = CurrentTime;
static int taskbar_height = 44;      /* what the work area leaves for the taskbar */
static int bar_height = 44;          /* the taskbar's own height */
static int taskbar_top;              /* the taskbar at the top edge, not the bottom */
#define AREA_Y  (taskbar_top ? taskbar_height : 0)     /* where the work area starts */

/* Preferences from the shell (wcwm.conf). */
static int autohide, focus_follows_mouse;
static long focus_delay;            /* ms the pointer rests on a window before it gets the focus */
static struct client *pending_focus;
static long pending_due;
static int screen_w, screen_h;
static int other_wm;

/* A move or resize in progress (see on_moveresize), in one of the
 * _NET_WM_MOVERESIZE directions. */
enum {
    MR_SIZE_TOPLEFT, MR_SIZE_TOP, MR_SIZE_TOPRIGHT, MR_SIZE_RIGHT, MR_SIZE_BOTTOMRIGHT,
    MR_SIZE_BOTTOM, MR_SIZE_BOTTOMLEFT, MR_SIZE_LEFT, MR_MOVE, MR_SIZE_KEYBOARD,
    MR_MOVE_KEYBOARD, MR_CANCEL
};
static struct {
    struct client *c;
    int dir, button;            /* the mouse button held, 1 to 3, or 0 for none */
    int moving;                 /* the pointer went past the drag threshold */
    int px, py;                 /* the pointer where it started */
    int ox, oy, ow, oh;         /* the window where it started, for Esc */
    int snapped;                /* and the half it was snapped to */
    int x, y, w, h;             /* what the pointer's moves apply to */
    int cur_x, cur_y, cur_w, cur_h;
    int from_x, from_y, from_w, from_h;     /* where a snap goes back to */
    int min_w, min_h;           /* the smallest size the program accepted */
    int hedge, vedge;           /* keyboard sizing: the edges that move, -1 or 1 */
    int zone;                   /* SNAP_* where the pointer is */
} drag;
static Window outline[8];          /* the snap outline, dark bars then light */
static int xi_opcode = -1;
static void end_drag(int cancel);
static void on_moveresize(struct client *c, XClientMessageEvent *e);

/* ---------------------------------------------------------------- utils */

static int on_error(Display *d, XErrorEvent *e)
{
    /* Windows come and go under us all the time; that is never fatal. */
    if (e->error_code == BadAccess && e->request_code == X_ChangeWindowAttributes)
        other_wm = 1;
    return 0;
}

static struct client *find_client(Window win)
{
    struct client *c;

    for (c = clients; c; c = c->next)
        if (c->win == win)
            return c;
    return NULL;
}

static void set_cardinals(Window win, Atom prop, Atom type, const long *values, int count)
{
    XChangeProperty(dpy, win, prop, type, 32, PropModeReplace,
                    (const unsigned char *)values, count);
}

static void set_wm_state(Window win, long state)
{
    long data[2] = { state, None };

    XChangeProperty(dpy, win, atoms[WM_STATE], atoms[WM_STATE], 32, PropModeReplace,
                    (unsigned char *)data, 2);
}

static int get_cardinal(Window win, Atom prop, long *value)
{
    Atom type;
    int format, ok = 0;
    unsigned long count, remaining;
    unsigned char *data = NULL;

    if (XGetWindowProperty(dpy, win, prop, 0, 1, False, AnyPropertyType, &type, &format,
                           &count, &remaining, &data) == Success && data) {
        if (format == 32 && count >= 1) {
            *value = *(long *)data;
            ok = 1;
        }
        XFree(data);
    }
    return ok;
}

/* Is this one of the shell's own windows, e.g. its "Taskbar" (any of them
 * for a NULL title)? */
static int is_shell_window(Window win, const char *title)
{
    XClassHint hint = { NULL, NULL };
    char *name = NULL;
    int match = 0;

    if (XGetClassHint(dpy, win, &hint)) {
        if (hint.res_name && !strcmp(hint.res_name, SHELL_CLASS))
            match = !title || (XFetchName(dpy, win, &name) && name && !strcmp(name, title));
        if (hint.res_name) XFree(hint.res_name);
        if (hint.res_class) XFree(hint.res_class);
        if (name) XFree(name);
    }
    return match;
}

/*
 * The server's current time. Focus requests carrying an older timestamp
 * than the last focus change are silently dropped, so always ask.
 */
static Time server_time(void)
{
    XEvent ev;

    XChangeProperty(dpy, check_win, XA_WM_CLASS, XA_STRING, 8, PropModeAppend, NULL, 0);
    XWindowEvent(dpy, check_win, PropertyChangeMask, &ev);
    return ev.xproperty.time;
}

static void update_workarea(void)
{
    int w = screen_w, h = screen_h;
    long area[4] = { 0, AREA_Y, w, h - taskbar_height }, size[2] = { w, h }, origin[2] = { 0, 0 };

    set_cardinals(root, atoms[NET_WORKAREA], XA_CARDINAL, area, 4);
    set_cardinals(root, atoms[NET_DESKTOP_GEOMETRY], XA_CARDINAL, size, 2);
    set_cardinals(root, atoms[NET_DESKTOP_VIEWPORT], XA_CARDINAL, origin, 2);
}

static void update_client_list(void)
{
    Window list[MAX_WINDOWS];
    struct client *c;
    int n = 0;

    for (c = clients; c && n < MAX_WINDOWS; c = c->next)
        list[n++] = c->win;
    XChangeProperty(dpy, root, atoms[NET_CLIENT_LIST], XA_WINDOW, 32, PropModeReplace,
                    (unsigned char *)list, n);
}

/* ------------------------------------------------------------- stacking */

static void move_to_top(struct client *c)
{
    struct client **p;

    for (p = &clients; *p; p = &(*p)->next)
        if (*p == c) {
            *p = c->next;
            break;
        }
    c->next = clients;
    clients = c;
}

static void move_to_bottom(struct client *c)
{
    struct client **p;

    for (p = &clients; *p; p = &(*p)->next)
        if (*p == c) {
            *p = c->next;
            break;
        }
    for (p = &clients; *p; p = &(*p)->next)
        ;
    c->next = NULL;
    *p = c;
}

/* Raise a window, then the dialogs that belong to it, so they stay in front. */
static void raise_client(struct client *c, int depth)
{
    struct client *t, *list[64];
    int n = 0, i;

    move_to_top(c);
    if (depth > 8)
        return;
    for (t = clients; t && n < 64; t = t->next)
        if (t != c && t->transient_for == c->win)
            list[n++] = t;
    for (i = n - 1; i >= 0; i--)        /* keep their relative order */
        raise_client(list[i], depth + 1);
}

static int in_list(const Window *list, int n, Window win)
{
    int i;

    for (i = 0; i < n; i++)
        if (list[i] == win)
            return 1;
    return 0;
}

/* Recompute the whole stacking order and apply it. Cheap, and never stale. */
static void restack(void)
{
    Window order[MAX_WINDOWS], dummy, *children = NULL;
    unsigned int count = 0;
    struct client *c;
    int n = 0, i;

    /* 1. unmanaged pop-ups (menus, tooltips, drop-downs) keep their place on top */
    if (XQueryTree(dpy, root, &dummy, &dummy, &children, &count)) {
        for (i = (int)count - 1; i >= 0 && n < MAX_WINDOWS - 8; i--) {
            XWindowAttributes wa;
            Window w = children[i];

            if (w == taskbar_win || w == desktop_win || w == check_win || find_client(w))
                continue;
            if (!XGetWindowAttributes(dpy, w, &wa) || !wa.override_redirect ||
                wa.map_state != IsViewable || wa.class != InputOutput)
                continue;
            /* the shell's windows may only now have received their titles */
            if ((!taskbar_win || !desktop_win) && adopt_shell_window(w))
                continue;
            order[n++] = w;
        }
        if (children)
            XFree(children);
    }
    /* 2. the active window, if full-screen, covers the taskbar */
    if (focused && focused->fullscreen && !focused->iconic) {
        for (c = clients; c && n < MAX_WINDOWS - 8; c = c->next)
            if (c->transient_for == focused->win && !c->iconic)
                order[n++] = c->win;
        order[n++] = focused->win;
    }
    /* 3. always-on-top windows, then the taskbar */
    for (c = clients; c && n < MAX_WINDOWS - 8; c = c->next)
        if (c->above && !c->iconic && !in_list(order, n, c->win))
            order[n++] = c->win;
    if (taskbar_win)
        order[n++] = taskbar_win;
    /* 4. everything else, most recently raised first */
    for (c = clients; c && n < MAX_WINDOWS - 8; c = c->next)
        if (!c->iconic && !in_list(order, n, c->win))
            order[n++] = c->win;
    /* 5. the desktop */
    if (desktop_win)
        order[n++] = desktop_win;

    if (n > 1)
        XRestackWindows(dpy, order, n);
    else if (n == 1)
        XRaiseWindow(dpy, order[0]);
}

/* ---------------------------------------------------------------- focus */

static void send_take_focus(Window win)
{
    XEvent ev;

    memset(&ev, 0, sizeof(ev));
    ev.xclient.type = ClientMessage;
    ev.xclient.window = win;
    ev.xclient.message_type = atoms[WM_PROTOCOLS];
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = atoms[WM_TAKE_FOCUS];
    ev.xclient.data.l[1] = last_time;
    XSendEvent(dpy, win, False, NoEventMask, &ev);
}

static void focus_client(struct client *c)
{
    Window active = c ? c->win : None;

    focused = c;
    last_time = server_time();
    if (c) {
        if (c->input)
            XSetInputFocus(dpy, c->win, RevertToPointerRoot, last_time);
        if (c->take_focus)
            send_take_focus(c->win);
    } else {
        /* Nothing to focus: keys go to whatever is under the pointer (the shell). */
        XSetInputFocus(dpy, PointerRoot, RevertToPointerRoot, CurrentTime);
    }
    XChangeProperty(dpy, root, atoms[NET_ACTIVE_WINDOW], XA_WINDOW, 32, PropModeReplace,
                    (unsigned char *)&active, 1);
}

static void activate(struct client *c)
{
    raise_client(c, 0);
    focus_client(c);
    restack();
}

/* Focus the top-most remaining window after the focused one went away. */
static void focus_next(void)
{
    struct client *c;

    for (c = clients; c; c = c->next)
        if (!c->iconic && !c->above)
            break;
    if (!c)
        for (c = clients; c; c = c->next)
            if (!c->iconic)
                break;
    if (c)
        activate(c);
    else {
        focus_client(NULL);
        restack();
    }
}

/* --------------------------------------------------------------- clients */

static void read_hints(struct client *c)
{
    XWMHints *hints = XGetWMHints(dpy, c->win);
    Atom *protocols = NULL;
    int n = 0, i;

    c->input = 1;
    if (hints) {
        if (hints->flags & InputHint)
            c->input = hints->input;
        XFree(hints);
    }
    c->take_focus = 0;
    if (XGetWMProtocols(dpy, c->win, &protocols, &n)) {
        for (i = 0; i < n; i++)
            if (protocols[i] == atoms[WM_TAKE_FOCUS])
                c->take_focus = 1;
        XFree(protocols);
    }
    c->transient_for = None;
    XGetTransientForHint(dpy, c->win, &c->transient_for);
}

static void read_functions(struct client *c)
{
    Atom type;
    int format;
    unsigned long count = 0, remaining;
    unsigned char *data = NULL;

    c->functions = MWM_FUNC_RESIZE | MWM_FUNC_MOVE | MWM_FUNC_MINIMIZE | MWM_FUNC_MAXIMIZE;
    if (XGetWindowProperty(dpy, c->win, atoms[MOTIF_WM_HINTS], 0, 5, False, AnyPropertyType, &type,
                           &format, &count, &remaining, &data) == Success && data) {
        const long *hints = (const long *)data;

        if (format == 32 && count >= 2 && (hints[0] & MWM_HINTS_FUNCTIONS))
            c->functions = (hints[1] & MWM_FUNC_ALL) ? c->functions & ~hints[1] : hints[1];
        XFree(data);
    }
}

static void read_net_state(struct client *c)
{
    Atom type, *states = NULL;
    int format;
    unsigned long count = 0, remaining, i;

    c->above = c->fullscreen = c->maximized = 0;
    if (XGetWindowProperty(dpy, c->win, atoms[NET_WM_STATE], 0, 32, False, XA_ATOM, &type,
                           &format, &count, &remaining, (unsigned char **)&states) == Success &&
        states) {
        for (i = 0; type == XA_ATOM && format == 32 && i < count; i++) {
            if (states[i] == atoms[NET_WM_STATE_ABOVE]) c->above = 1;
            if (states[i] == atoms[NET_WM_STATE_FULLSCREEN]) c->fullscreen = 1;
            if (states[i] == atoms[NET_WM_STATE_MAXIMIZED_VERT] ||
                states[i] == atoms[NET_WM_STATE_MAXIMIZED_HORZ]) c->maximized = 1;
        }
        XFree(states);
    }
}

static void write_net_state(struct client *c)
{
    Atom states[4];
    int n = 0;

    if (c->above) states[n++] = atoms[NET_WM_STATE_ABOVE];
    if (c->fullscreen) states[n++] = atoms[NET_WM_STATE_FULLSCREEN];
    if (c->maximized) {
        states[n++] = atoms[NET_WM_STATE_MAXIMIZED_VERT];
        states[n++] = atoms[NET_WM_STATE_MAXIMIZED_HORZ];
    }
    XChangeProperty(dpy, c->win, atoms[NET_WM_STATE], XA_ATOM, 32, PropModeReplace,
                    (unsigned char *)states, n);
}

/*
 * Size a window for its state: full-screen covers the screen, maximised
 * fills the work area, otherwise it returns to where it was.
 */
static void apply_state_geometry(struct client *c)
{
    XWindowAttributes wa;

    if (c->fullscreen || c->maximized) {
        if (!c->has_saved && XGetWindowAttributes(dpy, c->win, &wa)) {
            c->saved_x = wa.x;
            c->saved_y = wa.y;
            c->saved_w = wa.width;
            c->saved_h = wa.height;
            c->has_saved = 1;
        }
        XMoveResizeWindow(dpy, c->win, 0, c->fullscreen ? 0 : AREA_Y, screen_w,
                          c->fullscreen ? screen_h : screen_h - taskbar_height);
    } else if (c->has_saved) {
        c->has_saved = 0;
        XMoveResizeWindow(dpy, c->win, c->saved_x, c->saved_y, c->saved_w, c->saved_h);
    }
}

/*
 * Maximising and restoring here rather than at the program's request: Wine
 * follows the window's state, and a window restored this way goes back to
 * where it is at that moment. So the state changes before the move when
 * maximising, after it when restoring.
 */
static void maximize(struct client *c)
{
    c->maximized = 1;
    write_net_state(c);
    apply_state_geometry(c);
}

static void restore(struct client *c)
{
    c->maximized = 0;
    apply_state_geometry(c);
    write_net_state(c);
}

/* The side of the screen a half or quarter is on, and the quarter of a side. */
static int snap_side(int zone)
{
    return zone == SNAP_RIGHT || zone == SNAP_TOPRIGHT || zone == SNAP_BOTTOMRIGHT ? SNAP_RIGHT : SNAP_LEFT;
}

static int snap_quarter(int side, int lower)
{
    if (side == SNAP_LEFT)
        return lower ? SNAP_BOTTOMLEFT : SNAP_TOPLEFT;
    return lower ? SNAP_BOTTOMRIGHT : SNAP_TOPRIGHT;
}

/* The part of the work area a snapped window fills: a half or a quarter, or
 * all of it for SNAP_MAXIMIZE. */
static void snap_area(int zone, int *x, int *y, int *w, int *h)
{
    int area_h = screen_h - taskbar_height, left = screen_w / 2, top = area_h / 2;

    *x = 0;
    *y = AREA_Y;
    *w = screen_w;
    *h = area_h;
    if (zone == SNAP_MAXIMIZE)
        return;
    if (snap_side(zone) == SNAP_RIGHT) {
        *x = left;
        *w = screen_w - left;
    } else {
        *w = left;
    }
    if (zone == SNAP_TOPLEFT || zone == SNAP_TOPRIGHT) {
        *h = top;
    } else if (zone == SNAP_BOTTOMLEFT || zone == SNAP_BOTTOMRIGHT) {
        *y = AREA_Y + top;
        *h = area_h - top;
    }
}

/* Whether a window still fills the part it was snapped to: moved or sized
 * since, it no longer counts as snapped. */
static int still_snapped(struct client *c)
{
    XWindowAttributes wa;
    int x, y, w, h;

    if (c->snapped && !c->maximized) {
        snap_area(c->snapped, &x, &y, &w, &h);
        if (!XGetWindowAttributes(dpy, c->win, &wa) ||
            wa.x != x || wa.y != y || wa.width != w || wa.height != h)
            c->snapped = 0;
    }
    return c->snapped;
}

/* Snap a window to a half or a quarter of the work area. Where it was before
 * (x, y, w, h) is kept for going back, unless it was snapped already. */
static void snap(struct client *c, int zone, int x, int y, int w, int h)
{
    if (!still_snapped(c)) {
        c->snap_x = x;
        c->snap_y = y;
        c->snap_w = w;
        c->snap_h = h;
    }
    c->snapped = zone;
    snap_area(zone, &x, &y, &w, &h);
    XMoveResizeWindow(dpy, c->win, x, y, w, h);
    if (c->maximized) {
        c->maximized = 0;
        c->has_saved = 0;
        write_net_state(c);
    }
}

static void unsnap(struct client *c)
{
    c->snapped = 0;
    XMoveResizeWindow(dpy, c->win, c->snap_x, c->snap_y, c->snap_w, c->snap_h);
}

static struct client *manage(Window win)
{
    struct client *c = calloc(1, sizeof(*c));

    if (!c)
        return NULL;
    c->win = win;
    c->next = clients;
    clients = c;
    XSelectInput(dpy, win, PropertyChangeMask | (focus_follows_mouse ? EnterWindowMask : 0));
    /* Click to focus: see the press first, then let the program have it. */
    XGrabButton(dpy, AnyButton, AnyModifier, win, False, ButtonPressMask,
                GrabModeSync, GrabModeAsync, None, None);
    read_hints(c);
    read_functions(c);
    read_net_state(c);
    update_client_list();
    return c;
}

static void unmanage(struct client *c)
{
    struct client **p;
    int was_focused = c == focused;

    if (drag.c == c) {
        drag.c = NULL;
        end_drag(0);
    }
    for (p = &clients; *p; p = &(*p)->next)
        if (*p == c) {
            *p = c->next;
            break;
        }
    if (was_focused)
        focused = NULL;
    if (pending_focus == c)
        pending_focus = NULL;
    free(c);
    update_client_list();
    if (was_focused)
        focus_next();
    else
        restack();
}

static void map_client(struct client *c, int take_focus)
{
    c->iconic = 0;
    XMapWindow(dpy, c->win);
    set_wm_state(c->win, NormalState);
    if (take_focus)
        activate(c);
    else {
        raise_client(c, 0);
        restack();
    }
}

static void iconify(struct client *c)
{
    if (c->iconic)
        return;
    c->iconic = 1;
    c->ignore_unmap++;
    XUnmapWindow(dpy, c->win);
    set_wm_state(c->win, IconicState);
    move_to_bottom(c);
    if (c == focused) {
        focused = NULL;
        focus_next();
    } else {
        restack();
    }
}

/* --------------------------------------------------------------- events */

/*
 * The shell's desktop and taskbar are part of the furniture, not programs:
 * they have a fixed place in the stack and never take the focus.
 */
static int adopt_shell_window(Window win)
{
    if (is_shell_window(win, "Taskbar"))
        taskbar_win = win;
    else if (is_shell_window(win, "Desktop"))
        desktop_win = win;
    else
        return 0;
    return 1;
}

/*
 * Notification-area ("tray") icons are the taskbar's own, but if Wine's
 * floating tray window (its explorer process's) is turned back on, dock it
 * in the corner by the clock: above it, or below it when the taskbar is at
 * the top.
 */
static int is_tray_window(Window win)
{
    XClassHint hint = { NULL, NULL };
    char *name = NULL;
    int match = 0;

    if (XGetClassHint(dpy, win, &hint)) {
        match = hint.res_name && !strcmp(hint.res_name, "explorer.exe") &&
                !(XFetchName(dpy, win, &name) && name && name[0]);
        if (hint.res_name) XFree(hint.res_name);
        if (hint.res_class) XFree(hint.res_class);
        if (name) XFree(name);
    }
    return match;
}

static void dock_tray(struct client *c, int width, int height)
{
    XMoveResizeWindow(dpy, c->win, screen_w - width - 6,
                      taskbar_top ? taskbar_height + 6 : screen_h - taskbar_height - height - 6,
                      width, height);
}

/*
 * Programs that do not care where their window goes ask for the top-left
 * corner. Give those a cascading position instead, as Windows does.
 */
static void place_new_window(struct client *c)
{
    static int slot;
    XWindowAttributes wa;
    int x, y;

    if (!XGetWindowAttributes(dpy, c->win, &wa))
        return;
    if (is_tray_window(c->win)) {
        c->tray = c->above = 1;
        dock_tray(c, wa.width, wa.height);
        return;
    }
    if (c->transient_for || c->fullscreen || c->maximized || is_shell_window(c->win, NULL))
        return;
    /* the corner of the screen or of the work area */
    if (wa.x != 0 || (wa.y != 0 && wa.y != AREA_Y) || wa.width >= screen_w ||
        wa.height >= screen_h - taskbar_height)
        return;
    x = 40 + 30 * (slot++ % 8);
    y = AREA_Y + x;
    if (x + wa.width > screen_w)
        x = screen_w - wa.width;
    if (y + wa.height > AREA_Y + screen_h - taskbar_height)
        y = AREA_Y + screen_h - taskbar_height - wa.height;
    XMoveWindow(dpy, c->win, x < 0 ? 0 : x, y < AREA_Y ? AREA_Y : y);
}

static void on_map_request(XMapRequestEvent *e)
{
    struct client *c = find_client(e->window);
    long user_time;
    int take_focus = 1, is_new = !c;

    if (!c && adopt_shell_window(e->window)) {
        XMapWindow(dpy, e->window);
        set_wm_state(e->window, NormalState);
        restack();
        return;
    }
    if (!c && !(c = manage(e->window)))
        return;
    read_hints(c);
    if (!c->iconic)
        read_net_state(c);
    /* A program can ask not to be focused when it appears. */
    if (get_cardinal(c->win, atoms[NET_WM_USER_TIME], &user_time) && user_time == 0)
        take_focus = 0;
    if (is_new)
        place_new_window(c);
    if (c->tray)
        take_focus = 0;
    apply_state_geometry(c);
    map_client(c, take_focus);
}

static void on_unmap(XUnmapEvent *e)
{
    struct client *c = find_client(e->window);

    if (e->window == taskbar_win) taskbar_win = None;
    if (e->window == desktop_win) desktop_win = None;
    if (!c)
        return;
    if (!e->send_event && c->ignore_unmap > 0) {
        c->ignore_unmap--;
        return;
    }
    /* The program withdrew its window. */
    XDeleteProperty(dpy, c->win, atoms[WM_STATE]);
    unmanage(c);
}

static void on_map_notify(XMapEvent *e)
{
    if (!e->override_redirect || find_client(e->window))
        return;
    adopt_shell_window(e->window);
    restack();
}

static void on_configure_request(XConfigureRequestEvent *e)
{
    struct client *c = find_client(e->window);
    XWindowChanges changes;
    unsigned long mask = e->value_mask & (CWX | CWY | CWWidth | CWHeight | CWBorderWidth);

    changes.x = e->x;
    changes.y = e->y;
    changes.width = e->width;
    changes.height = e->height;
    changes.border_width = e->border_width;
    if (!c) {
        /* not mapped yet: grant everything, stacking included */
        changes.sibling = e->above;
        changes.stack_mode = e->detail;
        XConfigureWindow(dpy, e->window, e->value_mask, &changes);
        return;
    }
    {
        XWindowAttributes wa;
        XEvent ev;

        if (!XGetWindowAttributes(dpy, c->win, &wa))
            return;
        if (c->tray) {      /* it may grow and shrink, but it stays docked */
            dock_tray(c, (mask & CWWidth) ? e->width : wa.width,
                      (mask & CWHeight) ? e->height : wa.height);
            return;
        }
        if (c == drag.c) {
            /* sizing it smaller than the program will go: that is its minimum */
            if (drag.dir != MR_MOVE && drag.dir != MR_MOVE_KEYBOARD) {
                if ((mask & CWWidth) && e->width > drag.cur_w) drag.min_w = e->width;
                if ((mask & CWHeight) && e->height > drag.cur_h) drag.min_h = e->height;
            }
            if (mask & CWX) drag.cur_x = e->x;
            if (mask & CWY) drag.cur_y = e->y;
            if (mask & CWWidth) drag.cur_w = e->width;
            if (mask & CWHeight) drag.cur_h = e->height;
        }
        if (((mask & CWX) && e->x != wa.x) || ((mask & CWY) && e->y != wa.y) ||
            ((mask & CWWidth) && e->width != wa.width) ||
            ((mask & CWHeight) && e->height != wa.height) ||
            ((mask & CWBorderWidth) && e->border_width != wa.border_width)) {
            XConfigureWindow(dpy, c->win, mask, &changes);
        } else {
            /* ICCCM: a request that changes nothing still gets an answer */
            memset(&ev, 0, sizeof(ev));
            ev.xconfigure.type = ConfigureNotify;
            ev.xconfigure.event = ev.xconfigure.window = c->win;
            ev.xconfigure.x = wa.x;
            ev.xconfigure.y = wa.y;
            ev.xconfigure.width = wa.width;
            ev.xconfigure.height = wa.height;
            ev.xconfigure.border_width = wa.border_width;
            XSendEvent(dpy, c->win, False, StructureNotifyMask, &ev);
        }
    }
    if ((e->value_mask & CWStackMode) && !(e->value_mask & CWSibling)) {
        if (e->detail == Above)
            raise_client(c, 0);
        else if (e->detail == Below)
            move_to_bottom(c);
        restack();
    }
}

static void on_client_message(XClientMessageEvent *e)
{
    struct client *c = find_client(e->window);

    if (!c)
        return;
    if (e->message_type == atoms[WM_CHANGE_STATE]) {
        if (e->data.l[0] == IconicState)
            iconify(c);
    } else if (e->message_type == atoms[NET_ACTIVE_WINDOW]) {
        if (e->data.l[1])
            last_time = e->data.l[1];
        if (c->iconic) {
            c->iconic = 0;
            XMapWindow(dpy, c->win);
            set_wm_state(c->win, NormalState);
        }
        activate(c);
    } else if (e->message_type == atoms[NET_WM_STATE]) {
        int i;

        int maximize_seen = 0;

        for (i = 1; i <= 2; i++) {
            Atom state = e->data.l[i];
            int *flag = state == atoms[NET_WM_STATE_ABOVE] ? &c->above :
                        state == atoms[NET_WM_STATE_FULLSCREEN] ? &c->fullscreen :
                        (state == atoms[NET_WM_STATE_MAXIMIZED_VERT] ||
                         state == atoms[NET_WM_STATE_MAXIMIZED_HORZ]) ? &c->maximized : NULL;

            /* the two maximise atoms arrive together and count once */
            if (!flag || (flag == &c->maximized && maximize_seen++))
                continue;
            switch (e->data.l[0]) {
            case 0: *flag = 0; break;
            case 1: *flag = 1; break;
            case 2: *flag = !*flag; break;
            }
        }
        apply_state_geometry(c);
        write_net_state(c);
        restack();
    } else if (e->message_type == atoms[NET_WM_MOVERESIZE]) {
        on_moveresize(c, e);
    }
}

static void on_button_press(XButtonEvent *e)
{
    struct client *c = find_client(e->window);

    last_time = e->time;
    if (c && (c != focused || clients != c))
        activate(c);
    XAllowEvents(dpy, ReplayPointer, e->time);
}

static void on_property(XPropertyEvent *e)
{
    struct client *c;

    last_time = e->time;
    if (e->window == root || !(c = find_client(e->window)))
        return;
    if (e->atom == XA_WM_HINTS || e->atom == atoms[WM_PROTOCOLS] ||
        e->atom == XA_WM_TRANSIENT_FOR)
        read_hints(c);
    else if (e->atom == atoms[MOTIF_WM_HINTS])
        read_functions(c);
}

/* -------------------------------------------------------- move and size */

/*
 * Wine hands title-bar and border drags over to the window manager with
 * _NET_WM_MOVERESIZE, as on any Linux desktop, and waits for the mouse
 * button to come up. A move that ends with the pointer on the left or right
 * edge of the screen snaps the window to that half of the work area, in a
 * corner to that quarter; on the top edge, it maximises the window. An
 * outline shows where it will go. The window menu's Move and Size, done with
 * the keyboard, come here too.
 */
#define DRAG_THRESHOLD  4       /* pixels the pointer moves before a press becomes a drag */
#define KEY_STEP        8       /* pixels per arrow key, moving or sizing with the keyboard */
#define TITLE_KEEP      32      /* pixels of a moved window kept above the taskbar */
#define SNAP_EDGE       8       /* pixels from an edge of the screen that count as on it */
#define SNAP_CORNER     64      /* pixels along an edge from a corner that snap to its quarter */
#define OUTLINE_WIDTH   6
#define OUTLINE_INSET   8       /* from the edges of the area the window will fill */

static int drag_keyboard(void)
{
    return drag.dir == MR_SIZE_KEYBOARD || drag.dir == MR_MOVE_KEYBOARD;
}

/* The outline: a dark frame round a light one, as bars (nothing here draws
 * translucently). */
static void show_outline(int zone)
{
    static const char *colors[2] = { "#14284d", "#dce8ff" };
    int x, y, w, h, i, t = OUTLINE_WIDTH;

    if (zone == SNAP_NONE) {
        for (i = 0; i < 8; i++)
            if (outline[i])
                XUnmapWindow(dpy, outline[i]);
        return;
    }
    if (!outline[0]) {
        XSetWindowAttributes swa;
        XColor color, exact;
        Colormap cmap = DefaultColormap(dpy, DefaultScreen(dpy));

        swa.override_redirect = True;
        for (i = 0; i < 8; i++) {
            swa.background_pixel = i < 4 ? BlackPixel(dpy, DefaultScreen(dpy)) : WhitePixel(dpy, DefaultScreen(dpy));
            if (XAllocNamedColor(dpy, cmap, colors[i / 4], &color, &exact))
                swa.background_pixel = color.pixel;
            outline[i] = XCreateWindow(dpy, root, 0, 0, 1, 1, 0, CopyFromParent, InputOutput,
                                       CopyFromParent, CWOverrideRedirect | CWBackPixel, &swa);
        }
    }
    snap_area(zone, &x, &y, &w, &h);
    x += OUTLINE_INSET;
    y += OUTLINE_INSET;
    w -= 2 * OUTLINE_INSET;
    h -= 2 * OUTLINE_INSET;
    /* the dark bars, then the light ones a pixel inside them */
    for (i = 0; i < 8; i += 4, x++, y++, w -= 2, h -= 2, t -= 2) {
        XMoveResizeWindow(dpy, outline[i], x, y, w, t);
        XMoveResizeWindow(dpy, outline[i + 1], x, y + h - t, w, t);
        XMoveResizeWindow(dpy, outline[i + 2], x, y, t, h);
        XMoveResizeWindow(dpy, outline[i + 3], x + w - t, y, t, h);
    }
    for (i = 0; i < 8; i++)
        XMapRaised(dpy, outline[i]);
}

/* Which snap the pointer's place on the screen's edges calls for: a corner
 * (on an edge, SNAP_CORNER pixels or less from the corner) that quarter, the
 * left or right edge that half, the top edge the whole work area. The edges
 * are SNAP_EDGE pixels wide: in a virtual machine's window the pointer may
 * stop short of the last pixel (QEMU's own window never reaches the right
 * and bottom ones), or leave the window before it gets there. */
static int zone_at(struct client *c, int px, int py)
{
    int left = px < SNAP_EDGE, right = px >= screen_w - SNAP_EDGE;
    int top = py < SNAP_EDGE, bottom = py >= screen_h - SNAP_EDGE;
    int near_left = px < SNAP_CORNER, near_right = px >= screen_w - SNAP_CORNER;
    int near_top = py < SNAP_CORNER, near_bottom = py >= screen_h - SNAP_CORNER;

    if (c->functions & MWM_FUNC_RESIZE) {
        if ((left && near_top) || (top && near_left))
            return SNAP_TOPLEFT;
        if ((right && near_top) || (top && near_right))
            return SNAP_TOPRIGHT;
        if ((left && near_bottom) || (bottom && near_left))
            return SNAP_BOTTOMLEFT;
        if ((right && near_bottom) || (bottom && near_right))
            return SNAP_BOTTOMRIGHT;
        if (left)
            return SNAP_LEFT;
        if (right)
            return SNAP_RIGHT;
    }
    if (top && (c->functions & MWM_FUNC_MAXIMIZE))
        return SNAP_MAXIMIZE;
    return SNAP_NONE;
}

static void drag_to(int x, int y, int w, int h)
{
    if (w < drag.min_w) w = drag.min_w;
    if (h < drag.min_h) h = drag.min_h;
    /* a moved window's title bar stays within reach: not above the work
     * area, nor behind a taskbar at the bottom */
    if (drag.dir == MR_MOVE || drag.dir == MR_MOVE_KEYBOARD) {
        if (y > AREA_Y + screen_h - taskbar_height - TITLE_KEEP)
            y = AREA_Y + screen_h - taskbar_height - TITLE_KEEP;
        if (y < AREA_Y)
            y = AREA_Y;
    }
    if (x == drag.cur_x && y == drag.cur_y && w == drag.cur_w && h == drag.cur_h)
        return;
    drag.cur_x = x;
    drag.cur_y = y;
    drag.cur_w = w;
    drag.cur_h = h;
    XMoveResizeWindow(dpy, drag.c->win, x, y, w, h);
}

static void on_moveresize(struct client *c, XClientMessageEvent *e)
{
    static Cursor cursors[MR_CANCEL];
    static const unsigned int shapes[MR_CANCEL] = {
        XC_top_left_corner, XC_top_side, XC_top_right_corner, XC_right_side,
        XC_bottom_right_corner, XC_bottom_side, XC_bottom_left_corner, XC_left_side,
        XC_left_ptr, XC_fleur, XC_fleur,
    };
    long dir = e->data.l[2], button = e->data.l[3];
    unsigned int mask, held = Button1Mask | Button2Mask | Button3Mask;
    XWindowAttributes wa;
    XSizeHints hints;
    Window r, child;
    int wx, wy, moving = dir == MR_MOVE || dir == MR_MOVE_KEYBOARD;
    long supplied;

    if (dir == MR_CANCEL) {
        if (drag.c == c)
            end_drag(1);
        return;
    }
    if (drag.c || dir < 0 || dir > MR_MOVE_KEYBOARD || c->iconic || c->maximized ||
        c->fullscreen || c->tray || !(c->functions & (moving ? MWM_FUNC_MOVE : MWM_FUNC_RESIZE)) ||
        !XGetWindowAttributes(dpy, c->win, &wa) ||
        !XQueryPointer(dpy, root, &r, &child, &drag.px, &drag.py, &wx, &wy, &mask))
        return;
    if (button >= 1 && button <= 3)
        held = Button1Mask << (button - 1);
    if (dir != MR_SIZE_KEYBOARD && dir != MR_MOVE_KEYBOARD && !(mask & held))
        return;                 /* the button is up already: that was a click */
    if (!cursors[dir])
        cursors[dir] = XCreateFontCursor(dpy, shapes[dir]);
    if (XGrabPointer(dpy, root, False, ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                     GrabModeAsync, GrabModeAsync, None, cursors[dir], CurrentTime) != GrabSuccess)
        return;
    XGrabKeyboard(dpy, root, False, GrabModeAsync, GrabModeAsync, CurrentTime);
    drag.c = c;
    drag.dir = dir;
    drag.button = button;
    drag.moving = drag_keyboard();
    drag.ox = drag.x = drag.cur_x = drag.from_x = wa.x;
    drag.oy = drag.y = drag.cur_y = drag.from_y = wa.y;
    drag.ow = drag.w = drag.cur_w = drag.from_w = wa.width;
    drag.oh = drag.h = drag.cur_h = drag.from_h = wa.height;
    drag.snapped = dir == MR_MOVE ? still_snapped(c) : SNAP_NONE;
    drag.min_w = drag.min_h = 32;
    if (XGetWMNormalHints(dpy, c->win, &hints, &supplied) && (hints.flags & PMinSize)) {
        drag.min_w = hints.min_width;
        drag.min_h = hints.min_height;
    }
    drag.hedge = drag.vedge = 0;
    drag.zone = SNAP_NONE;
}

static void on_drag_motion(int px, int py)
{
    struct client *c = drag.c;
    int dx = px - drag.px, dy = py - drag.py, d = drag.dir, zone;
    int x = drag.x, y = drag.y, w = drag.w, h = drag.h;
    int left = d == MR_SIZE_TOPLEFT || d == MR_SIZE_LEFT || d == MR_SIZE_BOTTOMLEFT;
    int right = d == MR_SIZE_TOPRIGHT || d == MR_SIZE_RIGHT || d == MR_SIZE_BOTTOMRIGHT;
    int top = d == MR_SIZE_TOPLEFT || d == MR_SIZE_TOP || d == MR_SIZE_TOPRIGHT;
    int bottom = d == MR_SIZE_BOTTOMLEFT || d == MR_SIZE_BOTTOM || d == MR_SIZE_BOTTOMRIGHT;

    if (d == MR_SIZE_KEYBOARD)
        return;
    if (!drag.moving) {
        if (abs(dx) < DRAG_THRESHOLD && abs(dy) < DRAG_THRESHOLD)
            return;
        drag.moving = 1;
        /* a snapped window comes away at the size it had before, under the pointer */
        if (drag.snapped) {
            c->snapped = SNAP_NONE;
            x = drag.x = drag.px - (int)((long)(drag.px - drag.x) * c->snap_w / drag.w);
            w = drag.w = drag.from_w = c->snap_w;
            h = drag.h = drag.from_h = c->snap_h;
            drag.from_x = c->snap_x;
            drag.from_y = c->snap_y;
        }
    }
    if (d == MR_MOVE || d == MR_MOVE_KEYBOARD) {
        x += dx;
        y += dy;
    } else {
        if (left) w -= dx;
        if (right) w += dx;
        if (top) h -= dy;
        if (bottom) h += dy;
        if (w < drag.min_w) w = drag.min_w;
        if (h < drag.min_h) h = drag.min_h;
        /* the opposite edges stay put */
        if (left) x += drag.w - w;
        if (top) y += drag.h - h;
    }
    drag_to(x, y, w, h);
    if (d == MR_MOVE && (zone = zone_at(c, px, py)) != drag.zone) {
        drag.zone = zone;
        show_outline(zone);
    }
}

/* Arrow keys move the window, or move the edges sizing it: as on Windows, the
 * first arrow picks the edge. Enter keeps the result, Esc undoes it. */
static void on_drag_key(XKeyEvent *e)
{
    KeySym key = XLookupKeysym(e, 0);
    int dx = 0, dy = 0, x = drag.cur_x, y = drag.cur_y, w = drag.cur_w, h = drag.cur_h;

    switch (key) {
    case XK_Escape:   end_drag(1); return;
    case XK_Return:
    case XK_KP_Enter: end_drag(0); return;
    case XK_Left:  case XK_KP_Left:  dx = -KEY_STEP; break;
    case XK_Right: case XK_KP_Right: dx = KEY_STEP; break;
    case XK_Up:    case XK_KP_Up:    dy = -KEY_STEP; break;
    case XK_Down:  case XK_KP_Down:  dy = KEY_STEP; break;
    default: return;
    }
    if (drag.dir == MR_MOVE_KEYBOARD) {
        drag.x += dx;
        drag.y += dy;
        drag_to(x + dx, y + dy, w, h);
    } else if (drag.dir == MR_SIZE_KEYBOARD) {
        if (dx && !drag.hedge) drag.hedge = dx < 0 ? -1 : 1;
        if (dy && !drag.vedge) drag.vedge = dy < 0 ? -1 : 1;
        if (drag.hedge < 0) {
            w = w - dx < drag.min_w ? drag.min_w : w - dx;
            x = drag.cur_x + drag.cur_w - w;
        } else if (drag.hedge > 0) {
            w += dx;
        }
        if (drag.vedge < 0) {
            h = h - dy < drag.min_h ? drag.min_h : h - dy;
            y = drag.cur_y + drag.cur_h - h;
        } else if (drag.vedge > 0) {
            h += dy;
        }
        drag_to(x, y, w, h);
    }
}

static void on_drag_button(XButtonEvent *e)
{
    if (drag_keyboard()) {
        if (e->type == ButtonPress)
            end_drag(0);                /* a click ends it, as Enter does */
    } else if (e->type == ButtonRelease && (drag.button < 1 || drag.button > 3 ||
                                            e->button == (unsigned int)drag.button)) {
        on_drag_motion(e->x_root, e->y_root);
        end_drag(0);
    }
}

static void end_drag(int cancel)
{
    struct client *c = drag.c;
    int zone = drag.zone;

    drag.c = NULL;
    drag.zone = SNAP_NONE;
    show_outline(SNAP_NONE);
    XUngrabPointer(dpy, CurrentTime);
    XUngrabKeyboard(dpy, CurrentTime);
    if (!c)
        return;
    if (cancel) {
        XMoveResizeWindow(dpy, c->win, drag.ox, drag.oy, drag.ow, drag.oh);
        c->snapped = drag.snapped;
    } else if (zone == SNAP_MAXIMIZE) {
        maximize(c);
    } else if (zone != SNAP_NONE) {
        snap(c, zone, drag.from_x, drag.from_y, drag.from_w, drag.from_h);
    }
}

/* ------------------------------------------------------------ snap keys */

/*
 * Windows+Left and Windows+Right snap the active window to that half of the
 * work area (the other way, a half goes back to where it was and a quarter
 * goes across). Windows+Up and Windows+Down take a snapped window a step up
 * or down its side: top quarter, half, bottom quarter; above the top
 * quarter it is maximised, below the bottom one it goes back to where it
 * was. Otherwise Windows+Up maximises, and Windows+Down restores a maximised
 * window or minimises. The keys are seen raw, as they go to the program;
 * the shell keeps them from it.
 */
static KeyCode key_left, key_right, key_up, key_down;

static void setup_keys(void)
{
    unsigned char bits[XIMaskLen(XI_LASTEVENT)];
    XIEventMask mask;
    int event, error, major = 2, minor = 2;

    if (!XQueryExtension(dpy, "XInputExtension", &xi_opcode, &event, &error) ||
        XIQueryVersion(dpy, &major, &minor) != Success) {
        xi_opcode = -1;
        return;
    }
    key_left = XKeysymToKeycode(dpy, XK_Left);
    key_right = XKeysymToKeycode(dpy, XK_Right);
    key_up = XKeysymToKeycode(dpy, XK_Up);
    key_down = XKeysymToKeycode(dpy, XK_Down);
    memset(bits, 0, sizeof(bits));
    XISetMask(bits, XI_RawKeyPress);
    XISetMask(bits, XI_RawKeyRelease);
    mask.deviceid = XIAllMasterDevices;
    mask.mask_len = sizeof(bits);
    mask.mask = bits;
    XISelectEvents(dpy, root, &mask, 1);
}

static void snap_key(KeyCode key)
{
    struct client *c = focused;
    XWindowAttributes wa;
    int side = key == key_left ? SNAP_LEFT : SNAP_RIGHT, area_h = screen_h - taskbar_height;
    int zone, half, upper;

    if (!c || drag.c || c->iconic || c->fullscreen || c->tray || is_shell_window(c->win, NULL) ||
        !XGetWindowAttributes(dpy, c->win, &wa))
        return;
    zone = c->maximized ? SNAP_NONE : still_snapped(c);
    half = zone == SNAP_LEFT || zone == SNAP_RIGHT;
    upper = zone == SNAP_TOPLEFT || zone == SNAP_TOPRIGHT;
    if (key == key_up) {
        /* a snapped window goes up a step: bottom quarter, half, top quarter, maximised */
        if (zone && !upper && (c->functions & MWM_FUNC_RESIZE))
            snap(c, half ? snap_quarter(snap_side(zone), 0) : snap_side(zone), 0, 0, 0, 0);
        else if (!c->maximized && (c->functions & MWM_FUNC_MAXIMIZE))
            maximize(c);
    } else if (key == key_down) {
        if (c->maximized) {
            /* one that was maximised from the start has no other size here yet */
            if (!c->has_saved || (c->saved_w >= screen_w && c->saved_h >= area_h)) {
                c->has_saved = 1;
                c->saved_w = screen_w * 3 / 4;
                c->saved_h = area_h * 3 / 4;
                c->saved_x = (screen_w - c->saved_w) / 2;
                c->saved_y = AREA_Y + (area_h - c->saved_h) / 2;
            }
            restore(c);
        } else if (half || upper) {
            /* a step down: top quarter, half, bottom quarter, where it was */
            snap(c, half ? snap_quarter(snap_side(zone), 1) : snap_side(zone), 0, 0, 0, 0);
        } else if (zone) {
            unsnap(c);
        } else if (c->functions & MWM_FUNC_MINIMIZE) {
            iconify(c);
        }
    } else if (!(c->functions & MWM_FUNC_RESIZE)) {
        return;
    } else if (c->maximized) {
        if (c->has_saved)
            snap(c, side, c->saved_x, c->saved_y, c->saved_w, c->saved_h);
        else
            snap(c, side, wa.x, wa.y, wa.width, wa.height);
    } else if (!zone) {
        snap(c, side, wa.x, wa.y, wa.width, wa.height);
    } else if (snap_side(zone) != side) {
        /* the other way: a half goes back to where it was, a quarter across */
        if (half)
            unsnap(c);
        else
            snap(c, snap_quarter(side, !upper), 0, 0, 0, 0);
    }
}

static void on_raw_key(XGenericEventCookie *cookie)
{
    static unsigned char down[256];
    XIRawEvent *e = cookie->data;
    Window r, child;
    int x, y, wx, wy;
    unsigned int mask;

    if (e->detail < 0 || e->detail > 255)
        return;
    if (cookie->evtype == XI_RawKeyRelease) {
        down[e->detail] = 0;
        return;
    }
    if (down[e->detail])        /* held down: it is repeating */
        return;
    down[e->detail] = 1;
    if ((e->detail == key_left || e->detail == key_right || e->detail == key_up ||
         e->detail == key_down) &&
        XQueryPointer(dpy, root, &r, &child, &x, &y, &wx, &wy, &mask) &&
        (mask & Mod4Mask) && !(mask & (ShiftMask | ControlMask | Mod1Mask)))
        snap_key(e->detail);
}

/* --------------------------------------------------------- preferences */

static long now_ms(void)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000L + tv.tv_usec / 1000;
}

/* Read wcwm.conf again if it changed, and apply what changed. */
static void check_prefs(void)
{
    static char path[512];
    static time_t mtime;
    static off_t size = -1;
    static ino_t ino;
    struct stat st;
    char line[128];
    FILE *f;
    int new_autohide = 0, new_ffm = 0, new_bar = bar_height, new_top = 0, height;
    long new_delay = 0;
    struct client *c;

    if (!path[0]) {
        const char *dir = getenv("XDG_RUNTIME_DIR");

        snprintf(path, sizeof(path), "%s/wcwm.conf", dir && *dir ? dir : "/tmp");
    }
    /* the shell replaces the file on each change: a new inode */
    if (stat(path, &st) || (st.st_ino == ino && st.st_mtime == mtime && st.st_size == size))
        return;
    ino = st.st_ino;
    mtime = st.st_mtime;
    size = st.st_size;
    if (!(f = fopen(path, "r")))
        return;
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "taskbar=", 8)) new_bar = atoi(line + 8);
        else if (!strncmp(line, "position=", 9)) new_top = !strncmp(line + 9, "top", 3);
        else if (!strncmp(line, "autohide=", 9)) new_autohide = atoi(line + 9);
        else if (!strncmp(line, "focus_follows_mouse=", 20)) new_ffm = atoi(line + 20);
        else if (!strncmp(line, "focus_delay=", 12)) new_delay = atol(line + 12);
    }
    fclose(f);

    if (new_bar >= 16 && new_bar <= 200)
        bar_height = new_bar;
    autohide = new_autohide;
    focus_delay = new_delay < 0 ? 0 : new_delay > 10000 ? 10000 : new_delay;
    /* Hidden, the taskbar leaves a 2-pixel strip; maximised windows get the rest. */
    height = autohide ? 2 : bar_height;
    if (height != taskbar_height || new_top != taskbar_top) {
        XWindowAttributes wa;

        for (c = clients; c; c = c->next)
            still_snapped(c);               /* to the old work area */
        taskbar_height = height;
        taskbar_top = new_top;
        update_workarea();
        for (c = clients; c; c = c->next)
            if (c->maximized)
                apply_state_geometry(c);
            else if (c->snapped)
                snap(c, c->snapped, c->snap_x, c->snap_y, c->snap_w, c->snap_h);
            else if (c->tray && XGetWindowAttributes(dpy, c->win, &wa))
                dock_tray(c, wa.width, wa.height);
    }
    if (new_ffm != focus_follows_mouse) {
        focus_follows_mouse = new_ffm;
        for (c = clients; c; c = c->next)
            XSelectInput(dpy, c->win, PropertyChangeMask | (focus_follows_mouse ? EnterWindowMask : 0));
        pending_focus = NULL;
    }
}

/* Focus follows the mouse: the pointer entering a window gives it the focus,
 * after focus_delay, if it is still there by then. */
static void on_enter(XCrossingEvent *e)
{
    struct client *c;

    last_time = e->time;
    if (!focus_follows_mouse || e->mode != NotifyNormal || e->detail == NotifyInferior)
        return;
    c = find_client(e->window);
    if (!c || c->iconic || c == focused)
        return;
    pending_focus = c;
    pending_due = now_ms() + focus_delay;
}

/* Whether the input focus is on a window that closes when it loses it: the
 * shell's Start menu, or a window this manager does not manage (Wine focuses
 * those itself when they are activated). Moving the pointer away from one
 * must leave it open, as on Windows. */
static int popup_has_focus(void)
{
    Window f, parent, root_ret, *kids;
    unsigned int n;
    int revert;
    XWindowAttributes wa;

    XGetInputFocus(dpy, &f, &revert);
    if (f == None || f == PointerRoot || f == root)
        return 0;
    /* the top-level window holding it */
    for (;;) {
        if (!XQueryTree(dpy, f, &root_ret, &parent, &kids, &n))
            return 0;
        if (kids)
            XFree(kids);
        if (parent == root || parent == None)
            break;
        f = parent;
    }
    if (find_client(f))
        return is_shell_window(f, "Start");
    return XGetWindowAttributes(dpy, f, &wa) && wa.override_redirect && wa.map_state == IsViewable;
}

static void focus_pending(void)
{
    struct client *c = pending_focus;
    Window root_ret, child = None;
    int rx, ry, wx, wy;
    unsigned int mask;

    if (!c || drag.c || now_ms() < pending_due)      /* not while dragging: after it */
        return;
    pending_focus = NULL;
    if (popup_has_focus())
        return;
    if (XQueryPointer(dpy, root, &root_ret, &child, &rx, &ry, &wx, &wy, &mask) && child == c->win &&
        !c->iconic && c != focused)
        focus_client(c);
}

/* ----------------------------------------------------------------- main */

static void setup(void)
{
    static const int supported[] = {
        NET_SUPPORTED, NET_SUPPORTING_WM_CHECK, NET_WM_NAME, NET_ACTIVE_WINDOW,
        NET_WORKAREA, NET_WM_STATE, NET_WM_STATE_FULLSCREEN, NET_WM_STATE_ABOVE,
        NET_WM_STATE_MAXIMIZED_VERT, NET_WM_STATE_MAXIMIZED_HORZ,
        NET_CLIENT_LIST, NET_NUMBER_OF_DESKTOPS, NET_CURRENT_DESKTOP,
        NET_DESKTOP_GEOMETRY, NET_DESKTOP_VIEWPORT, NET_WM_MOVERESIZE,
    };
    Atom list[sizeof(supported) / sizeof(supported[0])];
    Window dummy, *children = NULL, none = None;
    unsigned int count = 0, i;
    long zero = 0, one = 1;
    XColor color, exact;

    XInternAtoms(dpy, (char **)atom_names, NATOMS, False, atoms);

    /* A plain arrow and the wallpaper's base colour until the shell is up. */
    XDefineCursor(dpy, root, XCreateFontCursor(dpy, XC_left_ptr));
    if (XAllocNamedColor(dpy, DefaultColormap(dpy, DefaultScreen(dpy)), "#0b1530", &color, &exact)) {
        XSetWindowBackground(dpy, root, color.pixel);
        XClearWindow(dpy, root);
    }

    check_win = XCreateSimpleWindow(dpy, root, -1, -1, 1, 1, 0, 0, 0);
    XSelectInput(dpy, check_win, PropertyChangeMask);
    XChangeProperty(dpy, check_win, atoms[NET_SUPPORTING_WM_CHECK], XA_WINDOW, 32,
                    PropModeReplace, (unsigned char *)&check_win, 1);
    XChangeProperty(dpy, check_win, atoms[NET_WM_NAME], atoms[UTF8_STRING], 8,
                    PropModeReplace, (const unsigned char *)"wcwm", 4);
    XChangeProperty(dpy, root, atoms[NET_SUPPORTING_WM_CHECK], XA_WINDOW, 32,
                    PropModeReplace, (unsigned char *)&check_win, 1);
    for (i = 0; i < sizeof(supported) / sizeof(supported[0]); i++)
        list[i] = atoms[supported[i]];
    XChangeProperty(dpy, root, atoms[NET_SUPPORTED], XA_ATOM, 32, PropModeReplace,
                    (unsigned char *)list, i);
    set_cardinals(root, atoms[NET_NUMBER_OF_DESKTOPS], XA_CARDINAL, &one, 1);
    set_cardinals(root, atoms[NET_CURRENT_DESKTOP], XA_CARDINAL, &zero, 1);
    XChangeProperty(dpy, root, atoms[NET_ACTIVE_WINDOW], XA_WINDOW, 32, PropModeReplace,
                    (unsigned char *)&none, 1);
    update_workarea();
    update_client_list();

    /* Adopt windows that already exist (the window manager was restarted). */
    if (XQueryTree(dpy, root, &dummy, &dummy, &children, &count)) {
        for (i = 0; i < count; i++) {
            XWindowAttributes wa;
            struct client *c;

            if (!XGetWindowAttributes(dpy, children[i], &wa) || wa.map_state != IsViewable)
                continue;
            if (adopt_shell_window(children[i]) || wa.override_redirect)
                continue;
            if ((c = manage(children[i])))
                set_wm_state(c->win, NormalState);
        }
        if (children)
            XFree(children);
    }
    if (clients)
        activate(clients);
    else
        restack();
}

int main(int argc, char **argv)
{
    const char *env = getenv("WINCRAFT_TASKBAR");
    int h = argc > 1 ? atoi(argv[1]) : env ? atoi(env) : 0;
    XEvent ev;

    if (h >= 0 && (argc > 1 || env))
        taskbar_height = bar_height = h;
    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "wcwm: cannot open display\n");
        return 1;
    }
    root = DefaultRootWindow(dpy);
    screen_w = DisplayWidth(dpy, DefaultScreen(dpy));
    screen_h = DisplayHeight(dpy, DefaultScreen(dpy));
    XSetErrorHandler(on_error);
    XSelectInput(dpy, root, SubstructureRedirectMask | SubstructureNotifyMask |
                            StructureNotifyMask | PropertyChangeMask);
    XSync(dpy, False);
    if (other_wm) {
        fprintf(stderr, "wcwm: another window manager is running\n");
        return 1;
    }
    setup();
    setup_keys();
    check_prefs();

    for (;;) {
        fd_set fds;
        struct timeval tv = { 0, 250000 };
        int fd = ConnectionNumber(dpy);

        /* events first; between them, the preferences and a pending focus */
        if (!XPending(dpy)) {
            FD_ZERO(&fds);
            FD_SET(fd, &fds);
            if (pending_focus) {
                long left = pending_due - now_ms();

                if (left < 250)
                    tv.tv_usec = left > 0 ? left * 1000 : 0;
            }
            select(fd + 1, &fds, NULL, NULL, &tv);
            check_prefs();
            focus_pending();
            if (!XPending(dpy))
                continue;
        }
        XNextEvent(dpy, &ev);
        switch (ev.type) {
        case EnterNotify:      on_enter(&ev.xcrossing); break;
        case MapRequest:       on_map_request(&ev.xmaprequest); break;
        case MapNotify:        on_map_notify(&ev.xmap); break;
        case UnmapNotify:      on_unmap(&ev.xunmap); break;
        case ConfigureRequest: on_configure_request(&ev.xconfigurerequest); break;
        case ClientMessage:    on_client_message(&ev.xclient); break;
        case PropertyNotify:   on_property(&ev.xproperty); break;
        case ButtonPress:
            if (drag.c)
                on_drag_button(&ev.xbutton);
            else
                on_button_press(&ev.xbutton);
            break;
        case ButtonRelease:
            if (drag.c)
                on_drag_button(&ev.xbutton);
            break;
        case MotionNotify:
            /* only the latest position matters */
            while (XCheckTypedEvent(dpy, MotionNotify, &ev))
                ;
            if (drag.c)
                on_drag_motion(ev.xmotion.x_root, ev.xmotion.y_root);
            break;
        case KeyPress:
            if (drag.c)
                on_drag_key(&ev.xkey);
            break;
        case GenericEvent:
            if (ev.xcookie.extension == xi_opcode && XGetEventData(dpy, &ev.xcookie)) {
                if (ev.xcookie.evtype == XI_RawKeyPress || ev.xcookie.evtype == XI_RawKeyRelease)
                    on_raw_key(&ev.xcookie);
                XFreeEventData(dpy, &ev.xcookie);
            }
            break;
        case DestroyNotify: {
            struct client *c = find_client(ev.xdestroywindow.window);

            if (ev.xdestroywindow.window == taskbar_win) taskbar_win = None;
            if (ev.xdestroywindow.window == desktop_win) desktop_win = None;
            if (c)
                unmanage(c);
            break;
        }
        case ConfigureNotify:
            if (ev.xconfigure.window == root) {     /* screen resolution changed */
                struct client *c;

                for (c = clients; c; c = c->next)
                    still_snapped(c);       /* to the old screen */
                screen_w = ev.xconfigure.width;
                screen_h = ev.xconfigure.height;
                update_workarea();
                for (c = clients; c; c = c->next)
                    if (c->fullscreen || c->maximized)
                        apply_state_geometry(c);
                    else if (c->snapped)
                        snap(c, c->snapped, c->snap_x, c->snap_y, c->snap_w, c->snap_h);
            }
            break;
        }
    }
}
