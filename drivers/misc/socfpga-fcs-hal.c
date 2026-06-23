// SPDX-License-Identifier: GPL-2.0-or-later OR MIT
/*
 * Copyright (C) 2026 Altera Corporation
 *
 * SDOS-only subset of the SoCFPGA FCS HAL. Provides the SDOS (Secure Data
 * Object Service) AES encrypt/decrypt operation, the crypto session
 * open/close lifecycle it depends on, and the HAL init/deinit glue.
 */

#include <misc/socfpga-fcs-hal.h>
#include <misc/socfpga-fcs-plat.h>

#include <linux/uaccess.h>

#define OWNER_ID_OFFSET				12
#define OWNER_ID_SIZE				8

#define SDOS_DECRYPTION_REPROVISION_KEY_WARN		0x102
#define SDOS_DECRYPTION_NOT_LATEST_KEY_WARN		0x103

static struct socfpga_fcs_priv *priv;

int hal_session_open(struct fcs_cmd_context *const k_ctx)
{
	int ret = 0;

	ret = priv->plat_data->svc_send_request(priv, FCS_DEV_CRYPTO_OPEN_SESSION,
						FCS_REQUEST_TIMEOUT);
	if (ret) {
		LOG_ERR("Failed to send the cmd=%d,ret=%d\n",
			FCS_DEV_CRYPTO_OPEN_SESSION, ret);
		return ret;
	}

	if (priv->status) {
		ret = -EIO;
		LOG_ERR("Mailbox error, Failed to open session ret: %d\n", ret);
		goto copy_mbox_status;
	}

	fcs_plat_uuid_generate(priv);

	memcpy(&priv->session_id, &priv->resp, sizeof(priv->session_id));

	ret = copy_to_user(k_ctx->open_session.suuid, &priv->uuid_id,
			   sizeof(uuid_t)) ? -EFAULT : 0;
	if (ret) {
		LOG_ERR("Failed to copy session ID to user suuid addr: %p ret: %d\n",
			k_ctx->open_session.suuid, ret);
		goto copy_mbox_status;
	}

copy_mbox_status:
	ret = copy_to_user(k_ctx->error_code_addr, &priv->status,
			   sizeof(priv->status)) ? -EFAULT : 0;
	if (ret) {
		LOG_ERR("Failed to copy mail box status code to user ret: %d\n",
			ret);
	}

	return ret;
}
EXPORT_SYMBOL(hal_session_open);

int hal_session_close(struct fcs_cmd_context *const k_ctx)
{
	int ret = 0;
	struct fcs_cmd_context ctx;

	memcpy(&ctx, k_ctx, sizeof(struct fcs_cmd_context));

	ret = fcs_plat_uuid_compare(&priv->uuid_id,
				    &k_ctx->close_session.suuid);
	if (!ret) {
		ret = -EINVAL;
		LOG_ERR("Session UUID Mismatch ret: %d\n", ret);
		return ret;
	}

	ret = priv->plat_data->svc_send_request(priv, FCS_DEV_CRYPTO_CLOSE_SESSION,
						FCS_REQUEST_TIMEOUT);
	if (ret) {
		LOG_ERR("Failed to send the cmd=%d,ret=%d\n",
			FCS_DEV_CRYPTO_CLOSE_SESSION, ret);
		return ret;
	}

	fcs_plat_uuid_clear(priv);
	if (priv->status) {
		ret = -EIO;
		LOG_ERR("Mailbox error, Failed to close session ret: %d\n",
			ret);
	}

	ret = copy_to_user(ctx.error_code_addr, &priv->status,
			   sizeof(priv->status)) ? -EFAULT : 0;
	if (ret) {
		LOG_ERR("Failed to copy mail box status code to user ret: %d\n",
			ret);
	}

	return ret;
}
EXPORT_SYMBOL(hal_session_close);

void hal_get_atf_version(u32 *version)
{
	memcpy(version, priv->atf_version, sizeof(priv->atf_version));
}
EXPORT_SYMBOL(hal_get_atf_version);

int hal_sdos_crypt(struct fcs_cmd_context *const k_ctx)
{
	void *s_buf = NULL, *d_buf = NULL;
	struct fcs_cmd_context ctx;
	u32 output_size;
	u64 owner_id;
	int ret = 0;
	char *temp;

	memcpy(&ctx, k_ctx, sizeof(struct fcs_cmd_context));

	if (ctx.sdos.op_mode) {
		output_size = SDOS_ENCRYPTED_MAX_SZ;
		/* encrypt: input is header + plaintext */
		if (k_ctx->sdos.src_size < SDOS_DECRYPTED_MIN_SZ ||
		    k_ctx->sdos.src_size > SDOS_DECRYPTED_MAX_SZ) {
			LOG_ERR("Invalid SDOS src_size %u\n", k_ctx->sdos.src_size);
			return -EINVAL;
		}
	} else {
		output_size = SDOS_DECRYPTED_MAX_SZ;
		/* decrypt: input is header + plaintext + HMAC */
		if (k_ctx->sdos.src_size < SDOS_ENCRYPTED_MIN_SZ ||
		    k_ctx->sdos.src_size > SDOS_ENCRYPTED_MAX_SZ) {
			LOG_ERR("Invalid SDOS src_size %u\n", k_ctx->sdos.src_size);
			return -EINVAL;
		}
	}

	s_buf = priv->plat_data->svc_alloc_memory(priv, k_ctx->sdos.src_size);
	if (IS_ERR(s_buf)) {
		ret = -ENOMEM;
		LOG_ERR("Failed to allocate memory for SDOS input data kernel buffer ret: %d\n",
			ret);
		return ret;
	}

	k_ctx->sdos.dst_size = &output_size;

	d_buf = priv->plat_data->svc_alloc_memory(priv, *k_ctx->sdos.dst_size);
	if (IS_ERR(d_buf)) {
		ret = -ENOMEM;
		LOG_ERR("Failed to allocate memory for SDOS output kernel buffer ret: %d\n",
			ret);
		goto free_sbuf;
	}

	/* Copy the user space input data to the input data kernel buffer */
	ret = copy_from_user(s_buf, k_ctx->sdos.src,
			     k_ctx->sdos.src_size) ? -EFAULT : 0;
	if (ret) {
		LOG_ERR("Failed to copy SDOS data from user to kernel buffer ret: %d\n",
			ret);
		goto free_dbuf;
	}

	/* Get Owner ID from buf */
	temp = (uint8_t *)s_buf;
	memcpy(&owner_id, temp + OWNER_ID_OFFSET, OWNER_ID_SIZE);
	k_ctx->sdos.own = owner_id;
	k_ctx->sdos.src = s_buf;
	k_ctx->sdos.dst = d_buf;

	ret = priv->plat_data->svc_send_request(priv, FCS_DEV_SDOS_DATA_EXT,
						FCS_REQUEST_TIMEOUT);
	if (ret) {
		LOG_ERR("Failed to send the cmd=%d,ret=%d\n",
			FCS_DEV_SDOS_DATA_EXT, ret);
		goto free_dbuf;
	}
	if (priv->status &&
	    priv->status != SDOS_DECRYPTION_REPROVISION_KEY_WARN &&
	    priv->status != SDOS_DECRYPTION_NOT_LATEST_KEY_WARN) {
		LOG_ERR("Failed to perform SDOS operation ret: %d Mailbox Status = %d\n",
			ret, priv->status);
		goto copy_mbox_status;
	}

	/* Copy the encrypted/decrypted output from kernel space to user space */
	ret = copy_to_user(ctx.sdos.dst, d_buf, priv->resp) ? -EFAULT : 0;
	if (ret) {
		LOG_ERR("Failed to copy encrypted output to user ret: %d\n",
			ret);
		goto copy_mbox_status;
	}

	/* Copy the encrypted output length from kernel space to user space */
	ret = copy_to_user(ctx.sdos.dst_size, &priv->resp,
			   sizeof(priv->resp)) ? -EFAULT : 0;
	if (ret) {
		LOG_ERR("Failed to copy encrypted output length to user ret: %d\n",
			ret);
	}

copy_mbox_status:
	ret = copy_to_user(ctx.error_code_addr, &priv->status,
			   sizeof(priv->status)) ? -EFAULT : 0;
	if (ret) {
		LOG_ERR("Failed to copy mailbox status code to user ret: %d\n",
			ret);
	}
free_dbuf:
	priv->plat_data->svc_free_memory(priv, d_buf);
free_sbuf:
	priv->plat_data->svc_free_memory(priv, s_buf);

	return ret;
}
EXPORT_SYMBOL(hal_sdos_crypt);

struct fcs_cmd_context *hal_get_fcs_cmd_ctx(void)
{
	mutex_lock(&priv->lock);
	return &priv->k_ctx;
}
EXPORT_SYMBOL(hal_get_fcs_cmd_ctx);

void hal_release_fcs_cmd_ctx(struct fcs_cmd_context *const k_ctx)
{
	mutex_unlock(&priv->lock);
}
EXPORT_SYMBOL(hal_release_fcs_cmd_ctx);

static int hal_read_version_from_atf(void)
{
	int ret = 0;

	ret = priv->plat_data->svc_send_request(priv, FCS_DEV_ATF_VERSION,
						FCS_REQUEST_TIMEOUT);
	if (ret) {
		LOG_ERR("Failed to send the cmd=%d,ret=%d\n",
			FCS_DEV_ATF_VERSION, ret);
		return ret;
	}

	if (priv->status) {
		ret = -EIO;
		LOG_ERR("Mailbox error, Failed to read ATF version ret: %d\n",
			ret);
	}

	priv->plat_data->svc_task_done(priv);

	return ret;
}

int hal_fcs_init(struct device *dev)
{
	int ret;

	priv = devm_kzalloc(dev, sizeof(struct socfpga_fcs_priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	ret = fcs_plat_init(dev, priv);
	if (ret) {
		LOG_ERR("Failed to initialize platform data ret: %d\n", ret);
		return ret;
	}

	hal_read_version_from_atf();

	return ret;
}

void hal_fcs_deinit(void)
{
	if (priv && priv->session_id) {
		int ret = priv->plat_data->svc_send_request(priv, FCS_DEV_CRYPTO_CLOSE_SESSION,
							    FCS_REQUEST_TIMEOUT);
		if (ret) {
			LOG_ERR("Failed to close FCS service session,ret=%d\n",
				ret);
		}
	}

	if (priv)
		fcs_plat_deinit(priv);
}

int hal_get_platform(void)
{
	return priv->platform;
}

void hal_fcs_cleanup(void)
{
	fcs_plat_cleanup(priv);
	priv = NULL;
}
