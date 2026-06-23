/* SPDX-License-Identifier: GPL-2.0-or-later OR MIT */
/*
 * Copyright (C) 2026 Altera
 *
 * SDOS-only subset of the SoCFPGA FCS type definitions.
 */

#ifndef SOCFPGA_FCS_TYPES_H
#define SOCFPGA_FCS_TYPES_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#include <linux/completion.h>
#include <linux/device.h>
#include <linux/mutex.h>
#include <linux/types.h>
#include <linux/uuid.h>
#include <linux/firmware/intel/stratix10-svc-client.h>

#define LOG_ERR(fmt, ...)		pr_err(fmt, ##__VA_ARGS__)
#define LOG_DBG(fmt, ...)		pr_debug(fmt, ##__VA_ARGS__)
#define LOG_INF(fmt, ...)		pr_info(fmt, ##__VA_ARGS__)
#define LOG_WRN(fmt, ...)		pr_warn(fmt, ##__VA_ARGS__)

#define FCS_REQUEST_TIMEOUT	(msecs_to_jiffies(SVC_FCS_REQUEST_TIMEOUT_MS))
#define FCS_COMPLETED_TIMEOUT	(msecs_to_jiffies(SVC_COMPLETED_TIMEOUT_MS))

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* SOCFPGA_FCS_TYPES_H */
