/* SPDX-License-Identifier: GPL-2.0-or-later OR MIT */
/*
 * Copyright (C) 2026 Altera
 */

#ifndef SOCFPGA_FCS_PLAT_H_
#define SOCFPGA_FCS_PLAT_H_

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#define FCS_STATUS_OK			SVC_STATUS_OK
#define FCS_STATUS_BUFFER_SUBMITTED	SVC_STATUS_BUFFER_SUBMITTED
#define FCS_STATUS_BUFFER_DONE		SVC_STATUS_BUFFER_DONE
#define FCS_STATUS_COMPLETED		SVC_STATUS_COMPLETED
#define FCS_STATUS_BUSY			SVC_STATUS_BUSY
#define FCS_STATUS_ERROR		SVC_STATUS_ERROR
#define FCS_STATUS_NO_SUPPORT		SVC_STATUS_NO_SUPPORT
#define FCS_STATUS_INVALID_PARAM	SVC_STATUS_INVALID_PARAM

/**
 * struct socfpga_fcs_service_ops - Service operations for SoCFPGA FCS
 * @svc_send_request: Function pointer for sending a request
 * @svc_alloc_memory: Function pointer for allocating memory
 * @svc_free_memory: Function pointer for freeing allocated memory
 * @svc_task_done: Function pointer for marking a task as done
 *
 * This structure defines the service operations for the SoCFPGA FCS (FPGA
 * Crypto Service). Each member is a function pointer to the respective
 * operation required for handling FCS services.
 */
struct socfpga_fcs_service_ops {
	int(*svc_send_request)
	(struct socfpga_fcs_priv *priv, enum fcs_command_code command,
	 unsigned long timeout);
	void *(*svc_alloc_memory)(struct socfpga_fcs_priv *priv,
				  size_t size);
	void (*svc_free_memory)
	(struct socfpga_fcs_priv *priv, void *buf);
	void (*svc_task_done)(struct socfpga_fcs_priv *priv);
};

bool fcs_plat_uuid_compare(uuid_t *uuid1, uuid_t *uuid2);
void fcs_plat_uuid_generate(struct socfpga_fcs_priv *priv);
void fcs_plat_uuid_clear(struct socfpga_fcs_priv *priv);
int fcs_plat_init(struct device *dev, struct socfpga_fcs_priv *priv);
void fcs_plat_deinit(struct socfpga_fcs_priv *priv);
void fcs_plat_cleanup(struct socfpga_fcs_priv *priv);
int fcs_platform_get(struct device *dev, struct socfpga_fcs_priv *priv);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif
