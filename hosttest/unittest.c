/*
	unittest.c

	Unit tests for the platform-independent parts of the AmigaOS port: the
	DateStamp conversion, the character set mapping and the hand-rolled
	printf subset.  These run natively on the build host, against the same
	source file the handler ships.

	Copyright (C) 2026  Willem Drijver

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 2 of the License, or
	(at your option) any later version.
*/

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "hostcompat.h"

static int failures;
static int checks;

static void ok(int cond, const char* what, const char* got, const char* want)
{
	checks++;
	if (cond)
		return;
	failures++;
	printf("  FAIL %s\n", what);
	if (got != NULL)
		printf("       got  %s\n       want %s\n", got, want);
}

/* ------------------------------------------------------------------ */
/* DateStamp conversion                                               */
/* ------------------------------------------------------------------ */

/* Independent reference: days from 1978-01-01 to the given date, computed
   with a different method than the code under test (civil-days algorithm),
   so a shared off-by-one cannot hide. */
static long days_from_civil(long y, unsigned m, unsigned d)
{
	y -= m <= 2;
	const long era = (y >= 0 ? y : y - 399) / 400;
	const unsigned yoe = (unsigned)(y - era * 400);
	const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + (long)doe - 719468;	/* days from 1970-01-01 */
}

static void check_date(const char* label, long y, unsigned mo, unsigned d,
		int hh, int mm, int ss)
{
	long unix_days = days_from_civil(y, mo, d);
	time_t t = (time_t)(unix_days * 86400L + hh * 3600L + mm * 60L + ss);
	long want_days = unix_days - days_from_civil(1978, 1, 1);
	struct DateStamp ds;
	char got[128], want[128];

	exfat_amiga_time_to_datestamp(t, &ds);
	snprintf(got, sizeof(got), "days %d minute %d tick %d",
			ds.ds_Days, ds.ds_Minute, ds.ds_Tick);
	snprintf(want, sizeof(want), "days %ld minute %d tick %d",
			want_days, hh * 60 + mm, ss * TICKS_PER_SECOND);
	ok(ds.ds_Days == want_days && ds.ds_Minute == hh * 60 + mm &&
			ds.ds_Tick == ss * TICKS_PER_SECOND, label, got, want);
}

static void test_datestamp(void)
{
	struct DateStamp ds;
	char got[64];

	printf("DateStamp conversion\n");

	/* The AmigaDOS epoch itself must land on day 0. */
	check_date("1978-01-01 00:00:00 is day 0", 1978, 1, 1, 0, 0, 0);
	/* A leap year between the two epochs is where an 8-year offset error
	   would show up. */
	check_date("1980-02-29 12:00:00", 1980, 2, 29, 12, 0, 0);
	check_date("2000-01-01 00:00:00", 2000, 1, 1, 0, 0, 0);
	check_date("2024-02-29 23:59:59", 2024, 2, 29, 23, 59, 59);
	check_date("2038-01-01 06:30:15", 2038, 1, 1, 6, 30, 15);

	/* ACTION_SET_DATE converts the other way; the pair must round trip to
	   the second, or copied files get wrong timestamps written back. */
	{
		static const long samples[] = {
			252460800L,	/* 1978-01-01 */
			315532800L,	/* 1980-01-01 */
			946684800L,	/* 2000-01-01 */
			1709164800L,	/* 2024-02-29 */
			2145916800L,	/* 2038-01-01 */
		};
		unsigned k;

		for (k = 0; k < sizeof(samples) / sizeof(samples[0]); k++)
		{
			struct DateStamp d;
			time_t want = (time_t)(samples[k] + 3661);
			time_t back;
			char g[64], w[64];

			exfat_amiga_time_to_datestamp(want, &d);
			back = exfat_amiga_datestamp_to_time(&d);
			snprintf(g, sizeof(g), "%ld", (long)back);
			snprintf(w, sizeof(w), "%ld", (long)want);
			ok(back == want, "time -> DateStamp -> time round trip", g, w);
		}
	}

	/* Anything before the AmigaDOS epoch has no representation; it must
	   clamp rather than produce a negative day count. */
	exfat_amiga_time_to_datestamp((time_t)0, &ds);	/* 1970-01-01 */
	snprintf(got, sizeof(got), "days %d", ds.ds_Days);
	ok(ds.ds_Days == 0 && ds.ds_Minute == 0 && ds.ds_Tick == 0,
			"pre-1978 clamps to day 0", got, "days 0");
}

/* ------------------------------------------------------------------ */
/* Character set                                                      */
/* ------------------------------------------------------------------ */

static void check_bstr(const char* label, const char* utf8,
		const char* want_bytes, int want_len)
{
	UBYTE bstr[128];
	char got[256], want[256];
	int i, n;

	memset(bstr, 0xAA, sizeof(bstr));
	exfat_amiga_name_to_bstr(utf8, bstr, 106);
	n = bstr[0];

	got[0] = '\0';
	for (i = 0; i < n && i < 40; i++)
		snprintf(got + strlen(got), sizeof(got) - strlen(got), "%02x ",
				bstr[1 + i]);
	want[0] = '\0';
	for (i = 0; i < want_len && i < 40; i++)
		snprintf(want + strlen(want), sizeof(want) - strlen(want), "%02x ",
				(unsigned char)want_bytes[i]);

	ok(n == want_len && memcmp(bstr + 1, want_bytes, want_len) == 0,
			label, got, want);
}

static void test_charset(void)
{
	char utf8[256];
	UBYTE bstr[128];

	printf("Character set\n");

	check_bstr("plain ASCII", "Kickstart.rom", "Kickstart.rom", 13);
	/* U+00E9 e-acute is inside Latin-1 and must map straight through */
	check_bstr("Latin-1 passes through", "caf\xC3\xA9", "caf\xE9", 4);
	/* U+20AC euro is outside Latin-1 and must become the substitute */
	check_bstr("above U+00FF substitutes", "a\xE2\x82\xAC" "b", "a?b", 3);
	/* a 4-byte sequence (U+1F600) is still one character */
	check_bstr("non-BMP substitutes as one char", "x\xF0\x9F\x98\x80y", "x?y", 3);
	check_bstr("empty name", "", "", 0);

	/* round trip for the representable range */
	exfat_amiga_cstr_to_utf8("caf\xE9", utf8, sizeof(utf8));
	ok(strcmp(utf8, "caf\xC3\xA9") == 0, "cstr_to_utf8 encodes Latin-1",
			utf8, "caf<c3><a9>");

	bstr[0] = 4;
	memcpy(bstr + 1, "caf\xE9", 4);
	exfat_amiga_bstr_to_utf8(bstr, utf8, sizeof(utf8));
	ok(strcmp(utf8, "caf\xC3\xA9") == 0, "bstr_to_utf8 encodes Latin-1",
			utf8, "caf<c3><a9>");

	/* the length byte must never exceed the cap we pass in */
	{
		char longname[400];

		memset(longname, 'x', sizeof(longname) - 1);
		longname[sizeof(longname) - 1] = '\0';
		memset(bstr, 0, sizeof(bstr));
		exfat_amiga_name_to_bstr(longname, bstr, 106);
		ok(bstr[0] == 106, "over-long name is clamped to the cap", NULL, NULL);
	}
}

/* ------------------------------------------------------------------ */
/* printf subset                                                      */
/* ------------------------------------------------------------------ */

static void check_fmt(const char* label, const char* want, const char* fmt, ...)
{
	char got[256];
	va_list ap;

	va_start(ap, fmt);
	exfat_amiga_vsnprintf(got, sizeof(got), fmt, ap);
	va_end(ap);
	ok(strcmp(got, want) == 0, label, got, want);
}

static void test_printf(void)
{
	char buf[8];
	int n;

	printf("printf subset\n");

	check_fmt("string", "hello world", "hello %s", "world");
	check_fmt("null string", "x(null)", "x%s", (char*)NULL);
	check_fmt("char", "[A]", "[%c]", 'A');
	check_fmt("signed long", "-42", "%ld", (long)-42);
	check_fmt("unsigned long", "4294967295", "%lu", (unsigned long)4294967295UL);
	check_fmt("hex lower", "deadbeef", "%lx", (unsigned long)0xDEADBEEF);
	check_fmt("hex upper", "DEADBEEF", "%lX", (unsigned long)0xDEADBEEF);
	check_fmt("zero pad width", "007b", "%04lx", (unsigned long)123);
	check_fmt("space pad width", "  42", "%4ld", (long)42);
	check_fmt("percent", "100%", "100%%");
	/* libexfat uses PRIu64 for cluster and size values */
	check_fmt("64-bit unsigned", "255835767808", "%llu",
			(unsigned long long)255835767808ULL);
	check_fmt("64-bit hex", "3b9aca00", "%llx", (unsigned long long)1000000000ULL);
	check_fmt("size_t", "1048576", "%zu", (size_t)1048576);
	check_fmt("mixed", "cluster 4 of 975808", "cluster %lu of %lu",
			(unsigned long)4, (unsigned long)975808);

	/* truncation must not overrun and must still terminate */
	n = exfat_amiga_snprintf(buf, sizeof(buf), "%s", "0123456789");
	ok(strcmp(buf, "0123456") == 0 && buf[7] == '\0',
			"truncates and NUL terminates", buf, "0123456");
	ok(n == 10, "returns the length it would have written", NULL, NULL);
}

int main(void)
{
	printf("== host unit tests ==\n\n");
	test_datestamp();
	test_charset();
	test_printf();
	printf("\n%d checks, %d failures\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
