/*
	hostcompat.h

	The handful of AmigaOS types that ../amiga/amigaos.c needs, so that file
	can be compiled natively for the host unit tests.  Deliberately minimal:
	anything more would start to be a fake AmigaOS rather than a test shim.

	Copyright (C) 2026  Willem Drijver

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 2 of the License, or
	(at your option) any later version.
*/

#ifndef HOSTCOMPAT_H_INCLUDED
#define HOSTCOMPAT_H_INCLUDED

#include <stdint.h>
#include <stddef.h>
#include <time.h>

typedef uint8_t		UBYTE;
typedef uint16_t	UWORD;
typedef uint32_t	ULONG;
typedef int32_t		LONG;
typedef int32_t		BOOL;

#ifndef TRUE
#define TRUE	1
#define FALSE	0
#endif

/* dos/dos.h */
#define TICKS_PER_SECOND 50

struct DateStamp
{
	LONG	ds_Days;	/* days since 1978-01-01 */
	LONG	ds_Minute;	/* minutes since midnight */
	LONG	ds_Tick;	/* ticks (1/50 s) past the minute */
};

/* the routines under test, as declared in ../amiga/exfat_amiga.h */
void exfat_amiga_name_to_bstr(const char* utf8, UBYTE* bstr, int maxlen);
void exfat_amiga_bstr_to_utf8(const UBYTE* bstr, char* utf8, int maxlen);
void exfat_amiga_cstr_to_utf8(const char* src, char* utf8, int maxlen);
void exfat_amiga_time_to_datestamp(time_t t, struct DateStamp* ds);
time_t exfat_amiga_datestamp_to_time(const struct DateStamp* ds);
int exfat_amiga_vsnprintf(char* buf, size_t size, const char* fmt, va_list ap);
int exfat_amiga_snprintf(char* buf, size_t size, const char* fmt, ...);
void exfat_tzset(void);

#endif /* HOSTCOMPAT_H_INCLUDED */
