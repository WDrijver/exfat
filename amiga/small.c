/* small.c - lighter replacements for library routines the handler links.

   Linked ahead of libgcc and libc, so these definitions win.  Together they
   take 6.1 KB off the DEBUG=0 build (measured 2026-09-16: 59,128 ->
   53,012) and change nothing the handler does.

   64-bit division.  libgcc's __udivdi3 / __umoddi3 / __divdi3 are the
   generic versions written for a 68000 without a 32-bit divide: 1.4 KB
   each plus __clz, 4.6 KB in all.  exFAT's cluster arithmetic divides
   64-bit offsets and sizes by the sector and cluster size, once per file
   system operation, never per byte.  The 32-bit fast path below is one
   divu.l on the 080 and covers every offset under 4 GB; the shift-subtract
   loop behind it costs about a microsecond, next to a card access that
   costs milliseconds.

   strstr.  libc's is the two-way algorithm, 1.6 KB, and the handler calls
   it once - to parse "ro_fallback,noatime" at mount time. */

#include <stddef.h>

typedef unsigned long long u64;
typedef long long s64;

static u64 udivmod64(u64 n, u64 d, u64* rem)
{
	u64 q = 0, r = 0;
	int i;

	if (d == 0)
	{
		/* libgcc would raise a divide-by-zero trap here.  There is no
		   caller in this handler that can pass zero - the divisors are
		   sector and cluster sizes from a validated superblock - so
		   answer 0 rather than take the machine down from a file system
		   process. */
		if (rem != NULL)
			*rem = 0;
		return 0;
	}
	if ((n >> 32) == 0 && (d >> 32) == 0)
	{
		unsigned long a = (unsigned long)n, b = (unsigned long)d;

		if (rem != NULL)
			*rem = a % b;
		return a / b;
	}
	for (i = 63; i >= 0; i--)
	{
		r = (r << 1) | ((n >> i) & 1);
		if (r >= d)
		{
			r -= d;
			q |= 1ULL << i;
		}
	}
	if (rem != NULL)
		*rem = r;
	return q;
}

u64 __udivdi3(u64 n, u64 d)
{
	return udivmod64(n, d, NULL);
}

u64 __umoddi3(u64 n, u64 d)
{
	u64 r;

	udivmod64(n, d, &r);
	return r;
}

s64 __divdi3(s64 n, s64 d)
{
	int neg = 0;
	u64 q;

	if (n < 0) { n = -n; neg ^= 1; }
	if (d < 0) { d = -d; neg ^= 1; }
	q = udivmod64((u64)n, (u64)d, NULL);
	return neg ? -(s64)q : (s64)q;
}

char* strstr(const char* haystack, const char* needle)
{
	size_t nlen = 0;

	while (needle[nlen] != '\0')
		nlen++;
	if (nlen == 0)
		return (char*)haystack;
	for (; *haystack != '\0'; haystack++)
	{
		size_t i = 0;

		while (i < nlen && haystack[i] == needle[i])
			i++;
		if (i == nlen)
			return (char*)haystack;
	}
	return NULL;
}
