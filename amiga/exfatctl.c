/*
	exfatctl.c

	Control an mounted exFAT volume from the Shell.

	AmigaOS 3.2 has no standard "unmount" command, and a file system that is
	not shut down cleanly leaves the exFAT VolumeDirty flag set - the volume
	then gets checked by the next host that mounts it.  This sends the
	packets needed to put a volume into a safe state before the card is
	pulled or the machine is rebooted.

	  exfatctl EXF0: flush     write out everything pending
	  exfatctl EXF0: die       flush, clear the dirty flag, and shut down
	  exfatctl EXF0: inhibit   stop the file system touching the medium
	  exfatctl EXF0: uninhibit resume
	  exfatctl EXF0: info      report what the handler thinks the state is

	Copyright (C) 2026  Willem Drijver

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 2 of the License, or
	(at your option) any later version.
*/

#include <exec/types.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/dos.h>

#ifndef _TIMEVAL_DEFINED
#define _TIMEVAL_DEFINED
#endif

#include <dos/rdargs.h>
#include <stdio.h>
#include <string.h>

/* Arguments come from ReadArgs(), not argc/argv: it reads the Shell's
   argument line through pr_CIS and so does not depend on whatever startup
   code the binary was linked with.  It also gives "exfatctl ?" for free. */
#define TEMPLATE "DEVICE/A,COMMAND/A"

enum { ARG_DEVICE, ARG_COMMAND, ARG_COUNT };

int main(void)
{
	struct RDArgs* rda;
	LONG args[ARG_COUNT];
	struct MsgPort* port;
	const char* dev;
	const char* cmd;
	LONG res;
	int rc;

	memset(args, 0, sizeof(args));
	rda = ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL);
	if (rda == NULL)
	{
		PrintFault(IoErr(), (CONST_STRPTR)"exfatctl");
		printf("usage: exfatctl <device:> "
				"<flush|die|inhibit|uninhibit|info>\n");
		return RETURN_ERROR;
	}
	dev = (const char*)args[ARG_DEVICE];
	cmd = (const char*)args[ARG_COMMAND];

	port = DeviceProc((CONST_STRPTR)dev);
	if (port == NULL)
	{
		printf("%s is not mounted (IoErr %ld)\n", dev, (long)IoErr());
		FreeArgs(rda);
		return RETURN_FAIL;
	}

	rc = RETURN_ERROR;

	if (strcmp(cmd, "flush") == 0)
	{
		res = DoPkt0(port, ACTION_FLUSH);
		printf("flush: %s\n", res ? "ok" : "FAILED");
		rc = res ? RETURN_OK : RETURN_FAIL;
	}
	else
	if (strcmp(cmd, "die") == 0)
	{
		/* Flush first, and take the result seriously.  If the medium cannot
		   be written there is no point shutting down: exfat_unmount() would
		   flush again, ignore the failure, and leave with the handler gone
		   and the volume possibly inconsistent.  Better to stay up, say so,
		   and leave VolumeDirty set so the next host checks the card. */
		if (!DoPkt0(port, ACTION_FLUSH))
		{
			printf("%s: FLUSH FAILED (IoErr %ld) - not shutting down.\n",
					dev, (long)IoErr());
			printf("Data may still be unwritten.  Do not remove the card:\n");
			printf("investigate the medium first, then retry.  The volume\n");
			printf("stays flagged dirty, so a host will check it.\n");
			FreeArgs(rda);
			return RETURN_FAIL;
		}
		res = DoPkt0(port, ACTION_DIE);
		if (res)
			printf("%s shut down cleanly - safe to remove the card\n", dev);
		else
		{
			printf("%s refused to shut down (IoErr %ld).\n", dev,
					(long)IoErr());
			printf("Something still holds a lock or an open file: close any\n");
			printf("Workbench window on the volume and CD elsewhere, then\n");
			printf("retry.  If you just want to remove the card safely, use\n");
			printf("  exfatctl %s inhibit\n", dev);
			printf("which flushes and marks the volume clean without exiting.\n");
		}
		rc = res ? RETURN_OK : RETURN_FAIL;
	}
	else
	if (strcmp(cmd, "inhibit") == 0 || strcmp(cmd, "uninhibit") == 0)
	{
		LONG on = (cmd[0] == 'i') ? DOSTRUE : DOSFALSE;

		res = DoPkt1(port, ACTION_INHIBIT, on);
		printf("inhibit %s: %s\n", on ? "on" : "off", res ? "ok" : "FAILED");
		rc = res ? RETURN_OK : RETURN_FAIL;
	}
	else
	if (strcmp(cmd, "info") == 0)
	{
		struct InfoData id;

		memset(&id, 0, sizeof(id));
		res = DoPkt1(port, ACTION_DISK_INFO, (LONG)MKBADDR(&id));
		if (!res)
		{
			printf("ACTION_DISK_INFO failed (IoErr %ld)\n", (long)IoErr());
			FreeArgs(rda);
			return RETURN_FAIL;
		}
		printf("unit          %ld\n", (long)id.id_UnitNumber);
		printf("state         %ld (%s)\n", (long)id.id_DiskState,
				id.id_DiskState == ID_WRITE_PROTECTED ? "write protected" :
				id.id_DiskState == ID_VALIDATING ? "validating" :
				id.id_DiskState == ID_VALIDATED ? "read/write" : "?");
		printf("blocks        %ld total, %ld used\n",
				(long)id.id_NumBlocks, (long)id.id_NumBlocksUsed);
		printf("bytes/block   %ld\n", (long)id.id_BytesPerBlock);
		printf("disk type     0x%08lx\n", (unsigned long)id.id_DiskType);
		printf("in use        %s\n", id.id_InUse ? "yes" : "no");
		rc = RETURN_OK;
	}
	else
	{
		printf("unknown command \"%s\"\n", cmd);
		printf("usage: exfatctl <device:> "
				"<flush|die|inhibit|uninhibit|info>\n");
	}

	FreeArgs(rda);
	return rc;
}
