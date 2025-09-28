# SPDX-License-Identifier: GPL-2.0-only

KHDR_INCLUDES ?= -I../../../usr/include
CFLAGS += -Wall -O2 -Wno-unused-function
CFLAGS += $(KHDR_INCLUDES)
LDFLAGS += -static

# --- Test Configuration (Edit this section when adding new tests) ---
LUO_SHARED_SRCS := luo_test_utils.c
LUO_SHARED_HDRS += luo_test_utils.h

LUO_MANUAL_TESTS += luo_multi_file
LUO_MANUAL_TESTS += luo_multi_kexec
LUO_MANUAL_TESTS += luo_multi_session
LUO_MANUAL_TESTS += luo_unreclaimed

TEST_FILES += do_kexec.sh

LUO_MAIN_TESTS += liveupdate

# --- Automatic Rule Generation (Do not edit below) ---

TEST_GEN_PROGS_EXTENDED += $(LUO_MANUAL_TESTS)
TEST_GEN_PROGS := $(LUO_MAIN_TESTS)

liveupdate_SOURCES := liveupdate.c $(LUO_SHARED_SRCS)

$(OUTPUT)/liveupdate: $(liveupdate_SOURCES) $(LUO_SHARED_HDRS)
	$(call msg,LINK,,$@)
	$(Q)$(LINK.c) $^ $(LDLIBS) -o $@

# Define the full list of sources for each manual test.
$(foreach test,$(LUO_MANUAL_TESTS), \
	$(eval $(test)_SOURCES := $(test).c $(LUO_SHARED_SRCS)))

# This loop automatically generates an explicit build rule for each manual test.
# It includes dependencies on the shared headers and makes the output
# executable.
# Note the use of '$$' to escape automatic variables for the 'eval' command.
$(foreach test,$(LUO_MANUAL_TESTS), \
	$(eval $(OUTPUT)/$(test): $($(test)_SOURCES) $(LUO_SHARED_HDRS) \
		$(call msg,LINK,,$$@) ; \
		$(Q)$(LINK.c) $$^ $(LDLIBS) -o $$@ ; \
		$(Q)chmod +x $$@ \
	) \
)

include ../lib.mk
