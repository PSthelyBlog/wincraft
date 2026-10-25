#!/usr/bin/env python3
"""assistant-knowledge.py assistant|agent|reference - WinCraft Assistant's and
the WinCraft Agent's reference, as this build installs it.

The model gets, at the start of every chat, its working rules
(src/agent/knowledge/assistant.md or agent.md) and an index of WinCraft's
reference: one line per section of the troubleshooting guide, the README, the
changelog (a section per release) and the notes on the fixes WinCraft carries
for Wine. It reads the sections it needs with the read_reference tool, so a
request carries only what the question calls for.

  assistant   the assistant's rules and the index   -> knowledge.md
  agent       the agent's rules and the index       -> agent.md
  reference   every section, each after a line "@@ <id>" -> reference.md

A guide section's index line comes from an "<!-- index ID: summary -->" line
under its heading; other sections are summed up by their first sentence. The
build fails if the rules name a section (`guide-...`, `readme-...`,
`changelog-...`, `wine-fix-...`) that does not exist.
"""
import os
import re
import sys

W = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SUMMARY_MAX = 150

# The README's sections mostly open with a line that leads into a list, which
# says little; their index lines are written here. A section missing from
# this list is summed up by its first sentence.
README_INDEX = {
    "readme-overview": "what WinCraft OS is in a few lines: ISO size, memory use, live or installed, 64- and "
                       "32-bit programs, the Assistant and the Agent",
    "readme-download": "getting the ISO and checking it, the QEMU command it was tested with, sharing the "
                       "clipboard with a virtual machine",
    "readme-how-it-works": "how WinCraft is made: Linux kernel, Wine and Wine Mono, X.Org, its own init, "
                           "window manager, shell and assistant; one read-only system image with a writable "
                           "layer",
    "readme-what-works-tested": "what has been verified: boot, the desktop shell and its settings, "
                                "networking, internet time, installing, Firefox, the clipboard with QEMU, "
                                "the Assistant, the Agent, features, .NET programs, 3D and sound in a "
                                "virtual machine",
    "readme-what-it-is-not": "known limitations: not every Windows program, little GPU acceleration, no "
                             "Wi-Fi or Bluetooth, no browser or media codecs or printing, .NET in part, the "
                             "time zone and keyboard defaults, what is untested, no security review",
    "readme-wincraft-assistant": "setting the Assistant up and its providers (Anthropic, OpenAI, OpenRouter, "
                                 "OpenAI-compatible servers), what it knows and can do, its permission "
                                 "dialogs, reading web pages, what it cannot do, privacy",
    "readme-features": "what a feature is, how it works (its folder, feature.conf, settings, hooks, services, "
                       "registry, environment), the locales the image has, WinCraft Features and starting "
                       "without features",
    "readme-wincraft-agent": "how the Agent builds a feature in a sandbox, the plan and result the person "
                             "approves, and what it cannot reach",
    "readme-run-it-from-a-build": "running a build in QEMU, the boot options (wc.kbd=, wc.nofeatures, "
                                  "wc.toram, wc.nodata, wc.shell) and installing to a disk",
    "readme-build-it": "building WinCraft from source, patching Wine, security updates",
    "readme-layout": "the source tree",
    "readme-license": "licences: MIT for WinCraft's own code, LGPL for the Wine patches, the components'",
}

# The Wine fixes' own descriptions are written for Wine's developers; their
# index lines say what each one changed for the person.
WINE_INDEX = {
    "wine-fix-0001": "Firefox 153 and later no longer go down a few seconds after opening a page with media, "
                     "such as youtube.com",
    "wine-fix-0002": "Wine's desktop no longer shuts down while the taskbar still uses it, a few seconds "
                     "after the last File Explorer window is closed",
    "wine-fix-0003": "Firefox no longer starts \"Not Responding\" at random",
    "wine-fix-0004": "a click on a program's notification icon runs its default action again when the "
                     "program is not in English (Ditto)",
    "wine-fix-0005": "Google Chrome no longer crashes at every start after it was closed maximized",
    "wine-fix-0006": "Google Chrome's bubbles (Restore pages?, profile, passwords) are drawn instead of "
                     "staying blank white",
}


def read(path):
    with open(os.path.join(W, path), encoding="utf-8") as f:
        return f.read()


def slug(text):
    text = re.sub(r"[`*_\"'’]", "", text.lower())
    return re.sub(r"[^a-z0-9.]+", "-", text).strip("-")[:40].rstrip("-")


def plain(text):
    """Markdown to one plain line."""
    text = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", text)
    text = re.sub(r"[*`]", "", text)
    text = re.sub(r"^\s*(?:[-*]|\d+\.)\s+", "", text)
    return " ".join(text.split())


def first_sentence(body):
    """The first sentence of a section, skipping code, pictures and comments."""
    lines, in_code = [], False
    for line in body.splitlines():
        if line.startswith("```"):
            in_code = not in_code
            continue
        if in_code or line.startswith(("<!--", "![", "#", "|")):
            if lines:
                break
            continue
        if not line.strip():
            if lines:
                break
            continue
        lines.append(line)
    text = plain(" ".join(lines))
    m = re.match(r"(.+?[.:!?])(\s|$)", text)
    text = m.group(1) if m else text
    if len(text) > SUMMARY_MAX:
        text = text[:SUMMARY_MAX].rsplit(" ", 1)[0] + "..."
    return text


def split(markdown, level):
    """Sections at a heading level: (heading, text from the heading on).
    What comes before the first heading is returned as (None, text)."""
    mark = "#" * level + " "
    parts, head, buf, in_code = [], None, [], False
    for line in markdown.splitlines():
        if line.startswith("```"):
            in_code = not in_code
        if not in_code and line.startswith(mark):
            parts.append((head, "\n".join(buf).strip()))
            head, buf = line[len(mark):].strip(), []
        buf.append(line)
    parts.append((head, "\n".join(buf).strip()))
    return [(h, t) for h, t in parts if t]


def sections():
    """[(group, id, title, summary, text)] in index order."""
    out = []

    for head, text in split(read("src/agent/knowledge/troubleshooting.md"), 2):
        if head is None:
            continue
        m = re.search(r"^<!-- index (\S+): (.*?) -->\n\n?", text, re.M)
        if m:
            sid, summary = m.group(1), m.group(2)
            text = text[:m.start()] + text[m.end():]
        else:
            sid, summary = "guide-" + slug(head), first_sentence(text.split("\n", 1)[1])
        out.append(("guide", sid, plain(head), summary, text))

    readme = re.sub(r"^!\[.*\n", "", read("README.md"), flags=re.M)
    for head, text in split(readme, 2):
        sid = "readme-" + slug(head) if head else "readme-overview"
        title = plain(head) if head else plain(text.split("\n", 1)[0].lstrip("# "))
        summary = README_INDEX.get(sid) or first_sentence(text.split("\n", 1)[1])
        out.append(("readme", sid, title, summary, text))

    for head, text in split(read("CHANGELOG.md"), 2):
        if head is None:
            continue
        version, _, date = head.partition(" — ")
        out.append(("changelog", "changelog-" + version.strip(), "%s (%s)" % (version.strip(), date.strip()),
                    first_sentence(text.split("\n", 1)[1]), text))

    pdir = os.path.join(W, "wine", "patches")
    for name in sorted(os.listdir(pdir)) if os.path.isdir(pdir) else []:
        if not name.endswith(".patch"):
            continue
        with open(os.path.join(pdir, name), encoding="utf-8") as f:
            head = []
            for line in f:
                if line.rstrip("\n") == "---":
                    break
                head.append(line.rstrip("\n"))
        subject, body = head[0].rstrip("."), "\n".join(head[1:])
        num = name.split("-", 1)[0]
        sid = "wine-fix-" + num
        out.append(("wine", sid, subject, WINE_INDEX.get(sid) or first_sentence(body),
                    "## Fix %s: %s\n\n%s" % (num, subject, body.strip())))

    seen = set()
    for _, sid, _, _, _ in out:
        if sid in seen:
            sys.exit("assistant-knowledge: two sections named " + sid)
        seen.add(sid)
    return out


GROUPS = [
    ("guide", "Troubleshooting guide: what to check and what to tell the person"),
    ("readme", "WinCraft's documentation (the README)"),
    ("changelog", "Changelog, a section per release, newest first"),
    ("wine", "Fixes WinCraft carries for Wine (wine/patches)"),
]


def index(secs):
    version = read("VERSION").strip()
    lines = ["# WinCraft's reference", "",
             "WinCraft OS %s comes with the reference below, in sections. This index lists them; read the "
             "ones you need with `read_reference`, by id, or with its `search` for the sections that "
             "contain some words. The sections are for exactly this version." % version]
    for group, title in GROUPS:
        lines += ["", "## " + title, ""]
        for g, sid, head, summary, _ in secs:
            if g == group:
                lines.append("- `%s` %s%s" % (sid, head, ": " + summary if summary else ""))
    return "\n".join(lines) + "\n"


def check(rules, secs, name):
    ids = {sid for _, sid, _, _, _ in secs}
    for ref in re.findall(r"`((?:guide|readme|changelog|wine-fix)-[a-z0-9.-]+)`", rules):
        if ref not in ids:
            sys.exit("assistant-knowledge: %s names a section that does not exist: %s" % (name, ref))


def main():
    what = sys.argv[1] if len(sys.argv) > 1 else ""
    secs = sections()
    if what in ("assistant", "agent"):
        rules = read("src/agent/knowledge/%s.md" % what)
        check(rules, secs, what + ".md")
        sys.stdout.write(rules.rstrip("\n") + "\n\n" + index(secs))
    elif what == "reference":
        for _, sid, _, _, text in secs:
            sys.stdout.write("@@ %s\n%s\n\n" % (sid, text))
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
