/*
	mbrscan.c

	Find exFAT volumes on a block device and print the LowCyl/HighCyl values
	needed to mount them.

	This is a CLI tool, deliberately kept out of the file system: locating
	partitions is the job of the device driver and its mounter, never of the
	handler (see ../CLAUDE.md, settled decision 1).  The handler is given a
	partition and never looks at an MBR, a GPT or an RDB.  This program is
	how you find out what to hand it, and it is a sketch of the enumeration
	sagasd.device will eventually do for itself.

	Usage:  mbrscan [device] [unit]        default: sagasd.device 0

	Copyright (C) 2026  Willem Drijver

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 2 of the License, or
	(at your option) any later version.
*/

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/io.h>
#include <devices/trackdisk.h>
#include <devices/newstyle.h>
#include <proto/exec.h>
#include <proto/dos.h>

/* devices/timer.h (via proto/dos.h) already defined struct timeval; stop
   newlib's <sys/_timeval.h>, reached from <stdio.h>, defining it again. */
#ifndef _TIMEVAL_DEFINED
#define _TIMEVAL_DEFINED
#endif

#include <dos/rdargs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef TD_READ64
#define TD_READ64	24
#endif

#define SECSIZE		512

static struct MsgPort*	port;
static struct IOExtTD*	req;
static BOOL		devopen;
static UBYTE*		buf;
static UWORD		readcmd = CMD_READ;

/* MBR and GPT fields are little-endian; this CPU is not. */
static ULONG le32(const UBYTE* p)
{
	return (ULONG)p[0] | ((ULONG)p[1] << 8) | ((ULONG)p[2] << 16) |
			((ULONG)p[3] << 24);
}

static UWORD le16(const UBYTE* p)
{
	return (UWORD)(p[0] | (p[1] << 8));
}

static unsigned long long le64(const UBYTE* p)
{
	return (unsigned long long)le32(p) |
			((unsigned long long)le32(p + 4) << 32);
}

static BOOL readblock(unsigned long long lba, UBYTE* dest)
{
	unsigned long long off = lba * (unsigned long long)SECSIZE;

	req->iotd_Req.io_Command = readcmd;
	req->iotd_Req.io_Data = dest;
	req->iotd_Req.io_Length = SECSIZE;
	req->iotd_Req.io_Offset = (ULONG)(off & 0xFFFFFFFFULL);
	req->iotd_Req.io_Actual = (ULONG)(off >> 32);	/* io_HighOffset */

	if (DoIO((struct IORequest*)req) != 0)
		return FALSE;
	return req->iotd_Req.io_Actual == SECSIZE;
}

/* Report a candidate: is there an exFAT VBR at this LBA? */
static void check_candidate(const char* what, unsigned long long start,
		unsigned long long count)
{
	UBYTE* vbr = AllocVec(SECSIZE, MEMF_PUBLIC | MEMF_CLEAR);

	if (vbr == NULL)
		return;

	printf("  %-18s start LBA %-12llu sectors %-12llu ", what, start, count);

	if (!readblock(start, vbr))
		printf("[read failed]\n");
	else if (memcmp(vbr + 3, "EXFAT   ", 8) == 0)
	{
		printf("[exFAT]\n");
		printf("        -> LowCyl = %llu\n", start);
		printf("        -> HighCyl = %llu\n",
				(count > 0) ? start + count - 1 : start);
	}
	else if (memcmp(vbr + 3, "NTFS    ", 8) == 0)
		printf("[NTFS]\n");
	else if (le16(vbr + 510) == 0xAA55)
		printf("[boot sector, not exFAT]\n");
	else
		printf("[no signature]\n");

	FreeVec(vbr);
}

static void scan_gpt(void)
{
	UBYTE* hdr = AllocVec(SECSIZE, MEMF_PUBLIC | MEMF_CLEAR);
	UBYTE* ent = AllocVec(SECSIZE, MEMF_PUBLIC | MEMF_CLEAR);
	unsigned long long entlba;
	ULONG nents, entsize, i;

	if (hdr == NULL || ent == NULL)
		goto out;

	if (!readblock(1, hdr) || memcmp(hdr, "EFI PART", 8) != 0)
	{
		printf("  (protective MBR but no GPT header at LBA 1)\n");
		goto out;
	}

	entlba = le64(hdr + 72);
	nents = le32(hdr + 80);
	entsize = le32(hdr + 84);
	printf("GPT: %lu entries of %lu bytes at LBA %llu\n",
			(unsigned long)nents, (unsigned long)entsize, entlba);

	if (entsize == 0 || entsize > SECSIZE)
		goto out;

	for (i = 0; i < nents && i < 128; i++)
	{
		unsigned long long lba = entlba + (i * entsize) / SECSIZE;
		ULONG off = (i * entsize) % SECSIZE;
		unsigned long long first, last;
		char label[24];

		if (!readblock(lba, ent))
			break;
		/* an all-zero type GUID means the entry is unused */
		if (le32(ent + off) == 0 && le32(ent + off + 4) == 0 &&
				le32(ent + off + 8) == 0 && le32(ent + off + 12) == 0)
			continue;

		first = le64(ent + off + 32);
		last = le64(ent + off + 40);
		sprintf(label, "GPT entry %lu", (unsigned long)i);
		check_candidate(label, first, (last >= first) ? last - first + 1 : 0);
	}
out:
	if (hdr != NULL)
		FreeVec(hdr);
	if (ent != NULL)
		FreeVec(ent);
}

/* ReadArgs() rather than argc/argv: it takes the Shell's argument line from
   pr_CIS and so does not depend on the linked startup code. */
#define TEMPLATE "DEVICE,UNIT/N"

enum { ARG_DEVICE, ARG_UNIT, ARG_COUNT };

int main(void)
{
	struct RDArgs* rda;
	LONG args[ARG_COUNT];
	const char* devname = "sagasd.device";
	ULONG unit = 0;
	int i;
	int found_mbr = 0;

	memset(args, 0, sizeof(args));
	rda = ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL);
	if (rda == NULL)
	{
		PrintFault(IoErr(), (CONST_STRPTR)"mbrscan");
		printf("usage: mbrscan [device] [unit]   "
				"(default: sagasd.device 0)\n");
		return RETURN_ERROR;
	}
	if (args[ARG_DEVICE] != 0)
		devname = (const char*)args[ARG_DEVICE];
	if (args[ARG_UNIT] != 0)
		unit = (ULONG)*(LONG*)args[ARG_UNIT];
	FreeArgs(rda);

	port = CreateMsgPort();
	if (port == NULL)
	{
		printf("cannot create message port\n");
		return RETURN_FAIL;
	}
	req = (struct IOExtTD*)CreateIORequest(port, sizeof(struct IOExtTD));
	if (req == NULL)
	{
		printf("cannot create io request\n");
		DeleteMsgPort(port);
		return RETURN_FAIL;
	}
	if (OpenDevice((CONST_STRPTR)devname, unit, (struct IORequest*)req, 0) != 0)
	{
		printf("cannot open %s unit %lu\n", devname, (unsigned long)unit);
		DeleteIORequest((struct IORequest*)req);
		DeleteMsgPort(port);
		return RETURN_FAIL;
	}
	devopen = TRUE;

	/* Use 64-bit reads if the driver offers them, so partitions past the
	   4 GB mark can be probed too. */
	{
		struct NSDeviceQueryResult nsd;
		UWORD* cmds;

		memset(&nsd, 0, sizeof(nsd));
		req->iotd_Req.io_Command = NSCMD_DEVICEQUERY;
		req->iotd_Req.io_Data = &nsd;
		req->iotd_Req.io_Length = sizeof(nsd);
		req->iotd_Req.io_Actual = 0;
		if (DoIO((struct IORequest*)req) == 0 &&
				nsd.nsdqr_SupportedCommands != NULL)
		{
			for (cmds = (UWORD*)nsd.nsdqr_SupportedCommands; *cmds; cmds++)
				if (*cmds == NSCMD_TD_READ64)
					readcmd = NSCMD_TD_READ64;
		}
		if (readcmd == CMD_READ)
			readcmd = TD_READ64;	/* assume TD64, as the FFS does */
	}
	printf("%s unit %lu, read command %u\n\n", devname,
			(unsigned long)unit, (unsigned)readcmd);

	buf = AllocVec(SECSIZE, MEMF_PUBLIC | MEMF_CLEAR);
	if (buf == NULL)
		goto out;

	if (!readblock(0, buf))
	{
		printf("cannot read LBA 0\n");
		goto out;
	}

	/* A card formatted as a "superfloppy" has the exFAT VBR at LBA 0 and
	   no partition table at all. */
	if (memcmp(buf + 3, "EXFAT   ", 8) == 0)
	{
		printf("No partition table: exFAT volume starts at LBA 0.\n");
		printf("  -> LowCyl = 0\n");
		printf("  -> HighCyl = <total sectors - 1>\n");
		goto out;
	}

	if (le16(buf + 510) != 0xAA55)
	{
		printf("LBA 0 has no 0xAA55 signature: no MBR, and no exFAT here.\n");
		goto out;
	}

	printf("MBR partition table:\n");
	for (i = 0; i < 4; i++)
	{
		const UBYTE* e = buf + 446 + i * 16;
		UBYTE type = e[4];
		unsigned long long start = le32(e + 8);
		unsigned long long count = le32(e + 12);
		char label[24];

		if (type == 0 || count == 0)
			continue;
		found_mbr++;

		if (type == 0xEE)
		{
			printf("  entry %d: protective MBR (GPT follows)\n", i);
			scan_gpt();
			continue;
		}
		sprintf(label, "entry %d type %02x", i, (unsigned)type);
		check_candidate(label, start, count);
	}
	if (found_mbr == 0)
		printf("  (all four entries empty)\n");

	printf("\nPut the LowCyl/HighCyl of the exFAT line into DEVS:DOSDrivers/EXF0.\n");

out:
	if (buf != NULL)
		FreeVec(buf);
	if (devopen)
		CloseDevice((struct IORequest*)req);
	DeleteIORequest((struct IORequest*)req);
	DeleteMsgPort(port);
	return RETURN_OK;
}
