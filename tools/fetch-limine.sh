#!/bin/sh
# Download the pinned Limine binary release into third_party/limine and build
# its host-side installer. Does nothing if it is already there.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
DIR=$ROOT/third_party/limine
TAG=v9.6.7-binary
URL=https://github.com/limine-bootloader/limine.git

if [ ! -f "$DIR/limine-bios.sys" ]; then
    echo "  FETCH   Limine $TAG"
    git clone -q -c advice.detachedHead=false --depth=1 --branch=$TAG "$URL" "$DIR"
fi
if [ ! -x "$DIR/limine" ]; then
    echo "  BUILD   limine installer"
    make -C "$DIR" -s >/dev/null
fi
