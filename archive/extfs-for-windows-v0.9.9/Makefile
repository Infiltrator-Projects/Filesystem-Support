# SPDX-License-Identifier: GPL-3.0-or-later
CC ?= cc
AR ?= ar
CFLAGS ?= -O2
CPPFLAGS ?=
LDFLAGS ?=
LDLIBS ?=
WARNINGS = -Wall -Wextra -Wpedantic -Werror
INCLUDES = -Iinclude
BUILD = build
LEGACY_RESIZE_DEFINES = -Dextfs_resize_file_ext2_direct=extfs_resize_file_ext2_legacy_direct -Dextfs_resize_file_ext3_journaled_direct=extfs_resize_file_ext3_journaled_legacy_direct
SINGLE_INDIRECT_IMPL_DEFINES = -Dextfs_resize_file_ext2_direct=extfs_resize_file_ext2_single_indirect_impl -Dextfs_resize_file_ext3_journaled_direct=extfs_resize_file_ext3_single_indirect_impl

.PHONY: all test integration clean

all: $(BUILD)/libextfs.a $(BUILD)/extfs-tool $(BUILD)/test-extfs $(BUILD)/test-single-indirect $(BUILD)/extfs-mutate-test

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/extfs.o: core/extfs.c include/extfs/extfs.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(LEGACY_RESIZE_DEFINES) -std=c11 $(INCLUDES) -c $< -o $@

$(BUILD)/extfs_classic_resize.o: core/extfs_classic_resize.c include/extfs/extfs.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(SINGLE_INDIRECT_IMPL_DEFINES) -std=c11 $(INCLUDES) -c $< -o $@

$(BUILD)/extfs_resize_dispatch.o: core/extfs_resize_dispatch.c include/extfs/extfs.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -std=c11 $(INCLUDES) -c $< -o $@

$(BUILD)/libextfs.a: $(BUILD)/extfs.o $(BUILD)/extfs_classic_resize.o $(BUILD)/extfs_resize_dispatch.o
	$(AR) rcs $@ $^

$(BUILD)/extfs-tool.o: tools/extfs-tool.c include/extfs/extfs.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -std=c11 $(INCLUDES) -c $< -o $@

$(BUILD)/extfs-tool: $(BUILD)/extfs-tool.o $(BUILD)/libextfs.a
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(BUILD)/test-extfs.o: tests/test_extfs.c include/extfs/extfs.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -std=c11 $(INCLUDES) -c $< -o $@

$(BUILD)/test-extfs: $(BUILD)/test-extfs.o $(BUILD)/libextfs.a
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(BUILD)/test_single_indirect.o: tests/test_single_indirect.c include/extfs/extfs.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -std=c11 $(INCLUDES) -c $< -o $@

$(BUILD)/test-single-indirect: $(BUILD)/test_single_indirect.o $(BUILD)/libextfs.a
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(BUILD)/mutate_image.o: tests/mutate_image.c include/extfs/extfs.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -std=c11 $(INCLUDES) -c $< -o $@

$(BUILD)/extfs-mutate-test: $(BUILD)/mutate_image.o $(BUILD)/libextfs.a
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

test: all
	$(BUILD)/test-extfs
	$(BUILD)/test-single-indirect
	$(MAKE) integration

integration: $(BUILD)/extfs-tool $(BUILD)/extfs-mutate-test
	sh tests/integration.sh

clean:
	rm -rf $(BUILD)
