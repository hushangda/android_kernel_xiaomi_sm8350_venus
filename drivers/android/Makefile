# SPDX-License-Identifier: GPL-2.0-only
ccflags-y += -I$(src)			# needed for trace events

obj-$(CONFIG_ANDROID_BINDERFS)		+= binderfs.o
obj-$(CONFIG_ANDROID_BINDER_IPC)	+= binder.o binder_alloc.o
obj-$(CONFIG_ANDROID_BINDER_IPC_SELFTEST) += binder_alloc_selftest.o
obj-$(CONFIG_ANDROID_VENDOR_HOOKS) += vendor_hooks.o
obj-$(CONFIG_OPLUS_FEATURE_HANS) += oplus_hans.o
obj-$(CONFIG_OPLUS_FEATURE_LINKPOWER) += oplus_linkpower.o
obj-$(CONFIG_OPLUS_FEATURE_STABILITY_HELPER) += oplus_stability.o

obj-y += hwui_mon.o
obj-y += jank_detect_demo.o
