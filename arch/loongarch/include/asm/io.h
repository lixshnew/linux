/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2020 Loongson Technology Corporation Limited
 */
#ifndef _ASM_IO_H
#define _ASM_IO_H

#define ARCH_HAS_IOREMAP_WC

#include <linux/compiler.h>
#include <linux/kernel.h>
#include <linux/types.h>

#include <asm/addrspace.h>
#include <asm/bug.h>
#include <asm/byteorder.h>
#include <asm/cpu.h>
#include <asm/page.h>
#include <asm/pgtable-bits.h>
#include <asm/string.h>

/*
 * On LoongArch, I/O ports mappring is following:
 *
 *		|    	  ....		|
 *		|-----------------------|
 *		| pci io ports(16K~32M)	|
 *		|-----------------------|
 *		| isa io ports(0  ~16K)	|
 * PCI_IOBASE ->|-----------------------|
 *		|    	  ....       	|
 */
#define PCI_IOBASE	((void __iomem *)(vm_map_base + (2 * PAGE_SIZE)))
#define PCI_IOSIZE	SZ_32M
#define ISA_IOSIZE	SZ_16K
#define IO_SPACE_LIMIT	(PCI_IOSIZE - 1)
#define ISA_PHY_IOBASE	LOONGSON_LIO_BASE

/*
 * ISA I/O bus memory addresses are 1:1 with the physical address.
 */
#define isa_virt_to_bus virt_to_phys
#define isa_bus_to_virt phys_to_virt

#define isa_page_to_bus page_to_phys

/*
 * Change "struct page" to physical address.
 */
#define page_to_phys(page)	((phys_addr_t)page_to_pfn(page) << PAGE_SHIFT)

extern void __init __iomem *early_ioremap(u64 phys_addr, unsigned long size);
extern void __init early_iounmap(void __iomem *addr, unsigned long size);

#define early_memremap early_ioremap
#define early_memunmap early_iounmap

#define ioremap_prot ioremap_prot
#define ioremap_uc ioremap
extern void __iomem *ioremap_prot(phys_addr_t phys_addr, size_t size, unsigned long prot);

/*
 * ioremap -   map bus memory into CPU space
 * @offset:    bus address of the memory
 * @size:      size of the resource to map
 *
 * ioremap performs a platform specific sequence of operations to
 * make bus memory CPU accessible via the readb/readw/readl/writeb/
 * writew/writel functions and the other mmio helpers. The returned
 * address is not guaranteed to be usable directly as a virtual
 * address.
 */
#define ioremap ioremap
#define ioremap_nocache ioremap
extern void __iomem *ioremap(phys_addr_t phys_addr, size_t size);

/*
 * ioremap_wc - map bus memory into CPU space
 * @offset:     bus address of the memory
 * @size:       size of the resource to map
 *
 * ioremap_wc performs a platform specific sequence of operations to
 * make bus memory CPU accessible via the readb/readw/readl/writeb/
 * writew/writel functions and the other mmio helpers. The returned
 * address is not guaranteed to be usable directly as a virtual
 * address.
 *
 * This version of ioremap ensures that the memory is marked uncachable
 * but accelerated by means of write-combining feature. It is specifically
 * useful for PCIe prefetchable windows, which may vastly improve a
 * communications performance. If it was determined on boot stage, what
 * CPU CCA doesn't support WUC, the method shall fall-back to the
 * _CACHE_SUC option (see cpu_probe() method).
 */
#define ioremap_wc ioremap_wc
extern void __iomem *ioremap_wc(phys_addr_t phys_addr, size_t size);

/*
 * ioremap_cache -  map bus memory into CPU space
 * @offset:	    bus address of the memory
 * @size:	    size of the resource to map
 *
 * ioremap_cache performs a platform specific sequence of operations to
 * make bus memory CPU accessible via the readb/readw/readl/writeb/
 * writew/writel functions and the other mmio helpers. The returned
 * address is not guaranteed to be usable directly as a virtual
 * address.
 *
 * This version of ioremap ensures that the memory is marked cachable by
 * the CPU.  Also enables full write-combining.	 Useful for some
 * memory-like regions on I/O busses.
 */
#define ioremap_cache ioremap_cache
extern void __iomem *ioremap_cache(phys_addr_t phys_addr, size_t size);

/*
 * ioremap_wc - map bus memory into CPU space
 * @offset:	bus address of the memory
 * @size:	size of the resource to map
 *
 * ioremap_wc performs a platform specific sequence of operations to
 * make bus memory CPU accessible via the readb/readw/readl/writeb/
 * writew/writel functions and the other mmio helpers. The returned
 * address is not guaranteed to be usable directly as a virtual
 * address.
 *
 * This version of ioremap ensures that the memory is marked uncachable
 * but accelerated by means of write-combining feature. It is specifically
 * useful for PCIe prefetchable windows, which may vastly improve a
 * communications performance. If it was determined on boot stage, what
 * CPU CCA doesn't support WUC, the method shall fall-back to the
 * _CACHE_SUC option (see cpu_probe() method).
 */
extern void __iomem *ioremap_wc(phys_addr_t phys_addr, size_t size);
#define ioremap_wc ioremap_wc

#define iounmap iounmap
extern void iounmap(const volatile void __iomem *addr);

/* Depends on LoongArch II instruction set */
#define mmiowb() asm volatile ("dbar 0" ::: "memory")

#define __BUILD_MEMORY_SINGLE(pfx, bwlq, type)				\
									\
static inline void pfx##write##bwlq(type val,				\
				    volatile void __iomem *mem)		\
{									\
	volatile type *__mem;						\
									\
	wmb();								\
									\
	__mem = (void *)(unsigned long)(mem);				\
									\
	if (sizeof(type) != sizeof(u64) || sizeof(u64) == sizeof(long)) \
		*__mem = val;						\
	else								\
		BUG();							\
}									\
									\
static inline type pfx##read##bwlq(const volatile void __iomem *mem)	\
{									\
	volatile type *__mem;						\
	type __val;							\
									\
	__mem = (void *)(unsigned long)(mem);				\
									\
	if (sizeof(type) != sizeof(u64) || sizeof(u64) == sizeof(long)) \
		__val = *__mem;						\
	else {								\
		__val = 0;						\
		BUG();							\
	}								\
									\
	/* prevent prefetching of coherent DMA data prematurely */	\
	rmb();								\
	return __val;							\
}

#define __BUILD_MEMORY_PFX(bus, bwlq, type)				\
									\
__BUILD_MEMORY_SINGLE(bus, bwlq, type)

#define BUILDIO_MEM(bwlq, type)						\
									\
__BUILD_MEMORY_PFX(__raw_, bwlq, type)

BUILDIO_MEM(b, u8)
BUILDIO_MEM(w, u16)
BUILDIO_MEM(l, u32)
#ifdef CONFIG_64BIT
BUILDIO_MEM(q, u64)
#endif

#define __raw_readb __raw_readb
#define __raw_readw __raw_readw
#define __raw_readl __raw_readl
#define __raw_writeb __raw_writeb
#define __raw_writew __raw_writew
#define __raw_writel __raw_writel
#ifdef CONFIG_64BIT
#define __raw_readq __raw_readq
#define __raw_writeq __raw_writeq
#endif

#define readb	__raw_readb
#define readw	__raw_readw
#define readl	__raw_readl
#define writeb	__raw_writeb
#define writew	__raw_writew
#define writel	__raw_writel
#ifdef CONFIG_64BIT
#define readq	__raw_readq
#define writeq	__raw_writeq
#endif

#define inb(c)		({ u8  __v = __raw_readb(PCI_IOBASE + (c)); __v; })
#define inw(c)		({ u16  __v = __raw_readw(PCI_IOBASE + (c)); __v; })
#define inl(c)		({ u32  __v = __raw_readl(PCI_IOBASE + (c)); __v; })

#define outb(v, c)	({ __raw_writeb((v), (PCI_IOBASE + (c))); })
#define outw(v, c)	({ __raw_writew((v), (PCI_IOBASE + (c))); })
#define outl(v, c)	({ __raw_writel((v), (PCI_IOBASE + (c))); })

#define readb_be(addr)							\
	__raw_readb((__force unsigned *)(addr))
#define readw_be(addr)							\
	be16_to_cpu(__raw_readw((__force unsigned *)(addr)))
#define readl_be(addr)							\
	be32_to_cpu(__raw_readl((__force unsigned *)(addr)))
#define readq_be(addr)							\
	be64_to_cpu(__raw_readq((__force unsigned *)(addr)))

#define writeb_be(val, addr)						\
	__raw_writeb((val), (__force unsigned *)(addr))
#define writew_be(val, addr)						\
	__raw_writew(cpu_to_be16((val)), (__force unsigned *)(addr))
#define writel_be(val, addr)						\
	__raw_writel(cpu_to_be32((val)), (__force unsigned *)(addr))
#define writeq_be(val, addr)						\
	__raw_writeq(cpu_to_be64((val)), (__force unsigned *)(addr))

/*
 * String version of I/O memory access operations.
 */
extern void __memset_io(volatile void __iomem *dst, int c, size_t count);
extern void __memcpy_toio(volatile void __iomem *to, const void *from, size_t count);
extern void __memcpy_fromio(void *to, const volatile void __iomem *from, size_t count);
#define memset_io(c, v, l)     __memset_io((c), (v), (l))
#define memcpy_fromio(a, c, l) __memcpy_fromio((a), (c), (l))
#define memcpy_toio(c, a, l)   __memcpy_toio((c), (a), (l))

/*
 * The caches on some architectures aren't dma-coherent and have need to
 * handle this in software.  There are three types of operations that
 * can be applied to dma buffers.
 *
 *  - dma_cache_wback_inv(start, size) makes caches and coherent by
 *    writing the content of the caches back to memory, if necessary.
 *    The function also invalidates the affected part of the caches as
 *    necessary before DMA transfers from outside to memory.
 *  - dma_cache_wback(start, size) makes caches and coherent by
 *    writing the content of the caches back to memory, if necessary.
 *    The function also invalidates the affected part of the caches as
 *    necessary before DMA transfers from outside to memory.
 *  - dma_cache_inv(start, size) invalidates the affected parts of the
 *    caches.  Dirty lines of the caches may be written back or simply
 *    be discarded.  This operation is necessary before dma operations
 *    to the memory.
 *
 * This API used to be exported; it now is for arch code internal use only.
 */

#define dma_cache_wback_inv(start,size) \
	do { (void) (start); (void) (size); } while (0)
#define dma_cache_wback(start,size)	\
	do { (void) (start); (void) (size); } while (0)
#define dma_cache_inv(start,size)	\
	do { (void) (start); (void) (size); } while (0)

/*
 * Convert a physical pointer to a virtual kernel pointer for /dev/mem
 * access
 */
#define xlate_dev_mem_ptr(p)	__va(p)

/*
 * Convert a virtual cached pointer to an uncached pointer
 */
#define xlate_dev_kmem_ptr(p)	p

#include <asm-generic/io.h>

#endif /* _ASM_IO_H */
