// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2013,2014 Linaro Limited
 *     Roy Franz <roy.franz@linaro.org
 * Copyright (C) 2013 Red Hat, Inc.
 *     Mark Salter <msalter@redhat.com>
 * Copyright (C) 2020 Loongson, Inc.
 *     Yun Liu <liuyun@loongson.cn>
 */

#include <linux/efi.h>
#include <linux/mm.h>
#include <linux/sort.h>
#include <asm/efi.h>
#include <asm/addrspace.h>
#include <asm/mach-loongson64/boot_param.h>
#include "efistub.h"

#define MAX_ARG_COUNT		128
#define CMDLINE_MAX_SIZE	0x200

#define EFI_LOONGSON_BOOT_PARAM_GUID \
	EFI_GUID(0x4660f721, 0x2ec5, 0x416a, 0x89, 0x9a, 0x43, 0x18, 0x02, 0x50, 0xa0, 0xc9)

typedef struct loongson_mem_map mmap;
typedef void (*kernel_entry_t)(int argc, char *argv[], struct boot_params *boot_p);

extern char efi_heap[];
extern kernel_entry_t kernel_entry;
extern void decompress_kernel(unsigned long boot_heap_start);

static int argc;
static char **argv;
const efi_system_table_t *efi_system_table;
static unsigned int map_entry[LOONGSON3_BOOT_MEM_MAP_MAX];
static struct efi_mmap mmap_array[EFI_MAX_MEMORY_TYPE][LOONGSON3_BOOT_MEM_MAP_MAX];

struct exit_boot_struct {
	struct boot_params *bp;
	unsigned int *runtime_entry_count;
};

unsigned char efi_crc8(char *buff, int size)
{
	int sum, cnt;

	for (sum = 0, cnt = 0; cnt < size; cnt++)
		sum = (char) (sum + *(buff + cnt));

	return (char)(0x100 - sum);
}

static unsigned long convert_priv_cmdline(char *cmdline_ptr,
		unsigned long rd_addr, unsigned long rd_size)
{
	unsigned int rdprev_size;
	unsigned int cmdline_size;
	efi_status_t status;
	char *pstr, *substr;
	char *initrd_ptr = NULL;
	char convert_str[CMDLINE_MAX_SIZE];
	static char cmdline_array[CMDLINE_MAX_SIZE];

	cmdline_size = strlen(cmdline_ptr);
	snprintf(cmdline_array, CMDLINE_MAX_SIZE, "kernel ");

	initrd_ptr = strstr(cmdline_ptr, "initrd=");
	if (!initrd_ptr) {
		snprintf(cmdline_array, CMDLINE_MAX_SIZE, "kernel %s", cmdline_ptr);
		goto completed;
	}
	snprintf(convert_str, CMDLINE_MAX_SIZE, " initrd=0x%lx,0x%lx", rd_addr, rd_size);
	rdprev_size = cmdline_size - strlen(initrd_ptr);
	strncat(cmdline_array, cmdline_ptr, rdprev_size);

	cmdline_ptr = strnstr(initrd_ptr, " ", CMDLINE_MAX_SIZE);
	strcat(cmdline_array, convert_str);
	if (!cmdline_ptr)
		goto completed;

	strcat(cmdline_array, cmdline_ptr);

completed:
	status = efi_allocate_pages((MAX_ARG_COUNT + 1) * (sizeof(char *)),
					(unsigned long *)&argv, ULONG_MAX);
	if (status != EFI_SUCCESS) {
		efi_err("Alloc argv mmap_array error\n");
		return status;
	}

	argc = 0;
	pstr = cmdline_array;

	substr = strsep(&pstr, " \t");
	while (substr != NULL) {
		if (strlen(substr)) {
			argv[argc++] = substr;
			if (argc == MAX_ARG_COUNT) {
				efi_err("Argv mmap_array full!\n");
				break;
			}
		}
		substr = strsep(&pstr, " \t");
	}

	return EFI_SUCCESS;
}

unsigned int efi_memmap_sort(struct loongsonlist_mem_map *memmap,
			unsigned int index, unsigned int mem_type)
{
	unsigned int i, t;
	unsigned long msize;

	for (i = 0; i < map_entry[mem_type]; i = t) {
		msize = mmap_array[mem_type][i].mem_size;
		for (t = i + 1; t < map_entry[mem_type]; t++) {
			if (mmap_array[mem_type][i].mem_start + msize <
					mmap_array[mem_type][t].mem_start)
				break;

			msize += mmap_array[mem_type][t].mem_size;
		}
		memmap->map[index].mem_type = mem_type;
		memmap->map[index].mem_start = mmap_array[mem_type][i].mem_start;
		memmap->map[index].mem_size = msize;
		memmap->map[index].attribute = mmap_array[mem_type][i].attribute;
		index++;
	}

	return index;
}

static efi_status_t mk_mmap(struct efi_boot_memmap *map, struct boot_params *p)
{
	char checksum;
	unsigned int i;
	unsigned int nr_desc;
	unsigned int mem_type;
	unsigned long count;
	efi_memory_desc_t *mem_desc;
	struct loongsonlist_mem_map *mhp = NULL;

	if (!strncmp((char *)p, "BPI", 3)) {
		p->systemtable = efi_system_table;
		p->flags |= BPI_FLAGS_UEFI_SUPPORTED;
		p->extlist_offset = sizeof(*p) + sizeof(unsigned long);
		mhp = (struct loongsonlist_mem_map *)((char *)p + p->extlist_offset);

		memcpy(&mhp->header.signature, "MEM", sizeof(unsigned long));
		mhp->header.length = sizeof(*mhp);
		mhp->desc_version = *map->desc_ver;
	}
	if (!(*(map->map_size)) || !(*(map->desc_size)) || !mhp) {
		efi_err("get memory info error\n");
		return EFI_INVALID_PARAMETER;
	}
	nr_desc = *(map->map_size) / *(map->desc_size);

	/*
	 * According to UEFI SPEC, mmap_buf is the accurate Memory Map
	 * mmap_array now we can fill platform specific memory structure.
	 */
	for (i = 0; i < nr_desc; i++) {
		mem_desc = (efi_memory_desc_t *)((void *)(*map->map) + (i * (*(map->desc_size))));
		switch (mem_desc->type) {
		case EFI_RESERVED_TYPE:
		case EFI_RUNTIME_SERVICES_CODE:
		case EFI_RUNTIME_SERVICES_DATA:
		case EFI_MEMORY_MAPPED_IO:
		case EFI_MEMORY_MAPPED_IO_PORT_SPACE:
		case EFI_UNUSABLE_MEMORY:
		case EFI_PAL_CODE:
			mem_type = ADDRESS_TYPE_RESERVED;
			break;

		case EFI_ACPI_MEMORY_NVS:
			mem_type = ADDRESS_TYPE_NVS;
			break;

		case EFI_ACPI_RECLAIM_MEMORY:
			mem_type = ADDRESS_TYPE_ACPI;
			break;

		case EFI_LOADER_CODE:
		case EFI_LOADER_DATA:
		case EFI_PERSISTENT_MEMORY:
		case EFI_BOOT_SERVICES_CODE:
		case EFI_BOOT_SERVICES_DATA:
		case EFI_CONVENTIONAL_MEMORY:
			mem_type = ADDRESS_TYPE_SYSRAM;
			break;

		default:
			continue;
		}

		mmap_array[mem_type][map_entry[mem_type]].mem_type = mem_type;
		mmap_array[mem_type][map_entry[mem_type]].mem_start =
						mem_desc->phys_addr & TO_PHYS_MASK;
		mmap_array[mem_type][map_entry[mem_type]].mem_size =
						mem_desc->num_pages << EFI_PAGE_SHIFT;
		mmap_array[mem_type][map_entry[mem_type]].attribute =
						mem_desc->attribute;
		map_entry[mem_type]++;
	}

	count = mhp->map_count;
	/* Sort EFI memmap and add to BPI for kernel */
	for (i = 0; i < LOONGSON3_BOOT_MEM_MAP_MAX; i++) {
		if (!map_entry[i])
			continue;
		count = efi_memmap_sort(mhp, count, i);
	}

	mhp->map_count = count;
	mhp->header.checksum = 0;

	checksum = efi_crc8((char *)mhp, mhp->header.length);
	mhp->header.checksum = checksum;

	return EFI_SUCCESS;
}

static efi_status_t exit_boot_func(struct efi_boot_memmap *map, void *priv)
{
	efi_status_t status;
	struct exit_boot_struct *p = priv;

	status = mk_mmap(map, p->bp);
	if (status != EFI_SUCCESS) {
		efi_err("Make kernel memory map failed!\n");
		return status;
	}

	return EFI_SUCCESS;
}

static efi_status_t exit_boot_services(struct boot_params *boot_params, void *handle)
{
	unsigned int desc_version;
	unsigned int runtime_entry_count = 0;
	unsigned long map_sz, key, desc_size, buff_size;
	efi_status_t status;
	struct efi_boot_memmap map;
	struct exit_boot_struct priv;
	efi_memory_desc_t *mem_map;

	map.map			= &mem_map;
	map.map_size		= &map_sz;
	map.desc_size		= &desc_size;
	map.desc_ver		= &desc_version;
	map.key_ptr		= &key;
	map.buff_size		= &buff_size;
	status = efi_get_memory_map(&map);
	if (status != EFI_SUCCESS) {
		efi_err("Unable to retrieve UEFI memory map.\n");
		return status;
	}

	priv.bp = boot_params;
	priv.runtime_entry_count = &runtime_entry_count;

	/* Might as well exit boot services now */
	status = efi_exit_boot_services(handle, &map, &priv, exit_boot_func);
	if (status != EFI_SUCCESS)
		return status;

	return EFI_SUCCESS;
}

/*
 * EFI entry point for the LoongArch EFI stub.
 */
efi_status_t __efiapi efi_pe_entry(efi_handle_t handle, efi_system_table_t *sys_table)
{
	efi_status_t status;
	unsigned int cmdline_size = 0;
	unsigned long initrd_addr = 0;
	unsigned long initrd_size = 0;
	enum efi_secureboot_mode secure_boot;
	char *cmdline_ptr = NULL;
	struct boot_params *boot_p;
	efi_loaded_image_t *image;
	efi_guid_t loaded_image_proto = LOADED_IMAGE_PROTOCOL_GUID;

	efi_system_table = sys_table;

	/* Check if we were booted by the EFI firmware */
	if (sys_table->hdr.signature != EFI_SYSTEM_TABLE_SIGNATURE)
		goto fail;

	/*
	 * Get a handle to the loaded image protocol.  This is used to get
	 * information about the running image, such as size and the command
	 * line.
	 */
	status = sys_table->boottime->handle_protocol(handle,
					&loaded_image_proto, (void *)&image);
	if (status != EFI_SUCCESS) {
		efi_err("Failed to get loaded image protocol\n");
		goto fail;
	}

	/* Get the command line from EFI, using the LOADED_IMAGE protocol. */
	cmdline_ptr = efi_convert_cmdline(image, &cmdline_size);
	if (!cmdline_ptr) {
		efi_err("Getting command line failed!\n");
		goto fail_free_cmdline;
	}

#ifdef CONFIG_CMDLINE_BOOL
	if (cmdline_size == 0)
		efi_parse_options(CONFIG_CMDLINE);
#endif
	if (!IS_ENABLED(CONFIG_CMDLINE_OVERRIDE) && cmdline_size > 0)
		efi_parse_options(cmdline_ptr);

	efi_info("Booting Linux Kernel...\n");

	secure_boot = efi_get_secureboot();
	efi_enable_reset_attack_mitigation();

	status = efi_load_initrd(image, &initrd_addr, &initrd_size, SZ_4G, ULONG_MAX);
	if (status != EFI_SUCCESS) {
		efi_err("Failed get initrd addr!\n");
		goto fail_free;
	}

	status = convert_priv_cmdline(cmdline_ptr, initrd_addr, initrd_size);
	if (status != EFI_SUCCESS) {
		efi_err("Covert cmdline failed!\n");
		goto fail_free;
	}

	boot_p = get_efi_config_table(EFI_LOONGSON_BOOT_PARAM_GUID);
	if (!boot_p) {
		efi_err("System table not found BPI!\n");
		goto fail;
	}

	status = exit_boot_services(boot_p, handle);
	if (status != EFI_SUCCESS) {
		efi_err("exit_boot services failed!\n");
		goto fail_free;
	}

	decompress_kernel((unsigned long)efi_heap);

	kernel_entry(argc, argv, boot_p);

	return EFI_SUCCESS;

fail_free:
	efi_free(initrd_size, initrd_addr);

fail_free_cmdline:
	efi_free(cmdline_size, (unsigned long)cmdline_ptr);

fail:
	return status;
}
