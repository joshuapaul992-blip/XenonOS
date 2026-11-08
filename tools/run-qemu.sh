#!/bin/sh
# SPDX-License-Identifier: MIT
# Run XenonOS in QEMU.
#
#   tools/run-qemu.sh [--uefi] [--q35] [--headless] [--img FILE] [-- extra qemu args]
#
# The serial port (COM1) is connected to this terminal; the screen opens in a
# window unless --headless is given.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
IMG=$ROOT/build/xenonos.img
UEFI=0; Q35=0; HEADLESS=0
while [ $# -gt 0 ]; do
    case "$1" in
        --uefi) UEFI=1 ;;
        --q35) Q35=1 ;;
        --headless) HEADLESS=1 ;;
        --img) IMG=$2; shift ;;
        --) shift; break ;;
        *) echo "run-qemu: unknown option $1" >&2; exit 2 ;;
    esac
    shift
done

find_ovmf() {
    for f in /usr/share/ovmf/OVMF.fd /usr/share/OVMF/OVMF.fd /usr/share/qemu/OVMF.fd \
             /usr/share/edk2/x64/OVMF.fd /usr/share/edk2-ovmf/x64/OVMF.fd; do
        [ -f "$f" ] && { echo "$f"; return; }
    done
    echo "run-qemu: OVMF firmware not found (install the 'ovmf' package)" >&2
    exit 1
}

set -- -m 512M -no-reboot "$@"
if [ $Q35 = 1 ]; then
    set -- -machine q35 -drive id=c,file="$IMG",format=raw,if=none -device ahci,id=ahci \
        -device ide-hd,drive=c,bus=ahci.0 "$@"
else
    set -- -drive file="$IMG",format=raw,if=ide "$@"
fi
[ $UEFI = 1 ] && set -- -bios "$(find_ovmf)" "$@"
if [ $HEADLESS = 1 ]; then
    set -- -display none -serial stdio "$@"
else
    set -- -serial stdio "$@"
fi
[ -e /dev/kvm ] && [ -w /dev/kvm ] && set -- -enable-kvm -cpu host "$@"
exec qemu-system-x86_64 "$@"
