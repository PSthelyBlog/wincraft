#!/usr/bin/env python3
"""List the third-party components that are actually in the WinCraft image.

A package is listed when at least one of its files ships unchanged in the
root filesystem (same path alone is not enough: /usr/bin/sh, for one, is a
BusyBox link here, not the bash one). For each: name, version, licence and
upstream URL - what is needed to credit it and to find the matching source.

Usage: manifest.py <staging-root> <output-root> <kernel-version>
"""
import os
import subprocess
import sys


def pacman(stage, *args):
    db = os.path.join(stage, "var/lib/pacman")
    return subprocess.run(["pacman", "--root", stage, "--dbpath", db, *args], check=True,
                          capture_output=True, text=True, env={"LC_ALL": "C", "PATH": os.environ["PATH"]}).stdout


def same_file(a, b):
    """Is b the file that was copied from a?"""
    try:
        sa, sb = os.lstat(a), os.lstat(b)
    except OSError:
        return False
    if os.path.islink(a) or os.path.islink(b):
        return os.path.islink(a) and os.path.islink(b) and os.readlink(a) == os.readlink(b)
    return sa.st_size == sb.st_size and int(sa.st_mtime) == int(sb.st_mtime)


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    stage, root, kernel = sys.argv[1].rstrip("/"), sys.argv[2].rstrip("/"), sys.argv[3]

    owner = {}
    for line in pacman(stage, "-Ql").splitlines():
        pkg, path = line.split(" ", 1)
        if path.startswith(stage + "/"):
            path = path[len(stage):]
        owner.setdefault(path.strip("/"), pkg)

    used = set()
    for folder, _, files in os.walk(root):
        for name in files:
            rel = os.path.relpath(os.path.join(folder, name), root)
            if rel in owner and same_file(os.path.join(stage, rel), os.path.join(root, rel)):
                used.add(owner[rel])

    info = {}
    for block in pacman(stage, "-Qi", *sorted(used)).split("\n\n"):
        fields = {}
        for line in block.splitlines():
            if " : " in line and not line.startswith(" "):
                key, value = line.split(" : ", 1)
                fields[key.strip()] = value.strip()
        if "Name" in fields:
            info[fields["Name"]] = fields

    print("# Third-party components in this WinCraft OS image")
    print("#")
    print("# The kernel is built from the unmodified upstream source with the configuration")
    print("# in kernel/wincraft.config. Everything else comes from Arch Linux binary")
    print("# packages; their build recipes (which name the exact upstream sources) are at")
    print("#   https://gitlab.archlinux.org/archlinux/packaging/packages/<name>")
    print("# and the packages themselves at https://archive.archlinux.org/packages/")
    print("#")
    print("# name\tversion\tlicence\tupstream")
    major = kernel.split(".")[0]
    print(f"linux\t{kernel}\tGPL-2.0-only\t"
          f"https://cdn.kernel.org/pub/linux/kernel/v{major}.x/linux-{kernel}.tar.xz")
    for name in sorted(info):
        f = info[name]
        print(f"{name}\t{f.get('Version', '?')}\t{f.get('Licenses', '?')}\t{f.get('URL', '?')}")


if __name__ == "__main__":
    main()
