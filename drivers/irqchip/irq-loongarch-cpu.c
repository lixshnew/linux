// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020 Loongson Technologies, Inc.
 */

#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/irqdomain.h>

#include <asm/setup.h>
#include <asm/loongarchregs.h>

static struct irq_domain *irq_domain;

static void mask_loongarch_irq(struct irq_data *d)
{
	clear_csr_ecfg(ECFGF(d->hwirq));
}

static void unmask_loongarch_irq(struct irq_data *d)
{
	set_csr_ecfg(ECFGF(d->hwirq));
}

static struct irq_chip cpu_irq_controller = {
	.name		= "LoongArch",
	.irq_mask	= mask_loongarch_irq,
	.irq_unmask	= unmask_loongarch_irq,
};

static void handle_cpu_irq(struct pt_regs *regs)
{
	int hwirq;
	unsigned int estat = read_csr_estat() & CSR_ESTAT_IS;

	while ((hwirq = ffs(estat))) {
		estat &= ~BIT(hwirq - 1);
		handle_domain_irq(irq_domain, hwirq - 1, regs);
	}
}

int get_ipi_irq(void)
{
	return irq_create_mapping(irq_domain, EXCCODE_IPI - EXCCODE_INT_START);
}

int get_pmc_irq(void)
{
	return irq_create_mapping(irq_domain, EXCCODE_PMC - EXCCODE_INT_START);
}

int get_timer_irq(void)
{
	return irq_create_mapping(irq_domain, EXCCODE_TIMER - EXCCODE_INT_START);
}

static int loongarch_cpu_intc_map(struct irq_domain *d, unsigned int irq,
			     irq_hw_number_t hwirq)
{
	irq_set_chip_and_handler(irq, &cpu_irq_controller, handle_percpu_irq);

	return 0;
}

static const struct irq_domain_ops loongarch_cpu_intc_irq_domain_ops = {
	.map = loongarch_cpu_intc_map,
	.xlate = irq_domain_xlate_onecell,
};

static void __init __loongarch_cpu_irq_init(struct device_node *of_node)
{
	struct fwnode_handle *domain_handle;
	int irq_size, irq_base;
	int i;

	irq_size = EXCCODE_INT_NUM;
	irq_base = EXCCODE_INT_START;

	/* Mask interrupts. */
	clear_csr_ecfg(ECFG0_IM);
	clear_csr_estat(ESTATF_IP);

	if (of_node)
		domain_handle = of_node_to_fwnode(of_node);
	else
		domain_handle = irq_domain_alloc_named_fwnode("coreintc");

	irq_alloc_descs(-1, irq_base, irq_size, 0);
	for (i = irq_base; i < irq_base + irq_size; ++i) {
		irq_set_noprobe(i);
	}

	irq_domain = irq_domain_create_linear(domain_handle, irq_size,
					&loongarch_cpu_intc_irq_domain_ops, NULL);

	if (!irq_domain)
		panic("Failed to add irqdomain for LoongArch CPU");
	
	irq_domain_associate_many(irq_domain, irq_base, 0, irq_size);

	set_handle_irq(&handle_cpu_irq);
}

struct irq_domain * __init loongarch_cpu_irq_init(void)
{
	__loongarch_cpu_irq_init(NULL);
	return irq_domain;
}

int __init loongarch_cpu_irq_of_init(struct device_node *of_node,
				struct device_node *parent)
{
	__loongarch_cpu_irq_init(of_node);
	return 0;
}
IRQCHIP_DECLARE(cpu_intc, "loongson,cpu-interrupt-controller", loongarch_cpu_irq_of_init);