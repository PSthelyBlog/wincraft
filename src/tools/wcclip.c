/*
 * wcclip - share the clipboard with the host of a virtual machine.
 *
 * QEMU offers the host's clipboard to a guest through a virtio serial port
 * named com.redhat.spice.0 (its qemu-vdagent device; SPICE does the same),
 * speaking the SPICE agent protocol. wcclip is the guest's end, for plain
 * text: it keeps that port and the X CLIPBOARD selection in step, and Wine
 * maps the X clipboard to the Windows one.
 *
 *   wcclip [PORT]      run; rc.boot starts it as a service when the port exists
 *   wcclip -p          print the text on the X clipboard (testing)
 *   wcclip -s TEXT     put TEXT on the X clipboard, serve it until replaced
 *                      or for 60 seconds (testing)
 *   WCCLIP_DEBUG=1     log every message to and from the port
 *
 * The host copies: QEMU announces a grab, wcclip asks for the text and, once
 * it has it, owns CLIPBOARD and answers X programs from that copy.
 * The guest copies: XFixes reports the new owner, wcclip announces a grab,
 * and converts the selection when the host asks for the text.
 *
 * SPDX-License-Identifier: MIT
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xfixes.h>

#define PORT_NAME       "com.redhat.spice.0"
#define MAX_CHUNK       2048            /* VD_AGENT_MAX_DATA_SIZE */
#define MAX_TEXT        (1 << 20)

/* SPICE agent protocol (spice-protocol, vd_agent.h), little endian */
#define VDP_CLIENT_PORT 1
#define VD_AGENT_PROTOCOL 1
enum {
    VD_AGENT_CLIPBOARD = 4,
    VD_AGENT_ANNOUNCE_CAPABILITIES = 6,
    VD_AGENT_CLIPBOARD_GRAB = 7,
    VD_AGENT_CLIPBOARD_REQUEST = 8,
    VD_AGENT_CLIPBOARD_RELEASE = 9,
};
enum { CAP_CLIPBOARD_BY_DEMAND = 5, CAP_GUEST_LINEEND_LF = 8 };
#define TYPE_UTF8_TEXT  1

struct chunk_header {
    uint32_t port, size;
} __attribute__((packed));

struct message_header {
    uint32_t protocol, type;
    uint64_t opaque;
    uint32_t size;
} __attribute__((packed));

static Display *dpy;
static Window win;
static Atom a_clipboard, a_targets, a_utf8, a_text, a_plain, a_incr, a_prop;
static int fixes_event;
static int port = -1;

static char *host_text;         /* the host's clipboard, while we own CLIPBOARD for it */
static size_t host_len;
static int host_owned;          /* we own CLIPBOARD on behalf of the host */
static int host_request;        /* the host asked for the guest's text */

static void say(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    fprintf(stderr, "wcclip: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

/* ======================================================================
 * The port
 * ====================================================================== */

static int write_all(const void *buf, size_t len)
{
    const char *p = buf;

    while (len) {
        ssize_t n = write(port, p, len);

        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        p += n;
        len -= n;
    }
    return 0;
}

static int debug;               /* WCCLIP_DEBUG=1: log the messages */

static void send_message(uint32_t type, const void *data, uint32_t size)
{
    struct message_header m = { VD_AGENT_PROTOCOL, type, 0, size };
    char *buf = malloc(sizeof(m) + size);
    size_t total = sizeof(m) + size, off;

    if (!buf)
        exit(1);
    if (debug)
        say("sent message %u (%u bytes)", type, size);
    memcpy(buf, &m, sizeof(m));
    if (size)
        memcpy(buf + sizeof(m), data, size);
    for (off = 0; off < total; off += MAX_CHUNK) {
        size_t n = total - off > MAX_CHUNK ? MAX_CHUNK : total - off;
        struct chunk_header c = { VDP_CLIENT_PORT, (uint32_t)n };

        if (write_all(&c, sizeof(c)) || write_all(buf + off, n)) {
            say("writing to the port failed: %s", strerror(errno));
            exit(1);
        }
    }
    free(buf);
}

static void send_caps(uint32_t request)
{
    uint32_t data[2] = { request, (1u << CAP_CLIPBOARD_BY_DEMAND) | (1u << CAP_GUEST_LINEEND_LF) };

    send_message(VD_AGENT_ANNOUNCE_CAPABILITIES, data, sizeof(data));
}

static void send_text(const char *text, size_t len)
{
    char *buf = malloc(4 + len);
    uint32_t type = TYPE_UTF8_TEXT;

    if (!buf)
        exit(1);
    memcpy(buf, &type, 4);
    if (len)
        memcpy(buf + 4, text, len);
    send_message(VD_AGENT_CLIPBOARD, buf, (uint32_t)(4 + len));
    free(buf);
}

/* ======================================================================
 * The X side
 * ====================================================================== */

static Time server_time(void)
{
    XEvent ev;

    /* a zero-length append gives a PropertyNotify carrying the server's time */
    XChangeProperty(dpy, win, a_prop, XA_STRING, 8, PropModeAppend, (unsigned char *)"", 0);
    XWindowEvent(dpy, win, PropertyChangeMask, &ev);
    return ev.xproperty.time;
}

static void own_clipboard(void)
{
    XSetSelectionOwner(dpy, a_clipboard, win, server_time());
    host_owned = XGetSelectionOwner(dpy, a_clipboard) == win;
    if (!host_owned)
        say("could not take the clipboard");
}

static void answer_request(XSelectionRequestEvent *rq, const char *text, size_t len)
{
    XEvent reply;
    Atom prop = rq->property != None ? rq->property : rq->target;

    memset(&reply, 0, sizeof(reply));
    reply.xselection.type = SelectionNotify;
    reply.xselection.requestor = rq->requestor;
    reply.xselection.selection = rq->selection;
    reply.xselection.target = rq->target;
    reply.xselection.time = rq->time;
    reply.xselection.property = None;
    if (rq->target == a_targets) {
        Atom targets[] = { a_targets, a_utf8, a_plain, a_text, XA_STRING };

        XChangeProperty(dpy, rq->requestor, prop, XA_ATOM, 32, PropModeReplace,
                        (unsigned char *)targets, sizeof(targets) / sizeof(targets[0]));
        reply.xselection.property = prop;
    } else if (rq->target == a_utf8 || rq->target == a_plain || rq->target == a_text) {
        XChangeProperty(dpy, rq->requestor, prop, a_utf8, 8, PropModeReplace,
                        (const unsigned char *)text, (int)len);
        reply.xselection.property = prop;
    } else if (rq->target == XA_STRING) {
        /* Latin-1: keep ASCII, replace the rest */
        char *latin = malloc(len + 1);
        size_t i, n = 0;

        for (i = 0; latin && i < len; i++) {
            unsigned char c = (unsigned char)text[i];

            if (c < 0x80)
                latin[n++] = (char)c;
            else if ((c & 0xc0) != 0x80)
                latin[n++] = '?';
        }
        if (latin) {
            XChangeProperty(dpy, rq->requestor, prop, XA_STRING, 8, PropModeReplace,
                            (unsigned char *)latin, (int)n);
            reply.xselection.property = prop;
            free(latin);
        }
    }
    XSendEvent(dpy, rq->requestor, False, 0, &reply);
    XFlush(dpy);
}

/* Read the converted selection from our window; NULL when there is none. */
static char *read_selection(XSelectionEvent *ev, size_t *len)
{
    Atom type;
    int format;
    unsigned long n, after;
    unsigned char *data = NULL;
    char *text;

    *len = 0;
    if (ev->property == None)
        return NULL;
    if (XGetWindowProperty(dpy, win, ev->property, 0, MAX_TEXT / 4, True, AnyPropertyType, &type,
                           &format, &n, &after, &data) != Success)
        return NULL;
    if (type == a_incr || format != 8) {        /* too large to pass on */
        if (data)
            XFree(data);
        return NULL;
    }
    text = malloc(n + 1);
    if (text) {
        memcpy(text, data, n);
        text[n] = 0;
        *len = n;
    }
    XFree(data);
    return text;
}

static int x_io_error(Display *d)
{
    say("lost the display");
    exit(1);
}

static void open_display(void)
{
    const char *name = getenv("DISPLAY") ? getenv("DISPLAY") : ":0";
    int err;

    /* the display server comes up after us, and again with every session */
    while (!(dpy = XOpenDisplay(name)))
        sleep(1);
    XSetIOErrorHandler(x_io_error);
    win = XCreateSimpleWindow(dpy, DefaultRootWindow(dpy), -10, -10, 1, 1, 0, 0, 0);
    XSelectInput(dpy, win, PropertyChangeMask);
    a_clipboard = XInternAtom(dpy, "CLIPBOARD", False);
    a_targets = XInternAtom(dpy, "TARGETS", False);
    a_utf8 = XInternAtom(dpy, "UTF8_STRING", False);
    a_text = XInternAtom(dpy, "TEXT", False);
    a_plain = XInternAtom(dpy, "text/plain;charset=utf-8", False);
    a_incr = XInternAtom(dpy, "INCR", False);
    a_prop = XInternAtom(dpy, "WCCLIP", False);
    if (!XFixesQueryExtension(dpy, &fixes_event, &err)) {
        say("the display server has no XFixes");
        exit(1);
    }
}

/* ======================================================================
 * Messages from the host
 * ====================================================================== */

static void handle_message(uint32_t type, const unsigned char *data, uint32_t size)
{
    uint32_t i, word;

    if (debug)
        say("received message %u (%u bytes)", type, size);
    switch (type) {
    case VD_AGENT_ANNOUNCE_CAPABILITIES:
        if (size >= 4 && (memcpy(&word, data, 4), word))
            send_caps(0);
        break;
    case VD_AGENT_CLIPBOARD_GRAB:
        /* the host copied something: fetch the text if there is some */
        for (i = 0; i + 4 <= size; i += 4) {
            memcpy(&word, data + i, 4);
            if (word == TYPE_UTF8_TEXT) {
                send_message(VD_AGENT_CLIPBOARD_REQUEST, &word, 4);
                break;
            }
        }
        break;
    case VD_AGENT_CLIPBOARD:
        if (size < 4 || (memcpy(&word, data, 4), word) != TYPE_UTF8_TEXT || size - 4 > MAX_TEXT)
            break;
        free(host_text);
        host_len = size - 4;
        host_text = malloc(host_len + 1);
        if (!host_text)
            exit(1);
        memcpy(host_text, data + 4, host_len);
        host_text[host_len] = 0;
        own_clipboard();
        break;
    case VD_AGENT_CLIPBOARD_REQUEST:
        /* the host pastes what the guest copied */
        if (host_owned) {
            send_text(host_text, host_len);
        } else {
            host_request = 1;
            XConvertSelection(dpy, a_clipboard, a_utf8, a_prop, win, CurrentTime);
            XFlush(dpy);
        }
        break;
    case VD_AGENT_CLIPBOARD_RELEASE:
        if (host_owned) {
            XSetSelectionOwner(dpy, a_clipboard, None, CurrentTime);
            host_owned = 0;
        }
        break;
    }
}

/* The port carries chunks; their payloads form a stream of messages. */
static void read_port(void)
{
    static unsigned char raw[65536], *stream;
    static size_t raw_len, stream_len, stream_cap, chunk_left;
    ssize_t n = read(port, raw + raw_len, sizeof(raw) - raw_len);
    size_t off = 0;

    if (n < 0 && (errno == EINTR || errno == EAGAIN))
        return;
    if (n <= 0) {
        say("the port closed");
        exit(1);
    }
    raw_len += n;
    while (off < raw_len) {
        if (!chunk_left) {
            struct chunk_header c;

            if (raw_len - off < sizeof(c))
                break;
            memcpy(&c, raw + off, sizeof(c));
            off += sizeof(c);
            chunk_left = c.size;
            continue;
        }
        n = raw_len - off < chunk_left ? (ssize_t)(raw_len - off) : (ssize_t)chunk_left;
        if (stream_len + n > stream_cap) {
            stream_cap = (stream_len + n) * 2;
            stream = realloc(stream, stream_cap);
            if (!stream)
                exit(1);
        }
        memcpy(stream + stream_len, raw + off, n);
        stream_len += n;
        off += n;
        chunk_left -= n;
    }
    memmove(raw, raw + off, raw_len - off);
    raw_len -= off;

    for (;;) {
        struct message_header m;

        if (stream_len < sizeof(m))
            break;
        memcpy(&m, stream, sizeof(m));
        if (m.size > MAX_TEXT + 64) {
            say("message too large (%u bytes)", m.size);
            exit(1);
        }
        if (stream_len < sizeof(m) + m.size)
            break;
        handle_message(m.type, stream + sizeof(m), m.size);
        memmove(stream, stream + sizeof(m) + m.size, stream_len - sizeof(m) - m.size);
        stream_len -= sizeof(m) + m.size;
    }
}

/* ======================================================================
 * Events from the display
 * ====================================================================== */

static void read_display(void)
{
    while (XPending(dpy)) {
        XEvent ev;

        XNextEvent(dpy, &ev);
        if (ev.type == fixes_event + XFixesSelectionNotify) {
            XFixesSelectionNotifyEvent *fx = (XFixesSelectionNotifyEvent *)&ev;

            if (fx->selection != a_clipboard || fx->owner == win)
                continue;
            if (fx->owner == None) {
                send_message(VD_AGENT_CLIPBOARD_RELEASE, NULL, 0);
            } else {
                /* a program in the guest copied: tell the host there is text */
                uint32_t type = TYPE_UTF8_TEXT;

                host_owned = 0;
                send_message(VD_AGENT_CLIPBOARD_GRAB, &type, 4);
            }
        } else if (ev.type == SelectionRequest) {
            answer_request(&ev.xselectionrequest, host_text ? host_text : "", host_text ? host_len : 0);
        } else if (ev.type == SelectionClear) {
            host_owned = 0;
        } else if (ev.type == SelectionNotify && host_request) {
            size_t len;
            char *text = read_selection(&ev.xselection, &len);

            host_request = 0;
            send_text(text ? text : "", text ? len : 0);
            free(text);
        }
    }
}

/* ======================================================================
 * Test modes
 * ====================================================================== */

static int print_clipboard(void)
{
    time_t end = time(NULL) + 5;
    size_t len;

    XConvertSelection(dpy, a_clipboard, a_utf8, a_prop, win, CurrentTime);
    XFlush(dpy);
    while (time(NULL) < end) {
        XEvent ev;

        if (!XPending(dpy)) {
            usleep(20000);
            continue;
        }
        XNextEvent(dpy, &ev);
        if (ev.type == SelectionNotify) {
            char *text = read_selection(&ev.xselection, &len);

            if (!text) {
                say("the clipboard holds no text");
                return 1;
            }
            fwrite(text, 1, len, stdout);
            putchar('\n');
            free(text);
            return 0;
        }
    }
    say("no answer from the clipboard's owner");
    return 1;
}

static int set_clipboard(const char *text)
{
    time_t end = time(NULL) + 60;

    host_text = strdup(text);
    host_len = strlen(text);
    own_clipboard();
    if (!host_owned)
        return 1;
    while (time(NULL) < end) {
        XEvent ev;

        if (!XPending(dpy)) {
            usleep(20000);
            continue;
        }
        XNextEvent(dpy, &ev);
        if (ev.type == SelectionRequest)
            answer_request(&ev.xselectionrequest, host_text, host_len);
        else if (ev.type == SelectionClear)
            return 0;
    }
    return 0;
}

/* ======================================================================
 * Entry point
 * ====================================================================== */

static int find_port(char *out, size_t size)
{
    DIR *d = opendir("/sys/class/virtio-ports");
    struct dirent *e;
    int found = 0;

    while (d && (e = readdir(d))) {
        char path[512], name[64] = "";
        FILE *f;

        if (e->d_name[0] == '.')
            continue;
        snprintf(path, sizeof(path), "/sys/class/virtio-ports/%s/name", e->d_name);
        if ((f = fopen(path, "r"))) {
            if (fgets(name, sizeof(name), f))
                name[strcspn(name, "\n")] = 0;
            fclose(f);
        }
        if (!strcmp(name, PORT_NAME)) {
            snprintf(out, size, "/dev/%s", e->d_name);
            found = 1;
            break;
        }
    }
    if (d)
        closedir(d);
    return found;
}

int main(int argc, char **argv)
{
    char path[512];
    struct pollfd fds[2];

    debug = getenv("WCCLIP_DEBUG") != NULL;
    if (argc > 1 && !strcmp(argv[1], "-p")) {
        open_display();
        return print_clipboard();
    }
    if (argc > 2 && !strcmp(argv[1], "-s")) {
        open_display();
        return set_clipboard(argv[2]);
    }
    if (argc > 1)
        snprintf(path, sizeof(path), "%s", argv[1]);
    else if (!find_port(path, sizeof(path))) {
        say("no %s port: this machine does not share its clipboard", PORT_NAME);
        sleep(60);      /* a service: do not let wcinit restart us in a loop */
        return 1;
    }
    port = open(path, O_RDWR | O_CLOEXEC);
    if (port < 0) {
        say("cannot open %s: %s", path, strerror(errno));
        sleep(10);
        return 1;
    }
    open_display();
    XFixesSelectSelectionInput(dpy, DefaultRootWindow(dpy), a_clipboard,
                                    XFixesSetSelectionOwnerNotifyMask);
    XFlush(dpy);
    send_caps(1);
    say("sharing the clipboard through %s", path);

    fds[0].fd = port;
    fds[0].events = POLLIN;
    fds[1].fd = ConnectionNumber(dpy);
    fds[1].events = POLLIN;
    for (;;) {
        read_display();
        if (poll(fds, 2, -1) < 0 && errno != EINTR)
            return 1;
        if (fds[0].revents & (POLLIN | POLLHUP | POLLERR))
            read_port();
        if (fds[1].revents & POLLIN)
            read_display();
    }
}
