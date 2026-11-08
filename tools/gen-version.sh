#!/bin/sh
# SPDX-License-Identifier: MIT
# Generate build/include/xenon/version.h from VERSION and BUILD_NUMBER.
# The file is only rewritten when its contents change.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:-$ROOT/build/include/xenon/version.h}
VERSION=$(tr -d ' \n' < "$ROOT/VERSION")
BUILD=$(tr -d ' \n' < "${BUILD_NUMBER_FILE:-$ROOT/BUILD_NUMBER}")
DATE=$(date -u +"%Y-%m-%d %H:%M UTC")
mkdir -p "$(dirname "$OUT")"
TMP="$OUT.tmp.$$"
sed -e "s/@VERSION@/$VERSION/g" -e "s/@BUILD@/$BUILD/g" -e "s/@DATE@/$DATE/g" \
    "$ROOT/include/xenon/version.h.in" > "$TMP"
if [ -f "$OUT" ] && cmp -s "$TMP" "$OUT"; then
    rm -f "$TMP"
else
    mv "$TMP" "$OUT"
fi
