/* SPDX-License-Identifier: GPL-2.0+ WITH Linux-syscall-note */
/*
* Copyright (C) 2020 Loongson Technology Corporation Limited
*
* Author: Hanlu Li <lihanlu@loongson.cn>
*/
#ifndef _ASM_SIGCONTEXT_H
#define _ASM_SIGCONTEXT_H

#include <linux/types.h>
#include <linux/posix_types.h>

/* FP context was used */
#define USED_FP			(1 << 0)
#define ADRERR_RD		(1 << 30)
#define ADRERR_WR		(1 << 31)

#define FPU_REG_WIDTH		256
#define FPU_ALIGN		__attribute__((aligned(32)))

struct sigcontext {
	__u64	sc_pc;
	__u64	sc_regs[32];
	__u32	sc_flags;
	__u32	sc_fcsr;
	__u32	sc_vcsr;
	__u64	sc_fcc;
	/* For Binary Translation */
	__u64	sc_scr[4];
	union {
		__u32	val32[FPU_REG_WIDTH / 32];
		__u64	val64[FPU_REG_WIDTH / 64];
	} sc_fpregs[32] FPU_ALIGN;

	/* Reserved for future scalable vectors */
	__u8	sc_reserved[4096] __attribute__((__aligned__(16)));
};

#endif /* _ASM_SIGCONTEXT_H */
