/*
	fstest.c

	Drives libexfat against a file-backed disk image, using the same call
	sequences the AmigaOS handler uses.  Each scenario runs on a fresh image
	and is followed by exfatfsck (see run-tests.sh), so allocation, FAT chain
	and directory entry set bugs surface on the build host rather than on a
	card.

	NOTE: the host is little-endian.  This exercises the format LOGIC only -
	it does not test the byte swapping, the Exec device layer, de_MaxTransfer
	chunking or the unaligned read-modify-write in ../amiga/dev_io.c.  Those
	still need hardware.

	Copyright (C) 2026  Willem Drijver

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 2 of the License, or
	(at your option) any later version.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "exfat.h"

static struct exfat ef;
static int failures;

#define CHECK(cond, ...) \
	do { \
		if (!(cond)) { \
			printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); \
			failures++; \
		} \
	} while (0)

/* ------------------------------------------------------------------ */
/* helpers mirroring the handler's packet sequences                   */
/* ------------------------------------------------------------------ */

/* ACTION_FINDOUTPUT: create then look up (fuse_exfat_create does the same) */
static struct exfat_node* create_file(const char* path)
{
	struct exfat_node* node = NULL;
	int rc = exfat_mknod(&ef, path);

	if (rc != 0)
	{
		printf("  mknod %s failed: %d\n", path, rc);
		return NULL;
	}
	rc = exfat_lookup(&ef, &node, path);
	if (rc != 0)
	{
		printf("  lookup %s failed: %d\n", path, rc);
		return NULL;
	}
	return node;
}

/* ACTION_WRITE */
static int write_at(struct exfat_node* node, const void* buf, size_t size,
		uint64_t offset)
{
	ssize_t n = exfat_generic_pwrite(&ef, node, buf, size, (exfat_off_t)offset);

	return (n == (ssize_t)size) ? 0 : -1;
}

/* ACTION_END */
static void close_file(struct exfat_node* node)
{
	exfat_flush_node(&ef, node);
	exfat_put_node(&ef, node);
}

/* ACTION_DELETE_OBJECT (fuse_exfat_unlink) */
static int delete_file(const char* path)
{
	struct exfat_node* node;
	int rc = exfat_lookup(&ef, &node, path);

	if (rc != 0)
		return rc;
	rc = exfat_unlink(&ef, node);
	exfat_put_node(&ef, node);
	if (rc != 0)
		return rc;
	return exfat_cleanup_node(&ef, node);
}

/* Read a file back and compare against a pattern. */
static int verify(const char* path, const unsigned char* want, size_t size,
		uint64_t offset)
{
	struct exfat_node* node;
	unsigned char* got;
	ssize_t n;
	int rc = exfat_lookup(&ef, &node, path);

	if (rc != 0)
	{
		printf("  lookup %s failed: %d\n", path, rc);
		return -1;
	}
	got = malloc(size);
	if (got == NULL)
	{
		exfat_put_node(&ef, node);
		return -1;
	}
	memset(got, 0, size);
	n = exfat_generic_pread(&ef, node, got, size, (exfat_off_t)offset);
	rc = 0;
	if (n != (ssize_t)size)
	{
		printf("  short read of %s: %ld of %lu\n", path, (long)n,
				(unsigned long)size);
		rc = -1;
	}
	else if (memcmp(got, want, size) != 0)
	{
		size_t i;

		for (i = 0; i < size; i++)
			if (got[i] != want[i])
				break;
		printf("  %s differs at offset %lu: got %02x want %02x\n", path,
				(unsigned long)i, got[i], want[i]);
		rc = -1;
	}
	free(got);
	exfat_put_node(&ef, node);
	return rc;
}

static void fill_pattern(unsigned char* buf, size_t size, unsigned seed)
{
	size_t i;

	for (i = 0; i < size; i++)
		buf[i] = (unsigned char)((i * 31 + seed) & 0xFF);
}

static uint64_t file_size(const char* path)
{
	struct exfat_node* node;
	uint64_t size;

	if (exfat_lookup(&ef, &node, path) != 0)
		return (uint64_t)-1;
	size = node->size;
	exfat_put_node(&ef, node);
	return size;
}

/* ------------------------------------------------------------------ */
/* scenarios                                                          */
/* ------------------------------------------------------------------ */

/* One write smaller than a cluster. */
static void sc_small(void)
{
	unsigned char buf[1000];
	struct exfat_node* node;

	fill_pattern(buf, sizeof(buf), 1);
	node = create_file("/small.bin");
	CHECK(node != NULL, "create /small.bin");
	if (node == NULL)
		return;
	CHECK(write_at(node, buf, sizeof(buf), 0) == 0, "write 1000 bytes");
	close_file(node);
	CHECK(file_size("/small.bin") == sizeof(buf), "size is 1000");
	CHECK(verify("/small.bin", buf, sizeof(buf), 0) == 0, "contents match");
}

/* A write that crosses cluster boundaries. */
static void sc_spanning(void)
{
	size_t size = CLUSTER_SIZE(*ef.sb) * 3 + 1234;
	unsigned char* buf = malloc(size);
	struct exfat_node* node;

	CHECK(buf != NULL, "allocate %lu bytes", (unsigned long)size);
	if (buf == NULL)
		return;
	fill_pattern(buf, size, 2);
	node = create_file("/span.bin");
	CHECK(node != NULL, "create /span.bin");
	if (node != NULL)
	{
		CHECK(write_at(node, buf, size, 0) == 0, "write %lu bytes",
				(unsigned long)size);
		close_file(node);
		CHECK(file_size("/span.bin") == size, "size matches");
		CHECK(verify("/span.bin", buf, size, 0) == 0, "contents match");
	}
	free(buf);
}

/* Write, then extend by writing past the end - exercises ValidDataLength. */
static void sc_grow(void)
{
	unsigned char a[512], b[512], zero[512];
	struct exfat_node* node;
	uint64_t gap = CLUSTER_SIZE(*ef.sb) * 2;

	fill_pattern(a, sizeof(a), 3);
	fill_pattern(b, sizeof(b), 4);
	memset(zero, 0, sizeof(zero));

	node = create_file("/grow.bin");
	CHECK(node != NULL, "create /grow.bin");
	if (node == NULL)
		return;
	CHECK(write_at(node, a, sizeof(a), 0) == 0, "write at 0");
	/* leave a hole, then write past it */
	CHECK(write_at(node, b, sizeof(b), gap) == 0, "write past a hole");
	close_file(node);

	CHECK(file_size("/grow.bin") == gap + sizeof(b), "size spans the hole");
	CHECK(verify("/grow.bin", a, sizeof(a), 0) == 0, "first block intact");
	CHECK(verify("/grow.bin", b, sizeof(b), gap) == 0, "second block intact");
	/* The hole must read as zeroes, never as stale media content - this is
	   the ValidDataLength rule from spec section 7.6.5. */
	CHECK(verify("/grow.bin", zero, sizeof(zero), sizeof(a)) == 0,
			"hole reads as zeroes, not stale data");
}

/* Truncate down, then check the file really shrank. */
static void sc_shrink(void)
{
	size_t size = CLUSTER_SIZE(*ef.sb) * 4;
	unsigned char* buf = malloc(size);
	struct exfat_node* node;

	if (buf == NULL)
		return;
	fill_pattern(buf, size, 5);
	node = create_file("/shrink.bin");
	CHECK(node != NULL, "create /shrink.bin");
	if (node != NULL)
	{
		CHECK(write_at(node, buf, size, 0) == 0, "write %lu bytes",
				(unsigned long)size);
		CHECK(exfat_truncate(&ef, node, 100, true) == 0, "truncate to 100");
		close_file(node);
		CHECK(file_size("/shrink.bin") == 100, "size is 100");
		CHECK(verify("/shrink.bin", buf, 100, 0) == 0, "head preserved");
	}
	free(buf);
}

/* Directories: create, populate, nest. */
static void sc_dirs(void)
{
	unsigned char buf[300];
	struct exfat_node* node;

	fill_pattern(buf, sizeof(buf), 6);
	CHECK(exfat_mkdir(&ef, "/dir") == 0, "mkdir /dir");
	CHECK(exfat_mkdir(&ef, "/dir/sub") == 0, "mkdir /dir/sub");
	node = create_file("/dir/sub/file.bin");
	CHECK(node != NULL, "create nested file");
	if (node != NULL)
	{
		CHECK(write_at(node, buf, sizeof(buf), 0) == 0, "write nested file");
		close_file(node);
		CHECK(verify("/dir/sub/file.bin", buf, sizeof(buf), 0) == 0,
				"nested contents match");
	}
	/* a directory that still has children must not be removable */
	{
		struct exfat_node* d;

		CHECK(exfat_lookup(&ef, &d, "/dir/sub") == 0, "lookup /dir/sub");
		CHECK(exfat_rmdir(&ef, d) != 0, "rmdir refuses a non-empty directory");
		exfat_put_node(&ef, d);
	}
}

/* Rename within a directory and across directories. */
static void sc_rename(void)
{
	unsigned char buf[777];
	struct exfat_node* node;

	fill_pattern(buf, sizeof(buf), 7);
	CHECK(exfat_mkdir(&ef, "/from") == 0, "mkdir /from");
	CHECK(exfat_mkdir(&ef, "/to") == 0, "mkdir /to");
	node = create_file("/from/a.bin");
	CHECK(node != NULL, "create /from/a.bin");
	if (node == NULL)
		return;
	CHECK(write_at(node, buf, sizeof(buf), 0) == 0, "write");
	close_file(node);

	CHECK(exfat_rename(&ef, "/from/a.bin", "/from/b.bin") == 0,
			"rename within a directory");
	CHECK(verify("/from/b.bin", buf, sizeof(buf), 0) == 0,
			"contents survive rename");
	CHECK(exfat_rename(&ef, "/from/b.bin", "/to/c.bin") == 0,
			"move to another directory");
	CHECK(verify("/to/c.bin", buf, sizeof(buf), 0) == 0,
			"contents survive the move");
	CHECK(file_size("/from/b.bin") == (uint64_t)-1, "old name is gone");
}

/* Delete files and directories. */
static void sc_delete(void)
{
	unsigned char buf[64];
	struct exfat_node* node;
	struct exfat_node* d;

	fill_pattern(buf, sizeof(buf), 8);
	CHECK(exfat_mkdir(&ef, "/gone") == 0, "mkdir /gone");
	node = create_file("/gone/f.bin");
	CHECK(node != NULL, "create /gone/f.bin");
	if (node != NULL)
	{
		CHECK(write_at(node, buf, sizeof(buf), 0) == 0, "write");
		close_file(node);
	}
	CHECK(delete_file("/gone/f.bin") == 0, "delete the file");
	CHECK(file_size("/gone/f.bin") == (uint64_t)-1, "file is gone");
	CHECK(exfat_lookup(&ef, &d, "/gone") == 0, "lookup /gone");
	CHECK(exfat_rmdir(&ef, d) == 0, "rmdir the now-empty directory");
	exfat_put_node(&ef, d);
	CHECK(exfat_cleanup_node(&ef, d) == 0, "cleanup");
}

/* Names: long, and outside ASCII. */
static void sc_names(void)
{
	static const char* names[] = {
		"/0123456789012345678901234567890123456789012345678901234567890123456789.bin",
		"/caf\xC3\xA9-\xC3\xBC\xC3\xB1\xC3\xAF.bin",	/* Latin-1 range */
		"/a b c with spaces.bin",
		"/UPPERlower.MiXeD",
	};
	unsigned char buf[128];
	unsigned i;

	fill_pattern(buf, sizeof(buf), 9);
	for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
	{
		struct exfat_node* node = create_file(names[i]);

		CHECK(node != NULL, "create %s", names[i]);
		if (node == NULL)
			continue;
		CHECK(write_at(node, buf, sizeof(buf), 0) == 0, "write %s", names[i]);
		close_file(node);
		CHECK(verify(names[i], buf, sizeof(buf), 0) == 0, "read %s", names[i]);
	}
}

/* Fragmentation: punch a hole in the allocation, then grow into it. */
static void sc_fragment(void)
{
	size_t chunk = CLUSTER_SIZE(*ef.sb) * 2;
	unsigned char* buf = malloc(chunk);
	struct exfat_node* node;

	if (buf == NULL)
		return;
	fill_pattern(buf, chunk, 10);

	node = create_file("/a.bin");
	if (node != NULL) { write_at(node, buf, chunk, 0); close_file(node); }
	node = create_file("/b.bin");
	if (node != NULL) { write_at(node, buf, chunk, 0); close_file(node); }
	node = create_file("/c.bin");
	if (node != NULL) { write_at(node, buf, chunk, 0); close_file(node); }

	CHECK(delete_file("/b.bin") == 0, "delete the middle file");

	/* /a.bin now has to grow into the freed, non-contiguous space */
	CHECK(exfat_lookup(&ef, &node, "/a.bin") == 0, "lookup /a.bin");
	if (node != NULL)
	{
		CHECK(write_at(node, buf, chunk, chunk) == 0, "grow into the hole");
		close_file(node);
		CHECK(file_size("/a.bin") == chunk * 2, "size doubled");
		CHECK(verify("/a.bin", buf, chunk, 0) == 0, "first half intact");
		CHECK(verify("/a.bin", buf, chunk, chunk) == 0, "second half intact");
	}
	free(buf);
}

/* Keep writing until the volume is full; it must fail cleanly, not corrupt. */
static void sc_fill(void)
{
	size_t chunk = CLUSTER_SIZE(*ef.sb);
	unsigned char* buf = malloc(chunk);
	int i;
	int hit_enospc = 0;

	if (buf == NULL)
		return;
	fill_pattern(buf, chunk, 11);

	for (i = 0; i < 100000 && !hit_enospc; i++)
	{
		char path[64];
		struct exfat_node* node;

		sprintf(path, "/fill%05d.bin", i);
		if (exfat_mknod(&ef, path) != 0)
		{
			hit_enospc = 1;
			break;
		}
		if (exfat_lookup(&ef, &node, path) != 0)
			break;
		if (write_at(node, buf, chunk, 0) != 0)
			hit_enospc = 1;
		close_file(node);
	}
	CHECK(hit_enospc, "running out of space is reported, not ignored");
	printf("  (filled with %d files of %lu bytes)\n", i, (unsigned long)chunk);
	free(buf);
}

/* ------------------------------------------------------------------ */

static const struct
{
	const char* name;
	void (*fn)(void);
}
scenarios[] =
{
	{ "small",    sc_small },
	{ "spanning", sc_spanning },
	{ "grow",     sc_grow },
	{ "shrink",   sc_shrink },
	{ "dirs",     sc_dirs },
	{ "rename",   sc_rename },
	{ "delete",   sc_delete },
	{ "names",    sc_names },
	{ "fragment", sc_fragment },
	{ "fill",     sc_fill },
};

int main(int argc, char** argv)
{
	unsigned i;
	int found = 0;

	if (argc != 3)
	{
		printf("usage: fstest <image> <scenario|all>\n");
		printf("scenarios:");
		for (i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++)
			printf(" %s", scenarios[i].name);
		printf("\n");
		return 2;
	}

	if (exfat_mount(&ef, argv[1], "") != 0)
	{
		printf("cannot mount %s\n", argv[1]);
		return 2;
	}

	for (i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++)
	{
		if (strcmp(argv[2], "all") == 0 ||
				strcmp(argv[2], scenarios[i].name) == 0)
		{
			found = 1;
			scenarios[i].fn();
		}
	}

	exfat_unmount(&ef);

	if (!found)
	{
		printf("unknown scenario '%s'\n", argv[2]);
		return 2;
	}
	return failures == 0 ? 0 : 1;
}
