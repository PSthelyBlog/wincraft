#!/bin/sh
# Render the WinCraft Assistant icon (a speech bubble in WinCraft blue) to a
# multi-resolution .ico.
# usage: mkicon.sh <output.ico>
set -e
out=$1
tmp=$(mktemp -d)
magick -size 256x256 xc:none \
    -fill '#2b78c5' -draw 'roundrectangle 20,28 236,184 36,36' \
    -fill '#2b78c5' -draw 'polygon 64,176 112,176 52,236' \
    -fill '#4aa5f0' -draw 'roundrectangle 28,36 228,174 30,30' \
    -fill '#4aa5f0' -draw 'polygon 70,168 108,168 60,222' \
    -fill white -draw 'circle 80,105 80,123' -draw 'circle 128,105 128,123' -draw 'circle 176,105 176,123' \
    "$tmp/icon.png"
magick "$tmp/icon.png" -define icon:auto-resize=64,48,32,24,16 "$out"
rm -rf "$tmp"
