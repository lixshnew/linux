#ifndef __LINUX_PLATFORM_DATA_LS_NAND_H
#define __LINUX_PLATFORM_DATA_LS_NAND_H

#define LS2K_VER3 3

struct ls2k_nand_plat_data {
	int	enable_arbiter;
	u32	nr_parts;
	u32	chip_ver;
	struct mtd_partition *parts;
	int cs;
	u32 csrdy;
	int chip_cap;
};

#endif /* __LINUX_PLATFORM_DATA_LS_NAND_H */
