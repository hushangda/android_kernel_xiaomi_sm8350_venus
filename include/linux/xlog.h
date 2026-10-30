/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_XLOG_H
#define _LINUX_XLOG_H

#include <linux/types.h>

ssize_t xlogchar_kwrite(const char *buffer, size_t count);

#endif
