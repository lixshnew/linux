/* SPDX-License-Identifier: GPL-2.0 */
/*
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * Copyright (C) 2020 Loongson Technology Corporation Limited
 */

#include <asm/asm.h>
#include <asm/asmmacro.h>
#include <asm/compiler.h>
#include <asm/irqflags.h>
#include <asm/regdef.h>
#include <asm/loongarchregs.h>
#include <asm/stackframe.h>
#include <asm/thread_info.h>
#include <asm/unwind_hints.h>

#ifndef CONFIG_PREEMPT
#define resume_kernel   restore_all
#else
#define __ret_from_irq  ret_from_exception
#endif
	.text
	.cfi_sections	.debug_frame
	.align	5

#ifndef CONFIG_PREEMPT
SYM_CODE_START(ret_from_exception)
	UNWIND_HINT_REGS
	local_irq_disable                       # preempt stop
	b       __ret_from_irq
SYM_CODE_END(ret_from_exception)
#endif
SYM_CODE_START(ret_from_irq)
	UNWIND_HINT_REGS
	LONG_S  s0, tp, TI_REGS
	b       __ret_from_irq
SYM_CODE_END(ret_from_irq)

SYM_CODE_START(__ret_from_irq)
	UNWIND_HINT_REGS
/*
 * We can be coming here from a syscall done in the kernel space,
 * e.g. a failed kernel_execve().
 */
resume_userspace_check:
	LONG_L  t0, sp, PT_PRMD # returning to kernel mode?
	andi    t0, t0, PLV_MASK
	beqz    t0, resume_kernel

resume_userspace:
	local_irq_disable               # make sure we dont miss an
					# interrupt setting need_resched
					# between sampling and return
	LONG_L  a2, tp, TI_FLAGS        # current->work
	andi    t0, a2, _TIF_WORK_MASK  # (ignoring syscall_trace)
	bnez    t0, work_pending
	b       restore_all
SYM_CODE_END(__ret_from_irq)

#ifdef CONFIG_PREEMPT
resume_kernel:
	local_irq_disable
	ld.w    t0, tp, TI_PRE_COUNT
	bnez    t0, restore_all
need_resched:
	LONG_L  t0, tp, TI_FLAGS
	andi    t1, t0, _TIF_NEED_RESCHED
	beqz    t1, restore_all

	LONG_L  t0, sp, PT_PRMD         # Interrupts off?
	andi    t0, t0, CSR_CRMD_IE
	beqz    t0, restore_all
	bl      preempt_schedule_irq
	b       need_resched
#endif

SYM_FUNC_START(handle_syscall)
	csrrd	t0, PERCPU_BASE_KS
	la.abs	t1, kernelsp
	add.d	t1, t1, t0
	move	t2, sp
	ld.d	sp, t1, 0

	addi.d	sp, sp, -PT_SIZE
	cfi_st	t2, PT_R3
	cfi_rel_offset  sp, PT_R3
	st.d	zero, sp, PT_R0
	csrrd	t2, LOONGARCH_CSR_PRMD
	st.d	t2, sp, PT_PRMD
	csrrd	t2, LOONGARCH_CSR_CRMD
	st.d	t2, sp, PT_CRMD
	csrrd	t2, LOONGARCH_CSR_EUEN
	st.d	t2, sp, PT_EUEN
	csrrd	t2, LOONGARCH_CSR_ECFG
	st.d	t2, sp, PT_ECFG
	csrrd	t2, LOONGARCH_CSR_ESTAT
	st.d	t2, sp, PT_ESTAT
	cfi_st	ra, PT_R1
	cfi_st	a0, PT_R4
	cfi_st	a1, PT_R5
	cfi_st	a2, PT_R6
	cfi_st	a3, PT_R7
	cfi_st	a4, PT_R8
	cfi_st	a5, PT_R9
	cfi_st	a6, PT_R10
	cfi_st	a7, PT_R11
	csrrd	ra, LOONGARCH_CSR_ERA
	st.d	ra, sp, PT_ERA
	cfi_rel_offset ra, PT_ERA

	cfi_st	tp, PT_R2
	cfi_st	u0, PT_R21
	cfi_st	fp, PT_R22

	SAVE_STATIC

#ifdef CONFIG_KGDB
	li.w	t1, CSR_CRMD_WE
	csrxchg	t1, t1, LOONGARCH_CSR_CRMD
#endif
	UNWIND_HINT_REGS

	move	u0, t0
	li.d	tp, ~_THREAD_MASK
	and	tp, tp, sp

	move	a0, sp
	bl	do_syscall

	RESTORE_ALL_AND_RET
SYM_FUNC_END(handle_syscall)

SYM_CODE_START(ret_from_fork)
	UNWIND_HINT_REGS
	bl	schedule_tail		# a0 = struct task_struct *prev
	move	a0, sp
	bl 	syscall_exit_to_user_mode
	RESTORE_STATIC
	RESTORE_SOME
	RESTORE_SP_AND_RET
SYM_CODE_END(ret_from_fork)

SYM_CODE_START(ret_from_kernel_thread)
	UNWIND_HINT_REGS
	bl	schedule_tail		# a0 = struct task_struct *prev
	move	a0, s1
	jirl	ra, s0, 0
	move	a0, sp
	bl	syscall_exit_to_user_mode
	RESTORE_STATIC
	RESTORE_SOME
	RESTORE_SP_AND_RET
SYM_CODE_END(ret_from_kernel_thread)

SYM_CODE_START(syscall_exit)
	UNWIND_HINT_REGS
#ifdef CONFIG_DEBUG_RSEQ
	move    a0, sp
	bl      rseq_syscall
#endif
	local_irq_disable               # make sure need_resched and
					# signals dont change between
					# sampling and return
	LONG_L  a2, tp, TI_FLAGS        # current->work
	li.w    t0, _TIF_ALLWORK_MASK
	and     t0, a2, t0
	bnez    t0, syscall_exit_work

restore_all:                            # restore full frame
	RESTORE_TEMP
	RESTORE_STATIC
restore_partial:                # restore partial frame
#ifdef CONFIG_TRACE_IRQFLAGS
	SAVE_STATIC
	SAVE_TEMP

	LONG_L  v0, sp, PT_PRMD
	andi    v0, v0, CSR_PRMD_PIE
	beqz    v0, 1f

	bl      trace_hardirqs_on
	b       2f
1:      bl      trace_hardirqs_off
2:
	RESTORE_TEMP
	RESTORE_STATIC
#endif
	RESTORE_SOME
	RESTORE_SP_AND_RET

work_pending:
	andi    t0, a2, _TIF_NEED_RESCHED # a2 is preloaded with TI_FLAGS
	beqz    t0, work_notifysig
work_resched:
	TRACE_IRQS_OFF
	bl      schedule

	local_irq_disable               # make sure need_resched and
					# signals dont change between
					# sampling and return
	LONG_L  a2, tp, TI_FLAGS
	andi    t0, a2, _TIF_WORK_MASK  # is there any work to be done
					# other than syscall tracing?
	beqz    t0, restore_all
	andi    t0, a2, _TIF_NEED_RESCHED
	bnez    t0, work_resched

work_notifysig:                         # deal with pending signals and
					# notify-resume requests
	move    a0, sp
	li.w    a1, 0
	bl      do_notify_resume        # a2 already loaded
	b       resume_userspace_check
SYM_CODE_END(syscall_exit)

SYM_CODE_START(syscall_exit_partial)
	UNWIND_HINT_REGS
#ifdef CONFIG_DEBUG_RSEQ
	move    a0, sp
	bl      rseq_syscall
#endif
	local_irq_disable               # make sure need_resched doesn't
					# change between and return
	LONG_L  a2, tp, TI_FLAGS        # current->work
	li.w    t0, _TIF_ALLWORK_MASK
	and     t0, t0, a2
	beqz    t0, restore_partial
	SAVE_STATIC
syscall_exit_work:
	LONG_L  t0, sp, PT_PRMD                 # returning to kernel mode?
	andi    t0, t0, PLV_MASK
	beqz    t0, resume_kernel
	li.w    t0, _TIF_WORK_SYSCALL_EXIT
	and     t0, t0, a2                      # a2 is preloaded with TI_FLAGS
	beqz    t0, work_pending        # trace bit set?
	TRACE_IRQS_ON
	local_irq_enable                # could let syscall_trace_leave()
					# call schedule() instead
	move    a0, sp
	bl      syscall_trace_leave
	b       resume_userspace
SYM_CODE_END(syscall_exit_partial)
