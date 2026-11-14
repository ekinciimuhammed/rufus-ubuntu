# Rufus for Linux - build system
# Copyright © 2026 Rufus Linux port contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Usage:
#   make                 build ./build/rufus
#   make SYSROOT=<dir>   build against development headers extracted in <dir>
#                        (see tools/make-sysroot.sh; build/sysroot is used automatically)
#   make deb             build the Debian package (see packaging/build-deb.sh)
#   make install DESTDIR=<dir> PREFIX=/usr
#   make check           run the end to end tests (as root, on loop devices, see tests/run-tests.sh)

TOP       := $(abspath ..)
SRC       := $(TOP)/src
LNX       := $(TOP)/linux
BUILD     ?= $(LNX)/build
PREFIX    ?= /usr
DATADIR   ?= $(PREFIX)/share/rufus
VERSION   := $(shell sh $(LNX)/tools/version.sh $(SRC)/rufus.rc)

CC        ?= gcc
PKGS      := gtk+-3.0

# Use the sysroot created by tools/make-sysroot.sh when the -dev packages aren't installed
ifeq ($(SYSROOT),)
ifeq ($(shell pkg-config --exists gtk+-3.0 && echo yes),)
ifneq ($(wildcard $(BUILD)/sysroot/root/usr/include),)
SYSROOT := $(BUILD)/sysroot/root
endif
endif
endif

ifneq ($(SYSROOT),)
export PKG_CONFIG_SYSROOT_DIR := $(SYSROOT)
export PKG_CONFIG_LIBDIR := $(SYSROOT)/usr/lib/x86_64-linux-gnu/pkgconfig:$(SYSROOT)/usr/share/pkgconfig:/usr/share/pkgconfig
SYSROOT_LDFLAGS := -L$(SYSROOT)/usr/lib/x86_64-linux-gnu -Wl,-rpath-link,$(SYSROOT)/usr/lib/x86_64-linux-gnu
SYSROOT_CFLAGS := -idirafter $(SYSROOT)/usr/include -idirafter $(SYSROOT)/usr/include/x86_64-linux-gnu
endif

PKG_CFLAGS := $(shell pkg-config --cflags $(PKGS))
PKG_LIBS   := $(shell pkg-config --libs $(PKGS))

# NB: like on Windows, assertions are always enabled in Rufus
COMMON_FLAGS := -std=gnu11 -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE -UNDEBUG \
	-D_RUFUS -DRUFUS_LINUX -DEXT2_FLAT_INCLUDES=0 -DSOLUTION=rufus \
	-DRUFUS_DATADIR=\"$(DATADIR)\" -DRUFUS_VERSION_STR=\"$(VERSION)\" \
	-I$(LNX)/include -I$(LNX)/compat -I$(LNX)/core -I$(LNX)/ui -I$(BUILD) -I$(SRC) -I$(SRC)/ms-sys/inc \
	-I$(SRC)/syslinux/libfat -I$(SRC)/syslinux/libinstaller -I$(SRC)/syslinux/win \
	-I$(SRC)/libcdio -I$(TOP)/res \
	-O2 -g -pipe -fno-strict-aliasing -fstack-protector-strong -D_FORTIFY_SOURCE=2 \
	-Wall -Wno-unused-function -Wno-pointer-sign -Wno-unknown-pragmas -Wno-format-truncation \
	-Wno-stringop-truncation -Wno-misleading-indentation -Wno-multichar -Wno-address-of-packed-member \
	-Wno-unused-but-set-variable -Wno-unused-variable -Wno-missing-braces \
	-Wno-unused-label -Wno-unused-value -Wno-parentheses -Wno-sign-compare
CFLAGS   ?=
CFLAGS   += $(COMMON_FLAGS) $(PKG_CFLAGS) $(SYSROOT_CFLAGS)
LDFLAGS  += $(SYSROOT_LDFLAGS) -pthread
LIBS     := $(PKG_LIBS) -lcurl -lcrypto -lwim -lm

# Third-party libraries bundled with Rufus (built unmodified)
CDIO_SRC := $(addprefix $(SRC)/libcdio/iso9660/,iso9660.c iso9660_fs.c rock.c xa.c) \
	$(addprefix $(SRC)/libcdio/udf/,udf.c udf_file.c udf_fs.c udf_time.c filemode.c) \
	$(addprefix $(SRC)/libcdio/driver/,disc.c ds.c logging.c memory.c read.c sector.c track.c \
	util.c _cdio_stdio.c _cdio_stream.c utf8.c)
BLED_SRC := $(addprefix $(SRC)/bled/,bled.c bled_size.c crc32.c data_align.c data_extract_all.c \
	data_skip.c decompress_bunzip2.c decompress_gunzip.c decompress_uncompress.c \
	decompress_unlzma.c decompress_unxz.c decompress_unzip.c decompress_unzstd.c decompress_vtsi.c \
	filter_accept_all.c filter_accept_list.c filter_accept_reject_list.c find_list_entry.c \
	fse_decompress.c header_list.c header_skip.c header_verbose_list.c huf_decompress.c \
	init_handle.c open_transformer.c seek_by_jump.c seek_by_read.c xxhash.c zstd_common.c zstd_ddict.c zstd_decompress.c \
	zstd_decompress_block.c zstd_entropy_common.c zstd_error_private.c)
MSSYS_SRC := $(addprefix $(SRC)/ms-sys/,br.c fat12.c fat16.c fat32.c file.c ntfs.c partition_info.c)
SYSL_SRC := $(addprefix $(SRC)/syslinux/libfat/,cache.c fatchain.c open.c searchdir.c dumpdir.c) \
	$(addprefix $(SRC)/syslinux/libinstaller/,fs.c setadv.c syslxmod.c)

# Rufus' own platform independent modules (shared with the Windows build)
RUFUS_SRC := $(addprefix $(SRC)/,iso.c hash.c badblocks.c dos.c dos_locale.c syslinux.c vhd.c \
	wue.c xml.c cregex_compile.c cregex_parse.c cregex_vm.c localization.c parser.c icon.c \
	stdio.c stdfn.c drive.c format.c format_fat32.c rufus.c)

# Linux platform layer
LINUX_SRC := $(wildcard $(LNX)/compat/*.c) $(wildcard $(LNX)/core/*.c)
UI_SRC    := $(wildcard $(LNX)/ui/*.c)

ALL_SRC := $(CDIO_SRC) $(BLED_SRC) $(MSSYS_SRC) $(SYSL_SRC) $(RUFUS_SRC) $(LINUX_SRC) $(UI_SRC)
OBJS    := $(patsubst $(TOP)/%.c,$(BUILD)/obj/%.o,$(ALL_SRC))
DEPS    := $(OBJS:.o=.d)

.PHONY: all clean install deb check
all: $(BUILD)/rufus

$(BUILD)/resources.h: $(SRC)/rufus.rc $(LNX)/tools/gen_resources.sh
	@mkdir -p $(BUILD)
	@mkdir -p $(dir $@)
	sh $(LNX)/tools/gen_resources.sh $< > $@

$(BUILD)/obj/%.o: $(TOP)/%.c $(BUILD)/resources.h
	@mkdir -p $(dir $@)
	@echo "  CC      $(patsubst $(TOP)/%,%,$<)"
	@$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

# libcdio sources need its config.h and private include paths
$(BUILD)/obj/src/libcdio/%.o: CFLAGS += -DHAVE_CONFIG_H -I$(SRC)/libcdio/driver -Wno-implicit-fallthrough
# libfat gets _mm_malloc() from MSVC's <malloc.h>
$(BUILD)/obj/src/syslinux/%.o: CFLAGS += -include rufus_mm_malloc.h
# NB: xz_dec_*.c are included by decompress_unxz.c
# bled expects to be compiled for disk images
$(BUILD)/obj/src/bled/%.o: CFLAGS += -DBLED_EXPECT_DISK_IMAGE -I$(SRC)/bled

$(BUILD)/rufus: $(OBJS)
	@echo "  LINK    $@"
	@$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

install: $(BUILD)/rufus
	sh $(LNX)/packaging/install.sh "$(DESTDIR)" "$(PREFIX)" "$(BUILD)/rufus"

deb:
	sh $(LNX)/packaging/build-deb.sh $(SYSROOT)

check: $(BUILD)/rufus
	bash $(LNX)/tests/run-tests.sh

clean:
	rm -rf -- "$(BUILD)/obj" "$(BUILD)/deb" "$(BUILD)/rufus" "$(BUILD)/resources.h"

-include $(DEPS)
