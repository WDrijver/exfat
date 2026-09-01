#!/bin/sh
#
# Verify an exFAT card written by the Amiga handler.
#
#   sudo ./checkcard.sh disk4s1
#
# Unmounts the volume, runs our exfatfsck on it, and reports the state of the
# exFAT VolumeDirty flag - which is what tells you whether the handler was
# shut down cleanly with "exfatctl EXF0: die".
#
# IMPORTANT: this uses the BUFFERED device (/dev/diskNsM), never the raw one
# (/dev/rdiskNsM).  libexfat reads a FAT entry as a 4-byte pread at an
# arbitrary offset; the raw character device on macOS requires block-aligned
# I/O in whole sectors, so those reads fail with EINVAL.  libexfat then
# reports the failure as "bad cluster 0xfffffff7", which looks exactly like
# corruption and is not.  Apple's fsck_exfat does its own aligned I/O and is
# happy with the raw device, which makes the two disagree.
#
# Must name a PARTITION (disk4s1), not the whole device (disk4): the exFAT
# boot sector is at offset 0 of the partition, while offset 0 of the device
# is the MBR.
#

set -u

FSCK=./build/exfatfsck

if [ $# -ne 1 ]; then
	echo "usage: sudo ./checkcard.sh <partition>     e.g. disk4s1"
	echo
	echo "list candidates with:  diskutil list"
	exit 2
fi

PART=$1
case "$PART" in
	/dev/r*) echo "use the buffered device, not the raw one: ${PART#/dev/r}"; exit 2 ;;
	/dev/*)  PART=${PART#/dev/} ;;
esac
# "disk4" contains an s, so test the part AFTER the disk prefix
REST=${PART#disk}
case "$REST" in
	*s*) ;;
	*)   echo "'$PART' looks like a whole device; name the partition, e.g. ${PART}s1"
	     echo "(offset 0 of a device is the MBR; the exFAT boot sector is at"
	     echo " offset 0 of the partition)"
	     exit 2 ;;
esac

DEV=/dev/$PART

if [ ! -x "$FSCK" ]; then
	echo "build first: make"
	exit 2
fi
if [ ! -e "$DEV" ]; then
	echo "$DEV does not exist"
	exit 2
fi

echo "== unmounting $DEV =="
diskutil unmount "$DEV" >/dev/null 2>&1 || true

echo
echo "== VolumeDirty flag =="
# VolumeFlags is a 16-bit little-endian field at offset 106 of the boot
# sector; bit 1 is VolumeDirty (spec section 3.1.13.2).
FLAGS=$(dd if="$DEV" bs=512 count=1 2>/dev/null | od -An -tu1 -j106 -N1 | tr -d ' ')
if [ -z "$FLAGS" ]; then
	echo "could not read the boot sector (try sudo)"
	exit 2
fi
if [ $((FLAGS & 2)) -ne 0 ]; then
	echo "VolumeFlags=$FLAGS  DIRTY - the handler was not shut down cleanly"
	echo "  (expected after a reset without 'exfatctl EXF0: die')"
	dirty=1
else
	echo "VolumeFlags=$FLAGS  clean"
	dirty=0
fi

echo
echo "== exfatfsck =="
"$FSCK" "$DEV"
rc=$?

echo
if [ $rc -eq 0 ] && [ $dirty -eq 0 ]; then
	echo "RESULT: clean and cleanly unmounted"
elif [ $rc -eq 0 ]; then
	echo "RESULT: structurally clean, but the volume is flagged dirty"
else
	echo "RESULT: exfatfsck reported problems (exit $rc)"
fi

echo
echo "NOTE: exfatfsck checks that every cluster a file references is marked"
echo "allocated, but NOT the converse.  Clusters marked allocated that no"
echo "file references - leaked clusters - pass this check silently.  Watch"
echo "the \"Used space\" figure across runs, and use Disk Utility's First Aid"
echo "(Apple's fsck_exfat does check the bitmap both ways) when it drifts."
echo
echo "re-mount with: diskutil mount $DEV"
exit $rc
