#!/bin/sh
# usage: ./apply.sh /path/to/Lighthouse
set -e
here=$(cd "$(dirname "$0")" && pwd)
dst=${1:?path to a Lighthouse checkout}
cp "$here/src/port/BananaPanel.cpp" "$dst/src/port/BananaPanel.cpp"
(cd "$dst" && git apply "$here/patches/lighthouse-bananaos.patch")
(cd "$dst/libultraship" && git apply "$here/patches/libultraship-bananaos.patch")
echo "applied"
