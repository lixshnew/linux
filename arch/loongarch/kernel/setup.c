// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020 Loongson Technology Corporation Limited
 */
#include <linux/init.h>
#include <linux/ioport.h>
#include <linux/export.h>
#include <linux/screen_info.h>
#include <linux/memblock.h>
#include <linux/initrd.h>
#include <linux/root_dev.h>
#include <linux/console.h>
#include <linux/highmem.h>
#include <linux/pfn.h>
#include <linux/debugfs.h>
#include <linux/kexec.h>
#include <linux/sizes.h>
#include <linux/device.h>
#include <linux/dma-map-ops.h>
#include <linux/of_fdt.h>
#include <linux/of_reserved_mem.h>
#include <linux/crash_dump.h>
#include <linux/swiotlb.h>

#include <asm/addrspace.h>
#include <asm/alternative.h>
#include <asm/bootinfo.h>
#include <asm/bugs.h>
#include <asm/cache.h>
#include <asm/cpu.h>
#include <asm/debug.h>
#include <asm/dma.h>
#include <asm/pgalloc.h>
#include <asm/sections.h>
#include <asm/setup.h>
#include <asm/smp.h>
#include <asm/unwind.h>

#include <boot_param.h>

DEFINE_PER_CPU(unsigned long, kernelsp);
unsigned long fw_arg0, fw_arg1, fw_arg2, fw_arg3;
struct cpuinfo_loongarch cpu_data[NR_CPUS] __read_mostly;

EXPORT_SYMBOL(cpu_data);

#ifdef CONFIG_VT
struct screen_info screen_info;
#endif

#ifdef CONFIG_CRASH_DUMP
static phys_addr_t crashmem_start, crashmem_size;
#endif

/*
 * Setup information
 *
 * These are initialized so they are in the .data section
 */

char __initdata arcs_cmdline[COMMAND_LINE_SIZE];
static char __initdata command_line[COMMAND_LINE_SIZE];

#ifdef CONFIG_CMDLINE_BOOL
static const char builtin_cmdline[] __initconst = CONFIG_CMDLINE;
#else
static const char builtin_cmdline[] __initconst = "";
#endif

static int num_standard_resources;
static struct resource *standard_resources;

static struct resource code_resource = { .name = "Kernel code", };
static struct resource data_resource = { .name = "Kernel data", };
static struct resource bss_resource = { .name = "Kernel bss", };

unsigned long __kaslr_offset __ro_after_init;
EXPORT_SYMBOL(__kaslr_offset);

void __init check_bugs(void)
{
	alternative_instructions();
}

static void *detect_magic __initdata = detect_memory_region;

void __init detect_memory_region(phys_addr_t start, phys_addr_t sz_min, phys_addr_t sz_max)
{
	void *dm = &detect_magic;
	phys_addr_t size;

	for (size = sz_min; size < sz_max; size <<= 1) {
		if (!memcmp(dm, dm + size, sizeof(detect_magic)))
			break;
	}

	pr_debug("Memory: %lluMB of RAM detected at 0x%llx (min: %lluMB, max: %lluMB)\n",
		((unsigned long long) size) / SZ_1M,
		(unsigned long long) start,
		((unsigned long long) sz_min) / SZ_1M,
		((unsigned long long) sz_max) / SZ_1M);

	memblock_add(start, size);
}

/*
 * Manage initrd
 */
#ifdef CONFIG_BLK_DEV_INITRD

static unsigned long __init init_initrd(void)
{
	if (!phys_initrd_start || !phys_initrd_size)
		goto disable;

	initrd_start = (unsigned long)phys_to_virt(phys_initrd_start);
	initrd_end   = (unsigned long)phys_to_virt(phys_initrd_start + phys_initrd_size);

	if (!initrd_start || initrd_end <= initrd_start)
		goto disable;

	if (initrd_start & ~PAGE_MASK) {
		pr_err("initrd start must be page aligned\n");
		goto disable;
	}
	if (initrd_start < PAGE_OFFSET) {
		pr_err("initrd start < PAGE_OFFSET\n");
		goto disable;
	}

	ROOT_DEV = Root_RAM0;

	initrd_below_start_ok = 1;
	memblock_reserve(phys_initrd_start, phys_initrd_size);

	pr_info("Initial ramdisk at: 0x%lx (%lu bytes)\n",
		initrd_start, initrd_end - initrd_start);

	return 0;

disable:
	initrd_start = 0;
	initrd_end = 0;
	printk(KERN_CONT " - disabling initrd\n");

	return 0;
}

static int __init early_initrd(char *p)
{
	unsigned long start, size;
	char *endp;

	start = memparse(p, &endp);
	if (*endp == ',')
		size = memparse(endp + 1, NULL);

	if (start + size > PFN_PHYS(max_low_pfn)) {
		pr_err("Initrd physical address is out of memory!");
		return 0;
	}

	phys_initrd_start = start;
	phys_initrd_size = size;

	return 0;
}
early_param("initrd", early_initrd);

static int __init rd_start_early(char *p)
{
	unsigned long start = memparse(p, &p);

	phys_initrd_start = TO_PHYS(start);

	return 0;
}
early_param("rd_start", rd_start_early);

static int __init rd_size_early(char *p)
{
	unsigned long size;

	size = memparse(p, &p);
	phys_initrd_size = size;

	return 0;
}
early_param("rd_size", rd_size_early);

#else  /* !CONFIG_BLK_DEV_INITRD */

static unsigned long __init init_initrd(void)
{
	return 0;
}

#endif

static void __init dt_bootmem_init(void)
{
	phys_addr_t ramstart, ramend;
	unsigned long start, end;
	int i;

	ramstart = memblock_start_of_DRAM();
	ramend = memblock_end_of_DRAM();

	/*
	 * Sanity check any INITRD first. We don't take it into account
	 * for bootmem setup initially, rely on the end-of-kernel-code
	 * as our memory range starting point. Once bootmem is inited we
	 * will reserve the area used for the initrd.
	 */
	// it`s done in setup_arch() already.
	// No need to do it again at here.
	// init_initrd();

	/* Reserve memory occupied by kernel. */
	memblock_reserve(__pa_symbol(&_text),
			__pa_symbol(&_end) - __pa_symbol(&_text));

	/*
	 * Reserve any memory between the start of RAM and PHYS_OFFSET
	 */
	if (ramstart > PHYS_OFFSET)
		memblock_reserve(PHYS_OFFSET, ramstart - PHYS_OFFSET);

	if (PFN_UP(ramstart) > ARCH_PFN_OFFSET) {
		pr_info("Wasting %lu bytes for tracking %lu unused pages\n",
			(unsigned long)((PFN_UP(ramstart) - ARCH_PFN_OFFSET) * sizeof(struct page)),
			(unsigned long)(PFN_UP(ramstart) - ARCH_PFN_OFFSET));
	}

	min_low_pfn = ARCH_PFN_OFFSET;
	max_pfn = PFN_DOWN(ramend);
	for_each_mem_pfn_range(i, MAX_NUMNODES, &start, &end, NULL) {
		/*
		 * Skip highmem here so we get an accurate max_low_pfn if low
		 * memory stops short of high memory.
		 * If the region overlaps HIGHMEM_START, end is clipped so
		 * max_pfn excludes the highmem portion.
		 */
		if (start >= PFN_DOWN(HIGHMEM_START))
			continue;
		if (end > PFN_DOWN(HIGHMEM_START))
			end = PFN_DOWN(HIGHMEM_START);
		if (end > max_low_pfn)
			max_low_pfn = end;
	}

	if (min_low_pfn >= max_low_pfn)
		panic("Incorrect memory mapping !!!");

	if (max_pfn > PFN_DOWN(HIGHMEM_START)) {
#ifdef CONFIG_HIGHMEM
		highstart_pfn = PFN_DOWN(HIGHMEM_START);
		highend_pfn = max_pfn;
#else
		max_low_pfn = PFN_DOWN(HIGHMEM_START);
		max_pfn = max_low_pfn;
#endif
	}
}

static int usermem __initdata;

static int __init early_parse_mem(char *p)
{
	phys_addr_t start, size;

	/*
	 * If a user specifies memory size, we
	 * blow away any automatically generated
	 * size.
	 */
	if (usermem == 0) {
		usermem = 1;
		memblock_remove(memblock_start_of_DRAM(),
			memblock_end_of_DRAM() - memblock_start_of_DRAM());
	}
	start = 0;
	size = memparse(p, &p);

	memblock_add(start, size);

#ifdef CONFIG_CRASH_DUMP
	if (start && size) {
		crashmem_start = start;
		crashmem_size = size;
	}
#endif

	return 0;
}
early_param("mem", early_parse_mem);

static int __init early_parse_memmap(char *p)
{
	phys_addr_t start, size;

	/*
	 * If a user specifies memory size, we
	 * blow away any automatically generated
	 * size.
	 */
	if (usermem == 0) {
		usermem = 1;
		memblock_remove(memblock_start_of_DRAM(),
			memblock_end_of_DRAM() - memblock_start_of_DRAM());
	}
	size = memparse(p, &p);
	if (*p == '@')
		start = memparse(p + 1, &p);
	else {
		pr_err("\"memmap\" invalid format!\n");
		return -EINVAL;
	}

	memblock_add(start, size);

#ifdef CONFIG_CRASH_DUMP
	if (start && size) {
		crashmem_start = start;
		crashmem_size = size;
	}
#endif

	return 0;
}
early_param("memmap", early_parse_memmap);

#ifdef CONFIG_PROC_VMCORE
unsigned long setup_elfcorehdr, setup_elfcorehdr_size;
static int __init early_parse_elfcorehdr(char *p)
{
	u64 i;
	phys_addr_t start, end;

	setup_elfcorehdr = memparse(p, &p);

	for_each_mem_range(i, &start, &end) {
		if (setup_elfcorehdr >= start && setup_elfcorehdr < end) {
			/*
			 * Reserve from the elf core header to the end of
			 * the memory segment, that should all be kdump
			 * reserved memory.
			 */
			setup_elfcorehdr_size = end - setup_elfcorehdr;
			break;
		}
	}
	/*
	 * If we don't find it in the memory map, then we shouldn't
	 * have to worry about it, as the new kernel won't use it.
	 */
	return 0;
}
early_param("elfcorehdr", early_parse_elfcorehdr);
#endif

#ifdef CONFIG_KEXEC

/* 64M alignment for crash kernel regions */
#define CRASH_ALIGN	SZ_64M
#define CRASH_ADDR_MAX	SZ_512M

static void __init loongarch_parse_crashkernel(void)
{
	unsigned long long total_mem;
	unsigned long long crash_size, crash_base;
	int ret;

	total_mem = memblock_phys_mem_size();
	ret = parse_crashkernel(boot_command_line, total_mem,
				&crash_size, &crash_base);
	if (ret != 0 || crash_size <= 0)
		return;

	if (crash_base <= 0) {
		crash_base = memblock_find_in_range(CRASH_ALIGN, CRASH_ADDR_MAX,
							crash_size, CRASH_ALIGN);
		if (!crash_base) {
			pr_warn("crashkernel reservation failed - No suitable area found.\n");
			return;
		}
	} else {
		unsigned long long start;

		start = memblock_find_in_range(crash_base, crash_base + crash_size,
						crash_size, 1);
		if (start != crash_base) {
			pr_warn("Invalid memory region reserved for crash kernel\n");
			return;
		}
	}

	crashk_res.start = crash_base;
	crashk_res.end	 = crash_base + crash_size - 1;
}

static void __init request_crashkernel(struct resource *res)
{
	int ret;

	if (crashk_res.start == crashk_res.end)
		return;

	ret = request_resource(res, &crashk_res);
	if (!ret)
		pr_info("Reserving %ldMB of memory at %ldMB for crashkernel\n",
			(unsigned long)((crashk_res.end -
					 crashk_res.start + 1) >> 20),
			(unsigned long)(crashk_res.start  >> 20));
}
#else /* !defined(CONFIG_KEXEC)		*/
static void __init loongarch_parse_crashkernel(void)
{
}

static void __init request_crashkernel(struct resource *res)
{
}
#endif /* !defined(CONFIG_KEXEC)  */

/* Traditionally, LoongArch's contiguous low memory is 256M, so crashkernel=X@Y
 * is unable to be large enough in some cases. Thus, if the total memory of a
 * node is more than 1GB, we reserve the top 256MB for the capture kernel */
static void reserve_crashm_region(int node, unsigned long s0, unsigned long e0)
{
#ifdef CONFIG_KEXEC
	if (crashk_res.start == crashk_res.end)
		return;

	if ((e0 - s0) <= (SZ_1G >> PAGE_SHIFT))
		return;

	s0 = e0 - (SZ_256M >> PAGE_SHIFT);

	memblock_reserve(PFN_PHYS(s0), (e0 - s0) << PAGE_SHIFT);
#endif
}

/*
 * After the kdump operation is performed to enter the capture kernel, the
 * memory area used by the previous production kernel should be reserved to
 * avoid destroy to the captured data.
 */
static void reserve_oldmem_region(int node, unsigned long s0, unsigned long e0)
{
#ifdef CONFIG_CRASH_DUMP
	unsigned long s1, e1;

	if (!is_kdump_kernel())
		return;

	if ((e0 - s0) > (SZ_1G >> PAGE_SHIFT))
		e0 = e0 - (SZ_256M >> PAGE_SHIFT);

	/* crashmem_start is crashk_res reserved by primary kernel */
	s1 = PFN_UP(crashmem_start);
	e1 = PFN_DOWN(crashmem_start + crashmem_size);

	if (node == 0) {
		memblock_reserve(PFN_PHYS(s0), (s1 - s0) << PAGE_SHIFT);
		memblock_reserve(PFN_PHYS(e1), (e0 - e1) << PAGE_SHIFT);
	} else {
		memblock_reserve(PFN_PHYS(s0), (e0 - s0) << PAGE_SHIFT);
	}
#endif
}

static void __init check_kernel_sections_mem(void)
{
	phys_addr_t start = __pa_symbol(&_text);
	phys_addr_t size = __pa_symbol(&_end) - start;

	if (!memblock_is_region_memory(start, size)) {
		pr_info("Kernel sections are not in the memory maps\n");
		memblock_add(start, size);
	}
}

static void __init bootcmdline_append(const char *s, size_t max)
{
	if (!s[0] || !max)
		return;

	if (boot_command_line[0])
		strlcat(boot_command_line, " ", COMMAND_LINE_SIZE);

	strlcat(boot_command_line, s, max);
}

static void __init bootcmdline_init(char **cmdline_p)
{
	/*
	 * If CMDLINE_OVERRIDE is enabled then initializing the command line is
	 * trivial - we simply use the built-in command line unconditionally &
	 * unmodified.
	 */
	if (IS_ENABLED(CONFIG_CMDLINE_OVERRIDE)) {
		strlcpy(boot_command_line, builtin_cmdline, COMMAND_LINE_SIZE);
		return;
	}

	/*
	 * in eraly_init -> fdt_setup
	 * __dt_setup_arch func will let dts->chosen->bootargs value copy to boot_command_line
	 * so dont clear boot_command_line in here.
	 */

	/*
	 * Take arguments from the bootloader at first. Early code should have
	 * filled arcs_cmdline with arguments from the bootloader.
	 */
	bootcmdline_append(arcs_cmdline, COMMAND_LINE_SIZE);

	/*
	 * If the user specified a built-in command line & we didn't already
	 * prepend it, we append it to boot_command_line here.
	 */
	if (IS_ENABLED(CONFIG_CMDLINE_BOOL))
		bootcmdline_append(builtin_cmdline, COMMAND_LINE_SIZE);

	strlcpy(command_line, boot_command_line, COMMAND_LINE_SIZE);
	*cmdline_p = command_line;

	parse_early_param();
}

/*
 * arch_mem_init - initialize memory management subsystem
 */
static void __init arch_mem_init(char **cmdline_p)
{
	unsigned int node;
	unsigned long start_pfn, end_pfn;

	/* call board setup routine */
	plat_mem_setup();
	memblock_set_bottom_up(true);

	if (usermem)
		pr_info("User-defined physical RAM map overwrite\n");

	check_kernel_sections_mem();

	if (loongson_fdt_blob)
		dt_bootmem_init();

#ifdef CONFIG_PROC_VMCORE
	if (setup_elfcorehdr && setup_elfcorehdr_size) {
		printk(KERN_INFO "kdump reserved memory at %lx-%lx\n",
		       setup_elfcorehdr, setup_elfcorehdr_size);
		memblock_reserve(setup_elfcorehdr, setup_elfcorehdr_size);
	}
#endif

	loongarch_parse_crashkernel();
#ifdef CONFIG_KEXEC
	if (crashk_res.start != crashk_res.end)
		memblock_reserve(crashk_res.start, resource_size(&crashk_res));
#endif
	for_each_online_node(node) {
		get_pfn_range_for_nid(node, &start_pfn, &end_pfn);
		reserve_crashm_region(node, start_pfn, end_pfn);
		reserve_oldmem_region(node, start_pfn, end_pfn);
	}

	/*
	 * In order to reduce the possibility of kernel panic when failed to
	 * get IO TLB memory under CONFIG_SWIOTLB, it is better to allocate
	 * low memory as small as possible before plat_swiotlb_setup(), so
	 * make sparse_init() using top-down allocation.
	 */
	memblock_set_bottom_up(false);
	sparse_init();
	memblock_set_bottom_up(true);

	plat_swiotlb_setup();

	dma_contiguous_reserve(PFN_PHYS(max_low_pfn));

	/* Reserve for hibernation. */
	memblock_reserve(__pa_symbol(&__nosave_begin),
		__pa_symbol(&__nosave_end) - __pa_symbol(&__nosave_begin));

	memblock_dump_all();

	early_memtest(PFN_PHYS(ARCH_PFN_OFFSET), PFN_PHYS(max_low_pfn));
}

static void __init resource_init(void)
{
	long i = 0;
	size_t res_size;
	struct resource *res;
	struct memblock_region *region;

	code_resource.start = __pa_symbol(&_text);
	code_resource.end = __pa_symbol(&_etext) - 1;
	data_resource.start = __pa_symbol(&_etext);
	data_resource.end = __pa_symbol(&_edata) - 1;
	bss_resource.start = __pa_symbol(&__bss_start);
	bss_resource.end = __pa_symbol(&__bss_stop) - 1;

	num_standard_resources = memblock.memory.cnt;
	res_size = num_standard_resources * sizeof(*standard_resources);
	standard_resources = memblock_alloc(res_size, SMP_CACHE_BYTES);

	for_each_mem_region(region) {
		res = &standard_resources[i++];
		if (!memblock_is_nomap(region)) {
			res->name  = "System RAM";
			res->flags = IORESOURCE_SYSTEM_RAM | IORESOURCE_BUSY;
			res->start = __pfn_to_phys(memblock_region_memory_base_pfn(region));
			res->end = __pfn_to_phys(memblock_region_memory_end_pfn(region)) - 1;
		} else {
			res->name  = "Reserved";
			res->flags = IORESOURCE_MEM;
			res->start = __pfn_to_phys(memblock_region_reserved_base_pfn(region));
			res->end = __pfn_to_phys(memblock_region_reserved_end_pfn(region)) - 1;
		}

		request_resource(&iomem_resource, res);

		/*
		 *  We don't know which RAM region contains kernel data,
		 *  so we try it repeatedly and let the resource manager
		 *  test it.
		 */
		request_resource(res, &code_resource);
		request_resource(res, &data_resource);
		request_resource(res, &bss_resource);
		request_crashkernel(res);
	}
}

static int __init reserve_memblock_reserved_regions(void)
{
	u64 i, j;

	for (i = 0; i < num_standard_resources; ++i) {
		struct resource *mem = &standard_resources[i];
		phys_addr_t r_start, r_end, mem_size = resource_size(mem);

		if (!memblock_is_region_reserved(mem->start, mem_size))
			continue;

		for_each_reserved_mem_range(j, &r_start, &r_end) {
			resource_size_t start, end;

			start = max(PFN_PHYS(PFN_DOWN(r_start)), mem->start);
			end = min(PFN_PHYS(PFN_UP(r_end)) - 1, mem->end);

			if (start > mem->end || end < mem->start)
				continue;

			reserve_region_with_split(mem, start, end, "Reserved");
		}
	}

	return 0;
}
arch_initcall(reserve_memblock_reserved_regions);

#ifdef CONFIG_SMP
static void __init prefill_possible_map(void)
{
	int i, possible;

	possible = num_processors + disabled_cpus;
	if (possible > nr_cpu_ids)
		possible = nr_cpu_ids;

	pr_info("SMP: Allowing %d CPUs, %d hotplug CPUs\n",
			possible, max((possible - num_processors), 0));

	for (i = 0; i < possible; i++)
		set_cpu_possible(i, true);
	for (; i < NR_CPUS; i++)
		set_cpu_possible(i, false);

	nr_cpu_ids = possible;
}
#else
static inline void prefill_possible_map(void) {}
#endif

void __init setup_arch(char **cmdline_p)
{
	cpu_probe();

	early_init();
	pagetable_init();
	bootcmdline_init(cmdline_p);

	init_initrd();
	platform_init();

	arch_mem_init(cmdline_p);

	resource_init();
	plat_smp_setup();
	prefill_possible_map();

	paging_init();

#if defined(CONFIG_KASAN)
	kasan_init();
#endif

	unwind_init();
}

#ifdef CONFIG_USE_OF
unsigned long fw_passed_dtb;

void __init __dt_setup_arch(void *bph)
{
	early_init_dt_scan(bph);
}
#endif

#ifdef CONFIG_DEBUG_FS
struct dentry *loongarch_debugfs_dir;
static int __init debugfs_loongarch(void)
{
	struct dentry *d;

	d = debugfs_create_dir("loongarch", NULL);
	if (!d)
		return -ENOMEM;
	loongarch_debugfs_dir = d;
	return 0;
}
arch_initcall(debugfs_loongarch);
#endif
