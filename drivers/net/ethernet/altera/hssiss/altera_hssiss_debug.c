// SPDX-License-Identifier: GPL-2.0
/* Altera FPGA HSSI SS debugfs
 * Copyright (C) 2022, 2026 Altera Corporation. All rights reserved
 *
 * Contributors:
 *   Subhransu S. Prusty
 *   Preetam Narayan
 *
 */
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <linux/debugfs.h>
#include "../altera_utils.h"
#include "altera_hssiss.h"
#include "altera_hssiss_csr.h"

struct altera_hssiss_dbg_read_data {
	u32 dr_grp; /* get_hss_profile */
	u32 profile; /* get_hss_profile */
	u32 data; /* port_data for mac_stat, data for link_status, fw_version and csr */
	u32 max_tx_frame_size;
	u32 max_rx_frame_size;
};

struct altera_hssiss_dbg {
	struct altera_hssiss *hssiss;
	struct dentry *dbgfs;
	enum altera_hssiss_sal_usrcmd sal_cmd;
	struct altera_hssiss_dbg_read_data read;
};

static struct altera_eth_intf
*hssiss_get_intf_from_port(struct altera_hssiss *priv, u32 port)
{
	for (int i = 0; i < ALTERA_HSSISS_NUM_INTF; i++) {
		if (priv->intf_map[i].port == port)
			return priv->intf_map[i].intf;
	}

	return NULL;
}

static ssize_t csr_read(struct file *filep, char __user *ubuf,
			size_t count, loff_t *offp)
{
	struct altera_hssiss_dbg *d = filep->private_data;
	char buf[10];
	int size;

	size = snprintf(buf, sizeof(buf), "%x\n", d->read.data);

	return simple_read_from_buffer(ubuf, count, offp, buf, size);
}

/*
 * hssiss_dbgfs_csr_write() - hssiss debugfs-node csr write callback
 * for read:
 *	echo "ch type offset word" > hssi_reg
 * for write:
 *	echo "ch type offset word data" > hssi_reg
 *
 * word: 1 for word read/write, 0 for byte read/write
 */
static ssize_t csr_write(struct file *filep, const char __user *ubuf,
			 size_t count, loff_t *offp)
{
	struct altera_hssiss_dbg *d = filep->private_data;
	struct altera_hssiss *priv = d->hssiss;
	struct get_set_csr_data data;
	char *buf;
	int word, type, ch, ret;
	u32 offset, val;

	/* Copy data from User-space */
	buf = kmalloc(count + 1, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	ret = simple_write_to_buffer(buf, count, offp, ubuf, count);
	if (ret < 0)
		goto free_buf;
	buf[count] = 0;

	/* Parse the values */
	ret = sscanf(buf, "%d %d %x %d %x", &ch, &type, &offset, &word, &val);

	if (ret < 4) {
		ret = -EINVAL;
		goto free_buf;
	}

	data.ch = ch;
	data.reg_type = type;
	data.offs = offset;
	data.word = word ? true : false;
	data.data = val;

	if (ret == 4) {
		ret = priv->ops.get_set_csr(priv, &data, true);
		if (ret == 0)
			d->read.data = data.data;
	} else {
		ret = priv->ops.get_set_csr(priv, &data, false);
	}

free_buf:
	kfree(buf);
	return (ret < 0 ? ret : count);
}

/*
 * hssiss_dbgfs_sal_read() - hssiss debugfs-node SAL read callback
 * Note: Except get/set csr. Use get/set csr dbgfs to read csr registers.
 */
static ssize_t sal_read(struct file *filep, char __user *ubuf,
			size_t count, loff_t *offp)
{
	struct altera_hssiss_dbg *d = filep->private_data;
	char buf[100];
	int size;

	switch (d->sal_cmd) {
	case SAL_GET_HSSI_PROFILE:
		size = scnprintf(buf, sizeof(buf),
				 "dr_grp: %x profile: %x",
				d->read.dr_grp, d->read.profile);
		break;
	case SAL_READ_MAC_STAT:
		size = scnprintf(buf, sizeof(buf), "%x", d->read.data);
		break;
	case SAL_GET_MTU:
		size = scnprintf(buf, sizeof(buf),
				 "max_tx_frame_size: %x max_rx_frame_size:%x",
				 d->read.max_tx_frame_size,
				 d->read.max_rx_frame_size);
		break;
	case SAL_NCSI_GET_LINK_STS:
		size = scnprintf(buf, sizeof(buf), "%x", d->read.data);
		break;
	case SAL_FW_VERSION:
		size = scnprintf(buf, sizeof(buf), "%x", d->read.data);
		break;
	default:
		size = scnprintf(buf, sizeof(buf), "No command in progress\n");
		break;
	}

	return simple_read_from_buffer(ubuf, count, offp, buf, size);
}

/*
 * hssiss_dbgfs_sal_write() - hssiss debugfs-node sal write callback
 * Note: Except get/set csr. Use get/set csr dbgfs to read csr registers.
 */
static ssize_t sal_write(struct file *filep, const char __user *ubuf,
			 size_t count, loff_t *offp)
{
	struct altera_hssiss_dbg *d = filep->private_data;
	struct altera_hssiss *priv = d->hssiss;
	struct altera_eth_intf *intf;
	char *buf;
	u32 cmd, port;
	int ret;

	/* Copy data from User-space */
	buf = kmalloc(count + 1, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	ret = simple_write_to_buffer(buf, count, offp, ubuf, count);
	if (ret < 0)
		goto free_buf;
	buf[count] = 0;

	/* Parse SAL command */
	ret = kstrtou32(buf, 0, &cmd);
	if (ret < 0)
		goto free_buf;

	d->sal_cmd = cmd;

	/* Parse and prepare data for command */
	switch (cmd) {
	case SAL_GET_HSSI_PROFILE:
	case SAL_SET_HSSI_PROFILE:
	{
		struct get_set_dr_data data;

		ret = sscanf(buf, "%x %x %x %x", &cmd, &data.dr_grp, &data.profile, &port);
		if (ret != 4) {
			ret = -EINVAL;
			goto free_buf;
		}

		intf = hssiss_get_intf_from_port(priv, port);
		if (!intf) {
			ret = -EINVAL;
			goto free_buf;
		}

		if (cmd == SAL_SET_HSSI_PROFILE)
			altera_hssiss_get_set_dr_profile(intf, &data, true);
		else
			altera_hssiss_get_set_dr_profile(intf, &data, false);

		d->read.dr_grp = data.dr_grp;
		d->read.profile = data.profile;

		break;
	}
	case SAL_READ_MAC_STAT:
	{
		enum altera_hssiss_mac_stat_counter_type type;
		int lsb;

		ret = sscanf(buf, "%x %x %d %x", &cmd, &type, &lsb, &port);
		if (ret != 4) {
			ret = -EINVAL;
			goto free_buf;
		}

		intf = hssiss_get_intf_from_port(priv, port);
		if (!intf) {
			ret = -EINVAL;
			goto free_buf;
		}

		d->read.data = altera_hssiss_read_mac_stat(intf, type, lsb);

		break;
	}
	case SAL_GET_MTU:
	{
		u16 max_tx_frame_size, max_rx_frame_size;

		ret = sscanf(buf, "%x %x", &cmd, &port);
		if (ret != 2) {
			ret = -EINVAL;
			goto free_buf;
		}

		intf = hssiss_get_intf_from_port(priv, port);
		if (!intf) {
			ret = -EINVAL;
			goto free_buf;
		}

		altera_hssiss_get_mtu(intf, &max_tx_frame_size, &max_rx_frame_size);
		d->read.max_tx_frame_size = max_tx_frame_size;
		d->read.max_rx_frame_size = max_rx_frame_size;

		break;
	}
	case SAL_SET_MTU:
	{
		u16 max_tx_frame_size, max_rx_frame_size;

		ret = sscanf(buf, "%x %hu %hu %x",
			     &cmd, &max_tx_frame_size, &max_rx_frame_size, &port);
		if (ret != 4) {
			ret = -EINVAL;
			goto free_buf;
		}

		intf = hssiss_get_intf_from_port(priv, port);
		if (!intf) {
			ret = -EINVAL;
			goto free_buf;
		}

		altera_hssiss_set_mtu(intf, max_tx_frame_size, max_rx_frame_size);

		break;
	}
	case SAL_RESET_MAC_STAT:
	{
		int tx, rx;

		ret = sscanf(buf, "%x %d %d %x", &cmd, &tx, &rx, &port);
		if (ret != 4) {
			ret = -EINVAL;
			goto free_buf;
		}

		intf = hssiss_get_intf_from_port(priv, port);
		if (!intf) {
			ret = -EINVAL;
			goto free_buf;
		}

		altera_hssiss_reset_mac_stat(intf, (tx ? true : false), (rx ? true : false));

		break;
	}
	case SAL_NCSI_GET_LINK_STS:
	{
		union ncsi_link_status_data data;

		ret = sscanf(buf, "%x %x %x", &cmd, &data.full, &port);
		if (ret != 3) {
			ret = -EINVAL;
			goto free_buf;
		}

		intf = hssiss_get_intf_from_port(priv, port);
		if (!intf) {
			ret = -EINVAL;
			goto free_buf;
		}

		altera_hssiss_ncsi_link_status(intf, &data);
		d->read.data = data.full;

		break;
	}
	case SAL_FW_VERSION:
	{
		u32 data;

		ret = sscanf(buf, "%x %x", &cmd, &port);
		if (ret != 2) {
			ret = -EINVAL;
			goto free_buf;
		}

		intf = hssiss_get_intf_from_port(priv, port);
		if (!intf) {
			ret = -EINVAL;
			goto free_buf;
		}

		altera_hssiss_get_fw_version(intf, &data);
		d->read.data = data;

		break;
	}

	case SAL_DISABLE_LOOPBACK:
	case SAL_ENABLE_LOOPBACK:
	{
		ret = sscanf(buf, "%x %x", &cmd, &port);
		if (ret != 2) {
			ret = -EINVAL;
			goto free_buf;
		}

		intf = hssiss_get_intf_from_port(priv, port);
		if (!intf) {
			ret = -EINVAL;
			goto free_buf;
		}

		if (cmd == SAL_ENABLE_LOOPBACK)
			altera_hssiss_enable_disable_loopback(intf, true);
		else
			altera_hssiss_enable_disable_loopback(intf, false);
		break;
	}

	default:
		ret = -EINVAL;
		break;
	}

free_buf:
	kfree(buf);
	return (ret < 0 ? ret : count);
}

#define BUF_SIZE	PAGE_SIZE
static ssize_t readme_read(struct file *filep, char __user *ubuf,
			   size_t count, loff_t *offp)
{
	char *buf;
	int ret;

	buf = kzalloc(BUF_SIZE, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	ret = scnprintf(buf, BUF_SIZE, "get_csr: to read byte data:\n");
	ret += scnprintf(buf + ret, BUF_SIZE - ret,
			 "\techo \"ch type offset 0\" > hssi_reg\n");
	ret += scnprintf(buf + ret, BUF_SIZE - ret, "\tcat hssi_reg\n");
	ret += scnprintf(buf + ret, BUF_SIZE - ret, "get_csr: to read word data:\n");
	ret += scnprintf(buf + ret, BUF_SIZE - ret,
			 "\techo \"ch type offset 1\" > hssi_reg\n");
	ret += scnprintf(buf + ret, BUF_SIZE - ret, "\tcat hssi_reg\n");
	ret += scnprintf(buf + ret, BUF_SIZE - ret, "set_csr: to write byte data:\n");
	ret += scnprintf(buf + ret, BUF_SIZE - ret,
			 "\techo \"ch type offset 0 data\" > hssi_reg\n");
	ret += scnprintf(buf + ret, BUF_SIZE - ret, "set_csr: to write word data:\n");
	ret += scnprintf(buf + ret, BUF_SIZE - ret,
			 "\techo \"ch type offset 1 data\" > hssi_reg\n");
	ret += scnprintf(buf + ret, BUF_SIZE - ret, "Execute sal command:\n");
	ret += scnprintf(buf + ret, BUF_SIZE - ret, "\techo \"cmd x y z\" > sal\n");
	ret += scnprintf(buf + ret, BUF_SIZE - ret, "\tcmd: SAL command\n");
	ret += scnprintf(buf + ret, BUF_SIZE - ret, "\tx, y, z: SAL command specific data\n");
	ret += scnprintf(buf + ret, BUF_SIZE - ret, "\tcat sal\n");
	ret += scnprintf(buf + ret, BUF_SIZE - ret, "Execute direct SAL command:\n");
	ret += scnprintf(buf + ret, BUF_SIZE - ret,
			 "\techo <ctrladdr reg_data> > ctrladdr\n");
	ret += scnprintf(buf + ret, BUF_SIZE - ret, "\tfor write: echo <wr reg_data> > wr\n");
	ret += scnprintf(buf + ret, BUF_SIZE - ret, "\techo <cmdsts reg_data> > cmdsts\n");
	ret += scnprintf(buf + ret, BUF_SIZE - ret, "\tto check ack or err: cat cmdsts\n");
	ret += scnprintf(buf + ret, BUF_SIZE - ret, "\tto read data: cat rd\n");
	ret += scnprintf(buf + ret, BUF_SIZE - ret, "Execute direct register access:\n");
	ret += scnprintf(buf + ret, BUF_SIZE - ret,
			 "\tfor wr: echo <baseaddr offset direct val> > direct_reg\n");
	ret += scnprintf(buf + ret, BUF_SIZE - ret,
			 "\tfor rd: echo <baseaddr offset direct> > direct_reg\n");
	ret += scnprintf(buf + ret, BUF_SIZE - ret, "\tcat direct_reg\n");

	ret = simple_read_from_buffer(ubuf, count, offp, buf, ret);

	kfree(buf);
	return ret;
}

static ssize_t dumpcsr_read(struct file *filep, char __user *ubuf,
			    size_t count, loff_t *offp)
{
	struct altera_hssiss_dbg *d = filep->private_data;
	struct altera_hssiss *priv = d->hssiss;
	void __iomem *base = priv->sscsr;
	char *buf;
	int ret;
	int i;
	u32 val;

	buf = kzalloc(BUF_SIZE, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	ret = scnprintf(buf, BUF_SIZE, "Dumping device feature registers\n");
	for (i = 0; i < 10; i++)
		ret += scnprintf(buf + ret, BUF_SIZE - ret, "\t%x: %x\n",
				(i * 4), csrrd32(base, (i * 4)));

	ret += scnprintf(buf + ret, BUF_SIZE - ret, "Dumping other CSR registers\n");

	val = csrrd32_withoffset(priv, HSSISS_CSR_VER);
	ret += scnprintf(buf + ret, BUF_SIZE - ret, "HSSISS_CSR_VER: %x\n", val);

	val = csrrd32_withoffset(priv, HSSISS_CSR_COMMON_FEATURE_LIST);
	ret += scnprintf(buf + ret, BUF_SIZE - ret, "HSSISS_CSR_COMMON_FEATURE_LIST: %x\n", val);

	ret += scnprintf(buf + ret, BUF_SIZE - ret, "Dumping port attributes\n");
	for (i = 0; i < 16; i++) { /* E-tile and FGT in F-tile */
		val = csrrd32_withoffset(priv,
					 HSSISS_CSR_INTER_ATTRIB_PORT + (i * 4));
		ret += scnprintf(buf + ret, BUF_SIZE - ret, "\t%x: %x\n", i, val);
	}

	if (priv->ver == HSSISS_FTILE) { /* For F-tile FHT only */
		for (i = 16; i < 20; i++) {
			val = csrrd32(base, HSSISS_CSR_INTER_ATTRIB_PORT_FHT + (i * 4));
			ret += scnprintf(buf + ret, BUF_SIZE - ret, "\t%x: %x\n", i, val);
		}
	}

	val = csrrd32_withoffset(priv, HSSISS_CSR_CMDSTS);
	ret += scnprintf(buf + ret, BUF_SIZE - ret, "HSSISS_CSR_CMDSTS: %x\n", val);

	val = csrrd32_withoffset(priv, HSSISS_CSR_CTRLADDR);
	ret += scnprintf(buf + ret, BUF_SIZE - ret, "HSSISS_CSR_CTRLADDR: %x\n", val);

	val = csrrd32_withoffset(priv, HSSISS_CSR_RD_DATA);
	ret += scnprintf(buf + ret, BUF_SIZE - ret, "HSSISS_CSR_RD_DATA: %x\n", val);

	val = csrrd32_withoffset(priv, HSSISS_CSR_WR_DATA);
	ret += scnprintf(buf + ret, BUF_SIZE - ret, "HSSISS_CSR_WR_DATA: %x\n", val);

	val = csrrd32_withoffset(priv, HSSISS_CSR_GMII_TX_LATENCY);
	ret += scnprintf(buf + ret, BUF_SIZE - ret, "HSSISS_CSR_GMII_TX_LATENCY: %x\n", val);

	val = csrrd32_withoffset(priv, HSSISS_CSR_GMII_RX_LATENCY);
	ret += scnprintf(buf + ret, BUF_SIZE - ret, "HSSISS_CSR_GMII_RX_LATENCY: %x\n", val);

	ret += scnprintf(buf + ret, BUF_SIZE - ret, "Dumping port status\n");

	for (i = 0; i < 15; i++) { /* E-tile and FGT in F-tile */
		val = csrrd32_withoffset(priv,
					 HSSISS_CSR_ETH_PORT_STS + (i * 4));
		ret += scnprintf(buf + ret, BUF_SIZE - ret, "\t%x: %x\n", i, val);
	}

	if (priv->ver == HSSISS_FTILE) { /* For F-tile FHT only */
		for (i = 16; i < 20; i++) {
			val = csrrd32(base, HSSISS_CSR_ETH_PORT_STS_FHT + (i * 4));
			ret += scnprintf(buf + ret, BUF_SIZE - ret, "\t%x: %x\n", i, val);
		}
	}

	val = csrrd32_withoffset(priv, HSSISS_CSR_TSE_CTRL);
	ret += scnprintf(buf + ret, BUF_SIZE - ret, "HSSISS_CSR_TSE_CTRL: %x\n", val);

	val = csrrd32_withoffset(priv, HSSISS_CSR_DBG_CTRL);
	ret += scnprintf(buf + ret, BUF_SIZE - ret, "HSSISS_CSR_DBG_CTRL: %x\n", val);

	val = csrrd32_withoffset(priv, HSSISS_CSR_HOTPLUG_DBG_CTRL);
	ret += scnprintf(buf + ret, BUF_SIZE - ret, "HSSISS_CSR_HOTPLUG_DBG_CTRL: %x\n", val);

	val = csrrd32_withoffset(priv, HSSISS_CSR_HOTPLUG_DBG_STS);
	ret += scnprintf(buf + ret, BUF_SIZE - ret, "HSSISS_CSR_HOTPLUG_DBG_STS: %x\n", val);

	ret = simple_read_from_buffer(ubuf, count, offp, buf, ret);

	kfree(buf);
	return ret;
}

static ssize_t mailbox_reg_write(struct file *filep, const char __user *ubuf,
				 size_t count, loff_t *offp, u32 reg)
{
	struct altera_hssiss_dbg *d = filep->private_data;
	struct altera_hssiss *priv = d->hssiss;
	u32 val;
	char *buf;
	int ret;

	/* Copy data from User-space */
	buf = kmalloc(count + 1, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	ret = simple_write_to_buffer(buf, count, offp, ubuf, count);
	if (ret < 0) {
		kfree(buf);
		return -EIO;
	}
	buf[count] = 0;

	/* Parse the values */
	ret = kstrtou32(buf, 0, &val);
	kfree(buf);
	if (ret < 0)
		return ret;

	csrwr32_withoffset(priv, val, reg);

	return count;
}

static ssize_t mailbox_reg_read(struct file *filep, char __user *ubuf,
				size_t count, loff_t *offp, u32 reg)
{
	struct altera_hssiss_dbg *d = filep->private_data;
	struct altera_hssiss *priv = d->hssiss;
	char buf[10];
	u32 val;
	int size;

	val = csrrd32_withoffset(priv, reg);

	size = snprintf(buf, sizeof(buf), "%x\n", val);

	return simple_read_from_buffer(ubuf, count, offp, buf, size);
}

static ssize_t ctrladdr_read(struct file *filep, char __user *ubuf,
			     size_t count, loff_t *offp)
{
	return mailbox_reg_read(filep, ubuf, count, offp, HSSISS_CSR_CTRLADDR);
}

static ssize_t ctrladdr_write(struct file *filep, const char __user *ubuf,
			      size_t count, loff_t *offp)
{
	return mailbox_reg_write(filep, ubuf, count, offp, HSSISS_CSR_CTRLADDR);
}

static ssize_t cmdsts_read(struct file *filep, char __user *ubuf,
			   size_t count, loff_t *offp)
{
	return mailbox_reg_read(filep, ubuf, count, offp, HSSISS_CSR_CMDSTS);
}

static ssize_t cmdsts_write(struct file *filep, const char __user *ubuf,
			    size_t count, loff_t *offp)
{
	return  mailbox_reg_write(filep, ubuf, count, offp, HSSISS_CSR_CMDSTS);
}

static ssize_t wr_reg_read(struct file *filep, char __user *ubuf,
			   size_t count, loff_t *offp)
{
	return mailbox_reg_read(filep, ubuf, count, offp, HSSISS_CSR_WR_DATA);
}

static ssize_t wr_reg_write(struct file *filep, const char __user *ubuf,
			    size_t count, loff_t *offp)
{
	return  mailbox_reg_write(filep, ubuf, count, offp, HSSISS_CSR_WR_DATA);
}

static ssize_t rd_reg_read(struct file *filep, char __user *ubuf,
			   size_t count, loff_t *offp)
{
	return mailbox_reg_read(filep, ubuf, count, offp, HSSISS_CSR_RD_DATA);
}

static ssize_t rd_reg_write(struct file *filep, const char __user *ubuf,
			    size_t count, loff_t *offp)
{
	return  mailbox_reg_write(filep, ubuf, count, offp, HSSISS_CSR_RD_DATA);
}

static ssize_t direct_reg_read(struct file *filep, char __user *ubuf,
			       size_t count, loff_t *offp)
{
	struct altera_hssiss_dbg *d = filep->private_data;
	char buf[10];
	int size;

	size = snprintf(buf, sizeof(buf), "%x\n", d->read.data);

	return simple_read_from_buffer(ubuf, count, offp, buf, size);
}

static ssize_t direct_reg_write(struct file *filep, const char __user *ubuf,
				size_t count, loff_t *offp)
{
	struct altera_hssiss_dbg *d = filep->private_data;
	struct altera_hssiss *priv = d->hssiss;
	u32 base, offset, val;
	char *buf;
	int ret, direct;

	/* Copy data from User-space */
	buf = kmalloc(count + 1, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	ret = simple_write_to_buffer(buf, count, offp, ubuf, count);
	if (ret < 0) {
		kfree(buf);
		return -EIO;
	}
	buf[count] = 0;

	/* Parse the values */
	ret = sscanf(buf, "%x %x %d %x", &base, &offset, &direct, &val);
	kfree(buf);
	if (ret < 3)
		return -EINVAL;

	if (ret == 4) {
		if (direct)
			csrwr32_direct_withoffset(priv, val, (base + offset));
		else
			csrwr32_withoffset(priv, val, offset);
	} else {
		if (direct)
			d->read.data = csrrd32_direct_withoffset(priv, (base + offset));
		else
			d->read.data = csrrd32_withoffset(priv, offset);
	}

	return count;
}

static const struct file_operations ctrladdr_ops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = ctrladdr_write,
	.read = ctrladdr_read
};

static const struct file_operations cmdsts_ops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = cmdsts_write,
	.read = cmdsts_read
};

static const struct file_operations csr_ops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = csr_write,
	.read = csr_read
};

static const struct file_operations wr_reg_ops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = wr_reg_write,
	.read = wr_reg_read
};

static const struct file_operations rd_reg_ops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = rd_reg_write,
	.read = rd_reg_read
};

static const struct file_operations sal_ops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = sal_write,
	.read = sal_read
};

static const struct file_operations readme_ops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = readme_read
};

static const struct file_operations dumpcsr_ops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = dumpcsr_read
};

static const struct file_operations direct_reg_ops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = direct_reg_write,
	.read = direct_reg_read
};

struct altera_hssiss_dbg *altera_hssiss_dbgfs_init(struct platform_device *pdev,
						   struct altera_hssiss *data)
{
	struct altera_hssiss_dbg *d;
	char *hssidev_name;

	d = devm_kzalloc(&pdev->dev, sizeof(*d), GFP_KERNEL);
	if (!d)
		return NULL;

	d->hssiss = data;
	hssidev_name = kasprintf(GFP_KERNEL, "%s", dev_name(&pdev->dev));
	if (!hssidev_name)
		return NULL;

	d->dbgfs = debugfs_create_dir(hssidev_name, NULL);
	kfree(hssidev_name);
	if (IS_ERR(d->dbgfs)) {
		dev_warn(&pdev->dev, "Failed to create debugfs dir\n");
		return NULL;
	}

	/* unsafe low-level escape hatches */
	debugfs_create_file("csr", 0644, d->dbgfs, d, &csr_ops);
	debugfs_create_file("ctrladdr", 0644, d->dbgfs, d, &ctrladdr_ops);
	debugfs_create_file("cmdsts", 0644, d->dbgfs, d, &cmdsts_ops);
	debugfs_create_file("wr", 0644, d->dbgfs, d, &wr_reg_ops);
	debugfs_create_file("rd", 0644, d->dbgfs, d, &rd_reg_ops);
	debugfs_create_file("sal", 0644, d->dbgfs, d, &sal_ops);
	debugfs_create_file("dumpcsr", 0444, d->dbgfs, d, &dumpcsr_ops);
	debugfs_create_file("readme", 0444, d->dbgfs, d, &readme_ops);
	debugfs_create_file("direct_reg", 0600, d->dbgfs, d, &direct_reg_ops);

	return d;
}

void altera_hssiss_dbgfs_remove(struct altera_hssiss_dbg *d)
{
	debugfs_remove_recursive(d->dbgfs);
}
