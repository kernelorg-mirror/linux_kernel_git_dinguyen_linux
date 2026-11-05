/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note */
/*
 * Description:
 * This driver is developed for the SDM SoCFPGA Crypto Service (FCS). The
 * caller must call FCS_IOC_OPEN_SESSION and close it with
 * FCS_IOC_CLOSE_SESSION (also done automatically on fd close) in order to
 * use the SDM crypto services. The caller owns the crypto session lifecycle
 * once it opens it.
 * Only one session may be open at a time across the whole device. The
 * per-request context ID is managed by the kernel internally and is not
 * part of the user ABI.
 * The driver also provides an ioctl interface for the SDOS
 * (Secure Data Object Service) for encrypt/decrypt operation using SDOS
 * service root key certificate.
 */
#ifndef __SOCFPGA_FCS_CRYPTO_H
#define __SOCFPGA_FCS_CRYPTO_H

#include <linux/types.h>
#include <linux/ioctl.h>

/**
 * struct fcs_ioc_sdos - argument for FCS_IOC_SDOS
 * @src: address of the input buffer. The owner ID is a little-endian 64-bit
 *	 value at offset 12 of this buffer and is passed to the SDM with the
 *	 request.
 * @dst: address of the output buffer. This address is an input and must be
 *	 non-NULL; NULL is rejected with ``EINVAL``. The kernel does not
 *	 write a new address back.
 * @src_size: number of bytes at @src. For encrypt the input is a 40-byte
 *	      header followed by 32..32672 bytes of plaintext; for decrypt it
 *	      additionally carries a trailing 48-byte HMAC.
 * @dst_size: on entry, the capacity of @dst in bytes. On success the kernel
 *	      overwrites it with the number of bytes the SDM produced. On
 *	      failure it is left unchanged.
 * @op_mode: value 1 for encrypt, value 0 for decrypt. Any other value is
 *	     rejected with ``EINVAL``.
 * @error_code: SDM status. Set it to 0 before the call. The kernel writes it
 *		whenever the request reached the SDM, including when the SDM
 *		reported a failure, and leaves it unchanged otherwise.
 * @flags: must be 0. Reserved for future extensions; any other value is
 *	   rejected with ``EINVAL``.
 * @reserved: must be 0. Any other value is rejected with ``EINVAL``.
 *
 * ``src`` and ``dst`` are user-space addresses stored in ``__u64``, so the
 * structure has the same layout for 32-bit and 64-bit callers and contains
 * no implicit padding. The kernel writes back only ``dst_size`` and
 * ``error_code``.
 */
struct fcs_ioc_sdos {
	__u64 src;		/* __user ptr to input buffer (in) */
	__u64 dst;		/* __user ptr to output buffer (in) */
	__u32 src_size;		/* bytes at src (in) */
	__u32 dst_size;		/* capacity (in), bytes produced (out) */
	__u32 op_mode;		/* 1 encrypt, 0 decrypt (in) */
	__s32 error_code;	/* SDM status (out); set to 0 before the call */
	__u32 flags;		/* must be 0 (in) */
	__u32 reserved;		/* must be 0 (in) */
};

#define FCS_IOC_MAGIC		0xA7

/**
 * DOC: FCS_IOC_OPEN_SESSION
 *
 * Open a crypto session on the SDM. Only one session may be open at a time
 * for the whole device, and it belongs to the file description that opened
 * it: only that file may run commands on it or close it. A second open fails
 * with ``EBUSY`` until the first is closed.
 *
 * Return: 0 on success, -1 on failure with ``errno`` set.
 *
 * ===========  =======================================================
 * Error        Description
 * ===========  =======================================================
 * EBUSY        A session is already open.
 * ENODEV       The device was unbound while the file descriptor was
 *              open.
 * EIO          The SDM reported a fatal error opening the session.
 * ETIMEDOUT    The SDM did not complete the request within the
 *              service request timeout.
 * EINTR        A signal arrived while waiting for another request.
 * ===========  =======================================================
 */
#define FCS_IOC_OPEN_SESSION		_IO(FCS_IOC_MAGIC, 1)

/**
 * DOC: FCS_IOC_CLOSE_SESSION
 *
 * Close the crypto session on the SDM. A no-op, returning 0, if no session
 * is open. Also performed automatically when the owning file is closed.
 *
 * Return: 0 on success, -1 on failure with ``errno`` set.
 *
 * ==========  ========================================================
 * Error       Description
 * ==========  ========================================================
 * EPERM       The session was opened through a different file.
 * EIO         The SDM reported a fatal error closing the session.
 *             The local session state is dropped regardless, so a
 *             future open is not blocked.
 * ETIMEDOUT   The SDM did not complete the request within the
 *             service request timeout. The local session state is
 *             dropped regardless, as for ``EIO``.
 * ==========  ========================================================
 */
#define FCS_IOC_CLOSE_SESSION		_IO(FCS_IOC_MAGIC, 2)

/**
 * DOC: FCS_IOC_SDOS
 *
 * Encrypt or decrypt one SDOS object. Takes a pointer to a
 * &struct fcs_ioc_sdos. Requires a crypto session opened beforehand with
 * FCS_IOC_OPEN_SESSION on the same file; fails with ``ENOTCONN`` if no
 * session is open and ``EPERM`` if another file owns it.
 *
 * The SDM derives the working key from a device-unique SDOS root key that
 * never leaves the secure boundary, so the host never handles raw key
 * material. Only one SDM transaction is in flight at a time.
 *
 * A failed request, for any reason, never closes the session: it is left
 * open for the caller to retry or to close explicitly with
 * FCS_IOC_CLOSE_SESSION.
 *
 * Return: 0 on success, -1 on failure with ``errno`` set. On success
 * ``dst_size`` holds the number of bytes produced; on failure it is left
 * unchanged. The SDM status is written to ``error_code`` whenever the
 * request reached the SDM. Set ``error_code`` to 0 before the call: a
 * failure that never reaches the SDM leaves it unchanged. Some non-zero
 * statuses indicate success with a warning. See
 * Documentation/misc-devices/socfpga-fcs.rst for the error codes and the SDM
 * status values.
 */
#define FCS_IOC_SDOS		_IOWR(FCS_IOC_MAGIC, 3, struct fcs_ioc_sdos)

#endif /* __SOCFPGA_FCS_CRYPTO_H */
