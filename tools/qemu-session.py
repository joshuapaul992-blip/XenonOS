#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Drive a XenonOS shell over the serial port.

Boots a throw-away copy of the disk image, waits for the prompt, types each
command, and records what the system printed back. Used by tools/test.sh,
and handy by hand:

    tools/qemu-session.py --prompt "kd> " "objdir \\" "threads"

Exit status: 0 if every command ran and the VM exited through the debug-exit
device after the final "exit" command, 1 otherwise. The transcript goes to
stdout (or --log FILE).
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ANSI = re.compile(rb"\x1b\[[0-9;?]*[A-Za-z]")


KEYS = {" ": "spc", "\n": "ret", "\\": "backslash", "/": "slash", "-": "minus", ".": "dot",
        ",": "comma", "=": "equal", ";": "semicolon", "'": "apostrophe", "[": "bracket_left",
        "]": "bracket_right", "`": "grave_accent"}
SHIFTED = {"_": "minus", "?": "slash", ":": "semicolon", "|": "backslash", "+": "equal",
           "!": "1", "@": "2", "#": "3", "$": "4", "%": "5", "^": "6", "&": "7", "*": "8",
           "(": "9", ")": "0", "<": "comma", ">": "dot", '"': "apostrophe"}


def qemu_key(ch):
    """QEMU monitor 'sendkey' name for a character (US layout)."""
    if ch.isalpha():
        return f"shift-{ch.lower()}" if ch.isupper() else ch
    if ch.isdigit():
        return ch
    if ch in KEYS:
        return KEYS[ch]
    if ch in SHIFTED:
        return "shift-" + SHIFTED[ch]
    raise ValueError(f"cannot type {ch!r} on the PS/2 keyboard")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("commands", nargs="*")
    ap.add_argument("--image", default=os.path.join(ROOT, "build", "xenonos.img"))
    ap.add_argument("--prompt", default="kd> ")
    ap.add_argument("--cmdline", default="xenon.debugexit xenon.interactive")
    ap.add_argument("--timeout", type=float, default=60.0, help="seconds per step")
    ap.add_argument("--log", help="write the transcript here")
    ap.add_argument("--qemu-args", default="", help="extra QEMU arguments")
    ap.add_argument("--input", choices=["serial", "ps2"], default="serial",
                    help="type over COM1 or with the emulated PS/2 keyboard")
    args = ap.parse_args()

    work = tempfile.mkdtemp(prefix="xenon-session-")
    try:
        disk = os.path.join(work, "disk.img")
        shutil.copyfile(args.image, disk)
        conf = os.path.join(work, "limine.conf")
        with open(conf, "w") as f:
            f.write("timeout: 0\n\n/XenonOS\n    protocol: limine\n"
                    "    path: boot():/XenonOS/System/xnkrnl.elf\n"
                    f"    cmdline: {args.cmdline}\n")
        env = dict(os.environ, MTOOLS_SKIP_CHECK="1")
        subprocess.run(["mcopy", "-o", "-i", disk + "@@1M", conf, "::/boot/limine/limine.conf"],
                       check=True, env=env)
        mon_path = os.path.join(work, "monitor.sock")
        monitor = ["-monitor", f"unix:{mon_path},server,nowait"] if args.input == "ps2" else ["-monitor", "none"]
        cmd = ["qemu-system-x86_64", "-m", "256M", "-no-reboot", "-display", "none",
               *monitor, "-serial", "stdio",
               "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
               "-drive", f"file={disk},format=raw,if=ide"] + args.qemu_args.split()
        qemu = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                stderr=subprocess.DEVNULL)
        os.set_blocking(qemu.stdout.fileno(), False)
        transcript = bytearray()
        prompt = args.prompt.encode()

        def read_until(marker, timeout):
            start = len(transcript)
            deadline = time.time() + timeout
            while time.time() < deadline:
                try:
                    chunk = qemu.stdout.read()
                except BlockingIOError:
                    chunk = None
                if chunk:
                    transcript.extend(chunk)
                    clean = ANSI.sub(b"", bytes(transcript[start:]))
                    if marker in clean:
                        return True
                elif qemu.poll() is not None:
                    return False
                else:
                    time.sleep(0.02)
            return False

        mon = None
        if args.input == "ps2":
            import socket
            for _ in range(100):
                try:
                    mon = socket.socket(socket.AF_UNIX)
                    mon.connect(mon_path)
                    break
                except OSError:
                    time.sleep(0.05)

        def type_line(line):
            if mon is None:
                # Type slowly enough that a 16-byte UART FIFO never overflows.
                for ch in (line + "\r").encode():
                    qemu.stdin.write(bytes([ch]))
                    qemu.stdin.flush()
                    time.sleep(0.002)
                return
            for ch in line + "\n":
                mon.sendall(f"sendkey {qemu_key(ch)}\n".encode())
                time.sleep(0.03)
            try:
                mon.setblocking(False)
                mon.recv(65536)
            except OSError:
                pass
            mon.setblocking(True)

        ok = read_until(prompt, args.timeout)
        for c in args.commands:
            if not ok:
                break
            type_line(c)
            if c.strip() == "exit":
                break
            ok = read_until(prompt, args.timeout)
        status = None
        try:
            status = qemu.wait(timeout=args.timeout)
        except subprocess.TimeoutExpired:
            qemu.kill()
            ok = False
        try:
            rest = qemu.stdout.read()
            if rest:
                transcript.extend(rest)
        except (BlockingIOError, ValueError):
            pass

        text = ANSI.sub(b"", bytes(transcript)).replace(b"\r", b"").decode("utf-8", "replace")
        if args.log:
            with open(args.log, "w") as f:
                f.write(text)
        else:
            sys.stdout.write(text)
        # isa-debug-exit: (0x10 << 1) | 1 = 33 means the kernel ended the run itself.
        return 0 if ok and status == 33 else 1
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
