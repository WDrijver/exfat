/*
	amigaos.c

	AmigaOS support code for the exFAT handler: a self-contained printf
	subset for the log back end, name conversion between exFAT's UTF-8/UTF-16
	and the Amiga 8-bit character set, and exFAT time to DateStamp.

	Nothing here may call into newlib's stdio: a file system process is
	started by AmigaDOS without a C run-time, so there is no initialised
	reent structure to render through.

	Copyright (C) 2026  Willem Drijver

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 2 of the License, or
	(at your option) any later version.
*/

#include <string.h>
#include <stdarg.h>
#include <stdint.h>

#ifdef EXFAT_HOSTTEST
/* Built natively for the host unit tests (../hosttest).  The routines below
   are plain C, so they are compiled from this same file rather than copied -
   the tests exercise exactly the code that ships. */
#include "hostcompat.h"
#else
#include <exec/types.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include "exfat_amiga.h"
#endif

/* ------------------------------------------------------------------ */
/* Minimal vsnprintf                                                  */
/* ------------------------------------------------------------------ */

struct sbuf
{
	char*	p;
	size_t	left;		/* space left, excluding the NUL */
	size_t	count;		/* characters that would have been written */
};

static void sb_putc(struct sbuf* sb, char c)
{
	sb->count++;
	if (sb->left > 0)
	{
		*sb->p++ = c;
		sb->left--;
	}
}

static void sb_puts(struct sbuf* sb, const char* s)
{
	if (s == NULL)
		s = "(null)";
	while (*s != '\0')
		sb_putc(sb, *s++);
}

static void sb_putu(struct sbuf* sb, uint64_t v, unsigned base, int upper,
		int width, char pad)
{
	char tmp[24];
	const char* digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
	int n = 0;

	if (v == 0)
		tmp[n++] = '0';
	while (v != 0 && n < (int)sizeof(tmp))
	{
		tmp[n++] = digits[v % base];
		v /= base;
	}
	while (width-- > n)
		sb_putc(sb, pad);
	while (n > 0)
		sb_putc(sb, tmp[--n]);
}

static void sb_putd(struct sbuf* sb, int64_t v, int width, char pad)
{
	if (v < 0)
	{
		sb_putc(sb, '-');
		sb_putu(sb, (uint64_t)(-(v + 1)) + 1, 10, 0, width > 0 ? width - 1 : 0,
				pad);
	}
	else
		sb_putu(sb, (uint64_t)v, 10, 0, width, pad);
}

/* Supports the conversions libexfat actually uses: %s %c %% and the
   d/i/u/x/X family with the l, ll, z and j length modifiers, plus %p and a
   leading zero/space field width.  Precision is parsed and ignored. */
int exfat_amiga_vsnprintf(char* buf, size_t size, const char* fmt, va_list ap)
{
	struct sbuf sb;

	sb.p = buf;
	sb.left = (size > 0) ? size - 1 : 0;
	sb.count = 0;

	while (*fmt != '\0')
	{
		int longs = 0;
		int width = 0;
		char pad = ' ';

		if (*fmt != '%')
		{
			sb_putc(&sb, *fmt++);
			continue;
		}
		fmt++;

		/* flags (only the ones that change output) */
		while (*fmt == '-' || *fmt == '+' || *fmt == ' ' || *fmt == '#' ||
				*fmt == '0')
		{
			if (*fmt == '0')
				pad = '0';
			fmt++;
		}
		/* field width */
		while (*fmt >= '0' && *fmt <= '9')
			width = width * 10 + (*fmt++ - '0');
		/* precision - parsed, not honoured */
		if (*fmt == '.')
		{
			fmt++;
			while (*fmt >= '0' && *fmt <= '9')
				fmt++;
		}
		/* length modifiers */
		for (;;)
		{
			if (*fmt == 'l')
			{
				longs++;
				fmt++;
			}
			else if (*fmt == 'h')
				fmt++;
			else if (*fmt == 'z' || *fmt == 'j' || *fmt == 't')
			{
				/* size_t and ptrdiff_t are 32 bits here; intmax_t is 64 */
				if (*fmt == 'j')
					longs = 2;
				fmt++;
			}
			else
				break;
		}

		switch (*fmt)
		{
		case '\0':
			continue;
		case 's':
			sb_puts(&sb, va_arg(ap, const char*));
			break;
		case 'c':
			sb_putc(&sb, (char)va_arg(ap, int));
			break;
		case 'd':
		case 'i':
			if (longs >= 2)
				sb_putd(&sb, va_arg(ap, long long), width, pad);
			else
				sb_putd(&sb, va_arg(ap, long), width, pad);
			break;
		case 'u':
			if (longs >= 2)
				sb_putu(&sb, va_arg(ap, unsigned long long), 10, 0, width, pad);
			else
				sb_putu(&sb, va_arg(ap, unsigned long), 10, 0, width, pad);
			break;
		case 'x':
		case 'X':
			if (longs >= 2)
				sb_putu(&sb, va_arg(ap, unsigned long long), 16,
						*fmt == 'X', width, pad);
			else
				sb_putu(&sb, va_arg(ap, unsigned long), 16,
						*fmt == 'X', width, pad);
			break;
		case 'p':
			sb_puts(&sb, "0x");
			sb_putu(&sb, (uint64_t)(uintptr_t)va_arg(ap, void*), 16, 0, 8, '0');
			break;
		case '%':
		default:
			sb_putc(&sb, *fmt);
			break;
		}
		fmt++;
	}

	if (size > 0)
		*sb.p = '\0';
	return (int)sb.count;
}

int exfat_amiga_snprintf(char* buf, size_t size, const char* fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = exfat_amiga_vsnprintf(buf, size, fmt, ap);
	va_end(ap);
	return n;
}

/* ------------------------------------------------------------------ */
/* Character set                                                      */
/* ------------------------------------------------------------------ */

/* libexfat hands us UTF-8 (decoded from the volume's UTF-16).  AmigaOS is
   8-bit, and its default character set is close enough to ISO-8859-1 to map
   the first 256 code points straight through.  Anything above that becomes
   '?', which makes the name un-lookupable - see ../CLAUDE.md, decision 3. */

#define EXFAT_AMIGA_SUBST '?'

static ULONG utf8_next(const char** s)
{
	const unsigned char* p = (const unsigned char*)*s;
	ULONG c = *p++;

	if (c < 0x80)
		;
	else if ((c & 0xE0) == 0xC0)
	{
		c = (c & 0x1F) << 6;
		c |= (*p++ & 0x3F);
	}
	else if ((c & 0xF0) == 0xE0)
	{
		c = (c & 0x0F) << 12;
		c |= (ULONG)(*p++ & 0x3F) << 6;
		c |= (*p++ & 0x3F);
	}
	else if ((c & 0xF8) == 0xF0)
	{
		c = (c & 0x07) << 18;
		c |= (ULONG)(*p++ & 0x3F) << 12;
		c |= (ULONG)(*p++ & 0x3F) << 6;
		c |= (*p++ & 0x3F);
	}
	else
		c = EXFAT_AMIGA_SUBST;	/* stray continuation byte */

	*s = (const char*)p;
	return c;
}

/* UTF-8 -> BSTR (length byte first), as ACTION_EXAMINE_* requires. */
void exfat_amiga_name_to_bstr(const char* utf8, UBYTE* bstr, int maxlen)
{
	int n = 0;

	while (*utf8 != '\0' && n < maxlen)
	{
		ULONG c = utf8_next(&utf8);

		bstr[1 + n] = (c < 0x100) ? (UBYTE)c : EXFAT_AMIGA_SUBST;
		n++;
	}
	bstr[0] = (UBYTE)n;
	/* dos.library converts the BSTR to a C string for Examine(); a trailing
	   NUL costs nothing and keeps debugging output sane. */
	if (n < maxlen)
		bstr[1 + n] = '\0';
}

/* 8-bit Amiga string -> UTF-8, for feeding paths back into libexfat. */
void exfat_amiga_cstr_to_utf8(const char* src, char* utf8, int maxlen)
{
	int n = 0;

	while (*src != '\0' && n < maxlen - 3)
	{
		unsigned char c = (unsigned char)*src++;

		if (c < 0x80)
			utf8[n++] = (char)c;
		else
		{
			utf8[n++] = (char)(0xC0 | (c >> 6));
			utf8[n++] = (char)(0x80 | (c & 0x3F));
		}
	}
	utf8[n] = '\0';
}

/* BSTR -> UTF-8. */
void exfat_amiga_bstr_to_utf8(const UBYTE* bstr, char* utf8, int maxlen)
{
	int len = bstr[0];
	int i;
	int n = 0;

	for (i = 0; i < len && n < maxlen - 3; i++)
	{
		unsigned char c = bstr[1 + i];

		if (c < 0x80)
			utf8[n++] = (char)c;
		else
		{
			utf8[n++] = (char)(0xC0 | (c >> 6));
			utf8[n++] = (char)(0x80 | (c & 0x3F));
		}
	}
	utf8[n] = '\0';
}

/* ------------------------------------------------------------------ */
/* Time                                                               */
/* ------------------------------------------------------------------ */

/* libexfat converts the on-disk exFAT timestamp to a time_t with a Unix
   epoch.  DateStamp counts from 1978-01-01.  The gap is 2922 days
   (8 years, of which 1972 and 1976 were leap years).

   Both sides are local time here: libexfat's timezone offset is forced to
   zero for this port (exfat_tzset() below), so the value we get back is the
   local time the volume recorded, which is what DateStamp wants.

   Caveat: time_t is a 32-bit long on this toolchain, so timestamps beyond
   2038 wrap.  exFAT can record up to 2107.  Noted in ../CLAUDE.md. */

#define SECS_PER_DAY	(24L * 60L * 60L)
#define DAYS_1970_TO_1978 2922L

void exfat_amiga_time_to_datestamp(time_t t, struct DateStamp* ds)
{
	LONG secs = (LONG)t;
	LONG days;

	if (secs < 0)
		secs = 0;

	days = secs / SECS_PER_DAY;
	secs -= days * SECS_PER_DAY;
	days -= DAYS_1970_TO_1978;

	if (days < 0)
	{
		days = 0;
		secs = 0;
	}

	ds->ds_Days = days;
	ds->ds_Minute = secs / 60;
	ds->ds_Tick = (secs % 60) * TICKS_PER_SECOND;
}

/* The inverse, for ACTION_SET_DATE.  DateStamp has no sub-second field
   beyond ticks, so the conversion is exact in both directions to the
   second. */
time_t exfat_amiga_datestamp_to_time(const struct DateStamp* ds)
{
	LONG days = ds->ds_Days;
	LONG secs;

	if (days < 0)
		days = 0;
	secs = ds->ds_Minute * 60L + ds->ds_Tick / TICKS_PER_SECOND;
	return (time_t)((days + DAYS_1970_TO_1978) * SECS_PER_DAY + secs);
}

/* libexfat calls this at mount time.  The upstream implementation uses
   tzset()/gmtime()/mktime(); we have no C run-time, and DateStamp is local
   time anyway, so keep the library's offset at zero and treat what the
   volume recorded as local time.  See ../CLAUDE.md, open decision 4. */
void exfat_tzset(void)
{
}

#ifndef EXFAT_HOSTTEST

/* ------------------------------------------------------------------ */
/* newlib stubs                                                       */
/* ------------------------------------------------------------------ */

/* node.c calls time() when it updates a node's mtime.  Implement it here
   rather than let newlib provide it: newlib's time() calls _gettimeofday_r,
   which pulls in _impure_ptr and from there the locale tables, stdio and
   malloc - all of which want a global SysBase that a ROM-resident handler
   cannot have. */
time_t time(time_t* t)
{
	EXFAT_SYSBASE;
	struct DosLibrary* DOSBase;
	struct DateStamp ds;
	time_t now = 0;

	/* No global DOSBase to borrow, and no context is passed in here.
	   Opening an already-open library is a list lookup and a refcount, and
	   this only runs when a write updates a file's timestamps. */
	DOSBase = (struct DosLibrary*)OpenLibrary("dos.library", 37);
	if (DOSBase != NULL)
	{
		DateStamp(&ds);
		CloseLibrary((struct Library*)DOSBase);
		now = (time_t)((ds.ds_Days + DAYS_1970_TO_1978) * SECS_PER_DAY +
				ds.ds_Minute * 60L + ds.ds_Tick / TICKS_PER_SECOND);
	}
	if (t != NULL)
		*t = now;
	return now;
}

/* Pulled in by newlib's exit(); nothing in the handler calls it.  A file
   system process must never simply vanish, so make the intent explicit. */
void _exit(int code)
{
	EXFAT_SYSBASE;
	(void)code;
	for (;;)
		Wait(0L);
}

/* ------------------------------------------------------------------ */
/* Allocator                                                          */
/* ------------------------------------------------------------------ */

/* libexfat allocates with malloc()/free()/calloc().  newlib's malloc needs a
   global SysBase, which a ROM-resident handler may not have, so provide our
   own on top of AllocVec().  A small header carries the size, because
   AllocVec() does not expose it and realloc() would need it - and it keeps
   the returned pointer longword aligned. */

#define ALLOC_HDR	8

void* malloc(size_t size)
{
	struct ExecBase* SysBase = *(struct ExecBase**)4UL;
	ULONG* p;

	if (size == 0)
		size = 1;
	p = AllocVec((ULONG)size + ALLOC_HDR, MEMF_ANY);
	if (p == NULL)
		return NULL;
	p[0] = (ULONG)size;
	return (void*)((UBYTE*)p + ALLOC_HDR);
}

void free(void* ptr)
{
	struct ExecBase* SysBase = *(struct ExecBase**)4UL;

	if (ptr != NULL)
		FreeVec((UBYTE*)ptr - ALLOC_HDR);
}

void* calloc(size_t count, size_t size)
{
	size_t total = count * size;
	void* p = malloc(total);

	if (p != NULL)
		memset(p, 0, total);
	return p;
}

void* realloc(void* ptr, size_t size)
{
	void* fresh;
	size_t old;

	if (ptr == NULL)
		return malloc(size);
	if (size == 0)
	{
		free(ptr);
		return NULL;
	}
	old = (size_t)((ULONG*)((UBYTE*)ptr - ALLOC_HDR))[0];
	fresh = malloc(size);
	if (fresh == NULL)
		return NULL;
	memcpy(fresh, ptr, (old < size) ? old : size);
	free(ptr);
	return fresh;
}

/* libexfat's option parser calls strtol().  newlib's pulls in _impure_ptr
   and the locale tables, which drag in ctype, stdio and finally malloc -
   all of which want a global SysBase that a ROM-resident handler cannot
   have.  This handles what get_int_option() needs and nothing more: optional
   whitespace, an optional sign, an optional 0x/0 prefix when base is 0. */
long strtol(const char* nptr, char** endptr, int base)
{
	const char* p = nptr;
	long value = 0;
	int negative = 0;
	int digits = 0;

	while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
		p++;
	if (*p == '-')
	{
		negative = 1;
		p++;
	}
	else if (*p == '+')
		p++;

	if ((base == 0 || base == 16) && p[0] == '0' &&
			(p[1] == 'x' || p[1] == 'X'))
	{
		base = 16;
		p += 2;
	}
	else if (base == 0 && p[0] == '0')
	{
		base = 8;
		p++;
	}
	else if (base == 0)
		base = 10;

	for (;;)
	{
		int d;

		if (*p >= '0' && *p <= '9')
			d = *p - '0';
		else if (*p >= 'a' && *p <= 'z')
			d = *p - 'a' + 10;
		else if (*p >= 'A' && *p <= 'Z')
			d = *p - 'A' + 10;
		else
			break;
		if (d >= base)
			break;
		value = value * base + d;
		digits++;
		p++;
	}

	if (endptr != NULL)
		*endptr = (char*)((digits > 0) ? p : nptr);
	return negative ? -value : value;
}

#endif /* !EXFAT_HOSTTEST */
