/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_VENUS_BOOTTRACE_H
#define _LINUX_VENUS_BOOTTRACE_H

#include <linux/init.h>
#include <linux/types.h>

#ifdef CONFIG_VENUS_BOOTTRACE
void venus_boottrace_stage(unsigned int stage);
void venus_boottrace_clock_ready(void);
void venus_boottrace_initcall_start(initcall_t fn);
void venus_boottrace_initcall_finish(initcall_t fn, int ret);
void venus_boottrace_dump_metadata(void);
ssize_t venus_printk_read_record(char *buf, size_t count, u64 *seq, u32 *idx);
#else
static inline void venus_boottrace_stage(unsigned int stage) { }
static inline void venus_boottrace_clock_ready(void) { }
static inline void venus_boottrace_initcall_start(initcall_t fn) { }
static inline void venus_boottrace_initcall_finish(initcall_t fn, int ret) { }
static inline void venus_boottrace_dump_metadata(void) { }
#endif

#endif /* _LINUX_VENUS_BOOTTRACE_H */
