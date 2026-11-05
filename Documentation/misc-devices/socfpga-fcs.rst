.. SPDX-License-Identifier: GPL-2.0

===================================
Altera SoCFPGA Crypto Service (FCS)
===================================

Overview
========

The Secure Device Manager (SDM) is the hardware security controller in Altera
SoCFPGA devices. It offers a runtime cryptographic interface, the FPGA Crypto
Service (FCS), which the host reaches through the stratix10-svc mailbox using
Arm Trusted Firmware SIP SMC calls.

The caller owns the crypto session lifecycle: open it once with
``FCS_IOC_OPEN_SESSION`` and close it with ``FCS_IOC_CLOSE_SESSION`` (also
performed automatically when the file descriptor is closed, including on
process exit). Only one session may be open at a time across the whole
device, and it belongs to the file description that opened it: other files
cannot run commands on it or close it. Opens from other files fail with
``EBUSY`` until the session is closed. A descriptor shared through ``fork()``
or ``dup()`` refers to the same file description and therefore shares the
session. The per-request context ID is managed by the kernel internally
and is not part of the user ABI.

This driver exposes one FCS operation: the Secure Data Object Service (SDOS),
which encrypts and decrypts data at rest. The SDM derives the working key from
a device-unique SDOS root key that never leaves the secure boundary, so the
host never handles raw key material. A primary use case is black-key
provisioning.


Why a dedicated ioctl
=====================

SDOS seals and unseals arbitrary application data of up to 32 KiB, not keys of
a bounded size, so the keyring payload limits of the trusted-keys subsystem do
not apply to it. The object it produces is a firmware-defined container rather
than a generic AEAD output, no key is ever set from the host so the crypto API
has nothing to bind a transform to, and the SDM returns per-operation status
values that must reach the caller alongside a valid result.

Device node
===========

The driver registers a misc device::

    /dev/socfpga-fcs

Opening it takes a reference on the driver state. If the driver is unbound
while the file is open, ``FCS_IOC_OPEN_SESSION`` and ``FCS_IOC_SDOS`` fail
with ``ENODEV`` rather than tearing the file descriptor out from under the
caller. ``FCS_IOC_CLOSE_SESSION`` does not return ``ENODEV``.

Buffer sizes
============

An SDOS object consists of a 40-byte firmware-defined header, the payload, and
for an encrypted object a trailing 48-byte HMAC. The payload is between 32 and
32672 bytes.

==================  ==================  ==============
``op_mode``         ``src_size`` range  Maximum output
==================  ==================  ==============
``1`` (encrypt)     72 .. 32712 bytes   32760 bytes
``0`` (decrypt)     120 .. 32760 bytes   32712 bytes
==================  ==================  ==============

Size the output buffer for the maximum shown above. On success the kernel
writes the exact produced length to ``dst_size``. On failure ``dst_size`` is
left unchanged.

Data structures
===============

.. kernel-doc:: include/uapi/misc/socfpga-fcs-crypto.h
   :identifiers: fcs_ioc_sdos

Driver ioctls
=============

FCS_IOC_OPEN_SESSION
---------------------

.. c:macro:: FCS_IOC_OPEN_SESSION

.. kernel-doc:: include/uapi/misc/socfpga-fcs-crypto.h
   :doc: FCS_IOC_OPEN_SESSION

FCS_IOC_CLOSE_SESSION
----------------------

.. c:macro:: FCS_IOC_CLOSE_SESSION

.. kernel-doc:: include/uapi/misc/socfpga-fcs-crypto.h
   :doc: FCS_IOC_CLOSE_SESSION

FCS_IOC_SDOS
-------------

.. c:macro:: FCS_IOC_SDOS

.. kernel-doc:: include/uapi/misc/socfpga-fcs-crypto.h
   :doc: FCS_IOC_SDOS

Return value
============

On success 0 is returned and, for ``FCS_IOC_SDOS``, ``dst_size`` holds the
number of bytes the SDM produced. On error -1 is returned, ``errno`` is set,
and ``dst_size`` is left unchanged.

``src`` and ``dst`` are input addresses. The kernel writes back only
``dst_size`` and ``error_code``. Set ``error_code`` to 0 before the call, and
set ``flags`` and ``reserved`` to 0. The kernel overwrites ``error_code`` when
the request reached the SDM, including when the SDM reported a failure, and
leaves it unchanged otherwise.

=========== ================================================================
Error       Description
=========== ================================================================
EINVAL      ``dst`` was NULL, or ``src_size`` was out of range for
            ``op_mode``, or ``op_mode`` was other than ``0`` or ``1``,
            or ``flags`` or ``reserved`` was not zero.
EMSGSIZE    The output did not fit in ``dst``. The operation itself
            succeeded and ``error_code`` holds the SDM status, but the
            result is discarded and ``dst_size`` is left unchanged.
            Size ``dst`` for the maximum output.
EFAULT      One of the supplied addresses was not accessible.
EIO         The SDM reported a fatal error after the request reached it.
            For ``FCS_IOC_SDOS``, ``error_code`` holds the status; a
            transport failure that never reaches the SDM leaves it
            unchanged instead.
ETIMEDOUT   The SDM did not complete the request within the service
            request timeout. For ``FCS_IOC_SDOS``, ``error_code`` is
            left unchanged because no SDM status was reported.
ENODEV      The device was unbound while the file descriptor was open.
            Returned by ``FCS_IOC_OPEN_SESSION`` and ``FCS_IOC_SDOS``;
            ``FCS_IOC_CLOSE_SESSION`` is unaffected.
ENOMEM      Staging buffers could not be allocated.
ENOTCONN    ``FCS_IOC_SDOS`` was issued with no crypto session open. Open
            one first with ``FCS_IOC_OPEN_SESSION``.
EBUSY       ``FCS_IOC_OPEN_SESSION`` was issued while a session was already
            open. Retry once that session is closed.
EPERM       ``FCS_IOC_SDOS`` or ``FCS_IOC_CLOSE_SESSION`` was issued on a
            session owned by another file.
EINTR       A signal arrived while waiting for another request to finish.
            Only one SDM transaction is in flight at a time.
ENOTTY      ``cmd`` was not one of the ioctls documented here.
=========== ================================================================

SDM status values
=================

The ``error_code`` member carries the firmware status. Set it to 0 before
the call. The kernel writes it when the request reached the SDM and leaves
it unchanged otherwise. Two decrypt results are successes that carry a
warning: the ioctl returns 0 for both, ``dst_size`` is the produced length,
and the plaintext is in ``dst``.

======= ================================================================
Status  Description
======= ================================================================
0x000   The operation completed successfully.
0x102   Decryption succeeded; reprovisioning the key is recommended.
0x103   Decryption succeeded, but not with the latest key.
======= ================================================================

Any other non-zero status is fatal and the ioctl fails with ``EIO``.

Userspace usage
===============

A sample userspace application demonstrating these ioctls can be found at
`Git Repository - altera-fpga/libfcs <https://github.com/altera-fpga/libfcs>`_

Decrypting an object:

.. code-block:: c

    #include <fcntl.h>
    #include <stdint.h>
    #include <stdio.h>
    #include <unistd.h>
    #include <sys/ioctl.h>
    #include <misc/socfpga-fcs-crypto.h>

    int decrypt_object(const void *in, __u32 in_len)
    {
            __u8 out[32712];                /* maximum decrypt output */
            int ret = 0;
            int fd;

            struct fcs_ioc_sdos arg = {
                    .src        = (__u64)(uintptr_t)in,
                    .src_size   = in_len,
                    .dst        = (__u64)(uintptr_t)out,
                    .dst_size   = sizeof(out),
                    .op_mode    = 0,        /* decrypt */
                    .error_code = 0,        /* set to 0; kernel writes the SDM status */
                    .flags      = 0,
                    .reserved   = 0,
            };

            fd = open("/dev/socfpga-fcs", O_RDWR);
            if (fd < 0) {
                    fprintf(stderr, "open failed: %m\n");
                    return -1;
            }

            if (ioctl(fd, FCS_IOC_OPEN_SESSION, NULL) != 0) {
                    fprintf(stderr, "open session failed: %m\n");
                    ret = -1;
                    goto done;
            }

            if (ioctl(fd, FCS_IOC_SDOS, &arg) == 0)
                    /* arg.dst_size bytes of plaintext in out */
                    write(STDOUT_FILENO, out, arg.dst_size);
            else {
                    fprintf(stderr, "SDOS failed: %m, SDM status 0x%x\n",
                            arg.error_code);
                    ret = -1;
            }

            ioctl(fd, FCS_IOC_CLOSE_SESSION, NULL);

    done:
            close(fd);
            return ret;
    }
