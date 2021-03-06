/*
 *  PowerPC version
 *    Copyright (C) 1995-1996 Gary Thomas (gdt@linuxppc.org)
 *
 *  Modifications by Paul Mackerras (PowerMac) (paulus@cs.anu.edu.au)
 *  and Cort Dougan (PReP) (cort@cs.nmt.edu)
 *    Copyright (C) 1996 Paul Mackerras
 *
 *  Derived from "arch/i386/mm/init.c"
 *    Copyright (C) 1991, 1992, 1993, 1994  Linus Torvalds
 *
 *  Dave Engebretsen <engebret@us.ibm.com>
 *      Rework for PPC64 port.
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version
 *  2 of the License, or (at your option) any later version.
 *
 */

#undef DEBUG

#include <linux/signal.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/mman.h>
#include <linux/mm.h>
#include <linux/swap.h>
#include <linux/stddef.h>
#include <linux/vmalloc.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/bootmem.h>
#include <linux/highmem.h>
#include <linux/idr.h>
#include <linux/nodemask.h>
#include <linux/module.h>
#include <linux/poison.h>
#include <linux/lmb.h>

#include <asm/pgalloc.h>
#include <asm/page.h>
#include <asm/prom.h>
#include <asm/rtas.h>
#include <asm/io.h>
#include <asm/mmu_context.h>
#include <asm/pgtable.h>
#include <asm/mmu.h>
#include <asm/uaccess.h>
#include <asm/smp.h>
#include <asm/machdep.h>
#include <asm/tlb.h>
#include <asm/eeh.h>
#include <asm/processor.h>
#include <asm/mmzone.h>
#include <asm/cputable.h>
#include <asm/sections.h>
#include <asm/system.h>
#include <asm/iommu.h>
#include <asm/abs_addr.h>
#include <asm/vdso.h>

#include "mmu_decl.h"

#ifdef CONFIG_PPC_STD_MMU_64
#if PGTABLE_RANGE > USER_VSID_RANGE
#warning Limited user VSID range means pagetable space is wasted
#endif

#if (TASK_SIZE_USER64 < PGTABLE_RANGE) && (TASK_SIZE_USER64 < USER_VSID_RANGE)
#warning TASK_SIZE is smaller than it needs to be.
#endif
#endif /* CONFIG_PPC_STD_MMU_64 */

phys_addr_t memstart_addr = ~0;
phys_addr_t kernstart_addr;

void free_initmem(void)
{
	unsigned long addr;

	addr = (unsigned long)__init_begin;
	for (; addr < (unsigned long)__init_end; addr += PAGE_SIZE) {
		memset((void *)addr, POISON_FREE_INITMEM, PAGE_SIZE);
		ClearPageReserved(virt_to_page(addr));
		init_page_count(virt_to_page(addr));
		free_page(addr);
		totalram_pages++;
	}
	printk ("Freeing unused kernel memory: %luk freed\n",
		((unsigned long)__init_end - (unsigned long)__init_begin) >> 10);
}

#ifdef CONFIG_BLK_DEV_INITRD
void free_initrd_mem(unsigned long start, unsigned long end)
{
	if (start < end)
		printk ("Freeing initrd memory: %ldk freed\n", (end - start) >> 10);
	for (; start < end; start += PAGE_SIZE) {
		ClearPageReserved(virt_to_page(start));
		init_page_count(virt_to_page(start));
		free_page(start);
		totalram_pages++;
	}
}
#endif

static void pgd_ctor(void *addr)
{
	memset(addr, 0, PGD_TABLE_SIZE);
}

static void pmd_ctor(void *addr)
{
	memset(addr, 0, PMD_TABLE_SIZE);
}

static const unsigned int pgtable_cache_size[2] = {
	PGD_TABLE_SIZE, PMD_TABLE_SIZE
};
static const char *pgtable_cache_name[ARRAY_SIZE(pgtable_cache_size)] = {
#ifdef CONFIG_PPC_64K_PAGES
	"pgd_cache", "pmd_cache",
#else
	"pgd_cache", "pud_pmd_cache",
#endif /* CONFIG_PPC_64K_PAGES */
};

#ifdef CONFIG_HUGETLB_PAGE
/* Hugepages need an extra cache per hugepagesize, initialized in
 * hugetlbpage.c.  We can't put into the tables above, because HPAGE_SHIFT
 * is not compile time constant. */
struct kmem_cache *pgtable_cache[ARRAY_SIZE(pgtable_cache_size)+MMU_PAGE_COUNT];
#else
struct kmem_cache *pgtable_cache[ARRAY_SIZE(pgtable_cache_size)];
#endif

void pgtable_cache_init(void)
{
	pgtable_cache[0] = kmem_cache_create(pgtable_cache_name[0], PGD_TABLE_SIZE, PGD_TABLE_SIZE, SLAB_PANIC, pgd_ctor);
	pgtable_cache[1] = kmem_cache_create(pgtable_cache_name[1], PMD_TABLE_SIZE, PMD_TABLE_SIZE, SLAB_PANIC, pmd_ctor);
}

#ifdef CONFIG_SPARSEMEM_VMEMMAP
/*
 * Given an address within the vmemmap, determine the pfn of the page that
 * represents the start of the section it is within.  Note that we have to
 * do this by hand as the proffered address may not be correctly aligned.
 * Subtraction of non-aligned pointers produces undefined results.
 */
static unsigned long __meminit vmemmap_section_start(unsigned long page)
{
	unsigned long offset = page - ((unsigned long)(vmemmap));

	/* Return the pfn of the start of the section. */
	return (offset / sizeof(struct page)) & PAGE_SECTION_MASK;
}

/*
 * Check if this vmemmap page is already initialised.  If any section
 * which overlaps this vmemmap page is initialised then this page is
 * initialised already.
 */
static int __meminit vmemmap_populated(unsigned long start, int page_size)
{
	unsigned long end = start + page_size;

	for (; start < end; start += (PAGES_PER_SECTION * sizeof(struct page)))
		if (pfn_valid(vmemmap_section_start(start)))
			return 1;

	return 0;
}

/* On hash-based CPUs, the vmemmap is bolted in the hash table.
 *
 * On Book3E CPUs, the vmemmap is currently mapped in the top half of
 * the vmalloc space using normal page tables, though the size of
 * pages encoded in the PTEs can be different
 */

#ifdef CONFIG_PPC_BOOK3E
static void __meminit vmemmap_create_mapping(unsigned long start,
					     unsigned long page_size,
					     unsigned long phys)
{
	/* Create a PTE encoding without page size */
	unsigned long i, flags = _PAGE_PRESENT | _PAGE_ACCESSED |
		_PAGE_KERNEL_RW;

	/* PTEs only contain page size encodings up to 32M */
	BUG_ON(mmu_psize_defs[mmu_vmemmap_psize].enc > 0xf);

	/* Encode the size in the PTE */
	flags |= mmu_psize_defs[mmu_vmemmap_psize].enc << 8;

	/* For each PTE for that area, map things. Note that we don't
	 * increment phys because all PTEs are of the large size and
	 * thus must have the low bits clear
	 */
	for (i = 0; i < page_size; i += PAGE_SIZE)
		BUG_ON(map_kernel_page(start + i, phys, flags));
}
#else /* CONFIG_PPC_BOOK3E */
static void __meminit vmemmap_create_mapping(unsigned long start,
					     unsigned long page_size,
					     unsigned long phys)
{
	int  mapped = htab_bolt_mapping(start, start + page_size, phys,
					PAGE_KERNEL, mmu_vmemmap_psize,
					mmu_kernel_ssize);
	BUG_ON(mapped < 0);
}
#endif /* CONFIG_PPC_BOOK3E */

int __meminit vmemmap_populate(struct page *start_page,
			       unsigned long nr_pages, int node)
{
	unsigned long start = (unsigned long)start_page;
	unsigned long end = (unsigned long)(start_page + nr_pages);
	unsigned long page_size = 1 << mmu_psize_defs[mmu_vmemmap_psize].shift;

	/* Align to the page size of the linear mapping. */
	start = _ALIGN_DOWN(start, page_size);

	pr_debug("vmemmap_populate page %p, %ld pages, node %d\n",
		 start_page, nr_pages, node);
	pr_debug(" -> map %lx..%lx\n", start, end);

	for (; start < end; start += page_size) {
		void *p;

		if (vmemmap_populated(start, page_size))
			continue;

		p = vmemmap_alloc_block(page_size, node);
		if (!p)
			return -ENOMEM;

		pr_debug("      * %016lx..%016lx allocated at %p\n",
			 start, start + page_size, p);

		vmemmap_create_mapping(start, page_size, __pa(p));
	}

	return 0;
}
#endif /* CONFIG_SPARSEMEM_VMEMMAP */
