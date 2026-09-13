/*
	handler.c

	The AmigaDOS side of the exFAT file system: handler startup, the packet
	loop, locks, directory scanning and file reading.

	Milestone 1 is read-only and implements the packet set needed to mount a
	volume and browse it: LOCATE_OBJECT, FREE_LOCK, COPY_DIR, PARENT,
	SAME_LOCK, EXAMINE_OBJECT, EXAMINE_NEXT, FINDINPUT, READ, SEEK, END,
	INFO, DISK_INFO, CURRENT_VOLUME, IS_FILESYSTEM, FLUSH, INHIBIT and DIE.
	Everything else is answered with ERROR_ACTION_NOT_KNOWN.

	Packet semantics follow docs/os/amigados/13-packet-documentation.md;
	startup follows section 12.1.2 of the same manual.

	Copyright (C) 2026  Willem Drijver
	Based on the free exFAT implementation, Copyright (C) 2010-2023 Andrew Nayenko.

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 2 of the License, or
	(at your option) any later version.
*/

#include "exfat_amiga.h"

#include <exec/memory.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/expansion.h>
#include <dos/dostags.h>
#include <dos/filehandler.h>
#include <string.h>
#include <errno.h>

#include "ApolloCrossDev_Debug.h"

extern void exfat_handler_entry(void);	/* entry.S, module offset 0 */
#include "exfatfs.h"	/* EXFAT_STATE_MOUNTED, for the dirty flag */
#include <resources/filesysres.h>	/* EXFAT_STATE_MOUNTED, for the dirty flag */

/* Two different things that are easy to confuse:

   EXFAT_DOSTYPE ('FATX') is the DOSType that identifies *which file system
   handles this partition*.  It belongs in de_DosType - i.e. the DosType
   keyword of the mount file, or an RDB / FileSystem.resource entry.  Nothing
   in this file sets it; it is here for reference so the two stay in step.

   id_DiskType in an InfoData, and dl_DiskType on the volume node, are NOT
   that.  Section 5.2.4 is explicit: id_DiskType says whether the file system
   recognises the medium and claims responsibility for it, "shall not be used
   to identify a particular file system", and new designs "should rather
   return the generic ID_DOS_DISK".  Table 5.6 lists the only valid values,
   and a value outside it makes Info report an unreadable disk. */
/* EXFAT_DOSTYPE and the FSEF_* bits live in exfat_amiga.h - romtag.c needs
   the same values to build the FileSystem.resource entry. */
#define EXFAT_ID_BUSY	0x42555359UL	/* 'BUSY' - while inhibited */

#define MAX_PATH_UTF8	1024

/* NO per-instance state may live in a global.  When the handler is resident
   in FileSystem.resource every partition's process is started from the same
   seglist and therefore shares this file's data and BSS; a shared handler
   struct means two volumes silently overwrite each other.  The state is
   allocated per process in exfat_handler_main() instead.

   SysBase and DOSBase stay global on purpose: every instance writes the same
   value (a library base is a singleton), so sharing them is harmless, and
   the proto/ inline macros expect them by name. */

static BOOL mount_fs(struct ExfatHandler* h);
static void unmount_fs(struct ExfatHandler* h);
static void add_volume(struct ExfatHandler* h);
static void remove_volume(struct ExfatHandler* h);

/* ------------------------------------------------------------------ */
/* Locks                                                              */
/* ------------------------------------------------------------------ */

struct ExfatLock* exfat_amiga_lock_from_bptr(BPTR blk)
{
	return (struct ExfatLock*)BADDR(blk);
}

struct ExfatLock* exfat_amiga_makelock(struct ExfatHandler* h,
		struct exfat_node* node, LONG access)
{
	EXFAT_BASES(h);
	struct ExfatLock* lock;

	/* MEMF_PUBLIC: dos.library and other processes read the FileLock. */
	lock = AllocVec(sizeof(struct ExfatLock), MEMF_PUBLIC | MEMF_CLEAR);
	if (lock == NULL)
		return NULL;

	lock->h = h;
	lock->node = node;		/* takes over the caller's reference */
	lock->scan_next = NULL;
	lock->scan_index = -1;

	lock->fl.fl_Key = (LONG)node->start_cluster;
	lock->fl.fl_Access = access;
	lock->fl.fl_Task = h->port;
	lock->fl.fl_Volume = MKBADDR(h->volume);

	/* Chain onto the volume's lock list, as a well-behaved file system
	   should; dos.library walks this when a volume is removed. */
	if (h->volume != NULL)
	{
		lock->fl.fl_Link = h->volume->dl_LockList;
		h->volume->dl_LockList = MKBADDR(lock);
	}
	h->nlocks++;
	return lock;
}

void exfat_amiga_freelock(struct ExfatHandler* h, struct ExfatLock* lock)
{
	EXFAT_BASES(h);
	if (lock == NULL)
		return;

	if (h->volume != NULL)
	{
		/* unlink from dl_LockList */
		BPTR* prev = &h->volume->dl_LockList;

		while (*prev != 0)
		{
			struct ExfatLock* cur = (struct ExfatLock*)BADDR(*prev);

			if (cur == lock)
			{
				*prev = cur->fl.fl_Link;
				break;
			}
			prev = &cur->fl.fl_Link;
		}
	}

	if (lock->node != NULL)
		exfat_put_node(&h->ef, lock->node);
	FreeVec(lock);
	h->nlocks--;
}

/* ------------------------------------------------------------------ */
/* Safe name conversion                                               */
/* ------------------------------------------------------------------ */

/* exfat_get_name() calls exfat_bug() when node->name is not well-formed
   UTF-16, and exfat_bug() cannot return - on AmigaOS it parks the handler
   process, which hangs every program touching the volume and destroys the
   evidence.  Validate first so a single bad entry degrades to a placeholder
   name and a log line instead of taking the volume down. */
static BOOL name_is_sane(const struct exfat_node* node)
{
	int i;

	for (i = 0; i < EXFAT_NAME_MAX; i++)
	{
		uint16_t u = le16_to_cpu(node->name[i]);

		if (u == 0)
			return TRUE;
		if ((u & 0xFC00) == 0xD800)		/* lead surrogate */
		{
			uint16_t lo;

			if (i + 1 >= EXFAT_NAME_MAX)
				return FALSE;
			lo = le16_to_cpu(node->name[i + 1]);
			if ((lo & 0xFC00) != 0xDC00)	/* must be a trail */
				return FALSE;
			i++;
		}
		else if ((u & 0xFC00) == 0xDC00)	/* stray trail surrogate */
			return FALSE;
	}
	return TRUE;
}

static void safe_get_name(const struct exfat_node* node,
		char buffer[EXFAT_UTF8_NAME_BUFFER_MAX])
{
	if (name_is_sane(node))
	{
		exfat_get_name(node, buffer);
		return;
	}

	Debug_Error("bad UTF-16 name on node %08lx: refs %ld attrib %04lx "
			"cluster %08lx size %lu:%lu flags%s%s%s%s\n",
			(ULONG)node, (LONG)node->references, (ULONG)node->attrib,
			(ULONG)node->start_cluster,
			(ULONG)(node->size >> 32), (ULONG)(node->size & 0xFFFFFFFFUL),
			node->is_contiguous ? " contig" : "",
			node->is_cached ? " cached" : "",
			node->is_dirty ? " dirty" : "",
			node->is_unlinked ? " unlinked" : "");
	Debug_Error("  name[0..7] = %04lx %04lx %04lx %04lx %04lx %04lx %04lx %04lx\n",
			(ULONG)le16_to_cpu(node->name[0]), (ULONG)le16_to_cpu(node->name[1]),
			(ULONG)le16_to_cpu(node->name[2]), (ULONG)le16_to_cpu(node->name[3]),
			(ULONG)le16_to_cpu(node->name[4]), (ULONG)le16_to_cpu(node->name[5]),
			(ULONG)le16_to_cpu(node->name[6]), (ULONG)le16_to_cpu(node->name[7]));

	strcpy(buffer, "<bad name>");
}

/* ------------------------------------------------------------------ */
/* Paths                                                              */
/* ------------------------------------------------------------------ */

/* Build the absolute path of a node by walking up to the root.  Returns
   FALSE if it does not fit. */
static BOOL node_abs_path(struct ExfatHandler* h, struct exfat_node* node,
		char* buf, int size)
{
	EXFAT_BASES(h);
	struct exfat_node* chain[64];
	int depth = 0;
	int pos = 0;
	int i;

	while (node != NULL && node != h->ef.root && depth < 64)
	{
		chain[depth++] = node;
		node = node->parent;
	}
	if (depth >= 64)
		return FALSE;

	buf[pos++] = '/';
	for (i = depth - 1; i >= 0; i--)
	{
		char name[EXFAT_UTF8_NAME_BUFFER_MAX];
		int len;

		safe_get_name(chain[i], name);
		len = (int)strlen(name);
		if (pos + len + 2 > size)
			return FALSE;
		memcpy(buf + pos, name, len);
		pos += len;
		if (i > 0)
			buf[pos++] = '/';
	}
	buf[pos] = '\0';
	return TRUE;
}

/* Turn an AmigaDOS path (BSTR, relative to `base') into an absolute UTF-8
   path for exfat_lookup().

   AmigaDOS conventions honoured here: a colon makes the path absolute and
   whatever precedes it is the device or volume name; a leading slash means
   "parent of", and repeats climb further; an empty path denotes the base
   object itself. */
static BOOL resolve_path(struct ExfatHandler* h, struct ExfatLock* base,
		const UBYTE* bpath, char* out, int outsize)
{
	EXFAT_BASES(h);
	char rel[MAX_PATH_UTF8];
	char abs[MAX_PATH_UTF8];
	const char* p;
	int pos;

	exfat_amiga_bstr_to_utf8(bpath, rel, sizeof(rel));

	/* Anything before a colon has ALREADY been resolved by dos.library:
	   for a volume name it passes no lock and means the root, and for an
	   assign it passes the assign's own lock in dp_Arg1 - and, in both
	   cases, the full string with the prefix still on it.  So the prefix is
	   skipped and the rest is resolved relative to the lock, exactly as a
	   name with no colon would be.  Only a NULL lock means the root.

	   This used to treat every colon as "start from the root", which is
	   right for EXF0:foo and wrong for C:foo: the lock on SYS:C arrived in
	   dp_Arg1 and was thrown away, so C:ApolloMap was looked up as
	   /ApolloMap and "CD C:" landed on the root.  The card booted and no
	   command in the Startup-Sequence could be found. */
	p = strchr(rel, ':');
	if (p == rel)
	{
		/* A leading colon with nothing before it - ":foo" - means the root
		   of the volume the lock is on, whatever the lock.  There is one
		   volume behind this handler, so that is simply the root. */
		base = NULL;
	}
	p = (p != NULL) ? p + 1 : rel;

	if (base == NULL)
	{
		abs[0] = '/';
		abs[1] = '\0';
	}
	else if (!node_abs_path(h, base->node, abs, sizeof(abs)))
		return FALSE;

	pos = (int)strlen(abs);
	/* strip the trailing slash of the root so appends are uniform */
	if (pos > 1 && abs[pos - 1] == '/')
		abs[--pos] = '\0';
	if (pos == 1)
		pos = 0;			/* "/" -> "" */

	while (*p != '\0')
	{
		if (*p == '/')
		{
			/* climb one level */
			while (pos > 0 && abs[pos - 1] != '/')
				pos--;
			if (pos > 0)
				pos--;		/* drop the slash itself */
			abs[pos] = '\0';
			p++;
			continue;
		}
		{
			const char* slash = strchr(p, '/');
			int len = (slash != NULL) ? (int)(slash - p) : (int)strlen(p);

			if (pos + len + 2 > (int)sizeof(abs))
				return FALSE;
			abs[pos++] = '/';
			memcpy(abs + pos, p, len);
			pos += len;
			abs[pos] = '\0';
			p += len;
			if (*p == '/')
				p++;
		}
	}

	if (pos == 0)
	{
		abs[0] = '/';
		abs[1] = '\0';
		pos = 1;
	}
	if (pos + 1 > outsize)
		return FALSE;
	strcpy(out, abs);
	return TRUE;
}

/* ------------------------------------------------------------------ */
/* FileInfoBlock                                                      */
/* ------------------------------------------------------------------ */

static void fill_fib(struct ExfatHandler* h, struct exfat_node* node,
		struct FileInfoBlock* fib, LONG diskkey, BOOL is_root)
{
	EXFAT_BASES(h);
	char name[EXFAT_UTF8_NAME_BUFFER_MAX];
	uint64_t size = node->size;
	ULONG clustersize = CLUSTER_SIZE(*h->ef.sb);

	/* Clear only the fields we own.  Section 13.3.1 says fib_Reserved
	   "shall be left alone" - dos.library's ExAll() emulation, which Copy
	   and Workbench go through, may keep its own iteration state there, and
	   a memset() over the whole structure would destroy it. */
	fib->fib_Protection = 0;
	fib->fib_Size = 0;
	fib->fib_NumBlocks = 0;
	fib->fib_Comment[0] = 0;
	fib->fib_OwnerUID = 0;
	fib->fib_OwnerGID = 0;
	memset(fib->fib_FileName, 0, sizeof(fib->fib_FileName));
	memset(&fib->fib_Date, 0, sizeof(fib->fib_Date));

	fib->fib_DiskKey = diskkey;

	if (is_root)
	{
		fib->fib_DirEntryType = ST_ROOT;
		/* The root's name is the volume name (section 13.3.1). */
		exfat_amiga_name_to_bstr(exfat_get_label(&h->ef),
				(UBYTE*)fib->fib_FileName, 30);
	}
	else
	{
		fib->fib_DirEntryType = (node->attrib & EXFAT_ATTRIB_DIR) ?
				ST_USERDIR : ST_FILE;
		safe_get_name(node, name);
		exfat_amiga_name_to_bstr(name, (UBYTE*)fib->fib_FileName, 106);
	}
	/* Programs read one or the other; keep them identical (13.3.1). */
	fib->fib_EntryType = fib->fib_DirEntryType;

	/* AmigaDOS protection bits are active-low for R/W/E/D: a SET bit
	   forbids the operation.  Reflect exFAT's ReadOnly attribute, and the
	   whole volume being read-only. */
	fib->fib_Protection = 0;
	if (h->ef.ro || (node->attrib & EXFAT_ATTRIB_RO))
		fib->fib_Protection |= FIBF_WRITE | FIBF_DELETE;
	if (node->attrib & EXFAT_ATTRIB_ARCH)
		fib->fib_Protection |= FIBF_ARCHIVE;

	if (node->attrib & EXFAT_ATTRIB_DIR)
	{
		fib->fib_Size = 0;
		fib->fib_NumBlocks = 0;
	}
	else
	{
		/* fib_Size is a LONG.  exFAT allows files past 2 GB, which cannot
		   be expressed; clamp and let the caller see a truncated size
		   rather than a negative one.  See ../CLAUDE.md, decision 2. */
		if (size > 0x7FFFFFFFULL)
		{
			fib->fib_Size = 0x7FFFFFFF;
			Debug_Warn("file larger than 2GB, size clamped\n");
		}
		else
			fib->fib_Size = (LONG)size;
		fib->fib_NumBlocks = (LONG)((size + clustersize - 1) / clustersize);
	}

	exfat_amiga_time_to_datestamp(node->mtime, &fib->fib_Date);

	fib->fib_Comment[0] = 0;	/* empty BSTR */
	fib->fib_OwnerUID = 0;
	fib->fib_OwnerGID = 0;
}

/* ------------------------------------------------------------------ */
/* Packet handlers                                                    */
/* ------------------------------------------------------------------ */

static void do_locate_object(struct ExfatHandler* h, struct DosPacket* pkt)
{
	EXFAT_BASES(h);
	struct ExfatLock* base = exfat_amiga_lock_from_bptr((BPTR)pkt->dp_Arg1);
	struct ExfatLock* lock;
	struct exfat_node* node;
	char path[MAX_PATH_UTF8];
	int rc;

	if (!resolve_path(h, base, (const UBYTE*)BADDR(pkt->dp_Arg2),
			path, sizeof(path)))
	{
		ReplyPkt(pkt, DOSFALSE, ERROR_OBJECT_NOT_FOUND);
		return;
	}

	rc = exfat_lookup(&h->ef, &node, path);
	if (rc != 0)
	{
		ReplyPkt(pkt, DOSFALSE, ERROR_OBJECT_NOT_FOUND);
		return;
	}

	/* Exclusive locks would need a conflict check against outstanding
	   locks; a read-only file system has nothing to protect, so treat every
	   request as shared (13.2.1 permits treating unknown modes so). */
	lock = exfat_amiga_makelock(h, node, SHARED_LOCK);
	if (lock == NULL)
	{
		exfat_put_node(&h->ef, node);
		ReplyPkt(pkt, DOSFALSE, ERROR_NO_FREE_STORE);
		return;
	}
	ReplyPkt(pkt, (LONG)MKBADDR(lock), 0);
}

static void do_copy_dir(struct ExfatHandler* h, struct DosPacket* pkt)
{
	EXFAT_BASES(h);
	struct ExfatLock* src = exfat_amiga_lock_from_bptr((BPTR)pkt->dp_Arg1);
	struct ExfatLock* lock;
	struct exfat_node* node;

	node = (src != NULL) ? src->node : h->ef.root;
	exfat_get_node(node);

	lock = exfat_amiga_makelock(h, node, SHARED_LOCK);
	if (lock == NULL)
	{
		exfat_put_node(&h->ef, node);
		ReplyPkt(pkt, DOSFALSE, ERROR_NO_FREE_STORE);
		return;
	}
	ReplyPkt(pkt, (LONG)MKBADDR(lock), 0);
}

/* ACTION_COPY_DIR_FH (1030) and ACTION_PARENT_FH (1031) are NOT lock
   packets: dp_Arg1 is the fh_Arg1 of an open FileHandle, so it must never be
   run through BADDR().  Sections 13.2.4 and 13.2.5. */
static void do_lock_from_fh(struct ExfatHandler* h, struct DosPacket* pkt,
		BOOL want_parent)
{
	EXFAT_BASES(h);
	struct ExfatFile* file = (struct ExfatFile*)pkt->dp_Arg1;
	struct ExfatLock* lock;
	struct exfat_node* node;

	if (file == NULL || file->node == NULL)
	{
		ReplyPkt(pkt, 0, ERROR_INVALID_LOCK);
		return;
	}

	if (want_parent)
	{
		node = file->node->parent;
		if (node == NULL)
			node = h->ef.root;
	}
	else
		node = file->node;

	exfat_get_node(node);
	lock = exfat_amiga_makelock(h, node, SHARED_LOCK);
	if (lock == NULL)
	{
		exfat_put_node(&h->ef, node);
		ReplyPkt(pkt, 0, ERROR_NO_FREE_STORE);
		return;
	}
	ReplyPkt(pkt, (LONG)MKBADDR(lock), 0);
}

static void do_parent(struct ExfatHandler* h, struct DosPacket* pkt)
{
	EXFAT_BASES(h);
	struct ExfatLock* src = exfat_amiga_lock_from_bptr((BPTR)pkt->dp_Arg1);
	struct ExfatLock* lock;
	struct exfat_node* parent;

	if (src == NULL || src->node == h->ef.root || src->node->parent == NULL)
	{
		/* The parent of the root is ZERO with no error (5.1.3). */
		ReplyPkt(pkt, 0, 0);
		return;
	}

	parent = src->node->parent;
	exfat_get_node(parent);
	lock = exfat_amiga_makelock(h, parent, SHARED_LOCK);
	if (lock == NULL)
	{
		exfat_put_node(&h->ef, parent);
		ReplyPkt(pkt, DOSFALSE, ERROR_NO_FREE_STORE);
		return;
	}
	ReplyPkt(pkt, (LONG)MKBADDR(lock), 0);
}

static void do_examine_object(struct ExfatHandler* h, struct DosPacket* pkt)
{
	EXFAT_BASES(h);
	struct ExfatLock* lock = exfat_amiga_lock_from_bptr((BPTR)pkt->dp_Arg1);
	struct FileInfoBlock* fib = (struct FileInfoBlock*)BADDR(pkt->dp_Arg2);
	struct exfat_node* node = (lock != NULL) ? lock->node : h->ef.root;
	BOOL is_root = (node == h->ef.root);

	fill_fib(h, node, fib, 0, is_root);

	/* This packet also arms a directory scan (13.3.1): reset the state we
	   keep in the lock so the following EXAMINE_NEXT starts at entry 0. */
	if (lock != NULL)
	{
		lock->scan_index = -1;
		lock->scan_next = NULL;
	}
	ReplyPkt(pkt, DOSTRUE, 0);
}

static void do_examine_next(struct ExfatHandler* h, struct DosPacket* pkt)
{
	EXFAT_BASES(h);
	struct ExfatLock* lock = exfat_amiga_lock_from_bptr((BPTR)pkt->dp_Arg1);
	struct FileInfoBlock* fib = (struct FileInfoBlock*)BADDR(pkt->dp_Arg2);
	struct exfat_node* dir = (lock != NULL) ? lock->node : h->ef.root;
	struct exfat_iterator it;
	struct exfat_node* node;
	LONG want;
	LONG index = 0;

	if (!(dir->attrib & EXFAT_ATTRIB_DIR))
	{
		ReplyPkt(pkt, DOSFALSE, ERROR_OBJECT_WRONG_TYPE);
		return;
	}

	/* fib_DiskKey is the only field we may trust between calls (13.3.2).
	   The lock carries the matching position, so the common case is O(1);
	   if the lock was replaced or an entry moved, fall back to counting from
	   fib_DiskKey, which always lands on a valid object. */
	want = fib->fib_DiskKey;

	if (lock != NULL && lock->scan_index == want && lock->scan_next != NULL &&
			dir->is_cached)
	{
		node = lock->scan_next;
		fill_fib(h, node, fib, want + 1, FALSE);
		lock->scan_next = node->next;
		lock->scan_index = want + 1;
		ReplyPkt(pkt, DOSTRUE, 0);
		return;
	}

	if (exfat_opendir(&h->ef, dir, &it) != 0)
	{
		ReplyPkt(pkt, DOSFALSE, ERROR_NO_MORE_ENTRIES);
		return;
	}

	while ((node = exfat_readdir(&it)) != NULL)
	{
		if (index == want)
		{
			fill_fib(h, node, fib, want + 1, FALSE);
			if (lock != NULL)
			{
				lock->scan_next = node->next;
				lock->scan_index = want + 1;
			}
			exfat_put_node(&h->ef, node);
			exfat_closedir(&h->ef, &it);
			ReplyPkt(pkt, DOSTRUE, 0);
			return;
		}
		exfat_put_node(&h->ef, node);
		index++;
	}
	exfat_closedir(&h->ef, &it);

	if (lock != NULL)
	{
		lock->scan_next = NULL;
		lock->scan_index = -1;
	}
	ReplyPkt(pkt, DOSFALSE, ERROR_NO_MORE_ENTRIES);
}

/* libexfat returns negative errno values; AmigaDOS wants its own codes. */
static LONG map_errno(int rc)
{
	switch (rc < 0 ? -rc : rc)
	{
	case 0:		return 0;
	case ENOENT:	return ERROR_OBJECT_NOT_FOUND;
	case EEXIST:	return ERROR_OBJECT_EXISTS;
	case ENOSPC:	return ERROR_DISK_FULL;
	case EROFS:	return ERROR_DISK_WRITE_PROTECTED;
	case EISDIR:
	case ENOTDIR:	return ERROR_OBJECT_WRONG_TYPE;
	case ENOTEMPTY:	return ERROR_DIRECTORY_NOT_EMPTY;
	case EACCES:
	case EPERM:	return ERROR_WRITE_PROTECTED;
	case ENAMETOOLONG: return ERROR_INVALID_COMPONENT_NAME;
	case ENOMEM:	return ERROR_NO_FREE_STORE;
	case EIO:	return ERROR_SEEK_ERROR;
	default:	return ERROR_OBJECT_NOT_FOUND;
	}
}

/* Interpret a size or position argument the way section 13.1.8 recommends:
   zero-extend for OFFSET_BEGINNING, sign-extend for OFFSET_CURRENT, and for
   OFFSET_END zero-extend the negated value then negate in 64 bits. */
static UNUSED BOOL resolve_offset(LONG arg, LONG mode, exfat_off_t current,
		exfat_off_t end, exfat_off_t* out)
{
	switch (mode)
	{
	case OFFSET_BEGINNING:
		*out = (exfat_off_t)(uint64_t)(uint32_t)arg;
		return TRUE;
	case OFFSET_CURRENT:
		*out = current + (exfat_off_t)arg;
		return TRUE;
	case OFFSET_END:
		*out = end - (exfat_off_t)(uint64_t)(uint32_t)(-arg);
		return TRUE;
	default:
		return FALSE;
	}
}

/* The three open packets differ only in whether the file may be created and
   whether an existing one is truncated (sections 13.1.1 - 13.1.3). */
enum { OPEN_OLD, OPEN_NEW, OPEN_UPDATE };

static void do_open(struct ExfatHandler* h, struct DosPacket* pkt, int mode)
{
	EXFAT_BASES(h);
	struct FileHandle* fh = (struct FileHandle*)BADDR(pkt->dp_Arg1);
	struct ExfatLock* base = exfat_amiga_lock_from_bptr((BPTR)pkt->dp_Arg2);
	struct ExfatFile* file;
	struct exfat_node* node = NULL;
	char path[MAX_PATH_UTF8];
	BOOL want_write = (mode != OPEN_OLD);
	int rc;

	if (fh == NULL || !resolve_path(h, base, (const UBYTE*)BADDR(pkt->dp_Arg3),
			path, sizeof(path)))
	{
		ReplyPkt(pkt, DOSFALSE, ERROR_OBJECT_NOT_FOUND);
		return;
	}
	if (want_write && (h->inhibited || h->ef.ro))
	{
		ReplyPkt(pkt, DOSFALSE, ERROR_DISK_WRITE_PROTECTED);
		return;
	}

	rc = exfat_lookup(&h->ef, &node, path);
	if (rc != 0)
	{
		if (!want_write)
		{
			ReplyPkt(pkt, DOSFALSE, ERROR_OBJECT_NOT_FOUND);
			return;
		}
		/* Create the file, but never the directories leading to it
		   (13.1.2). */
		rc = exfat_mknod(&h->ef, path);
		if (rc == 0)
			rc = exfat_lookup(&h->ef, &node, path);
		if (rc != 0)
		{
			Debug_Warn("create %s failed: %ld\n", path, (LONG)rc);
			ReplyPkt(pkt, DOSFALSE, map_errno(rc));
			return;
		}
		Debug_Info("created %s\n", path);
	}
	else if (node->attrib & EXFAT_ATTRIB_DIR)
	{
		exfat_put_node(&h->ef, node);
		ReplyPkt(pkt, DOSFALSE, ERROR_OBJECT_WRONG_TYPE);
		return;
	}
	else if (mode == OPEN_NEW)
	{
		/* MODE_NEWFILE replaces an existing file and its contents. */
		rc = exfat_truncate(&h->ef, node, 0, true);
		if (rc != 0)
		{
			exfat_flush_node(&h->ef, node);
			exfat_put_node(&h->ef, node);
			ReplyPkt(pkt, DOSFALSE, map_errno(rc));
			return;
		}
	}

	file = AllocVec(sizeof(struct ExfatFile), MEMF_ANY | MEMF_CLEAR);
	if (file == NULL)
	{
		exfat_put_node(&h->ef, node);
		ReplyPkt(pkt, DOSFALSE, ERROR_NO_FREE_STORE);
		return;
	}
	file->h = h;
	file->node = node;		/* keeps the reference */
	file->pos = 0;
	file->writable = want_write;

	/* Every later packet for this file arrives as fh_Arg1 (13.1.1). */
	fh->fh_Arg1 = (LONG)file;
	h->nfiles++;
	ReplyPkt(pkt, DOSTRUE, 0);
}

/* ACTION_EXAMINE_FH (1034): like EXAMINE_OBJECT, but the object is named by
   an open file handle rather than a lock.  dp_Arg1 is fh_Arg1, NOT a BPTR.
   Section 13.3.5.  ExamineFH() is how a program asks the size of a file it
   already has open, so a missing implementation shows up as a wrong or stale
   file size even though Examine() on a lock is correct. */
static void do_examine_fh(struct ExfatHandler* h, struct DosPacket* pkt)
{
	EXFAT_BASES(h);
	struct ExfatFile* file = (struct ExfatFile*)pkt->dp_Arg1;
	struct FileInfoBlock* fib = (struct FileInfoBlock*)BADDR(pkt->dp_Arg2);

	if (file == NULL || file->node == NULL || fib == NULL)
	{
		ReplyPkt(pkt, DOSFALSE, ERROR_INVALID_LOCK);
		return;
	}
	fill_fib(h, file->node, fib, 0, FALSE);
	ReplyPkt(pkt, DOSTRUE, 0);
}

/* ACTION_FH_FROM_LOCK (1026): open a file from a lock.  On success the lock
   is absorbed into the file handle and must be released when the file is
   closed, so the caller must not free it.  Section 13.1.4. */
static void do_fh_from_lock(struct ExfatHandler* h, struct DosPacket* pkt)
{
	EXFAT_BASES(h);
	struct FileHandle* fh = (struct FileHandle*)BADDR(pkt->dp_Arg1);
	struct ExfatLock* lock = exfat_amiga_lock_from_bptr((BPTR)pkt->dp_Arg2);
	struct ExfatFile* file;

	if (fh == NULL || lock == NULL || lock->node == NULL)
	{
		ReplyPkt(pkt, DOSFALSE, ERROR_INVALID_LOCK);
		return;
	}
	if (lock->node->attrib & EXFAT_ATTRIB_DIR)
	{
		ReplyPkt(pkt, DOSFALSE, ERROR_OBJECT_WRONG_TYPE);
		return;
	}

	file = AllocVec(sizeof(struct ExfatFile), MEMF_ANY | MEMF_CLEAR);
	if (file == NULL)
	{
		ReplyPkt(pkt, DOSFALSE, ERROR_NO_FREE_STORE);
		return;
	}
	file->h = h;
	file->node = lock->node;
	exfat_get_node(file->node);	/* the file's own reference */
	file->pos = 0;
	/* The lock's access mode decides: an exclusive lock may be written
	   through, a shared one may not (13.1.4). */
	file->writable = (lock->fl.fl_Access == EXCLUSIVE_LOCK) ? TRUE : FALSE;

	/* absorb the lock: its reference goes away with it, ours remains */
	exfat_amiga_freelock(h, lock);

	fh->fh_Arg1 = (LONG)file;
	h->nfiles++;
	ReplyPkt(pkt, DOSTRUE, 0);
}


static void do_write(struct ExfatHandler* h, struct DosPacket* pkt)
{
	EXFAT_BASES(h);
	struct ExfatFile* file = (struct ExfatFile*)pkt->dp_Arg1;
	const void* buffer = (const void*)pkt->dp_Arg2;
	LONG length = pkt->dp_Arg3;
	ssize_t n;

	if (file == NULL || length < 0)
	{
		ReplyPkt(pkt, -1, ERROR_INVALID_LOCK);
		return;
	}
	if (h->inhibited || h->ef.ro || !file->writable)
	{
		ReplyPkt(pkt, -1, ERROR_DISK_WRITE_PROTECTED);
		return;
	}
	if (length == 0)
	{
		ReplyPkt(pkt, 0, 0);
		return;
	}

	n = exfat_generic_pwrite(&h->ef, file->node, buffer, (size_t)length,
			file->pos);
	if (n < 0)
	{
		Debug_Error("write of %ld bytes failed: %ld\n", (LONG)length, (LONG)n);
		ReplyPkt(pkt, -1, map_errno((int)n));
		return;
	}
	file->pos += n;
	ReplyPkt(pkt, (LONG)n, 0);
}

static void do_set_file_size(struct ExfatHandler* h, struct DosPacket* pkt)
{
	EXFAT_BASES(h);
	struct ExfatFile* file = (struct ExfatFile*)pkt->dp_Arg1;
	exfat_off_t newsize;
	int rc;

	if (file == NULL)
	{
		ReplyPkt(pkt, -1, ERROR_INVALID_LOCK);
		return;
	}
	if (h->inhibited || h->ef.ro || !file->writable)
	{
		ReplyPkt(pkt, -1, ERROR_DISK_WRITE_PROTECTED);
		return;
	}
	if (!resolve_offset(pkt->dp_Arg2, pkt->dp_Arg3, file->pos,
			(exfat_off_t)file->node->size, &newsize) || newsize < 0)
	{
		ReplyPkt(pkt, -1, ERROR_SEEK_ERROR);
		return;
	}

	/* AmigaDOS does not require the extended region to be zeroed (13.1.9),
	   but exFAT's ValidDataLength makes reads past it return zeroes anyway,
	   which is the safer behaviour - it cannot expose deleted data. */
	rc = exfat_truncate(&h->ef, file->node, (uint64_t)newsize, true);
	if (rc != 0)
	{
		Debug_Error("truncate failed: %ld\n", (LONG)rc);
		ReplyPkt(pkt, -1, map_errno(rc));
		return;
	}
	/* "file pointers shall be clamped to the new file size if necessary,
	   but shall remain unaltered otherwise" (13.1.9). */
	if (file->pos > newsize)
		file->pos = newsize;

	ReplyPkt(pkt, (LONG)(uint32_t)(uint64_t)newsize, 0);
}

/* ACTION_SET_PROTECT (21) and ACTION_SET_DATE (34) both take the lock in
   dp_Arg2 and the path in dp_Arg3 - dp_Arg1 is unused.  Sections 13.5.3 and
   13.5.5.  Copy issues both on the destination after the data, so refusing
   them makes an otherwise successful copy report a write-protected volume. */
static struct exfat_node* lookup_arg23(struct ExfatHandler* h,
		struct DosPacket* pkt, LONG* err)
{
	EXFAT_BASES(h);
	struct ExfatLock* base = exfat_amiga_lock_from_bptr((BPTR)pkt->dp_Arg2);
	struct exfat_node* node = NULL;
	char path[MAX_PATH_UTF8];
	int rc;

	if (!resolve_path(h, base, (const UBYTE*)BADDR(pkt->dp_Arg3),
			path, sizeof(path)))
	{
		*err = ERROR_OBJECT_NOT_FOUND;
		return NULL;
	}
	rc = exfat_lookup(&h->ef, &node, path);
	if (rc != 0)
	{
		*err = map_errno(rc);
		return NULL;
	}
	*err = 0;
	return node;
}

static void do_set_protect(struct ExfatHandler* h, struct DosPacket* pkt)
{
	EXFAT_BASES(h);
	struct exfat_node* node;
	LONG bits = pkt->dp_Arg4;
	LONG err = 0;
	uint16_t attrib;

	if (h->inhibited || h->ef.ro)
	{
		ReplyPkt(pkt, DOSFALSE, ERROR_DISK_WRITE_PROTECTED);
		return;
	}
	node = lookup_arg23(h, pkt, &err);
	if (node == NULL)
	{
		ReplyPkt(pkt, DOSFALSE, err);
		return;
	}

	/* exFAT carries only ReadOnly / Hidden / System / Archive.  Section
	   13.5.3 explicitly allows a file system to implement a subset and
	   ignore the rest, so map what maps and drop the Amiga-only bits
	   (Pure, Script, Hold, and the group/other sets). */
	attrib = node->attrib;
	if (bits & (FIBF_WRITE | FIBF_DELETE))
		attrib |= EXFAT_ATTRIB_RO;
	else
		attrib &= ~EXFAT_ATTRIB_RO;
	if (bits & FIBF_ARCHIVE)
		attrib |= EXFAT_ATTRIB_ARCH;
	else
		attrib &= ~EXFAT_ATTRIB_ARCH;

	if (attrib != node->attrib)
	{
		node->attrib = attrib;
		node->is_dirty = true;
		if (exfat_flush_node(&h->ef, node) != 0)
			err = ERROR_DISK_FULL;
	}
	exfat_put_node(&h->ef, node);
	ReplyPkt(pkt, err ? DOSFALSE : DOSTRUE, err);
}

static void do_set_date(struct ExfatHandler* h, struct DosPacket* pkt)
{
	EXFAT_BASES(h);
	struct exfat_node* node;
	struct DateStamp* ds = (struct DateStamp*)BADDR(pkt->dp_Arg4);
	LONG err = 0;

	if (h->ef.ro)
	{
		ReplyPkt(pkt, DOSFALSE, ERROR_DISK_WRITE_PROTECTED);
		return;
	}
	if (ds == NULL)
	{
		ReplyPkt(pkt, DOSFALSE, ERROR_BAD_NUMBER);
		return;
	}
	node = lookup_arg23(h, pkt, &err);
	if (node == NULL)
	{
		ReplyPkt(pkt, DOSFALSE, err);
		return;
	}

	node->mtime = exfat_amiga_datestamp_to_time(ds);
	node->is_dirty = true;
	if (exfat_flush_node(&h->ef, node) != 0)
		err = ERROR_DISK_FULL;
	exfat_put_node(&h->ef, node);
	ReplyPkt(pkt, err ? DOSFALSE : DOSTRUE, err);
}

/* ACTION_SET_COMMENT (28).  exFAT has nowhere to keep an 80 character
   AmigaDOS comment, and silently discarding one loses user data.  But file
   managers set the comment on every copy, usually to the empty string, and
   refusing that turns a perfectly good copy into a "write protected"
   requester.

   So: accept an empty comment, which carries no information and therefore
   loses nothing, and refuse a non-empty one with ERROR_ACTION_NOT_KNOWN -
   which at least says "this file system cannot do that" rather than
   blaming write protection. */
static void do_set_comment(struct ExfatHandler* h, struct DosPacket* pkt)
{
	EXFAT_BASES(h);
	const UBYTE* comment = (const UBYTE*)BADDR(pkt->dp_Arg4);
	struct exfat_node* node;
	LONG err = 0;

	if (h->inhibited || h->ef.ro)
	{
		ReplyPkt(pkt, DOSFALSE, ERROR_DISK_WRITE_PROTECTED);
		return;
	}
	if (comment != NULL && comment[0] != 0)
	{
		Debug_Warn("refusing a %ld character comment: exFAT cannot store "
				"one\n", (LONG)comment[0]);
		ReplyPkt(pkt, DOSFALSE, ERROR_ACTION_NOT_KNOWN);
		return;
	}

	/* Still verify the object exists, so a bad path is reported as such. */
	node = lookup_arg23(h, pkt, &err);
	if (node == NULL)
	{
		ReplyPkt(pkt, DOSFALSE, err);
		return;
	}
	exfat_put_node(&h->ef, node);
	ReplyPkt(pkt, DOSTRUE, 0);
}

/* ACTION_DELETE_OBJECT (16).  Section 13.5.2.  NOTE the argument layout:
   dp_Arg1 is the lock and dp_Arg2 the path, unlike SET_PROTECT/SET_DATE/
   SET_COMMENT which use dp_Arg2 and dp_Arg3. */
/* Directory scans keep a pointer to the next entry in the lock (see
   do_examine_next).  When an entry is removed, any scan sitting on it would
   be left holding a node that is about to be freed, so advance those scans
   past it first.  Section 13.3.2 calls for exactly this: keep the state in
   the lock, and update it when an object leaves the directory. */
static void scan_skip_node(struct ExfatHandler* h, struct exfat_node* node)
{
	EXFAT_BASES(h);
	BPTR b;

	if (h->volume == NULL)
		return;
	for (b = h->volume->dl_LockList; b != 0; )
	{
		struct ExfatLock* l = (struct ExfatLock*)BADDR(b);

		if (l->scan_next == node)
			l->scan_next = node->next;
		b = l->fl.fl_Link;
	}
}

/* A rename can move an object out of the directory being scanned or shuffle
   the list; rather than reason about every case, drop the cached pointers
   and let the affected scans fall back to counting from fib_DiskKey. */
static void scan_invalidate_all(struct ExfatHandler* h)
{
	EXFAT_BASES(h);
	BPTR b;

	if (h->volume == NULL)
		return;
	for (b = h->volume->dl_LockList; b != 0; )
	{
		struct ExfatLock* l = (struct ExfatLock*)BADDR(b);

		l->scan_next = NULL;
		b = l->fl.fl_Link;
	}
}

static void do_delete_object(struct ExfatHandler* h, struct DosPacket* pkt)
{
	EXFAT_BASES(h);
	struct ExfatLock* base = exfat_amiga_lock_from_bptr((BPTR)pkt->dp_Arg1);
	struct exfat_node* node;
	char path[MAX_PATH_UTF8];
	int rc;

	if (h->inhibited || h->ef.ro)
	{
		ReplyPkt(pkt, DOSFALSE, ERROR_DISK_WRITE_PROTECTED);
		return;
	}
	if (!resolve_path(h, base, (const UBYTE*)BADDR(pkt->dp_Arg2),
			path, sizeof(path)))
	{
		ReplyPkt(pkt, DOSFALSE, ERROR_OBJECT_NOT_FOUND);
		return;
	}
	rc = exfat_lookup(&h->ef, &node, path);
	if (rc != 0)
	{
		ReplyPkt(pkt, DOSFALSE, map_errno(rc));
		return;
	}
	if (node == h->ef.root)
	{
		exfat_put_node(&h->ef, node);
		ReplyPkt(pkt, DOSFALSE, ERROR_OBJECT_WRONG_TYPE);
		return;
	}
	/* Our lookup holds one reference; anything above that means a lock or
	   an open file still refers to the object.  libexfat would happily
	   unlink it and free the node once the last reference went away, but
	   AmigaDOS programs expect ERROR_OBJECT_IN_USE, and our locks hold raw
	   node pointers that must not be left dangling. */
	if (node->references > 1)
	{
		exfat_put_node(&h->ef, node);
		ReplyPkt(pkt, DOSFALSE, ERROR_OBJECT_IN_USE);
		return;
	}

	/* exfat_rmdir() refuses a non-empty directory, which is what section
	   13.5.2 requires (ERROR_DIRECTORY_NOT_EMPTY via map_errno). */
	scan_skip_node(h, node);
	if (node->attrib & EXFAT_ATTRIB_DIR)
		rc = exfat_rmdir(&h->ef, node);
	else
		rc = exfat_unlink(&h->ef, node);
	exfat_put_node(&h->ef, node);
	if (rc != 0)
	{
		ReplyPkt(pkt, DOSFALSE, map_errno(rc));
		return;
	}
	/* Frees the node and releases its clusters now that nothing holds it.
	   Deliberately NOT followed by exfat_flush(): that rewrites the whole
	   allocation bitmap, which on a 238 GB volume is ~122 KB, and doing it
	   per operation makes bulk deletes crawl.  The bitmap is written at the
	   explicit sync points instead - ACTION_FLUSH, INHIBIT and DIE - which
	   is what the FUSE front end does too.  VolumeDirty is set for the whole
	   session, so an unclean shutdown is flagged for checking. */
	rc = exfat_cleanup_node(&h->ef, node);
	Debug_Info("deleted %s\n", path);
	ReplyPkt(pkt, rc ? DOSFALSE : DOSTRUE, rc ? map_errno(rc) : 0);
}

/* ACTION_CREATE_DIR (22).  Section 13.2.6: returns an exclusive lock on the
   new directory, not a boolean. */
static void do_create_dir(struct ExfatHandler* h, struct DosPacket* pkt)
{
	EXFAT_BASES(h);
	struct ExfatLock* base = exfat_amiga_lock_from_bptr((BPTR)pkt->dp_Arg1);
	struct ExfatLock* lock;
	struct exfat_node* node;
	char path[MAX_PATH_UTF8];
	int rc;

	if (h->inhibited || h->ef.ro)
	{
		ReplyPkt(pkt, 0, ERROR_DISK_WRITE_PROTECTED);
		return;
	}
	if (!resolve_path(h, base, (const UBYTE*)BADDR(pkt->dp_Arg2),
			path, sizeof(path)))
	{
		ReplyPkt(pkt, 0, ERROR_OBJECT_NOT_FOUND);
		return;
	}
	rc = exfat_mkdir(&h->ef, path);
	if (rc == 0)
		rc = exfat_lookup(&h->ef, &node, path);
	if (rc != 0)
	{
		ReplyPkt(pkt, 0, map_errno(rc));
		return;
	}
	lock = exfat_amiga_makelock(h, node, EXCLUSIVE_LOCK);
	if (lock == NULL)
	{
		exfat_put_node(&h->ef, node);
		ReplyPkt(pkt, 0, ERROR_NO_FREE_STORE);
		return;
	}
	/* bitmap flush deferred to the next sync point - see do_delete_object */
	Debug_Info("created directory %s\n", path);
	ReplyPkt(pkt, (LONG)MKBADDR(lock), 0);
}

/* ACTION_RENAME_OBJECT (17).  Section 13.5.1: source lock and path in
   dp_Arg1/dp_Arg2, target lock and path in dp_Arg3/dp_Arg4.  Renaming and
   moving are the same operation. */
static void do_rename_object(struct ExfatHandler* h, struct DosPacket* pkt)
{
	EXFAT_BASES(h);
	struct ExfatLock* srcbase = exfat_amiga_lock_from_bptr((BPTR)pkt->dp_Arg1);
	struct ExfatLock* dstbase = exfat_amiga_lock_from_bptr((BPTR)pkt->dp_Arg3);
	char srcpath[MAX_PATH_UTF8];
	char dstpath[MAX_PATH_UTF8];
	int rc;

	if (h->inhibited || h->ef.ro)
	{
		ReplyPkt(pkt, DOSFALSE, ERROR_DISK_WRITE_PROTECTED);
		return;
	}
	if (!resolve_path(h, srcbase, (const UBYTE*)BADDR(pkt->dp_Arg2),
				srcpath, sizeof(srcpath)) ||
			!resolve_path(h, dstbase, (const UBYTE*)BADDR(pkt->dp_Arg4),
				dstpath, sizeof(dstpath)))
	{
		ReplyPkt(pkt, DOSFALSE, ERROR_OBJECT_NOT_FOUND);
		return;
	}

	rc = exfat_rename(&h->ef, srcpath, dstpath);
	if (rc != 0)
	{
		Debug_Warn("rename %s -> %s failed: %ld\n", srcpath, dstpath,
				(LONG)rc);
		ReplyPkt(pkt, DOSFALSE, map_errno(rc));
		return;
	}
	scan_invalidate_all(h);
	/* bitmap flush deferred to the next sync point - see do_delete_object */
	Debug_Info("renamed %s -> %s\n", srcpath, dstpath);
	ReplyPkt(pkt, DOSTRUE, 0);
}

/* ACTION_RENAME_DISK (9).  Section 13.7.4: change the volume label, and
   "also change the name of the DosList representing the volume in the device
   list, i.e. adjust the dol_Name element".

   The name MakeDosEntry() allocated is sized for the original label, so a
   longer one will not fit in place.  Instead point dl_Name at a buffer of our
   own and swap it under Forbid(), keeping the original so remove_volume() can
   put it back before FreeDosEntry() disposes of it.  Swapping the pointer
   rather than replacing the volume node keeps outstanding locks valid - their
   fl_Volume still refers to the same DeviceList. */
static void do_rename_disk(struct ExfatHandler* h, struct DosPacket* pkt)
{
	EXFAT_BASES(h);
	const UBYTE* bname = (const UBYTE*)BADDR(pkt->dp_Arg1);
	char label[EXFAT_UTF8_ENAME_BUFFER_MAX];
	int rc;

	if (h->inhibited || h->ef.ro)
	{
		ReplyPkt(pkt, DOSFALSE, ERROR_DISK_WRITE_PROTECTED);
		return;
	}
	if (bname == NULL || bname[0] == 0)
	{
		ReplyPkt(pkt, DOSFALSE, ERROR_INVALID_COMPONENT_NAME);
		return;
	}

	exfat_amiga_bstr_to_utf8(bname, label, sizeof(label));
	rc = exfat_set_label(&h->ef, label);
	if (rc != 0)
	{
		Debug_Warn("relabel to \"%s\" failed: %ld\n", label, (LONG)rc);
		ReplyPkt(pkt, DOSFALSE, map_errno(rc));
		return;
	}

	if (h->volume != NULL)
	{
		UBYTE len = bname[0];
		UBYTE* newname = AllocVec((ULONG)len + 2, MEMF_PUBLIC | MEMF_CLEAR);

		if (newname != NULL)
		{
			BSTR previous;

			newname[0] = len;
			memcpy(newname + 1, bname + 1, len);

			Forbid();
			if (h->volname_orig == 0)
				h->volname_orig = h->volume->dl_Name;
			previous = h->volname;
			h->volname = MKBADDR(newname);
			h->volume->dl_Name = h->volname;
			Permit();

			if (previous != 0)
				FreeVec(BADDR(previous));
		}
		else
			Debug_Warn("relabelled on disk, but dl_Name not updated\n");
	}

	Debug_Info("volume relabelled to \"%s\"\n", label);
	ReplyPkt(pkt, DOSTRUE, 0);
}


/* Set or clear exFAT's VolumeDirty flag without unmounting.  This is what
   finalize_super_block() does internally; it touches only VolumeFlags, which
   the boot checksum excludes (spec section 3.4), so nothing else has to be
   recomputed.  Doing it without unmounting matters because a full unmount
   would free the cached nodes that outstanding locks still point at. */
static LONG set_volume_dirty(struct ExfatHandler* h, BOOL dirty)
{
	EXFAT_BASES(h);
	uint16_t state;

	if (h->ef.ro)
		return 0;

	state = le16_to_cpu(h->ef.sb->volume_state);
	if (dirty)
		state |= EXFAT_STATE_MOUNTED;
	else
		state &= ~EXFAT_STATE_MOUNTED;
	h->ef.sb->volume_state = cpu_to_le16(state);

	if (exfat_pwrite(h->ef.dev, h->ef.sb,
			sizeof(struct exfat_super_block), 0) < 0)
		return ERROR_DISK_FULL;
	if (exfat_fsync(h->ef.dev) != 0)
		return ERROR_DISK_FULL;
	return 0;
}

/* ACTION_INHIBIT (31).  Section 13.9.2: an inhibited file system stops
   touching the medium and reports 'BUSY'.  Used here as the safe way to park
   a volume when ACTION_DIE is refused because locks are still outstanding -
   flush everything and clear the dirty flag, so the card can be removed and
   will not need checking on the next host.

   NOTE: this is a partial implementation.  Writes are refused while
   inhibited, but reads are not blocked, and uninhibiting does not re-validate
   the medium as the section requires.  Do not use it to hand the raw device
   to another program. */
static void do_inhibit(struct ExfatHandler* h, struct DosPacket* pkt)
{
	EXFAT_BASES(h);
	BOOL on = (pkt->dp_Arg1 != 0) ? TRUE : FALSE;
	LONG err = 0;

	if (on && !h->inhibited)
	{
		if (h->nlocks == 0 && h->nfiles == 0)
		{
			/* Nothing is holding the volume, so let go of it completely.
			   This is what section 13.9.2 means by simulating a medium
			   change, and ACTION_FORMAT requires it: the on-disk structure
			   is about to be replaced, so no cached state may survive. */
			remove_volume(h);
			unmount_fs(h);
		}
		else
		{
			/* Something still holds a lock or an open file, so a full
			   unmount would leave those pointing at freed nodes.  Park the
			   volume instead: flush and clear the dirty flag so the card is
			   safe to remove, but keep the mount in memory. */
			if (!h->ef.ro)
			{
				if (exfat_flush_nodes(&h->ef) != 0 ||
						exfat_flush(&h->ef) != 0)
					err = ERROR_DISK_FULL;
				if (err == 0)
					err = set_volume_dirty(h, FALSE);
			}
			if (err == 0)
				Debug_Info("inhibited (parked: %lu locks, %lu files still "
						"open, so the volume stays mounted)\n",
						(ULONG)h->nlocks, (ULONG)h->nfiles);
		}
		if (err == 0)
		{
			h->inhibited = TRUE;
			Debug_Info("inhibited: flushed and marked clean, safe to remove "
					"the medium\n");
		}
	}
	else if (!on && h->inhibited)
	{
		/* Re-validate as if the medium had been re-inserted (13.9.2): after
		   ACTION_FORMAT the structure on disk is a different volume. */
		if (!h->mounted)
		{
			if (!mount_fs(h))
			{
				Debug_Error("could not re-mount after uninhibit\n");
				ReplyPkt(pkt, DOSFALSE, ERROR_NOT_A_DOS_DISK);
				return;
			}
			add_volume(h);
		}
		else
			err = set_volume_dirty(h, TRUE);

		if (err == 0)
		{
			h->inhibited = FALSE;
			Debug_Info("uninhibited\n");
		}
	}
	ReplyPkt(pkt, err ? DOSFALSE : DOSTRUE, err);
}


/* ACTION_FORMAT (1020).  Section 13.7.5: writes a blank file system onto the
   partition.  Only legal while inhibited - and this build additionally
   requires the volume to be fully unmounted, which do_inhibit() does when
   nothing holds it open.  The volume node reappears on uninhibit. */
static void do_format(struct ExfatHandler* h, struct DosPacket* pkt)
{
	EXFAT_BASES(h);
	const UBYTE* blabel = (const UBYTE*)BADDR(pkt->dp_Arg1);
	char label[EXFAT_UTF8_ENAME_BUFFER_MAX];

	if (h->ef.ro && h->mounted)
	{
		ReplyPkt(pkt, DOSFALSE, ERROR_DISK_WRITE_PROTECTED);
		return;
	}
	if (!h->inhibited || h->mounted)
	{
		/* Refusing here is the main safeguard against formatting a volume
		   that is still in use. */
		Debug_Error("refusing ACTION_FORMAT: the file system must be "
				"inhibited and fully released first (%lu locks, %lu files)\n",
				(ULONG)h->nlocks, (ULONG)h->nfiles);
		ReplyPkt(pkt, DOSFALSE, ERROR_OBJECT_IN_USE);
		return;
	}

	label[0] = '\0';
	if (blabel != NULL && blabel[0] != 0)
		exfat_amiga_bstr_to_utf8(blabel, label, sizeof(label));

	Debug_Warn("FORMATTING %s unit %lu - all data will be lost\n",
			h->spec.devname, (ULONG)h->spec.unit);

	if (exfat_amiga_format(&h->spec, label) != 0)
	{
		ReplyPkt(pkt, DOSFALSE, ERROR_NOT_A_DOS_DISK);
		return;
	}
	ReplyPkt(pkt, DOSTRUE, 0);
}


static void do_end(struct ExfatHandler* h, struct DosPacket* pkt)
{
	EXFAT_BASES(h);
	struct ExfatFile* file = (struct ExfatFile*)pkt->dp_Arg1;

	LONG err = 0;

	if (file != NULL)
	{
		/* Commit the directory entry before letting go of the file: size
		   and timestamps live there, not in the data clusters. */
		if (!h->ef.ro)
		{
			if (exfat_flush_node(&h->ef, file->node) != 0)
				err = ERROR_DISK_FULL;
		}
		exfat_put_node(&h->ef, file->node);
		FreeVec(file);
		h->nfiles--;
	}
	ReplyPkt(pkt, err ? DOSFALSE : DOSTRUE, err);
}

static void do_read(struct ExfatHandler* h, struct DosPacket* pkt)
{
	EXFAT_BASES(h);
	struct ExfatFile* file = (struct ExfatFile*)pkt->dp_Arg1;
	void* buffer = (void*)pkt->dp_Arg2;
	LONG length = pkt->dp_Arg3;
	ssize_t n;

	if (file == NULL || length < 0)
	{
		ReplyPkt(pkt, -1, ERROR_INVALID_LOCK);
		return;
	}
	if (length == 0)
	{
		ReplyPkt(pkt, 0, 0);
		return;
	}

	n = exfat_generic_pread(&h->ef, file->node, buffer, (size_t)length,
			file->pos);
	if (n < 0)
	{
		ReplyPkt(pkt, -1, ERROR_SEEK_ERROR);
		return;
	}
	file->pos += n;
	ReplyPkt(pkt, (LONG)n, 0);
}

static void do_seek(struct ExfatHandler* h, struct DosPacket* pkt)
{
	EXFAT_BASES(h);
	struct ExfatFile* file = (struct ExfatFile*)pkt->dp_Arg1;
	LONG offset = pkt->dp_Arg2;
	LONG mode = pkt->dp_Arg3;
	exfat_off_t oldpos;
	exfat_off_t newpos;

	if (file == NULL)
	{
		ReplyPkt(pkt, -1, ERROR_INVALID_LOCK);
		return;
	}
	oldpos = file->pos;

	/* The 64-bit interpretation recommended by section 13.1.8: zero-extend
	   for OFFSET_BEGINNING, sign-extend for OFFSET_CURRENT, and for
	   OFFSET_END zero-extend the negated value then negate in 64 bits. */
	switch (mode)
	{
	case OFFSET_BEGINNING:
		newpos = (exfat_off_t)(uint64_t)(uint32_t)offset;
		break;
	case OFFSET_CURRENT:
		newpos = oldpos + (exfat_off_t)offset;
		break;
	case OFFSET_END:
		newpos = (exfat_off_t)file->node->size -
				(exfat_off_t)(uint64_t)(uint32_t)(-offset);
		break;
	default:
		ReplyPkt(pkt, -1, ERROR_SEEK_ERROR);
		return;
	}

	if (newpos < 0 || (uint64_t)newpos > file->node->size)
	{
		ReplyPkt(pkt, -1, ERROR_SEEK_ERROR);
		return;
	}
	file->pos = newpos;
	/* Only the low 32 bits of the previous position can be reported. */
	ReplyPkt(pkt, (LONG)(uint32_t)(uint64_t)oldpos, 0);
}

static void fill_infodata(struct ExfatHandler* h, struct InfoData* id)
{
	EXFAT_BASES(h);
	uint32_t total = le32_to_cpu(h->ef.sb->cluster_count);
	uint32_t freec = exfat_count_free_clusters(&h->ef);

	memset(id, 0, sizeof(struct InfoData));
	id->id_NumSoftErrors = 0;
	id->id_UnitNumber = (LONG)h->spec.unit;
	id->id_DiskState = h->ef.ro ? ID_WRITE_PROTECTED : ID_VALIDATED;
	id->id_NumBlocks = (LONG)total;
	id->id_NumBlocksUsed = (LONG)(total - freec);
	id->id_BytesPerBlock = (LONG)CLUSTER_SIZE(*h->ef.sb);
	/* We mounted the volume, so we claim it: the generic ID_DOS_DISK.  While
	   inhibited, Table 5.6 calls for 'BUSY' instead. */
	id->id_DiskType = h->inhibited ? EXFAT_ID_BUSY : ID_DOS_DISK;
	id->id_VolumeNode = MKBADDR(h->volume);
	id->id_InUse = (h->nlocks != 0 || h->nfiles != 0) ? DOSTRUE : DOSFALSE;

	Debug_Info("InfoData: state %ld blocks %ld used %ld bytes/block %ld "
			"type %08lx volnode %08lx inuse %ld locks %lu files %lu\n",
			(LONG)id->id_DiskState, (LONG)id->id_NumBlocks,
			(LONG)id->id_NumBlocksUsed, (LONG)id->id_BytesPerBlock,
			(ULONG)id->id_DiskType, (ULONG)id->id_VolumeNode,
			(LONG)id->id_InUse, (ULONG)h->nlocks, (ULONG)h->nfiles);
}

/* ------------------------------------------------------------------ */
/* Partition discovery                                                */
/* ------------------------------------------------------------------ */

/* Build a device name for an extra partition from our own: "EXF0" gives
   "EXF1", "EXF2"; a name with no trailing digit simply gets one appended. */
static void sibling_name(struct ExfatHandler* h, char* out, int size, int idx)
{
	EXFAT_BASES(h);
	const UBYTE* mine = (const UBYTE*)BADDR(h->devlist->dol_Name);
	int len = (mine != NULL) ? mine[0] : 0;
	int i;

	if (len == 0 || len > size - 4)
	{
		exfat_amiga_snprintf(out, size, "EXF%ld", (LONG)idx);
		return;
	}
	for (i = 0; i < len; i++)
		out[i] = (char)mine[1 + i];
	/* drop a trailing run of digits, then append the index */
	while (i > 0 && out[i - 1] >= '0' && out[i - 1] <= '9')
		i--;
	out[i] = '\0';
	exfat_amiga_snprintf(out + i, size - i, "%ld", (LONG)idx);
}

/* Publish a DOS device node for a partition this process will not serve.
   dn_Task is left NULL, so AmigaDOS starts a separate handler process for it
   the first time something touches the name - and that process receives a
   FileSysStartupMsg describing a real partition, so it never parses anything.

   Geometry is one block per cylinder (Surfaces = BlocksPerTrack = 1) so
   LowCyl/HighCyl are plain LBAs and any partition range is expressible. */
static BOOL publish_partition(struct ExfatHandler* h, int idx,
		const struct ExfatPartition* part)
{
	EXFAT_BASES(h);
	struct ExpansionBase* ExpansionBase;
	struct DeviceNode* node;
	char name[32];
	ULONG pp[4 + DE_BOOTBLOCKS + 1];

	int attempt;

	ExpansionBase = (struct ExpansionBase*)OpenLibrary("expansion.library", 36);
	if (ExpansionBase == NULL)
	{
		Debug_Error("cannot open expansion.library\n");
		return FALSE;
	}

	/* The name may already be taken - sagasd.device calls unit 0's card
	   SDROM0, so our siblings SDROM1, SDROM2 collide with units 1 and 2.
	   Walk forward until a free name is found rather than losing the
	   partition. */
	for (attempt = 0; attempt < 32; attempt++)
	{
	sibling_name(h, name, sizeof(name), h->next_name_idx + attempt);

	memset(pp, 0, sizeof(pp));
	pp[0] = (ULONG)name;
	pp[1] = (ULONG)h->spec.devname;
	pp[2] = h->spec.unit;
	pp[3] = h->spec.flags;
	pp[DE_TABLESIZE + 4]    = DE_BOOTBLOCKS;
	pp[DE_SIZEBLOCK + 4]    = h->spec.blocksize >> 2;
	pp[DE_NUMHEADS + 4]     = 1;
	pp[DE_SECSPERBLK + 4]   = 1;
	pp[DE_BLKSPERTRACK + 4] = 1;
	pp[DE_RESERVEDBLKS + 4] = 0;
	pp[DE_LOWCYL + 4]       = (ULONG)part->first_lba;
	pp[DE_UPPERCYL + 4]     = (ULONG)(part->first_lba + part->sectors - 1);
	pp[DE_NUMBUFFERS + 4]   = 100;
	pp[DE_BUFMEMTYPE + 4]   = MEMF_PUBLIC;
	pp[DE_MAXTRANSFER + 4]  = h->spec.maxtransfer;
	pp[DE_MASK + 4]         = h->spec.mask;
	pp[DE_BOOTPRI + 4]      = 0;
	pp[DE_DOSTYPE + 4]      = EXFAT_DOSTYPE;
	pp[DE_BOOTBLOCKS + 4]   = 0;

	node = (struct DeviceNode*)MakeDosNode(pp);
	if (node == NULL)
	{
		Debug_Error("MakeDosNode failed for %s\n", name);
		CloseLibrary((struct Library*)ExpansionBase);
		return FALSE;
	}

	/* Tell AmigaDOS how to start a handler for this node.

	   Prefer the FileSystem.resource entry for our DOSType: that is how a
	   ROM-resident handler is reached, and it is what fat95 does.  Every
	   partition then runs from the same seglist, which is safe only because
	   this handler holds no per-instance state in globals - see the note in
	   exfat_handler_main().  Do not reintroduce one.

	   Failing that, load a private copy from the path we came from. */
	{
		struct FileSysResource* fsr;
		struct FileSysEntry* fse = NULL;
		BSTR path = h->devlist->dol_misc.dol_handler.dol_Handler;

		Forbid();
		fsr = (struct FileSysResource*)OpenResource("FileSystem.resource");
		if (fsr != NULL)
		{
			struct FileSysEntry* e;

			for (e = (struct FileSysEntry*)fsr->fsr_FileSysEntries.lh_Head;
					e->fse_Node.ln_Succ != NULL;
					e = (struct FileSysEntry*)e->fse_Node.ln_Succ)
				if (e->fse_DosType == EXFAT_DOSTYPE)
				{
					fse = e;
					break;
				}
		}
		Permit();

		if (fse != NULL)
		{
			node->dn_SegList = fse->fse_SegList;
			if (fse->fse_PatchFlags & FSEF_GLOBALVEC)
				node->dn_GlobalVec = fse->fse_GlobalVec;
			Debug_Info("  %s uses the resident exFAT handler (seglist %08lx)\n",
					name, (ULONG)fse->fse_SegList);
		}
		else if (path != 0)
		{
			node->dn_Handler = path;
			Debug_Info("  %s loads its own copy of \"%b\"\n", name,
					(ULONG)path);
		}
		else
		{
			Debug_Error("  %s cannot be published: no resident exFAT handler "
					"and no path to load one from\n", name);
			FreeDosEntry((struct DosList*)node);
			CloseLibrary((struct Library*)ExpansionBase);
			return FALSE;
		}
	}
	node->dn_StackSize = (h->devlist->dol_misc.dol_handler.dol_StackSize != 0) ?
			h->devlist->dol_misc.dol_handler.dol_StackSize : EXFAT_STACKSIZE;
	node->dn_Priority = h->devlist->dol_misc.dol_handler.dol_Priority;
	/* dn_GlobalVec is NOT reset from the parent here: when the resident
	   handler was found above, the FileSysEntry's value (-1, meaning "not
	   BCPL") has already been written and must survive.  Inheriting the
	   parent's is only right on the load-from-disk path. */
	if (node->dn_SegList == 0)
		node->dn_GlobalVec = h->devlist->dol_misc.dol_handler.dol_GlobVec;

	if (AddDosEntry((struct DosList*)node) == DOSFALSE)
	{
		/* Almost always "that name exists"; try the next one. */
		Debug_Info("  %s is taken (IoErr %ld), trying the next name\n", name,
				(LONG)IoErr());
		FreeDosEntry((struct DosList*)node);
		continue;
	}
	CloseLibrary((struct Library*)ExpansionBase);
	h->next_name_idx += attempt + 1;
	if (h->npublished < EXFAT_MAX_PARTITIONS)
	{
		exfat_amiga_snprintf(h->published[h->npublished],
				sizeof(h->published[0]), "%s:", name);
		h->npublished++;
	}
	Debug_Info("published %s: LBA %lu, %lu sectors, stack %lu, globvec %ld\n",
			name, (ULONG)part->first_lba, (ULONG)part->sectors,
			(ULONG)node->dn_StackSize, (LONG)node->dn_GlobalVec);
	return TRUE;
	}

	CloseLibrary((struct Library*)ExpansionBase);
	Debug_Error("no free device name found for the partition at LBA %lu\n",
			(ULONG)part->first_lba);
	return FALSE;
}

/* Decide what we were actually given.  If an exFAT boot sector sits at the
   start of our extent we were handed a partition and use it as-is - which is
   also what every node published above will see.  Otherwise look for a
   partition table, take the first exFAT partition for ourselves and publish
   nodes for the rest. */
static BOOL resolve_extent(struct ExfatHandler* h)
{
	EXFAT_BASES(h);
	struct ExfatPartition parts[EXFAT_MAX_PARTITIONS];
	struct exfat_dev* dev;
	uint64_t total_sectors;
	int n;
	int i;

	/* A non-zero de_LowCyl means we were handed a partition, not a device -
	   which is the case for every node published below, and for any mount
	   file naming a specific partition.  Skip the scan entirely: it would
	   only open the device a second time, re-probe NSD and read one sector
	   to conclude what the mount file already told us. */
	if (h->lowcyl != 0)
	{
		Debug_Info("mounted on a partition (LowCyl %lu), not scanning for a "
				"partition table\n", (ULONG)h->lowcyl);
		return TRUE;
	}
	dev = exfat_open((const char*)&h->spec, EXFAT_MODE_RO);
	if (dev == NULL)
		return FALSE;

	total_sectors = h->spec.length / (uint64_t)h->spec.blocksize;
	n = exfat_amiga_scan_partitions(dev, h->spec.blocksize, total_sectors,
			parts, EXFAT_MAX_PARTITIONS);
	exfat_close(dev);

	if (n == 0)
		return FALSE;

	/* A superfloppy, or a partition handed to us directly, comes back as a
	   single entry at LBA 0 covering everything - nothing to adjust. */
	if (n == 1 && parts[0].first_lba == 0)
		return TRUE;

	Debug_Info("%ld exFAT partition%s on this medium; serving the first\n",
			(LONG)n, (n == 1) ? "" : "s");

	/* Narrow our own extent to the first partition. */
	h->spec.firstbyte += parts[0].first_lba * (uint64_t)h->spec.blocksize;
	h->spec.length = parts[0].sectors * (uint64_t)h->spec.blocksize;

	/* The rest become device nodes of their own. */
	for (i = 1; i < n; i++)
		publish_partition(h, i, &parts[i]);

	return TRUE;
}

/* ------------------------------------------------------------------ */
/* One handler per partition                                          */
/* ------------------------------------------------------------------ */

/* A card can end up mounted twice - once from a DOSDrivers mount file and
   once by sagasd.device's own auto-mount - which would leave two handler
   processes writing the same partition, each with its own node cache,
   allocation bitmap and dirty flag.  That corrupts the volume.

   Claim the partition with a public semaphore named for the device, unit and
   starting block.  A named semaphore needs no DosList lock, so unlike
   walking the device list this is safe during startup, when GetDeviceProc()
   holds that lock (section 12.1.2). */
static UNUSED BOOL claim_partition(struct ExfatHandler* h)
{
	EXFAT_BASES(h);
	char name[80];
	struct SignalSemaphore* sem;
	int len;

	exfat_amiga_snprintf(name, sizeof(name), "exfat/%s/%lu/%lu:%lu",
			h->spec.devname, (ULONG)h->spec.unit,
			(ULONG)(h->spec.firstbyte >> 32),
			(ULONG)(h->spec.firstbyte & 0xFFFFFFFFUL));

	Forbid();
	if (FindSemaphore((STRPTR)name) != NULL)
	{
		Permit();
		Debug_Error("this partition is already served by another handler "
				"(%s).  Refusing to mount it twice: two handlers writing one "
				"volume will corrupt it.  Mount it from the DOSDrivers file "
				"OR let sagasd.device auto-mount it, not both.\n", name);
		return FALSE;
	}

	len = (int)strlen(name) + 1;
	sem = AllocVec(sizeof(struct SignalSemaphore) + len,
			MEMF_PUBLIC | MEMF_CLEAR);
	if (sem == NULL)
	{
		Permit();
		return FALSE;		/* out of memory: safer not to mount */
	}
	sem->ss_Link.ln_Name = (char*)(sem + 1);
	strcpy(sem->ss_Link.ln_Name, name);
	sem->ss_Link.ln_Pri = 0;
	InitSemaphore(sem);
	AddSemaphore(sem);
	Permit();

	h->claim = sem;
	Debug_Info("claimed %s\n", name);
	return TRUE;
}

static void release_partition(struct ExfatHandler* h)
{
	EXFAT_BASES(h);
	if (h->claim == NULL)
		return;
	Forbid();
	RemSemaphore(h->claim);
	Permit();
	FreeVec(h->claim);
	h->claim = NULL;
}

/* ------------------------------------------------------------------ */
/* Startup                                                            */
/* ------------------------------------------------------------------ */

static BOOL startup(struct ExfatHandler* h, struct DosPacket* pkt)
{
	EXFAT_BASES(h);
	struct FileSysStartupMsg* fssm;
	struct DosEnvec* de;
	const char* devname;
	uint64_t blocks_per_cyl;
	uint64_t first_block;
	uint64_t block_count;

	h->devlist = (struct DosList*)BADDR(pkt->dp_Arg3);
	fssm = (struct FileSysStartupMsg*)BADDR(pkt->dp_Arg2);
	if (fssm == NULL)
	{
		/* dol_Startup was ZERO.  Almost always a mountlist that says
		   HANDLER= instead of FILESYSTEM=: only FILESYSTEM (or EHANDLER)
		   makes Mount build a FileSysStartupMsg from DEVICE/UNIT/FLAGS.
		   See AmigaDOS RKM sections 7.1.1 and 7.1.2. */
		Debug_Error("dol_Startup is ZERO - no FileSysStartupMsg. Use "
				"FileSystem = L:exfat-handler in the mountlist, not "
				"Handler =\n");
		return FALSE;
	}
	de = (struct DosEnvec*)BADDR(fssm->fssm_Environ);
	if (de == NULL)
	{
		Debug_Error("no DosEnvec\n");
		return FALSE;
	}

	devname = (const char*)BADDR(fssm->fssm_Device);
	/* fssm_Device is a BSTR; step past the length byte.  It is NUL
	   terminated as well, which is what OpenDevice() needs. */
	devname += 1;

	h->spec.devname = devname;
	h->spec.unit = fssm->fssm_Unit;
	h->spec.flags = fssm->fssm_Flags;
	h->spec.blocksize = de->de_SizeBlock * 4;
	h->spec.maxtransfer = de->de_MaxTransfer;
	h->spec.mask = de->de_Mask;

	/* The partition extent comes from the DosEnvec and nothing else - no
	   MBR, GPT or RDB parsing here, and the exFAT boot sector's own
	   PartitionOffset is ignored.  See ../CLAUDE.md, decision 1. */
	blocks_per_cyl = (uint64_t)de->de_Surfaces * (uint64_t)de->de_BlocksPerTrack;
	first_block = (uint64_t)de->de_LowCyl * blocks_per_cyl;
	block_count = ((uint64_t)de->de_HighCyl - (uint64_t)de->de_LowCyl + 1) *
			blocks_per_cyl;

	h->lowcyl = de->de_LowCyl;
	h->next_name_idx = 1;
	h->spec.firstbyte = first_block * (uint64_t)h->spec.blocksize;
	h->spec.length = block_count * (uint64_t)h->spec.blocksize;

	Debug_Info("mounting %s unit %lu: lowcyl %lu highcyl %lu, %lu blocks of "
			"%lu bytes\n", devname, (ULONG)h->spec.unit,
			(ULONG)de->de_LowCyl, (ULONG)de->de_HighCyl,
			(ULONG)block_count, (ULONG)h->spec.blocksize);

	/* We may have been mounted on a whole device rather than a partition;
	   work out which, and publish nodes for any other exFAT partitions. */
	if (!resolve_extent(h))
	{
		Debug_Error("no exFAT volume found on this device or partition\n");
		return FALSE;
	}

	/* The partition claim is deliberately NOT taken here.

	   claim_partition() refuses to be the second handler on a partition,
	   which was the backstop against a card being mounted twice - once from
	   a DOSDrivers mountlist and once by sagasd.device's auto-mount - with
	   two handlers each keeping their own node cache, allocation bitmap and
	   dirty flag.  The mountlist route has since been removed, so
	   sagasd.device is the only mounter and there is nothing to race.

	   claim_partition() and release_partition() are kept compiled and
	   type-checked so the guard can be put back by restoring this call;
	   release_partition() is a no-op while h->claim is NULL. */

	/* Route every subsequent packet for this device to us (12.1.2). */
	h->devlist->dol_Task = h->port;

	return mount_fs(h);
}

/* Mount (or re-mount) the volume.  Split out of startup() because
   ACTION_FORMAT requires the volume to be torn down and brought back, and
   ACTION_INHIBIT does the same when nothing is holding it open. */
static BOOL mount_fs(struct ExfatHandler* h)
{
	EXFAT_BASES(h);
	char label[EXFAT_UTF8_ENAME_BUFFER_MAX];

	/* "ro_fallback" asks for read-write but accepts read-only if the medium
	   or the build will not allow it; exfat_mount() then sets ef.ro. */
	if (exfat_mount(&h->ef, (const char*)&h->spec, "ro_fallback,noatime") != 0)
	{
		Debug_Error("exfat_mount failed\n");
		return FALSE;
	}
	h->mounted = TRUE;

	/* Mark the volume dirty for the whole session, as the FUSE front end
	   does.  The two fields this touches - VolumeFlags and PercentInUse -
	   are exactly the ones the boot checksum excludes (spec section 3.4),
	   so no checksum recomputation is needed.  It is cleared again by
	   exfat_unmount(); an unclean shutdown deliberately leaves it set so
	   the volume gets checked on the next host mount. */
	if (!h->ef.ro)
	{
		if (exfat_soil_super_block(&h->ef) != 0)
			Debug_Warn("could not mark the volume dirty\n");
		else
			Debug_Info("volume mounted read-write and marked dirty\n");
	}
	else
		Debug_Info("volume mounted read-only\n");

	exfat_amiga_cstr_to_utf8(exfat_get_label(&h->ef), label, sizeof(label));
	Debug_Info("mounted, label \"%s\"\n", exfat_get_label(&h->ef));

	/* Superblock geometry, so a bad Dir/List can be told apart from a bad
	   mount at a glance.  If these look wrong, the byte swapping is wrong;
	   if they look right, suspect the directory code instead. */
	Debug_Info("volume: %lu byte sectors, %lu byte clusters, %lu clusters "
			"total, %lu free, root at cluster %lu\n",
			(ULONG)SECTOR_SIZE(*h->ef.sb),
			(ULONG)CLUSTER_SIZE(*h->ef.sb),
			(ULONG)le32_to_cpu(h->ef.sb->cluster_count),
			(ULONG)exfat_count_free_clusters(&h->ef),
			(ULONG)le32_to_cpu(h->ef.sb->rootdir_cluster));
	return TRUE;
}

/* Create and publish the DLT_VOLUME entry.  This must happen *after* the
   startup packet has been replied: GetDeviceProc() holds the device list
   locked until then, so adding an entry from within startup deadlocks
   (12.1.2). */
static void add_volume(struct ExfatHandler* h)
{
	EXFAT_BASES(h);
	const char* label = exfat_get_label(&h->ef);
	char name[64];
	struct DeviceList* vol;

	if (label == NULL || label[0] == '\0')
		label = "Untitled";
	exfat_amiga_cstr_to_utf8(label, name, sizeof(name));
	/* MakeDosEntry wants an 8-bit C string, which is what we display. */
	{
		int i = 0;
		const char* s = label;

		while (*s != '\0' && i < (int)sizeof(name) - 1)
			name[i++] = *s++;
		name[i] = '\0';
	}

	vol = (struct DeviceList*)MakeDosEntry(name, DLT_VOLUME);
	if (vol == NULL)
	{
		Debug_Warn("MakeDosEntry failed; volume not published\n");
		return;
	}
	vol->dl_Task = h->port;
	vol->dl_DiskType = ID_DOS_DISK;		/* not the file system's DOSType */
	DateStamp(&vol->dl_VolumeDate);

	if (AddDosEntry((struct DosList*)vol) == DOSFALSE)
	{
		Debug_Warn("AddDosEntry failed, IoErr %ld\n", (LONG)IoErr());
		FreeDosEntry((struct DosList*)vol);
		return;
	}
	h->volume = vol;
	Debug_Info("volume \"%s\" published\n", name);
}

/* Flush, clear the dirty flag and let go of the medium entirely.  Only safe
   with no outstanding locks or open files: exfat_unmount() resets the node
   cache, and our locks hold raw node pointers. */
static void unmount_fs(struct ExfatHandler* h)
{
	EXFAT_BASES(h);
	if (!h->mounted)
		return;
	exfat_unmount(&h->ef);		/* flushes and clears VolumeDirty */
	h->mounted = FALSE;
	Debug_Info("volume unmounted\n");
}

static void remove_volume(struct ExfatHandler* h)
{
	EXFAT_BASES(h);
	if (h->volume == NULL)
		return;
	if (AttemptLockDosList(LDF_VOLUMES | LDF_WRITE) != 0)
	{
		/* FreeDosEntry() disposes of the name MakeDosEntry() allocated, so
		   put that one back before letting it go (see do_rename_disk). */
		if (h->volname_orig != 0)
			h->volume->dl_Name = h->volname_orig;
		RemDosEntry((struct DosList*)h->volume);
		UnLockDosList(LDF_VOLUMES | LDF_WRITE);
		FreeDosEntry((struct DosList*)h->volume);
	}
	if (h->volname != 0)
	{
		FreeVec(BADDR(h->volname));
		h->volname = 0;
	}
	h->volname_orig = 0;
	h->volume = NULL;
}

/* Force each published partition to mount, so its volume appears on
   Workbench instead of only when something first touches the name.  Locking
   the name makes AmigaDOS start that node's handler process, as the mounter
   does with Lock() after AddDosEntry().

   Two reasons this is a separate process rather than inline code:

   - Locking a name blocks until that node's handler has started and
     answered, and a handler that is not servicing its port cannot answer
     anything.  Inline, this volume would be unresponsive while the others
     came up, and anything touching it would block behind us.
   - It must not reach into the handler's state, which is per process and
     may be freed by ACTION_DIE while this is still running.  The names are
     copied into the startup message instead, so the helper depends on
     nothing that can go away underneath it. */
struct ActivationMsg
{
	struct Message		msg;
	struct DosLibrary*	dosbase;
	int			count;
	char		names[EXFAT_MAX_PARTITIONS][34];
};

static UNUSED void activation_proc(void)
{
	struct ExecBase* SysBase = *(struct ExecBase**)4UL;
	struct DosLibrary* DOSBase;
	struct Process* me = (struct Process*)FindTask(NULL);
	struct ActivationMsg* am;
	int i;

	WaitPort(&me->pr_MsgPort);
	am = (struct ActivationMsg*)GetMsg(&me->pr_MsgPort);
	if (am == NULL)
		return;
	DOSBase = am->dosbase;

	for (i = 0; i < am->count; i++)
	{
		BPTR lock = Lock((CONST_STRPTR)am->names[i], SHARED_LOCK);

		if (lock != 0)
		{
			UnLock(lock);
			Debug_Info("activated %s\n", am->names[i]);
		}
		else
			Debug_Warn("could not activate %s (IoErr %ld); it stays "
					"available on first access\n",
					am->names[i], (LONG)IoErr());
	}
	FreeVec(am);		/* ours to release; nothing is waiting on a reply */
}

static void activate_partitions(struct ExfatHandler* h)
{
	EXFAT_BASES(h);
#if EXFAT_AMIGA_ACTIVATE
	struct ActivationMsg* am;
	struct Process* proc;
	struct TagItem tags[5];
	int i;

	if (h->npublished == 0)
		return;

	am = AllocVec(sizeof(struct ActivationMsg), MEMF_PUBLIC | MEMF_CLEAR);
	if (am == NULL)
		return;
	am->msg.mn_Node.ln_Type = NT_MESSAGE;
	am->msg.mn_Length = sizeof(struct ActivationMsg);
	am->msg.mn_ReplyPort = NULL;
	am->dosbase = h->dosbase;
	am->count = h->npublished;
	for (i = 0; i < h->npublished; i++)
		strcpy(am->names[i], h->published[i]);

	tags[0].ti_Tag = NP_Entry;     tags[0].ti_Data = (ULONG)activation_proc;
	tags[1].ti_Tag = NP_Name;      tags[1].ti_Data = (ULONG)"exfat activation";
	tags[2].ti_Tag = NP_StackSize; tags[2].ti_Data = 16384;
	tags[3].ti_Tag = NP_Priority;  tags[3].ti_Data = 0;
	tags[4].ti_Tag = TAG_END;      tags[4].ti_Data = 0;

	proc = CreateNewProc(tags);
	if (proc == NULL)
	{
		Debug_Warn("could not start the activation process; partitions stay "
				"available on first access\n");
		FreeVec(am);
		return;
	}
	PutMsg(&proc->pr_MsgPort, (struct Message*)am);
#else
	(void)h;
#endif
}

/* ------------------------------------------------------------------ */
/* Main loop                                                          */
/* ------------------------------------------------------------------ */

LONG exfat_handler_main(void)
{
	#if DEBUG
    /* Must run before the first Inform()/Warn()/Trace() call: sets the debug
     * UART baud rate. Safe to call this early - it only pokes hardware
     * registers directly, no OS dependency. */
    ApolloDebugInit();
	#endif
	
	struct ExecBase* SysBase = *(struct ExecBase**)4UL;
	struct DosLibrary* DOSBase;
	struct ExfatHandler* h;
	struct Process* proc;
	struct Message* msg;
	struct DosPacket* pkt;

	/* Debug_Warn, not Debug_Info: when a mount fails there has to be one
	   line that distinguishes "AmigaDOS never started us" from "we started
	   and then failed".  Logging our own entry point address also says
	   whether this process is running from ROM or from a LoadSeg'd copy. */
	Debug_Warn("handler process started, entry at %08lx\n", (ULONG)exfat_handler_entry);

	h = AllocVec(sizeof(struct ExfatHandler), MEMF_ANY | MEMF_CLEAR);
	if (h == NULL)
		return RETURN_FAIL;	/* nothing to reply to yet */

	proc = (struct Process*)FindTask(NULL);
	h->proc = proc;
	h->port = &proc->pr_MsgPort;

	/* The startup packet arrives on our process port (12.1.2). */
	WaitPort(h->port);
	msg = GetMsg(h->port);
	pkt = (struct DosPacket*)msg->mn_Node.ln_Name;

	DOSBase = (struct DosLibrary*)OpenLibrary("dos.library", 37);
	h->dosbase = DOSBase;
	if (DOSBase == NULL)
	{
		/* Reply by hand: without dos.library there is no ReplyPkt(). */
		struct MsgPort* reply = pkt->dp_Port;

		pkt->dp_Res1 = DOSFALSE;
		pkt->dp_Res2 = ERROR_INVALID_RESIDENT_LIBRARY;
		pkt->dp_Port = h->port;
		msg->mn_Node.ln_Name = (char*)pkt;
		PutMsg(reply, msg);
		FreeVec(h);
		return RETURN_FAIL;
	}

	if (!startup(h, pkt))
	{
		if (h->mounted)
			exfat_unmount(&h->ef);
		ReplyPkt(pkt, DOSFALSE, ERROR_OBJECT_IN_USE);
		release_partition(h);
		CloseLibrary((struct Library*)DOSBase);
		FreeVec(h);
		return RETURN_FAIL;
	}

	ReplyPkt(pkt, DOSTRUE, 0);

	/* Safe to touch the device list only now. */
	add_volume(h);
	activate_partitions(h);

	while (!h->quit)
	{
		WaitPort(h->port);
		while ((msg = GetMsg(h->port)) != NULL)
		{
			pkt = (struct DosPacket*)msg->mn_Node.ln_Name;

			{
				ULONG sp = (ULONG)&msg;
				ULONG lower = (ULONG)h->proc->pr_Task.tc_SPLower;
				ULONG headroom = (sp > lower) ? sp - lower : 0;

				Debug_Trace("packet %ld arg1 %08lx arg2 %08lx arg3 %08lx "
						"stack %lu\n",
						(LONG)pkt->dp_Type, (ULONG)pkt->dp_Arg1,
						(ULONG)pkt->dp_Arg2, (ULONG)pkt->dp_Arg3, headroom);
				if (headroom < 8192)
					Debug_Error("stack headroom down to %lu bytes - raise "
							"StackSize in the mount file\n", headroom);
			}

			switch (pkt->dp_Type)
			{
			case ACTION_LOCATE_OBJECT:
				do_locate_object(h, pkt);
				break;
			case ACTION_FREE_LOCK:
				exfat_amiga_freelock(h,
						exfat_amiga_lock_from_bptr((BPTR)pkt->dp_Arg1));
				ReplyPkt(pkt, DOSTRUE, 0);
				break;
			case ACTION_COPY_DIR:
				do_copy_dir(h, pkt);
				break;
			case ACTION_COPY_DIR_FH:
				do_lock_from_fh(h, pkt, FALSE);
				break;
			case ACTION_PARENT_FH:
				do_lock_from_fh(h, pkt, TRUE);
				break;
			case ACTION_PARENT:
				do_parent(h, pkt);
				break;
			case ACTION_SAME_LOCK:
			{
				struct ExfatLock* a =
						exfat_amiga_lock_from_bptr((BPTR)pkt->dp_Arg1);
				struct ExfatLock* b =
						exfat_amiga_lock_from_bptr((BPTR)pkt->dp_Arg2);
				struct exfat_node* na = a ? a->node : h->ef.root;
				struct exfat_node* nb = b ? b->node : h->ef.root;

				ReplyPkt(pkt, (na == nb) ? DOSTRUE : DOSFALSE, 0);
				break;
			}
			case ACTION_EXAMINE_OBJECT:
				do_examine_object(h, pkt);
				break;
			case ACTION_EXAMINE_NEXT:
				do_examine_next(h, pkt);
				break;
			case ACTION_FINDINPUT:
				do_open(h, pkt, OPEN_OLD);
				break;
			case ACTION_FINDOUTPUT:
				do_open(h, pkt, OPEN_NEW);
				break;
			case ACTION_FINDUPDATE:
				do_open(h, pkt, OPEN_UPDATE);
				break;
			case ACTION_WRITE:
				do_write(h, pkt);
				break;
			case ACTION_SET_FILE_SIZE:
				do_set_file_size(h, pkt);
				break;
			case ACTION_SET_PROTECT:
				do_set_protect(h, pkt);
				break;
			case ACTION_SET_DATE:
				do_set_date(h, pkt);
				break;
			case ACTION_SET_COMMENT:
				do_set_comment(h, pkt);
				break;
			case ACTION_DELETE_OBJECT:
				do_delete_object(h, pkt);
				break;
			case ACTION_CREATE_DIR:
				do_create_dir(h, pkt);
				break;
			case ACTION_RENAME_OBJECT:
				do_rename_object(h, pkt);
				break;
			case ACTION_FORMAT:
				do_format(h, pkt);
				break;
			case ACTION_RENAME_DISK:
				do_rename_disk(h, pkt);
				break;
			case ACTION_EXAMINE_FH:
				do_examine_fh(h, pkt);
				break;
			case ACTION_FH_FROM_LOCK:
				do_fh_from_lock(h, pkt);
				break;
			case ACTION_END:
				do_end(h, pkt);
				break;
			case ACTION_READ:
				do_read(h, pkt);
				break;
			case ACTION_SEEK:
				do_seek(h, pkt);
				break;
			case ACTION_INFO:
				fill_infodata(h, (struct InfoData*)BADDR(pkt->dp_Arg2));
				ReplyPkt(pkt, DOSTRUE, 0);
				break;
			case ACTION_DISK_INFO:
				fill_infodata(h, (struct InfoData*)BADDR(pkt->dp_Arg1));
				ReplyPkt(pkt, DOSTRUE, 0);
				break;
			case ACTION_CURRENT_VOLUME:
				ReplyPkt(pkt, (LONG)MKBADDR(h->volume), 0);
				break;
			case ACTION_IS_FILESYSTEM:
				ReplyPkt(pkt, DOSTRUE, 0);
				break;
			case ACTION_FLUSH:
			{
				LONG err = 0;

				if (!h->ef.ro)
				{
					if (exfat_flush_nodes(&h->ef) != 0 ||
							exfat_flush(&h->ef) != 0)
						err = ERROR_DISK_FULL;
					else if (exfat_fsync(h->ef.dev) != 0)
						err = ERROR_DISK_FULL;
				}
				Debug_Info("flush: %s\n", err ? "failed" : "ok");
				ReplyPkt(pkt, err ? DOSFALSE : DOSTRUE, err);
				break;
			}
			case ACTION_INHIBIT:
				do_inhibit(h, pkt);
				break;
			case ACTION_DIE:
				if (h->nlocks != 0 || h->nfiles != 0)
				{
					ReplyPkt(pkt, DOSFALSE, ERROR_OBJECT_IN_USE);
					break;
				}
				h->quit = TRUE;
				ReplyPkt(pkt, DOSTRUE, 0);
				break;

			/* Read-only build: refuse the mutating packets explicitly so
			   callers get ERROR_DISK_WRITE_PROTECTED rather than a
			   confusing "action not known". */
				ReplyPkt(pkt, DOSFALSE, ERROR_DISK_WRITE_PROTECTED);
				break;

			default:
				Debug_Info("unhandled packet type %ld\n", (LONG)pkt->dp_Type);
				ReplyPkt(pkt, DOSFALSE, ERROR_ACTION_NOT_KNOWN);
				break;
			}
		}
	}

	Debug_Info("shutting down\n");
	remove_volume(h);
	if (h->devlist != NULL)
		h->devlist->dol_Task = NULL;
	exfat_unmount(&h->ef);
	release_partition(h);
	CloseLibrary((struct Library*)DOSBase);
	FreeVec(h);
	return RETURN_OK;
}
