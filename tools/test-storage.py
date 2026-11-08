#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Storage tests (Phase 1.3): raw sector reads and writes on IDE, SATA (AHCI)
and floppy disks, through whole disks and through partitions.

For each QEMU configuration this script
  1. builds disk images with known contents (random data, MBR / GPT /
     no partition table, a sparse disk larger than LBA28 can address,
     floppies of different formats),
  2. boots XenonOS and drives the kernel debug shell over COM1,
  3. checks every CRC32 the kernel reports against the host's,
  4. after the VM has exited, checks that every "writetest" region in the
     image holds exactly the expected pattern, at the right offset, and that
     nothing else in any image changed.

Prints PASS/FAIL lines like tools/test.sh and exits non-zero on failure.
"""
import os
import random
import re
import subprocess
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "build", "test", "storage")
SESSION = os.path.join(ROOT, "tools", "qemu-session.py")
SECTOR = 512
MiB = 1 << 20

passed = failed = 0


def ok(msg):
    global passed
    passed += 1
    print(f"  \033[32mPASS\033[0m {msg}")


def bad(msg, detail=None):
    global failed
    failed += 1
    print(f"  \033[31mFAIL\033[0m {msg}")
    if detail:
        for line in detail.splitlines()[-25:]:
            print(f"       | {line}")


def run(cmd, **kw):
    return subprocess.run(cmd, check=True, capture_output=True, text=True, **kw)


def pattern(count_bytes, seed):
    """Same xorshift32 stream as KdCmdWritetest."""
    state = seed or 1
    out = bytearray()
    for _ in range(count_bytes // 4):
        state ^= (state << 13) & 0xFFFFFFFF
        state ^= state >> 17
        state ^= (state << 5) & 0xFFFFFFFF
        out += state.to_bytes(4, "little")
    return bytes(out)


def random_file(path, size, seed):
    r = random.Random(seed)
    with open(path, "wb") as f:
        left = size
        while left:
            n = min(left, MiB)
            f.write(r.randbytes(n))
            left -= n


def sfdisk(path, script):
    run(["sfdisk", "-q", path], input=script)


def crc_region(path, offset, length):
    crc = 0
    with open(path, "rb") as f:
        f.seek(offset)
        left = length
        while left:
            chunk = f.read(min(left, 4 * MiB))
            if not chunk:
                break
            crc = zlib.crc32(chunk, crc)
            left -= len(chunk)
    return crc


class Image:
    def __init__(self, name, size):
        self.path = os.path.join(OUT, name)
        self.size = size
        self.writes = []            # (offset, data) planned for the current run

    def digest(self, skip=()):
        """Hash of the image's content with holes counting as zeros, ignoring
        the byte ranges in 'skip'. Only non-zero 4 KiB blocks are hashed (with
        their offsets), so sparse images of any size are quick."""
        h = 0
        block = 4096
        zero = bytes(block)
        fd = os.open(self.path, os.O_RDONLY)
        try:
            pos = 0
            while pos < self.size:
                try:
                    data = os.lseek(fd, pos, os.SEEK_DATA)
                except OSError:
                    break
                hole = os.lseek(fd, data, os.SEEK_HOLE)
                b = data - data % block
                while b < hole:
                    os.lseek(fd, b, os.SEEK_SET)
                    chunk = bytearray(os.read(fd, block))
                    for off, length in skip:
                        s0, e0 = max(off, b), min(off + length, b + len(chunk))
                        if s0 < e0:
                            chunk[s0 - b:e0 - b] = bytes(e0 - s0)
                    if chunk != zero[:len(chunk)]:
                        h = zlib.crc32(b.to_bytes(8, "little") + chunk, h)
                    b += block
                pos = hole
        finally:
            os.close(fd)
        return h


def build_images():
    os.makedirs(OUT, exist_ok=True)
    imgs = {}

    # 64 MiB, MBR: 2 primary + extended with 2 logical partitions.
    m = Image("mbr.img", 64 * MiB)
    random_file(m.path, m.size, 1)
    sfdisk(m.path, "label: dos\nstart=2048,size=20480,type=c\nstart=22528,size=20480,type=83\n"
                   "start=43008,type=5\nstart=45056,size=10240,type=6\nstart=57344,size=8192,type=b\n")
    imgs["mbr"] = m

    # 48 MiB, GPT with three named partitions.
    g = Image("gpt.img", 48 * MiB)
    random_file(g.path, g.size, 2)
    sfdisk(g.path, "label: gpt\nstart=2048,size=16384,type=C12A7328-F81F-11D2-BA4B-00A0C93EC93B,name=EFI\n"
                   "start=18432,size=24576,type=EBD0A0A2-B9E5-4433-87C0-68B6B72699C7,name=Data\n"
                   "start=43008,size=4096,type=0FC63DAF-8483-4772-8E79-3D69D8477DE4,name=Linux\n")
    imgs["gpt"] = g

    # Same layout with a damaged primary GPT header: the backup must be used.
    gb = Image("gpt-bad.img", 48 * MiB)
    random_file(gb.path, gb.size, 3)
    sfdisk(gb.path, "label: gpt\nstart=2048,size=8192,name=One\nstart=10240,size=8192,name=Two\n")
    with open(gb.path, "r+b") as f:
        f.seek(SECTOR + 16)                 # header CRC field
        f.write(b"\xde\xad\xbe\xef")
    imgs["gptbad"] = gb

    # 32 MiB "superfloppy": FAT32 from sector 0, no partition table.
    sf = Image("superfloppy.img", 32 * MiB)
    if os.path.exists(sf.path):
        os.remove(sf.path)
    run(["mkfs.fat", "-C", "-F", "32", "-n", "SUPERFLOPPY", sf.path, str(sf.size // 1024)])
    random_file(os.path.join(OUT, "sf.bin"), 4 * MiB, 4)
    run(["mcopy", "-i", sf.path, os.path.join(OUT, "sf.bin"), "::/DATA.BIN"], env=dict(os.environ, MTOOLS_SKIP_CHECK="1"))
    imgs["superfloppy"] = sf

    # 200 GiB sparse disk: data beyond the 128 GiB that LBA28 can reach.
    big = Image("big.img", 200 << 30)
    with open(big.path, "wb") as f:
        f.truncate(big.size)
    big.high_lba = 400_000_000              # > 2^28 sectors
    with open(big.path, "r+b") as f:
        f.seek(big.high_lba * SECTOR)
        f.write(random.Random(5).randbytes(2 * MiB))
        f.seek(big.size - MiB)
        f.write(random.Random(6).randbytes(MiB))
    imgs["big"] = big

    # Floppies: 1.44 MB and 720 KB FAT12, and a 2.88 MB one.
    for name, kb, seed in (("fd144", 1440, 7), ("fd720", 720, 8), ("fd288", 2880, 9)):
        fd = Image(f"{name}.img", kb * 1024)
        if os.path.exists(fd.path):
            os.remove(fd.path)
        run(["mkfs.fat", "-C", "-n", name.upper(), fd.path, str(kb)])
        random_file(os.path.join(OUT, f"{name}.bin"), kb * 1024 // 2, seed)
        run(["mcopy", "-i", fd.path, os.path.join(OUT, f"{name}.bin"), "::/RANDOM.BIN"],
            env=dict(os.environ, MTOOLS_SKIP_CHECK="1"))
        imgs[name] = fd
    return imgs


def session(name, qemu_args, commands):
    log = os.path.join(OUT, f"{name}.log")
    r = subprocess.run([SESSION, "--log", log, "--timeout", "120", "--qemu-args", qemu_args, *commands])
    text = open(log).read() if os.path.exists(log) else ""
    return r.returncode, text


def check_run(name, qemu_args, images, reads, writes, expect_text=()):
    """reads:  list of (device, image, image byte offset, length[, device byte offset])
               checked by CRC (length -1 = to the end of the image; the device
               offset defaults to the image offset, use 0 for partitions)
       writes: list of (device, lba, count, seed, image, image byte offset)"""
    skips = {id(img): [(off, count * SECTOR) for _, _, count, _, im, off in writes if im is img] for img in images}
    untouched = {id(img): img.digest(skips[id(img)]) for img in images}
    commands = ["disks", "volumes"]
    expected = []
    for dev, img, off, length, *rest in reads:
        dev_off = rest[0] if rest else off
        if length == -1:
            length = img.size - off
        expected.append((dev, crc_region(img.path, off, length), length))   # before any writes
        if dev_off == 0 and off + length == img.size and off == 0:
            commands.append(f"crc32 {dev}")
        else:
            commands.append(f"crc32 {dev} {dev_off // SECTOR} {length // SECTOR}")
    for dev, lba, count, seed, img, img_off in writes:
        commands.append(f"writetest {dev} {lba} {count} {seed}")
    commands.append("exit")
    status, text = session(name, qemu_args, commands)
    if status != 0:
        bad(f"{name}: session did not finish cleanly (status {status})", text)
        return
    problems = []
    for want in expect_text:
        if want not in text:
            problems.append(f"missing '{want}'")
    for dev, host, length in expected:
        m = re.search(rf"crc32 {re.escape(dev)}: ([0-9a-f]{{8}}) over (\d+) bytes", text)
        if not m:
            problems.append(f"no crc32 result for {dev}")
        elif int(m.group(1), 16) != host or int(m.group(2)) != length:
            problems.append(f"{dev}: kernel crc {m.group(1)} over {m.group(2)} bytes, host {host:08x} over {length}")
    for dev, lba, count, seed, img, img_off in writes:
        if f"writetest {dev}: {count} sectors at LBA {lba} written and verified" not in text:
            problems.append(f"writetest on {dev} at LBA {lba} did not verify in the kernel")
        with open(img.path, "rb") as f:
            f.seek(img_off)
            if f.read(count * SECTOR) != pattern(count * SECTOR, seed):
                problems.append(f"{os.path.basename(img.path)}: pattern for {dev} not at byte {img_off} after exit")
    for img in images:
        if img.digest(skips[id(img)]) != untouched[id(img)]:
            problems.append(f"{os.path.basename(img.path)}: data outside the written regions changed")
    if problems:
        bad(f"{name}: " + "; ".join(problems[:4]), text)
    else:
        ok(f"{name}: {len(reads)} CRC checks match the host, {len(writes)} writes landed exactly, nothing else changed")


def main():
    imgs = build_images()
    boot = os.path.join(ROOT, "build", "xenonos.img")
    if not os.path.exists(boot):
        print("test-storage: build/xenonos.img missing - run 'make image' first")
        return 1
    m, g, gb, sf, big = imgs["mbr"], imgs["gpt"], imgs["gptbad"], imgs["superfloppy"], imgs["big"]
    f144, f720, f288 = imgs["fd144"], imgs["fd720"], imgs["fd288"]
    hi = big.high_lba
    last = big.size // SECTOR

    # ---- Run 1: IDE (PIIX, bus-master DMA) + two floppy drives ----------------
    reads = [
        ("PhysicalDrive1", m, 0, -1),
        ("\\Device\\Harddisk1\\Partition3", m, 45056 * SECTOR, 10240 * SECTOR, 0),  # logical partition
        ("\\Device\\Harddisk2\\DR0", big, hi * SECTOR, 2 * MiB),
        ("PhysicalDrive2", big, big.size - MiB, MiB),
        ("Floppy0", f144, 0, -1),
        ("Floppy1", f720, 0, -1),
    ]
    writes = [
        ("PhysicalDrive1", 100, 64, 7, m, 100 * SECTOR),
        ("\\Device\\Harddisk1\\Partition4", 50, 33, 8, m, (57344 + 50) * SECTOR),
        ("\\Device\\Harddisk2\\DR0", hi + 1000, 16, 11, big, (hi + 1000) * SECTOR),
        ("Floppy0", 1000, 20, 9, f144, 1000 * SECTOR),
        ("Floppy1", 700, 9, 10, f720, 700 * SECTOR),
    ]
    check_run("ide-floppy", " ".join([
        f"-drive file={m.path},format=raw,if=ide,index=1",
        f"-drive file={big.path},format=raw,if=ide,index=3",
        f"-drive file={f144.path},format=raw,if=floppy,index=0",
        f"-drive file={f720.path},format=raw,if=floppy,index=1"]),
        [m, big, f144, f720], reads, writes,
        expect_text=["IDE primary slave, LBA48, DMA", "Harddisk1\\Partition3: FAT16 (0x06)",
                     "ATAPI device (CD/DVD)", "medium 1440 KB (80 cylinders, 2 heads, 18 sectors/track)",
                     "medium 720 KB (80 cylinders, 2 heads, 9 sectors/track)"])

    # ---- Run 2: AHCI (q35): GPT, superfloppy, >128 GiB, damaged GPT ------------
    reads = [
        ("PhysicalDrive1", g, 0, -1),
        ("\\Device\\Harddisk1\\Partition2", g, 18432 * SECTOR, 24576 * SECTOR, 0),
        ("\\Device\\Harddisk2\\Partition1", sf, 0, -1),
        ("\\Device\\Harddisk3\\DR0", big, hi * SECTOR, 2 * MiB),
        ("\\Device\\Harddisk4\\Partition2", gb, 10240 * SECTOR, 8192 * SECTOR, 0),
    ]
    writes = [
        ("\\Device\\Harddisk1\\Partition3", 10, 100, 21, g, (43008 + 10) * SECTOR),
        ("PhysicalDrive2", 4000, 300, 22, sf, 4000 * SECTOR),
        ("PhysicalDrive3", hi + 3000, 64, 23, big, (hi + 3000) * SECTOR),
        ("\\Device\\Harddisk4\\Partition1", 0, 8, 24, gb, 2048 * SECTOR),
    ]
    check_run("ahci-gpt", " ".join([
        "-machine q35",
        f"-drive id=d1,file={g.path},format=raw,if=none -device ide-hd,drive=d1,bus=ide.1",
        f"-drive id=d2,file={sf.path},format=raw,if=none -device ide-hd,drive=d2,bus=ide.2",
        f"-drive id=d3,file={big.path},format=raw,if=none -device ide-hd,drive=d3,bus=ide.3",
        f"-drive id=d4,file={gb.path},format=raw,if=none -device ide-hd,drive=d4,bus=ide.4"]),
        [g, sf, big, gb], reads, writes,
        expect_text=["MSI interrupts", "Harddisk1\\Partition1: EFI system \"EFI\"",
                     "Harddisk1\\Partition2: Basic data \"Data\"", "Harddisk2\\Partition1: whole disk",
                     "primary GPT damaged, trying the backup", "Harddisk4\\Partition2: Linux \"Two\""])

    # ---- Run 3: 2.88 MB floppy ---------------------------------------------------
    check_run("floppy-2.88", f"-drive file={f288.path},format=raw,if=floppy,index=0",
              [f288], [("Floppy0", f288, 0, -1)], [],
              expect_text=["medium 2880 KB (80 cylinders, 2 heads, 36 sectors/track)"])

    print(f"storage: {passed} passed, {failed} failed")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
