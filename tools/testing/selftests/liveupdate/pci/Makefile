# SPDX-License-Identifier: GPL-2.0-only

TEST_GEN_PROGS_EXTENDED += luo_pci
TEST_GEN_PROGS_EXTENDED += luo_pci_auto_pf
TEST_GEN_MODS_DIR := test_modules

TEST_FILES += luo_pci.vmtest
TEST_FILES += luo_pci_auto_pf.vmtest

include ../../lib.mk
include ../lib/libliveupdate.mk

CFLAGS += $(KHDR_INCLUDES)
CFLAGS += -Wall -O2 -Wno-unused-function
CFLAGS += -MD

TEST_O := $(patsubst %, %.o, $(TEST_GEN_PROGS_EXTENDED))

TEST_DEP_FILES := $(patsubst %.o, %.d, $(LIBLIVEUPDATE_O))
TEST_DEP_FILES += $(patsubst %.o, %.d, $(TEST_O))
-include $(TEST_DEP_FILES)

$(TEST_GEN_PROGS_EXTENDED): $(OUTPUT)/%: %.o $(LIBLIVEUPDATE_O)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) $(TARGET_ARCH) $< $(LIBLIVEUPDATE_O) $(LDLIBS) -o $@

EXTRA_CLEAN += $(TEST_O)
EXTRA_CLEAN += $(TEST_DEP_FILES)
