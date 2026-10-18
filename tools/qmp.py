#!/usr/bin/env python3
"""Drive a running WinCraft VM (started by tools/vm) over QMP and its serial port.

  qmp.py shot FILE.png             screenshot
  qmp.py key ctrl-alt-f2 ...       press key combinations (QEMU qcode names)
  qmp.py type "some text"          type text
  qmp.py move X Y                  move the pointer (screen pixels)
  qmp.py click X Y [left|right]    click
  qmp.py dclick X Y                double click
  qmp.py sh "command" [timeout]    run a command in the serial root shell
                                   (needs the wc.shell boot option), print output
  qmp.py serial                    print the serial log
  qmp.py quit                      power the VM off hard
  qmp.py raw '{"execute": ...}'    send one raw QMP command

QEMU cannot take a screenshot of a virtio GPU with 3D acceleration (the
default in a window, see tools/vm). "shot" then reads the screen from the
guest's X server instead (tools/xshot.c, copied into the guest the first
time), and the pointer commands take the screen size from xrandr there. The
pictures travel through the virtio-serial port tools/vm --shell adds
(build/vm/out.sock); without it, as base64 through the serial console, which
stalls the guest for seconds.
"""
import base64
import gzip
import json
import os
import socket
import struct
import subprocess
import sys
import threading
import time
import zlib

PROJECT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VM = os.path.join(PROJECT, "build", "vm")

KEYS = {
    " ": "spc", "\n": "ret", "\t": "tab", "-": "minus", "=": "equal",
    "[": "bracket_left", "]": "bracket_right", ";": "semicolon",
    "'": "apostrophe", "`": "grave_accent", "\\": "backslash", ",": "comma",
    ".": "dot", "/": "slash",
}
SHIFTED = {
    "!": "1", "@": "2", "#": "3", "$": "4", "%": "5", "^": "6", "&": "7",
    "*": "8", "(": "9", ")": "0", "_": "minus", "+": "equal",
    "{": "bracket_left", "}": "bracket_right", ":": "semicolon",
    '"': "apostrophe", "~": "grave_accent", "|": "backslash", "<": "comma",
    ">": "dot", "?": "slash",
}


class QMP:
    def __init__(self):
        self.s = socket.socket(socket.AF_UNIX)
        self.s.connect(os.path.join(VM, "qmp.sock"))
        self.f = self.s.makefile("rw")
        self.f.readline()
        self.cmd("qmp_capabilities")

    def cmd(self, name, **args):
        msg = {"execute": name}
        if args:
            msg["arguments"] = args
        self.f.write(json.dumps(msg) + "\n")
        self.f.flush()
        while True:
            reply = json.loads(self.f.readline())
            if "return" in reply or "error" in reply:
                return reply

    def keys(self, combo, hold=60):
        names = combo.split("-") if len(combo) > 1 else [combo]
        self.cmd("send-key", keys=[{"type": "qcode", "data": k} for k in names],
                 **{"hold-time": hold})
        time.sleep(hold / 1000 + 0.05)

    def type(self, text):
        for ch in text:
            if ch in KEYS:
                self.keys(KEYS[ch])
            elif ch in SHIFTED:
                self.keys("shift-" + SHIFTED[ch])
            elif ch.isupper():
                self.keys("shift-" + ch.lower())
            else:
                self.keys(ch)

    def size(self):
        path = os.path.join(VM, ".size.png")
        r = self.cmd("screendump", filename="/work" + path[len(PROJECT):], format="png")
        if "error" in r:                    # a 3D-accelerated display: ask X
            out, _ = serial_sh("DISPLAY=:0 xrandr 2>/dev/null | "
                               "sed -n 's/.* current \\([0-9]*\\) x \\([0-9]*\\),.*/\\1 \\2/p'", 10)
            try:
                w, h = out.split()[:2]
                return int(w), int(h)
            except ValueError:
                sys.exit("cannot tell the screen size: " + r["error"]["desc"])
        with open(path, "rb") as f:
            head = f.read(24)
        os.unlink(path)
        return struct.unpack(">II", head[16:24])

    def move(self, x, y):
        w, h = self.size()
        ev = [{"type": "abs", "data": {"axis": "x", "value": int(x * 32767 / (w - 1))}},
              {"type": "abs", "data": {"axis": "y", "value": int(y * 32767 / (h - 1))}}]
        self.cmd("input-send-event", events=ev)
        time.sleep(0.15)

    def button(self, name, down):
        self.cmd("input-send-event",
                 events=[{"type": "btn", "data": {"down": down, "button": name}}])
        time.sleep(0.08)

    def click(self, x, y, name="left", count=1):
        self.move(x, y)
        for _ in range(count):
            self.button(name, True)
            self.button(name, False)

    def shot(self, path):
        path = os.path.abspath(path)
        if not path.startswith(PROJECT + "/"):
            sys.exit("screenshots must be saved inside the project directory")
        r = self.cmd("screendump", filename="/work" + path[len(PROJECT):], format="png")
        if "error" in r:
            if r["error"]["desc"] != "no surface":
                sys.exit(r["error"]["desc"])
            guest_shot(path, self)


def write_png(path, width, height, rgb):
    raw = b"".join(b"\0" + rgb[y * width * 3:(y + 1) * width * 3] for y in range(height))

    def chunk(kind, data):
        return (struct.pack(">I", len(data)) + kind + data +
                struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff))

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" +
                chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


# the guest's end of out.sock, found by the port's name
GUEST_PORT = ('for p in /sys/class/virtio-ports/*; do '
              '[ "$(cat $p/name 2>/dev/null)" = org.wincraft.out ] && port=/dev/${p##*/}; done; ')


def channel_connect(sock, q=None):
    """Connect to out.sock and wait until QEMU has taken the connection: the
    guest reads end of file, and its writes are lost, while QEMU sees none.
    q: the caller's QMP connection; QEMU takes one at a time, so a second
    one would wait for the first to close."""
    s = socket.socket(socket.AF_UNIX)
    s.connect(sock)
    own = q is None
    if own:
        q = QMP()
    for _ in range(50):
        if any(c["label"] == "wcout" and not c["filename"].startswith("disconnected")
               for c in q.cmd("query-chardev")["return"]):
            break
        time.sleep(0.05)
    if own:
        q.s.close()
    return s


def channel_read(sock, command, timeout=60, q=None):
    """Run command in the guest with its output going to out.sock; return what arrives.
    A PPM is complete when its header's size is reached."""
    s = channel_connect(sock, q)
    s.settimeout(timeout)
    got = {"data": bytearray()}

    def reader():
        buf, need = got["data"], None
        while need is None or len(buf) < need:
            try:
                chunk = s.recv(1 << 20)
            except socket.timeout:
                break
            if not chunk:
                break
            buf += chunk
            if need is None and buf.count(b"\n") >= 3:
                head = bytes(buf).split(b"\n", 3)
                w, h = map(int, head[1].split())
                need = len(buf) - len(head[3]) + w * h * 3

    t = threading.Thread(target=reader)
    t.start()
    serial_sh(GUEST_PORT + command + ' > "$port"', timeout)
    t.join()
    s.close()
    return bytes(got["data"])


def channel_write(sock, data, command, timeout=30, q=None):
    """Send data to the guest through out.sock, to command's standard input.
    The guest opens the port first: QEMU drops what arrives before that."""
    s = channel_connect(sock, q)
    serial_sh("kill $(cat /run/channel.pid 2>/dev/null) 2>/dev/null; rm -f /run/channel.done; " + GUEST_PORT +
              '( (%s) < "$port"; touch /run/channel.done ) >/dev/null 2>&1 & echo $! > /run/channel.pid'
              % command, 10)
    time.sleep(0.5)
    s.sendall(data)
    deadline = time.time() + timeout
    while time.time() < deadline:
        out, _ = serial_sh("test -e /run/channel.done && echo finished", 10)
        if "finished" in out:
            break
        time.sleep(0.3)
    s.close()


def guest_shot(path, q=None):
    """Screenshot taken by the guest's X server; q as for channel_connect."""
    xshot, src = os.path.join(PROJECT, "build", "xshot"), os.path.join(PROJECT, "tools", "xshot.c")
    sock = os.path.join(VM, "out.sock")
    if not os.path.exists(xshot) or os.path.getmtime(xshot) < os.path.getmtime(src):
        subprocess.run([os.path.join(PROJECT, "tools", "tb"), "gcc", "-O2", "-o", "/work/build/xshot",
                        "/work/tools/xshot.c", "-lX11"], check=True)
    out, _ = serial_sh("test -x /run/xshot && echo present", 10)
    if "present" not in out:
        data = open(xshot, "rb").read()
        if os.path.exists(sock):
            # BusyBox head exits with 1 here even when it got everything: check the size
            channel_write(sock, data, "head -c %d > /run/xshot.new; [ $(stat -c %%s /run/xshot.new) = %d ] && "
                                      "chmod 755 /run/xshot.new && mv /run/xshot.new /run/xshot"
                                      % (len(data), len(data)), q=q)
        else:
            data = base64.b64encode(data).decode()
            serial_sh("rm -f /run/xshot.b64", 10)
            for i in range(0, len(data), 1500):
                serial_sh("echo %s >> /run/xshot.b64" % data[i:i + 1500], 10)
            serial_sh("base64 -d /run/xshot.b64 > /run/xshot && chmod 755 /run/xshot; rm -f /run/xshot.b64", 10)
    if os.path.exists(sock):
        ppm = channel_read(sock, "/run/xshot", q=q)
    else:
        out, done = serial_sh("/run/xshot | gzip -1 | base64", 120)
        if not done:
            sys.exit("no screenshot: the guest's serial shell did not answer (boot it with --shell)")
        try:
            ppm = gzip.decompress(base64.b64decode(out))
        except (ValueError, OSError):
            sys.exit("no screenshot: " + out.strip()[:200])
    try:
        _, dims, _, rgb = ppm.split(b"\n", 3)
        width, height = map(int, dims.split())
        if len(rgb) < width * height * 3:
            raise ValueError("short")
    except ValueError:
        sys.exit("no screenshot: the guest sent %d bytes" % len(ppm))
    write_png(path, width, height, rgb)


def serial_sh(command, timeout=60):
    """Run a command in the serial root shell; return (output, finished)."""
    import re
    tag = "%d" % (time.time() * 1000 % 1000000)
    begin, end = "WCB" + tag, "WCE" + tag
    s = socket.socket(socket.AF_UNIX)
    s.connect(os.path.join(VM, "serial.sock"))
    # Echo is switched off so only real output lies between the two markers.
    s.sendall(("\nstty -echo 2>/dev/null; echo %s; %s; echo %s$?\n" % (begin, command, end)).encode())
    pattern = re.compile(begin + r"\n(.*)" + end + r"(\d+)", re.S)
    buf = b""
    s.settimeout(1.0)
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            chunk = s.recv(65536)
        except socket.timeout:
            continue
        if not chunk:
            break
        buf += chunk
        m = pattern.search(buf.decode(errors="replace").replace("\r", ""))
        if m:
            return m.group(1).rstrip("\n"), True
    return buf.decode(errors="replace").replace("\r", ""), False


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    op, args = sys.argv[1], sys.argv[2:]
    if op == "serial":
        with open(os.path.join(VM, "serial.log"), errors="replace") as f:
            sys.stdout.write(f.read())
        return
    if op == "sh":
        out, done = serial_sh(args[0], float(args[1]) if len(args) > 1 else 60)
        print(out)
        if not done:
            print("[qmp.py: timed out waiting for the command to finish]")
        return
    q = QMP()
    if op == "shot":
        q.shot(args[0])
    elif op == "key":
        for combo in args:
            q.keys(combo)
    elif op == "type":
        q.type(args[0])
    elif op == "move":
        q.move(int(args[0]), int(args[1]))
    elif op == "click":
        q.click(int(args[0]), int(args[1]), args[2] if len(args) > 2 else "left")
    elif op == "dclick":
        q.click(int(args[0]), int(args[1]), count=2)
    elif op == "quit":
        q.cmd("quit")
    elif op == "raw":
        print(json.dumps(q.cmd(**{"name": json.loads(args[0])["execute"],
                                  **json.loads(args[0]).get("arguments", {})})))
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
