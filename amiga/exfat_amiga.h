/*
	exfat_amiga.h

	AmigaOS 3.2 port of the free exFAT implementation: shared declarations
	for the handler process and the Exec block-device I/O layer.

	Copyright (C) 2026  Willem Drijver
	Based on the free exFAT implementation, Copyright (C) 2010-2023 Andrew Nayenko.

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 2 of the License, or
	(at your option) any later version.
*/

#ifndef EXFAT_AMIGA_H_INCLUDED
#define EXFAT_AMIGA_H_INCLUDED

/* Include the AmigaOS headers first: dos/dosextens.h pulls in
   devices/timer.h, which defines struct timeval unconditionally.  newlib's
   <sys/_timeval.h> - reached from <stdio.h> inside libexfat/exfat.h - would
   then define it a second time, so claim its guard macro in between. */
#include <exec/types.h>
#include <exec/ports.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/filehandler.h>

#ifndef _TIMEVAL_DEFINED
#define _TIMEVAL_DEFINED
#endif

#include <stdint.h>

#include "../libexfat/exfat.h"

/* ------------------------------------------------------------------ */
/* Write support is unconditional.                                    */
/*                                                                    */
/* It was behind EXFAT_AMIGA_ALLOW_WRITE while the byte swapping and  */
/* the device layer were being proven against real media.  They were: */
/* an exFAT card mounts read-write from Kickstart and served Brian    */
/* the Lion's 551 MB preload under load, 2026-09-11.  The switch is   */
/* gone and the read-only branches with it.                           */
/* ------------------------------------------------------------------ */

/* Mount every partition found at startup so the volumes appear on Workbench
   straight away.  Build with ACTIVATE=0 to leave them to start on first
   access instead, which is the quieter behaviour if this causes trouble. */
#ifndef EXFAT_AMIGA_ACTIVATE
#define EXFAT_AMIGA_ACTIVATE 1
#endif

/* ------------------------------------------------------------------ */
/* Device layer (dev_io.c)                                            */
/* ------------------------------------------------------------------ */

/* Where the volume lives.  Filled from the FileSysStartupMsg and its
   DosEnvec by the handler before exfat_mount() is called.  The handler
   serves exactly one volume, so a single context is correct here.

   NOTE: the partition extent comes from the DosEnvec and nowhere else.
   The file system never looks at an MBR, GPT or RDB, and it ignores the
   PartitionOffset field in the exFAT boot sector.  See ../CLAUDE.md. */
/* The DOSType AmigaOS uses for exFAT.  This is de_DosType / fse_DosType
   only - it says which file system owns the partition.  It is NOT what goes
   into id_DiskType or dl_DiskType; those must be ID_DOS_DISK (section 5.2.4,
   table 5.6) or Info reports an unreadable disk. */
#define EXFAT_DOSTYPE	0x46415458UL	/* 'FATX' */

/* fse_PatchFlags bits, which NDK 3.2 does not define.  filesysres.doc gives
   the encoding: "$180 for substitute SegList & GlobalVec", and the fields
   after fse_PatchFlags are Type, Task, Lock, Handler, StackSize, Priority,
   Startup, SegList, GlobalVec - so StackSize is bit 4 and GlobalVec bit 8. */
#ifndef FSEF_SEGLIST
#define FSEF_TYPE	(1L << 0)
#define FSEF_TASK	(1L << 1)
#define FSEF_LOCK	(1L << 2)
#define FSEF_HANDLER	(1L << 3)
#define FSEF_STACKSIZE	(1L << 4)
#define FSEF_PRIORITY	(1L << 5)
#define FSEF_STARTUP	(1L << 6)
#define FSEF_SEGLIST	(1L << 7)
#define FSEF_GLOBALVEC	(1L << 8)
#endif

/* The stack the handler wants when AmigaDOS starts a process for it.
   Published through the FileSystem.resource entry so every mounter agrees;
   MakeDosNode()'s default is far too small for libexfat's VLAs plus path
   resolution.  Process priority is left at MakeDosNode()'s default of 10. */
#define EXFAT_STACKSIZE	65536

struct ExfatDevSpec
{
	const char*	devname;	/* e.g. "sagasd.device"          */
	ULONG		unit;
	ULONG		flags;		/* fssm_Flags for OpenDevice()   */
	ULONG		blocksize;	/* bytes; de_SizeBlock * 4       */
	ULONG		maxtransfer;	/* de_MaxTransfer                */
	ULONG		mask;		/* de_Mask                       */
	uint64_t		firstbyte;	/* partition start, in bytes     */
	uint64_t		length;		/* partition length, in bytes    */
};

/* One exFAT partition found on the medium (parttable.c). */
struct ExfatPartition
{
	uint64_t	first_lba;
	uint64_t	sectors;
};

#define EXFAT_MAX_PARTITIONS 8

int exfat_amiga_scan_partitions(struct exfat_dev* dev, ULONG blocksize,
		uint64_t total_sectors, struct ExfatPartition* out, int max);

/* On AmigaOS the `spec' argument of exfat_mount()/exfat_open() is not a
   path but a pointer to one of these.  Passing it down the call chain rather
   than through a global is what lets several handler processes share one
   seglist - which they do when the handler is resident in
   FileSystem.resource.  Cast at the call site:

       exfat_mount(&h->ef, (const char*)&h->spec, options);
*/

/* A ROM-resident handler may hold NO writable static data - ROM cannot be
   written, and the ROM build tools reject a BSS section outright.  That
   rules out the usual global SysBase/DOSBase, so every function declares
   them locally instead: SysBase from the fixed location 4, DOSBase from the
   per-process handler state.  Put EXFAT_BASES(h) at the top of any function
   that calls exec or dos. */
#define EXFAT_SYSBASE \
	struct ExecBase* SysBase UNUSED = *(struct ExecBase**)4UL

#define EXFAT_BASES(h) \
	struct ExecBase* SysBase UNUSED = *(struct ExecBase**)4UL; \
	struct DosLibrary* DOSBase UNUSED = (h)->dosbase

/* ------------------------------------------------------------------ */
/* Handler state (main.c)                                             */
/* ------------------------------------------------------------------ */

struct ExfatLock;

struct ExfatHandler
{
	struct Process*		proc;
	struct DosLibrary*	dosbase;
	struct MsgPort*		port;
	struct DosList*		devlist;	/* our DLT_DEVICE entry     */
	struct DeviceList*	volume;		/* our DLT_VOLUME entry     */
	BSTR			volname;	/* our dl_Name, if relabelled */
	BSTR			volname_orig;	/* the one MakeDosEntry made  */
	struct ExfatDevSpec	spec;
	ULONG			lowcyl;		/* de_LowCyl we were given */
	struct exfat		ef;
	BOOL			mounted;
	BOOL			inhibited;
	struct SignalSemaphore*	claim;		/* our claim on this partition */
	BOOL			quit;
	ULONG			nlocks;
	ULONG			nfiles;
	/* names of the partitions we published, to activate after startup */
	char			published[EXFAT_MAX_PARTITIONS][34];
	int			npublished;
	int			next_name_idx;	/* next sibling name to try */
};

/* ------------------------------------------------------------------ */
/* Locks (locks.c)                                                    */
/* ------------------------------------------------------------------ */

/* A FileLock as AmigaDOS sees it, with our private state hung off the
   end.  dos.library only ever looks at the leading struct FileLock, so
   extending it like this is the normal AmigaOS idiom. */
struct ExfatLock
{
	struct FileLock		fl;		/* MUST be first            */
	struct ExfatHandler*	h;
	struct exfat_node*	node;		/* held (get/put) reference */
	/* Directory-scan state for ACTION_EXAMINE_NEXT.  `scan_index' is
	   the number of entries already returned; it is mirrored into
	   fib_DiskKey so a scan survives the lock being replaced. */
	struct exfat_node*	scan_next;
	LONG			scan_index;
};

struct ExfatLock* exfat_amiga_makelock(struct ExfatHandler* h,
		struct exfat_node* node, LONG access);
void exfat_amiga_freelock(struct ExfatHandler* h, struct ExfatLock* lock);
struct ExfatLock* exfat_amiga_lock_from_bptr(BPTR blk);

/* ------------------------------------------------------------------ */
/* Open files (files.c)                                               */
/* ------------------------------------------------------------------ */

struct ExfatFile
{
	struct ExfatHandler*	h;
	struct exfat_node*	node;
	exfat_off_t		pos;
	BOOL			writable;	/* opened for writing */
};

/* ------------------------------------------------------------------ */
/* Name and time conversion (amigaos.c)                               */
/* ------------------------------------------------------------------ */

/* exFAT stores names as UTF-16.  AmigaOS is 8-bit; milestone 1 maps the
   Latin-1 range directly and substitutes '?' above it.  A substituted
   name cannot be looked up again - see ../CLAUDE.md, open decision 3. */
void exfat_amiga_name_to_bstr(const char* utf8, UBYTE* bstr, int maxlen);
void exfat_amiga_bstr_to_utf8(const UBYTE* bstr, char* utf8, int maxlen);
void exfat_amiga_cstr_to_utf8(const char* src, char* utf8, int maxlen);

/* exFAT timestamps are local time; so is DateStamp.  Milestone 1 ignores
   the UTC-offset byte - see ../CLAUDE.md, open decision 4. */
int exfat_amiga_snprintf(char* buf, size_t size, const char* fmt, ...);
void exfat_amiga_time_to_datestamp(time_t t, struct DateStamp* ds);
time_t exfat_amiga_datestamp_to_time(const struct DateStamp* ds);

/* ------------------------------------------------------------------ */
/* Formatting (format.c)                                              */
/* ------------------------------------------------------------------ */

int exfat_amiga_format(const struct ExfatDevSpec* spec, const char* label_utf8);

#endif /* EXFAT_AMIGA_H_INCLUDED */
