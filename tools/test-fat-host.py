#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Host tests for the FAT core (drivers/fs/fatcore.c), built into build/host/xfat.

1. Stress: thousands of random operations (create, write, truncate, delete,
   rename/move, delete-while-open, remount) on FAT12, FAT16 and FAT32 images,
   checked against an in-memory model inside xfat.
2. fsck.fat -n must find nothing wrong with the result.
3. mtools must read back exactly what XenonOS wrote: every path (with its
   case), size and CRC32 of the tree extracted with mcopy matches.
4. The other way round: a random tree written with mtools must be read back
   exactly by the XenonOS core.

Prints PASS/FAIL lines like tools/test.sh; exit status 1 on any failure.
"""
import os
import random
import shutil
import subprocess
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
XFAT = os.path.join(ROOT, "build", "host", "xfat")
OUT = os.path.join(ROOT, "build", "test", "fat-host")
ENV = dict(os.environ, MTOOLS_SKIP_CHECK="1", LC_ALL="C.UTF-8", LANG="C.UTF-8")
SIZES = {12: 1440, 16: 16384, 32: 65536}      # KiB

passed = failed = 0


def ok(msg):
    global passed
    passed += 1
    print(f"  \033[32mPASS\033[0m {msg}")


def bad(msg, detail=""):
    global failed
    failed += 1
    print(f"  \033[31mFAIL\033[0m {msg}")
    for line in str(detail).splitlines()[-20:]:
        print(f"       | {line}")


def sh(cmd, check=True):
    r = subprocess.run(cmd, capture_output=True, text=True, env=ENV)
    if check and r.returncode != 0:
        raise RuntimeError(f"{' '.join(cmd)}: {r.stderr.strip() or r.stdout.strip()}")
    return r


def mkfs(path, fat, label):
    if os.path.exists(path):
        os.remove(path)
    sh(["mkfs.fat", "-C", "-F", str(fat), "-n", label, path, str(SIZES[fat])])


def fsck_problems(path):
    r = sh(["fsck.fat", "-n", path], check=False)
    lines = [l for l in r.stdout.splitlines() + r.stderr.splitlines()
             if l.strip() and not l.startswith("fsck.fat") and "files," not in l]
    return lines if (lines or r.returncode != 0) else []


def read_manifest(text):
    entries = {}
    for line in text.splitlines():
        if line.startswith("D "):
            entries[line[2:]] = ("D",)
        elif line.startswith("F "):
            _, size, crc, path = line.split(" ", 3)
            entries[path] = ("F", int(size), int(crc, 16))
    return entries


def tree_manifest(top):
    entries = {}
    for dirpath, dirnames, filenames in os.walk(top):
        rel = os.path.relpath(dirpath, top)
        for d in dirnames:
            entries[d if rel == "." else os.path.join(rel, d)] = ("D",)
        for f in filenames:
            p = os.path.join(dirpath, f)
            data = open(p, "rb").read()
            key = f if rel == "." else os.path.join(rel, f)
            entries[key] = ("F", len(data), zlib.crc32(data))
    return entries


def diff(a, b, an, bn):
    out = []
    for k in sorted(set(a) | set(b)):
        if k not in a:
            out.append(f"only in {bn}: {k}")
        elif k not in b:
            out.append(f"only in {an}: {k}")
        elif a[k] != b[k]:
            out.append(f"differs: {k}: {an} {a[k]} vs {bn} {b[k]}")
    return out


def stress_case(fat, seed, ops):
    img = os.path.join(OUT, f"stress{fat}-{seed}.img")
    man = os.path.join(OUT, f"stress{fat}-{seed}.manifest")
    mkfs(img, fat, f"STRESS{fat}")
    r = sh([XFAT, img, "stress", str(seed), str(ops), man], check=False)
    name = f"FAT{fat} seed {seed}"
    if r.returncode != 0:
        bad(f"{name}: stress run failed", r.stdout + r.stderr)
        return
    probs = fsck_problems(img)
    if probs:
        bad(f"{name}: fsck.fat -n found problems", "\n".join(probs))
        return
    # Extract with mtools and compare with what XenonOS says it wrote.
    ext = os.path.join(OUT, f"extract{fat}-{seed}")
    shutil.rmtree(ext, ignore_errors=True)
    os.makedirs(ext)
    # Copy the root itself ("::/*" would skip names that start with a dot).
    r2 = sh(["mcopy", "-s", "-n", "-i", img, "::/", ext + "/"], check=False)
    if r2.returncode != 0:
        bad(f"{name}: mcopy could not extract the tree", r2.stderr)
        return
    # mtools shows characters outside the Basic Multilingual Plane (stored as
    # UTF-16 surrogate pairs, e.g. emoji) as "__"; compare on that basis.
    ours = {"".join("__" if ord(c) > 0xFFFF else c for c in k): v
            for k, v in read_manifest(open(man).read()).items()}
    theirs = tree_manifest(ext)
    d = diff(ours, theirs, "XenonOS", "mtools")
    if d:
        bad(f"{name}: mtools reads something different", "\n".join(d[:20]))
        return
    stats = r.stdout.strip().split(": ", 1)[-1]
    ok(f"{name}: {ops} random operations, fsck.fat clean, mtools reads back all {len(ours)} entries "
       f"exactly ({stats.split(';')[0]})")


# No "~": mtools mangles names that contain it (it turns "[~" into "_~ ").
NAME_PIECES = ["a", "B", "c", "Z", "0", "9", "-", "_", "+", ",", ";", "=", "[", "]", " ", ".", "é", "Ü",
               "ß", "中", "文", "readme", "README", "Photo", "notes", "txt", "TXT", "long name ", "!", "#", "(", ")"]


def random_name(r):
    if r.random() < 0.3:
        lower = r.random() < 0.5
        base = "".join(r.choice("abcdefghijklmnopqrstuvwxyz" if lower else "ABCDEFGHIJKLMNOPQRSTUVWXYZ")
                       for _ in range(r.randint(1, 8)))
        ext = "".join(r.choice("abcdefghijklmnopqrstuvwxyz" if lower else "ABCDEFGHIJKLMNOPQRSTUVWXYZ")
                      for _ in range(r.randint(0, 3)))
        return base + ("." + ext if ext else "")
    s = "".join(r.choice(NAME_PIECES) for _ in range(r.randint(1, 12))).strip(" ").rstrip(". ")
    return s or "x"


def reverse_case(fat, seed):
    """Tree written by mtools must be read back exactly by XenonOS."""
    r = random.Random(seed)
    src = os.path.join(OUT, f"src{fat}-{seed}")
    shutil.rmtree(src, ignore_errors=True)
    os.makedirs(src)
    dirs = [src]
    taken = {}
    budget = SIZES[fat] * 1024 // 3
    count = 0
    for _ in range(120 if fat != 12 else 60):
        parent = r.choice(dirs)
        name = random_name(r)
        key = (parent, name.upper())
        if key in taken:
            continue
        taken[key] = True
        p = os.path.join(parent, name)
        if r.random() < 0.2 and len(dirs) < 30:
            os.makedirs(p)
            dirs.append(p)
        else:
            size = r.choice([0, 1, 511, 512, 513, 4096, r.randint(0, 20000), r.randint(0, 120000)])
            size = min(size, max(0, budget))
            budget -= size
            with open(p, "wb") as f:
                f.write(r.randbytes(size))
        count += 1
    img = os.path.join(OUT, f"reverse{fat}-{seed}.img")
    mkfs(img, fat, f"MTOOLS{fat}")
    items = os.listdir(src)
    if items:
        sh(["mcopy", "-s", "-i", img] + [os.path.join(src, x) for x in items] + ["::/"])
    res = sh([XFAT, img, "manifest"], check=False)
    name = f"FAT{fat} seed {seed}"
    if res.returncode != 0:
        bad(f"{name}: XenonOS could not read the mtools image", res.stderr)
        return
    d = diff(tree_manifest(src), read_manifest(res.stdout), "host tree", "XenonOS")
    if d:
        bad(f"{name}: XenonOS reads something different from what mtools wrote", "\n".join(d[:20]))
    else:
        ok(f"{name}: {count} files and directories written by mtools read back exactly")


def main():
    os.makedirs(OUT, exist_ok=True)
    if not os.path.exists(XFAT):
        print("test-fat-host: build/host/xfat missing - run 'make host'")
        return 1
    ops = int(os.environ.get("FAT_STRESS_OPS", "3000"))
    print("FAT core vs model, fsck.fat and mtools:")
    for fat in (12, 16, 32):
        for seed in (1, 2):
            stress_case(fat, seed, ops)
    print("FAT core reading mtools-written images:")
    for fat in (12, 16, 32):
        reverse_case(fat, 100 + fat)
    print(f"fat-host: {passed} passed, {failed} failed")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
