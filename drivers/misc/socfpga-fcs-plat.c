// SPDX-License-Identifier: GPL-2.0-or-later OR MIT
/*
 * Copyright (C) 2026 Altera
 *
 * SDOS-only subset of the SoCFPGA FCS platform layer. Implements just the
 * platform helpers and the stratix10-svc transport needed by the SDOS
 * encrypt/decrypt operation (and its session open/close lifecycle).
 */

#ifndef __SOCFPA_HAL_LL_H
#define __SOCFPA_HAL_LL_H

#include <misc/socfpga-fcs-hal.h>
#include <misc/socfpga-fcs-plat.h>

#include <linux/firmware/intel/stratix10-svc-client.h>
#include <linux/delay.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/slab.h>

#define MSG_RETRY		3
#define RETRY_SLEEP_MS		1
#define TIMEOUT			1000

#define FCS_SVC_CLIENT_NAME "socfpga-fcs"

static void fcs_atf_version_callback(struct stratix10_svc_client *client,
				     struct stratix10_svc_cb_data *data)
{
	struct socfpga_fcs_priv *priv = client->priv;

	priv->status = data->status;
	if (data->status == BIT(SVC_STATUS_OK)) {
		priv->status = 0;
		priv->atf_version[0] = *((unsigned int *)data->kaddr1);
		priv->atf_version[1] = *((unsigned int *)data->kaddr2);
		priv->atf_version[2] = *((unsigned int *)data->kaddr3);
	} else if (data->status == BIT(SVC_STATUS_ERROR)) {
		priv->status = *((unsigned int *)data->kaddr1);
		dev_err(client->dev, "mbox_error=0x%x\n", priv->status);
	}

	complete(&priv->completion);
}

bool fcs_plat_uuid_compare(uuid_t *uuid1, uuid_t *uuid2)
{
	return uuid_equal(uuid1, uuid2);
}

void fcs_plat_uuid_generate(struct socfpga_fcs_priv *priv)
{
	uuid_gen(&priv->uuid_id);
}

void fcs_plat_uuid_clear(struct socfpga_fcs_priv *priv)
{
	memset(&priv->uuid_id, 0, sizeof(uuid_t));
	memset(&priv->session_id, 0, sizeof(u32));
}

static void *plat_sip_svc_allocate_memory(struct socfpga_fcs_priv *priv, size_t size)
{
	return stratix10_svc_allocate_memory(priv->chan, size);
}

static void plat_sip_svc_free_memory(struct socfpga_fcs_priv *priv, void *buf)
{
	stratix10_svc_free_memory(priv->chan, buf);
}

static void plat_sip_svc_task_done(struct socfpga_fcs_priv *priv)
{
	stratix10_svc_done(priv->chan);
}

static void soc64_async_callback(void *ptr)
{
	if (ptr)
		complete(ptr);
}

static int plat_sip_svc_send_request(struct socfpga_fcs_priv *priv,
				     enum fcs_command_code command,
				     unsigned long timeout)
{
	int ret = 0;
	int status, index;
	void *handle = NULL;
	struct stratix10_svc_cb_data data;
	struct completion completion;
	struct fcs_cmd_context *k_ctx = &priv->k_ctx;
	struct stratix10_svc_client_msg *msg =
		kzalloc(sizeof(*msg), GFP_KERNEL);

	if (!msg) {
		pr_err("failed to allocate memory for svc client message ret: %d\n",
		       ret);
		return -ENOMEM;
	}

	priv->status = 0;
	priv->resp = 0;

	switch (command) {
	case FCS_DEV_CRYPTO_OPEN_SESSION:
		pr_debug("Sending command: COMMAND_FCS_CRYPTO_OPEN_SESSION\n");
		msg->command = COMMAND_FCS_CRYPTO_OPEN_SESSION;
		break;

	case FCS_DEV_CRYPTO_CLOSE_SESSION:
		pr_debug("Sending command: COMMAND_FCS_CRYPTO_CLOSE_SESSION with session_id: 0x%x\n",
			 priv->session_id);
		msg->arg[0] = priv->session_id;
		msg->command = COMMAND_FCS_CRYPTO_CLOSE_SESSION;
		break;

	case FCS_DEV_ATF_VERSION:
		pr_debug("Sending command: COMMAND_SMC_ATF_BUILD_VER\n");
		msg->command = COMMAND_SMC_ATF_BUILD_VER;
		priv->client.receive_cb = fcs_atf_version_callback;
		break;

	case FCS_DEV_SDOS_DATA_EXT:
		pr_debug("Sending command: COMMAND_FCS_SDOS_DATA_EXT with session_id: 0x%x, context_id: 0x%x, op_mode: 0x%x, own: 0x%llx\n",
			 priv->session_id, k_ctx->sdos.context_id,
			 k_ctx->sdos.op_mode, k_ctx->sdos.own);
		msg->arg[0] = priv->session_id;
		msg->arg[1] = k_ctx->sdos.context_id;
		msg->arg[2] = k_ctx->sdos.op_mode;
		msg->arg[3] = k_ctx->sdos.own;
		msg->payload = k_ctx->sdos.src;
		msg->payload_length = k_ctx->sdos.src_size;
		msg->payload_output = k_ctx->sdos.dst;
		msg->payload_length_output = *k_ctx->sdos.dst_size;
		msg->command = COMMAND_FCS_SDOS_DATA_EXT;
		break;

	default:
		pr_err("Unknown command: 0x%x\n", command);
		ret = -EINVAL;
		break;
	}

	if (ret) {
		kfree(msg);
		return ret;
	}

	if (command == FCS_DEV_ATF_VERSION) {
		reinit_completion(&priv->completion);

		ret = stratix10_svc_send(priv->chan, msg);
		if (ret) {
			pr_err("failed to send message to service channel\n");
			goto fun_ret;
		}

		if (!wait_for_completion_timeout(&priv->completion, timeout)) {
			pr_err("svc timeout to get completed status\n");
			ret = -ETIMEDOUT;
		}
fun_ret:
		kfree(msg);
		return ret;
	}

	init_completion(&completion);

	for (index = 0; index < MSG_RETRY; index++) {
		status = stratix10_svc_async_send(priv->chan, msg, &handle,
						  soc64_async_callback,
						  &completion);
		if (status == 0)
			break;
		msleep(RETRY_SLEEP_MS);
	}

	if (!handle || status != 0) {
		pr_err("Failed to send async message\n");
		kfree(msg);
		return -ETIMEDOUT;
	}

	ret = wait_for_completion_io_timeout(&completion, (TIMEOUT));
	if (ret > 0)
		pr_debug("Received async interrupt\n");
	else
		pr_err("timeout occurred while waiting for async message\n");

	ret = stratix10_svc_async_poll(priv->chan, handle, &data);
	if (ret) {
		pr_err("Failed to poll async message\n");
		goto out;
	}

	priv->status = data.status;

	if (data.kaddr1)
		priv->resp = *((u32 *)data.kaddr1);
	else
		priv->resp = 0;

out:
	stratix10_svc_async_done(priv->chan, handle);
	kfree(msg);

	return ret;
}

static const struct socfpga_fcs_service_ops plat_sip_svc_ops = {
	.svc_send_request = plat_sip_svc_send_request,
	.svc_alloc_memory = plat_sip_svc_allocate_memory,
	.svc_free_memory = plat_sip_svc_free_memory,
	.svc_task_done = plat_sip_svc_task_done,
};

int fcs_platform_get(struct device *dev, struct socfpga_fcs_priv *priv)
{
	int ret = 0;

	if (of_device_is_compatible(dev->of_node, "intel,agilex5-soc-fcs-config"))
		priv->platform = AGILEX5_PLAT;
	else
		priv->platform = 0;

	if (!priv->platform) {
		pr_err("Unsupported platform\n");
		ret = -ENODEV;
	}
	return ret;
}

int fcs_plat_init(struct device *dev, struct socfpga_fcs_priv *priv)
{
	s32 ret = 0;

	mutex_init(&priv->lock);

	priv->plat_data = &plat_sip_svc_ops;

	priv->dev = dev;
	priv->client.dev = dev;
	priv->client.receive_cb = NULL;
	priv->client.priv = priv;

	priv->chan = stratix10_svc_request_channel_byname(&priv->client,
							  SVC_CLIENT_FCS);
	if (IS_ERR(priv->chan)) {
		pr_err("couldn't get service channel %s\n", SVC_CLIENT_FCS);
		return -EPROBE_DEFER;
	}

	ret = stratix10_svc_add_async_client(priv->chan, true);
	if (ret) {
		pr_err("Failed to add async client\n");
		return ret;
	}

	init_completion(&priv->completion);

	ret = fcs_platform_get(priv->dev, priv);
	if (ret) {
		pr_err("Failed to get platform data\n");
		return ret;
	}

	return 0;
}

void fcs_plat_deinit(struct socfpga_fcs_priv *priv)
{
	stratix10_svc_remove_async_client(priv->chan);
	stratix10_svc_free_channel(priv->chan);
}

void fcs_plat_cleanup(struct socfpga_fcs_priv *priv)
{
	stratix10_svc_free_channel(priv->chan);
}

#endif /* __SOCFPA_HAL_LL_H */
