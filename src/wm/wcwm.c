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
};

enum {
    WM_PROTOCOLS, WM_TAKE_FOCUS, WM_STATE, WM_CHANGE_STATE,
    NET_SUPPORTED, NET_SUPPORTING_WM_CHECK, NET_WM_NAME, NET_ACTIVE_WINDOW,
    NET_WORKAREA, NET_WM_STATE, NET_WM_STATE_FULLSCREEN, NET_WM_STATE_ABOVE,
    NET_WM_STATE_MAXIMIZED_VERT, NET_WM_STATE_MAXIMIZED_HORZ,
    NET_CLIENT_LIST, NET_NUMBER_OF_DESKTOPS, NET_CURRENT_DESKTOP,
    NET_DESKTOP_GEOMETRY, NET_DESKTOP_VIEWPORT, NET_WM_USER_TIME, UTF8_STRING,
    NATOMS
};

static const char *atom_names[NATOMS] = {
    "WM_PROTOCOLS", "WM_TAKE_FOCUS", "WM_STATE", "WM_CHANGE_STATE",
    "_NET_SUPPORTED", "_NET_SUPPORTING_WM_CHECK", "_NET_WM_NAME", "_NET_ACTIVE_WINDOW",
    "_NET_WORKAREA", "_NET_WM_STATE", "_NET_WM_STATE_FULLSCREEN", "_NET_WM_STATE_ABOVE",
    "_NET_WM_STATE_MAXIMIZED_VERT", "_NET_WM_STATE_MAXIMIZED_HORZ",
    "_NET_CLIENT_LIST", "_NET_NUMBER_OF_DESKTOPS", "_NET_CURRENT_DESKTOP",
    "_NET_DESKTOP_GEOMETRY", "_NET_DESKTOP_VIEWPORT", "_NET_WM_USER_TIME", "UTF8_STRING",
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
    read_net_state(c);
    update_client_list();
    return c;
}

static void unmanage(struct client *c)
{
    struct client **p;
    int was_focused = c == focused;

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
 * Notification-area ("tray") icons live in a small window owned by the
 * runtime's explorer process. Dock it in the corner by the clock: above it,
 * or below it when the taskbar is at the top.
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

        taskbar_height = height;
        taskbar_top = new_top;
        update_workarea();
        for (c = clients; c; c = c->next)
            if (c->maximized)
                apply_state_geometry(c);
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

    if (!c || now_ms() < pending_due)
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
        NET_DESKTOP_GEOMETRY, NET_DESKTOP_VIEWPORT,
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
        case ButtonPress:      on_button_press(&ev.xbutton); break;
        case PropertyNotify:   on_property(&ev.xproperty); break;
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

                screen_w = ev.xconfigure.width;
                screen_h = ev.xconfigure.height;
                update_workarea();
                for (c = clients; c; c = c->next)
                    if (c->fullscreen || c->maximized)
                        apply_state_geometry(c);
            }
            break;
        }
    }
}
