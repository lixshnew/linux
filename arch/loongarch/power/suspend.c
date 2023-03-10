// SPDX-License-Identifier: GPL-2.0
/*
 * loongson-specific suspend support
 *
 * Author: Huacai Chen <chenhuacai@loongson.cn>
 * Copyright (C) 2020 Loongson Technology Co., Ltd.
 */
#include <linux/acpi.h>
#include <linux/pm.h>
#include <linux/suspend.h>

#include <asm/loongarchregs.h>
#include <asm/setup.h>
#include <asm/time.h>
#include <asm/tlbflush.h>

#include <loongson.h>

u32 loongarch_nr_nodes;
u64 loongarch_suspend_addr;
u32 loongarch_pcache_ways;
u32 loongarch_scache_ways;
u32 loongarch_pcache_sets;
u32 loongarch_scache_sets;
u32 loongarch_pcache_linesz;
u32 loongarch_scache_linesz;

struct saved_registers {
	u32 ecfg;
	u32 euen;
	u64 pgd;
	u64 kpgd;
	u32 pwctl0;
	u32 pwctl1;
};
static struct saved_registers saved_regs;

static void arch_common_suspend(void)
{
	save_counter();
	saved_regs.pgd = csr_readq(LOONGARCH_CSR_PGDL);
	saved_regs.kpgd = csr_readq(LOONGARCH_CSR_PGDH);
	saved_regs.pwctl0 = csr_readl(LOONGARCH_CSR_PWCTL0);
	saved_regs.pwctl1 = csr_readl(LOONGARCH_CSR_PWCTL1);
	saved_regs.ecfg = csr_readl(LOONGARCH_CSR_ECFG);
	saved_regs.euen = csr_readl(LOONGARCH_CSR_EUEN);

	loongarch_nr_nodes = loongson_sysconf.nr_nodes;
	loongarch_suspend_addr = loongson_sysconf.suspend_addr;
	loongarch_pcache_ways = cpu_data[0].dcache.ways;
	loongarch_scache_ways = cpu_data[0].scache.ways;
	loongarch_pcache_sets = cpu_data[0].dcache.sets;
	loongarch_scache_sets = cpu_data[0].scache.sets;
	loongarch_pcache_linesz = cpu_data[0].dcache.linesz;
	loongarch_scache_linesz = cpu_data[0].scache.linesz;
}

static void arch_common_resume(void)
{
	sync_counter();
	local_flush_tlb_all();
#ifdef CONFIG_SMP
	csr_writeq(per_cpu_offset(0), PERCPU_BASE_KS);
#else
	csr_writeq(0, PERCPU_BASE_KS);
#endif
	csr_writeq(eentry, LOONGARCH_CSR_EENTRY);
	csr_writeq(eentry, LOONGARCH_CSR_MERRENTRY);
	csr_writeq(tlbrentry, LOONGARCH_CSR_TLBRENTRY);

	csr_writeq(saved_regs.pgd, LOONGARCH_CSR_PGDL);
	csr_writeq(saved_regs.kpgd, LOONGARCH_CSR_PGDH);
	csr_writel(saved_regs.pwctl0, LOONGARCH_CSR_PWCTL0);
	csr_writel(saved_regs.pwctl1, LOONGARCH_CSR_PWCTL1);
	csr_writel(saved_regs.ecfg, LOONGARCH_CSR_ECFG);
	csr_writel(saved_regs.euen, LOONGARCH_CSR_EUEN);
}

#ifdef CONFIG_ACPI_SLEEP
int loongarch_acpi_suspend(void)
{
	arch_common_suspend();

	enable_gpe_wakeup();
	enable_pci_wakeup();

	/* processor specific suspend */
	loongarch_suspend_enter();

	arch_common_resume();

	return 0;
}
#endif
