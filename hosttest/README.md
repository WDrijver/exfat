# Host test harness

Runs on the **build machine**, not the Amiga. This is Phase 0 of the write
work: get the format logic wrong here, where a failure costs a second, rather
than on a card.

```
make            # build everything
make test       # or ./run-tests.sh
./run-tests.sh grow fragment    # just these scenarios
make clean
```

## Why this works without any emulation

libexfat's POSIX device layer is still present — the AmigaOS port fenced it
off with `#if !defined(__amigaos__)` rather than deleting it — and
`exfat_open()` explicitly accepts a regular file (`S_ISREG`). So a plain disk
image **is** the block device. No host device layer had to be written.

The repository also already ships `mkfs` and `fsck`, which build natively, so
test volumes can be created and validated with the same format engine.

## What it runs

**Unit tests** (`unittest.c`) for the port's pure functions. These compile
`../amiga/amigaos.c` natively through `hostcompat.h`, so they exercise the
code that actually ships rather than a copy:

- `exfat_amiga_time_to_datestamp()` — checked against an independent
  civil-days algorithm across leap years and the epoch boundary, so the
  hard-coded 2922-day offset is proven rather than assumed.
- The UTF-8 ↔ Amiga 8-bit conversion, including the substitution of
  characters above U+00FF and the length cap.
- `exfat_amiga_vsnprintf()`, the hand-rolled formatter the log path depends
  on, including 64-bit conversions and truncation behaviour.

**Filesystem scenarios** (`fstest.c`), each on a fresh 64 MB image with 4 KB
clusters, using the same call sequences the handler's packet cases use:

| Scenario | Exercises |
|---|---|
| `small` | one sub-cluster write |
| `spanning` | a write crossing several cluster boundaries |
| `grow` | writing past the end, and that the hole reads as **zeroes** — the `ValidDataLength` rule of spec §7.6.5, which is what stops deleted data leaking into a file |
| `shrink` | `exfat_truncate()` downward, freeing clusters |
| `dirs` | nested directory creation, and that a non-empty directory refuses `rmdir` |
| `rename` | rename in place and move between directories |
| `delete` | unlink, cleanup, then rmdir |
| `names` | long names, Latin-1 names, spaces, mixed case |
| `fragment` | free a middle allocation, then grow another file into the hole |
| `fill` | write until the volume is full; must fail cleanly |

## The pass criterion

A scenario passes only if **both** its own checks pass **and** `exfatfsck`
reports the resulting image clean. A scenario that appears to work but leaves
a corrupt volume is a failure — that is the whole point.

This was verified to have teeth: corrupting eight sectors of the FAT in an
otherwise-good image makes `exfatfsck` exit 1 and report
`ERRORS FOUND: 1`, and the harness's verdict flips to FAIL.

For an extra independent check, images can also be mounted on macOS
(`hdiutil attach build/test.img`) and the contents compared there.

## Checking a real card

```
sudo ./checkcard.sh disk4s1        # diskutil list to find it
```

Unmounts the volume, reports the exFAT `VolumeDirty` flag, then runs
`exfatfsck` on it.

**Two traps this exists to avoid:**

- **Use the buffered device (`/dev/disk4s1`), never the raw one
  (`/dev/rdisk4s1`).** libexfat reads a FAT entry as a 4-byte `pread()` at an
  arbitrary offset. macOS raw character devices require block-aligned I/O in
  whole sectors, so those reads fail with `EINVAL` - and
  `exfat_next_cluster()` turns a failed read into `EXFAT_CLUSTER_BAD`, which
  surfaces as `ERROR: bad cluster 0xfffffff7`. That looks exactly like
  corruption and is not. Apple's `fsck_exfat` does its own aligned I/O and
  reads the same card happily, which is how the two come to disagree.
- **Name the partition, not the device.** Offset 0 of `disk4` is the MBR;
  the exFAT boot sector is at offset 0 of `disk4s1`.

**`exfatfsck` has a blind spot:** it checks that every cluster a file
references is marked allocated in the bitmap, but not the reverse. Clusters
marked allocated that nothing references - leaked by a buggy truncate or
delete - pass it silently. Track the reported "Used space" across runs, and
cross-check with Disk Utility First Aid, whose `fsck_exfat` validates the
bitmap in both directions.

The `VolumeDirty` check reads bit 1 of the 16-bit `VolumeFlags` at boot
sector offset 106 (spec section 3.1.13.2). That is what tells you whether the
handler was shut down cleanly with `exfatctl EXF0: die`. Verified against
synthetic images: a clean volume reads 0, a dirtied one reads 2.

## What this does NOT cover

The host is **little-endian**. This harness therefore does not exercise:

- the byte swapping — the single largest risk on a 68k target;
- the Exec device layer, `de_MaxTransfer` chunking or the `de_Mask` bounce
  path in `../amiga/dev_io.c`;
- the unaligned read-modify-write in `partition_io()`, which is the riskiest
  untested code in the write path;
- any AmigaDOS packet semantics.

It is a **filter, not a substitute**. Hardware testing still has to follow,
and the ladder ends with: write on the Amiga → `exfatfsck` the card on the
host → mount it and compare. That last step is what catches an endianness
error in the write path.
