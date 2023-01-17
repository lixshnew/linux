/* SPDX-License-Identifier: GPL-2.0 */
/*
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file COPYING in the main directory of this archive
 * for more details.
 *
 * Copyright (C) 2020 Loongson Technology Co., Ltd.
 */
#ifndef _ASM_BOOTINFO_H
#define _ASM_BOOTINFO_H

#include <linux/types.h>
#include <asm/setup.h>

const char *get_system_type(void);

extern void early_memblock_init(void);
extern void detect_memory_region(phys_addr_t start, phys_addr_t sz_min,  phys_addr_t sz_max);

extern void early_init(void);
extern void platform_init(void);
extern void plat_swiotlb_setup(void);

/*
 * Initial kernel command line, usually setup by fw_init_cmdline()
 */
extern char arcs_cmdline[COMMAND_LINE_SIZE];

/*
 * Registers a0, a1, a2 and a3 as passed to the kernel entry by firmware
 */
extern unsigned long fw_arg0, fw_arg1, fw_arg2, fw_arg3;

#ifdef CONFIG_USE_OF
extern unsigned long fw_passed_dtb;
#endif

extern unsigned long initrd_start, initrd_end;

/*
 * Platform memory detection hook called by setup_arch
 */
extern void plat_mem_setup(void);

#endif /* _ASM_BOOTINFO_H */
