#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""FAT file system and drive letter tests (Phase 1.4), in QEMU.

Run 1  IDE machine with the boot disk (C:), a data disk with FAT16, non-FAT,
       FAT32 (logical) and FAT12 (logical) partitions, and a floppy (A:).
       Files made by mtools are read back by XenonOS (CRC32); XenonOS then
       creates, writes, appends, copies, renames, deletes, changes attributes
       and hits the expected errors; the floppy is swapped mid-session.
       Afterwards fsck.fat must find every volume clean and mtools must see
       exactly the changes XenonOS made.
Run 2  Boot again from what run 1 left, with an extra disk inserted in front
       of the data disk: files written in run 1 must read back, and the
       sticky letters must keep D:, E:, F: for the same volumes.
Run 3  SATA (AHCI) machine with a GPT disk.
"""
import os
import random
import re
import shutil
import subprocess
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "build", "test", "fat")
SESSION = os.path.join(ROOT, "tools", "qemu-session.py")
ENV = dict(os.environ, MTOOLS_SKIP_CHECK="1", LC_ALL="C.UTF-8", LANG="C.UTF-8")
SECTOR = 512
MiB = 1 << 20

passed = failed = 0


def ok(msg):
    global passed
    passed += 1
    print(f"  \033[32mPASS\033[0m {msg}")


def bad(msg, detail=""):
    global failed
    failed += 1
    print(f"  \033[31mFAIL\033[0m {msg}")
    for line in str(detail).splitlines()[-30:]:
        print(f"       | {line}")


def sh(cmd, inp=None, check=True):
    r = subprocess.run(cmd, input=inp, capture_output=True, env=ENV)
    if check and r.returncode != 0:
        raise RuntimeError(f"{' '.join(map(str, cmd))}: {r.stderr.decode(errors='replace')}")
    return r


def pattern(n, seed):
    """Same xorshift32 stream as the kernel's "write" command."""
    s = seed or 1
    out = bytearray()
    while len(out) < n:
        s ^= (s << 13) & 0xFFFFFFFF
        s ^= s >> 17
        s ^= (s << 5) & 0xFFFFFFFF
        out += s.to_bytes(4, "little")
    return bytes(out[:n])


def rnd(n, seed):
    return random.Random(seed).randbytes(n)


def make_fat(path, kib, fat, label, hidden=0, files=(), cluster_sectors=None):
    """Create a FAT file system image file and fill it with files (path, data)."""
    if os.path.exists(path):
        os.remove(path)
    cmd = ["mkfs.fat", "-C", "-F", str(fat), "-n", label, "-h", str(hidden)]
    if cluster_sectors:
        cmd += ["-s", str(cluster_sectors)]
    sh(cmd + [path, str(kib)])
    made = set()
    for name, data in files:
        parts = name.split("/")
        for i in range(1, len(parts)):
            d = "/".join(parts[:i])
            if d not in made:
                sh(["mmd", "-i", path, "::/" + d])
                made.add(d)
        tmp = os.path.join(OUT, "tmpfile")
        open(tmp, "wb").write(data)
        sh(["mcopy", "-i", path, tmp, "::/" + name])


def put(disk, offset, part_img):
    with open(disk, "r+b") as d, open(part_img, "rb") as p:
        d.seek(offset)
        shutil.copyfileobj(p, d)


def extract(disk, offset, length, out):
    with open(disk, "rb") as d, open(out, "wb") as o:
        d.seek(offset)
        o.write(d.read(length))
    return out


def fsck(img):
    r = sh(["fsck.fat", "-n", img], check=False)
    text = (r.stdout + r.stderr).decode(errors="replace")
    lines = [l for l in text.splitlines() if l.strip() and not l.startswith("fsck.fat") and "files," not in l]
    return lines if (lines or r.returncode) else []


def mread(img, path):
    r = sh(["mcopy", "-n", "-i", img, "::/" + path, "-"], check=False)
    return r.stdout if r.returncode == 0 else None


def mattr(img, path):
    r = sh(["mattrib", "-i", img, "::/" + path], check=False)
    return r.stdout.decode(errors="replace") if r.returncode == 0 else ""


def session(name, args, commands, image=None, disk_out=None):
    log = os.path.join(OUT, f"{name}.log")
    cmd = [SESSION, "--log", log, "--timeout", "120", "--qemu-args", args]
    if image:
        cmd += ["--image", image]
    if disk_out:
        cmd += ["--disk-out", disk_out]
    r = subprocess.run(cmd + commands)
    text = open(log, encoding="utf-8", errors="replace").read() if os.path.exists(log) else ""
    return r.returncode, text


def check(name, status, text, problems):
    if status != 0:
        problems.insert(0, f"session status {status}")
    if problems:
        bad(f"{name}: " + "; ".join(problems[:5]), text)
    else:
        ok(name)


def crc(data):
    return f"{zlib.crc32(data):08x}"


def main():
    os.makedirs(OUT, exist_ok=True)
    boot = os.path.join(ROOT, "build", "xenonos.img")
    if not os.path.exists(boot):
        print("test-fat: build/xenonos.img missing - run 'make image'")
        return 1

    # ---- Disks for run 1 ---------------------------------------------------------
    data = os.path.join(OUT, "data.img")
    with open(data, "wb") as f:
        f.write(rnd(80 * MiB, 1))
    P1, P2, EXT, L5, L6 = 2048, 43008, 51200, 53248, 124928
    sh(["sfdisk", "-q", data], inp=(f"label: dos\nstart={P1},size=40960,type=6\nstart={P2},size=8192,type=83\n"
                                    f"start={EXT},type=5\nstart={L5},size=69632,type=b\nstart={L6},size=8192,type=1\n").encode())
    d_files = [("Documents/Report 2026.txt", rnd(12345, 2)), ("Documents/日記.txt", rnd(777, 3)),
               ("README.TXT", rnd(200, 4)), ("DeleteMe.dat", rnd(4000, 5))]
    e_files = [("Photos/Summer Holiday/IMG_0001.JPG", rnd(300000, 6)), ("music.mp3", rnd(65536, 7)),
               ("Folder With A Very Long Name Indeed/inner file.bin", rnd(5000, 8))]
    f_files = [("SMALL.TXT", rnd(10, 9)), ("lowercase.txt", rnd(1000, 10))]
    a_files = [("FLOPPY.TXT", rnd(3000, 11)), ("Long Floppy Name.doc", rnd(20000, 12))]
    swap_files = [("SECOND.TXT", rnd(500, 13))]
    tmp = os.path.join(OUT, "part.img")
    make_fat(tmp, 40960 // 2, 16, "DATA16", P1, d_files); put(data, P1 * SECTOR, tmp)
    make_fat(tmp, 69632 // 2, 32, "DATA32", L5, e_files, cluster_sectors=1); put(data, L5 * SECTOR, tmp)
    make_fat(tmp, 8192 // 2, 12, "DATA12", L6, f_files); put(data, L6 * SECTOR, tmp)
    floppy = os.path.join(OUT, "floppy.img")
    make_fat(floppy, 1440, 12, "FLOPPYA", 0, a_files)
    swap = os.path.join(OUT, "floppy-swap.img")
    make_fat(swap, 1440, 12, "SECONDDISK", 0, swap_files)
    after_c = os.path.join(OUT, "boot-after-run1.img")
    swap_before = open(swap, "rb").read()

    print("run 1: drive letters, reading and writing on FAT12/16/32:")
    cmds = ["drives"]
    for letter, files in (("D", d_files), ("E", e_files), ("F", f_files), ("A", a_files)):
        for path, _ in files:
            cmds.append(f'fcrc "{letter}:\\{path.replace("/", chr(92))}"')
    cmds += [
        'dir "D:\\Documents"',
        'mkdir "D:\\New Dir"', 'write "D:\\New Dir\\big file.bin" 3000000 11',
        'mkdir E:\\deep', 'mkdir E:\\deep\\a', 'mkdir "E:\\deep\\a\\b c"',
        'write "E:\\deep\\a\\b c\\file with spaces.dat" 123457 13',
        'write F:\\new.txt 777 14',
        'write A:\\written.bin 50000 15',
        'copy "D:\\Documents\\Report 2026.txt" "E:\\Report copy.txt"',
        'ren E:\\music.mp3 "E:\\Photos\\moved music.mp3"',
        'ren "E:\\Photos\\moved music.mp3" D:\\x.mp3',
        'del D:\\DeleteMe.dat',
        'attrib "E:\\Report copy.txt" +r',
        'write "E:\\Report copy.txt" 10 1',
        'del E:\\Photos',
        'mkdir D:\\Documents',
        'fcrc D:\\nope.txt',
        'fcrc D:\\nodir\\x.txt',
        'write D:\\append.txt 1000 21', 'write D:\\append.txt 500 22 append',
        'fcrc "D:\\New Dir\\big file.bin"',
        f'@monitor change floppy0 {swap} raw',
        'dir A:\\', 'fcrc A:\\SECOND.TXT',
        'drives',
        'exit',
    ]
    args = " ".join([f"-drive file={data},format=raw,if=ide,index=1",
                     f"-drive file={floppy},format=raw,if=floppy,index=0"])
    status, text = session("run1", args, cmds, disk_out=after_c)
    problems = []
    for want in [r"A:\s+FAT12\s+FLOPPYA", r"C:\s+FAT32\s+XENON.*\(boot\)", r"D:\s+FAT16\s+DATA16.*HarddiskVolume2",
                 r"E:\s+FAT32\s+DATA32.*HarddiskVolume4", r"F:\s+FAT12\s+DATA12.*HarddiskVolume5",
                 r"A:\s+FAT12\s+SECONDDISK"]:
        if not re.search(want, text):
            problems.append(f"drive list lacks /{want}/")
    if "HarddiskVolume3" in "".join(l for l in text.splitlines() if re.match(r"^[A-Z]:\s", l)):
        problems.append("the non-FAT partition got a drive letter")
    for letter, files in (("D", d_files), ("E", e_files), ("F", f_files), ("A", a_files)):
        for path, content in files:
            p = f'{letter}:\\{path.replace("/", chr(92))}'
            if f"fcrc {p}: {crc(content)} over {len(content)} bytes" not in text:
                problems.append(f"wrong or missing CRC for {p}")
    for want in ["ren E:\\Photos\\moved music.mp3: STATUS_NOT_SAME_DEVICE",
                 "write E:\\Report copy.txt: STATUS_ACCESS_DENIED",
                 "del E:\\Photos: STATUS_DIRECTORY_NOT_EMPTY",
                 "mkdir D:\\Documents: STATUS_OBJECT_NAME_COLLISION",
                 "fcrc D:\\nope.txt: STATUS_OBJECT_NAME_NOT_FOUND",
                 "fcrc D:\\nodir\\x.txt: STATUS_OBJECT_PATH_NOT_FOUND",
                 f"fcrc D:\\New Dir\\big file.bin: {crc(pattern(3000000, 11))} over 3000000 bytes",
                 "medium in Floppy0 changed - remounting",
                 'FastFat: FAT12 mounted on \\Device\\Floppy0: label "SECONDDISK"',
                 f"fcrc A:\\SECOND.TXT: {crc(swap_files[0][1])} over 500 bytes",
                 "Io: disk caches flushed for shutdown"]:
        if want not in text:
            problems.append(f"missing '{want}'")
    check("run 1: letters A: C: D: E: F:, 11 files read, writes/renames/deletes/errors, floppy swap",
          status, text, problems)

    # Host side: fsck and mtools.
    problems = []
    vols = {"D": extract(data, P1 * SECTOR, 40960 * SECTOR, os.path.join(OUT, "D.img")),
            "E": extract(data, L5 * SECTOR, 69632 * SECTOR, os.path.join(OUT, "E.img")),
            "F": extract(data, L6 * SECTOR, 8192 * SECTOR, os.path.join(OUT, "F.img")),
            "A": floppy,
            "C": extract(after_c, MiB, os.path.getsize(after_c) - MiB, os.path.join(OUT, "C.img"))}
    for letter, img in vols.items():
        p = fsck(img)
        if p:
            problems.append(f"fsck.fat on {letter}: " + " / ".join(p[:3]))
    expect = [("D", "New Dir/big file.bin", pattern(3000000, 11)),
              ("D", "append.txt", pattern(1000, 21) + pattern(500, 22)),
              ("E", "deep/a/b c/file with spaces.dat", pattern(123457, 13)),
              ("E", "Report copy.txt", d_files[0][1]),
              ("E", "Photos/moved music.mp3", e_files[1][1]),
              ("F", "new.txt", pattern(777, 14)),
              ("A", "written.bin", pattern(50000, 15))]
    for letter, path, content in expect:
        got = mread(vols[letter], path)
        if got != content:
            problems.append(f"{letter}:/{path}: mtools reads {'nothing' if got is None else f'{len(got)} different bytes'}")
    for letter, path in (("D", "DeleteMe.dat"), ("E", "music.mp3")):
        if mread(vols[letter], path) is not None:
            problems.append(f"{letter}:/{path} still exists")
    if "R" not in mattr(vols["E"], "Report copy.txt").split("::")[0]:
        problems.append("read-only attribute not set on E:/Report copy.txt")
    if open(swap, "rb").read() != swap_before:
        problems.append("the floppy inserted mid-session was modified (it was only read)")
    cfg = (mread(vols["C"], "XenonOS/Config/mountmgr.cfg") or b"").decode(errors="replace")
    for letter, start, size in (("D", P1, 40960), ("E", L5, 69632), ("F", L6, 8192)):
        if f"{letter}: serial=" not in cfg or f"offset={start * SECTOR} length={size * SECTOR}" not in cfg:
            problems.append(f"mountmgr.cfg lacks {letter}:")
    check("run 1 on the host: fsck.fat finds all 5 volumes clean, mtools sees every change exactly, "
          "the swapped-in floppy is untouched", 0, cfg,
          problems)

    # ---- Run 2: persistence and sticky letters ----------------------------------------
    print("run 2: reboot from run 1's disks, with an extra disk in front:")
    extra = os.path.join(OUT, "extra.img")
    make_fat(extra, 16 * 1024, 16, "EXTRA", 0, [("EXTRA.TXT", rnd(100, 20))])
    cmds = ["drives", 'fcrc "D:\\New Dir\\big file.bin"', 'fcrc "E:\\deep\\a\\b c\\file with spaces.dat"',
            "fcrc F:\\new.txt", "fcrc G:\\EXTRA.TXT", "type C:\\XenonOS\\Config\\mountmgr.cfg", "exit"]
    args = " ".join([f"-drive file={extra},format=raw,if=ide,index=1",
                     f"-drive file={data},format=raw,if=ide,index=2",
                     f"-drive file={floppy},format=raw,if=floppy,index=0"])
    status, text = session("run2", args, cmds, image=after_c)
    problems = []
    for want in [r"D:\s+FAT16\s+DATA16", r"E:\s+FAT32\s+DATA32", r"F:\s+FAT12\s+DATA12", r"G:\s+FAT16\s+EXTRA"]:
        if not re.search(want, text):
            problems.append(f"drive list lacks /{want}/")
    for path, content in (("D:\\New Dir\\big file.bin", pattern(3000000, 11)),
                          ("E:\\deep\\a\\b c\\file with spaces.dat", pattern(123457, 13)),
                          ("F:\\new.txt", pattern(777, 14)), ("G:\\EXTRA.TXT", rnd(100, 20))):
        if f"fcrc {path}: {crc(content)} over {len(content)} bytes" not in text:
            problems.append(f"wrong or missing CRC for {path}")
    check("run 2: data from run 1 reads back; D: E: F: keep their letters, the new disk gets G:", status, text,
          problems)

    # ---- Run 3: AHCI + GPT --------------------------------------------------------------
    print("run 3: SATA disk with GPT:")
    gpt = os.path.join(OUT, "gpt.img")
    with open(gpt, "wb") as f:
        f.write(rnd(48 * MiB, 30))
    G1 = 2048
    sh(["sfdisk", "-q", gpt], inp=(f"label: gpt\nstart={G1},size=73728,type=EBD0A0A2-B9E5-4433-87C0-68B6B72699C7,name=Data\n"
                                   "start=75776,size=16384,type=EBD0A0A2-B9E5-4433-87C0-68B6B72699C7,name=Raw\n").encode())
    make_fat(tmp, 73728 // 2, 32, "GPTDATA", G1, [("hello/world.txt", rnd(4321, 31))], cluster_sectors=1)
    put(gpt, G1 * SECTOR, tmp)
    cmds = ["drives", "fcrc D:\\hello\\world.txt", "write \"D:\\hello\\new file.bin\" 1000000 41", "exit"]
    args = f"-machine q35 -drive id=g,file={gpt},format=raw,if=none -device ide-hd,drive=g,bus=ide.1"
    status, text = session("run3", args, cmds)
    problems = []
    if not re.search(r"D:\s+FAT32\s+GPTDATA", text):
        problems.append("D: is not the GPT FAT32 partition")
    if re.search(r"^E:", text, re.M):
        problems.append("the unformatted GPT partition got a letter")
    if f"fcrc D:\\hello\\world.txt: {crc(rnd(4321, 31))}" not in text:
        problems.append("wrong CRC for D:\\hello\\world.txt")
    gimg = extract(gpt, G1 * SECTOR, 73728 * SECTOR, os.path.join(OUT, "G.img"))
    p = fsck(gimg)
    if p:
        problems.append("fsck.fat: " + " / ".join(p[:3]))
    if mread(gimg, "hello/new file.bin") != pattern(1000000, 41):
        problems.append("mtools does not read back hello/new file.bin")
    check("run 3: GPT FAT32 on SATA: read, 1 MB write, fsck.fat clean, mtools reads it back", status, text, problems)

    print(f"fat: {passed} passed, {failed} failed")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
