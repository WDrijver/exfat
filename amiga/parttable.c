/*
	parttable.c

	MBR and GPT partition table scanning.

	This is here because the handler may be mounted on a whole device rather
	than on a partition - the same arrangement sagasd.device uses for fat95,
	where SDROM0: covers the entire card and the file system finds the
	partitions itself.  See ../CLAUDE.md, "Partition discovery".

	All the on-disk structures below are little-endian; this CPU is not, so
	every field is assembled byte by byte rather than overlaid with a struct.

	Copyright (C) 2026  Willem Drijver

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 2 of the License, or
	(at your option) any later version.
*/

#include "exfat_amiga.h"

#include <exec/memory.h>
#include <proto/exec.h>
#include <string.h>

#include "ApolloCrossDev_Debug.h"

/* Offsets into a classic MBR (LBA 0). */
#define MBR_TABLE	446
#define MBR_ENTRY_SIZE	16
#define MBR_ENTRIES	4
#define MBR_SIG		510

#define MBR_TYPE_EXFAT	0x07	/* also NTFS; the VBR check disambiguates */
#define MBR_TYPE_GPT	0xEE	/* protective MBR */

/* Offsets into a GPT header (LBA 1). */
#define GPT_SIG		0
#define GPT_ENT_LBA	72
#define GPT_ENT_COUNT	80
#define GPT_ENT_SIZE	84

static ULONG le32at(const UBYTE* p)
{
	return (ULONG)p[0] | ((ULONG)p[1] << 8) | ((ULONG)p[2] << 16) |
			((ULONG)p[3] << 24);
}

static UWORD le16at(const UBYTE* p)
{
	return (UWORD)(p[0] | (p[1] << 8));
}

static uint64_t le64at(const UBYTE* p)
{
	return (uint64_t)le32at(p) | ((uint64_t)le32at(p + 4) << 32);
}

/* Is there an exFAT boot sector at this LBA?  A partition type byte only
   says what a partition is meant to hold, so check what is actually there. */
static BOOL is_exfat_at(struct exfat_dev* dev, uint64_t lba, ULONG blocksize,
		UBYTE* buf)
{
	EXFAT_SYSBASE;
	if (exfat_pread(dev, buf, blocksize, (exfat_off_t)(lba * blocksize)) < 0)
		return FALSE;
	return memcmp(buf + 3, "EXFAT   ", 8) == 0;
}

static void add(struct ExfatPartition* out, int max, int* n,
		uint64_t first, uint64_t count, const char* how)
{
	EXFAT_SYSBASE;
	if (*n >= max)
		return;
	out[*n].first_lba = first;
	out[*n].sectors = count;
	(*n)++;
	Debug_Info("  exFAT partition %ld: %s, LBA %lu, %lu sectors\n",
			(LONG)(*n - 1), how, (ULONG)first, (ULONG)count);
}

static int scan_gpt(struct exfat_dev* dev, ULONG blocksize, UBYTE* buf,
		struct ExfatPartition* out, int max)
{
	EXFAT_SYSBASE;
	UBYTE* hdr = AllocVec(blocksize, MEMF_PUBLIC | MEMF_CLEAR);
	int n = 0;
	uint64_t ent_lba;
	ULONG ent_count, ent_size, i;

	if (hdr == NULL)
		return 0;

	if (exfat_pread(dev, hdr, blocksize, (exfat_off_t)blocksize) < 0 ||
			memcmp(hdr + GPT_SIG, "EFI PART", 8) != 0)
	{
		Debug_Warn("protective MBR but no GPT header at LBA 1\n");
		FreeVec(hdr);
		return 0;
	}

	ent_lba = le64at(hdr + GPT_ENT_LBA);
	ent_count = le32at(hdr + GPT_ENT_COUNT);
	ent_size = le32at(hdr + GPT_ENT_SIZE);
	Debug_Info("GPT: %lu entries of %lu bytes at LBA %lu\n",
			ent_count, ent_size, (ULONG)ent_lba);

	if (ent_size == 0 || ent_size > blocksize || ent_count > 128)
		ent_count = (ent_count > 128) ? 128 : ent_count;

	for (i = 0; i < ent_count && n < max; i++)
	{
		uint64_t lba = ent_lba + (uint64_t)(i * ent_size) / blocksize;
		ULONG off = (i * ent_size) % blocksize;
		uint64_t first, last;

		if (exfat_pread(dev, hdr, blocksize,
				(exfat_off_t)(lba * blocksize)) < 0)
			break;
		/* an all-zero type GUID marks the entry unused */
		if (le32at(hdr + off) == 0 && le32at(hdr + off + 4) == 0 &&
				le32at(hdr + off + 8) == 0 && le32at(hdr + off + 12) == 0)
			continue;

		first = le64at(hdr + off + 32);
		last = le64at(hdr + off + 40);
		if (last < first)
			continue;
		if (is_exfat_at(dev, first, blocksize, buf))
			add(out, max, &n, first, last - first + 1, "GPT entry");
	}

	FreeVec(hdr);
	return n;
}

/* Find every exFAT partition on the device the handler was given.  Returns
   the number found, 0 if there is no partition table we understand.  A
   superfloppy (exFAT at LBA 0, no table) reports one partition covering the
   whole device. */
int exfat_amiga_scan_partitions(struct exfat_dev* dev, ULONG blocksize,
		uint64_t total_sectors, struct ExfatPartition* out, int max)
{
	EXFAT_SYSBASE;
	UBYTE* buf = AllocVec(blocksize, MEMF_PUBLIC | MEMF_CLEAR);
	int n = 0;
	int i;

	if (buf == NULL)
		return 0;

	if (exfat_pread(dev, buf, blocksize, 0) < 0)
	{
		Debug_Error("cannot read LBA 0\n");
		FreeVec(buf);
		return 0;
	}

	/* No partition table at all: the volume starts at sector 0. */
	if (memcmp(buf + 3, "EXFAT   ", 8) == 0)
	{
		add(out, max, &n, 0, total_sectors, "superfloppy");
		FreeVec(buf);
		return n;
	}

	if (le16at(buf + MBR_SIG) != 0xAA55)
	{
		Debug_Warn("no partition table and no exFAT volume at LBA 0\n");
		FreeVec(buf);
		return 0;
	}

	for (i = 0; i < MBR_ENTRIES && n < max; i++)
	{
		const UBYTE* e = buf + MBR_TABLE + i * MBR_ENTRY_SIZE;
		UBYTE type = e[4];
		uint64_t first = le32at(e + 8);
		uint64_t count = le32at(e + 12);

		if (type == 0 || count == 0)
			continue;
		if (type == MBR_TYPE_GPT)
		{
			n += scan_gpt(dev, blocksize, buf, out + n, max - n);
			continue;
		}
		/* Trust the boot sector rather than the type byte: 0x07 covers both
		   exFAT and NTFS, and some formatters use other values. */
		if (is_exfat_at(dev, first, blocksize, buf))
			add(out, max, &n, first, count, "MBR entry");
		/* is_exfat_at() reused buf, so restore the MBR for the next entry */
		if (i + 1 < MBR_ENTRIES && exfat_pread(dev, buf, blocksize, 0) < 0)
			break;
	}

	if (n == 0)
		Debug_Warn("partition table found, but no exFAT partitions in it\n");
	FreeVec(buf);
	return n;
}
