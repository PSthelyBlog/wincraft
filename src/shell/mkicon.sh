#!/bin/sh
# Render the WinCraft logo (an isometric block) to a multi-resolution .ico.
# usage: mkicon.sh <output.ico>
set -e
out=$1
tmp=$(mktemp -d)
# Cube corners on a 256 px canvas: centre (128,128), radius 112.
magick -size 256x256 xc:none \
    -fill '#8fd0ff' -draw 'polygon 128,16 225,72 128,128 31,72' \
    -fill '#4aa5f0' -draw 'polygon 31,72 128,128 128,240 31,184' \
    -fill '#2b78c5' -draw 'polygon 128,128 225,72 225,184 128,240' \
    "$tmp/logo.png"
magick "$tmp/logo.png" -define icon:auto-resize=64,48,32,24,16 "$out"
rm -rf "$tmp"
