#!/bin/sh
# SPDX-License-Identifier: MIT
# Increment the build number. Called once by every "make image".
#
# BUILD_NUMBER holds the number of the most recent image build, so a fresh
# checkout (BUILD_NUMBER = 0) produces "build 1" on its first image build.
# Set BUILD_NUMBER_FILE to work on a different file (used by the tests).
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
FILE=${BUILD_NUMBER_FILE:-$ROOT/BUILD_NUMBER}
OLD=$(tr -d ' \n' < "$FILE" 2>/dev/null || echo 0)
case "$OLD" in
    ''|*[!0-9]*) echo "bump-build: $FILE does not contain a number: '$OLD'" >&2; exit 1 ;;
esac
NEW=$((OLD + 1))
echo "$NEW" > "$FILE.tmp.$$" && mv "$FILE.tmp.$$" "$FILE"
echo "$NEW"
