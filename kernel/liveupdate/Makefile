# SPDX-License-Identifier: GPL-2.0

luo-y :=								\
		kho_block.o						\
		luo_core.o						\
		luo_file.o						\
		luo_flb.o						\
		luo_session.o

obj-$(CONFIG_KEXEC_HANDOVER)		+= kexec_handover.o kexec_handover_common.o
obj-$(CONFIG_KEXEC_HANDOVER_READONLY)	+= kexec_handover_ro.o kexec_handover_common.o
obj-$(CONFIG_KEXEC_HANDOVER_DEBUGFS)	+= kexec_handover_debugfs.o

obj-$(CONFIG_LIVEUPDATE)		+= luo.o
