# exFAT file system for AmigaOS 3.2 — project rules

Layers on top of the workspace `CLAUDE.md` at the repository root. Where this
file and the root file disagree about this project, this file wins.


## Versioning

`VERSION` in `amiga/Makefile` is the single source: it becomes the
`$VER:` string, the tag's `rt_Version` byte and the `fse_Version` longword
in `FileSystem.resource`. **Bump it on every commit that changes the
handler.** The serial log's `$VER` line is then the only thing needed to
know which ROM is running, and sagasd's mounter prefers the higher of a
ROM entry and a disk copy - at a stale version, a copy left on a card
quietly wins.

It sat at 0.1 from the start of the project through all of the ROM work.
1.0 is the first build in which ROM registration is expected to work.

## Goal

An exFAT file system handler for AmigaOS 3.2 that mounts through the standard
AmigaDOS mechanism and sits on top of an ordinary Amiga block-device driver —
`sagasd.device` (SD card, in `projects/sagasd.device/`) being the first target,
but nothing may be specific to it.

## Read before writing code

| Topic | Doc |
|---|---|
| exFAT on-disk format | `docs/fs/exfat-on-disk-format.md` (working reference), `docs/fs/exfat-specification-upstream.md` (authoritative, Microsoft rev 1.00) |
| DOS packets — what each `ACTION_*` receives and must return | `docs/os/amigados/13-packet-documentation.md`, indexed in `docs/os/amigados/README.md` |
| Handler startup, main loop, shutdown | `docs/os/amigados/12-handlers-devices-and-file-systems.md` §12.1 |
| Mount, device list, `DosEnvec` keywords | `docs/os/amigados/07-administration-of-volumes-devices-and.md` |
| `FileSystem.resource` / `FileSysEntry` / `fse_PatchFlags` | `docs/os/NDK/NDK3.2/Autodocs/filesysres.doc` |
| What a current 3.2 file system is expected to do | `docs/os/NDK/NDK3.2/ReleaseNotes/FastFileSystem-RelNotes`, `DOS-RelNotes` |
| Device command set (`TD64`, `NSD`) | `docs/os/NDK/NDK3.2/Autodocs/trackdisk.doc`, `scsidisk.doc` |

## Settled decisions

### 1. Partition discovery

**Changed from the original decision.**  This project first specified that the
handler is always given a partition and must never look at a partition table.
That has been reversed deliberately: the handler now parses MBR and GPT
itself, matching the arrangement `sagasd.device` already uses for fat95,
where one node covers the whole card and the file system finds the partitions.

How the handler decides what it was given, at startup:

1. Read the first block of its extent.  If it holds an exFAT boot sector, it
   was handed a **partition** - use the `DosEnvec` range as-is.  This is what
   every node it publishes below will see, and what a hand-written mountlist
   naming a specific partition gets.
2. Otherwise look for a partition table: an MBR at LBA 0, or a GPT behind a
   protective MBR.  Take the **first** exFAT partition as this device's own
   extent, and publish a DOS device node for each of the others (`EXF1:`,
   `EXF2:` ...) with `dn_Task` NULL, so AmigaDOS starts a separate handler
   process for each on first access.
3. If neither, the mount fails.

Published nodes have `dn_Task` NULL and use `dn_Handler` so AmigaDOS
**loads its own copy of the handler for each**.  They must NOT share this
process's seglist: a seglist is one loaded image, so all processes started
from it share the same data and BSS, and this handler keeps its state in
globals (`g_handler`, `SysBase`/`DOSBase`, `dev_io.c`'s device spec,
`format.c`'s mkfs parameters, libexfat's `exfat_errors`).  Sharing corrupts
every instance within seconds - this was tried and it crashed immediately.
Making the handler reentrant, as FFS is, would be the alternative.  The
handler then locks each published name once, **after** its own startup packet
has been replied, to force AmigaDOS to start those processes so every volume
appears on Workbench immediately.  Doing this before the reply would deadlock:
`GetDeviceProc()` holds the device list locked until then (section 12.1.2).
No recursion - each child is handed a real partition and takes the "already a
partition" path.

Partition type bytes are not trusted on their own - `0x07` covers exFAT and
NTFS alike - so a candidate only counts if an exFAT boot sector is actually
present at its first sector.  The same applies to a GPT type GUID.

Three table shapes are walked, all in `amiga/parttable.c`:

- **MBR primaries** - the four entries at offset 446.
- **GPT** - behind a protective `0xEE` entry; header at LBA 1, entry array
  wherever the header says.
- **Extended / logical partitions** - types `0x05`, `0x0F`, `0x85`.  An
  extended partition has no boot sector of its own, only a chain of EBRs.
  **Each EBR uses two different bases**: its first slot describes a logical
  partition, start relative to *that EBR*; its second slot links to the next
  EBR, start relative to the *extended partition as a whole*.  Mixing those
  up walks off into nothing.  The chain is bounded at 16 links because a
  corrupt table can link to itself.

Each of those uses `buf` as its probe buffer, so the MBR is re-read before
the next entry is taken from it.  That restore used to be skipped on the GPT
path - harmless only because a protective MBR carries a single entry.

Published nodes use a synthetic geometry of `Surfaces = BlocksPerTrack = 1`,
so one cylinder is one block and `LowCyl`/`HighCyl` are plain LBAs.  That is
the only way an arbitrary partition range is expressible in a `DosEnvec`.

The exFAT boot sector's own `PartitionOffset` field is still ignored (spec
section 3.1.4): our position comes from the `DosEnvec` or from the partition
table, never from the volume itself.

### 2. Licence: GPL-2.0, derived from relan/exfat

This project is a port of the relan/exfat sources in this directory
(`libexfat/`, upstream <https://github.com/relan/exfat>, commit `1264da3`),
which are **GPL-2.0**. The resulting handler is therefore GPL-2.0 and must
carry the upstream copyright notices; keep `COPYING` and the per-file headers
intact, and keep the port a recognisable derivative rather than a rewrite that
loses attribution.

`libexfat/` is the format engine. The FUSE layer (`fuse/`) is not used — the
AmigaDOS packet loop replaces it.

### 3. Volumes past 4 GB

Use `NSCMD_TD_READ64`/`NSCMD_TD_WRITE64`, falling back to `TD_READ64`/
`TD_WRITE64` when `NSCMD_DEVICEQUERY` fails — the same order the 3.2 FFS uses
(`FastFileSystem-RelNotes`). `sagasd.device` already answers all of these.
Honour `de_MaxTransfer` and `de_Mask` on every transfer.

### 4. Platform constraints

- exFAT is **little-endian on disk**; the 68080 is big-endian. Never overlay a
  native C struct on an on-disk structure — byte-swap through accessors.
- `off_t` in ApolloCrossDev GCC 6.5 is **32-bit** (`typedef long _off_t`).
  Define and use a project 64-bit offset type for every volume and file offset.
- Buffers for CPU-only use go in Fast RAM. A cluster may be up to 32 MB, so
  size cluster buffers from `SectorsPerClusterShift`, never from an assumption.

## Verified on hardware

Milestone 1 (read-only mount and browse) is complete.  Confirmed on a Vampire
V4SA with a 256 GB exFAT SD card via `sagasd.device`, so these are measured
facts rather than assumptions:

- The card presents 512-byte blocks; the volume uses **256 KB clusters** with
  975808 clusters, and both ends of the partition sit past the 4 GB mark, so
  **NSD64** (`NSCMD_TD_READ64`) is the path actually in use.
- If a DOSDrivers mount file is used at all, it is a single-entry file whose
  filename is the device name, and it must say `FileSystem =`, never
  `Handler =`, or `dol_Startup` arrives ZERO.  Not needed in normal use -
  sagasd.device auto-mounts.
- `StackSize = 65536`.  The handler logs an error if headroom drops below
  8 KB; libexfat's VLAs plus path resolution are the consumers.
- A 1 MB file read through the handler is **byte-for-byte identical** to the
  original, and ApolloMap loads a Kickstart ROM directly from the volume.

Two traps that each cost a debugging round during bring-up:

- Packets where `dp_Arg1` is an `fh_Arg1` and **not** a BPTR - `EXAMINE_FH`,
  `COPY_DIR_FH`, `PARENT_FH`.  Running one through `BADDR()` corrupts memory
  or silently returns wrong data.
- `de_DosType` versus `id_DiskType`.  `'FATX'` says *which file system owns
  the partition* and belongs only in the mount file.  `id_DiskType` and
  `dl_DiskType` say *do we claim this medium* and must be `ID_DOS_DISK`
  (section 5.2.4, table 5.6).  Putting `'FATX'` in the latter makes `Info`
  report "Unreadable disk" while everything else works normally.

## Provisional choices made for milestone 1

These were needed to make the read-only handler work.  They are recorded here
so they stay visible, not because they are final - revisit each before the
write path is enabled.

1. ~~**DOSType `EXFA`**~~ - **resolved: AmigaOS uses `FATX` (`0x46415458`)**
   for exFAT.  Set in `amiga/handler.c` (`EXFAT_DOSTYPE`, reported in both
   `id_DiskType` and the volume node's `dl_DiskType`) and in `amiga/EXF0`.
   Any mounter or RDB / FileSystem.resource entry must use the same value.
2. **Character set: ISO-8859-1 direct.**  Unicode below U+0100 maps straight
   through; anything above becomes `?`.  Such a name is listable but cannot
   be opened, because the substitution is not reversible.
3. **Timestamps: local time, UTC offset ignored.**  exFAT records local time
   and so does `DateStamp`, so the exFAT value is used as-is and libexfat's
   timezone offset is forced to 0.  Bounded by a 32-bit `time_t`: dates past
   2038 wrap, while exFAT can record to 2107.
4. **Files over 2 GB: `fib_Size` clamped** to `0x7FFFFFFF` with a warning to
   the log.  Reading past 2 GB still works via the 64-bit `ACTION_SEEK`
   interpretation from RKM section 13.1.8.
5. **`EXAMINE_NEXT` re-walks the directory** from the start on every packet,
   keyed only on `fib_DiskKey`.  Correct under the lock-replacement rules of
   section 13.3.2, but O(n^2).

## Write phase progress

**ROM + WRITE=1, verified on hardware 2026-09-11.**  `exfat-handler 1.4`
built `ROMREG=1 WRITE=1 DEBUG=12` (62,968 bytes, 60,544 CODE) runs from
Kickstart, registers `FATX`, and `sagasd.device`'s auto-mount brings an
exFAT card up **read-write** with no `L:exfat-handler` on any volume.

The workload that proves it is a CD32 title, not a synthetic test: Brian
the Lion, 551 MB of preload blob read in one sustained pass while the
title also writes - CD audio correct throughout, and its save slots
reading EMPTY rather than "RAM FULL", which is a write-path answer
because `nonvolatile.library` only reports free space on a volume it can
write.  That is the largest read and the first real write this handler
has served from ROM.

Two things worth carrying forward from getting here:

- **An exFAT ROM does not need fat95.**  The card is exFAT, so fat95 has
  nothing to mount, and dropping it frees 27,336 bytes - which is what
  pays for write support (+16,564 over the read-only DEBUG=12 build,
  +11,264 without debug) inside a 128 KB budget that was previously
  5 KB short.  The read-only build was never the size ceiling it looked
  like; fat95 was.
- **`cdload.log` is a free verdict on the mount mode.**  cdload writes
  it to `SYS:`, so it raised a write-protect requester on the read-only
  exFAT card and simply appears on the read-write one.  A card tells you
  which way it mounted before the title starts.

The phases behind that, in the order they were brought up:

- **Phase 0 (host harness): done.**  `hosttest/` runs 30 unit tests and 10
  filesystem scenarios, each validated with `exfatfsck`.  `make test`.
- **Phase 1 (read-write mount): verified on hardware.**  Mounting read-write
  writes exactly one sector - the boot sector, to set `VolumeDirty`.  Round
  trip confirmed: Apple `fsck_exfat` and our `exfatfsck` both report the
  volume clean, and `VolumeFlags` reads 0 after `exfatctl EXF0: die`, so
  clean shutdown demonstrably clears the flag.
- **Phase 2 (file writing): verified on hardware.**
  `FINDOUTPUT`, `FINDUPDATE`, `WRITE`, `SET_FILE_SIZE`, `SET_PROTECT`,
  `SET_DATE`, and `END` flushing the directory entry.  `make WRITE=1` only.
  Confirmed: create and write; on-volume copy; a 1 MB (four cluster) file
  round tripping byte-for-byte; `exfatfsck` clean afterwards, so FAT chains
  and the allocation bitmap agree.  Shrinking verified by measurement:
  141 clusters used -> 145 after writing a 1 MB file -> 142 after
  overwriting it with a small one, so `exfat_truncate()` returns clusters
  to the bitmap exactly.
  Two bugs found here: reads dirty a node via atime (fixed by mounting
  `noatime` and flushing on close whenever the volume is writable), and
  `Copy` issues `SET_PROTECT`/`SET_DATE` after the data, so refusing them
  made a successful copy report a write-protected volume.
- **Directory listing performance.**  libexfat reads directory entries 32
  bytes at a time; `partition_io()` served each from its own 512-byte device
  transfer, so first entry into any uncached directory was slow (seconds for
  a ~120 file directory).  Fixed with a one-block read cache in
  `amiga/dev_io.c`.  Symptom to recognise: slow on first entry, instant on
  the second, because libexfat then has the directory cached in RAM.
- **Phase 4 (formatting): verified on hardware.**  The `mkfs` engine in
  `mkfs/` compiled for m68k with no changes beyond the `off_t` rename and
  fencing six stdout progress lines; `mkfs/main.c` is excluded and
  `amiga/format.c` supplies the `get_*()` accessors and the `objects[]`
  layout table it would otherwise provide.  Cluster sizing mirrors
  `mkexfatfs` so volumes match a host-made layout.  Only linked into
  `WRITE=1` builds (60 KB read-only vs 78 KB read-write).
- **Phase 3 (directory mutation): verified on hardware.**
  `DELETE_OBJECT`, `CREATE_DIR`, `RENAME_OBJECT`, plus `SET_COMMENT`
  accepting an empty comment.  Brought forward from its planned slot because
  Directory Opus deletes the destination before overwriting, so a file
  manager could not overwrite an existing file without it.
  `EXAMINE_NEXT` now keeps its position in the lock as section 13.3.2
  recommends, with the `fib_DiskKey` count as the rebuild path; deletes
  advance any scan sitting on the doomed entry and renames drop the cached
  positions.  The one-block read cache in `dev_io.c` is verified on hardware
  by a byte-for-byte compare of a multi-cluster file.  Confirmed on hardware:
  wildcard delete, deleting from an open Workbench window, renaming during an
  open listing, large directory listings, and `exfatfsck` clean afterwards.
- **Argument layouts differ between mutation packets** and are easy to get
  wrong: `DELETE_OBJECT` and `CREATE_DIR` take the lock in `dp_Arg1` and the
  path in `dp_Arg2`, while `SET_PROTECT`, `SET_DATE` and `SET_COMMENT` take
  them in `dp_Arg2` and `dp_Arg3` with `dp_Arg1` unused.  Note also that `exfatfsck` only
  checks that clusters a file references are marked allocated, never the
  converse, so leaked clusters pass it silently.  Measure `blocks used` via
  `exfatctl info` before and after, or use Disk Utility First Aid, which
  validates the bitmap both ways.  `CREATE_DIR`, `DELETE_OBJECT`,
  `RENAME_OBJECT`.  Note `EXAMINE_NEXT` re-walks the child list by index,
  which is safe read-only but interacts badly with a directory that changes
  mid-scan (section 13.3.2) - fix that as part of Phase 3.

## Partition discovery: verified on hardware

A three-partition card mounts all three volumes from one generic mount file
(`LowCyl = 0`): `EXF0:` serves the first, `EXF1:`/`EXF2:` are published and
activated, each with the correct geometry for its own extent.

**GPT verified too:** a card partitioned GPT with an EFI System Partition
plus two exFAT volumes mounts both, via `sagasd.device`'s auto-mount.  That
took a matching fix on the driver side - a GPT card normally carries an ESP
as entry 0, formatted FAT32, so a content probe finds FAT there first and
hands the whole card to fat95.  See the sagasd changelog.

**Extended / logical partitions verified** as well, so all three table
shapes - MBR primaries, GPT, and the EBR chain - are confirmed on hardware
on both the driver and handler sides.  Partition discovery has no untested
branch left.

Three traps found getting there, all worth remembering:

- **The handler must stay reentrant.**  A seglist is one loaded image, so
  every process started from it shares that image's data and BSS.  A
  ROM-resident handler reached through `FileSystem.resource` gives every
  partition the *same* seglist, so per-instance state in a global corrupts
  all of them - this was tried before the refactor and both volumes were
  destroyed within seconds.  Accordingly:
    - handler state is allocated per process in `exfat_handler_main()`;
      there is no `g_handler`;
    - the partition to open is passed through `exfat_mount()`/`exfat_open()`'s
      `spec` argument (which libexfat only forwards, never inspects) rather
      than a global in `dev_io.c`;
    - `log.c`'s render buffer is a caller's local;
    - the activation helper gets a *copy* of the names in its startup
      message, so it depends on nothing that `ACTION_DIE` can free.
  `SysBase`/`DOSBase` stay global deliberately: a library base is a
  singleton, so every instance writes the same value.
  **Do not add a mutable global that differs per volume.**
- `publish_partition()` prefers the `FATX` entry in `FileSystem.resource`
  (the ROM path, and what fat95 uses) and falls back to loading a private
  copy from `dol_Handler`.  `FSEF_SEGLIST`/`FSEF_GLOBALVEC` are not in NDK
  3.2; the bit numbers come from filesysres.doc ("$180 for substitute
  SegList & GlobalVec").
- **Never block the packet loop during startup.**  Activation locks each
  published name, which waits for that node's handler to start and answer.
  Done inline it left this volume unresponsive while the others came up, and
  froze the machine.  It runs in its own process (`CreateNewProc`) instead.
- **`DEBUG=23` is expensive.**  The debug UART is 115200 baud and
  `bug_print()` holds `Forbid()` for the whole message, so a trace line costs
  ~10 ms with multitasking off.  At TRACE level every packet pays that.  Use
  `DEBUG=6` unless actively debugging.

Build switches: `WRITE=1` for read-write, `ACTIVATE=0` to leave published
partitions to start on first access instead of at mount.

## sagasd.device integration

`3rdparty/mounter/mounter.c` `ScanMBR()` now probes the medium before it
builds the parameter packet, instead of assuming FAT:

- `rawread()` reads a plain block (RDB's `readblock()` cannot - it validates
  an identifier and a checksum).  It uses `TD_READ64` when the offset passes
  4 GB, so a partition high on a large card can still be probed.
- `detect_fstype()` checks LBA 0 for a card with no partition table, then
  each MBR entry's own boot sector.  The partition **type byte is not
  trusted**: `0x07` covers exFAT and NTFS, and formatters vary, so the boot
  sector decides - `"EXFAT   "` at offset 3, or a FAT12/16/32 signature at
  offset 54 or 82 with a `0xAA55` marker.
- `pp[DE_DOSTYPE]` becomes `FATX` (`0x46415458`) or `FAT\1` (`0x46415401`)
  accordingly, and the `FileSystem.resource` lookup uses the same value.
  Neither found means the card is not mounted at all, where before it was
  handed to fat95 regardless.
- For exFAT with no `FileSystem.resource` entry - the normal case, since
  nothing registers `FATX` - the node gets `dn_Handler = "L:exfat-handler"`
  so AmigaDOS loads it on first access.

Device naming stays `SDROM<unit>` for both.  The handler's published siblings
would then be `SDROM1`, `SDROM2`, which collide with units 1 and 2, so
`publish_partition()` walks forward to the next free name instead of losing
the partition.

## One handler per partition

A card mounted both from a DOSDrivers file and by `sagasd.device`'s
auto-mount gets two handler processes per partition, each with its own node
cache, allocation bitmap and dirty flag - which corrupts the volume.

`claim_partition()` guards against that: it claims each partition with a
public semaphore named `exfat/<device>/<unit>/<firstbyte>` and refuses a
second claim.  A named semaphore needs no DosList lock, so it works during
startup where walking the device list would deadlock.

**It is deliberately not called** (`handler.c`, in `startup()`).  The
mountlist route has been removed, so `sagasd.device` is the only mounter and
there is nothing to race.  Both functions stay compiled and type-checked so
the guard can be restored by putting the call back;
`release_partition()` is a no-op while `h->claim` is NULL, so the two are
never mismatched.

**The supported route is sagasd.device's auto-mount.**  It probes the boot
sector when it finds no RDB and hands an exFAT card to this handler, the way
it already does for FAT with fat95, so nothing per-card needs configuring.
The DOSDrivers mount file used during bring-up has been removed; the handler
still accepts one (whole device or a single partition) but it must never be
combined with the auto-mount.  The semaphore is the backstop, not the plan.

## Trap: a device's init() must not take DOS locks

`sagasd.device`'s `init()` used to mount its units directly.  `init()` runs
in the context of whoever triggered the load, and `MountUnit()` takes the
DosList lock via `AddDosEntry()`.  When the trigger is a file system handler
calling `OpenDevice()`, `GetDeviceProc()` is holding that lock while it waits
for the handler's startup packet - so:

    Mount EXF0: -> GetDeviceProc locks the DosList, starts the handler, waits
      handler -> OpenDevice("sagasd.device") -> ramlib loads it -> init()
        init() -> MountUnit -> AddDosEntry -> blocks on the DosList lock
    ... init() never returns, so OpenDevice never returns, so the startup
    packet is never replied, so the lock is never released.  Hard hang.

Section 12.1.2 of the AmigaDOS RKM describes the trap from the handler side.
Mounting now happens in the disk change daemon task, which already runs a
`CheckUnit()` pass before its first `Wait()`, so nothing is lost.

**Do not do DOS work in a device's init vector.**

## ROM residency: no writable static data at all

The ROM build tools reject a BSS section, and more fundamentally ROM cannot
be written - so the handler must hold **no** mutable static data.  It now has
none: `nm` over every object shows no `B`, `b` or `C` symbol, and the linked
binary has only CODE and DATA hunks.

What that required:

- **`SysBase`/`DOSBase` are per-function locals**, via the `EXFAT_BASES(h)`
  and `EXFAT_SYSBASE` macros in `amiga/exfat_amiga.h`.  `SysBase` is read
  from the fixed location 4; `DOSBase` comes from the per-process handler
  state.  Anything calling exec or dos needs one of these at the top.
- **A private allocator.**  libexfat uses `malloc`/`free`/`calloc`, and
  newlib's malloc wants a global `SysBase`.  `amiga/amigaos.c` implements
  them over `AllocVec()` with an eight byte size header.
- **Private `time()` and `strtol()`.**  Leaving either to newlib pulls in a
  chain - `strtol` -> `_impure_ptr` -> locale -> ctype -> stdio -> malloc,
  and `time` -> `_gettimeofday_r` -> the same - every link of which wants a
  global `SysBase`.  Providing both cut the binary from **82 KB to 64 KB**,
  because none of newlib's stdio is linked any more.
- **`format.c`'s mkfs parameters live on the stack**, reached through this
  process's `tc_UserData` for the duration of the call, because mkfs's
  `get_*()` accessors take no context.  Saved and restored around the call.
- Functions with no context of their own (`time()`, `make_serial()`) open
  dos.library for the call rather than borrow a global.

Link order matters: libgcc's 64-bit divide helpers call `__udivsi3`, which
lives in libc, so the link line is `-lc $(LIBGCC) -lc`.

- **The DATA hunk is const-only.**  `upcase_table` (5836 bytes) and
  `objects[]` were upstream non-const globals; both are read-only in fact,
  so they are now declared `const` and the up-case table moves into the CODE
  hunk.  What is left in DATA is the 28-byte `objects[]` pointer array,
  which gcc cannot put in CODE because it carries relocations.  It is never
  written, so ROM is safe.

**Check with:**

    nm obj/*.o | awk '$2=="B"||$2=="b"||$2=="C"'   # BSS/common: must be empty
    nm obj/*.o | awk '$2=="D"||$2=="d"'            # writable data: review each

Current sizes: **46,492** bytes read-only, **64,084** read-write.

## ROM residency - DISMISSED, do not resume without a new reason

**This is no longer a goal.**  The handler ships as `L:exfat-handler` and is
mounted by `sagasd.device`'s auto-mount, which is proven and is the supported
route.  Everything below is kept because it was expensive to learn and is
correct as far as it goes - not because the work is meant to continue.

Where it ended: a module matched to the handler on file size, CODE size,
hunk layout, relocation count and relocation spread **boots and runs its
`rt_Init`**, while the handler at that same weight does not.  Everything
checkable about the handler's binary is correct.  The cause was never
identified and would need Remus's own diagnostics, not more binaries.

What still matters from it, regardless of ROM:

- `amiga/entry.S` **must stay first in the link order**.  That is not a ROM
  concern: GCC 6.5.0 places a string literal at the start of the code hunk,
  so AmigaDOS would enter the handler on data and die with `80000004`.  The
  creep-ltx/exfat-aos3 port hit the identical trap independently.
- The no-writable-static-data work (no BSS, no DATA, `SysBase`/`DOSBase` as
  locals, private allocator) is what makes the handler **reentrant**, which
  is what lets several partitions share one seglist.  That is exercised in
  normal use and must not be undone.
- `ROMREG` defaults to 0, so the ROM tag registers nothing and costs nothing
  at run time; `rt_Init` is only ever called by a ROM boot scan.

## ROM residency: the module must be a Resident

Dropping the BSS is necessary but not sufficient.  A file destined for
Kickstart is not an executable that something runs - it is a **Resident**
("ROMTag") that the ROM boot scan finds and initialises, and a file without
one is rejected with *"no resident found"*.

**The reference is `~/fat95/src/fat95.s`** - the handler this workspace
already runs from ROM.  A first attempt that was structurally different
(tag 20 KB into the module, `RTF_AFTERDOS`, `rt_EndSkip` just past the tag,
a fabricated segment list, a second hunk) built cleanly and **crashed the
V4 during boot**.  The layout below copies fat95's instead.  Deviating from
it again needs a reason and a hardware test.

| | fat95 | here |
|---|---|---|
| Module start | `bra.s` past the tag | `jmp _exfat_handler_main` |
| ROMTag at | offset 2 | offset 6 |
| `rt_Flags` | `RTF_COLDSTART` | `RTF_COLDSTART` |
| `rt_Type` / `rt_Pri` | `NT_UNKNOWN` / 0 | `NT_UNKNOWN` / 0 |
| `rt_EndSkip` | `CodeEnd` | `__etext` |
| `fse_SegList` | `MKBADDR(Start - 4)` | a 14-byte segment in RAM |
| `fse_PatchFlags` | `$190` | `$190` |
| Hunks | one | one |

- **`amiga/entry.S`**, assembly and first in the link order, holds the first
  instruction and the tag right behind it.  C gives no way to order a
  function and a data object inside one object file, which is why this is
  not C.
- **`RTF_COLDSTART`, priority 10 (`PRI=` in the Makefile).**  fat95
  registers from ROM at coldstart, which is proof that
  `FileSystem.resource` already exists then.

  The priority has to be **above sagasd.device's 5**, and this note used
  to say 0 was fine because sagasd's mounting "is deferred by five
  seconds anyway".  That was wrong: the five-second delay is the
  DiskChangeDaemon's, and sagasd calls `CheckUnit()` **directly from its
  `init()`** as well - `ScanMBR()` looks for `FATX` in
  `FileSystem.resource` at coldstart, at priority 5.  Coldstart tags run
  in descending priority, so at 0 this module registered after that
  look, and the serial log said "exfat dostype not found" on every boot
  with nothing else wrong.  fat95 needed the same raise for the same
  reason.
- **`rt_EndSkip` is the end of the whole module**, not just past the tag.
  The linker defines `__etext` at the end of `.text`, which is the module's
  end **only because there is a single hunk** - see below.
- **One hunk, CODE only.**  Two `const` objects carrying relocations
  (`objects[]` in `format.c`) and one local initialiser template
  (`units[]` in `libexfat/utils.c`) were landing in DATA; the first is
  pinned to `.text`, the second made `static const`.  A single hunk is what
  makes `__etext` meaningful and matches fat95.
- **The segment list is built in RAM.**  AmigaDOS starts a handler from a
  seglist - a BPTR to a "next segment" longword with code right after it -
  which normally only `LoadSeg()` produces.  fat95 avoids fabricating one by
  using `MKBADDR(Start - 4)`, so `BADDR + 4` lands on the module's first
  instruction.  **That works only if the ROM builder places the module on a
  longword boundary**, because `MKBADDR` is a plain `>> 2` that discards the
  low two bits; two bytes out and AmigaDOS starts the handler in the middle
  of an instruction - a freeze with nothing in the log.  Where the module
  goes is Remus's choice, not ours, so `rt_Init` allocates fourteen bytes
  instead - size, a zero next-pointer, `JMP` to the ROM entry - which
  `AllocMem()` guarantees is longword aligned.  `CacheClearU()` after
  writing it: the 68080 has split caches and that JMP was written through
  the data cache.
- **`rt_Init` registers a `FileSysEntry`** for DOSType `FATX`, so every
  mounter finds the handler the way it finds the ROM FastFileSystem.
  `fse_PatchFlags` is `$190` - SegList, GlobalVec and **StackSize**: a node
  from `MakeDosNode()` carries a default stack far below the 64 KB this
  handler needs, and a mounter only overrides what the patch flags name.
  `fse_GlobalVec` is -1 (C, not BCPL).  An existing `FATX` entry is left
  alone.
- **One binary serves both routes.**  Offset 0 is still the handler's entry
  point, so the same file works as `L:exfat-handler`; a Resident in a file
  nothing scans is ignored.

## ROM residency: where this actually stands

**Proven on hardware:** the handler is a well-formed Remus ROM member.  It
boots, the ROM tag runs, and the machine is stable with an exFAT card
inserted at boot or later.  That is the **default build** (`ROMREG=0`), in
which `rt_Init` logs one line and registers nothing.

**Not working:** `ROMREG=1`, where `rt_Init` registers `FATX` in
`FileSystem.resource` so that mounting uses the ROM copy.  Until that works,
**the ROM copy is inert and `L:exfat-handler` is still required** - so ROM
residency is not yet achieved, it merely does no harm.

What the bisect established, in order, each on hardware:

1. `romstub` (272 bytes, a Resident and nothing else) boots and logs.  Remus
   integration, the tag layout and `RTF_COLDSTART` at priority 0 are all
   fine.
2. `romstub` padded to 62 KB with incompressible filler boots.  **Not a ROM
   space problem**, and not a module-size problem.
3. The full handler with `rt_Init` doing nothing boots, with and without a
   card.  **Not the module's content**, its 597 relocations, or Remus's
   handling of them.
4. The full handler registering `FATX` **freezes on mounting a card**, at
   boot or on later insertion, with no handler output at all - so AmigaDOS
   never reached the handler's code.
5. Building the segment list in RAM instead of `MKBADDR(entry - 4)` (see
   below) stopped the freeze, but the card then does not mount.  **That
   observation predates the startup marker and the `dn_GlobalVec` fix**, so
   it says less than it looks like it does - retest before trusting it.
6. `exfatres install` - the same `FileSysEntry` the ROM tag builds, but
   pointing at a `LoadSeg`ed copy - **mounts every partition correctly**.
   So the `FileSystem.resource` path is sound, and so is the shared-seglist
   reentrancy: three handler processes, one segment list, stable.

7. The registering build with the self-checks **crashes with no card in the
   machine and prints nothing at all** - not even `init entered`, which is
   `rt_Init`'s first statement.  So `rt_Init` is never called, and mounting
   is not involved.  The fault is in the ROM scan or the module layout, not
   in any code of ours that runs.

Lining the builds up by the size that actually occupies ROM - the relocated
`HUNK_CODE` image, not the file - gives an uncomfortable pattern:

| CODE bytes | Result |
|---|---|
| 200 | `rt_Init` ran |
| 62,200 (padded stub) | `rt_Init` ran |
| 62,108 (registers nothing) | `rt_Init` ran, both lines |
| 63,000 (registering) | booted, no mount, no romtag lines |
| 63,896 (registering + self-checks) | crash, `rt_Init` never called |

Everything at or below ~62 KB works and everything above ~63 KB does not,
regardless of what the code does.  That is why `romstub PAD=63700` exists:
it is 63,900 CODE bytes of a module that is otherwise trivial.  If **that**
crashes, this is a ROM space or layout limit and the handler has to shrink;
if it runs, size is exonerated at that size and the fault is in the module's
content.

9. `romstub` matched on **all three** dimensions (file 66,324 / CODE 63,900 /
   593 relocations) **boots**.  Size, file size and relocation count are all
   exonerated at the handler's exact weight, so the fault is the module's
   content.

At that point only one hardware-proven-good handler build exists: the old
non-registering one at CODE 62,108.  Everything since differs from it by
about 1.8 KB of our own code - the registration body, the self-checks and
the handler's startup marker.

10. With raw UART markers in `rt_Init` - `ApolloDebugPutStr()`, no exec call
    in the way - the registering build still prints **nothing**.  So
    `rt_Init` genuinely is never called, and the crash is in the ROM scan
    itself, before any of our code runs.

Also checked and clean, on both the crashing handler and the working stub:
the **whole file** scanned for `0x4AFC` at even offsets (one hit, the real
tag - the earlier scans only covered the CODE hunk), and the **relocated
image** simulated at four plausible ROM bases, looking for a false tag whose
`rt_MatchTag` self-matches.  None.

Where that leaves it: a module matched to the handler on file size, CODE
size and relocation count, with an identical tag, boots and runs its
`rt_Init`; the handler at that weight does not, and the only remaining
difference is that its content is real code with relocations spread across
the whole module rather than filler with them clustered low.  No test
performed so far discriminates further.

11. The **read-only** registering build (CODE 47,276) runs `rt_Init` to
    completion and logs every stage: the ROM entry verified at `00F12488`,
    `FileSystem.resource` found, the entry built, the segment list built
    (`seglist 00703C89 -> code 01C0F228`, which is arithmetically correct),
    `AddHead` done.  **Then the machine crashes anyway**, with no card in
    it.

That isolates it.  The module, the tag, the relocations and everything
`rt_Init` does are all fine.  The crash follows `rt_Init` returning, and the
only difference from the last known-good boot is that a `FileSysEntry` has
been left in `FileSystem.resource`.

12. `AFTERDOS=1` - the same module with **two bytes changed**, `rt_Flags` 4
    instead of 1 and `rt_Pri` -100 instead of 0 - crashes **earlier**, with
    no log at all, at a point in the boot where our module has done nothing.

**Do not use `RTF_AFTERDOS` on this ROM.**  Registration cannot be the cause
of that crash, because with AFTERDOS nothing was ever registered; the two
tag bytes are the only difference.  The likely explanation is that Remus does
not handle a negative-priority AFTERDOS module in the ROM's module table.
This also retro-explains the very first ROM attempt, which was AFTERDOS and
was written off at the time as having other defects.  Stay on
`RTF_COLDSTART` - at priority 10, not 0; see the tag notes above for why
0 lost the race against sagasd.device.

13. `NOADD=1` - the same read-only registering build with **24 bytes**
    changed, one `#ifdef` swapping `AddHead` for a log call - prints
    **nothing at all**.  `rt_Init` is not called.  The 24-bytes-smaller
    build ran it to completion.

## Unresolved: the ROM refuses to run this module's rt_Init, erratically

Whether `rt_Init` is called at all does not correlate with anything
measurable about the module:

| CODE | build | `rt_Init` ran? | Boot |
|---|---|---|---|
| 200 / 62,200 / 63,900 | stubs | yes | OK |
| 40,492 | `ro-tiny`, registering | **no** | crash |
| 47,276 | `ro-romreg`, registering | **yes, fully** | crash after |
| 47,300 | `ro-noadd`, +24 bytes | **no** | crash |
| 62,108 | `.noop`, non-registering | yes | OK |
| 64,152 | `.romreg`, registering | no | crash |

Not monotonic in size - the *smallest* build fails and a mid-sized one runs
to completion.  Not explained by content either: every candidate was checked
and is structurally identical and correct (one hunk, no DATA, no BSS,
`rt_EndSkip` exactly at the module end, one `0x4AFC` in the file and one in
the relocated image at four candidate ROM bases, `rt_MatchTag` self-matching,
entry a `JMP`, relocations demonstrably applied).

Two conclusions stated earlier in this file were overreach and are withdrawn:
that a size threshold existed, and that this was definitely a Remus layout
problem.  Both were drawn from tests that varied more than one thing.

## What fat95 does differently: relocations

Comparing the three modules as they exist on disk, with a parser that
understands **both** relocation block formats:

| module | format | relocations | offset spread | ROM |
|---|---|---|---|---|
| fat95 | RELOC32**SHORT** | **5** (offsets 4-24) | its ROM tag only | works |
| sagasd.device | RELOC32 | 102 | 10 .. 39,296 | works |
| exfat-handler | RELOC32 | 586 | 2 .. 62,550 | fails |

fat95's five relocations are exactly its ROM tag's pointer fields -
`rt_MatchTag`, `rt_EndSkip`, `rt_Name`, `rt_IdString`, `rt_Init`.  Its whole
body is PC-relative (`lea Start(pc),a0`, `lea FSRName(pc),a1`), so nothing
else needs relocating.  That is what hand-written assembly buys and what a
compiled 62 KB C module cannot have.

**Trap when measuring this:** a hunk parser that stops at unknown block types
reports fat95 as having *zero* relocations, because it uses
`HUNK_RELOC32SHORT` (0x3F7) rather than `HUNK_RELOC32` (0x3EC).  That
mistake was made here and briefly turned into a conclusion.  Handle both.

The earlier `romstub PAD=... RELOCS=...` matched the handler's relocation
*count* but packed them into a narrow band; the handler's are spread over
62 KB.  `make romstub SPREADN=586 SPREAD=104` interleaves filler with
relocated longwords to reproduce the distribution (591 relocations over
63,482 bytes against 586 over 62,550).  That is the test that discriminates
relocation density, which is the one dimension never varied.

Note the format itself is not the discriminator: sagasd uses plain RELOC32
and works from ROM.

**And density is not it either.**  `romstub SPREADN=586 SPREAD=104` - 591
relocations spread over 63,482 bytes, against the handler's 586 over 62,550 -
**boots and prints its line**.

`-mpcrel`, which is how fat95 gets down to five relocations, is not available
as a workaround: GCC 6.5.0 accepts it on small files but ICEs on `handler.c`
(`internal compiler error: in extract_constrain_insn, at recog.c:2199`).
Reducing relocation count would not have helped anyway, given the above.

### Structural analysis is exhausted

Every dimension that can be defined and matched has been matched, with a stub
that boots at the handler's exact weight on all of them:

| dimension | handler | matched stub | stub boots |
|---|---|---|---|
| CODE size | 62,948 | 63,488 | yes |
| file size | 65,344 | 65,904 | yes |
| hunks | 1 CODE | 1 CODE | yes |
| relocation count | 586 | 591 | yes |
| relocation spread | 2..62,550 | 6..63,482 | yes |
| tag offset / self-match / EndSkip | ok | ok | yes |
| `0x4AFC` count, file and relocated | 1 | 1 | yes |

The only remaining difference is that the handler's bytes are real compiled
code rather than filler.  Since `rt_Init` is never reached, that code never
executes, so whatever Remus objects to is a property of the bytes that no
test here can name.  **Further progress needs Remus's own output** - free
space, where it places the module, and any warning it emits - not more
binaries from this side.

**The control that was never run:** every ROM cycle after `.noop` changed
two or more things at once - read-only *and* registering, or a new switch
*and* new instrumentation.  Today's non-registering build is +840 CODE bytes
over `.noop` and carries five changes never tested in ROM (`rom_mark()`, the
relocation self-check, the seglist verification, the `dn_GlobalVec` fix, the
handler startup marker).  **Establish whether `make WRITE=1` still boots
before interpreting anything else.**  If it does not, the fault is in those
changes and is bisectable locally; if it does, it is registration or the ROM
build.

**Do not debug this further from the handler side.**  What is needed is what
Remus reports when it ingests the module - free space, where it places the
module, and whether it warns.  Ten ROM cycles went into narrowing this from
the binary alone; the binary is fine.  Everything checkable about it has been
checked: one hunk, no DATA, no BSS, one `0x4AFC` in the file and in the
relocated image at four bases, `rt_MatchTag` self-matching, `rt_EndSkip` at
the module end, entry a `JMP`, relocations applied (a boot log confirmed the
ROM entry at `00F12488`), and `rt_Init` demonstrably running to completion
and doing correct work.

If the goal is simply to get it into ROM, the lever is size:
`make DEBUG=0 ROMREG=1` is 40,492 CODE bytes, a third smaller than anything
that has crashed, and `rom_mark()` still logs because it calls
`ApolloDebugPutStr()` directly rather than through the `Debug_*` macros.

A confound that survived this far: the only handler build that ever booted
(`.noop`) was **read-write and non-registering**, while the read-only build
has only ever been tested **registering**.  Read-only vs read-write and
registering vs not were never varied independently.  Hence two controls,
both read-only and both COLDSTART:

| build | CODE | what it tests |
|---|---|---|
| `ro-plain` (`make`) | 46,072 | does the read-only module boot at all |
| `ro-noadd` (`ROMREG=1 NOADD=1`) | 47,300 | everything `rt_Init` does **except** linking the entry into the list |

`ro-noadd` boots -> `AddHead`, or the entry's presence in the list, is the
trigger.  `ro-noadd` crashes -> the `AllocMem`/`CacheClearU` work in
`rt_Init` is, and `ro-plain` then says whether the read-only module is sound
at all.

Note the earlier size story was a red herring: the bigger builds failed
before `rt_Init` for a reason still unknown, but the read-only build shows
the registration itself is what takes the machine down, independent of that.

**Beware of reading "no output" as "never ran".**  `Debug_Warn()` reaches the
serial port through `Forbid()` and `RawDoFmt()`; the stub poked the UART
registers directly.  So silence from `rt_Init` has always had two readings -
it never ran, or it ran and died inside that machinery - and the tests so far
could not tell them apart.  `rom_mark()` in `romtag.c` now prints through
`ApolloDebugPutStr()`, which is a poll-and-poke loop with no exec call in it,
at every stage of `rt_Init`.  That is what the next ROM cycle should settle.

8. `romstub` padded to **63,900 CODE bytes boots**.  So ROM space is not the
   limit at the size the handler crashes at.

But that stub matched only the handler's **CODE** size.  A module has three
sizes that move independently - CODE (what occupies ROM), file (what Remus
reads) and relocation count - and the handler's file is ~2.5 KB bigger than
its CODE because of 588 relocations.  `romstub PAD=61350 RELOCS=588` matches
all three (file 66,324 / CODE 63,900 / 593 relocations against 66,300 /
63,896 / 588) and is the test that actually discriminates.

Two conclusions drawn too early in this investigation, both from tests that
matched one dimension and not the others:

- "size is not the problem" - true at 62 KB, which is all the first padded
  stub tested;
- "the module content is fine" - that came from the build whose `rt_Init`
  does nothing, which is also the *smallest* build, so size and content were
  confounded.

If it does turn out to be content, the one difference left between the
working case and the broken one is that **ROM code is not writable**.  Nothing in the module should ever write
to itself - there is no DATA and no BSS - so if the ROM build still fails
with the self-checks in place, the next move is to find a write into the
code hunk, not to keep adjusting the tag.

Ruled out along the way, none of these were it: module size, ROM space,
relocation count, tag placement, `rt_Flags`, `rt_EndSkip`, `a6` clobbering
in `rt_Init`, a stray `0x4AFC`, the `FileSysEntry` patch flags, and the
segment struct's packing (`sizeof` 14, `entry` at offset 10, so the `JMP`
encodes correctly - verified with a compile-time assertion).

So the fault is in the registration path, somewhere between the
`FileSysEntry` and AmigaDOS starting a process from it - **not** in the ROM
module, the tag, or the handler proper.  Step 5 is where to resume: the
`ROMREG=1` build logs the seglist BPTR, the code address it resolves to and
the ROM entry address, which is enough to tell a bad BPTR from a bad mount.

The one significant thing still untested anywhere: several handler processes
sharing **one** seglist.  The ROM path is the first thing that would do it,
and the test card has three partitions.  `amiga/exfatres.c` now tests exactly
that from disk - it registers the same `FileSysEntry` the ROM tag builds, but
with a `LoadSeg`ed copy - so the shared-seglist question can be settled
without a ROM rebuild.  What it cannot reproduce is ROM being unwritable.

Two bugs found while auditing this, both real:

- `publish_partition()` set `dn_GlobalVec` from the `FileSysEntry` and then
  overwrote it from the parent three lines later.  It now only inherits the
  parent's value on the load-from-disk path.
- `exfat_handler_main()` logged nothing before `startup()`, so a failed mount
  could not be told apart from AmigaDOS never starting the process.  It now
  logs its own entry point address at `Debug_Warn` - which also says whether
  the process is running from ROM or from a `LoadSeg`ed copy.

**Check with:**

    objdump -h build/exfat-handler   # one CODE hunk, no DATA, no BSS
    nm obj/*.o | awk '$2=="B"||$2=="b"||$2=="C"||$2=="D"||$2=="d"'   # empty

and decode the tag in the linked file: exactly one `0x4AFC`, its
`rt_MatchTag` equal to its own offset, and `rt_EndSkip` equal to the hunk
length.

Build the ROM candidate with **`DEBUG=7`**: the registration line
`exfat romtag: registered 'FATX' ...` is `Debug_Info`, which the default
`DEBUG=6` compiles out, and it is the only evidence that the tag ran.

## Trap: MKBADDR needs a longword-aligned pointer

`MKBADDR(x)` is `((LONG)(x)) >> 2` - it silently discards the low two bits.
Applied to anything not longword aligned, `BADDR()` gives back a *different,
lower* address and whatever consumes it reads rubbish.

This bit us with a BSTR built from a string literal:

    static const UBYTE path[] = "\017L:exfat-handler";
    devnode->dn_Handler = MKBADDR(path);       /* WRONG - may be unaligned */

DOS then `LoadSeg()`ed from a garbage path and the failure surfaced as a
software failure **in the ramlib task**, several steps away from the cause.
Build such a BSTR in `AllocMem()`/`AllocVec()` memory, which is always
aligned.  Everything else passed to `MKBADDR()` in these projects is either
allocated or a stack struct of `LONG`s, so it is safe by construction.

## Verification status of the housekeeping packets

All verified on hardware:

- `ACTION_RENAME_DISK`, including a name longer than the original and
  repeated relabelling.  `dl_Name` is swapped under `Forbid()` rather than
  the volume node being replaced, so outstanding locks stay valid.
- `ACTION_FORMAT`, including reading the resulting volume on macOS - so the
  layout the Amiga writes is interoperable, not merely self-consistent.
  Refused unless the file system is inhibited **and** fully unmounted, which
  inhibit only does when no locks or open files remain.  `dp_Arg2` (the
  DosType) is ignored: this handler only makes exFAT.

- `ACTION_FLUSH` standalone.
- `ACTION_INHIBIT` both directions, with writes correctly refused while
  inhibited and working again after uninhibit.  Inhibit flushes and clears
  `VolumeDirty`, so it is the safe way to park a volume when `ACTION_DIE` is
  refused because locks are outstanding.
- `ACTION_DIE`, which flushes first and aborts if that flush fails rather
  than exiting with data possibly unwritten.

Still partial **by design**: inhibit does not block reads and uninhibit does
not re-validate the medium as section 13.9.2 requires, so it must not be used
to hand the raw device to another program such as `Format`.

## Write-phase choices (provisional, revisit before general use)

- **Flush policy: directory entries eagerly, the bitmap at sync points.**
  `exfat_flush_node()` (one 32 byte directory entry set) runs on file close
  and after each metadata change - cheap and keeps sizes and dates correct.
  `exfat_flush()` writes the **entire** allocation bitmap: `chunk_size` is
  `cmap.size`, so on a 238 GB volume with 975808 clusters that is ~122 KB
  per call.  Calling it after every delete/mkdir/rename made bulk operations
  visibly slow, so it now runs only at explicit sync points - `ACTION_FLUSH`,
  `ACTION_INHIBIT`, `ACTION_DIE` and unmount - which is also what the FUSE
  front end does (it calls `exfat_flush()` from `fsync` alone).
  This is safe because `VolumeDirty` is set for the whole session: an
  unclean shutdown leaves the volume flagged for checking rather than
  silently inconsistent.
- **`VolumeDirty` for the whole session**, set at mount via
  `exfat_soil_super_block()` and cleared by `exfat_unmount()`, matching what
  the FUSE front end does.  An unclean shutdown deliberately leaves it set.
- **`ACTION_SET_COMMENT`: refuse.**  exFAT has nowhere to store an 80
  character AmigaDOS comment, and silently discarding one loses user data
  without saying so.
- **Write-protected media degrades to a read-only mount** rather than
  failing, via `TD_PROTSTATUS` and the `ro_fallback` mount option.

## Open decisions - do not guess, ask

1. **Enabling writes.**  Controlled by `make WRITE=1`, which sets
   `EXFAT_AMIGA_ALLOW_WRITE`.  Default is 0 and compiles the write path out.
   Read-write builds are for **scratch media only** until each phase is
   proven; see `amiga/README.md`, "Write bring-up".
2. **`mkfs` / `fsck` equivalents** - in scope or not.
3. **Protection bits and comments.**  Currently every object reports
   `FIBF_WRITE | FIBF_DELETE` set (read-only) plus `FIBF_ARCHIVE` from the
   exFAT attribute.  exFAT has nowhere to keep an AmigaDOS comment.
4. **Testing.**  Real V4SA hardware only, or is there an emulator path?  A
   host-side build of the format logic for unit testing is still advisable.

## Build

`amiga/` holds the AmigaOS side and its own Makefile; see `amiga/README.md`
for building, installing, mounting and what to check on the first run.
