// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Altera Corporation
 */

#include <linux/delay.h>
#include <linux/err.h>
#include <linux/lockdep.h>
#include <linux/random.h>
#include <linux/slab.h>
#include <linux/unaligned.h>
#include <linux/firmware/intel/socfpga-fcs.h>
#include <linux/firmware/intel/stratix10-svc-client.h>

#define OWNER_ID_OFFSET				12

#define SDOS_DECRYPTION_REPROVISION_KEY_WARN	0x102
#define SDOS_DECRYPTION_NOT_LATEST_KEY_WARN	0x103

#define MSG_RETRY				3
#define FCS_RETRY_SLEEP_MS			1

/**
 * fcs_atf_version_callback() - service-layer callback for the ATF version query
 * @client: pointer to the stratix10-svc client
 * @data: pointer to the service-layer callback data
 */
static void fcs_atf_version_callback(struct stratix10_svc_client *client,
				     struct stratix10_svc_cb_data *data)
{
	struct socfpga_fcs_priv *p = client->priv;

	p->status = data->status;

	/* Prevent missing payload pointer */
	if (!data->kaddr1 || !data->kaddr2 || !data->kaddr3) {
		dev_err(client->dev, "ATF version is not valid\n");
		p->status = -EIO;
		p->atf_version_valid = false;
		goto complete;
	}

	if (data->status == BIT(SVC_STATUS_OK)) {
		p->status = 0;
		p->atf_version[0] = lower_32_bits(*(unsigned long *)data->kaddr1);
		p->atf_version[1] = lower_32_bits(*(unsigned long *)data->kaddr2);
		p->atf_version[2] = lower_32_bits(*(unsigned long *)data->kaddr3);
		p->atf_version_valid = true;
	} else if (data->status == BIT(SVC_STATUS_ERROR)) {
		p->status = lower_32_bits(*(unsigned long *)data->kaddr1);
		dev_err(client->dev, "mbox_error=0x%x\n", p->status);
	}

complete:
	complete(&p->completion);
}

/**
 * fcs_svc_send_sync() - run a command on the synchronous service path
 * @priv: FCS private state
 * @msg: service-layer message to send
 * @timeout: time to wait for the response
 *
 * Return: 0 on success, negative errno on failure.
 */
static int fcs_svc_send_sync(struct socfpga_fcs_priv *priv,
			     struct stratix10_svc_client_msg *msg,
			     unsigned long timeout)
{
	int ret;

	reinit_completion(&priv->completion);

	/*
	 * receive_cb is only used by the sync send path; leave it set so a
	 * late response cannot find a NULL callback.
	 */
	priv->client.receive_cb = fcs_atf_version_callback;

	ret = stratix10_svc_send(priv->chan, msg);
	if (ret) {
		dev_err(priv->client.dev, "failed to send message to service channel\n");
		priv->client.receive_cb = NULL;
		return ret;
	}

	if (!wait_for_completion_timeout(&priv->completion,
					 msecs_to_jiffies(timeout))) {
		dev_err(priv->client.dev, "svc timeout to get completed status\n");
		return -ETIMEDOUT;
	}

	return 0;
}

/**
 * fcs_svc_send_async() - run a command on the asynchronous mailbox path
 * @priv: FCS private state
 * @msg: service-layer message to send
 * @timeout: time to wait for the response
 *
 * Return: 0 on success, negative errno on failure.
 */
static int fcs_svc_send_async(struct socfpga_fcs_priv *priv,
			      struct stratix10_svc_client_msg *msg,
			      unsigned long timeout)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(timeout);
	struct stratix10_svc_cb_data data;
	void *handle = NULL;
	int status, index;
	int ret;

	for (index = 0; index < MSG_RETRY; index++) {
		status = stratix10_svc_async_send(priv->chan, msg, &handle,
						  NULL, NULL);
		if (status == 0)
			break;
		msleep(FCS_RETRY_SLEEP_MS);
	}

	if (status || !handle) {
		dev_err(priv->client.dev, "Failed to send async message\n");
		/*
		 * A NULL handle with a success status would otherwise be
		 * reported as a completed transaction that never ran.
		 */
		return status ? status : -EIO;
	}

	ret = -ETIMEDOUT;
	while (!time_after(jiffies, deadline)) {
		status = stratix10_svc_async_poll(priv->chan, handle, &data);

		if (status == -EAGAIN) {
		/*
		 * Keep polling until the deadline. Leaving an in-flight
		 * transaction orphans the SDM crypto session.
		 */
		} else if (status < 0) {
			ret = status;
			break;
		} else if (status == 0) {
			ret = 0;
			break;
		}
		msleep(FCS_RETRY_SLEEP_MS);
	}

	if (ret) {
		dev_err(priv->client.dev, "Failed to poll async message\n");
		goto out;
	}

	priv->status = data.status;

	/*
	 * For warnings 0x102/0x103 are success-with-warning (outdated key)
	 * the plaintext is valid and fcs_sdos_crypt() still returns it.
	 * Non-zero SDM status is a firmware result, not a transport failure.
	 */
	if (data.kaddr1)
		priv->resp = lower_32_bits(*(unsigned long *)data.kaddr1);

	if (data.status)
		dev_err(priv->client.dev, "SDM mailbox status 0x%x\n", data.status);

out:
	stratix10_svc_async_done(priv->chan, handle);

	return ret;
}

/**
 * fcs_svc_send_request() - build and send an FCS command to the service layer
 * @priv: FCS private state
 * @command: FCS command code to dispatch
 * @timeout: time to wait for completion, in milliseconds
 * @params: payload and arguments for @command, or NULL for commands that
 *          carry none
 *
 * Return: 0 on success, negative errno on failure.
 */
static int fcs_svc_send_request(struct socfpga_fcs_priv *priv,
				enum fcs_command_code command,
				unsigned long timeout,
				const struct fcs_cmd_params *params)
{
	struct stratix10_svc_client_msg *msg;
	int ret = 0;

	/*
	 * The service layer keeps this message alive in its transaction handle
	 * and still dereferences it from stratix10_svc_async_done(), so it
	 * cannot live on our stack.
	 */
	msg = kzalloc_obj(*msg);
	if (!msg)
		return -ENOMEM;

	priv->status = 0;
	priv->resp = 0;

	switch (command) {
	case FCS_DEV_CRYPTO_OPEN_SESSION:
		dev_dbg(priv->client.dev, "Sending command: COMMAND_FCS_CRYPTO_OPEN_SESSION\n");
		msg->command = COMMAND_FCS_CRYPTO_OPEN_SESSION;
		break;

	case FCS_DEV_CRYPTO_CLOSE_SESSION:
		dev_dbg(priv->client.dev, "Sending command: COMMAND_FCS_CRYPTO_CLOSE_SESSION with session_id: 0x%x\n",
			priv->session_id);
		msg->arg[0] = priv->session_id;
		msg->command = COMMAND_FCS_CRYPTO_CLOSE_SESSION;
		break;

	case FCS_DEV_ATF_VERSION:
		dev_dbg(priv->client.dev, "Sending command: COMMAND_SMC_ATF_BUILD_VER\n");
		msg->command = COMMAND_SMC_ATF_BUILD_VER;
		break;

	case FCS_DEV_SDOS_DATA_EXT:
		if (!params) {
			ret = -EINVAL;
			break;
		}
		dev_dbg(priv->client.dev, "Sending command: COMMAND_FCS_SDOS_DATA_EXT with session_id: 0x%x, context_id: 0x%x, op_mode: 0x%llx, own: 0x%llx\n",
			priv->session_id, params->context_id,
			params->arg[0], params->arg[1]);
		msg->arg[0] = priv->session_id;
		msg->arg[1] = params->context_id;
		msg->arg[2] = params->arg[0];
		msg->arg[3] = params->arg[1];
		msg->payload = (void *)params->src;
		msg->payload_length = params->src_len;
		msg->payload_output = params->dst;
		msg->payload_length_output = params->dst_len;
		msg->command = COMMAND_FCS_SDOS_DATA_EXT;
		break;

	default:
		dev_err(priv->client.dev, "Unknown command: 0x%x\n", command);
		ret = -EINVAL;
		break;
	}

	if (!ret) {
		if (command == FCS_DEV_ATF_VERSION)
			/* ATF fast call for simple command */
			ret = fcs_svc_send_sync(priv, msg, timeout);
		else
			ret = fcs_svc_send_async(priv, msg, timeout);
	}

	kfree(msg);

	return ret;
}

/*
 * fcs_seq_begin() - start a serialized FCS request
 *
 * Takes @priv->lock and checks the device is present, a session is
 * open and @owner owns it.
 *
 * Return: 0 on success, with @priv->lock held; negative errno on
 * failure, with the lock dropped.
 */
static int fcs_seq_begin(struct socfpga_fcs_priv *priv,
			 const void *owner, bool wait)
{
	int ret;

	if (wait) {
		if (mutex_lock_interruptible(&priv->lock))
			return -ERESTARTSYS;
	} else if (!mutex_trylock(&priv->lock)) {
		return -EAGAIN;
	}

	if (priv->removed) {
		ret = -ENODEV;
		goto unlock;
	}

	if (!priv->session_opened) {
		ret = -ENOTCONN;
		goto unlock;
	}

	if (priv->session_owner != owner) {
		ret = -EPERM;
		goto unlock;
	}

	return 0;

unlock:
	mutex_unlock(&priv->lock);
	return ret;
}

static void fcs_seq_end(struct socfpga_fcs_priv *priv)
{
	mutex_unlock(&priv->lock);
}

/*
 * fcs_engine_step_locked() - run one FCS command
 *
 * Caller must hold @priv->lock. Sends the command and copies the
 * output back; the SDM status is stored in @params.
 *
 * Return: 0 if the transaction completed, negative errno otherwise.
 */
static int fcs_engine_step_locked(struct socfpga_fcs_priv *priv,
				  enum fcs_command_code command,
				  struct fcs_cmd_params *params)
{
	struct fcs_cmd_params svc = *params;
	void *s_buf = NULL, *d_buf = NULL;
	u32 out_cap = params->dst_len;
	int ret;

	lockdep_assert_held(&priv->lock);

	if (!params->context_id)
		return -EINVAL;

	/*
	 * Copy-out below treats the firmware response value as a byte
	 * count, which is only defined for SDOS. Reject
	 * an output buffer for any other command instead of silently
	 * misinterpreting that value.
	 */
	if (params->dst && command != FCS_DEV_SDOS_DATA_EXT) {
		dev_err(priv->client.dev,
			"copy-out not supported for command %d\n", command);
		return -EINVAL;
	}

	params->status = 0;
	params->status_valid = false;

	if (params->src) {
		s_buf = stratix10_svc_allocate_memory(priv->chan,
						      params->src_len);
		if (IS_ERR(s_buf)) {
			ret = PTR_ERR(s_buf);
			goto out;
		}
		memcpy(s_buf, params->src, params->src_len);
		svc.src = s_buf;
	}

	if (params->dst) {
		d_buf = stratix10_svc_allocate_memory(priv->chan, out_cap);
		if (IS_ERR(d_buf)) {
			ret = PTR_ERR(d_buf);
			goto out;
		}
		svc.dst = d_buf;
	}

	ret = fcs_svc_send_request(priv, command,
				   SVC_FCS_REQUEST_TIMEOUT_MS, &svc);
	if (ret)
		goto out;

	params->status = priv->status;
	params->status_valid = true;

	if (params->dst) {
		if (priv->resp > out_cap) {
			dev_err(priv->client.dev,
				"FCS output %u exceeds kernel buffer %u\n",
				priv->resp, out_cap);
			ret = -EIO;
			goto out;
		}
		memcpy(params->dst, d_buf, priv->resp);
		params->dst_len = priv->resp;
	}

out:
	if (d_buf)
		stratix10_svc_free_memory(priv->chan, d_buf);
	if (s_buf)
		stratix10_svc_free_memory(priv->chan, s_buf);
	return ret;
}

/**
 * socfpga_fcs_core_engine() - run one FCS command
 * @priv: state returned by fcs_init()
 * @owner: token passed to fcs_ctx_begin() when the session was opened
 * @command: FCS command code to send
 * @params: command parameters, with a caller-generated context ID
 * @wait: true to wait for the lock, false to return -EAGAIN if busy
 *
 * Takes @priv->lock, checks the session, sends the command and copies
 * the output back. The SDM status is stored in @params->status, valid
 * only when @params->status_valid is set.
 *
 * Return: 0 if the transaction completed, negative errno otherwise.
 */
int socfpga_fcs_core_engine(struct socfpga_fcs_priv *priv, const void *owner,
			    enum fcs_command_code command,
			    struct fcs_cmd_params *params, bool wait)
{
	int ret;

	ret = fcs_seq_begin(priv, owner, wait);
	if (ret)
		return ret;

	ret = fcs_engine_step_locked(priv, command, params);
	fcs_seq_end(priv);

	return ret;
}

/**
 * fcs_open_session_locked() - open a crypto session on the SDM
 * @priv: FCS private state
 * @owner: opaque token recorded as the session owner
 *
 * Caller must hold @priv->lock. Only one session may be open.
 *
 * Return: 0 on success, negative errno on failure.
 */
static int fcs_open_session_locked(struct socfpga_fcs_priv *priv,
				   const void *owner)
{
	int ret;

	lockdep_assert_held(&priv->lock);

	if (priv->session_opened) {
		/* SDM allows one crypto session at a time; caller-triggerable */
		dev_dbg(priv->client.dev, "FCS: crypto session already opened\n");
		return -EBUSY;
	}

	ret = fcs_svc_send_request(priv, FCS_DEV_CRYPTO_OPEN_SESSION,
				   SVC_FCS_REQUEST_TIMEOUT_MS, NULL);
	if (ret)
		return ret;

	if (priv->status)
		return -EIO;

	priv->session_id = priv->resp;
	priv->session_opened = true;
	priv->session_owner = owner;

	return 0;
}

/**
 * fcs_close_session_locked() - close the crypto session on the SDM
 * @priv: FCS private state
 *
 * Caller must hold @priv->lock. Local session state is cleared even
 * if the close fails. The owner check is in fcs_ctx_end(); teardown
 * callers may close any session.
 *
 * Return: 0 on success or when no session is open, negative errno
 * otherwise.
 */
static int fcs_close_session_locked(struct socfpga_fcs_priv *priv)
{
	int ret;

	lockdep_assert_held(&priv->lock);

	if (!priv->session_opened)
		/* nothing to close */
		return 0;

	ret = fcs_svc_send_request(priv, FCS_DEV_CRYPTO_CLOSE_SESSION,
				   SVC_FCS_REQUEST_TIMEOUT_MS, NULL);

	priv->session_id = 0;
	priv->session_opened = false;
	priv->session_owner = NULL;

	if (!ret && priv->status)
		ret = -EIO;

	return ret;
}

/**
 * fcs_ctx_begin() - open a crypto session
 * @priv: state returned by fcs_init()
 * @owner: opaque token identifying the session owner
 *
 * Opens a session and records @owner as its owner. The session stays
 * open after return.
 *
 * Return: 0 on success, negative errno on failure.
 */
int fcs_ctx_begin(struct socfpga_fcs_priv *priv, const void *owner)
{
	int ret;

	/* Only a real owner token can own a session. */
	if (!owner)
		return -EINVAL;

	if (mutex_lock_interruptible(&priv->lock))
		return -ERESTARTSYS;

	if (priv->removed) {
		ret = -ENODEV;
		goto unlock;
	}

	ret = fcs_open_session_locked(priv, owner);

unlock:
	mutex_unlock(&priv->lock);

	return ret;
}

/**
 * fcs_ctx_end() - finish the current context and close the session
 * @priv: state returned by fcs_init()
 * @owner: token passed to fcs_ctx_begin()
 *
 * Closes the session if @owner owns it. Also called from file
 * release, so it waits for the lock.
 *
 * Return: 0 on success or when no session is open, negative errno
 * otherwise.
 */
int fcs_ctx_end(struct socfpga_fcs_priv *priv, const void *owner)
{
	int ret;

	if (!owner)
		return -EINVAL;

	mutex_lock(&priv->lock);

	if (!priv->session_opened) {
		ret = 0;
		goto unlock;
	}

	if (priv->session_owner != owner) {
		ret = -EPERM;
		goto unlock;
	}

	ret = fcs_close_session_locked(priv);

unlock:
	mutex_unlock(&priv->lock);

	return ret;
}

/**
 * fcs_get_atf_version() - return the cached Arm Trusted Firmware version
 * @priv: state returned by fcs_init()
 * @version: three u32 entries for major, minor and patch
 *
 * Return: 0 on success, -ENODATA if no version was obtained.
 */
int fcs_get_atf_version(struct socfpga_fcs_priv *priv, u32 *version)
{
	if (!priv->atf_version_valid)
		return -ENODATA;

	memcpy(version, priv->atf_version, sizeof(priv->atf_version));

	return 0;
}

/**
 * fcs_sdos_output_size() - validate an SDOS input length and size its output
 * @op_mode: value 1 for encrypt, value 0 for decrypt
 *	      Any other value is rejected with ``EINVAL``.
 * @src_len: length of the input, including the SDOS header
 * @out_len: receives the output capacity the SDM may need
 *
 * Return: 0 on success, -EINVAL on a bad @op_mode or @src_len.
 */
int fcs_sdos_output_size(u32 op_mode, u32 src_len, u32 *out_len)
{
	if (op_mode > 1)
		return -EINVAL;

	if (op_mode) {
		/* encrypt: input is header + plaintext */
		if (src_len < SDOS_DECRYPTED_MIN_SZ ||
		    src_len > SDOS_DECRYPTED_MAX_SZ)
			return -EINVAL;

		*out_len = SDOS_ENCRYPTED_MAX_SZ;
	} else {
		/* decrypt: input is header + plaintext + HMAC */
		if (src_len < SDOS_ENCRYPTED_MIN_SZ ||
		    src_len > SDOS_ENCRYPTED_MAX_SZ)
			return -EINVAL;

		*out_len = SDOS_DECRYPTED_MAX_SZ;
	}

	return 0;
}

/**
 * fcs_sdos_crypt() - perform an SDOS encrypt or decrypt operation
 * @priv: state returned by fcs_init()
 * @owner: token passed to fcs_ctx_begin() when the session was opened
 * @req: request describing the operation, using plain kernel buffers
 *
 * On return, @req->status and @req->status_valid carry the SDM status
 * if the transaction completed, even if the call failed.
 *
 * Return: 0 on success, negative errno on failure.
 */
int fcs_sdos_crypt(struct socfpga_fcs_priv *priv, const void *owner,
		   struct fcs_sdos_req *req)
{
	struct fcs_cmd_params params = { };
	u32 output_size;
	int ret;

	if (!owner || !req->src || !req->dst)
		return -EINVAL;

	ret = fcs_sdos_output_size(req->op_mode, req->src_len, &output_size);
	if (ret) {
		dev_dbg(priv->client.dev, "Invalid SDOS src_size %u\n", req->src_len);
		return ret;
	}

	/* The caller must have sized the output buffer for the worst case. */
	if (req->dst_len < output_size)
		return -EINVAL;

	/*
	 * SDM requires a non-zero context ID per request. A fresh random
	 * value per call, not per session, also keeps a late response to a
	 * timed-out request from being mistaken for the reply to the next
	 * one on the same session. The engine never generates it.
	 */
	params.context_id = get_random_u32_above(0);
	params.src = req->src;
	params.src_len = req->src_len;
	params.dst = req->dst;
	params.dst_len = output_size;
	params.arg[0] = req->op_mode;
	/* Owner ID is stored little-endian in the SDOS header (offset 12) */
	params.arg[1] = get_unaligned_le64((const u8 *)req->src + OWNER_ID_OFFSET);

	/*
	 * The session is never closed here on failure, for any reason
	 * (transport failure, a fatal SDM status or a bad response size):
	 * the caller is in charge of the session lifecycle, so a failed
	 * request is reported and the session is left exactly as it was.
	 * The caller decides whether to retry on the same session or close
	 * it with FCS_IOC_CLOSE_SESSION.
	 */
	ret = socfpga_fcs_core_engine(priv, owner, FCS_DEV_SDOS_DATA_EXT,
				      &params, true);

	/*
	 * Relay the SDM status whenever the mailbox transaction completed,
	 * even if the engine then rejected the result (e.g. an output
	 * larger than the buffer): the front-end writes it to error_code.
	 * On a transport failure status_valid is clear and nothing is
	 * relayed.
	 */
	req->status = params.status;
	req->status_valid = params.status_valid;

	if (ret) {
		dev_err(priv->client.dev,
			"Failed to send the cmd=%d,ret=%d\n", FCS_DEV_SDOS_DATA_EXT, ret);
		return ret;
	}

	if (params.status &&
	    params.status != SDOS_DECRYPTION_REPROVISION_KEY_WARN &&
	    params.status != SDOS_DECRYPTION_NOT_LATEST_KEY_WARN) {
		dev_err(priv->client.dev,
			"Failed to perform SDOS operation Mailbox Status = 0x%x\n",
			params.status);
		return -EIO;
	}

	req->dst_len = params.dst_len;

	return 0;
}

/**
 * fcs_read_version_from_atf() - query the Arm Trusted Firmware build version
 * @priv: FCS private state
 *
 * Send the ATF version command to the SDM and cache the result in @priv.
 *
 * Return: 0 on success, negative errno on failure.
 */
static int fcs_read_version_from_atf(struct socfpga_fcs_priv *priv)
{
	int ret;

	ret = fcs_svc_send_request(priv, FCS_DEV_ATF_VERSION,
				   SVC_FCS_REQUEST_TIMEOUT_MS, NULL);
	if (ret) {
		dev_err(priv->client.dev,
			"Failed to send the cmd=%d,ret=%d\n",
			FCS_DEV_ATF_VERSION, ret);
		goto svc_done;
	}

	if (priv->status) {
		ret = -EIO;
		dev_err(priv->client.dev,
			"Mailbox error, Failed to read ATF version ret: %d\n", ret);
	}

svc_done:
	stratix10_svc_done(priv->chan);

	return ret;
}

/**
 * fcs_kref_release() - free the FCS state when the last reference is dropped
 * @kref: reference counter embedded in the FCS state
 */
static void fcs_kref_release(struct kref *kref)
{
	struct socfpga_fcs_priv *priv =
		container_of(kref, struct socfpga_fcs_priv, refcount);

	if (priv->chan) {
		if (priv->session_opened) {
			int ret;

			mutex_lock(&priv->lock);
			ret = fcs_close_session_locked(priv);
			mutex_unlock(&priv->lock);

			if (ret)
				dev_err(priv->client.dev,
					"Failed to close FCS service session,ret=%d\n", ret);
		}

		stratix10_svc_remove_async_client(priv->chan);
		stratix10_svc_free_channel(priv->chan);
	}

	mutex_destroy(&priv->lock);
	kfree(priv);
}

/**
 * socfpga_fcs_get() - take a reference on the FCS state
 * @priv: state returned by fcs_init()
 */
void socfpga_fcs_get(struct socfpga_fcs_priv *priv)
{
	kref_get(&priv->refcount);
}

/**
 * socfpga_fcs_put() - drop a reference on the FCS state
 * @priv: state returned by fcs_init()
 */
void socfpga_fcs_put(struct socfpga_fcs_priv *priv)
{
	kref_put(&priv->refcount, fcs_kref_release);
}

/**
 * fcs_mark_removed() - refuse further operations after the device is gone
 * @priv: state returned by fcs_init()
 */
void fcs_mark_removed(struct socfpga_fcs_priv *priv)
{
	mutex_lock(&priv->lock);
	priv->removed = true;

	if (priv->session_opened)
		fcs_close_session_locked(priv);

	if (priv->chan)	{
		stratix10_svc_remove_async_client(priv->chan);
		stratix10_svc_free_channel(priv->chan);
		priv->chan = NULL;
	}

	mutex_unlock(&priv->lock);
}

/**
 * fcs_init() - allocate and initialise the FCS private state
 * @dev: pointer to fcs device
 *
 * Return: the new state, or an ERR_PTR on failure.
 */
struct socfpga_fcs_priv *fcs_init(struct device *dev)
{
	struct socfpga_fcs_priv *priv;
	int ret;

	priv = kzalloc_obj(*priv);
	if (!priv)
		return ERR_PTR(-ENOMEM);

	kref_init(&priv->refcount);
	mutex_init(&priv->lock);

	/* kzalloc() already cleared client.receive_cb. */
	priv->client.dev = dev;
	priv->client.priv = priv;

	priv->chan = stratix10_svc_request_channel_byname(&priv->client,
							  SVC_CLIENT_FCS);
	if (IS_ERR(priv->chan)) {
		dev_err(dev, "couldn't get service channel %s\n", SVC_CLIENT_FCS);
		ret = PTR_ERR(priv->chan);
		goto err_free;
	}

	ret = stratix10_svc_add_async_client(priv->chan, true);
	if (ret) {
		dev_err(dev, "Failed to add async client\n");
		stratix10_svc_free_channel(priv->chan);
		goto err_free;
	}

	init_completion(&priv->completion);

	/*
	 * Version query failure is non-fatal; sysfs reports -ENODATA.
	 */
	fcs_read_version_from_atf(priv);

	return priv;

err_free:
	mutex_destroy(&priv->lock);
	kfree(priv);

	return ERR_PTR(ret);
}
