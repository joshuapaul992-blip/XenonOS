#!/bin/sh
# XenonOS automated tests (Phases 1.0 to 1.3).
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

# boot NAME MACHINE(pc|q35) FIRMWARE(bios|uefi) "kernel cmdline" ["extra qemu args"]
# Leaves the serial log in $OUT/NAME.log and the QEMU exit status in $STATUS.
boot() {
    name=$1 machine=$2 fw=$3 cmdline=$4 extra=${5:-}
    img=$OUT/$name.img
    log=$OUT/$name.log
    cp --sparse=always "$IMG" "$img"
    title=$(printf '%s' "$BANNER")
    printf 'timeout: 0\n\n/%s\n    protocol: limine\n    path: boot():/XenonOS/System/xnkrnl.elf\n    cmdline: %s\n' \
        "$title" "$cmdline" > "$OUT/$name.conf"
    mcopy -o -i "$img@@1M" "$OUT/$name.conf" ::/boot/limine/limine.conf
    set -- -no-reboot -display none -serial file:"$log" \
        -device isa-debug-exit,iobase=0xf4,iosize=0x04
    case " $extra " in *" -m "*) ;; *) set -- "$@" -m 256M ;; esac
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
    # shellcheck disable=SC2086
    timeout "$TIMEOUT" qemu-system-x86_64 "$@" $extra >/dev/null 2>"$OUT/$name.qemu.err"
    STATUS=$?
    rm -f "${img:?}"
}

# expect_boot NAME MACHINE FIRMWARE FIRMWARE-TEXT ["extra qemu args"] ["text that must be logged" ...]
expect_boot() {
    name=$1 machine=$2 fw=$3 fwtext=$4 extra=${5:-}
    shift 4; [ $# -gt 0 ] && shift
    boot "$name" "$machine" "$fw" "xenon.debugexit xenon.selftest" "$extra"
    [ "$STATUS" = skip ] && return
    log=$OUT/$name.log
    missing=""
    for want in "$BANNER" "on $fwtext" "Phase 1.3 complete" "$@"; do
        grep -qF -- "$want" "$log" || missing="$missing '$want'"
    done
    selftest=$(grep -o "SELFTEST: [0-9]* passed, [0-9]* failed" "$log" | tail -1)
    if [ "$STATUS" = 33 ] && [ -z "$missing" ] && echo "$selftest" | grep -q ", 0 failed"; then
        ok "$name: boots on $fwtext, $selftest"
    else
        bad "$name: qemu status $STATUS (expected 33), ${selftest:-no self-test summary}${missing:+, missing:$missing}" "$log"
        grep "SELFTEST: .* FAIL" "$log" | sed 's/^/       ! /'
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

# 3. Booting and self-tests
echo "boot + self-tests:"
expect_boot bios-pc-ide   pc  bios "BIOS" "" \
    "PCI: using legacy configuration ports" "TSC (HPET calibrated)" "PCI: 00:01.1 8086:7010 Intel IDE controller"
expect_boot bios-q35-ahci q35 bios "BIOS" "" \
    "PCI: ECAM segment 0" "SATA controller (AHCI)"
expect_boot uefi-q35-ahci q35 uefi "UEFI (64-bit)" "" \
    "PCI: ECAM segment 0"
expect_boot smp4          q35 bios "BIOS" "-smp 4" \
    "HAL: 4 CPUs present"
expect_boot highmem-5G    q35 bios "BIOS" "-m 5G" \
    "Mm: 5"
expect_boot lowmem-48M    pc  bios "BIOS" "-m 48M"
expect_boot no-hpet       q35 bios "BIOS" "-machine hpet=off" \
    "TSC (ACPI PM timer calibrated)"
expect_boot usb-net       q35 bios "BIOS" "-device qemu-xhci -device usb-ehci -device piix3-usb-uhci -device pci-ohci -device e1000 -device rtl8139" \
    "USB 3.x controller (xHCI)" "USB 2.0 controller (EHCI)" "USB 1.1 controller (UHCI)" "USB 1.1 controller (OHCI)" \
    "8086:100e Intel Ethernet controller" "10ec:8139 Realtek Ethernet controller"

# 4. Kernel debug shell, driven over COM1 and the PS/2 keyboard
# expect_session NAME INPUT "qemu args" -- command... -- expected text...
expect_session() {
    name=$1 input=$2 qargs=$3
    shift 3; [ "$1" = "--" ] && shift
    cmds=""; n=0
    while [ $# -gt 0 ] && [ "$1" != "--" ]; do
        n=$((n + 1)); eval "cmd_$n=\$1"; shift
    done
    [ "$1" = "--" ] && shift
    set -- "$@"
    log=$OUT/$name.log
    i=1; argv=""
    # Rebuild the command list as positional arguments for qemu-session.py.
    session_args() {
        j=1
        while [ $j -le $n ]; do eval "printf '%s\\0' \"\$cmd_$j\""; j=$((j + 1)); done
    }
    session_args | xargs -0 "$ROOT/tools/qemu-session.py" --input "$input" --qemu-args "$qargs" \
        --log "$log" --timeout 60 >/dev/null 2>&1
    st=$?
    missing=""
    for want in "$@"; do
        grep -qF -- "$want" "$log" || missing="$missing '$want'"
    done
    if [ $st = 0 ] && [ -z "$missing" ]; then
        ok "$name: $n commands typed over $input, output as expected"
    else
        bad "$name: session status $st${missing:+, missing:$missing}" "$log"
    fi
}

echo "debug shell:"
expect_session kd-serial serial "" -- \
    'objdir \' 'objdir \ObjectTypes' 'objdir \DosDevices' threads ps 'threadtest 100' uptime mem irq exit -- \
    'Directory of \' 'DosDevices               SymbolicLink   -> \GLOBAL??' \
    'Thread                   Type' 'Directory of \GLOBAL??' 'KdShell' '       0  System' \
    'threadtest: 100 threads finished, 1000 mutex acquisitions (expected 1000)' 'clock' 'Leaving the test run.'
expect_session kd-ps2 ps2 "" -- \
    ver 'objdir \Device' 'Sleep 20' exit -- \
    "$BANNER" 'Directory of \Device' 'slept 2' 'Leaving the test run.'
OVMF=$(find_ovmf)
if [ -n "$OVMF" ]; then
    expect_session kd-ps2-uefi ps2 "-bios $OVMF" -- \
        ver threads exit -- \
        "$BANNER" 'KdShell' 'Leaving the test run.'
fi

# 5. Storage: raw sector reads and writes on IDE, SATA and floppy disks
echo "storage:"
"$ROOT/tools/test-storage.py" > "$OUT/storage.out" 2>&1
grep -E "PASS|FAIL|^       \|" "$OUT/storage.out" | grep -v "^storage:"
PASS=$((PASS + $(grep -c "PASS" "$OUT/storage.out")))
FAIL=$((FAIL + $(grep -c "FAIL" "$OUT/storage.out")))

# 6. Stop screen
echo "stop screen:"
expect_crash pagefault 0x00000050 PAGE_FAULT_IN_NONPAGED_AREA
expect_crash divide    0x0000001E "(0x00000000C0000094"
expect_crash opcode    0x0000001E "(0x00000000C000001D"
expect_crash gpf       0x0000001E "(0x00000000C0000005"
expect_crash manual    0x000000E2 MANUALLY_INITIATED_CRASH
expect_crash assert    0x00000139 "Assertion failed"
expect_crash stackoverflow 0x0000007F "(0x0000000000000008"
expect_crash doublefree    0x000000C2 "(0x0000000000000007"
expect_crash writecode     0x00000050 "protection violation, write"

echo
echo "$PASS passed, $FAIL failed"
[ $FAIL = 0 ]
