/*
 *  Copyright (c) 2003 ARM Limited
 *  Copyright (c) u-boot contributors
 *  Copyright (c) 2012 Pavel Machek <pavel@denx.de>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/linkage.h>
#include <linux/init.h>
#include <asm/memory.h>
#include <asm/assembler.h>

	.arch	armv7-a
	.arm

ENTRY(socfpga_secondary_trampoline)
ARM_BE8(setend be)
	ldr	pc, 1f
ENDPROC(socfpga_secondary_trampoline)
	.globl	socfpga_boot_fn
socfpga_boot_fn:
1:	.space	4
ENTRY(socfpga_secondary_trampoline_end)
