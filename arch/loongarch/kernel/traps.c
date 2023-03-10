// SPDX-License-Identifier: GPL-2.0
/*
 * Author: Huacai Chen <chenhuacai@loongson.cn>
 * Copyright (C) 2020 Loongson Technology Corporation Limited
 */
#include <linux/bitops.h>
#include <linux/bug.h>
#include <linux/compiler.h>
#include <linux/context_tracking.h>
#include <linux/entry-common.h>
#include <linux/kexec.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/extable.h>
#include <linux/mm.h>
#include <linux/sched/mm.h>
#include <linux/sched/debug.h>
#include <linux/smp.h>
#include <linux/spinlock.h>
#include <linux/kallsyms.h>
#include <linux/memblock.h>
#include <linux/interrupt.h>
#include <linux/ptrace.h>
#include <linux/kgdb.h>
#include <linux/kdebug.h>
#include <linux/kprobes.h>
#include <linux/notifier.h>
#include <linux/kdb.h>
#include <linux/irq.h>
#include <linux/perf_event.h>

#include <asm/addrspace.h>
#include <asm/bootinfo.h>
#include <asm/branch.h>
#include <asm/break.h>
#include <asm/cpu.h>
#include <asm/fpu.h>
#include <asm/loongarchregs.h>
#include <asm/pgtable.h>
#include <asm/ptrace.h>
#include <asm/sections.h>
#include <asm/siginfo.h>
#include <asm/tlb.h>
#include <asm/watch.h>
#include <asm/mmu_context.h>
#include <asm/types.h>
#include <asm/stacktrace.h>
#include <asm/unwind.h>

#include "access-helper.h"

extern asmlinkage void handle_ade(void);
extern asmlinkage void handle_ale(void);
extern asmlinkage void handle_sys(void);
extern asmlinkage void handle_bp(void);
extern asmlinkage void handle_ri(void);
extern asmlinkage void handle_fpu(void);
extern asmlinkage void handle_fpe(void);
extern asmlinkage void handle_lbt(void);
extern asmlinkage void handle_lsx(void);
extern asmlinkage void handle_lasx(void);
extern asmlinkage void handle_reserved(void);
extern asmlinkage void handle_watch(void);
extern asmlinkage void handle_vint(void);

void *exception_table[EXCCODE_INT_START] = {
	[0 ... EXCCODE_INT_START - 1] = handle_reserved,

	[EXCCODE_TLBI]		= handle_tlb_load,
	[EXCCODE_TLBL]		= handle_tlb_load,
	[EXCCODE_TLBS]		= handle_tlb_store,
	[EXCCODE_TLBM]		= handle_tlb_modify,
	[EXCCODE_TLBNR]		= handle_tlb_protect,
	[EXCCODE_TLBNX]		= handle_tlb_protect,
	[EXCCODE_TLBPE]		= handle_tlb_protect,
	[EXCCODE_ADE]		= handle_ade,
	[EXCCODE_ALE]		= handle_ale,
	[EXCCODE_SYS]		= handle_sys,
	[EXCCODE_BP]		= handle_bp,
	[EXCCODE_INE]		= handle_ri,
	[EXCCODE_IPE]		= handle_ri,
	[EXCCODE_FPDIS]		= handle_fpu,
	[EXCCODE_LSXDIS]	= handle_lsx,
	[EXCCODE_LASXDIS]	= handle_lasx,
	[EXCCODE_FPE]		= handle_fpe,
	[EXCCODE_BTDIS]		= handle_lbt,
	[EXCCODE_WATCH]		= handle_watch,
};
EXPORT_SYMBOL_GPL(exception_table);

static void show_backtrace(struct task_struct *task, const struct pt_regs *regs,
			   const char *loglvl, bool user)
{
	unsigned long pc;
	struct unwind_state state;
	struct pt_regs *pregs = (struct pt_regs *)regs;

	if (!task)
		task = current;

	unwind_start(&state, task, pregs);

#ifdef CONFIG_UNWINDER_PROLOGUE
	if (user_mode(regs))
		state.enable = false;
#endif

	printk("%sCall Trace:\n", loglvl);
	for (; !unwind_done(&state); unwind_next_frame(&state)) {
		pc = unwind_get_return_address(&state);
		print_ip_sym(loglvl, pc);
	}
	printk("%s\n", loglvl);
}

static void show_stacktrace(struct task_struct *task,
	const struct pt_regs *regs, const char *loglvl, bool user)
{
	int i;
	const int field = 2 * sizeof(unsigned long);
	unsigned long stackdata;
	unsigned long *sp = (unsigned long *)regs->regs[3];

	printk("%sStack :", loglvl);
	i = 0;
	while ((unsigned long) sp & (PAGE_SIZE - 1)) {
		if (i && ((i % (64 / field)) == 0)) {
			pr_cont("\n");
			printk("%s       ", loglvl);
		}
		if (i > 39) {
			pr_cont(" ...");
			break;
		}

		if (__get_addr(&stackdata, sp++, user)) {
			pr_cont(" (Bad stack address)");
			break;
		}

		pr_cont(" %0*lx", field, stackdata);
		i++;
	}
	pr_cont("\n");
	show_backtrace(task, regs, loglvl, user);
}

void show_stack(struct task_struct *task, unsigned long *sp, const char *loglvl)
{
	struct pt_regs regs;

	regs.csr_crmd = 0;
	if (sp) {
		regs.csr_era = 0;
		regs.regs[1] = 0;
		regs.regs[3] = (unsigned long)sp;
	} else {
		if (!task || task == current)
			prepare_frametrace(&regs);
		else {
			regs.csr_era = task->thread.reg01;
			regs.regs[1] = 0;
			regs.regs[3] = task->thread.reg03;
			regs.regs[22] = task->thread.reg22;
		}
	}

	show_stacktrace(task, &regs, loglvl, false);
}

static void show_code(void *pc, bool user)
{
	long i;
	unsigned int insn;

	printk("Code:");

	for (i = -3 ; i < 6 ; i++) {
		if (__get_inst(&insn, pc + i, user)) {
			pr_cont(" (Bad address in era)\n");
			break;
		}
		pr_cont("%c%08x%c", (i?' ':'<'), insn, (i?' ':'>'));
	}
	pr_cont("\n");
}

static void __show_regs(const struct pt_regs *regs)
{
	const int field = 2 * sizeof(unsigned long);
	unsigned int excsubcode;
	unsigned int exccode;
	int i;

	show_regs_print_info(KERN_DEFAULT);

	/*
	 * Saved main processor registers
	 */
	for (i = 0; i < 32; ) {
		if ((i % 4) == 0)
			printk("$%2d   :", i);
		pr_cont(" %0*lx", field, regs->regs[i]);

		i++;
		if ((i % 4) == 0)
			pr_cont("\n");
	}

	/*
	 * Saved csr registers
	 */
	printk("era   : %0*lx %pS\n", field, regs->csr_era,
	       (void *) regs->csr_era);
	printk("ra    : %0*lx %pS\n", field, regs->regs[1],
	       (void *) regs->regs[1]);

	printk("CSR crmd: %08lx	", regs->csr_crmd);
	printk("CSR prmd: %08lx	", regs->csr_prmd);
	printk("CSR euen: %08lx	", regs->csr_euen);
	printk("CSR ecfg: %08lx	", regs->csr_ecfg);
	printk("CSR estat: %08lx	", regs->csr_estat);

	pr_cont("\n");

	exccode = ((regs->csr_estat) & CSR_ESTAT_EXC) >> CSR_ESTAT_EXC_SHIFT;
	excsubcode = ((regs->csr_estat) & CSR_ESTAT_ESUBCODE) >> CSR_ESTAT_ESUBCODE_SHIFT;
	printk("ExcCode : %x (SubCode %x)\n", exccode, excsubcode);

	if (exccode >= EXCCODE_TLBL && exccode <= EXCCODE_ALE)
		printk("BadVA : %0*lx\n", field, regs->csr_badvaddr);

	printk("PrId  : %08x (%s)\n", read_cpucfg(LOONGARCH_CPUCFG0),
	       cpu_family_string());
}

void show_regs(struct pt_regs *regs)
{
	__show_regs((struct pt_regs *)regs);
	dump_stack();
}

void show_registers(struct pt_regs *regs)
{
	__show_regs(regs);
	print_modules();
	printk("Process %s (pid: %d, threadinfo=%p, task=%p)\n",
	       current->comm, current->pid, current_thread_info(), current);

	show_stacktrace(current, regs, KERN_DEFAULT, user_mode(regs));
	show_code((void *)regs->csr_era, user_mode(regs));
	printk("\n");
}

static DEFINE_RAW_SPINLOCK(die_lock);

void __noreturn die(const char *str, struct pt_regs *regs)
{
	static int die_counter;
	int sig = SIGSEGV;

	oops_enter();

	if (notify_die(DIE_OOPS, str, regs, 0, current->thread.trap_nr,
		       SIGSEGV) == NOTIFY_STOP)
		sig = 0;

	console_verbose();
	raw_spin_lock_irq(&die_lock);
	bust_spinlocks(1);

	printk("%s[#%d]:\n", str, ++die_counter);
	show_registers(regs);
	add_taint(TAINT_DIE, LOCKDEP_NOW_UNRELIABLE);
	raw_spin_unlock_irq(&die_lock);

	oops_exit();

	if (regs && kexec_should_crash(current))
		crash_kexec(regs);

	if (in_interrupt())
		panic("Fatal exception in interrupt");

	if (panic_on_oops)
		panic("Fatal exception");

	do_exit(sig);
}

static inline void setup_vint_size(unsigned int size)
{
	unsigned int vs;

	vs = ilog2(size/4);

	if (vs == 0 || vs > 7)
		panic("vint_size %d Not support yet", vs);

	csr_xchgl(vs<<CSR_ECFG_VS_SHIFT, CSR_ECFG_VS, LOONGARCH_CSR_ECFG);
}

/*
 * Send SIGFPE according to FCSR Cause bits, which must have already
 * been masked against Enable bits.  This is impotant as Inexact can
 * happen together with Overflow or Underflow, and `ptrace' can set
 * any bits.
 */
void force_fcsr_sig(unsigned long fcsr, void __user *fault_addr,
		     struct task_struct *tsk)
{
	int si_code = FPE_FLTUNK;

	if (fcsr & FPU_CSR_INV_X)
		si_code = FPE_FLTINV;
	else if (fcsr & FPU_CSR_DIV_X)
		si_code = FPE_FLTDIV;
	else if (fcsr & FPU_CSR_OVF_X)
		si_code = FPE_FLTOVF;
	else if (fcsr & FPU_CSR_UDF_X)
		si_code = FPE_FLTUND;
	else if (fcsr & FPU_CSR_INE_X)
		si_code = FPE_FLTRES;

	force_sig_fault(SIGFPE, si_code, fault_addr);
}

int process_fpemu_return(int sig, void __user *fault_addr, unsigned long fcsr)
{
	int si_code;
	struct vm_area_struct *vma;

	switch (sig) {
	case 0:
		return 0;

	case SIGFPE:
		force_fcsr_sig(fcsr, fault_addr, current);
		return 1;

	case SIGBUS:
		force_sig_fault(SIGBUS, BUS_ADRERR, fault_addr);
		return 1;

	case SIGSEGV:
		mmap_read_lock(current->mm);
		vma = find_vma(current->mm, (unsigned long)fault_addr);
		if (vma && (vma->vm_start <= (unsigned long)fault_addr))
			si_code = SEGV_ACCERR;
		else
			si_code = SEGV_MAPERR;
		mmap_read_unlock(current->mm);
		force_sig_fault(SIGSEGV, si_code, fault_addr);
		return 1;

	default:
		force_sig(sig);
		return 1;
	}
}

/*
 * Delayed fp exceptions when doing a lazy ctx switch
 */
asmlinkage void noinstr do_fpe(struct pt_regs *regs, unsigned long fcsr)
{
	int sig;
	void __user *fault_addr;
	irqentry_state_t state = irqentry_enter(regs);

	if (notify_die(DIE_FP, "FP exception", regs, 0, current->thread.trap_nr,
		       SIGFPE) == NOTIFY_STOP)
		goto out;

	/* Clear FCSR.Cause before enabling interrupts */
	write_fcsr(LOONGARCH_FCSR0, fcsr & ~mask_fcsr_x(fcsr));
	local_irq_enable();

	die_if_kernel("FP exception in kernel code", regs);

	sig = SIGFPE;
	fault_addr = (void __user *) regs->csr_era;

	/* Send a signal if required.  */
	process_fpemu_return(sig, fault_addr, fcsr);

out:
	local_irq_disable();
	irqentry_exit(regs, state);
}

asmlinkage void noinstr do_bp(struct pt_regs *regs)
{
	bool user = user_mode(regs);
	unsigned int opcode, bcode;
	unsigned long era = exception_era(regs);
	irqentry_state_t state = irqentry_enter(regs);

	local_irq_enable();
	current->thread.trap_nr = read_csr_excode();
	if (__get_inst(&opcode, (u32 *)era, user))
		goto out_sigsegv;

	bcode = (opcode & 0x7fff);

#ifdef CONFIG_KGDB_LOW_LEVEL_TRAP
	if (kgdb_ll_trap(DIE_TRAP, str, regs, code, current->thread.trap_nr,
			 SIGTRAP) == NOTIFY_STOP)
		return;
#endif /* CONFIG_KGDB_LOW_LEVEL_TRAP */

	/*
	 * notify the kprobe handlers, if instruction is likely to
	 * pertain to them.
	 */
	switch (bcode) {
	case BRK_KPROBE_BP:
		if (notify_die(DIE_BREAK, "Kprobe", regs, bcode,
			       current->thread.trap_nr, SIGTRAP) == NOTIFY_STOP)
			goto out;
		else
			break;
	case BRK_KPROBE_SSTEPBP:
		if (notify_die(DIE_SSTEPBP, "Kprobe_SingleStep", regs, bcode,
			       current->thread.trap_nr, SIGTRAP) == NOTIFY_STOP)
			goto out;
		else
			break;
	case BRK_UPROBE_BP:
		if (notify_die(DIE_UPROBE, "Uprobe", regs, bcode,
			       current->thread.trap_nr, SIGTRAP) == NOTIFY_STOP)
			goto out;
		else
			break;
	case BRK_UPROBE_XOLBP:
		if (notify_die(DIE_UPROBE_XOL, "Uprobe_XOL", regs, bcode,
			       current->thread.trap_nr, SIGTRAP) == NOTIFY_STOP)
			goto out;
		else
			break;
	default:
		if (notify_die(DIE_TRAP, "Break", regs, bcode,
			       current->thread.trap_nr, SIGTRAP) == NOTIFY_STOP)
			goto out;
		else
			break;
	}

	switch (bcode) {
	case BRK_BUG:
		die_if_kernel("Kernel bug detected", regs);
		force_sig(SIGTRAP);
		break;
	case BRK_DIVZERO:
		die_if_kernel("Break instruction in kernel code", regs);
		force_sig_fault(SIGFPE, FPE_INTDIV, (void __user *)regs->csr_era);
		break;
	case BRK_OVERFLOW:
		die_if_kernel("Break instruction in kernel code", regs);
		force_sig_fault(SIGFPE, FPE_INTOVF, (void __user *)regs->csr_era);
		break;
	default:
		die_if_kernel("Break instruction in kernel code", regs);
		force_sig_fault(SIGTRAP, TRAP_BRKPT, (void __user *)regs->csr_era);
		break;
	}

out:
	local_irq_disable();
	irqentry_exit(regs, state);
	return;

out_sigsegv:
	force_sig(SIGSEGV);
	goto out;
}

/* Set CSR.FWPS.skip and CSR.LLBCTL.KLO */
static inline void watch_reset_skip_and_llsc(void)
{
	unsigned long fwps;

	fwps = csr_readq(LOONGARCH_CSR_FWPS);
	csr_writeq(0x10000 | fwps, LOONGARCH_CSR_FWPS);
	csr_writel(0x4, LOONGARCH_CSR_LLBCTL);
}

asmlinkage void noinstr do_watch(struct pt_regs *regs)
{
	irqentry_state_t state = irqentry_enter(regs);

	if (test_tsk_thread_flag(current, TIF_LOAD_WATCH)) {
		if (current->thread.single_step) {
			int llbit = (csr_readq(LOONGARCH_CSR_LLBCTL) & 0x1);
			unsigned long ip = regs->csr_era;

			if (llbit) {
				watch_reset_skip_and_llsc();
			} else if (ip == current->thread.single_step) {
				csr_writeq(0x10000 | csr_readq(LOONGARCH_CSR_FWPS), LOONGARCH_CSR_FWPS);
			} else {
				loongarch_read_watch_registers(regs);
				force_sig(SIGTRAP);
			}
		} else {
			loongarch_read_watch_registers(regs);
			force_sig(SIGTRAP);
		}
	} else {
		if (notify_die(DIE_TRAP, "Break", regs, 0,
			       current->thread.trap_nr, SIGTRAP) != NOTIFY_STOP)
			loongarch_clear_watch_registers();
	}

	irqentry_exit(regs, state);
}

asmlinkage void noinstr do_ri(struct pt_regs *regs)
{
	int status = -1;
	unsigned int opcode = 0;
	unsigned int __user *era = (unsigned int __user *)exception_era(regs);
	unsigned long old_era = regs->csr_era;
	unsigned long old_ra = regs->regs[1];
	irqentry_state_t state = irqentry_enter(regs);

	local_irq_enable();
	current->thread.trap_nr = read_csr_excode();

	if (notify_die(DIE_RI, "RI Fault", regs, 0, current->thread.trap_nr,
		       SIGILL) == NOTIFY_STOP)
		goto out;

	die_if_kernel("Reserved instruction in kernel code", regs);

	if (unlikely(compute_return_era(regs) < 0))
		goto out;

	if (unlikely(get_user(opcode, era) < 0)) {
		status = SIGSEGV;
		current->thread.error_code = 1;
	}

	if (status < 0)
		status = SIGILL;

	if (unlikely(status > 0)) {
		regs->csr_era = old_era;		/* Undo skip-over.  */
		regs->regs[1] = old_ra;
		force_sig(status);
	}

out:
	local_irq_disable();
	irqentry_exit(regs, state);
}

static void init_restore_fp(void)
{
	if (!used_math()) {
		/* First time FP context user. */
		init_fpu();
	} else {
		/* This task has formerly used the FP context */
		if (!is_fpu_owner())
			own_fpu_inatomic(1);
	}

	BUG_ON(!is_fp_enabled());
}

static void init_restore_lsx(void)
{
	enable_lsx();

	if (!thread_lsx_context_live()) {
		/* First time LSX context user */
		init_restore_fp();
		init_lsx_upper();
		set_thread_flag(TIF_LSX_CTX_LIVE);
	} else {
		if (!is_simd_owner()) {
			if (is_fpu_owner()) {
				restore_lsx_upper(current);
			} else {
				__own_fpu();
				restore_lsx(current);
			}
		}
	}

	set_thread_flag(TIF_USEDSIMD);
	write_vcsr16(current->thread.fpu.vcsr);

	BUG_ON(!is_fp_enabled());
	BUG_ON(!is_lsx_enabled());
}

static void init_restore_lasx(void)
{
	enable_lasx();

	if (!thread_lasx_context_live()) {
		/* First time LASX context user */
		init_restore_lsx();
		init_lasx_upper();
		set_thread_flag(TIF_LASX_CTX_LIVE);
	} else {
		if (is_fpu_owner() || is_simd_owner()) {
			init_restore_lsx();
			restore_lasx_upper(current);
		} else {
			__own_fpu();
			enable_lsx();
			restore_lasx(current);
		}
	}

	set_thread_flag(TIF_USEDSIMD);

	BUG_ON(!is_fp_enabled());
	BUG_ON(!is_lsx_enabled());
	BUG_ON(!is_lasx_enabled());
}

asmlinkage void noinstr do_fpu(struct pt_regs *regs)
{
	irqentry_state_t state = irqentry_enter(regs);

	local_irq_enable();
	die_if_kernel("do_fpu invoked from kernel context!", regs);
	BUG_ON(is_lsx_enabled());
	BUG_ON(is_lasx_enabled());

	preempt_disable();
	init_restore_fp();
	preempt_enable();

	local_irq_disable();
	irqentry_exit(regs, state);
}

asmlinkage void noinstr do_lsx(struct pt_regs *regs)
{
	irqentry_state_t state = irqentry_enter(regs);

	local_irq_enable();
	if (!cpu_has_lsx) {
		force_sig(SIGILL);
		goto out;
	}

	die_if_kernel("do_lsx invoked from kernel context!", regs);
	BUG_ON(is_lasx_enabled());

	preempt_disable();
	init_restore_lsx();
	preempt_enable();

out:
	local_irq_disable();
	irqentry_exit(regs, state);
}

asmlinkage void noinstr do_lasx(struct pt_regs *regs)
{
	irqentry_state_t state = irqentry_enter(regs);

	local_irq_enable();
	if (!cpu_has_lasx) {
		force_sig(SIGILL);
		goto out;
	}

	die_if_kernel("do_lasx invoked from kernel context!", regs);

	preempt_disable();
	init_restore_lasx();
	preempt_enable();

out:
	local_irq_disable();
	irqentry_exit(regs, state);
}

asmlinkage void noinstr do_lbt(struct pt_regs *regs)
{
	irqentry_state_t state = irqentry_enter(regs);
	local_irq_enable();
	force_sig(SIGILL);
	local_irq_disable();
	irqentry_exit(regs, state);
}

asmlinkage void noinstr do_reserved(struct pt_regs *regs)
{
	irqentry_state_t state = irqentry_enter(regs);
	/*
	 * Game over - no way to handle this if it ever occurs.	 Most probably
	 * caused by a new unknown cpu type or after another deadly
	 * hard/software error.
	 */
	local_irq_enable();
	show_regs(regs);
	panic("Caught reserved exception %u - should not happen.", read_csr_excode());
	local_irq_disable();
	irqentry_exit(regs, state);
}

asmlinkage void cache_parity_error(void)
{
	const int field = 2 * sizeof(unsigned long);
	unsigned int reg_val;

	/* For the moment, report the problem and hang. */
	printk("Cache error exception:\n");
	printk("csr_merrera == %0*llx\n", field, csr_readq(LOONGARCH_CSR_MERRERA));
	reg_val = csr_readl(LOONGARCH_CSR_MERRCTL);
	printk("csr_merrctl == %08x\n", reg_val);

	printk("Decoded c0_cacheerr: %s cache fault in %s reference.\n",
	       reg_val & (1<<30) ? "secondary" : "primary",
	       reg_val & (1<<31) ? "data" : "insn");
	if (((current_cpu_data.processor_id & 0xff0000) == PRID_COMP_LOONGSON)) {
		pr_err("Error bits: %s%s%s%s%s%s%s%s\n",
			reg_val & (1<<29) ? "ED " : "",
			reg_val & (1<<28) ? "ET " : "",
			reg_val & (1<<27) ? "ES " : "",
			reg_val & (1<<26) ? "EE " : "",
			reg_val & (1<<25) ? "EB " : "",
			reg_val & (1<<24) ? "EI " : "",
			reg_val & (1<<23) ? "E1 " : "",
			reg_val & (1<<22) ? "E0 " : "");
	} else {
		pr_err("Error bits: %s%s%s%s%s%s%s\n",
			reg_val & (1<<29) ? "ED " : "",
			reg_val & (1<<28) ? "ET " : "",
			reg_val & (1<<26) ? "EE " : "",
			reg_val & (1<<25) ? "EB " : "",
			reg_val & (1<<24) ? "EI " : "",
			reg_val & (1<<23) ? "E1 " : "",
			reg_val & (1<<22) ? "E0 " : "");
	}
	printk("IDX: 0x%08x\n", reg_val & ((1<<22)-1));

	panic("Can't handle the cache error!");
}

asmlinkage void noinstr do_vint(struct pt_regs *regs, unsigned long sp)
{
	register int cpu;
	register unsigned long stack;
	irqentry_state_t state = irqentry_enter(regs);

	cpu = smp_processor_id();

	if (on_irq_stack(cpu, sp))
		handle_arch_irq(regs);
	else {
		stack = per_cpu(irq_stack, cpu) + IRQ_STACK_START;

		/* Save task's sp on IRQ stack for unwinding */
		*(unsigned long *)stack = sp;

		__asm__ __volatile__(
		"move	$s0, $sp		\n" /* Preserve sp */
		"move	$sp, %[stk]		\n" /* Switch stack */
		"move	$a0, %[regs]		\n"
		"la	$t0, handle_arch_irq	\n"
		"ld.d	$t1, $t0, 0		\n"
		"jirl	$ra, $t1, 0		\n"
		"move	$sp, $s0		\n" /* Restore sp */
		: /* No outputs */
		: [stk] "r" (stack), [regs] "r" (regs)
		: "$a0", "$a1", "$a2", "$a3", "$a4", "$a5", "$a6", "$a7", "$s0",
		  "$t0", "$t1", "$t2", "$t3", "$t4", "$t5", "$t6", "$t7", "$t8",
		  "memory");
	}

	irqentry_exit(regs, state);
}

extern void tlb_init(void);
extern void cache_error_setup(void);

unsigned long eentry;
EXPORT_SYMBOL_GPL(eentry);
unsigned long tlbrentry;
EXPORT_SYMBOL_GPL(tlbrentry);

long exception_handlers[VECSIZE * 128 / sizeof(long)] __aligned(SZ_64K);

static void configure_exception_vector(void)
{
	eentry    = (unsigned long)exception_handlers;
	tlbrentry = (unsigned long)exception_handlers + 80*VECSIZE;

	csr_writeq(eentry, LOONGARCH_CSR_EENTRY);
	csr_writeq(eentry, LOONGARCH_CSR_MERRENTRY);
	csr_writeq(tlbrentry, LOONGARCH_CSR_TLBRENTRY);
}

void per_cpu_trap_init(int cpu)
{
	unsigned int i;

	setup_vint_size(VECSIZE);

	configure_exception_vector();

	if (!cpu_data[cpu].asid_cache)
		cpu_data[cpu].asid_cache = asid_first_version(cpu);

	mmgrab(&init_mm);
	current->active_mm = &init_mm;
	BUG_ON(current->mm);
	enter_lazy_tlb(&init_mm, current);

	/* Initialise exception handlers */
	if (cpu == 0)
		for (i = 0; i < 64; i++)
			set_handler(i * VECSIZE, handle_reserved, VECSIZE);

	tlb_init();
	cpu_cache_init();
}

/* Install CPU exception handler */
void set_handler(unsigned long offset, void *addr, unsigned long size)
{
	memcpy((void *)(eentry + offset), addr, size);
	local_flush_icache_range(eentry + offset, eentry + offset + size);
}

static const char panic_null_cerr[] =
	"Trying to set NULL cache error exception handler\n";

/*
 * Install uncached CPU exception handler.
 * This is suitable only for the cache error exception which is the only
 * exception handler that is being run uncached.
 */
void set_merr_handler(unsigned long offset, void *addr, unsigned long size)
{
	unsigned long uncached_eentry = TO_UNCAC(__pa(eentry));

	if (!addr)
		panic(panic_null_cerr);

	memcpy((void *)(uncached_eentry + offset), addr, size);
}

void __init trap_init(void)
{
	long i;

	/* Set interrupt vector handler */
	for (i = EXCCODE_INT_START; i < EXCCODE_INT_END; i++)
		set_handler(i * VECSIZE, handle_vint, VECSIZE);

	for (i = EXCCODE_ADE; i <= EXCCODE_BTDIS; i++)
		set_handler(i * VECSIZE, exception_table[i], VECSIZE);

	cache_error_setup();

	local_flush_icache_range(eentry, eentry + 0x400);
}
