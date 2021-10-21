/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2011 Tobias Klauser <tklauser@distanz.ch>
 */

#ifndef _ASM_NIOS2_REGISTERS_H
#define _ASM_NIOS2_REGISTERS_H

#ifndef __ASSEMBLY__
#include <asm/cpuinfo.h>
#endif

/* control register numbers */
#define NIOS2_CTL_FSTATUS	0
#define NIOS2_CTL_ESTATUS	1
#define NIOS2_CTL_BSTATUS	2
#define NIOS2_CTL_IENABLE	3
#define NIOS2_CTL_IPENDING	4
#define NIOS2_CTL_CPUID	5
#define NIOS2_CTL_RSV1	6
#define NIOS2_CTL_EXCEPTION	7
#define NIOS2_CTL_PTEADDR	8
#define NIOS2_CTL_TLBACC	9
#define NIOS2_CTL_TLBMISC	10
#define NIOS2_CTL_RSV2	11
#define NIOS2_CTL_BADADDR	12
#define NIOS2_CTL_CONFIG	13
#define NIOS2_CTL_MPUBASE	14
#define NIOS2_CTL_MPUACC	15

/* access control registers using GCC builtins */
#define NIOS2_RDCTL(r)	__builtin_rdctl(r)
#define NIOS2_WRCTL(r, v)	__builtin_wrctl(r, v)

/* status register bits */
#define STATUS_PIE	(1 << 0)	/* processor interrupt enable */
#define STATUS_U	(1 << 1)	/* user mode */
#define STATUS_EH	(1 << 2)	/* Exception mode */

/* estatus register bits */
#define ESTATUS_EPIE	(1 << 0)	/* processor interrupt enable */
#define ESTATUS_EU	(1 << 1)	/* user mode */
#define ESTATUS_EH	(1 << 2)	/* Exception mode */

/* tlbmisc register bits */
#define TLBMISC_PID_SHIFT	4
#ifndef __ASSEMBLY__
#define TLBMISC_PID_MASK	((1UL << cpuinfo.tlb_pid_num_bits) - 1)
#endif
#define TLBMISC_WAY_MASK	0xf
#define TLBMISC_WAY_SHIFT	20

#define TLBMISC_PID	(TLBMISC_PID_MASK << TLBMISC_PID_SHIFT)	/* TLB PID */
#define TLBMISC_WE	(1 << 18)	/* TLB write enable */
#define TLBMISC_RD	(1 << 19)	/* TLB read */
#define TLBMISC_WAY	(TLBMISC_WAY_MASK << TLBMISC_WAY_SHIFT) /* TLB way */

#endif /* _ASM_NIOS2_REGISTERS_H */
