// SPDX-License-Identifier: GPL-2.0
/*
 * Author: Huacai Chen <chenhuacai@loongson.cn>
 * Copyright (C) 2020 Loongson Technology Corporation Limited
 */

#include <linux/acpi.h>
#include <linux/dmi.h>
#include <linux/efi.h>
#include <linux/memblock.h>
#include <asm/acpi.h>
#include <asm/bootinfo.h>
#include <asm/cacheflush.h>
#include <asm/efi.h>
#include <asm/fw.h>
#include <asm/smp.h>
#include <asm/time.h>
#include <linux/libfdt.h>
#include <linux/of_fdt.h>
#include <linux/of.h>
#include <linux/of_address.h>

#include <loongson.h>

#define SMBIOS_BIOSSIZE_OFFSET		0x09
#define SMBIOS_BIOSEXTERN_OFFSET 	0x13
#define SMBIOS_FREQLOW_OFFSET		0x16
#define SMBIOS_FREQHIGH_OFFSET		0x17
#define SMBIOS_FREQLOW_MASK		0xFF
#define SMBIOS_CORE_PACKAGE_OFFSET	0x23
#define LOONGSON_EFI_ENABLE     	(1 << 3)

struct loongson_board_info b_info;
static const char dmi_empty_string[] = "        ";

extern void *loongson_fdt_blob;
static bool delayed_panic;

static const char *dmi_string_parse(const struct dmi_header *dm, u8 s)
{
	const u8 *bp = ((u8 *) dm) + dm->length;

	if (s) {
		s--;
		while (s > 0 && *bp) {
			bp += strlen(bp) + 1;
			s--;
		}

		if (*bp != 0) {
			size_t len = strlen(bp)+1;
			size_t cmp_len = len > 8 ? 8 : len;

			if (!memcmp(bp, dmi_empty_string, cmp_len))
				return dmi_empty_string;

			return bp;
		}
	}

	return "";

}

static void __init parse_cpu_table(const struct dmi_header *dm)
{
	long freq_temp = 0;
	char *dmi_data = (char *)dm;

	freq_temp = ((*(dmi_data + SMBIOS_FREQHIGH_OFFSET) << 8) + \
			((*(dmi_data + SMBIOS_FREQLOW_OFFSET)) & SMBIOS_FREQLOW_MASK));
	cpu_clock_freq = freq_temp * 1000000;

	loongson_sysconf.cpuname = (void *)dmi_string_parse(dm, dmi_data[16]);
	loongson_sysconf.cores_per_package = *(dmi_data + SMBIOS_CORE_PACKAGE_OFFSET);

	pr_info("CpuClock = %llu\n", cpu_clock_freq);

}

static void __init parse_bios_table(const struct dmi_header *dm)
{
	int bios_extern;
	char *dmi_data = (char *)dm;

	bios_extern = *(dmi_data + SMBIOS_BIOSEXTERN_OFFSET);
	b_info.bios_size = *(dmi_data + SMBIOS_BIOSSIZE_OFFSET);

#ifdef CONFIG_EFI
	if (bios_extern & LOONGSON_EFI_ENABLE)
		set_bit(EFI_BOOT, &efi.flags);
	else
		clear_bit(EFI_BOOT, &efi.flags);
#endif
}

static void __init find_tokens(const struct dmi_header *dm, void *dummy)
{
	switch (dm->type) {
	case 0x0: /* Extern BIOS */
		parse_bios_table(dm);
		break;
	case 0x4: /* Calling interface */
		parse_cpu_table(dm);
		break;
	}
}
static void __init smbios_parse(void)
{
	b_info.bios_vendor = (void *)dmi_get_system_info(DMI_BIOS_VENDOR);
	b_info.bios_version = (void *)dmi_get_system_info(DMI_BIOS_VERSION);
	b_info.bios_release_date = (void *)dmi_get_system_info(DMI_BIOS_DATE);
	b_info.board_vendor = (void *)dmi_get_system_info(DMI_BOARD_VENDOR);
	b_info.board_name = (void *)dmi_get_system_info(DMI_BOARD_NAME);
	dmi_walk(find_tokens, NULL);
}

static int __init parse_dt_topology(void)
{
	struct device_node *cn, *map;
	int ret = 0;

	cn = of_find_node_by_path("/cpus");
	if (!cn) {
		return 0;
	}

	map = of_get_child_by_name(cn, "cpu-map");
	if (!map)
		goto out;

out:
	of_node_put(cn);
	return ret;
}

static bool __init acpi_tables_present(void)
{
#ifdef CONFIG_EFI
	return efi.acpi20 != EFI_INVALID_TABLE_ADDR ||
			efi.acpi != EFI_INVALID_TABLE_ADDR;
#else
	return false;
#endif
}

#define INVALID_HWID 0xFFFF
static u64 __init of_get_hwid(struct device_node *dn)
{
	const __be32 *cell = of_get_property(dn, "reg", NULL);

	if (!cell) {
		pr_err("%pOF: missing reg property\n", dn);
		return INVALID_HWID;
	}

	return of_read_number(cell, of_n_addr_cells(dn));
}

static void __init parse_dt_cpus(void)
{
	struct device_node *dn;
	int i;
	int nid = 0;
	int hwids[NR_CPUS];
	nodemask_t nodes_mask;
	int nr_nodes;

	loongson_sysconf.boot_cpu_id = read_csr_cpuid();

	nodes_clear(nodes_mask);
	for_each_node_by_type(dn, "cpu") {
		u64 hwid = of_get_hwid(dn);

		if (hwid >= INVALID_HWID)
			continue;

		for (i = 0; i < loongson_sysconf.nr_cpus; i++) {
			if (hwids[i] == hwid) {
				pr_err("%pOF: duplicate cpu reg properties in the DT\n", dn);
				continue;
			}
		}

		nid = of_node_to_nid(dn);
		if (nid != NUMA_NO_NODE)
			node_set(nid, nodes_mask);

		if (of_node_to_nid(dn) == 0)
			loongson_sysconf.cores_per_node++;

		if (loongson_sysconf.nr_cpus >= NR_CPUS)
			break;

		hwids[loongson_sysconf.nr_cpus] = hwid;

#ifdef CONFIG_SMP
		cpu_logical_map(loongson_sysconf.nr_cpus) = loongson_sysconf.nr_cpus;
#endif
		loongson_sysconf.nr_cpus++;
	}
#ifdef CONFIG_ACPI
	num_processors = loongson_sysconf.nr_cpus;
#endif
	nr_nodes = nodes_weight(nodes_mask);
	if (nr_nodes)
		loongson_sysconf.nr_nodes = nodes_weight(nodes_mask);
}

static char cpu_full_name_bak[] = "        -        ";
#ifdef CONFIG_LOONGSON_2K500
static char cpu_ls2k500_name[] = "loongson-2k500";
#endif

static inline int check_cpu_full_name_invaild(int cpu)
{
	if (!__cpu_full_name[cpu])
		return 1;
	else if (!strncmp(__cpu_full_name[cpu], cpu_full_name_bak, 17))
		return 1;
	else if (!strlen(__cpu_full_name[cpu]))
		return 1;
	else
		return 0;
}

static void *get_fdt(void)
{
	void *fdt = NULL;

	if (b_info.board_name) {
#ifdef CONFIG_LOONGSON_2K500
		int i;
		if (!strncmp(b_info.board_name, "LS2K500-HL-MB", 13))
			fdt = &__dtb_ls2k500_hl_mb_begin;
		else if (!strncmp(b_info.board_name, "LS2K500-MINI-DP", 15))
			fdt = &__dtb_ls2k500_mini_dp_begin;

		for (i = 0; i < NR_CPUS; ++i)
			if (check_cpu_full_name_invaild(i))
				__cpu_full_name[i] = cpu_ls2k500_name;
#endif
#ifdef CONFIG_LOONGSON_2K1000
		if (!strncmp(b_info.board_name, "LS2K1000-JL-MB", 14)) {
			if (!strncmp(b_info.board_name, "LS2K1000-JL-MB-MU", 17))
				fdt = &__dtb_ls2k1000_jl_mb_mu_begin;
			else
				fdt = &__dtb_ls2k1000_jl_mb_begin;
		} else if (!strncmp(b_info.board_name, "LS2K1000-DP", 11))
			fdt = &__dtb_ls2k1000_dp_begin;
#endif
	}

	if (!fdt && IS_ENABLED(CONFIG_BUILTIN_DTB)
			&& (&__dtb_start != &__dtb_end))
		fdt = &__dtb_start;

	if (!fdt || fdt_check_header(fdt) != 0)
		return NULL;

	return fdt;
}

static void __init fdt_setup(void)
{
	unsigned long fdt_addr;

	fdt_addr = (uintptr_t)get_fdt();
	if (fdt_addr)
		loongson_fdt_blob = (void *)fdt_addr;


	if (!loongson_fdt_blob) {
		/*
		 * Since both acpi tables and dtb are not found, we have to
		 * panic. Considering earlycon with parameters may exist in
		 * cmdline, delay the panic to let the uart show the details.
		 */
		delayed_panic = true;
		return;
	}

	//init dtb and read chosen->bootargs value to boot_command_line(copy not strcat)
	__dt_setup_arch(loongson_fdt_blob);
	early_init_fdt_reserve_self();
	early_init_fdt_scan_reserved_mem();

	device_tree_init();
	parse_dt_cpus();
	parse_dt_topology();
}


void __init early_init(void)
{
	fw_init_cmdline();
	fw_init_environ();
	early_memblock_init();
#ifdef CONFIG_EFI
	efi_init();
#endif
	dmi_setup();
	smbios_parse();
	pr_info("The BIOS Version: %s\n", b_info.bios_version);

	if (!acpi_tables_present()) {
		fdt_setup();
#ifdef CONFIG_ACPI
		disable_acpi();
#endif
	}
}

void __init platform_init(void)
{
	if (delayed_panic)
		panic("Both acpi tables and dtb are not found.");
#ifdef CONFIG_ACPI_TABLE_UPGRADE
	acpi_table_upgrade();
#endif
#ifdef CONFIG_ACPI
	acpi_gbl_use_default_register_widths = false;
	acpi_boot_table_init();
	acpi_boot_init();
#endif

#ifndef CONFIG_NUMA
	fw_init_memory();
#else
	fw_init_numa_memory();
#endif

#ifdef CONFIG_EFI
	efi_runtime_init();
#endif

	register_smp_ops(&loongson3_smp_ops);
}
