/*
	romtag.c

	Makes the handler a ROM module.

	A file destined for Kickstart ROM is not an executable that something
	runs - it is a Resident ("ROMTag") that the ROM boot scan finds and
	initialises.  Without one the ROM build tool rejects the file with "no
	resident found", which is what this file exists to fix.

	The tag's init routine registers the handler in FileSystem.resource
	under DOSType 'FATX', so any mounter - sagasd.device's, an RDB, or
	AmigaDOS itself - finds it exactly the way it finds the ROM
	FastFileSystem, and no copy in L: is needed.

	The file stays a perfectly good AmigaDOS executable as well: entry.c is
	still the first object, so offset 0 of the code hunk is still the
	handler's entry point, and a Resident in a file that nothing scans is
	simply ignored.  One binary serves both the ROM and L: routes.

	Copyright (C) 2026  Willem Drijver

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 2 of the License, or
	(at your option) any later version.
*/

#include "exfat_amiga.h"

#include <exec/nodes.h>
#include <exec/memory.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#include <resources/filesysres.h>
#include <proto/exec.h>

#include "ApolloCrossDev_Debug.h"

#ifndef EXFAT_VER
#define EXFAT_VER	0
#define EXFAT_REV	1
#endif

extern void exfat_handler_entry(void);	/* entry.S, module offset 0 */

/* A marker that goes to the UART directly, with no exec call in the way.

   Debug_Flag() reaches the serial port through Forbid() and RawDoFmt(), and
   RawDoFmt() takes a callback and walks a format string.  When rt_Init
   produces no output at all, that is two possibilities, not one: the routine
   never ran, or it ran and died inside that machinery.  ApolloDebugPutStr()
   is a poll-and-poke loop over the UART registers - no exec, no callback, no
   format string - so a marker printed with it separates them. */
static void rom_mark(const char* text)
{
	ApolloDebugPutStr((const char*)"[exfat romtag] ");
	ApolloDebugPutStr(text);
	ApolloDebugPutStr((const char*)"\n");
}

/* Read the instruction stream a byte at a time.  Casting a function pointer
   to UWORD* breaks strict aliasing, and 68k code is big-endian regardless of
   what the compiler thinks the alignment is. */
static UWORD opcode_at(const UBYTE* p)
{
	return (UWORD)(((UWORD)p[0] << 8) | p[1]);
}

static UNUSED ULONG operand_at(const UBYTE* p)
{
	return ((ULONG)p[0] << 24) | ((ULONG)p[1] << 16) |
			((ULONG)p[2] << 8) | (ULONG)p[3];
}

/* An AmigaDOS segment list: a BPTR to the "next segment" longword with the
   code immediately after it, and the segment's size in the longword before
   it.  LoadSeg() builds these; this one is built by hand because the code it
   names lives in ROM.

   JMP and not JSR: the stack must look to exfat_handler_main() exactly as
   AmigaDOS left it, so that its RTS returns to AmigaDOS's process cleanup
   rather than into this stub. */
struct ExfatRomSeg
{
	ULONG	size;		/* what UnLoadSeg would read, at BADDR - 4 */
	BPTR	next;		/* 0: the only segment.  BADDR points here */
	UWORD	jmp;		/* 0x4EF9 = JMP <absolute long> */
	APTR	entry;
};

/* ------------------------------------------------------------------ */
/* what the ROM tag in entry.S points at                               */
/* ------------------------------------------------------------------ */

const char exfat_rom_name[] = "exfat-handler";
const char exfat_rom_id[] =
		"$VER: exfat-handler " VERSION " (" BUILD_DATE_STR ")\r\n";

/* ------------------------------------------------------------------ */

/* Called once at boot.  Nothing checks the result - InitCode() discards it -
   so a failure here just means the ROM copy is not offered and a mounter
   falls back to loading L:exfat-handler.

   The stage markers are Debug_Flag, on a channel of their own.  They used
   to be Debug_Warn so they would survive a build that dropped INFO - but
   WARN also carries libexfat's operational warnings, and those cost 3.5 KB
   of ROM that the 128 KB budget no longer has.  DBG_FLAG had no users at
   all, so it is free: build with DEBUG=12 (FLAG + ERROR) and you get every
   line this routine can say, plus every error anywhere, and none of the
   noise.  When a ROM boot dies this is the only narrative there is, and
   it now costs a few hundred bytes rather than several thousand. */
ULONG exfat_rom_init(void)
{
	EXFAT_SYSBASE;
#ifdef EXFAT_ROMTAG_REGISTER
	struct FileSysResource* fsr;
	struct FileSysEntry* fse;
	struct FileSysEntry* e;
	struct ExfatRomSeg* seg;
#endif

	/* Raw first, then the normal path: if only the raw one appears, the
	   fault is in Forbid()/RawDoFmt() at coldstart, not in rt_Init. */
	rom_mark("rt_Init reached");
	Debug_Flag("exfat romtag: init entered\n");
	rom_mark("Debug_Warn survived");

	/* Relocation self-check.  Offset 0 of this module is "jmp
	   _exfat_handler_main", so exfat_handler_entry must point at a 0x4EF9.
	   If the ROM builder did not apply our relocations the pointer still
	   holds its link-time offset - a small number - and reading it gives
	   the exception vectors rather than our code.  Everything after this
	   would then be a jump into low memory, so say so and stop. */
	if (opcode_at((const UBYTE*)exfat_handler_entry) != 0x4EF9)
	{
		Debug_Error("exfat romtag: entry %08lx holds %04lx, not our JMP "
				"(4ef9) - relocations were not applied\n",
				(ULONG)exfat_handler_entry,
				(ULONG)opcode_at((const UBYTE*)exfat_handler_entry));
		return 0;
	}
	Debug_Flag("exfat romtag: entry %08lx verified\n",
			(ULONG)exfat_handler_entry);

#ifndef EXFAT_ROMTAG_REGISTER
	/* The default, and the only configuration proven on hardware: the
	   module is a well-formed ROM member that registers nothing.  Mounting
	   goes through L:exfat-handler as it always has.

	   Registering (make ROMREG=1) is what is still being brought up; see
	   ../CLAUDE.md, "ROM residency", for where that stands. */
	Debug_Flag("exfat romtag: present, not registering "
			"(build with ROMREG=1 to register)\n");
	return 0;
#else

	rom_mark("calling OpenResource");
	fsr = (struct FileSysResource*)OpenResource(FSRNAME);
	if (fsr == NULL)
	{
		Debug_Error("exfat romtag: no FileSystem.resource\n");
		return 0;
	}

	Debug_Flag("exfat romtag: FileSystem.resource at %08lx\n", (ULONG)fsr);

	/* This runs at coldstart, before anything has validated the resource.
	   An empty exec List has lh_Head pointing at lh_Tail, never NULL, so a
	   NULL head means the list was never NewList()ed and walking it would
	   chase whatever happens to be at address 0. */
	if (fsr->fsr_FileSysEntries.lh_Head == NULL)
	{
		Debug_Error("exfat romtag: FileSystem.resource list is not "
				"initialised - too early to register\n");
		return 0;
	}

	/* Someone may have got here first - a newer handler patched in from
	   disk, or a second copy of this ROM module.  Leave theirs alone. */
	Forbid();
	for (e = (struct FileSysEntry*)fsr->fsr_FileSysEntries.lh_Head;
			e->fse_Node.ln_Succ != NULL;
			e = (struct FileSysEntry*)e->fse_Node.ln_Succ)
		if (e->fse_DosType == EXFAT_DOSTYPE)
		{
			Permit();
			Debug_Flag("exfat romtag: 'FATX' is already registered\n");
			return 0;
		}
	Permit();

	Debug_Flag("exfat romtag: no 'FATX' entry yet, adding one\n");

	rom_mark("allocating FileSysEntry");
	fse = AllocMem(sizeof(*fse), MEMF_PUBLIC | MEMF_CLEAR);
	if (fse == NULL)
	{
		Debug_Error("exfat romtag: out of memory for the FileSysEntry\n");
		return 0;
	}

	fse->fse_Node.ln_Name = (char*)exfat_rom_name;
	fse->fse_DosType      = EXFAT_DOSTYPE;
	fse->fse_Version      = ((ULONG)EXFAT_VER << 16) | (ULONG)EXFAT_REV;
	/* $190 - StackSize as well as the seglist, exactly what fat95 patches.
	   A device node built by MakeDosNode() carries a default stack far
	   below what this handler needs, and a mounter only overrides what the
	   patch flags name.  Priority is left alone: MakeDosNode()'s default of
	   10 is already what we want. */
	fse->fse_PatchFlags   = FSEF_STACKSIZE | FSEF_SEGLIST | FSEF_GLOBALVEC;
	fse->fse_StackSize    = EXFAT_STACKSIZE;
	/* Set HERE, with the other fields, and not further down.  It used to
	   sit after the RAM-segment fallback, and the fat95-style path above
	   it ends in `goto registered` - so on every boot where the module was
	   longword aligned, which is every boot on this ROM, it was skipped.
	   The entry then carried a GlobalVec of 0 with FSEF_GLOBALVEC set, the
	   mounter copied that 0 into the node exactly as told, and AmigaDOS
	   started a C handler as if it were BCPL.  The symptom was Lock()
	   failing with IoErr 103 on a volume that had mounted perfectly. */
	fse->fse_GlobalVec    = (BPTR)-1;	/* C handler, not BCPL */
	/* Prefer fat95's segment list: the module itself, with BADDR four bytes
	   before its first instruction.  That needs the module to be longword
	   aligned in the ROM - MKBADDR is a plain >> 2 - which was unknowable
	   until a boot log showed the entry at 00F12488.  When it holds, this
	   costs no allocation and no CacheClearU, and is byte-for-byte what
	   fat95 does in this same ROM.

	   The RAM stub below stays as the fallback for a ROM builder that
	   places the module on a two-byte boundary. */
#ifndef EXFAT_ROMTAG_RAMSEG
	if ((((ULONG)exfat_handler_entry) & 3) == 0)
	{
		fse->fse_SegList = MKBADDR((UBYTE*)exfat_handler_entry - 4);
		rom_mark("segment list is the module itself (fat95 style)");
		goto registered;
	}
#endif

	rom_mark("module is not longword aligned - building a RAM segment");
	seg = AllocMem(sizeof(*seg), MEMF_PUBLIC | MEMF_CLEAR);
	if (seg == NULL)
	{
		Debug_Error("exfat romtag: out of memory for the segment list\n");
		FreeMem(fse, sizeof(*fse));
		return 0;
	}
	seg->size  = sizeof(*seg);
	seg->next  = (BPTR)0;
	seg->jmp   = 0x4EF9;			/* JMP <absolute long> */
	seg->entry = (APTR)exfat_handler_entry;

	/* This is code we just wrote with the data cache.  The 68080 has split
	   caches, so it has to reach memory before anything fetches it. */
	CacheClearU();

	fse->fse_SegList      = MKBADDR(&seg->next);

	/* Read the segment back the way AmigaDOS will, and check it really is
	   our JMP at the address AmigaDOS will start the process from.  A bad
	   BPTR or a struct that packed differently would otherwise show up as a
	   freeze with nothing in the log. */
	{
		const UBYTE* code = (const UBYTE*)BADDR(fse->fse_SegList) + 4;

		if (opcode_at(code) != 0x4EF9 ||
				operand_at(code + 2) != (ULONG)exfat_handler_entry)
		{
			Debug_Error("exfat romtag: segment list is malformed - %08lx "
					"holds %04lx %08lx, wanted 4ef9 %08lx\n",
					(ULONG)code, (ULONG)opcode_at(code),
					operand_at(code + 2), (ULONG)exfat_handler_entry);
			FreeMem(seg, sizeof(*seg));
			FreeMem(fse, sizeof(*fse));
			return 0;
		}
	}
	/* AddHead, which is what fat95 uses.  Enqueue() would sort by ln_Pri,
	   which means walking every existing node at coldstart for no gain -
	   mounters take the first DOSType match, so a copy added later from
	   disk lands in front of this one either way. */
#ifndef EXFAT_ROMTAG_RAMSEG
registered:				/* only the fat95 path jumps here */
#endif
	Forbid();
#ifdef EXFAT_ROMTAG_NOADD
	rom_mark("NOADD build - entry built but NOT added to the list");
#else
	rom_mark("adding to FileSystem.resource");
	AddHead(&fsr->fsr_FileSysEntries, &fse->fse_Node);
#endif
	Permit();

	Debug_Flag("exfat romtag: registered 'FATX' %ld.%ld, seglist %08lx -> "
			"code %08lx, ROM entry %08lx\n",
			(LONG)EXFAT_VER, (LONG)EXFAT_REV, (ULONG)fse->fse_SegList,
			(ULONG)BADDR(fse->fse_SegList) + 4,
			(ULONG)exfat_handler_entry);
	rom_mark("registered - rt_Init complete");
	return 0;
#endif /* EXFAT_ROMTAG_REGISTER */
}
