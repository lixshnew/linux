/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2020-2021 Loongson Technology Corporation Limited
 */
#include <linux/init.h>

#include <asm/asm.h>
#include <asm/asmmacro.h>
#include <asm/cacheops.h>
#include <asm/irqflags.h>
#include <asm/regdef.h>
#include <asm/fpregdef.h>
#include <asm/loongarchregs.h>
#include <asm/stackframe.h>
#include <asm/thread_info.h>
#include <asm/unwind_hints.h>

	.align	5
SYM_FUNC_START(__arch_cpu_idle)
	/* start of rollback region */
	LONG_L	t0, tp, TI_FLAGS
	nop
	andi	t0, t0, _TIF_NEED_RESCHED
	bnez	t0, 1f
	nop
	nop
	nop
	idle	0
	/* end of rollback region */
1:	jirl	zero, ra, 0
SYM_FUNC_END(__arch_cpu_idle)

SYM_FUNC_START(handle_vint)
	BACKUP_T0T1
	SAVE_ALL
	UNWIND_HINT_REGS
	la.abs	t1, __arch_cpu_idle
	LONG_L  t0, sp, PT_ERA
	/* 32 byte rollback region */
	ori	t0, t0, 0x1f
	xori	t0, t0, 0x1f
	bne	t0, t1, 1f
	LONG_S  t0, sp, PT_ERA
1:	move	a0, sp
	move	a1, sp
	la.abs	t0, do_vint
	jirl    ra, t0, 0
	UNWIND_HINT_REGS
	RESTORE_ALL_AND_RET
SYM_FUNC_END(handle_vint)

SYM_FUNC_START(except_vec_cex)
	b	cache_parity_error
	nop
SYM_FUNC_END(except_vec_cex)

	.macro  __build_clear_none
	.endm

	.macro  __build_clear_sti
	TRACE_IRQS_ON
	STI
	.endm

	.macro  __build_clear_cli
	CLI
	TRACE_IRQS_OFF
	.endm

	.macro  __build_clear_fpe
	movfcsr2gr      a1, fcsr0
	CLI
	TRACE_IRQS_OFF
	.endm

	.macro  __build_clear_csti
	csrrd   t0, LOONGARCH_CSR_CRMD
	PTR_S   t0, sp, PT_CRMD
	csrrd   t0, LOONGARCH_CSR_EUEN
	PTR_S   t0, sp, PT_EUEN
	csrrd   t0, LOONGARCH_CSR_ECFG
	PTR_S   t0, sp, PT_ECFG
	csrrd   t0, LOONGARCH_CSR_ESTAT
	PTR_S   t0, sp, PT_ESTAT
	csrrd   t0, LOONGARCH_CSR_BADV
	PTR_S   t0, sp, PT_BVADDR
	STI
	.endm

	.macro  __build_clear_kmode
	csrrd   t0, LOONGARCH_CSR_CRMD
	PTR_S   t0, sp, PT_CRMD
	csrrd   t0, LOONGARCH_CSR_EUEN
	PTR_S   t0, sp, PT_EUEN
	csrrd   t0, LOONGARCH_CSR_ECFG
	PTR_S   t0, sp, PT_ECFG
	csrrd   t0, LOONGARCH_CSR_ESTAT
	PTR_S   t0, sp, PT_ESTAT
	csrrd   t0, LOONGARCH_CSR_BADV
	PTR_S   t0, sp, PT_BVADDR
	KMODE
	.endm

	.macro  __BUILD_HANDLER exception handler clear ext
	.align  5
	SYM_FUNC_START(handle_\exception)
	csrwr   t0, LOONGARCH_CSR_KS0
	csrwr   t1, LOONGARCH_CSR_KS1
	SAVE_ALL
	SYM_INNER_LABEL(handle_\exception\ext, SYM_L_GLOBAL)
	__build_clear_\clear
	move    a0, sp
	la.abs  t0, do_\handler
	jirl    ra, t0, 0
	la.abs  t0, ret_from_exception
	jirl    zero, t0, 0
	SYM_FUNC_END(handle_\exception)
	.endm

	.macro  BUILD_HANDLER exception handler clear verbose
	__BUILD_HANDLER \exception \handler \clear \verbose _int
	.endm

	BUILD_HANDLER ade ade kmode
	BUILD_HANDLER bp bp kmode
	BUILD_HANDLER ri ri csti
	BUILD_HANDLER fpu fpu sti
	BUILD_HANDLER fpe fpe fpe
	BUILD_HANDLER lsx lsx sti
	BUILD_HANDLER lasx lasx sti
	BUILD_HANDLER lbt lbt sti
	BUILD_HANDLER ale ale kmode
	BUILD_HANDLER reserved reserved csti    /* others */
	BUILD_HANDLER watch watch none

SYM_FUNC_START(handle_sys)
	la.abs	t0, handle_syscall
	jirl    zero, t0, 0
SYM_FUNC_END(handle_sys)
