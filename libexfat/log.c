/*
	log.c (02.09.09)
	exFAT file system implementation library.

	Free exFAT implementation.
	Copyright (C) 2010-2023  Andrew Nayenko

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 2 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License along
	with this program; if not, write to the Free Software Foundation, Inc.,
	51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#include "exfat.h"
#include <stdarg.h>

#if defined(__amigaos__) || defined(AMIGA)

/* AmigaOS: there is no stdout/stderr in a file system process, so route the
   library's diagnostics through the ApolloCrossDev debug facility (serial /
   RawPutChar), the same one sagasd.device uses.  Upstream's printf-style
   format strings are rendered with vsnprintf() first because RawDoFmt() is
   not printf-compatible (it wants %ld where C wants %d). */

#include <proto/exec.h>
#include "ApolloCrossDev_Debug.h"	/* docs/toolchain/_ApolloLib, via -I */

/* A ROM-resident handler may have no writable static data at all.  The
   counter is only ever read by fsck and dump, neither of which is part of
   this build, so drop it rather than keep a word of BSS. */
#define exfat_errors_bump()	((void)0)

int exfat_amiga_vsnprintf(char* buf, size_t size, const char* fmt, va_list ap);

/* The render buffer is a caller's local, not a static: several handler
   processes share this file's data when the handler is resident in
   FileSystem.resource, and a shared buffer would interleave their messages.
   UNUSED because at DEBUG=0 every Debug_* macro expands to nothing. */
#define EXFAT_LOGBUF_SIZE 512

static UNUSED const char* exfat_render(char* buf, const char* format,
		va_list ap)
{
	exfat_amiga_vsnprintf(buf, EXFAT_LOGBUF_SIZE, format, ap);
	return buf;
}

void exfat_bug(const char* format, ...)
{
	UNUSED char buf[EXFAT_LOGBUF_SIZE];
	va_list ap;

	va_start(ap, format);
	Debug_Error("BUG: %s", exfat_render(buf, format, ap));
	va_end(ap);
	/* No abort() without a C run-time, and taking the machine down would
	   lose the serial log.  Stop this process instead; the volume becomes
	   unresponsive but the system survives and the reason is on the wire. */
	for (;;)
	{
		struct ExecBase* SysBase = *(struct ExecBase**)4UL;
		Wait(0L);
	}
}

void exfat_error(const char* format, ...)
{
	UNUSED char buf[EXFAT_LOGBUF_SIZE];
	va_list ap;

	exfat_errors_bump();
	va_start(ap, format);
	Debug_Error("%s", exfat_render(buf, format, ap));
	va_end(ap);
}

void exfat_warn(const char* format, ...)
{
	UNUSED char buf[EXFAT_LOGBUF_SIZE];
	va_list ap;

	va_start(ap, format);
	Debug_Warn("%s", exfat_render(buf, format, ap));
	va_end(ap);
}

void exfat_debug(const char* format, ...)
{
	UNUSED char buf[EXFAT_LOGBUF_SIZE];
	va_list ap;

	va_start(ap, format);
	Debug_Info("%s", exfat_render(buf, format, ap));
	va_end(ap);
}

#else /* !AmigaOS - upstream POSIX implementation */

#define exfat_errors_bump()	(exfat_errors++)

#ifdef __ANDROID__
#include <android/log.h>
#else
#include <syslog.h>
#endif
#include <unistd.h>

int exfat_errors;

/*
 * This message means an internal bug in exFAT implementation.
 */
void exfat_bug(const char* format, ...)
{
	va_list ap, aq;

	va_start(ap, format);
	va_copy(aq, ap);

	fflush(stdout);
	fputs("BUG: ", stderr);
	vfprintf(stderr, format, ap);
	va_end(ap);
	fputs(".\n", stderr);

#ifdef __ANDROID__
	__android_log_vprint(ANDROID_LOG_FATAL, PACKAGE, format, aq);
#else
	if (!isatty(STDERR_FILENO))
		vsyslog(LOG_CRIT, format, aq);
#endif
	va_end(aq);

	abort();
}

/*
 * This message means an error in exFAT file system.
 */
void exfat_error(const char* format, ...)
{
	va_list ap, aq;

	exfat_errors_bump();
	va_start(ap, format);
	va_copy(aq, ap);

	fflush(stdout);
	fputs("ERROR: ", stderr);
	vfprintf(stderr, format, ap);
	va_end(ap);
	fputs(".\n", stderr);

#ifdef __ANDROID__
	__android_log_vprint(ANDROID_LOG_ERROR, PACKAGE, format, aq);
#else
	if (!isatty(STDERR_FILENO))
		vsyslog(LOG_ERR, format, aq);
#endif
	va_end(aq);
}

/*
 * This message means that there is something unexpected in exFAT file system
 * that can be a potential problem.
 */
void exfat_warn(const char* format, ...)
{
	va_list ap, aq;

	va_start(ap, format);
	va_copy(aq, ap);

	fflush(stdout);
	fputs("WARN: ", stderr);
	vfprintf(stderr, format, ap);
	va_end(ap);
	fputs(".\n", stderr);

#ifdef __ANDROID__
	__android_log_vprint(ANDROID_LOG_WARN, PACKAGE, format, aq);
#else
	if (!isatty(STDERR_FILENO))
		vsyslog(LOG_WARNING, format, aq);
#endif
	va_end(aq);
}

/*
 * Just debug message. Disabled by default.
 */
void exfat_debug(const char* format, ...)
{
	va_list ap, aq;

	va_start(ap, format);
	va_copy(aq, ap);

	fflush(stdout);
	fputs("DEBUG: ", stderr);
	vfprintf(stderr, format, ap);
	va_end(ap);
	fputs(".\n", stderr);

#ifdef __ANDROID__
	__android_log_vprint(ANDROID_LOG_DEBUG, PACKAGE, format, aq);
#else
	if (!isatty(STDERR_FILENO))
		vsyslog(LOG_DEBUG, format, aq);
#endif
	va_end(aq);
}

#endif /* !AmigaOS */
