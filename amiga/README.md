# exFAT handler for AmigaOS 3.2 — milestone 1 (read-only)

A port of the free exFAT implementation (relan/exfat, GPL-2.0) to an
AmigaDOS packet handler. `../libexfat/` is the upstream format engine with
platform branches added; everything AmigaOS-specific lives in this directory.

**Status: milestone 1 complete and verified on hardware.** Tested on a
Vampire V4SA against a 256 GB exFAT SD card through `sagasd.device`:

| Verified | |
|---|---|
| Mount | boot region, up-case table, allocation bitmap; 256 KB clusters, 975808 clusters, NSD64 addressing past 4 GB |
| `Info` | correct size, used and free space; reports `ID_DOS_DISK` |
| `Dir` / `List` | directory entry sets, UTF-16 names, sizes, dates, protection bits |
| `Type` | file open, read, EOF, close |
| `Copy` | 1 MB file copied off the card, **byte-for-byte identical** to the original — validates cluster chaining, the head/middle/tail split and `de_MaxTransfer` chunking |
| Direct use by a real tool | ApolloMap loads a 1 MB Kickstart ROM straight from the volume |

Not verified: **writing**, which remains compiled out.

## Building

```
make                 # read-only handler + mbrscan + exfatctl
make                 # read-write, registers FATX - both unconditional
make DEBUG=7         # add INFO messages to the serial log
make clean
```

**`WRITE=0` is the default and compiles the write path out entirely.**
`WRITE=1` is for bring-up on an expendable card; see "Write bring-up" below.

`DEBUG` is the ApolloCrossDev debug bitmask: 1 INFO, 2 WARN, 4 ERROR,
8 FLAG, 16 TRACE. The default is 6 (warnings and errors).

**Do not leave `DEBUG=23` on for normal use.** The debug UART runs at 115200
baud (~11.5 KB/s) and `bug_print()` wraps each message in `Forbid()`, so a
~120 character trace line costs about **10 ms with multitasking disabled**.
At TRACE level every packet logs a line, so mounting several partitions while
Workbench probes them can spend seconds in `Forbid()`. Use `DEBUG=6` unless
you are actively debugging. Diagnostics go out
of the serial port via `docs/toolchain/_ApolloLib/ApolloCrossDev_Debug.c`,
the same facility sagasd.device uses — watch them with a serial terminal.

## Installing

```
copy build/exfat-handler L:exfat-handler
copy build/exfatctl      C:
copy build/mbrscan       C:
```

Plus the matching `sagasd.device` in `DEVS:`. **That is the whole install —
there is no mount file.** When sagasd.device finds no RDB it probes the boot
sector, and hands an exFAT card straight to this handler, exactly as it does
for FAT with fat95. Volumes appear as `SDROM<unit>:` plus the sibling names
the handler publishes for any extra partitions.

Then `Info SDROM0:`, `List SDROM0:`, `Dir SDROM0:`, `Type SDROM0:somefile`.

A `DOSDrivers` mount file naming the whole device (`LowCyl = 0`) or one
specific partition still works and is occasionally handy for bring-up — but
never alongside the auto-mount for the same card. See "Do not mount a card
twice".

### Putting it in ROM

The same binary goes into a Kickstart ROM image (built with Remus) — it
carries a `Resident` tag, which is what the ROM tool scans for.

**ROM residency is no longer a goal** — see `../CLAUDE.md`, "ROM residency —
DISMISSED". The handler ships in `L:` and is mounted by `sagasd.device`'s
auto-mount. The `Resident` tag stays in the binary but registers nothing
(`ROMREG` defaults to 0) and costs nothing at run time, since `rt_Init` is
only ever called by a ROM boot scan.

`entry.S` still matters and must stay first in the link order — that is a
GCC 6.5.0 code-layout issue, not a ROM one.

Check it before handing the file to the ROM tool:

```
make clean && make DEBUG=7 WRITE=1
m68k-amigaos-objdump -h build/exfat-handler   # ONE hunk: CODE. No DATA, no BSS
```

A second hunk means writable static data crept back in, and it also breaks
`rt_EndSkip` — see `../CLAUDE.md`, "ROM residency", which lists the layout
this must match and why. Build with `DEBUG=7`: the boot log line
`exfat romtag: registered 'FATX' ...` is the only evidence the tag ran, and
the default `DEBUG=6` compiles it out.

### Testing the ROM path without a ROM: `exfatres`

A ROM rebuild per experiment is a slow way to debug. `exfatres` does from the
Shell exactly what the ROM tag does — `LoadSeg` the handler and add a
`FileSysEntry` for `FATX` — so every mount afterwards takes the resident code
path:

```
exfatres install              # registers L:exfat-handler
exfatres install L:my-build   # a specific file
exfatres list                 # what is registered
```

Then insert the card. `sagasd.device`'s mounter finds the entry, patches
`dn_SegList` from it, and **every partition is served by processes sharing
one segment list** — which is what a ROM-resident handler does, and the one
thing never yet tested.

| Result | Meaning |
|---|---|
| Works here, fails from ROM | The handler writes to its own code or static data. It should not: the module has no DATA and no BSS. |
| Fails here too | The fault is the `FileSystem.resource` path or the shared seglist — and it can now be debugged from disk, with full logging and no reflash. |

There is no `remove`: once a volume is mounted, processes are running from
that segment list and freeing it would take the machine down. Reboot.

### When a ROM build crashes: `romstub`

```
make romstub        # build/romstub, ~270 bytes
```

`romstub.S` is a Resident and nothing else — no C, no libc, no exec calls.
Its `rt_Init` writes one line to the debug UART and returns. Put it in the
ROM *instead of* `exfat-handler` and boot:

| Result | Meaning |
|---|---|
| `*** exfat romstub: ROM tag ran, module is alive ***`, machine boots | ROM integration works for a module of this shape. The problem is specific to `exfat-handler` — its size or its content. |
| No line, or still crashes | The problem is the ROM image or the tool, not the handler. Check free ROM space first. |

**Match all three dimensions, not just one.** A module has a CODE size (what
occupies ROM), a file size (what the ROM tool reads) and a relocation count,
and they move independently — the handler's file is ~2.5 KB bigger than its
CODE because of 588 relocations. Matching only CODE proves less than it
looks like it does; that mistake cost a round here.

```
make romstub PAD=61350 RELOCS=588     # file 66,324  CODE 63,900  relocs 593
```

`PAD` adds incompressible filler (CODE), `RELOCS` adds that many `RELOC32`
entries (CODE *and* file). Read the real numbers back with:

```
objdump -h build/romstub              # CODE size
ls -l build/romstub                   # file size
```

That separates the last two explanations. **Padded stub boots** → the ROM has
room, so the problem is what is *in* `exfat-handler`. **Padded stub crashes**
→ the ROM is out of space at that size and the handler has to shrink. The
filler is random rather than zeroes on purpose: a tool that packs or
compresses modules would swallow a run of zeroes and the test would wrongly
pass.

If the padded stub boots, the ROM has room and the fault is in the handler's
content. Bisect that with:

```
make clean && make WRITE=1 ROMTAG_NOOP=1
```

That is the whole handler — every byte of code, every relocation — with an
`rt_Init` that logs and returns without touching `FileSystem.resource`.
**Boots** → the module itself is fine and the fault is in what `rt_Init`
does. **Crashes** → the fault is the module's content, independent of what
the tag does.

`rt_Init` also logs its progress at `Debug_Warn`, so a default `DEBUG=6`
build shows how far it got:

```
exfat romtag: init entered
exfat romtag: FileSystem.resource at ........
exfat romtag: no 'FATX' entry yet, adding one
exfat romtag: registered 'FATX' 0.1, seglist ........
```

Whichever line is last is where it died.

If it has to shrink, these are the levers (fat95, for scale, is 27,688 bytes):

| Build | Size | Loses |
|---|---|---|
| `make DEBUG=0` | 41,460 | writing and formatting, all diagnostics |
| `make` | 47,108 | writing and formatting |
| `make DEBUG=0 WRITE=1` | 56,888 | all diagnostics |
| `make WRITE=1` | 64,700 | — |
| `make DEBUG=7 WRITE=1` | 68,456 | — (adds INFO logging) |

Dropping `WRITE=1` removes the `mkfs` engine, of which 5,836 bytes is the
up-case table alone. `DEBUG=0` removes every format string and call site.

## Finding the partition: `mbrscan`

`make` also builds `build/mbrscan`, a small CLI tool that reads the partition
table and prints the exact `LowCyl`/`HighCyl` to use in a mount file, if you are using one:

```
mbrscan                      ; defaults to sagasd.device unit 0
mbrscan sagasd.device 1
```

It reads LBA 0, handles a plain MBR, a GPT behind a protective MBR, and a
"superfloppy" card with no partition table at all, and for each candidate it
checks whether an exFAT boot sector is really there before recommending it.

This lives outside the file system on purpose. Locating partitions is the
device driver's job, never the handler's (`../CLAUDE.md`, settled decision 1),
and this tool is also a sketch of the enumeration `sagasd.device` will
eventually do for itself.

## exfatctl

AmigaOS 3.2 has no standard unmount command, and a file system that is not
shut down cleanly leaves exFAT's `VolumeDirty` flag set — the next host to
mount the card will want to check it.

```
exfatctl SDROM0: die         flush, clear the dirty flag, shut down
exfatctl SDROM0: flush       write out everything pending
exfatctl SDROM0: info        what the handler thinks the state is
exfatctl SDROM0: inhibit     / uninhibit
exfatctl ?                 show the argument template
```

Both `exfatctl` and `mbrscan` take their arguments through `ReadArgs()`
rather than `argc`/`argv`. The startup code the tools link against does not
populate `argc`/`argv` from the Shell, so a tool written the C way silently
sees no arguments. `ReadArgs()` reads the Shell's argument line through
`pr_CIS` and is independent of the startup code — and it gives `?` help and
proper error messages for free.

**Run `exfatctl SDROM0: die` before pulling the card or rebooting** on a
read-write build.

`die` flushes first and **aborts if the flush fails**, staying up rather than
exiting with data possibly unwritten — `VolumeDirty` then stays set so a host
will check the card.

`die` also refuses with `ERROR_OBJECT_IN_USE` (202) while any lock or open file
remains — §13.9.5 explicitly permits a file system to refuse in that case.
Close any Workbench window on the volume and `CD` elsewhere, then retry.

If something will not let go, **`exfatctl SDROM0: inhibit`** is the safe way
out: it flushes everything and clears `VolumeDirty` without the handler
exiting, so the card can be removed and will not need checking on the next
host. `uninhibit` resumes. Writes are refused while inhibited.

## Write bring-up (Phase 1)

The first read-write mount writes exactly **one 512-byte sector**: the boot
sector, to set and later clear `VolumeDirty`. The two fields it touches,
`VolumeFlags` and `PercentInUse`, are precisely the ones the boot checksum
excludes (spec §3.4), so no checksum recomputation is involved. That makes it
the smallest possible first write.

On a **scratch card**:

1. `make WRITE=1 DEBUG=23`, copy the handler over, re-mount.
2. The log should say `volume mounted read-write and marked dirty`.
3. `exfatctl SDROM0: info` → state should be `read/write`, not `write protected`.
4. `exfatctl SDROM0: die` → `shut down cleanly`.
5. Move the card to the host and run `hosttest/build/exfatfsck /dev/...` (or
   mount it). It must report **no errors**, and the volume must not be
   flagged dirty.

Step 5 failing means the write path corrupted the boot sector — which is why
this is done on a card you can afford to lose, and why nothing else is
enabled yet. If it passes, the device write path, the byte swapping on write,
and clean shutdown are all proven before any file data is at risk.

## Write bring-up (Phase 2) — file writing

**Use a scratch card.** Phase 1 only ever wrote two flag bytes in the boot
sector, so any card was safe. Phase 2 allocates clusters and writes file
data: a bug here can lose a volume.

Implemented: `ACTION_FINDOUTPUT` (create/replace, exclusive),
`ACTION_FINDUPDATE` (open or create, shared), `ACTION_WRITE`,
`ACTION_SET_FILE_SIZE`, and `ACTION_END` now flushing the directory entry.
Directory mutation (create/delete/rename) is still refused — that is Phase 3.

Ladder, checking with `sudo ../hosttest/checkcard.sh diskNsM` after each step:

1. `Echo >SDROM0:hello.txt "test"` then `Type SDROM0:hello.txt` — smallest
   possible create-and-write.
2. `Copy SDROM0:hello.txt SDROM0:hello2.txt` — read and write on the same volume.
3. Copy a file **larger than one cluster** (>256 KB on a typical card) from
   `RAM:` to `SDROM0:`, then copy it back and compare. This is the first real
   test of cluster allocation and the bitmap.
4. Copy the 1 MB Kickstart ROM across and compare byte-for-byte on the host.
5. Overwrite an existing file with a **shorter** one — exercises truncation.
6. `exfatctl SDROM0: die`, then `checkcard.sh`: must be clean **and** the files
   must be readable on the host with the right contents.

What to watch in the serial log: `created <path>`, and any `write of N bytes
failed` or `truncate failed`. A full volume should report `ERROR_DISK_FULL`
rather than anything dramatic.

## What is implemented

| Packet | Behaviour |
|---|---|
| `ACTION_STARTUP` | Reads the `FileSysStartupMsg` and `DosEnvec`, opens the device, mounts read-only, sets `dol_Task`. |
| `ACTION_LOCATE_OBJECT` | Resolves an AmigaDOS path (`:` = absolute, leading `/` = parent) and returns a `FileLock`. |
| `ACTION_FREE_LOCK`, `COPY_DIR`, `COPY_DIR_FH`, `PARENT`, `SAME_LOCK` | Lock lifecycle. |
| `ACTION_EXAMINE_OBJECT` | Fills a `FileInfoBlock`; on the root, reports the volume label as the name. Arms a directory scan. |
| `ACTION_EXAMINE_NEXT` | Continues the scan, keyed on `fib_DiskKey` only. |
| `ACTION_FINDINPUT`, `END`, `READ`, `SEEK` | File access. `END` flushes the directory entry when the file was writable. |
| `ACTION_FINDOUTPUT`, `FINDUPDATE`, `WRITE`, `SET_FILE_SIZE` | **`WRITE=1` builds only.** Create, replace, write and truncate. |
| `ACTION_DELETE_OBJECT`, `CREATE_DIR`, `RENAME_OBJECT` | **`WRITE=1` builds only.** Delete files and empty directories, create directories, rename and move. |
| `ACTION_SET_COMMENT` | **`WRITE=1` builds only.** Accepts an empty comment, refuses a non-empty one with `ERROR_ACTION_NOT_KNOWN` — exFAT has nowhere to store it. File managers set a comment on every copy, so refusing outright turned a good copy into a "write protected" requester. |
| `ACTION_SET_PROTECT`, `SET_DATE` | **`WRITE=1` builds only.** `Copy` issues both on the destination after the data, so refusing them makes a successful copy report a write-protected volume. exFAT keeps only ReadOnly/Archive of the AmigaDOS bits; §13.5.3 permits implementing a subset. |
| `ACTION_EXAMINE_FH`, `FH_FROM_LOCK`, `COPY_DIR_FH`, `PARENT_FH` | The file-handle variants. `dp_Arg1` is `fh_Arg1` on all but `FH_FROM_LOCK`, never a BPTR. |
| `ACTION_INFO`, `DISK_INFO`, `CURRENT_VOLUME`, `IS_FILESYSTEM`, `FLUSH`, `INHIBIT`, `DIE` | Volume and housekeeping. |
| Mutating packets | Answered `ERROR_DISK_WRITE_PROTECTED`, not "action not known", so callers get a sensible message. |
| Anything else | `ERROR_ACTION_NOT_KNOWN`. |

## Troubleshooting

**`Mount SDROM0:` succeeds but the first access fails.** Where startup errors
appear depends on the `Mount`/`Activate` keyword (they are synonyms). With
`Mount = 1` the handler is loaded and started by `Mount` itself; without it,
`Mount` only adds the `DosList` entry and the handler starts the first time a
path touches the device — so the error surfaces on the first `cd`/`list`
instead. Either way the serial log has the real reason.

**`dol_Startup is ZERO - no FileSysStartupMsg`.** The mountlist says
`Handler =` where it must say `FileSystem =`. Only `FILESYSTEM` (or
`EHANDLER`) makes `Mount` build a `FileSysStartupMsg` from the
`DEVICE`/`UNIT`/`FLAGS` keywords; with `HANDLER`, `dol_Startup` comes from the
`STARTUP` keyword and is ZERO when that is absent. AmigaDOS RKM sections
7.1.1 and 7.1.2.

**A meaningless error number in the shell** (a large value that looks like an
address) after a failed startup. The handler replies `DOSFALSE` with a proper
error code and exits as section 12.1.2 requires; what the shell prints in that
path is not reliably the code we passed. Trust the serial log for the real
reason.

**`exFAT file system is not found`.** `LowCyl` is not where the volume
starts. This is the only thing `exfat_mount()` checks first — the `"EXFAT   "`
signature at offset 3 of the partition's first sector — so it means the read
worked and the content simply is not an exFAT boot sector. Run `mbrscan` (see
below) to get the right numbers. A wrong range cannot damage anything.

**Mount succeeds but the volume is empty or names are garbage.** The range
points at a different partition, or the byte swapping is wrong. Check the
range against `mbrscan` first before suspecting the file system.

## Known limitations

- **Read-only.** `EXFAT_AMIGA_ALLOW_WRITE` in `exfat_amiga.h` gates the write
  path at compile time and is 0. Do not turn it on until reads are proven.
- **Names.** Unicode above U+00FF becomes `?`, which makes such a file
  visible but not openable by name.
- **Files over 2 GB.** `fib_Size` is a `LONG`; larger files are reported as
  2 GB − 1 and a warning goes to the log. Reading past 2 GB works, since
  `ACTION_SEEK` uses the 64-bit interpretation from §13.1.8 of the RKM.
- **Dates after 2038.** `time_t` is a 32-bit `long` on this toolchain, so
  timestamps beyond it wrap. exFAT can record up to 2107.
- **`ACTION_EXAMINE_NEXT` keeps its position in the lock**, as §13.3.2
  recommends, and falls back to counting from `fib_DiskKey` if the lock was
  replaced or an entry moved. Deleting an entry advances any scan sitting on
  it; a rename drops the cached positions so affected scans re-count. Bulk
  operations like `Delete SDROM0:#?` are therefore safe.
- **`dev_io.c` keeps a one-block read cache.** libexfat reads directory
  entries 32 bytes at a time (`read_entries()`), so without it every one of
  the 16 entries in a 512-byte sector cost its own device transfer. A
  directory of ~120 files went from roughly 480 round trips to 30, which is
  the difference between seconds and a fraction of one on SPI SD. Any write
  invalidates the cached block. Read-ahead (pulling several KB per miss)
  would cut it further and is the obvious next step if it is ever needed.
- **The allocation bitmap is written only at sync points.** `exfat_flush()`
  rewrites the whole bitmap — ~122 KB on a 238 GB volume — so it runs on
  `ACTION_FLUSH`, `INHIBIT`, `DIE` and unmount, not after every mutation.
  Directory entries are still flushed eagerly. Pull the card without
  `exfatctl die` or `inhibit` and the bitmap may be stale; `VolumeDirty`
  will be set, so the next host will check and fix it.
- **Mounted `noatime`.** AmigaDOS has no access-time concept — a
  `FileInfoBlock` carries only `fib_Date` (mtime) — so letting libexfat
  update atime on every read would dirty the node and cost a metadata write
  per read, for nothing. Bad for SD wear and speed.
- **`ACTION_INHIBIT` is partial.** It flushes, clears `VolumeDirty`, reports
  `'BUSY'` per Table 5.6 and refuses writes — enough to park a volume safely.
  But reads are not blocked, and uninhibiting does not re-validate the medium
  as §13.9.2 requires. Do not use it to hand the raw device to another
  program such as `Format`.
- **`id_DiskType` is not the file system's DOSType.** It reports
  `ID_DOS_DISK` ("we recognise this medium"), per §5.2.4, which says
  explicitly that it "shall not be used to identify a particular file
  system". `'FATX'` belongs in `de_DosType` — the mount file's `DosType`
  keyword — and nowhere else. Putting `'FATX'` in `id_DiskType` makes `Info`
  report "Unreadable disk", because Table 5.6 does not list it.
- **`StackSize` matters.** `libexfat/node.c` puts directory entry sets on the
  stack as variable-length arrays, up to 8 KB, and `exfat_put_node()` itself
  declares a 766-byte name buffer. The mount file asks for 64 KB; do not
  lower it. Note the headroom figure in the trace is sampled at the *top* of
  the packet loop, so it shows the loop is healthy but does not reveal the
  peak reached inside libexfat — treat the 64 KB as deliberate headroom
  rather than a measured requirement.

## What to check on the first run

In rough order, because each step depends on the one before:

1. **Mount succeeds.** The log should show `mounting …`, then `mounted, label
   "…"`, then `volume "…" published`. A wrong `LowCyl` shows up here as a
   mount failure.
2. **`Info SDROM0:`** — total and used blocks are in *clusters*, not 512-byte
   blocks, so the numbers will look small next to a normal Amiga volume.
   Confirm the size is plausible for the card.
3. **`Dir SDROM0:`** — exercises `EXAMINE_OBJECT` + `EXAMINE_NEXT`. This is the
   first real test of the byte swapping: garbled names or nonsense sizes mean
   an endianness or offset bug, not a device bug.
4. **`List SDROM0:`** — adds dates and protection bits.
5. **`Type SDROM0:<file>`** on a small text file — exercises `FINDINPUT`,
   `READ`, `END`.
6. **A large file**, bigger than one cluster and not cluster-aligned in
   length, compared byte-for-byte against the original. This is what
   validates the head/middle/tail split in `dev_io.c`.
7. **A file past the 4 GB point** on the medium, if the card is big enough —
   that is the only thing that proves the TD64/NSD64 path.

## Layout

| File | Contents |
|---|---|
| `entry.S` | The first instruction and the ROM `Resident` right behind it. **Must stay first in the link order** — AmigaDOS enters at the first byte of the segment, and the tag has to be in the first code hunk. Assembly because C cannot order a function and a data object within one object file. |
| `handler.c` | Startup, packet loop, locks, path resolution, `FileInfoBlock` filling. |
| `dev_io.c` | The Exec block-device back end: `CMD_READ`/`TD_READ64`/`NSCMD_TD_READ64`, `de_MaxTransfer` chunking, `de_Mask` bouncing, unaligned head/tail handling. |
| `amigaos.c` | A self-contained `vsnprintf` subset (no libc stdio in a handler), UTF-8 ↔ Amiga 8-bit conversion, exFAT time → `DateStamp`, and the two newlib stubs the link needs. |
| `device.h`, `newstyle.h` | Shims so the shared `ApolloCrossDev_Debug.c` compiles here unmodified. |
| `romtag.c` | The `rt_Init` the ROM tag points at: registers `FATX` in `FileSystem.resource`. Inert in a disk-loaded copy. |
| `parttable.c` | MBR and GPT scanning, used when the handler is given a whole device. |
| `format.c` | `ACTION_FORMAT`: drives the `mkfs` engine in `../mkfs/`. Read-write builds only. |
| `mbrscan.c` | CLI tool that finds exFAT partitions and prints their `LowCyl`/`HighCyl`. |

## Changes made to `../libexfat/`

Kept as small and as clearly fenced as possible, so upstream remains
mergeable:

- `platform.h` — an `__amigaos__` branch: big-endian byte order, GCC bswap
  builtins, and `typedef int64_t exfat_off_t`.
- All sources — `off_t` renamed to `exfat_off_t` (62 occurrences). The
  toolchain's `off_t` is a 32-bit `long`, which upstream's own
  `STATIC_ASSERT(sizeof(exfat_off_t) == 8)` now correctly enforces against
  our type instead.
- `io.c` — the POSIX device layer is inside `#if !defined(__amigaos__)`;
  `exfat_generic_pread`/`pwrite` stay shared and unmodified.
- `log.c` — an AmigaOS backend routing to `Debug_Error`/`Warn`/`Info`.
- `time.c` — `exfat_tzset()` fenced out; the AmigaOS one is in `amigaos.c`.
- `mount.c` — no `geteuid`/`getegid`; uid and gid default to 0.
- `utils.c`, `repair.c` — `exfat_print_info()` and the interactive repair
  prompt fenced out; both need stdio.
