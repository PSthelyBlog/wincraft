#!/usr/bin/env python3
"""Assemble the WinCraft root filesystem from the staged Arch package root.

The staged root (build/rootfs.arch) is a normal pacman-managed tree holding
far more than the OS needs. This script selects, file by file, what ships:

  1. every file of the packages listed in WHOLE,
  2. the listed files of the packages in PARTIAL,
  3. unowned, generated paths listed in EXTRA,
  4. shared libraries Wine opens with dlopen() (DLOPEN),
  5. the transitive closure of DT_NEEDED libraries of all of the above -
     only the library files themselves, not the packages they come from.

Then it verifies that every ELF object in the result can resolve its
dependencies, so a pruning mistake is a build error rather than a boot error.

Usage: assemble-rootfs.py <staging-root> <output-root>   (run in the toolbox)
"""
import fnmatch
import os
import subprocess
import sys

# Packages copied in full.
WHOLE = [
    "filesystem", "glibc", "busybox", "tzdata",
    "ca-certificates-mozilla", "ca-certificates-utils",
    "wine", "wine-mono",
    "libx11", "xkeyboard-config", "xorg-server", "xorg-server-common",
    "xf86-input-libinput", "libinput", "libwacom", "xorg-xkbcomp",
    "xorg-setxkbmap", "xorg-xrandr", "xorg-xset", "xorg-xinput", "xorg-server-xvfb",
    "mesa", "libglvnd",
    "fontconfig", "ttf-liberation",
    "alsa-lib",
    "dosfstools",
    "curl",
]

# Packages from which only some files are wanted (glob patterns).
PARTIAL = {
    "systemd": [
        "usr/bin/udevadm", "usr/lib/systemd/systemd-udevd",
        "usr/lib/udev/*", "etc/udev/*",
    ],
    "util-linux": [
        "usr/bin/" + n for n in (
            "sfdisk", "lsblk", "blkid", "findmnt", "mount", "umount",
            "mkswap", "swapon", "swapoff", "setpriv", "wipefs", "losetup",
            "partx", "blockdev", "fsck", "fstrim", "nsenter", "unshare")
    ],
    "e2fsprogs": [
        "usr/bin/mke2fs", "usr/bin/mkfs.ext4", "usr/bin/e2fsck",
        "usr/bin/fsck.ext4", "usr/bin/resize2fs", "usr/bin/tune2fs",
        "etc/mke2fs.conf",
    ],
    "limine": [
        "usr/bin/limine", "usr/share/limine/limine-bios.sys",
        "usr/share/limine/BOOTX64.EFI", "usr/share/limine/BOOTIA32.EFI",
    ],
    "iana-etc": ["etc/protocols", "etc/services"],
    "alsa-utils": ["usr/bin/amixer"],
}

# Generated at package-install time, owned by no package.
EXTRA = [
    "etc/ssl", "etc/ca-certificates", "etc/fonts",
    "usr/lib/udev/hwdb.bin", "usr/lib/locale",
]

# Libraries Wine loads by name at run time (not visible as DT_NEEDED).
DLOPEN = [
    "libEGL.so.1", "libGL.so.1", "libX11.so.6", "libXcomposite.so.1",
    "libXcursor.so.1", "libXext.so.6", "libXfixes.so.3", "libXi.so.6",
    "libXinerama.so.1", "libXrandr.so.2", "libXrender.so.1",
    "libXxf86vm.so.1", "libasound.so.2", "libdbus-1.so.3",
    "libfontconfig.so.1", "libfreetype.so.6", "libgnutls.so.30",
    "libgssapi_krb5.so.2", "libkrb5.so.3", "libudev.so.1",
    "libwayland-client.so.0", "libwayland-egl.so.1", "libxkbcommon.so.0",
    "libxkbregistry.so.0", "libgcc_s.so.1", "libresolv.so.2",
]

# Dropped even when selected above.
EXCLUDE = [
    # development tools shipped inside runtime packages
    "usr/bin/winebuild", "usr/bin/winecpp", "usr/bin/winegcc",
    "usr/bin/wineg++", "usr/bin/winemaker", "usr/bin/winedump",
    "usr/bin/widl", "usr/bin/wmc", "usr/bin/wrc", "usr/bin/function_grep.pl",
    "usr/share/applications/*", "usr/share/desktop-directories/*",
    # glibc extras
    "usr/lib/gconv/*", "usr/lib/getconf/*", "usr/lib/audit/*",
    "usr/bin/gencat", "usr/bin/getconf", "usr/bin/iconv", "usr/bin/iconvconfig",
    "usr/bin/locale-gen", "usr/bin/localedef", "usr/bin/makedb",
    "usr/bin/mtrace", "usr/bin/pcprofiledump", "usr/bin/pldd", "usr/bin/sln",
    "usr/bin/sotruss", "usr/bin/sprof", "usr/bin/tzselect", "usr/bin/xtrace",
    "usr/bin/zdump", "usr/bin/zic", "usr/bin/memusage*", "usr/bin/nscd",
    "usr/bin/ld.so", "usr/bin/gdbserver",
    "usr/lib/*.o", "usr/lib/libc_nonshared.a",
    # systemd policy that assumes systemd is PID 1
    "usr/lib/udev/rules.d/99-systemd.rules",
    "usr/lib/udev/rules.d/70-uaccess.rules",
    "usr/lib/udev/rules.d/71-seat.rules",
    "usr/lib/udev/rules.d/73-seat-late.rules",
    "usr/lib/udev/rules.d/90-vconsole.rules",
    "usr/lib/udev/rules.d/80-net-setup-link.rules",
    "usr/lib/udev/rules.d/75-net-description.rules",
    "usr/lib/udev/rules.d/81-net-dhcp.rules",
    "usr/lib/udev/rules.d/60-drm.rules.orig",
    "usr/lib/udev/hwdb.d/*",
    # Mesa video-decode front ends (need libva/libvdpau, not shipped)
    "usr/lib/dri/*_drv_video.so", "usr/lib/vdpau/*", "usr/lib/libVkLayer*",
    "usr/share/vulkan/*",
    # misc
    "usr/share/licenses/*/*.html", "usr/share/factory/*",
    "usr/lib/sysusers.d/*", "usr/lib/tmpfiles.d/*", "usr/lib/systemd/system/*",
    "usr/lib/systemd/user/*", "usr/share/libalpm/*", "usr/share/pacman/*",
    "etc/pacman.d/*", "var/lib/pacman/*",
]

# DT_NEEDED entries that may stay unresolved: optional Wine back ends whose
# host library is deliberately not shipped. Wine disables the feature cleanly.
OPTIONAL_LIBS = (
    "libsane.so", "libpulse.so", "libgphoto2", "libgst", "libavcodec.so",
    "libavformat.so", "libavutil.so", "libOpenCL.so", "libusb-1.0.so",
    "libpcsclite.so", "libpcap.so", "libgobject-2.0.so", "libglib-2.0.so",
)
OPTIONAL_USERS = ("usr/lib/wine/x86_64-unix/",)


def sh(*args):
    return subprocess.run(args, check=True, capture_output=True, text=True).stdout


def is_elf(path):
    try:
        if os.path.islink(path) or not os.path.isfile(path):
            return False
        with open(path, "rb") as f:
            return f.read(4) == b"\x7fELF"
    except OSError:
        return False


def elf_deps(path):
    """Return (needed sonames, runpath dirs) of an ELF file."""
    out = subprocess.run(["readelf", "-d", path], capture_output=True, text=True).stdout
    needed, rpath = [], []
    for line in out.splitlines():
        if "(NEEDED)" in line:
            needed.append(line.split("[", 1)[1].rstrip("]"))
        elif "(RUNPATH)" in line or "(RPATH)" in line:
            rpath += line.split("[", 1)[1].rstrip("]").split(":")
    return needed, rpath


class Assembler:
    def __init__(self, stage, out):
        self.stage = stage.rstrip("/")
        self.out = out.rstrip("/")
        self.selected = set()
        self.owner = {}
        self.pkg_files = {}
        self.auto_libs = {}

    def load_packages(self):
        db = os.path.join(self.stage, "var/lib/pacman")
        for line in sh("pacman", "--root", self.stage, "--dbpath", db, "-Ql").splitlines():
            pkg, path = line.split(" ", 1)
            if path.startswith(self.stage + "/"):   # -Ql prints root-prefixed paths
                path = path[len(self.stage):]
            rel = path.lstrip("/").rstrip("/")
            if not rel:
                continue
            self.pkg_files.setdefault(pkg, []).append(rel)
            self.owner.setdefault(rel, pkg)

    def exists(self, rel):
        return os.path.lexists(os.path.join(self.stage, rel))

    def add(self, rel):
        """Select a path, plus whatever a symlink chain points at."""
        rel = os.path.normpath(rel)
        seen = 0
        while rel not in self.selected and self.exists(rel) and seen < 16:
            self.selected.add(rel)
            full = os.path.join(self.stage, rel)
            if not os.path.islink(full):
                break
            target = os.readlink(full)
            if target.startswith("/"):
                rel = os.path.normpath(target.lstrip("/"))
            else:
                rel = os.path.normpath(os.path.join(os.path.dirname(rel), target))
            seen += 1

    def add_tree(self, rel):
        full = os.path.join(self.stage, rel)
        if not os.path.lexists(full):
            print(f"  warning: extra path missing: {rel}")
            return
        self.add(rel)
        if os.path.isdir(full) and not os.path.islink(full):
            for root, dirs, files in os.walk(full):
                for n in dirs + files:
                    self.add(os.path.relpath(os.path.join(root, n), self.stage))

    def resolve_lib(self, soname, origin, rpath):
        dirs = []
        for d in rpath:
            d = d.replace("$ORIGIN", "/" + os.path.dirname(origin)).replace("${ORIGIN}", "/" + os.path.dirname(origin))
            dirs.append(os.path.normpath(d).lstrip("/"))
        # The object's own directory covers Wine's unix libraries, which link
        # against the ntdll.so / win32u.so sitting next to them.
        dirs += ["usr/lib", os.path.dirname(origin)]
        for d in dirs:
            rel = os.path.join(d, soname)
            if self.exists(rel):
                return rel
        return None

    def select(self):
        for pkg in WHOLE:
            if pkg not in self.pkg_files:
                sys.exit(f"error: package {pkg} is not staged")
            for rel in self.pkg_files[pkg]:
                self.add(rel)
        for pkg, patterns in PARTIAL.items():
            if pkg not in self.pkg_files:
                sys.exit(f"error: package {pkg} is not staged")
            for rel in self.pkg_files[pkg]:
                if any(fnmatch.fnmatch(rel, p) for p in patterns):
                    self.add(rel)
        for rel in EXTRA:
            self.add_tree(rel)
        for soname in DLOPEN:
            rel = self.resolve_lib(soname, "usr/lib/x", [])
            if not rel:
                sys.exit(f"error: dlopen library {soname} is not staged")
            self.add(rel)

        self.selected = {r for r in self.selected
                         if not any(fnmatch.fnmatch(r, p) for p in EXCLUDE)}

        # Transitive DT_NEEDED closure.
        scanned = set()
        while True:
            todo = [r for r in self.selected if r not in scanned]
            if not todo:
                break
            for rel in todo:
                scanned.add(rel)
                full = os.path.join(self.stage, rel)
                if not is_elf(full):
                    continue
                needed, rpath = elf_deps(full)
                for soname in needed:
                    lib = self.resolve_lib(soname, rel, rpath)
                    if lib is None:
                        continue        # reported by verify()
                    if os.path.normpath(lib) not in self.selected:
                        self.auto_libs.setdefault(self.owner.get(lib, "?"), set()).add(soname)
                        self.add(lib)

    def copy(self):
        # Parent directories must be listed so they keep their staged modes.
        paths = set(self.selected)
        for rel in list(self.selected):
            d = os.path.dirname(rel)
            while d:
                paths.add(d)
                d = os.path.dirname(d)
        listing = self.out + ".files"
        with open(listing, "w") as f:
            for rel in sorted(paths):
                f.write(rel + "\n")
        os.makedirs(self.out, exist_ok=True)
        subprocess.run(["rsync", "-aHAX", "--files-from=" + listing,
                        self.stage + "/", self.out + "/"], check=True)
        os.unlink(listing)

    def verify(self):
        missing = {}
        for root, _, files in os.walk(self.out):
            for n in files:
                full = os.path.join(root, n)
                if not is_elf(full):
                    continue
                rel = os.path.relpath(full, self.out)
                needed, rpath = elf_deps(full)
                for soname in needed:
                    dirs = [os.path.normpath(d.replace("$ORIGIN", "/" + os.path.dirname(rel))).lstrip("/")
                            for d in rpath] + ["usr/lib", os.path.dirname(rel)]
                    if any(os.path.exists(os.path.join(self.out, d, soname)) for d in dirs):
                        continue
                    if rel.startswith(OPTIONAL_USERS) and soname.startswith(OPTIONAL_LIBS):
                        continue
                    missing.setdefault(soname, []).append(rel)
        return missing


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    a = Assembler(sys.argv[1], sys.argv[2])
    a.load_packages()
    a.select()
    print(f"  selected {len(a.selected)} paths; libraries pulled in by dependency:")
    for pkg in sorted(a.auto_libs):
        print(f"    {pkg}: {' '.join(sorted(a.auto_libs[pkg]))}")
    a.copy()
    missing = a.verify()
    if missing:
        print("error: unresolved shared libraries:")
        for soname, users in sorted(missing.items()):
            print(f"    {soname}  <- {', '.join(users[:4])}{' ...' if len(users) > 4 else ''}")
        sys.exit(1)
    print("  dependency closure verified")


if __name__ == "__main__":
    main()
