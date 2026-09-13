/*
	dev_io.c

	AmigaOS block-device back end for libexfat.  Replaces the POSIX
	file-descriptor device layer in libexfat/io.c (which is fenced off with
	#if !defined(__amigaos__)).  Everything here talks to an Exec device
	through an IOExtTD request.

	Copyright (C) 2026  Willem Drijver
	Based on the free exFAT implementation, Copyright (C) 2010-2023 Andrew Nayenko.

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 2 of the License, or
	(at your option) any later version.
*/

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/io.h>
#include <devices/trackdisk.h>
#include <devices/scsidisk.h>
#include <proto/exec.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>

#include "exfat_amiga.h"
#include "ApolloCrossDev_Debug.h"
#include <devices/newstyle.h>

/* TD64, as implemented by sagasd.device and lide.device. */
#ifndef TD_READ64
#define TD_READ64	24
#define TD_WRITE64	25
#endif

struct exfat_dev
{
	struct MsgPort*		port;
	struct IOExtTD*		req;
	BOOL			devopen;
	enum exfat_mode		mode;

	uint64_t			firstbyte;	/* partition start on media */
	exfat_off_t		size;		/* partition length, bytes  */
	exfat_off_t		pos;		/* for exfat_seek/read/write */

	ULONG			blocksize;
	ULONG			blockmask;	/* blocksize - 1            */
	ULONG			maxtransfer;
	ULONG			mask;

	UWORD			cmd_read;
	UWORD			cmd_write;

	UBYTE*			bounce;		/* one block, mask-safe     */
	uint64_t		bounce_lba;	/* what bounce holds, or ~0 */
};

#define NO_BLOCK	((uint64_t)~0ULL)

/* No state lives here.  The volume to open is passed through exfat_open()'s
   `spec' argument, which libexfat only forwards from exfat_mount() and never
   inspects - see exfat_open() below.  A static would be shared by every
   handler process started from the same seglist, which is exactly what
   happens when the handler is resident in FileSystem.resource. */

/* ------------------------------------------------------------------ */
/* NSD / TD64 probing                                                 */
/* ------------------------------------------------------------------ */

/* Pick the command pair to address the medium with.  Mirrors what the 3.2
   Fast File System does: try NSD first, fall back to TD64, and only use
   the 32-bit commands when the partition ends below 4 GB. */
static void probe_commands(struct exfat_dev* dev)
{
	EXFAT_SYSBASE;
	struct NSDeviceQueryResult nsd;
	uint64_t lastbyte = dev->firstbyte + (uint64_t)dev->size;
	BOOL need64 = (lastbyte > 0xFFFFFFFFULL);
	UWORD* cmds;

	dev->cmd_read = CMD_READ;
	dev->cmd_write = CMD_WRITE;

	memset(&nsd, 0, sizeof(nsd));
	nsd.nsdqr_SizeAvailable = 0;
	nsd.nsdqr_DevQueryFormat = 0;

	dev->req->iotd_Req.io_Command = NSCMD_DEVICEQUERY;
	dev->req->iotd_Req.io_Data = &nsd;
	dev->req->iotd_Req.io_Length = sizeof(nsd);
	dev->req->iotd_Req.io_Actual = 0;

	if (DoIO((struct IORequest*)dev->req) == 0 &&
			nsd.nsdqr_SizeAvailable >= 16 &&
			dev->req->iotd_Req.io_Actual >= 16 &&
			nsd.nsdqr_SupportedCommands != NULL)
	{
		for (cmds = (UWORD*)nsd.nsdqr_SupportedCommands; *cmds != 0; cmds++)
		{
			if (*cmds == NSCMD_TD_READ64)
			{
				dev->cmd_read = NSCMD_TD_READ64;
				dev->cmd_write = NSCMD_TD_WRITE64;
			}
		}
	}

	if (dev->cmd_read == CMD_READ && need64)
	{
		/* No NSD.  Assume TD64; there is no way to probe for it, which
		   is exactly the situation the FFS release notes describe. */
		dev->cmd_read = TD_READ64;
		dev->cmd_write = TD_WRITE64;
		Debug_Warn("no NSD64, falling back to TD64\n");
	}

	Debug_Info("io commands: read %ld write %ld (64-bit needed: %ld)\n",
			(LONG)dev->cmd_read, (LONG)dev->cmd_write, (LONG)need64);
}

/* ------------------------------------------------------------------ */
/* Raw transfers                                                      */
/* ------------------------------------------------------------------ */

/* One device transfer of `len' bytes at absolute media byte offset `off'.
   `off' and `len' must both be block-aligned and `buf' must satisfy the
   DosEnvec mask.  Returns 0 on success. */
static LONG raw_io(struct exfat_dev* dev, BOOL write, void* buf, ULONG len,
		uint64_t off)
{
	EXFAT_SYSBASE;
	LONG err;

	dev->req->iotd_Req.io_Command = write ? dev->cmd_write : dev->cmd_read;
	dev->req->iotd_Req.io_Data = buf;
	dev->req->iotd_Req.io_Length = len;
	dev->req->iotd_Req.io_Offset = (ULONG)(off & 0xFFFFFFFFULL);
	/* TD64 and NSD64 both carry the high 32 bits of the byte offset in
	   io_Actual; the 32-bit commands require it to be zero. */
	dev->req->iotd_Req.io_Actual = (ULONG)(off >> 32);

	err = DoIO((struct IORequest*)dev->req);
	if (err != 0)
	{
		Debug_Error("device io error %ld (cmd %ld, len %lu, off %lu:%lu)\n",
				(LONG)err, (LONG)dev->req->iotd_Req.io_Command,
				(ULONG)len, (ULONG)(off >> 32),
				(ULONG)(off & 0xFFFFFFFFULL));
		return err;
	}
	if (dev->req->iotd_Req.io_Actual != len)
	{
		Debug_Error("short transfer: wanted %lu got %lu\n",
				(ULONG)len, (ULONG)dev->req->iotd_Req.io_Actual);
		return -1;
	}
	return 0;
}

/* Transfer a block-aligned run, honouring de_MaxTransfer and de_Mask. */
static LONG aligned_io(struct exfat_dev* dev, BOOL write, UBYTE* buf,
		ULONG len, uint64_t off)
{
	EXFAT_SYSBASE;
	if (write)
		dev->bounce_lba = NO_BLOCK;	/* the cached block may be in range */

	while (len > 0)
	{
		ULONG chunk = len;
		BOOL bounced = FALSE;

		if (dev->maxtransfer != 0 && chunk > dev->maxtransfer)
			chunk = dev->maxtransfer & ~dev->blockmask;
		if (chunk == 0)
			chunk = dev->blocksize;

		/* If the caller's buffer is not acceptable to the driver, go
		   through our own block-aligned bounce buffer. */
		if ((((ULONG)buf) & ~dev->mask) != 0)
		{
			chunk = dev->blocksize;
			bounced = TRUE;
		}

		if (bounced)
		{
			if (write)
			{
				memcpy(dev->bounce, buf, chunk);
				if (raw_io(dev, TRUE, dev->bounce, chunk, off) != 0)
					return -1;
			}
			else
			{
				if (raw_io(dev, FALSE, dev->bounce, chunk, off) != 0)
					return -1;
				memcpy(buf, dev->bounce, chunk);
			}
		}
		else if (raw_io(dev, write, buf, chunk, off) != 0)
			return -1;

		buf += chunk;
		off += chunk;
		len -= chunk;
	}
	return 0;
}

/* Read one block into the bounce buffer, unless it is already there.

   libexfat reads directory entries 32 bytes at a time (read_entries()), so a
   directory scan is a run of small sequential reads inside the same sector.
   Without this, each of the 16 entries in a 512 byte sector costs its own
   device transfer.  Any write through this device invalidates the buffer. */
static LONG fetch_block(struct exfat_dev* dev, uint64_t blockoff)
{
	EXFAT_SYSBASE;
	if (dev->bounce_lba == blockoff)
		return 0;
	dev->bounce_lba = NO_BLOCK;
	if (raw_io(dev, FALSE, dev->bounce, dev->blocksize, blockoff) != 0)
		return -1;
	dev->bounce_lba = blockoff;
	return 0;
}

/* Read or write an arbitrary byte range within the partition.  Head and
   tail that do not fall on block boundaries go through the bounce buffer;
   the aligned middle is transferred directly. */
static ssize_t partition_io(struct exfat_dev* dev, BOOL write, void* buffer,
		size_t size, exfat_off_t offset)
{
	EXFAT_SYSBASE;
	UBYTE* buf = buffer;
	uint64_t abs;
	ULONG head;
	size_t remaining = size;

	if (offset < 0 || size == 0)
		return 0;
	if (offset >= dev->size)
		return 0;
	if ((exfat_off_t)(offset + (exfat_off_t)size) > dev->size)
	{
		Debug_Error("access past end of partition (off %ld, size %lu)\n",
				(LONG)offset, (ULONG)size);
		return -1;
	}

	abs = dev->firstbyte + (uint64_t)offset;

	/* unaligned head */
	head = (ULONG)(abs & (uint64_t)dev->blockmask);
	if (head != 0)
	{
		ULONG n = dev->blocksize - head;
		if ((size_t)n > remaining)
			n = (ULONG)remaining;
		if (fetch_block(dev, abs - head) != 0)
			return -1;
		if (write)
		{
			memcpy(dev->bounce + head, buf, n);
			if (raw_io(dev, TRUE, dev->bounce, dev->blocksize,
						abs - head) != 0)
			{
				dev->bounce_lba = NO_BLOCK;
				return -1;
			}
		}
		else
			memcpy(buf, dev->bounce + head, n);
		buf += n;
		abs += n;
		remaining -= n;
	}

	/* aligned middle */
	if (remaining >= dev->blocksize)
	{
		ULONG n = (ULONG)(remaining & ~(size_t)dev->blockmask);
		if (aligned_io(dev, write, buf, n, abs) != 0)
			return -1;
		buf += n;
		abs += n;
		remaining -= n;
	}

	/* unaligned tail */
	if (remaining > 0)
	{
		if (fetch_block(dev, abs) != 0)
			return -1;
		if (write)
		{
			memcpy(dev->bounce, buf, remaining);
			if (raw_io(dev, TRUE, dev->bounce, dev->blocksize, abs) != 0)
			{
				dev->bounce_lba = NO_BLOCK;
				return -1;
			}
		}
		else
			memcpy(buf, dev->bounce, remaining);
	}

	return (ssize_t)size;
}

/* ------------------------------------------------------------------ */
/* libexfat device interface                                          */
/* ------------------------------------------------------------------ */

struct exfat_dev* exfat_open(const char* spec, enum exfat_mode mode)
{
	EXFAT_SYSBASE;
	struct exfat_dev* dev;

	/* On AmigaOS `spec' is not a path but a pointer to the partition
	   description, handed down from exfat_mount().  This keeps the handler
	   reentrant: nothing about the volume is stored in a global. */
	const struct ExfatDevSpec* devspec = (const struct ExfatDevSpec*)spec;

	if (devspec == NULL)
	{
		exfat_error("no device parameters passed to exfat_open()");
		return NULL;
	}


	dev = AllocVec(sizeof(struct exfat_dev), MEMF_ANY | MEMF_CLEAR);
	if (dev == NULL)
	{
		exfat_error("failed to allocate memory for device structure");
		return NULL;
	}

	dev->mode = mode;
	dev->firstbyte = devspec->firstbyte;
	dev->size = (exfat_off_t)devspec->length;
	dev->blocksize = devspec->blocksize;
	dev->blockmask = devspec->blocksize - 1;
	dev->maxtransfer = devspec->maxtransfer;
	dev->mask = devspec->mask ? devspec->mask : 0xFFFFFFFFUL;
	dev->pos = 0;

	if (dev->blocksize == 0 || (dev->blocksize & dev->blockmask) != 0)
	{
		exfat_error("block size %u is not a power of two",
				(unsigned)dev->blocksize);
		FreeVec(dev);
		return NULL;
	}

	dev->port = CreateMsgPort();
	if (dev->port == NULL)
	{
		exfat_error("failed to create message port");
		FreeVec(dev);
		return NULL;
	}

	dev->req = (struct IOExtTD*)CreateIORequest(dev->port,
			sizeof(struct IOExtTD));
	if (dev->req == NULL)
	{
		exfat_error("failed to create io request");
		DeleteMsgPort(dev->port);
		FreeVec(dev);
		return NULL;
	}

	if (OpenDevice((CONST_STRPTR)devspec->devname,
			devspec->unit, (struct IORequest*)dev->req,
			devspec->flags) != 0)
	{
		exfat_error("failed to open %s unit %u",
				devspec->devname,
				(unsigned)devspec->unit);
		DeleteIORequest((struct IORequest*)dev->req);
		DeleteMsgPort(dev->port);
		FreeVec(dev);
		return NULL;
	}
	dev->devopen = TRUE;

	/* MEMF_PUBLIC so the driver may reach it; the mask check in
	   aligned_io() still applies to the caller's own buffers. */
	dev->bounce_lba = NO_BLOCK;
	dev->bounce = AllocVec(dev->blocksize, MEMF_PUBLIC | MEMF_CLEAR);
	if (dev->bounce == NULL)
	{
		exfat_error("failed to allocate bounce buffer");
		exfat_close(dev);
		return NULL;
	}

	probe_commands(dev);

	/* A physically write-protected card must degrade to a read-only mount
	   rather than failing every write later on. */
	if (dev->mode != EXFAT_MODE_RO)
	{
		dev->req->iotd_Req.io_Command = TD_PROTSTATUS;
		dev->req->iotd_Req.io_Data = NULL;
		dev->req->iotd_Req.io_Length = 0;
		dev->req->iotd_Req.io_Offset = 0;
		dev->req->iotd_Req.io_Actual = 0;
		if (DoIO((struct IORequest*)dev->req) == 0 &&
				dev->req->iotd_Req.io_Actual != 0)
		{
			exfat_warn("medium is write protected, mounting read-only");
			dev->mode = EXFAT_MODE_RO;
		}
	}

	Debug_Info("opened %s unit %lu: %lu byte blocks, partition at %lu:%lu, "
			"%lu:%lu bytes\n",
			devspec->devname, (ULONG)devspec->unit,
			(ULONG)dev->blocksize,
			(ULONG)(dev->firstbyte >> 32),
			(ULONG)(dev->firstbyte & 0xFFFFFFFFULL),
			(ULONG)((uint64_t)dev->size >> 32),
			(ULONG)((uint64_t)dev->size & 0xFFFFFFFFULL));

	return dev;
}

int exfat_close(struct exfat_dev* dev)
{
	EXFAT_SYSBASE;

	if (dev == NULL)
		return 0;
	if (dev->bounce != NULL)
		FreeVec(dev->bounce);
	if (dev->devopen)
		CloseDevice((struct IORequest*)dev->req);
	if (dev->req != NULL)
		DeleteIORequest((struct IORequest*)dev->req);
	if (dev->port != NULL)
		DeleteMsgPort(dev->port);
	FreeVec(dev);
	return 0;
}

int exfat_fsync(struct exfat_dev* dev)
{
	EXFAT_SYSBASE;
	if (dev->mode == EXFAT_MODE_RO)
		return 0;

	dev->req->iotd_Req.io_Command = CMD_UPDATE;
	dev->req->iotd_Req.io_Data = NULL;
	dev->req->iotd_Req.io_Length = 0;
	dev->req->iotd_Req.io_Offset = 0;
	dev->req->iotd_Req.io_Actual = 0;
	if (DoIO((struct IORequest*)dev->req) != 0)
	{
		exfat_error("CMD_UPDATE failed");
		return -EIO;
	}
	return 0;
}

enum exfat_mode exfat_get_mode(const struct exfat_dev* dev)
{
	EXFAT_SYSBASE;
	return dev->mode;
}

exfat_off_t exfat_get_size(const struct exfat_dev* dev)
{
	EXFAT_SYSBASE;
	return dev->size;
}

exfat_off_t exfat_seek(struct exfat_dev* dev, exfat_off_t offset, int whence)
{
	EXFAT_SYSBASE;
	switch (whence)
	{
	case SEEK_SET:	dev->pos = offset; break;
	case SEEK_CUR:	dev->pos += offset; break;
	case SEEK_END:	dev->pos = dev->size + offset; break;
	default:	return -1;
	}
	if (dev->pos < 0)
	{
		dev->pos = 0;
		return -1;
	}
	return dev->pos;
}

ssize_t exfat_read(struct exfat_dev* dev, void* buffer, size_t size)
{
	EXFAT_SYSBASE;
	ssize_t n = partition_io(dev, FALSE, buffer, size, dev->pos);

	if (n > 0)
		dev->pos += n;
	return n;
}

ssize_t exfat_write(struct exfat_dev* dev, const void* buffer, size_t size)
{
	EXFAT_SYSBASE;
	ssize_t n = partition_io(dev, TRUE, (void*)buffer, size, dev->pos);

	if (n > 0)
		dev->pos += n;
	return n;
}

ssize_t exfat_pread(struct exfat_dev* dev, void* buffer, size_t size,
		exfat_off_t offset)
{
	EXFAT_SYSBASE;
	return partition_io(dev, FALSE, buffer, size, offset);
}

ssize_t exfat_pwrite(struct exfat_dev* dev, const void* buffer, size_t size,
		exfat_off_t offset)
{
	EXFAT_SYSBASE;
	return partition_io(dev, TRUE, (void*)buffer, size, offset);
}
