#!/bin/sh
# SPDX-License-Identifier: MIT
# Boot a copy of the current image headless and save a PNG of the screen.
#
#   tools/screenshot.sh <output.png> ["kernel cmdline"] [--uefi] [--wait SECONDS]
#
# Used to check the framebuffer console and the stop screen by eye.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUTPNG=$1; shift
CMDLINE=${1:-}; [ $# -gt 0 ] && shift
FW=""; WAIT=6
while [ $# -gt 0 ]; do
    case "$1" in
        --uefi) FW="-bios /usr/share/ovmf/OVMF.fd" ;;
        --wait) WAIT=$2; shift ;;
    esac
    shift
done
export MTOOLS_SKIP_CHECK=1
WORK=$(mktemp -d)
trap 'rm -rf "${WORK:?}"' EXIT
cp --sparse=always "$ROOT/build/xenonos.img" "$WORK/disk.img"
printf 'timeout: 0\n\n/XenonOS\n    protocol: limine\n    path: boot():/XenonOS/System/xnkrnl.elf\n    cmdline: %s\n' \
    "$CMDLINE" > "$WORK/limine.conf"
[ -n "${RESOLUTION:-}" ] && printf '    resolution: %s\n' "$RESOLUTION" >> "$WORK/limine.conf"
mcopy -o -i "$WORK/disk.img@@1M" "$WORK/limine.conf" ::/boot/limine/limine.conf
# shellcheck disable=SC2086
qemu-system-x86_64 -m 256M $FW ${QEMU_EXTRA:-} -drive file="$WORK/disk.img",format=raw,if=ide \
    -display none -serial null -monitor unix:"$WORK/mon.sock",server,nowait &
QPID=$!
python3 - "$WORK/mon.sock" "$WORK/screen.ppm" "$WAIT" "$ROOT/tools/qemu-session.py" "${TYPE:-}" <<'PY'
import importlib.util, socket, sys, time
sock, ppm, wait, session, typed = sys.argv[1], sys.argv[2], float(sys.argv[3]), sys.argv[4], sys.argv[5]
spec = importlib.util.spec_from_file_location("session", session)
mod = importlib.util.module_from_spec(spec); spec.loader.exec_module(mod)
for _ in range(50):
    try:
        s = socket.socket(socket.AF_UNIX); s.connect(sock); break
    except OSError:
        time.sleep(0.1)
time.sleep(wait)
# TYPE="cmd1;cmd2": type each command on the PS/2 keyboard, then wait a bit.
for line in [c for c in typed.split(";") if c]:
    for ch in line + "\n":
        s.send(f"sendkey {mod.qemu_key(ch)}\n".encode()); time.sleep(0.04)
    time.sleep(1.0)
s.send(f"screendump {ppm}\n".encode()); time.sleep(1.0)
s.send(b"quit\n"); time.sleep(0.3)
PY
wait $QPID || true
python3 -c "
import sys
from PIL import Image
Image.open(sys.argv[1]).save(sys.argv[2])" "$WORK/screen.ppm" "$OUTPNG"
echo "screenshot: $OUTPNG"
