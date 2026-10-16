#!/bin/bash
# assistant-knowledge.sh - print WinCraft Assistant's reference pack.
#
# The assistant sends it to the model at the start of every chat: its working
# rules and troubleshooting guide (src/agent/knowledge), then WinCraft's own
# documentation as of this build, so its knowledge matches the system it
# ships in. The build installs the result as
# /usr/share/wincraft/assistant/knowledge.md.
set -euo pipefail
W="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

cat "$W/src/agent/knowledge/assistant.md"
echo
cat "$W/src/agent/knowledge/troubleshooting.md"
echo
echo "# Reference: WinCraft OS $(cat "$W/VERSION")"
echo
echo "The project's README, changelog and the notes on the fixes it carries for Wine."
echo
# headings move down one level, under the reference heading
sed -e '/^!\[/d' -e 's/^#/##/' "$W/README.md"
echo
sed -e 's/^#/##/' "$W/CHANGELOG.md"
echo
echo "## Fixes WinCraft carries for Wine (wine/patches)"
for p in "$W"/wine/patches/*.patch; do
    [ -e "$p" ] || continue
    echo
    echo "### $(basename "$p" .patch)"
    echo
    sed -n '1,/^---$/p' "$p" | sed '$d'
done
