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
"""
import json
import os
import socket
import struct
import sys
import time

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
        self.shot(path)
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
            sys.exit(r["error"]["desc"])


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
