| romstub.S
|
| A ROM bring-up diagnostic: the smallest possible Kickstart module.
|
| It is a Resident and nothing else - about 200 bytes, no C, no libc, no
| exec calls.  Its rt_Init writes one line to the debug UART and returns.
|
| Use it to split a ROM failure in two.  Build it, put it in the ROM in
| place of exfat-handler, and boot:
|
|   line appears, machine boots  -> ROM integration works for a module of
|                                   this shape.  The problem is specific to
|                                   exfat-handler: its size, or its content.
|   no line, or still crashes    -> the problem is the ROM image or the tool,
|                                   not the handler.  Check free ROM space
|                                   first: exfat-handler is ~62 KB against
|                                   fat95's 27 KB.
|
| The UART sequence is the one ApolloCrossDev_Debug.c uses, inlined so the
| stub depends on nothing at all.
|
| Once the bare stub is known to work, "make romstub PAD=62000" pads it to
| the handler's size with incompressible filler.  That separates the two
| remaining explanations:
|
|   padded stub boots  -> the ROM has room; the problem is what is *in*
|                         exfat-handler, not how big it is.
|   padded stub crashes -> the ROM is out of space at that size, and the
|                         handler has to shrink.
|
| The filler is random rather than zeroes on purpose: a ROM tool that packs
| or compresses modules would swallow a run of zeroes and the test would
| wrongly pass.
|
| Copyright (C) 2026  Willem Drijver
|
| This program is free software; you can redistribute it and/or modify
| it under the terms of the GNU General Public License as published by
| the Free Software Foundation, either version 2 of the License, or
| (at your option) any later version.

SERDAT   = 0xDFF030
SERDATR  = 0xDFF018
INTREQ   = 0xDFF09C
SERDATR_TBE = 0x2000
INTF_TBE = 0x0001
SERDAT_STP8 = 0x0100

	.text
	.globl	_start

| Offset 0.  Nothing starts this module as a process; the RTS is here only
| so the file is a valid executable too.
_start:
	moveq	#0,d0
	rts

	.even
romtag:
	.word	0x4AFC			| rt_MatchWord
	.long	romtag			| rt_MatchTag
	.long	__etext			| rt_EndSkip - end of the module
	.byte	1			| rt_Flags = RTF_COLDSTART
	.byte	0			| rt_Version
	.byte	0			| rt_Type = NT_UNKNOWN
	.byte	0			| rt_Pri - same as exfat-handler's
	.long	name			| rt_Name
	.long	idstring		| rt_IdString
	.long	init			| rt_Init

| d0/d1/a0/a1 are scratch; a6 holds SysBase on entry and is not touched.
init:
	lea	message(pc),a0
putstr:
	moveq	#0,d0
	move.b	(a0)+,d0
	beq.s	done
	bsr.s	putch
	bra.s	putstr
done:
	moveq	#0,d0
	rts

putch:
	cmp.b	#10,d0			| \n -> \r\n, as ApolloDebugPutChar does
	bne.s	putch1
	moveq	#13,d1
	exg	d0,d1
	bsr.s	putch1
	exg	d0,d1
putch1:
	move.l	#SERDATR,a1
wait:
	move.w	(a1),d1
	and.w	#SERDATR_TBE,d1
	beq.s	wait
	move.l	#INTREQ,a1
	move.w	#INTF_TBE,(a1)
	move.l	#SERDAT,a1
	and.w	#0x00FF,d0
	or.w	#SERDAT_STP8,d0
	move.w	d0,(a1)
	rts

	.even
name:
	.asciz	"exfat-romstub"
idstring:
	.asciz	"$VER: exfat-romstub 1.0\r\n"
message:
	.asciz	"\n*** exfat romstub: ROM tag ran, module is alive ***\n"
	.even

#ifdef ROMSTUB_SPREAD
| Relocations spread across the whole module, the way a compiled module's
| are.  ROMSTUB_RELOCS packs them into a narrow band, which matches the
| handler's relocation COUNT but not its offset DISTRIBUTION - fat95 needs
| only 5 (its ROM tag's pointers, everything else PC-relative), sagasd needs
| 102 over 39 KB, and the handler needs 586 over 62 KB.  Interleaving filler
| with relocated longwords reproduces the third case.
spreadbase:
	.rept	ROMSTUB_SPREAD_N
	.space	ROMSTUB_SPREAD, 0x4e
	.long	spreadbase
	.endr
	.even
#endif

#ifdef ROMSTUB_RELOCS
| A relocation table of the same weight the handler carries.  Padding alone
| matches the module's CODE size but not the size of the FILE the ROM tool
| reads: the handler's 588 relocations add ~2.3 KB of CODE and another
| ~2.3 KB of HUNK_RELOC32 on top.  Each .long here is one RELOC32, so this
| reproduces both.  Nothing reads it.
relocfill:
	.rept	ROMSTUB_RELOCS
	.long	relocfill
	.endr
	.even
#endif

#ifdef ROMSTUB_PAD
| Grow the module to a realistic size.  Nothing reads this; it is here only
| to occupy the ROM space exfat-handler would occupy.  __etext, and so
| rt_EndSkip, moves past it.
padding:
	.incbin	"build/romstub.pad"
	.even
#endif
