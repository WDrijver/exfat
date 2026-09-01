/*
	format.c

	ACTION_FORMAT support: drives the mkfs engine in ../mkfs/ to write a
	fresh exFAT structure onto the partition.

	../mkfs/main.c is the command line front end and is not part of this
	build; the get_*() accessors it would normally provide are implemented
	here instead, and mkfs() reads its parameters through them.

	The sizing heuristics deliberately mirror mkfs/main.c's setup_spc_bits()
	so a volume formatted on the Amiga is laid out exactly as mkexfatfs
	would lay it out on a host.

	Copyright (C) 2026  Willem Drijver
	Based on the free exFAT implementation, Copyright (C) 2011-2023 Andrew Nayenko.

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 2 of the License, or
	(at your option) any later version.
*/

#include "exfat_amiga.h"

#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>

#include "ApolloCrossDev_Debug.h"
#include "mkexfat.h"
#include "vbr.h"
#include "fat.h"
#include "cbm.h"
#include "uct.h"
#include "rootdir.h"

/* The on-disk layout order, exactly as mkfs/main.c defines it: two boot
   regions, the FAT, then inside the cluster heap the allocation bitmap, the
   up-case table and the root directory. */
/* Pinned to CODE: a const object carrying relocations would otherwise be
   placed in DATA, and the ROM module must be a single hunk so that the
   linker's __etext is genuinely its end - see entry.S. */
const struct fs_object* const objects[] __attribute__((section(".text"))) =
{
	&vbr,
	&vbr,
	&fat,
	/* clusters heap */
	&cbm,
	&uct,
	&rootdir,
	NULL,
};

/* mkfs reads its parameters through the get_*() calls below, which take no
   context - so upstream keeps them in a static.  A ROM-resident handler may
   hold no writable statics, so the block lives on exfat_amiga_format()'s
   stack instead and is reached through this process's tc_UserData for the
   duration of the call.  That is per-process storage, so two partitions can
   be formatted at once without interfering. */
struct MkfsParam
{
	int		sector_bits;
	int		spc_bits;
	exfat_off_t	volume_size;
	le16_t		volume_label[EXFAT_ENAME_MAX + 1];
	uint32_t	volume_serial;
	uint64_t	first_sector;
};

static struct MkfsParam* param_of(void)
{
	struct ExecBase* SysBase = *(struct ExecBase**)4UL;

	return (struct MkfsParam*)SysBase->ThisTask->tc_UserData;
}

#define param (*param_of())

/* ------------------------------------------------------------------ */
/* the parameter interface mkfs() reads through                        */
/* ------------------------------------------------------------------ */

int get_sector_bits(void)		{ return param.sector_bits; }
int get_spc_bits(void)			{ return param.spc_bits; }
exfat_off_t get_volume_size(void)	{ return param.volume_size; }
const le16_t* get_volume_label(void)	{ return param.volume_label; }
uint32_t get_volume_serial(void)	{ return param.volume_serial; }
uint64_t get_first_sector(void)		{ return param.first_sector; }
int get_sector_size(void)		{ return 1 << get_sector_bits(); }
int get_cluster_size(void)		{ return get_sector_size() << get_spc_bits(); }

/* ------------------------------------------------------------------ */

static int logarithm2(uint32_t n)
{
	EXFAT_SYSBASE;
	int i;

	for (i = 0; i < 31; i++)
		if ((1UL << i) == n)
			return i;
	return -1;
}

/* Same rule as mkfs/main.c: 4 KB clusters below 256 MB, 32 KB below 32 GB,
   then the smallest power of two that keeps the cluster count within what a
   FAT can address. */
static int choose_spc_bits(int sector_bits, exfat_off_t volume_size)
{
	EXFAT_SYSBASE;
	int i;

	if (volume_size < 256LL * 1024 * 1024)
		return MAX(0, 12 - sector_bits);
	if (volume_size < 32LL * 1024 * 1024 * 1024)
		return MAX(0, 15 - sector_bits);

	for (i = 17; i < 32; i++)
		if (DIV_ROUND_UP(volume_size, 1LL << i) <= EXFAT_LAST_DATA_CLUSTER)
			return MAX(0, i - sector_bits);
	return -1;
}

/* A serial number should be unique per volume; the system clock is what
   mkfs/main.c uses (via gettimeofday) and is all we have here too. */
static uint32_t make_serial(void)
{
	EXFAT_SYSBASE;
	struct DosLibrary* DOSBase;
	struct DateStamp ds;
	uint32_t serial;

	/* Open dos.library rather than rely on a global: a ROM-resident handler
	   may hold no writable statics, and an undefined DOSBase here resolves
	   against libc, dragging in newlib's stdio and malloc. */
	DOSBase = (struct DosLibrary*)OpenLibrary("dos.library", 37);
	if (DOSBase == NULL)
		return 1;
	DateStamp(&ds);
	CloseLibrary((struct Library*)DOSBase);
	serial = ((uint32_t)ds.ds_Days << 20) ^ ((uint32_t)ds.ds_Minute << 10) ^
			(uint32_t)ds.ds_Tick;
	return (serial != 0) ? serial : 1;	/* 0 would look uninitialised */
}

/* ------------------------------------------------------------------ */

/* Write a fresh exFAT structure onto the partition described by `spec'.
   The caller must have unmounted the volume first - ACTION_FORMAT is only
   legal while the file system is inhibited (section 13.7.5). */
int exfat_amiga_format(const struct ExfatDevSpec* spec, const char* label_utf8)
{
	EXFAT_SYSBASE;
	struct exfat_dev* dev;
	int rc;

	struct MkfsParam block;
	APTR saved;

	memset(&block, 0, sizeof(block));
	saved = SysBase->ThisTask->tc_UserData;
	SysBase->ThisTask->tc_UserData = &block;

	param.sector_bits = logarithm2(spec->blocksize);
	if (param.sector_bits < 9 || param.sector_bits > 12)
	{
		Debug_Error("block size %lu is not a valid exFAT sector size "
				"(512..4096)\n", (ULONG)spec->blocksize);
		SysBase->ThisTask->tc_UserData = saved;
		return -1;
	}
	param.first_sector = spec->firstbyte / (uint64_t)spec->blocksize;

	dev = exfat_open((const char*)spec, EXFAT_MODE_RW);
	if (dev == NULL)
	{
		Debug_Error("cannot open the device for formatting\n");
		SysBase->ThisTask->tc_UserData = saved;
		return -1;
	}

	param.volume_size = exfat_get_size(dev);
	param.spc_bits = choose_spc_bits(param.sector_bits, param.volume_size);
	if (param.spc_bits < 0)
	{
		Debug_Error("cannot choose a cluster size for this volume\n");
		exfat_close(dev);
		SysBase->ThisTask->tc_UserData = saved;
		return -1;
	}

	if (label_utf8 != NULL && label_utf8[0] != '\0')
	{
		if (exfat_utf8_to_utf16(param.volume_label, label_utf8,
				EXFAT_ENAME_MAX + 1, strlen(label_utf8)) != 0)
		{
			Debug_Warn("volume label does not fit, formatting unlabelled\n");
			memset(param.volume_label, 0, sizeof(param.volume_label));
		}
	}
	param.volume_serial = make_serial();

	Debug_Info("formatting: %lu byte sectors, %lu byte clusters, "
			"first sector %lu, serial %08lx\n",
			(ULONG)get_sector_size(), (ULONG)get_cluster_size(),
			(ULONG)param.first_sector, (ULONG)param.volume_serial);

	rc = mkfs(dev, param.volume_size);

	exfat_close(dev);
	SysBase->ThisTask->tc_UserData = saved;

	if (rc != 0)
	{
		Debug_Error("mkfs failed (%ld)\n", (LONG)rc);
		return -1;
	}
	Debug_Info("format complete\n");
	return 0;
}
