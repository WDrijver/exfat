##
## exfat-handler - exFAT file system for AmigaOS 3.2
##
## A port of the free exFAT implementation (relan/exfat, GPL-2.0) to an
## AmigaDOS packet handler.  libexfat/ is the unmodified format engine apart
## from the platform branches; everything AmigaOS-specific lives here.
##
## make        -> build/exfat-handler
## make clean
##
## Override the version with e.g.  make VERSION=0.2
## Extra flags:  make EXTRA_CFLAGS=-DAPOLLO_DEBUG
## (do not set CFLAGS on the command line: that discards the flags below)
##

PROJECT  = exfat-handler
TARGET   = exfat-handler
BUILDDIR = build
OBJDIR   = obj

export LC_ALL := C

# Bumped on EVERY commit that changes the handler, so the $VER line in
# the serial log and the rt_Version byte in the ROM say which build is
# actually running.  1.0 is the first build in which ROM registration is
# expected to work (coldstart priority above sagasd.device).
VERSION ?= 1.1

BUILD_DATE := $(shell date +"%d.%m.%Y-%H.%M.%S")

BGREEN = \033[1;32m
GREEN  = \033[0;32m
NC     = \033[0m

# ---- toolchain ------------------------------------------------------
# NOTE: strip is always given --strip-debug, never a default/-R strip.
# creep-ltx/exfat-aos3 documents bebbo's m68k-amigaos-strip rewriting the
# hunk file WITHOUT its HUNK_RELOC32 blocks in default mode - every
# relocation silently dropped, giving a binary that tramples low memory and
# gurus at random.  This toolchain's strip does not do that (checked: the
# tools keep 749-927 relocations), but the failure is silent, so do not rely
# on it.  Verify with the reloc count printed by any hunk dumper.
PREFIX  = $(HOME)/ApolloCrossDev/Compilers/GCC-6.50-Dev
CC      = $(PREFIX)/bin/m68k-amigaos-gcc
LD      = $(PREFIX)/bin/m68k-amigaos-ld

# The authoritative OS API source for this workspace is the NDK 3.2 tree
# under docs/, not whatever the compiler ships (see the root CLAUDE.md).
WORKSPACE = ../../..
NDK       = $(WORKSPACE)/docs/os/NDK/NDK3.2/Include_H
APOLLOLIB = $(WORKSPACE)/docs/toolchain/_ApolloLib
LIBEXFAT  = ../libexfat
MKFS      = ../mkfs

# ---- flags ----------------------------------------------------------
CFLAGS  = -m68080 -m68881
CFLAGS += -Wall -Wno-multichar -Wno-pointer-sign -Wno-unused-value
CFLAGS += -Os -fomit-frame-pointer
CFLAGS += -I$(NDK) -I$(APOLLOLIB) -I$(LIBEXFAT) -I$(MKFS) -I.
CFLAGS += -DPROJECT_NAME=\"$(PROJECT)\"
CFLAGS += -DBUILD_DATE=$(BUILD_DATE)
CFLAGS += -DVERSION=\"$(VERSION)\"
CFLAGS += -DBUILD_DATE_STR=\"$(BUILD_DATE)\"

# The ROM tag needs the version as numbers, not as a string: rt_Version is a
# UBYTE and fse_Version packs version and revision into one longword.
VER_MAJOR = $(word 1,$(subst ., ,$(VERSION)))
VER_MINOR = $(word 2,$(subst ., ,$(VERSION)))
CFLAGS += -DEXFAT_VER=$(VER_MAJOR) -DEXFAT_REV=$(if $(VER_MINOR),$(VER_MINOR),0)

# Debug level for the ApolloCrossDev debug macros:
#   1 INFO, 2 WARN, 4 ERROR, 8 FLAG, 16 TRACE.
#
# What each costs in ROM (ROMREG=1, measured 2026-09-11):
#   none      42244     DEBUG=8  FLAG          42252
#   DEBUG=4   errors    45528    DEBUG=12 FLAG+ERROR  ~46000
#   DEBUG=6   +warn     49028    DEBUG=7  everything   51616
#
# FLAG is the ROM tag's own channel: exfat_rom_init's stage markers and
# nothing else, so it is nearly free.  For diagnosing ROM registration use
#   make clean && make ROMREG=1 DEBUG=12
# which reports every step of the tag and every error anywhere, and fits
# where DEBUG=7 does not.
ifdef DEBUG
CFLAGS  += -DDEBUG=$(DEBUG)
endif

# Write support.  0 = read-only (the write path is compiled out entirely).
# Do not set this to 1 against media you care about - see ../CLAUDE.md.
WRITE ?= 0
CFLAGS += -DEXFAT_AMIGA_ALLOW_WRITE=$(WRITE)

# Mount every partition found at startup (1) or leave them until first
# access (0).
ACTIVATE ?= 1
CFLAGS += -DEXFAT_AMIGA_ACTIVATE=$(ACTIVATE)

CFLAGS += $(EXTRA_CFLAGS)

# libgcc supplies the 64-bit divide/modulo helpers (__udivdi3, __umoddi3)
# that exFAT's cluster arithmetic needs on the 68k.
LIBGCC  = $(shell $(CC) -m68080 -m68881 -print-libgcc-file-name)
# libgcc's 64-bit divide helpers call __udivsi3, which lives in libc, so
# libc has to be searched again after libgcc.
LDFLAGS = -lc $(LIBGCC) -lc

# ---- sources --------------------------------------------------------
# entry.o MUST come first: AmigaDOS enters the handler at the first byte of
# the loaded segment.
AMIGA_SRCS   = handler.c dev_io.c amigaos.c parttable.c romtag.c
ifeq ($(WRITE),1)
AMIGA_SRCS  += format.c
endif
LIBEXFAT_SRCS = cluster.c io.c log.c lookup.c mount.c node.c repair.c time.c \
                utf.c utils.c
# the mkfs engine, minus its command line front end (main.c)
MKFS_SRCS    = cbm.c fat.c mkexfat.c rootdir.c uct.c uctc.c vbr.c
APOLLO_SRCS  = ApolloCrossDev_Debug.c

OBJ  = $(OBJDIR)/entry.o
OBJ += $(addprefix $(OBJDIR)/,$(AMIGA_SRCS:%.c=%.o))
OBJ += $(addprefix $(OBJDIR)/,$(LIBEXFAT_SRCS:%.c=%.o))
ifeq ($(WRITE),1)
OBJ += $(addprefix $(OBJDIR)/,$(MKFS_SRCS:%.c=%.o))
endif
OBJ += $(addprefix $(OBJDIR)/,$(APOLLO_SRCS:%.c=%.o))

.PHONY: all clean tools

all: $(BUILDDIR)/$(TARGET) $(BUILDDIR)/mbrscan $(BUILDDIR)/exfatctl $(BUILDDIR)/exfatres

# mbrscan is an ordinary CLI program, so it is linked with the compiler
# driver (which supplies the C startup) rather than ld directly.
$(BUILDDIR)/exfatctl: exfatctl.c
	@mkdir -p $(BUILDDIR)
	@printf "$(GREEN)$@$(NC)\n"
	@$(CC) $(CFLAGS) -DBASE_FILE_NAME=\"exfatctl\" -o $@ exfatctl.c -lc
	@$(PREFIX)/bin/m68k-amigaos-strip --strip-debug $@

$(BUILDDIR)/exfatres: exfatres.c
	@mkdir -p $(BUILDDIR)
	@printf "$(GREEN)$@$(NC)\n"
	@$(CC) $(CFLAGS) -DBASE_FILE_NAME=\"exfatres\" -o $@ exfatres.c -lc
	@$(PREFIX)/bin/m68k-amigaos-strip --strip-debug $@

$(BUILDDIR)/mbrscan: mbrscan.c
	@mkdir -p $(BUILDDIR)
	@printf "$(GREEN)$@$(NC)\n"
	@$(CC) $(CFLAGS) -DBASE_FILE_NAME=\"mbrscan\" -o $@ mbrscan.c -lc
	@$(PREFIX)/bin/m68k-amigaos-strip --strip-debug $@

$(BUILDDIR)/$(TARGET): $(OBJ)
	@mkdir -p $(BUILDDIR)
	@printf "$(BGREEN)Linking $(TARGET)$(NC)\n"
	@$(LD) -s -o $@ $(OBJ) $(LDFLAGS)
	@printf "Done. copy $@ L:exfat-handler; sagasd.device auto-mounts exFAT cards\n"

# Whether the ROM tag registers the handler in FileSystem.resource.
#
# Default 0, which is the configuration proven on hardware: the module sits
# in the ROM as a well-formed member and registers nothing, so mounting goes
# through L:exfat-handler.  ROMREG=1 registers 'FATX' - that path still
# freezes or fails to mount on the V4, see ../CLAUDE.md.
# NOTE: changing this (or WRITE, or DEBUG) only alters CFLAGS - make has no
# way to know the objects are stale, so it will relink without recompiling
# and hand you a binary built with the PREVIOUS setting.  Always "make clean"
# when changing a switch.  This has already produced one binary that was
# labelled ROMREG=1 and was not.
ROMREG ?= 0
ifneq ($(ROMREG),0)
CFLAGS += -DEXFAT_ROMTAG_REGISTER
endif

# The coldstart priority.  It has to be ABOVE sagasd.device's, which is 5:
# coldstart tags run in descending priority, and sagasd calls CheckUnit()
# straight from its init - ScanMBR() looks for FATX in FileSystem.resource
# right then, not after the daemon's five-second delay.  At the old
# default of 0 this module registered after that look, and the serial log
# said "exfat dostype not found" on every boot.  fat95 needed the same
# raise for the same reason.  10 leaves room either side.
PRI ?= 10
CFLAGS += -DEXFAT_ROM_PRI=$(PRI)

# When the tag runs.  Default is RTF_COLDSTART at the priority above.
# AFTERDOS=1 makes it RTF_AFTERDOS priority -100 instead, so
# registration happens after dos.library is up - the ROM boot process
# populates FileSystem.resource itself, and inserting into that list at
# coldstart races it.
AFTERDOS ?= 0
ifneq ($(AFTERDOS),0)
CFLAGS += -DEXFAT_ROM_AFTERDOS
endif

# Force the RAM segment stub (AllocMem + a hand-built JMP + CacheClearU)
# instead of fat95's "the module is its own segment list".  The fat95 form
# needs the module longword aligned in ROM, which it is on this machine, so
# it is the default; this switch reproduces the earlier build whose rt_Init
# ran to completion.
RAMSEG ?= 0
ifneq ($(RAMSEG),0)
CFLAGS += -DEXFAT_ROMTAG_RAMSEG
endif

# Diagnostic: build the FileSysEntry but never put it on the list.  If this
# boots and ROMREG=1 does not, the crash is caused by the entry's presence
# in FileSystem.resource rather than by anything rt_Init does.
NOADD ?= 0
ifneq ($(NOADD),0)
CFLAGS += -DEXFAT_ROMTAG_NOADD
endif

# ROM bring-up diagnostic: the smallest possible Kickstart module, ~200
# bytes of assembly with no C and no libc.  See the comment in romstub.S.
.PHONY: romstub
romstub: $(BUILDDIR)/romstub

# The stub carries the same tag priority as the handler, for the same
# reason: it stands in for the handler in ROM experiments and must run in
# the same slot of the coldstart order.
ROMSTUB_FLAGS += -DEXFAT_ROM_PRI=$(PRI)

# PAD=<bytes> grows the stub to a given size with incompressible filler, to
# tell "the ROM is out of space" apart from "the handler's content is wrong".
PAD ?= 0
ifneq ($(PAD),0)
ROMSTUB_FLAGS += -DROMSTUB_PAD=$(PAD)
endif

# RELOCS=<n> gives the stub n relocations, so its FILE size can be matched to
# the handler's and not just its CODE size.  The two differ by several KB.
RELOCS ?= 0
ifneq ($(RELOCS),0)
ROMSTUB_FLAGS += -DROMSTUB_RELOCS=$(RELOCS)
endif

# SPREADN=<n> SPREAD=<gap> gives the stub n relocations spaced <gap> bytes
# apart, so the relocation OFFSETS cover the whole module the way a compiled
# module's do.  RELOCS alone matches only the count.
SPREADN ?= 0
ifneq ($(SPREADN),0)
ROMSTUB_FLAGS += -DROMSTUB_SPREAD_N=$(SPREADN) -DROMSTUB_SPREAD=$(SPREAD)
endif

$(BUILDDIR)/romstub: romstub.S
	@mkdir -p $(BUILDDIR) $(OBJDIR)
	@printf "$(BGREEN)$@$(NC)\n"
	@python3 -c "import os,sys; n=int(sys.argv[1]); \
	 b=bytearray(os.urandom(n)) if n else bytearray(); \
	 [b.__setitem__(i, 0) for i in range(0, max(0,len(b)-1), 2) \
	   if b[i]==0x4a and b[i+1]==0xfc]; \
	 open('$(BUILDDIR)/romstub.pad','wb').write(bytes(b))" $(PAD)
	@$(CC) -m68080 $(ROMSTUB_FLAGS) -c romstub.S -o $(OBJDIR)/romstub.o
	@$(LD) -s -o $@ $(OBJDIR)/romstub.o
	@printf "  %s bytes\n" $$(wc -c < $@)

$(OBJDIR)/%.o: %.S
	@mkdir -p $(OBJDIR)
	@printf "$(GREEN)$@$(NC)\n"
	@$(CC) -o $@ -c $< $(CFLAGS)

$(OBJDIR)/%.o: %.c
	@mkdir -p $(OBJDIR)
	@printf "$(GREEN)$@$(NC)\n"
	@$(CC) -o $@ -c $< $(CFLAGS) -DBASE_FILE_NAME=\"$*\"

$(OBJDIR)/%.o: $(LIBEXFAT)/%.c
	@mkdir -p $(OBJDIR)
	@printf "$(GREEN)$@$(NC)\n"
	@$(CC) -o $@ -c $< $(CFLAGS) -DBASE_FILE_NAME=\"$*\"

$(OBJDIR)/%.o: $(MKFS)/%.c
	@mkdir -p $(OBJDIR)
	@printf "$(GREEN)$@$(NC)\n"
	@$(CC) -o $@ -c $< $(CFLAGS) -DBASE_FILE_NAME=\"$*\"

$(OBJDIR)/%.o: $(APOLLOLIB)/%.c
	@mkdir -p $(OBJDIR)
	@printf "$(GREEN)$@$(NC)\n"
	@$(CC) -o $@ -c $< $(CFLAGS) -DBASE_FILE_NAME=\"$*\"

clean:
	@-rm -rf $(OBJDIR) $(BUILDDIR)
