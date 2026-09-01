/*
	exfatres.c

	Register the exFAT handler in FileSystem.resource from the Shell.

	This exists to take the ROM out of the debugging loop.  The ROM tag in
	romtag.c does exactly what this does - LoadSeg the handler and add a
	FileSysEntry for DOSType 'FATX' - but testing it costs a ROM rebuild and
	a reboot.  Run this instead and every mount afterwards takes the same
	code path a ROM-resident handler would: sagasd.device's mounter finds
	the entry, patches dn_SegList from it, and every partition on the card
	is served by processes sharing ONE segment list.

	The one thing it cannot reproduce is that ROM is not writable.  So:

	  works here, fails from ROM  -> the handler writes to its own code or
	                                 to static data.  Nothing should: the
	                                 module has no DATA and no BSS.
	  fails here too              -> the fault is in the FileSystem.resource
	                                 path or in sharing one seglist, and can
	                                 now be debugged from disk.

	  exfatres install            register L:exfat-handler
	  exfatres install L:my-build register a specific file
	  exfatres list               show what is registered

	There is deliberately no "remove": once a volume is mounted, processes
	are running from that segment list and freeing it would take the machine
	down.  Reboot instead.

	Copyright (C) 2026  Willem Drijver

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 2 of the License, or
	(at your option) any later version.
*/

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/nodes.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <resources/filesysres.h>
#include <proto/exec.h>
#include <proto/dos.h>

#ifndef _TIMEVAL_DEFINED
#define _TIMEVAL_DEFINED
#endif

#include <dos/rdargs.h>
#include <stdio.h>
#include <string.h>

#include "exfat_amiga.h"

/* See the note in exfatctl.c: ReadArgs, not argc/argv. */
#define TEMPLATE "COMMAND/A,FILE"

enum { ARG_COMMAND, ARG_FILE, ARG_COUNT };

#define DEFAULT_HANDLER "L:exfat-handler"

static void show(struct FileSysResource* fsr)
{
	struct FileSysEntry* e;
	int n = 0;

	printf("FileSystem.resource at %08lx, created by %s\n",
			(unsigned long)fsr, fsr->fsr_Creator ? fsr->fsr_Creator : "?");
	printf("  DosType    Version  PatchFlags  SegList   Stack  Creator\n");

	Forbid();
	for (e = (struct FileSysEntry*)fsr->fsr_FileSysEntries.lh_Head;
			e->fse_Node.ln_Succ != NULL;
			e = (struct FileSysEntry*)e->fse_Node.ln_Succ)
	{
		ULONG t = e->fse_DosType;

		printf("  %c%c%c%-2ld  %4ld.%-4ld  %08lx    %08lx  %5ld  %s\n",
				(char)(t >> 24), (char)(t >> 16), (char)(t >> 8),
				(long)(t & 0xff),
				(long)(e->fse_Version >> 16), (long)(e->fse_Version & 0xffff),
				(unsigned long)e->fse_PatchFlags,
				(unsigned long)e->fse_SegList,
				(long)e->fse_StackSize,
				e->fse_Node.ln_Name ? e->fse_Node.ln_Name : "?");
		n++;
	}
	Permit();
	printf("  %ld entr%s\n", (long)n, (n == 1) ? "y" : "ies");
}

static int install(struct FileSysResource* fsr, const char* path)
{
	struct FileSysEntry* fse;
	struct FileSysEntry* e;
	BPTR seg;

	Forbid();
	for (e = (struct FileSysEntry*)fsr->fsr_FileSysEntries.lh_Head;
			e->fse_Node.ln_Succ != NULL;
			e = (struct FileSysEntry*)e->fse_Node.ln_Succ)
		if (e->fse_DosType == EXFAT_DOSTYPE)
		{
			Permit();
			printf("'FATX' is already registered (seglist %08lx) - reboot "
					"before registering another.\n",
					(unsigned long)e->fse_SegList);
			return RETURN_WARN;
		}
	Permit();

	seg = LoadSeg((CONST_STRPTR)path);
	if (seg == 0)
	{
		printf("cannot load %s (error %ld)\n", path, (long)IoErr());
		return RETURN_FAIL;
	}

	fse = AllocMem(sizeof(*fse), MEMF_PUBLIC | MEMF_CLEAR);
	if (fse == NULL)
	{
		UnLoadSeg(seg);
		printf("out of memory\n");
		return RETURN_FAIL;
	}

	/* Deliberately identical to what romtag.c builds, so that what is
	   tested here is what the ROM would do. */
	fse->fse_Node.ln_Name = (char*)"exfatres";
	fse->fse_DosType      = EXFAT_DOSTYPE;
	fse->fse_Version      = 0x00000001;
	fse->fse_PatchFlags   = FSEF_STACKSIZE | FSEF_SEGLIST | FSEF_GLOBALVEC;
	fse->fse_StackSize    = EXFAT_STACKSIZE;
	fse->fse_SegList      = seg;
	fse->fse_GlobalVec    = (BPTR)-1;

	Forbid();
	AddHead(&fsr->fsr_FileSysEntries, &fse->fse_Node);
	Permit();

	printf("registered 'FATX' from %s\n", path);
	printf("  seglist %08lx -> code %08lx, stack %ld\n",
			(unsigned long)seg, (unsigned long)BADDR(seg) + 4,
			(long)EXFAT_STACKSIZE);
	printf("Insert the card (or reboot) - every partition will now be served\n"
			"from this one segment list, as it would be from ROM.\n");
	return RETURN_OK;
}

int main(void)
{
	struct RDArgs* rda;
	LONG args[ARG_COUNT];
	struct FileSysResource* fsr;
	const char* cmd;
	int rc;

	memset(args, 0, sizeof(args));
	rda = ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL);
	if (rda == NULL)
	{
		printf("usage: exfatres INSTALL|LIST [file]\n");
		return RETURN_FAIL;
	}
	cmd = (const char*)args[ARG_COMMAND];

	fsr = (struct FileSysResource*)OpenResource((CONST_STRPTR)FSRNAME);
	if (fsr == NULL)
	{
		printf("no FileSystem.resource on this machine\n");
		FreeArgs(rda);
		return RETURN_FAIL;
	}

	if (cmd[0] == 'l' || cmd[0] == 'L')
	{
		show(fsr);
		rc = RETURN_OK;
	}
	else if (cmd[0] == 'i' || cmd[0] == 'I')
	{
		const char* path = (args[ARG_FILE] != 0) ?
				(const char*)args[ARG_FILE] : DEFAULT_HANDLER;

		rc = install(fsr, path);
	}
	else
	{
		printf("unknown command \"%s\" - use INSTALL or LIST\n", cmd);
		rc = RETURN_FAIL;
	}

	FreeArgs(rda);
	return rc;
}
