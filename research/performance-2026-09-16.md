# exfat-handler: where the time goes

Static analysis of the 1.10 code paths, done 2026-09-16 without the
machine. The unit of cost on the V4 is a `DoIO()` to sagasd.device - a
message to the driver's IO task at priority 11 and back, on the order of
50-100 us when sagasd's cache answers, plus the card's ~2 MB/s when it does
not - so every path below is costed in device round trips first and in CPU
second. Nothing here was measured on hardware; the section at the end says
what to measure to confirm the ranking.

## 1. Mounting a volume

What one mount does, in order (`startup()` -> `resolve_extent()` ->
`mount_fs()` -> `exfat_mount()` -> `exfat_cache_directory(root)`):

| step | device round trips | notes |
|---|---|---|
| `resolve_extent()`: `exfat_open()` | 1 OpenDevice + 1 NSD query | probe for 64-bit commands |
| `exfat_amiga_scan_partitions()` | 1 (MBR) + 2-3 if GPT + 1 boot sector per candidate | reads through the one-block bounce |
| `exfat_close()` then `exfat_mount()` -> `exfat_open()` again | 1 OpenDevice + 1 NSD query | **the device is opened and probed twice** |
| boot sector | 1 | |
| VBR checksum | **12** -> **1** since 1.11 | twelve 512-byte reads of contiguous sectors |
| root directory | ~1 per 16 entries | root holds label, bitmap, upcase entries and the top-level files |
| upcase table | 1 (~5.8 KB) | |
| allocation bitmap | 1, but `cluster_count / 8` bytes | 32 GB card, 32 KB clusters: 128 KB, ~65 ms from the card |
| `exfat_soil_super_block()` | 1 write + 1 CMD_UPDATE | sets VolumeDirty |
| `add_volume()`, `activate_partitions()` | 0 | siblings: a full mount each, serially, via `Lock()` |

Roughly 25 round trips and one bulk read. At 50-100 us each that is a few
milliseconds; the bitmap read is the only item that scales with the card,
and it is a single request the driver serves at full speed. **A mount is
already in the tens-of-milliseconds range.** If mounting *feels* slow, the
time is elsewhere:

- sagasd's change poll runs every `CHANGEINT_INTERVAL` = 2 s, so an insert
  is noticed 0-2 s late; at boot the mount daemon waits a further 5 s
  (`SD_MOUNT_DELAY_SECS`). Both are driver-side choices, both deliberate
  (the poll interval was set by the audio-dropout measurement), and both
  dwarf the handler's own work.
- A multi-partition card mounts its siblings one after another, each a
  full mount, each started by a `Lock()` from the activation process.
- Workbench then reads `disk.info` and the root directory - through the
  handler, but on its own schedule.

What was done (1.11): the VBR checksum reads its twelve sectors in one
request (`verify_vbr_checksum()`, falls back to the loop if a cluster is
smaller than 6 KB, which no real volume has). Host harness: every scenario
mounts, all pass.

What is left, in order of value:

1. **Open the device once.** `resolve_extent()` opens, probes and closes;
   `exfat_mount()` opens and probes again. Handing the scan's `exfat_dev`
   to the mount saves an OpenDevice, an NSD query and the bounce refill -
   perhaps 0.5 ms. Cosmetic; only worth it if the rest of startup is being
   restructured anyway.
2. Nothing else in the mount path is worth touching until the poll
   interval and the boot delay are reconsidered, and those are sagasd's.

## 2. Dismounting

Under the inhibit policy a dismount is: `ACTION_INHIBIT` -> flush (nothing
dirty in the common case) -> `set_volume_dirty(FALSE)` (1 write, which
fails at once on a removed card - `sd_write()` checks `flags.present`
before touching the hardware) -> `remove_volume()` -> `release_mount()`
-> `exfat_unmount()` -> `finalize_super_block()`.

`finalize_super_block()` called `exfat_count_free_clusters()` to write the
allocated-percent byte - **one loop iteration per cluster, bit by bit**: a
million iterations on a 32 GB card, several million on 128 GB, tens of
milliseconds on the 080. Fixed in 1.11 (below). With that gone a dismount
is a handful of round trips, most of which fail fast because the card is
no longer there; nothing further to gain.

## 3. File and directory operations

### 3a. The free-cluster count - the one real CPU sink

`exfat_count_free_clusters()` ran `BMAP_GET()` over every cluster. It is
called from `fill_infodata()`, i.e. **every `ACTION_INFO` and
`ACTION_DISK_INFO`** - and Workbench sends one for every window it opens
or refreshes to draw the "xx% full, xxM free" title. On a 32 GB card with
32 KB clusters that was a million bit tests per window refresh; on a
128 GB card with 128 KB clusters the same; with smaller clusters
proportionally more.

1.11 counts whole bytes through a nibble table and only the bits of a
trailing partial byte one at a time - 32x fewer iterations, each cheaper.
The byte layout is the on-disk one on both byte orders (that is what the
`bitmap_t` choice in `byteorder.h` guarantees), so the same code is right
on the host and on the 68k. Proven by the new `freecount` scenario:
matches the bit loop on the live bitmap, ignores bits past `cmap.size`,
drops by exactly three after a three-cluster write, fsck clean.

A cached count invalidated on every mutating packet would remove the
scan entirely; at ~1 ms after 1.11 it no longer needs removing.

### 3b. Directory reads

libexfat reads a directory 32 bytes at a time (`readdir()` ->
`read_entries(..., 1, ...)`) and then re-reads each file's entry set to
parse it. Without help that is one round trip per entry. `dev_io.c`'s
one-block bounce (`fetch_block()`) already turns that into **one round
trip per 512-byte sector**, i.e. per 16 entries, per ~5 files. A directory
of 1000 files (3000+ entries, ~100 KB) is ~200 round trips: 10-20 ms
cached, ~50 ms from the card. Once read, a directory stays cached in
memory (`is_cached`) for the life of the mount, so `ExNext`, `Lock`,
`Examine` on it never touch the card again - `do_examine_next()` is O(1)
per call through the lock's scan cursor.

What is left:

3. **Two bounce blocks instead of one** (`dev_io.c`). A fragmented
   directory alternates between a directory sector and the FAT sector
   that says where the next cluster is (`exfat_next_cluster()` reads 4
   bytes); each switch evicts the other from the single bounce. Keeping
   the last data block and the last FAT block separately costs 512 bytes
   of RAM and removes the re-reads. Small, safe, Amiga-only, needs a
   hardware check that fragmented directories still list correctly.
4. **The first `Lock()` into a directory reads all of it.** libexfat
   caches a directory whole before looking anything up in it, so the
   first touch of a 10,000-file directory (~1 MB of entries) costs ~0.5 s
   from the card, once. Inherent to the library's design; not worth
   changing for the card sizes ApolloCD32 produces.

### 3c. File reads and writes

`exfat_generic_pread()` issues one device request per cluster or per
caller's chunk, whichever is smaller, and walks the FAT only for
fragmented files (contiguous ones - every freshly written file on a
non-fragmented card - skip it). A `Read()` of a 32 KB cluster is one round
trip; an application reading in 512-byte pieces costs a round trip per
piece, which sagasd's cache answers in ~50-100 us. Copying a 10 MB file in
4 KB reads is ~2,500 round trips, ~0.2 s of overhead on top of the card's
5 s - not where the time goes.

Writes: `exfat_generic_pwrite()` allocates clusters through the in-memory
bitmap (`find_bit_and_set()`) and writes FAT entries only when a file
turns non-contiguous - 4 bytes through the bounce, a read-modify-write of
one sector each, which the sagasd cache also absorbs. Directory entries
are flushed on close (`exfat_flush_node()`, ~2 round trips); the bitmap
at sync points only. There is no per-write `CMD_UPDATE`, and sagasd's
`CMD_UPDATE` is a no-op anyway (writes are never held back there).

What is left:

5. **Unaligned application buffers go through the bounce 512 bytes at a
   time** (`aligned_io()`, the `bounced` branch). sagasd's `de_Mask` is
   `0xFFFFFFFE`, so only an odd-address buffer takes this path, but one
   that does turns a 32 KB read into 64 round trips. A bounce of
   `de_MaxTransfer`-sized chunks (a 16 KB buffer, say) fixes it for the
   price of the buffer. Rare in practice; cheap to do.
6. **Read-ahead in the handler** (a cluster-sized buffer served to small
   `Read()`s) would cut round trips for applications that read in small
   pieces, but the card is the bottleneck at 2 MB/s and the cache-hit case
   is already ~50 MB/s. Not recommended without a measurement that shows
   a real title paying for it.

### 3d. CPU: the 64-bit divide

Every `exfat_generic_pread()`/`pwrite()` divides a 64-bit offset by the
cluster size twice. Since 1.10 that is our own `udivmod64()`; its 32-bit
fast path covered offsets under 4 GB and the rest fell into a 64-iteration
loop - on a 64 GB card, most of the card. 1.11 adds a power-of-two path
first: every divisor libexfat uses is one (sector size, cluster size,
entries per sector), so it is now a shift and a mask regardless of the
offset. Proven by 2,400 host checks against the native divide, both sides
of 4 GB, every power of two, and the general case.

## 4. What to measure on the machine

A `DEBUG=1` build with one timestamp per mount stage would settle the
ranking in an afternoon. The cheapest instrument: `ReadEClock()` at entry
to `startup()`, after `resolve_extent()`, after `exfat_mount()`, after
`add_volume()`, and at the end of `activate_partitions()`'s `Lock()`,
printed as deltas. Then the same around one `ACTION_INFO` and one
`ExNext` loop over a 1000-file directory, before and after 1.11. If the
mount stages sum to under 100 ms - which this analysis predicts - the
poll interval is the whole story for perceived mount time, and that
conversation belongs in sagasd.device.

## 5. Summary

| item | status | effect |
|---|---|---|
| free-cluster count byte-wise | done, 1.11, host-proven | every Info / window refresh, and dismount: ~30x fewer iterations |
| VBR checksum in one read | done, 1.11, host-proven | 11 round trips off every mount |
| power-of-two divide | done, 1.11, host-proven | every read/write on offsets past 4 GB |
| open the device once at startup | proposed | ~0.5 ms per mount |
| two bounce blocks (data + FAT) | proposed | fragmented directories |
| wide bounce for unaligned buffers | proposed | odd-address application buffers |
| handler read-ahead | not recommended | card-bound already |
| poll interval / boot delay | sagasd's, deliberate | dominates perceived mount time |
