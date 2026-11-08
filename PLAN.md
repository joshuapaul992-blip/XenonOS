# XenonOS — Milestone 1 Plan

**Target release:** `XenonOS 0.0.1 (build N)`
**Status:** Phase 1.1 complete (build 5)
**Scope of Milestone 1:** A bootable, from-scratch x86-64 operating system with an NT-style kernel architecture,
native FAT12/16/32 drive letters (A:–Z:), USB 1.x/2.0/3.x (storage, keyboard, mouse), a TCP/IP stack, and a
"smart" POSIX-style shell with ~70 built-in commands.

---

## 1. Ground rules and key decisions

| Topic | Decision | Why |
|---|---|---|
| CPU architecture | **x86-64 only** (long mode, 4-level paging) | One target keeps Milestone 1 doable. Modern USB 3 controllers expect 64-bit DMA. |
| Firmware | **Legacy BIOS and UEFI** | BIOS is needed for older floppy-equipped machines. UEFI is needed for modern PCs. |
| Bootloader | **Limine** (BSD-licensed), using the Limine boot protocol | It boots from BIOS and UEFI off the same FAT32 C: partition, gives us a framebuffer, memory map, ACPI RSDP and boot modules. Everything after the bootloader is written from scratch. A custom loader can replace it in a later milestone. |
| Kernel language | **C11 (freestanding) + NASM** | Easiest to debug and fastest to bootstrap. |
| Executable format | **ELF64** for the kernel, drivers (`.sys` = ELF relocatable) and programs | Works with GCC/Clang directly. PE support could come later for extra NT flavour. |
| Kernel design | **NT-style hybrid kernel**: HAL → Kernel (Ke) → Executive (Mm, Ob, Io, Ps, Ex) → drivers | This is the "Windows NT-like" part (see §3). |
| User API | Native `Nt*`-style system calls, plus a small POSIX-style C library (`xlibc`) on top | NT on the inside, Unix tools on the outside. |
| Users | Single user, **root (uid 0)** in Milestone 1, so the prompt ends in `#` | Multi-user and real `chown` semantics come later. |
| Third-party code | **None in the OS image except Limine.** awk, sed, gzip/deflate, md5, vi, the TCP/IP stack and so on are written from scratch. | The project is "from scratch". |

### Version and build number

- The version is fixed at `0.0.1` in `VERSION`.
- The build number is stored in `BUILD_NUMBER` (starts at `1`). Every full image build
  (`make image`) runs `tools/bump-build.sh`, which increments the number and generates `include/xenon/version.h`:
  ```c
  #define XENON_VERSION      "0.0.1"
  #define XENON_BUILD        1
  #define XENON_VERSION_STR  "XenonOS 0.0.1 (build 1)"
  ```
- The string appears in the boot banner, in `uname -a`, in `dmesg` line 1, and in the bugcheck (crash) screen.
- `BUILD_NUMBER` is committed to git, so build numbers stay in order across machines.

---

## 2. What the user sees

```
XenonOS 0.0.1 (build 1)
Copyright (c) 2026 XenonOS Project

[    0.000] HAL: x86-64, 4 CPUs (1 online), APIC, HPET
[    0.012] Mm: 2048 MiB usable
[    0.140] Io: AHCI port 0: QEMU HARDDISK  (8.0 GiB)
[    0.151] MountMgr: C: -> \Device\Harddisk0\Partition1 (FAT32, "XENON")
[    0.152] MountMgr: A: -> \Device\Floppy0 (FAT12, 1.44 MB)
[    0.410] USB: xHCI 3.0 at 00:04.0, 8 ports
[    0.620] USB: port 2: SanDisk Cruzer (MSC) -> E: (FAT32)
[    0.630] USB: port 3: HID keyboard
[    1.020] Net: e1000 52:54:00:12:34:56, DHCP 10.0.2.15/24

C:/# cd XenonOS
C:/XenonOS/# ls
Bin  Config  Logs  System
C:/XenonOS/# E:
E:/# cp C:/XenonOS/Logs/boot.log .
E:/#
```

### Prompt format

`<Drive>:<path>/#`, with the path always ending in `/`:

| Current directory | Prompt |
|---|---|
| Root of C: | `C:/#` |
| `C:\XenonOS` | `C:/XenonOS/#` |
| `E:\photos\2026` | `E:/photos/2026/#` |

The prompt can be changed with `PS1` (default `\w/#` style expansion). Plain `C:/#` is the default.

---

## 3. NT-style architecture

```
 ┌────────────────────────────────────────────────────────────────────┐
 │ User mode:  xsh (shell)  │  xbox (multi-call utilities)  │  vi     │
 │             xlibc (POSIX-style C library)  →  ntdll-like stubs     │
 ├──────────────────────────── syscall/sysret ────────────────────────┤
 │ Executive                                                          │
 │   Ob  Object Manager (namespace, handles, symlinks, refcounts)     │
 │   Io  I/O Manager (IRPs, driver/device objects, driver stacks)     │
 │   PnP Plug & Play manager + Mount Manager (drive letters)          │
 │   Mm  Memory Manager (PMM, VMM, pools, DMA buffers)                │
 │   Ps  Process & thread manager                                     │
 │   Ex  Work queues, timers, executive resources                     │
 │   Ldr Kernel module loader (insmod/rmmod/lsmod)                    │
 ├────────────────────────────────────────────────────────────────────┤
 │ Kernel (Ke): scheduler, IRQL, DPCs, APCs, spinlocks, dispatcher    │
 │              objects (events, mutexes, semaphores, timers)         │
 ├────────────────────────────────────────────────────────────────────┤
 │ HAL: APIC/IOAPIC, PIT/HPET/TSC, ACPI tables, PCI/PCIe (ECAM), CMOS  │
 │      RTC, port I/O, MSI/MSI-X, ISA DMA                             │
 ├────────────────────────────────────────────────────────────────────┤
 │ Drivers: ata, ahci, fdc, partmgr, fastfat, usbcore, xhci, ehci,    │
 │          uhci, ohci, usbhub, usbhid, usbstor, i8042, e1000,         │
 │          rtl8139, fbcon, serial, null/zero/random                  │
 └────────────────────────────────────────────────────────────────────┘
```

### NT-like traits that will be implemented

- **Object namespace:** `\Device\Harddisk0\Partition1`, `\Device\Floppy0`, `\Device\Usb\...`, `\Driver\fastfat`,
  `\DosDevices\C:` (a symbolic link to the volume device). Drive letters are just symbolic links, as on NT.
- **IRP-based I/O with stacked drivers:** `fastfat` → volume → `partmgr` → `disk` → `ahci` / `usbstor` / `fdc`.
- **IRQL levels:** `PASSIVE`, `APC`, `DISPATCH`, `DIRQL`, `HIGH`, with interrupt work deferred to **DPCs**.
- **Handles and access masks** on every kernel object (files, processes, threads, events, sockets).
- **Per-drive current directory** (as in `cmd.exe`): `cd D:/foo` and then `C:` brings you back to C:'s own
  working directory. `D:` alone switches drive.
- **Bugcheck screen** with a stop code and parameters (`*** STOP: 0x0000000A IRQL_NOT_LESS_OR_EQUAL`), also
  written to serial and, if possible, to `C:/XenonOS/Logs/crash.log`.
- **Boot-start drivers** loaded as Limine modules (like NTLDR loading boot drivers), so the kernel can reach
  C: before it can read files. All other drivers load from `C:/XenonOS/System/Drivers/`.
- **Win32-style device paths:** `//./PhysicalDrive0`, `//./HarddiskVolume1`, `//./Floppy0`, `//./COM1`,
  `//./null`, used by `dd`, `fdisk`, `mount` and `mknod`.

---

## 4. Drives, paths and the file system

### Drive letter rules (Mount Manager)

1. **A:** is floppy drive 0 and **B:** is floppy drive 1 (detected from the CMOS byte `0x10` and the ACPI FDC device).
   If there is no floppy drive, the letter stays empty. It is never given to anything else.
2. **C:** is always the **boot volume**: the FAT volume holding `XenonOS/System/xnkrnl.elf`, whatever kind of disk it is on
   (SATA, IDE or a USB stick).
3. **D: and up:** the remaining FAT volumes on fixed disks, in NT order: first the primary partitions of every disk
   in disk order, then the logical partitions.
4. **Removable volumes** (USB storage, and later CD) get the lowest free letter when they arrive, and the letter
   is freed when they leave.
5. **Sticky letters:** a letter is remembered by the volume's serial number and partition offset in
   `C:/XenonOS/Config/mountmgr.cfg`, so the same USB stick gets the same letter next time (like the NT
   `MountedDevices` key).
6. Partitions that are not FAT are listed by `lsblk`/`fdisk` but get no letter.
7. If more than 24 volumes exist, the extra ones are left without a letter and a warning is logged.
   They are still reachable as `//./HarddiskVolumeN/`.

### Path rules

- `/` and `\` are both accepted as separators. The shell shows `/`.
- `C:/a/b` is absolute. `/a/b` is relative to the root of the current drive. `a/b` is relative to the current directory.
- Names are **case-insensitive and case-preserving**, like Windows on FAT.
- Characters FAT does not allow (`" * : < > ? \ |` and control characters) are rejected. `*` and `?` are
  wildcards that the shell expands itself.

### FAT driver (`fastfat`) — what it must do

| Feature | Milestone 1 |
|---|---|
| FAT12 / FAT16 / FAT32 read and write | ✅ |
| Long file names (VFAT LFN), with checksums and generated 8.3 short names (`LONGFI~1.TXT`) | ✅ |
| FAT32 FSInfo (free cluster count and next-free hint) | ✅ |
| Mirroring to all FAT copies | ✅ |
| Volume dirty bit (set on mount, cleared on clean unmount) | ✅ |
| Created, modified and accessed timestamps from the CMOS RTC | ✅ |
| Attributes (R, H, S, A) | ✅, mapped to `chmod` |
| Volume label and serial number | ✅ |
| Block/sector cache with write-back, and `sync` on unmount | ✅ |
| exFAT, NTFS, ext2 | ❌ (later milestones) |

**Correctness bar:** every image that XenonOS writes must pass `fsck.fat -n` on Linux, and must mount and read back
correctly on Linux and Windows. The FAT driver is also built as a **normal Linux program** (`tests/host/fat`),
so it can be fuzzed and tested against `mkfs.fat`/`mtools` images without booting the OS.

### Unix commands on a FAT, NT-like system

| Command | How it behaves on XenonOS |
|---|---|
| `chmod` | FAT has no Unix permissions. Taking away write permission (`chmod -w`, `chmod 444`) sets the **Read-only** attribute, and giving it back clears it. `chmod +h/-h` and `+s/-s` (XenonOS extensions) set Hidden and System. Anything else is reported as unsupported, which matches Linux `vfat`. |
| `chown` | FAT has no owner. Changing the owner to root (uid 0, the owner of everything) succeeds. Anything else fails with `Operation not permitted`, which matches Linux `vfat`. |
| `ln` | Hard links are impossible on FAT, so `ln` fails with a clear message. `ln -s` works in the **object namespace** (`ln -s //./HarddiskVolume2 //./Backup`) and on the ramdisk. On FAT it fails as unsupported. |
| `mknod` | Creates a device object in `\Device` that points to a driver's (major, minor) pair, for example `mknod //./null2 c 1 3`. |
| `mount` | With no arguments, lists volumes. `mount //./Harddisk1/Partition2 F:` mounts a volume on a letter. `mount -o ro`. |
| `umount` | `umount E:` flushes the cache, clears the dirty bit and frees the letter. This must be done before pulling a USB stick out. |
| `df` | One line per drive letter: file system, size, used, free, use% and label. |
| `lsblk` | A tree of disks → partitions, with letters, file systems and sizes. |
| `fdisk` | Lists and edits MBR partition tables (`fdisk -l`, interactive mode). GPT disks are read-only in Milestone 1. |
| `dd` | Works on files and raw devices, e.g. `dd if=//./Floppy0 of=C:/floppy.img bs=512`. |

Extra command: **`format`** (`format E: /fs:fat32 /v:LABEL`, also callable as `mkfs.fat`). It is not on the
original list, but without it there is no way to prepare a new disk from inside XenonOS.

---

## 5. Hardware support in Milestone 1

### Storage
- **IDE/ATA** (PIO first, bus-master DMA after).
- **AHCI/SATA** (DMA, NCQ not needed).
- **Floppy controller** (82077AA-compatible): ISA DMA channel 2 with bounce buffers under 16 MiB that do not cross a
  64 KiB boundary, motor timing and seek/recalibrate, 1.44 MB and 720 KB media, media-change detection. Drives A: and B:.
- **Partition tables:** MBR (primary + extended/logical) and GPT (read only).
- **RAM disk** (optional letter, for `/tmp`-like scratch space).

### USB (all speeds)
| Layer | Pieces |
|---|---|
| Host controllers | **UHCI** (USB 1.x, Intel/VIA), **OHCI** (USB 1.x, others), **EHCI** (USB 2.0, including companion-controller routing), **xHCI** (USB 3.x, also runs 1.x/2.0 devices on its ports) |
| Firmware handoff | Take ownership from the BIOS for every controller (EHCI `USBLEGSUP`, xHCI extended capability, UHCI `LEGSUP`, OHCI `InterruptRouting`) and turn off legacy USB emulation |
| Core | Device enumeration, address assignment, descriptor parsing, configuration selection, control/bulk/interrupt transfers, hot-plug and removal through the PnP manager |
| Hubs | USB 2.0 hubs (with transaction translators for full/low-speed devices behind EHCI) and USB 3.x hubs |
| HID | **Keyboard** and **mouse** in boot protocol, plus a basic report-descriptor parser for report protocol. Keyboard repeat is done in software. Mouse wheel is supported. |
| Mass storage | **Bulk-Only Transport** + SCSI commands (INQUIRY, TEST UNIT READY, READ CAPACITY(10/16), READ/WRITE(10/16), REQUEST SENSE, MODE SENSE). Bad sticks get reset recovery. UAS can come later. |

USB drives are **FAT12/16/32 read/write only**, as requested.

The **mouse** has jobs in a text-mode OS: a pointer on the console, select-to-copy and middle-click-paste,
scroll-back with the wheel, and clicking to place the cursor in `vi`. It is also readable as `//./Mouse0`.
**PS/2** keyboard and mouse (i8042) are also supported, as a fallback and for early boot.

### Networking
- **NICs:** Intel e1000/e1000e (QEMU's default and common on real hardware) and Realtek RTL8139. RTL8169 and
  virtio-net are stretch goals.
- **Stack:** Ethernet II, ARP (cache with timeouts), IPv4 (no fragment reassembly needed at first), ICMP echo, UDP,
  TCP (3-way handshake, retransmission, sliding window, FIN/RST, TIME_WAIT), DHCP client, DNS resolver,
  loopback.
- **API:** BSD-style sockets in `xlibc` built on socket handles (`NtCreateSocket` and similar).
- **HTTP/1.1 client** for `wget`. HTTPS is not part of Milestone 1.

### Platform
- ACPI table parsing (RSDP → XSDT/RSDT → MADT, FADT, HPET, MCFG) and ACPI power-off/reset through the FADT. No AML interpreter yet.
- Local APIC + IOAPIC (falling back to the 8259 PIC), HPET or PIT, TSC calibration, CMOS RTC for `date`.
- PCI/PCIe enumeration (legacy port `0xCF8` access and ECAM), BAR mapping, MSI/MSI-X.
- Console: Limine framebuffer with a built-in PSF2 font, ANSI/VT100 escape codes (needed by `vi`), 1000-line
  scroll-back. Serial COM1 mirrors everything for debugging and automated tests.
- One CPU in Milestone 1. The scheduler and locks are written so SMP can be turned on later.

---

## 6. The smart shell (`xsh`)

### Language (POSIX `sh` subset)
- Pipelines `|`, lists `;` `&&` `||`, background `&`, grouping `{ }` and `( )`.
- Redirection `>` `>>` `<` `2>` `2>&1` `<<` (here-docs).
- Variables, `$?` `$$` `$#` `$@` `$0`–`$9`, `${var:-default}`, command substitution `$(...)` and backticks,
  arithmetic `$((...))`.
- `if/elif/else`, `while`, `until`, `for`, `case`, functions, `break`/`continue`/`return`, `exit`.
- Globbing `*` `?` `[...]`, case-insensitive on FAT drives.
- Quoting: `'...'`, `"..."`, `\`.
- Scripts: `sh script.sh`, and `#!` lines. Startup file: `C:/XenonOS/Config/profile`.

### "Smart" features
- **Line editor:** arrow keys, Home/End, Ctrl+A/E/K/U/W/L, Delete, word jumps with Ctrl+←/→.
- **Tab completion** for commands, paths (including drive letters: `E:/pho<Tab>`), variables, and command options.
- **History** saved to `C:/XenonOS/Config/history`, with ↑/↓ and **Ctrl+R** reverse search.
- **Suggestions in grey** from history as you type (accept with →).
- **"Did you mean?"** for mistyped commands (`grpe` → `grep`), using edit distance.
- **Syntax colouring** while typing: commands, strings, unknown commands in red.
- `help` and `<command> --help` for every command.
- Ctrl+C sends an interrupt to the foreground job and Ctrl+Z suspends it, with `jobs`, `fg` and `bg`.
- Coloured `ls` output by type (directories, executables, archives).

### How commands are built
- Every command lives in **one multi-call program**, `C:/XenonOS/Bin/xbox.elf` (similar to BusyBox, but written from scratch).
  FAT has no symlinks, so the shell maps command names to xbox itself and starts xbox with the
  command name as `argv[0]`.
- Each pipeline stage is a **real process** (spawned NT-style, without `fork`, with inherited handles), so `ps` and
  `kill` show and control real things.
- Only commands that must change the shell itself (`cd`, `export`, `exit`, `jobs`, `fg`, `alias`...) run inside `xsh`.

### Command list for Milestone 1

| Group | Commands | Notes |
|---|---|---|
| File operations | `cat` `cp` `chmod` `chown` `ln` `ls` `mkdir` `mv` `rm` `touch` | `cp -r`, `mv` across drives (copy + delete), `rm -rf`, `ls -la`, `mkdir -p` |
| Text processing | `awk` `cut` `grep` `head` `sed` `sort` `tail` `tr` `uniq` `wc` `vi` | Own regex engine (POSIX BRE + ERE) shared by grep, sed and awk. awk has fields, patterns, associative arrays, printf and built-in functions. `tail -f`. |
| System info and control | `date` `df` `dmesg` `du` `free` `hostname` `ps` `uname` `uptime` `kill` | `date -s` writes the RTC. `kill -l`, `-9`, `-15`. |
| Networking | `arp` `ifconfig` `nc` `nslookup` `ping` `telnet` `wget` `udhcpc` | `nc -l`, `ping -c`, `wget -O` |
| Archiving and compression | `dd` `gunzip` `gzip` `tar` `zcat` `base64` `md5sum` | Own DEFLATE encoder and decoder (RFC 1951/1952). `tar` uses ustar with `-z`. |
| Shell and logic | `sh` `echo` `expr` `false` `test` / `[` `true` `xargs` | |
| Storage and kernel | `mount` `umount` `fdisk` `lsblk` `insmod` `rmmod` `lsmod` `mknod` | Drivers are ELF `.sys` modules with `DriverEntry`/`DriverUnload` |
| Extras (needed to make the above usable) | `cd` `pwd` `clear`/`cls` `help` `history` `sync` `reboot` `shutdown` `format` `ver` `which` `env` `export` `sleep` `jobs` `fg` `bg` | `ver` prints `XenonOS 0.0.1 (build N)` |

**`vi` scope:** normal, insert and ex modes; `h j k l` and arrow keys, `w b e 0 $ gg G`, `x dd dw yy p P u . J`,
`i a o O A I`, `/` and `?` search with `n`/`N`, `:w :q :q! :wq :x :e :s/// :%s/// :set nu`, numeric counts,
handling of large files, and mouse clicks to move the cursor.

---

## 7. Disk layout on C:

```
C:/
├── boot/limine/            Limine files (limine-bios.sys, limine.conf)
├── EFI/BOOT/BOOTX64.EFI    UEFI boot (C: also works as the EFI System Partition)
└── XenonOS/
    ├── System/
    │   ├── xnkrnl.elf      kernel (named like ntoskrnl)
    │   └── Drivers/        *.sys loadable drivers (fastfat.sys, xhci.sys, e1000.sys, ...)
    ├── Bin/
    │   ├── xsh.elf         shell
    │   └── xbox.elf        all other commands
    ├── Config/             hostname, profile, network.cfg, mountmgr.cfg, history
    └── Logs/               boot.log, crash.log
```

---

## 8. Source tree

```
xenonos/
├── VERSION  BUILD_NUMBER  Makefile  PLAN.md  README.md
├── tools/          bump-build.sh, mkimage.sh (MBR + FAT32 with mtools), run-qemu.sh, test-runner
├── include/xenon/  version.h (generated), shared kernel/user headers
├── kernel/
│   ├── arch/x86_64/  entry, GDT/IDT/TSS, ISRs, syscall entry, context switch
│   ├── hal/          apic, ioapic, hpet, pit, tsc, acpi, pci, cmos, isadma
│   ├── ke/           scheduler, irql, dpc, apc, spinlock, dispatcher objects, bugcheck
│   ├── mm/           pmm, vmm, pool, dma, user address spaces
│   ├── ob/           object manager, handle tables, symbolic links
│   ├── io/           I/O manager, IRPs, PnP manager, mount manager, block cache
│   ├── ps/           processes, threads, ELF loader, spawn, termination
│   ├── ex/           work queues, timers, ring-buffer log (dmesg)
│   ├── ldr/          kernel module loader (ELF relocatable)
│   ├── net/          eth, arp, ipv4, icmp, udp, tcp, dhcp helpers, socket objects
│   └── syscall/      Nt* system call table
├── drivers/
│   ├── storage/      ata, ahci, fdc, disk, partmgr, ramdisk
│   ├── fs/           fastfat
│   ├── usb/          usbcore, uhci, ohci, ehci, xhci, usbhub, usbhid, usbstor
│   ├── input/        i8042, kbd layout (US first), mouse
│   ├── net/          e1000, rtl8139
│   └── char/         fbcon, serial, null, zero, random, tty
├── user/
│   ├── xlibc/        crt0, stdio, stdlib, string, unistd, sockets, time, regex
│   ├── xsh/          lexer, parser, executor, line editor, completion, history
│   └── xbox/         one file per command group; vi/, awk/, deflate/, tar/
└── tests/
    ├── host/         FAT, deflate, regex, awk, md5 built as Linux programs + fuzzers
    └── qemu/         scripted shell sessions over serial, with expected output
```

---

## 9. Phases (each one ends with a build that boots)

Milestone 1 is split into ten phases. Each has a clear "done when" test that runs in QEMU automatically.

| # | Phase | Main work | Done when |
|---|---|---|---|
| **1.0** | Bring-up | Cross toolchain, Limine, kernel entry, GDT/IDT, serial + framebuffer console, panic/bugcheck screen, build-number script, image script | QEMU shows `XenonOS 0.0.1 (build 1)`. The next `make image` shows build 2. |
| **1.1** | Memory and platform | PMM, VMM, kernel pools, ACPI, APIC/IOAPIC, HPET/PIT, RTC, PCI enumeration | `dmesg`-style log lists memory size, CPUs and PCI devices. A pool stress test passes. |
| **1.2** | Kernel core | Threads, preemptive priority scheduler, IRQL/DPC, events and mutexes, Object Manager, handles, log ring buffer | 100 kernel threads run on timers with no leaks. The object namespace can be listed from a debug shell. |
| **1.3** | Storage | I/O Manager and IRPs, ATA, AHCI, floppy, disk class, MBR/GPT, block cache | Raw sectors read correctly from IDE, SATA and floppy images in QEMU. |
| **1.4** | FAT + drive letters | `fastfat` (12/16/32, LFN, write path), Mount Manager, A:/B:/C:… rules | Host tests pass. Files XenonOS writes pass `fsck.fat -n` and read back correctly in Linux. |
| **1.5** | User mode + shell | syscall/sysret, ELF loader, processes, xlibc, TTY, PS/2 keyboard, `xsh` with prompt, file commands, `echo/test/true/false/expr/xargs/sh` | Boot to `C:/#`, `cd XenonOS` shows `C:/XenonOS/#`, and the file-operations test script passes. |
| **1.6** | Text and archive tools | Regex engine, `grep sed awk cut sort uniq tr wc head tail`, `vi`, DEFLATE, `gzip/gunzip/zcat tar base64 md5sum dd` | Output matches GNU/BusyBox on the test corpus. A `.tar.gz` made on Linux unpacks, and the reverse also works. |
| **1.7** | USB | usbcore, xHCI, EHCI, UHCI, OHCI, hubs, HID keyboard and mouse, mass storage, hot-plug → drive letters | In QEMU with each controller type (`qemu-xhci`, `usb-ehci`, `piix3-usb-uhci`, `pci-ohci`): typing on `usb-kbd` works, the mouse moves the pointer, and a `usb-storage` stick gets E:, can be written to, unmounted, and passes `fsck.fat`. |
| **1.8** | Networking | e1000, RTL8139, the full stack, sockets, and all networking commands | `udhcpc` gets an address from QEMU, `ping 10.0.2.2` works, `nslookup example.com` resolves, and `wget` downloads a file over HTTP. |
| **1.9** | Kernel modules, system tools, release | `insmod/rmmod/lsmod/mknod`, `ps kill free df du uptime hostname uname date`, `fdisk lsblk mount umount format`, testing on real PCs, docs | The whole command list passes the QEMU test suite and boots on at least 2 real PCs (one BIOS, one UEFI). Tag **v0.0.1**. |

### Rough size

About **60–80k lines of C** for Milestone 1. The largest pieces are USB (~15k), the TCP/IP stack (~8k),
the shell and line editor (~6k), awk + regex (~6k), FAT (~4k), vi (~3k) and DEFLATE (~2k).

---

## 10. Testing

- **Host unit tests:** FAT, DEFLATE, regex, awk, md5 and base64 are written to also build as Linux
  programs. They are checked against `mkfs.fat`, `fsck.fat`, `gzip`, `md5sum` and GNU tools, and fuzzed with libFuzzer.
- **QEMU integration tests:** `tools/test-runner` boots the image, drives the shell over COM1, and compares output
  with expected transcripts. One matrix covers BIOS/UEFI × IDE/AHCI/USB boot × each USB controller × e1000/RTL8139,
  plus a floppy image on A:.
- **File system safety:** after every test run, each image is checked with `fsck.fat -n`. A "pull the plug" test
  kills QEMU in the middle of a write and checks that the dirty bit is set and the damage stays within what `fsck` can repair.
- **Real hardware:** at least one older BIOS PC with a floppy drive and UHCI/EHCI, and one modern UEFI PC with xHCI.
- CI runs the host tests and the QEMU matrix on every push.

---

## 11. Main risks

| Risk | How it is handled |
|---|---|
| USB on real hardware is full of quirks (BIOS handoff, port routing, slow sticks) | Build xHCI first (it covers all speeds), test on real controllers early, and keep a log of quirks. PS/2 input is always there as a fallback. |
| A FAT bug destroys a user's USB stick | Strict host tests and fuzzing, `fsck.fat` in CI, the dirty bit, and a `ro` mount option. Real user drives are mounted read-only by default until Phase 1.4 has passed its tests. |
| Scope creep (awk, vi and TCP are each projects of their own) | Each tool has a fixed scope in §6, and extra features go to Milestone 2. |
| Floppy timing on real drives | Use generous delays first and tune them later. QEMU's floppy emulation is too forgiving, so real-hardware testing is required. |
| Debugging a crashing kernel | Serial log from the first instruction, QEMU `-s` with a GDB stub, a bugcheck screen with a stack trace, and `crash.log`. |

---

## 12. Not in Milestone 1 (candidates for later)

SMP, multiple users and permissions, NTFS/exFAT/ext2, CD/DVD (ISO 9660), NVMe, USB Attached SCSI, audio,
GUI and window manager, HTTPS/TLS, IPv6, PE executables and a Win32 subsystem, a registry, AML/ACPI power
management, swapping to disk.

---

## 13. Questions to confirm before Phase 1.0

1. **Bootloader:** is Limine acceptable, or must the bootloader also be written from scratch? Writing it would add about 2–3k lines
   and a separate BIOS and UEFI path.
2. **Language:** C (recommended) or Rust?
3. **Build number:** should it go up on *every* image build (the plan above), or only on builds that are committed or released?
4. **Single root user** in Milestone 1 (so the prompt always ends in `#`): OK?
5. **Extras:** OK to add `format`, `cd`, `ver`, `cls`, `sync`, `reboot` and the other helpers in §6?
