#!/bin/sh
# clones the two libretro cores at the commits this was built against and
# applies our patches (determinism fixes for online sync, qsound z80 and
# cps1 drawing on their own threads)
set -e
cd "$(dirname "$0")"

get() {
   [ -d "$1" ] && { echo "$1 already there, skipping"; return; }
   git clone -q "$2" "$1"
   git -C "$1" checkout -q "$3"
   git -C "$1" apply --whitespace=nowarn "../$1.patch"
}

get fbalpha2012 https://github.com/libretro/fbalpha2012.git 0ce31536bef3162fe7e69ff5f555334ec4913cef
get mame2000 https://github.com/libretro/mame2000-libretro.git f099ba44c7664906fd7e01cbed89d13a7e32dee1
