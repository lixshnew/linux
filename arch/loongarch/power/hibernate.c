// SPDX-License-Identifier: GPL-2.0
#include <asm/fpu.h>
#include <asm/sections.h>
#include <asm/tlbflush.h>

#include <loongson.h>

static u64 saved_crmd;
static u64 saved_prmd;
static u64 saved_euen;
static u64 saved_ecfg;
struct pt_regs saved_regs;

void save_processor_state(void)
{
	saved_crmd = csr_readl(LOONGARCH_CSR_CRMD);
	saved_prmd = csr_readl(LOONGARCH_CSR_PRMD);
	saved_euen = csr_readl(LOONGARCH_CSR_EUEN);
	saved_ecfg = csr_readl(LOONGARCH_CSR_ECFG);

	if (is_fpu_owner())
		save_fp(current);
}

void restore_processor_state(void)
{
	csr_writel(saved_crmd, LOONGARCH_CSR_CRMD);
	csr_writel(saved_prmd, LOONGARCH_CSR_PRMD);
	csr_writel(saved_euen, LOONGARCH_CSR_EUEN);
	csr_writel(saved_ecfg, LOONGARCH_CSR_ECFG);

	if (is_fpu_owner())
		restore_fp(current);
}

int pfn_is_nosave(unsigned long pfn)
{
	unsigned long nosave_begin_pfn = PFN_DOWN(__pa(&__nosave_begin));
	unsigned long nosave_end_pfn = PFN_UP(__pa(&__nosave_end));

	return	(pfn >= nosave_begin_pfn) && (pfn < nosave_end_pfn);
}

extern int swsusp_asm_suspend(void);

int swsusp_arch_suspend(void)
{
	enable_pci_wakeup();
	return swsusp_asm_suspend();
}

extern int swsusp_asm_resume(void);

int swsusp_arch_resume(void)
{
	/* Avoid TLB mismatch during and after kernel resume */
	local_flush_tlb_all();
	return swsusp_asm_resume();
}
