#!/bin/sh
# Turn addresses from a stop screen into function names and source lines.
#   usage: tools/symbolize.sh 0xffffffff80001234 [0x... ...]
ROOT=$(cd "$(dirname "$0")/.." && pwd)
exec addr2line -e "$ROOT/build/xnkrnl.elf" -f -C -p "$@"
