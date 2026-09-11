| entry.S
|
| The first bytes of the module, and the ROM tag.
|
| This file must be the FIRST object given to the linker.  Two separate
| things depend on that:
|
|   - AmigaDOS starts a handler by jumping to the first byte of the segment,
|     so offset 0 has to be executable code that reaches the handler.
|   - The ROM tag has to be in the first code hunk (exec.doc, InitResident),
|     and the ROM build tool looks for it near the front of the module.
|
| The layout mirrors fat95's, which is the handler this workspace already
| runs from ROM: a jump at the very start, the Resident immediately after
| it.  This is assembly rather than C because C gives no way to order a
| function and a data object within an object file.
|
| Copyright (C) 2026  Willem Drijver
|
| This program is free software; you can redistribute it and/or modify
| it under the terms of the GNU General Public License as published by
| the Free Software Foundation, either version 2 of the License, or
| (at your option) any later version.

#ifndef EXFAT_VER
#define EXFAT_VER	0
#endif

	.text

	.globl	_exfat_handler_entry
	.globl	_exfat_romtag

| Offset 0 of the module.  Also where the FileSystem.resource segment list
| points: romtag.c hands out MKBADDR(_exfat_handler_entry - 4), so AmigaDOS
| jumps here, exactly as fat95 does.
|
| JMP, not JSR: the stack must look to exfat_handler_main() exactly as
| AmigaDOS left it, so its RTS returns to AmigaDOS's process cleanup.
_exfat_handler_entry:
	jmp	_exfat_handler_main

	.even
_exfat_romtag:
	.word	0x4AFC			| rt_MatchWord (RTC_MATCHWORD)
	.long	_exfat_romtag		| rt_MatchTag - must point at itself
	.long	__etext			| rt_EndSkip - past the whole module.
					|   The linker defines __etext at the
					|   end of .text, and this module has
					|   no other hunk, so that is the end.
#ifdef EXFAT_ROM_AFTERDOS
	.byte	4			| rt_Flags = RTF_AFTERDOS.  The ROM
					|   boot process populates
					|   FileSystem.resource itself (see
					|   filesysres.doc), so inserting at
					|   coldstart races it.  AFTERDOS runs
					|   once dos.library is up and the list
					|   is settled.
	.byte	EXFAT_VER		| rt_Version
	.byte	0			| rt_Type = NT_UNKNOWN
	.byte	-100			| rt_Pri - exec.doc: AFTERDOS
					|   modules start at -100
#else
	.byte	1			| rt_Flags = RTF_COLDSTART, as fat95
					|   uses.
	.byte	EXFAT_VER		| rt_Version
	.byte	0			| rt_Type = NT_UNKNOWN: not a
					|   library, device or resource
	.byte	EXFAT_ROM_PRI		| rt_Pri - MUST exceed sagasd.device's.
					|   Coldstart tags run in descending
					|   priority, and sagasd (pri 5) calls
					|   CheckUnit() straight from its init:
					|   ScanMBR() looks for FATX in
					|   FileSystem.resource right then, not
					|   five seconds later.  At priority 0
					|   this module registered *after* that
					|   look, and the serial log read
					|   "exfat dostype not found" on every
					|   boot.  fat95 hit the same thing.
#endif
	.long	_exfat_rom_name		| rt_Name
	.long	_exfat_rom_id		| rt_IdString
	.long	_exfat_rom_init		| rt_Init - RTF_AUTOINIT is clear, so
					|   this is called as a plain routine
