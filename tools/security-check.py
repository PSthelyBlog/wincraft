#!/usr/bin/env python3
"""security-check.py - known security fixes that a WinCraft OS image lacks.

usage: tools/security-check.py [COMPONENTS]

COMPONENTS is the list every build writes next to the ISO,
out/wincraft-VERSION-components.txt; without it, the newest one in out/.

- Arch Linux packages are checked against Arch's security tracker. An advisory
  applies when the image has an older version than the one that fixes it.
  Advisories the tracker lists with no fixed version are shown too, with the
  version they were recorded against: many are years old and no longer kept
  up to date there, so they are for a person to check, and do not fail the
  check.
- The kernel is checked against kernel.org: whether its long-term series has a
  newer point release, which carries security fixes too, and whether the
  series is still maintained.

Prints what needs attention and exits with status 1 when there is a fix to take
(a newer package or kernel), so that a scheduled job fails and its owner hears
of it; 0 when the image has every fix that has been published. Needs network access; sends nothing but the two
requests.

SPDX-License-Identifier: MIT
"""
import glob
import json
import os
import re
import sys
import urllib.request

TRACKER = "https://security.archlinux.org/issues/all.json"
KERNELS = "https://www.kernel.org/releases.json"
USER_AGENT = "WinCraft-security-check"


def fetch_json(url):
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(req, timeout=60) as r:
        return json.load(r)


# ---- Arch's version order (pacman's alpm_pkg_vercmp) ----

def _rpmvercmp(a, b):
    if a == b:
        return 0
    i = j = 0
    while i < len(a) and j < len(b):
        si, sj = i, j
        while i < len(a) and not a[i].isalnum():
            i += 1
        while j < len(b) and not b[j].isalnum():
            j += 1
        if i >= len(a) or j >= len(b):
            break
        if i - si != j - sj:                    # separators of different lengths
            return -1 if i - si < j - sj else 1
        ei, ej = i, j
        if a[i].isdigit():
            while ei < len(a) and a[ei].isdigit():
                ei += 1
            while ej < len(b) and b[ej].isdigit():
                ej += 1
            isnum = True
        else:
            while ei < len(a) and a[ei].isalpha():
                ei += 1
            while ej < len(b) and b[ej].isalpha():
                ej += 1
            isnum = False
        one, two = a[i:ei], b[j:ej]
        if not two:                             # numeric beats alpha
            return 1 if isnum else -1
        if isnum:
            one, two = one.lstrip("0"), two.lstrip("0")
            if len(one) != len(two):
                return 1 if len(one) > len(two) else -1
        if one != two:
            return -1 if one < two else 1
        i, j = ei, ej
    rest_a, rest_b = a[i:], b[j:]
    if not rest_a and not rest_b:
        return 0
    if (not rest_a and not rest_b[:1].isalpha()) or rest_a[:1].isalpha():
        return -1
    return 1


def _evr(v):
    epoch, _, rest = v.partition(":") if ":" in v else ("0", "", v)
    version, _, release = rest.rpartition("-") if "-" in rest else (rest, "", None)
    return epoch or "0", version, release


def vercmp(a, b):
    """<0, 0, >0 as a is older than, the same as, or newer than b."""
    if a == b:
        return 0
    ea, va, ra = _evr(a)
    eb, vb, rb = _evr(b)
    r = _rpmvercmp(ea, eb) or _rpmvercmp(va, vb)
    if r == 0 and ra is not None and rb is not None:
        r = _rpmvercmp(ra, rb)
    return r


# ---- the image ----

def read_components(path):
    """{name: version} from a components list ("name<TAB>version<TAB>...")."""
    out = {}
    for line in open(path, encoding="utf-8"):
        if line.startswith("#") or not line.strip():
            continue
        fields = line.rstrip("\n").split("\t")
        if len(fields) >= 2:
            out[fields[0]] = fields[1]
    return out


def arch_findings(components, groups):
    found = []
    for g in groups:
        status = g.get("status")
        if status == "Not affected":
            continue
        for pkg in g.get("packages", []):
            have = components.get(pkg)
            if not have or pkg == "linux" or pkg.startswith("linux-"):
                continue                        # WinCraft builds its own kernel
            fixed = g.get("fixed")
            if fixed:
                if vercmp(have, fixed) < 0:
                    found.append((pkg, have, g, "fixed in " + fixed))
            elif status in ("Vulnerable", "Unknown") and vercmp(have, g.get("affected") or have) >= 0:
                found.append((pkg, have, g, "no fix published yet"))
    return found


def kernel_findings(components, releases):
    have = components.get("linux")
    if not have:
        return []
    series = ".".join(have.split(".")[:2])
    same = [r for r in releases.get("releases", []) if r["version"].startswith(series + ".")
            or r["version"] == series]
    if not same:
        return [f"linux {have}: the {series} series is no longer listed on kernel.org; move to a "
                "maintained long-term series"]
    out = []
    for r in same:
        if r.get("iseol"):
            out.append(f"linux {have}: the {series} series has reached its end of life ({r['version']} "
                       "was the last); move to a maintained long-term series")
        elif vercmp(have, r["version"]) < 0:
            out.append(f"linux {have}: {r['version']} is out in the {series} series ({r['moniker']}). "
                       "Point releases carry security fixes; take it")
    return out


def main():
    if len(sys.argv) > 2 or (len(sys.argv) == 2 and sys.argv[1] in ("-h", "--help")):
        print(__doc__.split("\n\n")[1])
        return 2
    if len(sys.argv) == 2:
        path = sys.argv[1]
    else:
        here = os.path.dirname(os.path.abspath(__file__))
        lists = glob.glob(os.path.join(here, "..", "out", "wincraft-*-components.txt"))
        if not lists:
            print("No components list in out/: build an image, or name one.", file=sys.stderr)
            return 2

        def release(p):
            return [int(x) for x in re.findall(r"\d+", os.path.basename(p))]
        path = max(lists, key=release)
    components = read_components(path)
    name = os.path.basename(path).replace("-components.txt", "")
    print(f"{name}: {len(components)} components")

    arch = arch_findings(components, fetch_json(TRACKER))
    kernel = kernel_findings(components, fetch_json(KERNELS))

    if kernel:
        print("\nKernel:")
        for k in kernel:
            print("- " + k)
    fixable = [f for f in arch if f[3] != "no fix published yet"]
    unfixed = [f for f in arch if f[3] == "no fix published yet"]
    titles = {
        True: "Arch packages with a published fix the image lacks:",
        False: "Listed by the tracker with no fixed version (often out of date there; check by hand):",
    }
    for has_fix, items in ((True, fixable), (False, unfixed)):
        if not items:
            continue
        print("\n" + titles[has_fix])
        for pkg, have, g, what in sorted(items, key=lambda f: (f[0], f[2]["name"])):
            cves = ", ".join(g.get("issues", [])[:4]) + (" ..." if len(g.get("issues", [])) > 4 else "")
            how = what if has_fix else f"recorded against {g.get('affected')}"
            print(f"- {pkg} {have}: {g['name']} ({g.get('severity', '?')}, {g.get('type', '?')}), {how}"
                  + (f": {cves}" if cves else ""))
    if not (kernel or fixable):
        print("\nNo fix to take: the image has every published fix for its components.")
        return 0
    print("\nTo take Arch's fixes, move the date in rootfs/pacman.conf forward; for the kernel, "
          "change kernel/VERSION. See \"Security updates\" in the README.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
