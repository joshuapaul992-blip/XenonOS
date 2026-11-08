#!/bin/sh
# XenonOS automated tests for Phase 1.0.
#
# Uses the image from the last "make image" (it does not build a new image,
# so running the tests does not change the build number). Each boot runs on a
# throw-away copy of the image whose limine.conf is rewritten with
# "timeout: 0" and the kernel options under test.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
IMG=${IMG:-$ROOT/build/xenonos.img}
OUT=$ROOT/build/test
TIMEOUT=${TIMEOUT:-60}
VERSION=$(tr -d ' \n' < "$ROOT/VERSION")
BUILD=$(tr -d ' \n' < "$ROOT/BUILD_NUMBER")
BANNER="XenonOS $VERSION (build $BUILD)"
PASS=0; FAIL=0
export MTOOLS_SKIP_CHECK=1

mkdir -p "$OUT"
[ -f "$IMG" ] || { echo "test: $IMG not found - run 'make image' first" >&2; exit 1; }

ok()   { PASS=$((PASS + 1)); printf '  \033[32mPASS\033[0m %s\n' "$1"; }
bad()  { FAIL=$((FAIL + 1)); printf '  \033[31mFAIL\033[0m %s\n' "$1"; [ -n "${2:-}" ] && sed 's/^/       | /' "$2" | tail -25; }

find_ovmf() {
    for f in /usr/share/ovmf/OVMF.fd /usr/share/OVMF/OVMF.fd /usr/share/qemu/OVMF.fd \
             /usr/share/edk2/x64/OVMF.fd /usr/share/edk2-ovmf/x64/OVMF.fd; do
        [ -f "$f" ] && { echo "$f"; return; }
    done
}

# boot NAME MACHINE(pc|q35) FIRMWARE(bios|uefi) "kernel cmdline"
# Leaves the serial log in $OUT/NAME.log and the QEMU exit status in $STATUS.
boot() {
    name=$1 machine=$2 fw=$3 cmdline=$4
    img=$OUT/$name.img
    log=$OUT/$name.log
    cp --sparse=always "$IMG" "$img"
    title=$(printf '%s' "$BANNER")
    printf 'timeout: 0\n\n/%s\n    protocol: limine\n    path: boot():/XenonOS/System/xnkrnl.elf\n    cmdline: %s\n' \
        "$title" "$cmdline" > "$OUT/$name.conf"
    mcopy -o -i "$img@@1M" "$OUT/$name.conf" ::/boot/limine/limine.conf
    set -- -m 256M -no-reboot -display none -serial file:"$log" \
        -device isa-debug-exit,iobase=0xf4,iosize=0x04
    if [ "$machine" = q35 ]; then
        set -- "$@" -machine q35 -drive id=c,file="$img",format=raw,if=none \
            -device ahci,id=ahci -device ide-hd,drive=c,bus=ahci.0
    else
        set -- "$@" -drive file="$img",format=raw,if=ide
    fi
    if [ "$fw" = uefi ]; then
        ovmf=$(find_ovmf)
        [ -n "$ovmf" ] || { echo "  SKIP $name (no OVMF firmware)"; STATUS=skip; return; }
        set -- "$@" -bios "$ovmf"
    fi
    timeout "$TIMEOUT" qemu-system-x86_64 "$@" >/dev/null 2>"$OUT/$name.qemu.err"
    STATUS=$?
    rm -f "${img:?}"
}

expect_boot() {
    name=$1 machine=$2 fw=$3 fwtext=$4
    boot "$name" "$machine" "$fw" "xenon.debugexit"
    [ "$STATUS" = skip ] && return
    log=$OUT/$name.log
    if [ "$STATUS" = 33 ] && grep -qF "$BANNER" "$log" && grep -qF "on $fwtext" "$log" \
       && grep -q "Phase 1.0 bring-up complete" "$log"; then
        ok "$name: boots to '$BANNER' on $fwtext"
    else
        bad "$name: boot (qemu status $STATUS, expected 33)" "$log"
    fi
}

expect_crash() {
    kind=$1 code=$2 text=$3
    name=crash-$kind
    boot "$name" pc bios "xenon.debugexit xenon.crash=$kind"
    log=$OUT/$name.log
    if [ "$STATUS" = 65 ] && grep -qF "*** STOP: $code" "$log" && grep -qF "$text" "$log" \
       && grep -qF "$BANNER has stopped" "$log"; then
        ok "$name: stop screen $code $text"
    else
        bad "$name: expected stop $code $text (qemu status $STATUS, expected 65)" "$log"
    fi
}

echo "XenonOS tests - $BANNER"

# 1. Build-number counter
echo "build number:"
T=$(mktemp)
echo 41 > "$T"
n1=$(BUILD_NUMBER_FILE=$T "$ROOT/tools/bump-build.sh")
n2=$(BUILD_NUMBER_FILE=$T "$ROOT/tools/bump-build.sh")
BUILD_NUMBER_FILE=$T "$ROOT/tools/gen-version.sh" "$T.h"
if [ "$n1" = 42 ] && [ "$n2" = 43 ] && grep -q '"XenonOS '"$VERSION"' (build 43)"' "$T.h"; then
    ok "bump-build.sh counts 41 -> 42 -> 43 and version.h follows"
else
    bad "bump-build.sh gave '$n1' '$n2'"
fi
echo 0 > "$T"
[ "$(BUILD_NUMBER_FILE=$T "$ROOT/tools/bump-build.sh")" = 1 ] && ok "a fresh checkout's first image build is build 1" \
    || bad "first build is not build 1"
rm -f "${T:?}" "${T:?}.h"
if grep -qF "/$BANNER" "$OUT/../limine.conf" 2>/dev/null || \
   mtype -i "$IMG@@1M" ::/boot/limine/limine.conf 2>/dev/null | grep -qF "/$BANNER"; then
    ok "boot menu entry is '$BANNER'"
else
    bad "boot menu entry does not name '$BANNER'"
fi

# 2. Disk image
echo "disk image:"
dd if="$IMG" of="$OUT/c.part" bs=1M skip=1 status=none
if fsck.fat -n "$OUT/c.part" >"$OUT/fsck.log" 2>&1; then
    ok "C: is a clean FAT32 volume (fsck.fat -n)"
else
    bad "fsck.fat reports problems on C:" "$OUT/fsck.log"
fi
rm -f "${OUT:?}/c.part"
if mdir -i "$IMG@@1M" ::/XenonOS/System/xnkrnl.elf >/dev/null 2>&1; then
    ok "C:/XenonOS/System/xnkrnl.elf present"
else
    bad "kernel missing from C:/XenonOS/System"
fi

# 3. Booting
echo "boot:"
expect_boot bios-pc-ide  pc  bios "BIOS"
expect_boot bios-q35-ahci q35 bios "BIOS"
expect_boot uefi-q35-ahci q35 uefi "UEFI (64-bit)"

# 4. Stop screen
echo "stop screen:"
expect_crash pagefault 0x00000050 PAGE_FAULT_IN_NONPAGED_AREA
expect_crash divide    0x0000001E "(0x00000000C0000094"
expect_crash opcode    0x0000001E "(0x00000000C000001D"
expect_crash gpf       0x0000001E "(0x00000000C0000005"
expect_crash manual    0x000000E2 MANUALLY_INITIATED_CRASH
expect_crash assert    0x00000139 "Assertion failed"

echo
echo "$PASS passed, $FAIL failed"
[ $FAIL = 0 ]
