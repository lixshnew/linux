/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2020 Loongson Technology Corporation Limited
 */
#ifndef _ASM_IRQ_H
#define _ASM_IRQ_H

#include <linux/irqdomain.h>
#include <irq.h>
#include <asm-generic/irq.h>

#define IRQ_STACK_SIZE			THREAD_SIZE
#define IRQ_STACK_START			(IRQ_STACK_SIZE - 16)

DECLARE_PER_CPU(unsigned long, irq_stack);

/*
 * The highest address on the IRQ stack contains a dummy frame put down in
 * genex.S (except_vec_vi_handler) which is structured as follows:
 *
 *   top ------------
 *       | task sp  | <- irq_stack[cpu] + IRQ_STACK_START
 *       ------------
 *       |          | <- First frame of IRQ context
 *       ------------
 *
 * task sp holds a copy of the task stack pointer where the struct pt_regs
 * from exception entry can be found.
 */

static inline bool on_irq_stack(int cpu, unsigned long sp)
{
	unsigned long low = per_cpu(irq_stack, cpu);
	unsigned long high = low + IRQ_STACK_SIZE;

	return (low <= sp && sp <= high);
}

struct irq_data;
struct device_node;

int get_ipi_irq(void);
int get_pmc_irq(void);
int get_timer_irq(void);
void arch_init_irq(void);
void spurious_interrupt(void);
struct irq_domain *loongarch_cpu_irq_init(void);

extern void do_IRQ(unsigned int irq);

#define NR_IRQS_LEGACY 16

bool arch_trigger_cpumask_backtrace(const struct cpumask *mask,
					bool exclude_self);
#define arch_trigger_cpumask_backtrace arch_trigger_cpumask_backtrace

#endif /* _ASM_IRQ_H */
