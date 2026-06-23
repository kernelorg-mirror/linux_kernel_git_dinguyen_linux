/* SPDX-License-Identifier: GPL-2.0-or-later OR MIT */
/*
 * Copyright (C) 2026 Altera
 *
 * SDOS-only subset of the SoCFPGA FCS HAL interface.
 *
 * @file socfpga_fcs_hal.h
 * @brief API interfaces for the SDOS encrypt/decrypt feature.
 */
#ifndef SOCFPGA_FCS_HAL_H
#define SOCFPGA_FCS_HAL_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#include "socfpga-fcs-types.h"

#define SDOS_HEADER_SZ		40
#define SDOS_HMAC_SZ		48
#define SDOS_PLAINDATA_MIN_SZ	32
#define SDOS_PLAINDATA_MAX_SZ	32672
#define SDOS_DECRYPTED_MIN_SZ	(SDOS_PLAINDATA_MIN_SZ + SDOS_HEADER_SZ)
#define SDOS_DECRYPTED_MAX_SZ	(SDOS_PLAINDATA_MAX_SZ + SDOS_HEADER_SZ)
#define SDOS_ENCRYPTED_MIN_SZ	(SDOS_PLAINDATA_MIN_SZ + SDOS_HEADER_SZ + SDOS_HMAC_SZ)
#define SDOS_ENCRYPTED_MAX_SZ	(SDOS_PLAINDATA_MAX_SZ + SDOS_HEADER_SZ + SDOS_HMAC_SZ)

/* Platform definitions */
#define AGILEX5_PLAT		2

#pragma pack(push, 1)
struct fcs_cmd_context {
	/* Error status variable address */
	int *error_code_addr;
	union {
		struct {
			/* Session id */
			uuid_t *suuid;
			unsigned int *suuid_len;
		} open_session;

		struct {
			/* Session id */
			uuid_t suuid;
		} close_session;

		struct {
			/* Session id */
			uuid_t suuid;
			/* context id */
			u32 context_id;
			char *rng;
			u32 rng_len;
		} rng;

		struct {
			/* Session id */
			uuid_t suuid;
			/* context id */
			u32 context_id;
			u32 op_mode;
			char *src;
			u32 src_size;
			char *dst;
			u32 *dst_size;
			u16 id;
			u64 own;
			int pad;
		} sdos;
	};
};

#pragma pack(pop)

// Forward declaration in socfpga-fcs-plat.h
struct socfpga_fcs_service_ops;

/**
 * @brief data struct of message which stands for the communication
 *  format with ATF when talk with OS dependent layer API
 */
struct socfpga_fcs_priv {
	/** Communication channel */
	struct stratix10_svc_chan *chan;
	/** platform */
	u32 platform;
	/** plat data */
	const struct socfpga_fcs_service_ops *plat_data;
	/* command context */
	struct fcs_cmd_context k_ctx;
	/** cli structure */
	struct stratix10_svc_client client;
	/** Completion status */
	struct completion completion;
	/** Mutex lock */
	struct mutex lock;
	/** status */
	int status;
	/** response */
	u32 resp;
	/* Session ID */
	u32 session_id;
	/** UUID */
	uuid_t uuid_id;
	/** device to issue command */
	struct device *dev;
	/** ATF version */
	u32 atf_version[3];
};

enum fcs_command_code {
	FCS_DEV_COMMAND_NONE = 0,
	FCS_DEV_CRYPTO_OPEN_SESSION,
	FCS_DEV_CRYPTO_CLOSE_SESSION,
	FCS_DEV_SDOS_DATA_EXT,
	FCS_DEV_ATF_VERSION,
};

/**
 * @brief Gets the FCS command context (and takes the HAL lock).
 *
 * @return Returns a pointer to the FCS command context.
 */
struct fcs_cmd_context *hal_get_fcs_cmd_ctx(void);

/**
 * @brief Releases the FCS command context (and drops the HAL lock).
 *
 * @param k_ctx A pointer to the command context structure.
 */
void hal_release_fcs_cmd_ctx(struct fcs_cmd_context *const k_ctx);

/**
 * @brief Gets the platform type.
 *
 * @return Returns an integer representing the platform type.
 */
int hal_get_platform(void);

/**
 * @brief Initializes the FCS HAL.
 *
 * @return Returns an int value indicating the status of the init.
 */
int hal_fcs_init(struct device *dev);

/**
 * @brief Deinitializes the FCS HAL.
 */
void hal_fcs_deinit(void);

/**
 * @brief Cleans up the FCS HAL.
 */
void hal_fcs_cleanup(void);

/**
 * @brief Requests SDM to open a crypto service session.
 *
 * @param ctx A pointer to the command context structure.
 * @return 0 on success, otherwise an error code.
 */
int hal_session_open(struct fcs_cmd_context *const ctx);

/**
 * @brief Requests SDM to close a previously opened session.
 *
 * @param ctx A pointer to the command context structure.
 * @return 0 on success, otherwise an error code.
 */
int hal_session_close(struct fcs_cmd_context *const ctx);

/**
 * @brief Retrieves the version of the ATF (Arm Trusted Firmware).
 *
 * @param[out] version Pointer to a buffer where the ATF version is stored.
 */
void hal_get_atf_version(u32 *version);

/**
 * @brief Encrypts/decrypts data using SDOS (Secure Data Object Service).
 *
 * @param k_ctx Pointer to the command context structure.
 * @return int Result of the operation.
 */
int hal_sdos_crypt(struct fcs_cmd_context *const k_ctx);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* SOCFPGA_FCS_HAL_H */
