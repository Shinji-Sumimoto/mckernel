/**
 * \file memory.c
 *  License details are found in the file LICENSE.
 * \brief
 *  Acquire physical pages and manipulate page table entries.
 * \author Taku Shimosawa  <shimosawa@is.s.u-tokyo.ac.jp> \par
 *      Copyright (C) 2011 - 2012  Taku Shimosawa
 */
/*
 * HISTORY
 */

#include <ihk/cpu.h>
#include <ihk/debug.h>
#include <ihk/mm.h>
#include <types.h>
#include <memory.h>
#include <string.h>
#include <errno.h>
#include <list.h>
#include <process.h>
#include <page.h>

#define	dkprintf(...)	do { if (0) kprintf(__VA_ARGS__); } while (0)
#define	ekprintf(...)	kprintf(__VA_ARGS__)

static char *last_page;
extern char _head[], _end[];

static struct ihk_mc_pa_ops *pa_ops;

extern unsigned long x86_kernel_phys_base;

void *early_alloc_page(void)
{
	void *p;

	if (!last_page) {
		last_page = (char *)(((unsigned long)_end + PAGE_SIZE - 1)
		                     & PAGE_MASK);
		/* Convert the virtual address from text's to straight maps */
		last_page = phys_to_virt(virt_to_phys(last_page));
	} else if (last_page == (void *)-1) {
		panic("Early allocator is already finalized. Do not use it.\n");
	}
	p = last_page;
	last_page += PAGE_SIZE;

	return p;
}

void *arch_alloc_page(enum ihk_mc_ap_flag flag)
{
	if (pa_ops)
		return pa_ops->alloc_page(1, PAGE_P2ALIGN, flag);
	else
		return early_alloc_page();
}
void arch_free_page(void *ptr)
{
	if (pa_ops)
		pa_ops->free_page(ptr, 1);
}

void *ihk_mc_alloc_aligned_pages(int npages, int p2align, enum ihk_mc_ap_flag flag)
{
	if (pa_ops)
		return pa_ops->alloc_page(npages, p2align, flag);
	else
		return NULL;
}

void *ihk_mc_alloc_pages(int npages, enum ihk_mc_ap_flag flag)
{
	return ihk_mc_alloc_aligned_pages(npages, PAGE_P2ALIGN, flag);
}

void ihk_mc_free_pages(void *p, int npages)
{
	if (pa_ops)
		pa_ops->free_page(p, npages);
}

void *ihk_mc_allocate(int size, enum ihk_mc_ap_flag flag)
{
	if (pa_ops && pa_ops->alloc)
		return pa_ops->alloc(size, flag);
	else
		return ihk_mc_alloc_pages(1, flag);
}

void ihk_mc_free(void *p)
{
	if (pa_ops && pa_ops->free)
		return pa_ops->free(p);
	else
		return ihk_mc_free_pages(p, 1);
}

void *get_last_early_heap(void)
{
	return last_page;
}

void flush_tlb(void)
{
	unsigned long cr3;

	asm volatile("movq %%cr3, %0; movq %0, %%cr3" : "=r"(cr3) : : "memory");
}

void flush_tlb_single(unsigned long addr)
{
   asm volatile("invlpg (%0)" :: "r" (addr) : "memory");
}

struct page_table {
	pte_t entry[PT_ENTRIES];
};

static struct page_table *init_pt;
static ihk_spinlock_t init_pt_lock;

#ifdef USE_LARGE_PAGES
static int use_1gb_page = 0;
#endif

#ifdef USE_LARGE_PAGES
static void check_available_page_size(void)
{
	uint32_t edx;

	asm ("cpuid" : "=d" (edx) : "a" (0x80000001) : "%rbx", "%rcx");
	use_1gb_page = (edx & (1 << 26))? 1: 0;
	kprintf("use_1gb_page: %d\n", use_1gb_page);

	return;
}
#endif

static unsigned long setup_l2(struct page_table *pt,
                              unsigned long page_head, unsigned long start,
                              unsigned long end)
{
	int i;
	unsigned long phys;

	for (i = 0; i < PT_ENTRIES; i++) {
		phys = page_head + ((unsigned long)i << PTL2_SHIFT);

		if (phys + PTL2_SIZE <= start || phys >= end) {
			pt->entry[i] = 0;
			continue;
		}

		pt->entry[i] = phys | PFL2_KERN_ATTR | PFL2_SIZE;
	}

	return virt_to_phys(pt);
}

static unsigned long setup_l3(struct page_table *pt,
                              unsigned long page_head, unsigned long start,
                              unsigned long end)
{
	int i;
	unsigned long phys, pt_phys;

	for (i = 0; i < PT_ENTRIES; i++) {
		phys = page_head + ((unsigned long)i << PTL3_SHIFT);

		if (phys + PTL3_SIZE <= start || phys >= end) {
			pt->entry[i] = 0;
			continue;
		}
		pt_phys = setup_l2(arch_alloc_page(IHK_MC_AP_CRITICAL), phys, start, end);

		pt->entry[i] = pt_phys | PFL3_PDIR_ATTR;
	}

	return virt_to_phys(pt);
}

static void init_normal_area(struct page_table *pt)
{
	unsigned long map_start, map_end, phys, pt_phys;
	int ident_index, virt_index;

	map_start = ihk_mc_get_memory_address(IHK_MC_GMA_MAP_START, 0);
	map_end = ihk_mc_get_memory_address(IHK_MC_GMA_MAP_END, 0);

	kprintf("map_start = %lx, map_end = %lx\n", map_start, map_end);
	ident_index = map_start >> PTL4_SHIFT;
	virt_index = (MAP_ST_START >> PTL4_SHIFT) & (PT_ENTRIES - 1);

	memset(pt, 0, sizeof(struct page_table));

	for (phys = (map_start & ~(PTL4_SIZE - 1)); phys < map_end;
	     phys += PTL4_SIZE) {
		pt_phys = setup_l3(arch_alloc_page(IHK_MC_AP_CRITICAL), phys,
		                   map_start, map_end);

		pt->entry[ident_index++] = pt_phys | PFL4_PDIR_ATTR;
		pt->entry[virt_index++] = pt_phys | PFL4_PDIR_ATTR;
	}
}

static struct page_table *__alloc_new_pt(enum ihk_mc_ap_flag ap_flag)
{
	struct page_table *newpt = arch_alloc_page(ap_flag);

	if(newpt)
		memset(newpt, 0, sizeof(struct page_table));

	return newpt;
}

/*
 * XXX: Confusingly, L4 and L3 automatically add PRESENT,
 *      but L2 and L1 do not!
 */

static enum ihk_mc_pt_attribute attr_mask
		= 0
		| PTATTR_FILEOFF
		| PTATTR_WRITABLE
		| PTATTR_USER
		| PTATTR_ACTIVE
		| 0;
#define	ATTR_MASK	attr_mask

void enable_ptattr_no_execute(void)
{
	attr_mask |= PTATTR_NO_EXECUTE;
	return;
}

#if 0
static unsigned long attr_to_l4attr(enum ihk_mc_pt_attribute attr)
{
	return (attr & ATTR_MASK) | PFL4_PRESENT;
}
#endif
static unsigned long attr_to_l3attr(enum ihk_mc_pt_attribute attr)
{
	unsigned long r = (attr & (ATTR_MASK | PTATTR_LARGEPAGE));

	if ((attr & PTATTR_UNCACHABLE) && (attr & PTATTR_LARGEPAGE)) {
		return r | PFL3_PCD | PFL3_PWT;
	}
	return r;
}
static unsigned long attr_to_l2attr(enum ihk_mc_pt_attribute attr)
{
	unsigned long r = (attr & (ATTR_MASK | PTATTR_LARGEPAGE));

	if ((attr & PTATTR_UNCACHABLE) && (attr & PTATTR_LARGEPAGE)) {
		return r | PFL2_PCD | PFL2_PWT; 
	}
	return r;
}
static unsigned long attr_to_l1attr(enum ihk_mc_pt_attribute attr)
{
	if (attr & PTATTR_UNCACHABLE) {
		return (attr & ATTR_MASK) | PFL1_PCD | PFL1_PWT;
	} else { 
		return (attr & ATTR_MASK);
	}
}

#define GET_VIRT_INDICES(virt, l4i, l3i, l2i, l1i) \
	l4i = ((virt) >> PTL4_SHIFT) & (PT_ENTRIES - 1); \
	l3i = ((virt) >> PTL3_SHIFT) & (PT_ENTRIES - 1); \
	l2i = ((virt) >> PTL2_SHIFT) & (PT_ENTRIES - 1); \
	l1i = ((virt) >> PTL1_SHIFT) & (PT_ENTRIES - 1)

#define	GET_INDICES_VIRT(l4i, l3i, l2i, l1i)		\
		( ((uint64_t)(l4i) << PTL4_SHIFT)	\
		| ((uint64_t)(l3i) << PTL3_SHIFT)	\
		| ((uint64_t)(l2i) << PTL2_SHIFT)	\
		| ((uint64_t)(l1i) << PTL1_SHIFT)	\
		)

void set_pte(pte_t *ppte, unsigned long phys, enum ihk_mc_pt_attribute attr)
{
	if (attr & PTATTR_LARGEPAGE) {
		*ppte = phys | attr_to_l2attr(attr) | PFL2_SIZE;
	}
	else {
		*ppte = phys | attr_to_l1attr(attr);
	}
}


#if 0
/* 
 * get_pte() 
 *
 * Descripton: walks the page tables (creates tables if not existing)
 *             and returns a pointer to the PTE corresponding to the
 *             virtual address.
 */
pte_t *get_pte(struct page_table *pt, void *virt, enum ihk_mc_pt_attribute attr, enum ihk_mc_ap_flag ap_flag)
{
	int l4idx, l3idx, l2idx, l1idx;
	unsigned long v = (unsigned long)virt;
	struct page_table *newpt;

	if (!pt) {
		pt = init_pt;
	}

	GET_VIRT_INDICES(v, l4idx, l3idx, l2idx, l1idx);

    /* TODO: more detailed attribute check */
	if (pt->entry[l4idx] & PFL4_PRESENT) {
		pt = phys_to_virt(pt->entry[l4idx] & PAGE_MASK);
	} else {
		if((newpt = __alloc_new_pt(ap_flag)) == NULL)
			return NULL;
		pt->entry[l4idx] = virt_to_phys(newpt) | attr_to_l4attr(attr);
		pt = newpt;
	}

	if (pt->entry[l3idx] & PFL3_PRESENT) {
		pt = phys_to_virt(pt->entry[l3idx] & PAGE_MASK);
	} else {
		if((newpt = __alloc_new_pt(ap_flag)) == NULL)
			return NULL;
		pt->entry[l3idx] = virt_to_phys(newpt) | attr_to_l3attr(attr);
		pt = newpt;
	}

	/* PTATTR_LARGEPAGE */
	if (attr & PTATTR_LARGEPAGE) {
		return &(pt->entry[l2idx]);
	}

	/* Requested regular page, but large is allocated? */
	if (pt->entry[l2idx] & PFL2_SIZE) {
		return NULL;
	}

	if (pt->entry[l2idx] & PFL2_PRESENT) {
		pt = phys_to_virt(pt->entry[l2idx] & PAGE_MASK);
	} else {
		if((newpt = __alloc_new_pt(ap_flag)) == NULL)
			return NULL;
		pt->entry[l2idx] = virt_to_phys(newpt) | attr_to_l2attr(attr)
			| PFL2_PRESENT;
		pt = newpt;
	}

	return &(pt->entry[l1idx]);
}
#endif

static int __set_pt_page(struct page_table *pt, void *virt, unsigned long phys,
                         enum ihk_mc_pt_attribute attr)
{
	int l4idx, l3idx, l2idx, l1idx;
	unsigned long v = (unsigned long)virt;
	struct page_table *newpt;
	enum ihk_mc_ap_flag ap_flag;
	int in_kernel =
		(((unsigned long long)virt) >= 0xffff000000000000ULL);
	unsigned long init_pt_lock_flags;
	int ret = -ENOMEM;

	init_pt_lock_flags = 0;	/* for avoidance of warning */
	if (in_kernel) {
		init_pt_lock_flags = ihk_mc_spinlock_lock(&init_pt_lock);
	}

	ap_flag = (attr & PTATTR_FOR_USER) ?
	                IHK_MC_AP_NOWAIT: IHK_MC_AP_CRITICAL;

	if (!pt) {
		pt = init_pt;
	}
	if (attr & PTATTR_LARGEPAGE) {
		phys &= LARGE_PAGE_MASK;
	} else {
		phys &= PAGE_MASK;
	}

	GET_VIRT_INDICES(v, l4idx, l3idx, l2idx, l1idx);

	/* TODO: more detailed attribute check */
	if (pt->entry[l4idx] & PFL4_PRESENT) {
		pt = phys_to_virt(pt->entry[l4idx] & PAGE_MASK);
	} else {
		if((newpt = __alloc_new_pt(ap_flag)) == NULL)
			goto out;
		pt->entry[l4idx] = virt_to_phys(newpt) | PFL4_PDIR_ATTR;
		pt = newpt;
	}

	if (pt->entry[l3idx] & PFL3_PRESENT) {
		pt = phys_to_virt(pt->entry[l3idx] & PAGE_MASK);
	} else {
		if((newpt = __alloc_new_pt(ap_flag)) == NULL)
			goto out;
		pt->entry[l3idx] = virt_to_phys(newpt) | PFL3_PDIR_ATTR;
		pt = newpt;
	}

	if (attr & PTATTR_LARGEPAGE) {
		if (pt->entry[l2idx] & PFL2_PRESENT) {
			if ((pt->entry[l2idx] & PAGE_MASK) != phys) {
				goto out;
			} else {
				ret = 0;
				goto out;
			}
		} else {
			pt->entry[l2idx] = phys | attr_to_l2attr(attr)
				| PFL2_SIZE;
			ret = 0;
			goto out;
		}
	}

	if (pt->entry[l2idx] & PFL2_PRESENT) {
		pt = phys_to_virt(pt->entry[l2idx] & PAGE_MASK);
	} else {
		if((newpt = __alloc_new_pt(ap_flag)) == NULL)
			goto out;
		pt->entry[l2idx] = virt_to_phys(newpt) | PFL2_PDIR_ATTR;
		pt = newpt;
	}

	if (pt->entry[l1idx] & PFL1_PRESENT) {
		if ((pt->entry[l1idx] & PT_PHYSMASK) != phys) {
			kprintf("EBUSY: page table for 0x%lX is already set\n", virt);
			ret = -EBUSY;
			goto out;
		} else {
			ret = 0;
			goto out;
		}
	}
	pt->entry[l1idx] = phys | attr_to_l1attr(attr);
	ret = 0;
out:
	if (in_kernel) {
		ihk_mc_spinlock_unlock(&init_pt_lock, init_pt_lock_flags);
	}
	return ret;
}

static int __clear_pt_page(struct page_table *pt, void *virt, int largepage)
{
	int l4idx, l3idx, l2idx, l1idx;
	unsigned long v = (unsigned long)virt;

	if (!pt) {
		pt = init_pt;
	}
	if (largepage) {
		v &= LARGE_PAGE_MASK;
	} else {
		v &= PAGE_MASK;
	}

	GET_VIRT_INDICES(v, l4idx, l3idx, l2idx, l1idx);

	if (!(pt->entry[l4idx] & PFL4_PRESENT)) {
		return -EINVAL;
	}
	pt = phys_to_virt(pt->entry[l4idx] & PAGE_MASK);

	if (!(pt->entry[l3idx] & PFL3_PRESENT)) {
		return -EINVAL;
	}
	pt = phys_to_virt(pt->entry[l3idx] & PAGE_MASK);

	if (largepage) {
		if (!(pt->entry[l2idx] & PFL2_PRESENT)) {
			return -EINVAL;
		} else {
			pt->entry[l2idx] = 0;
			return 0;
		}
	}
	
	if (!(pt->entry[l2idx] & PFL2_PRESENT)) {
		return -EINVAL;
	}

	pt = phys_to_virt(pt->entry[l2idx] & PAGE_MASK);

	pt->entry[l1idx] = 0;

	return 0;
}

uint64_t ihk_mc_pt_virt_to_pagemap(struct page_table *pt, unsigned long virt)
{
	int l4idx, l3idx, l2idx, l1idx;
	unsigned long v = (unsigned long)virt;
	uint64_t ret = 0;

	if (!pt) {
		pt = init_pt;
	}

	GET_VIRT_INDICES(v, l4idx, l3idx, l2idx, l1idx);

	if (!(pt->entry[l4idx] & PFL4_PRESENT)) {
		return ret;
	}
	pt = phys_to_virt(pt->entry[l4idx] & PAGE_MASK);

	if (!(pt->entry[l3idx] & PFL3_PRESENT)) {
		return ret;
	}
	pt = phys_to_virt(pt->entry[l3idx] & PAGE_MASK);

	if (!(pt->entry[l2idx] & PFL2_PRESENT)) {
		return ret;
	}
	if ((pt->entry[l2idx] & PFL2_SIZE)) {
		
		ret = PM_PFRAME(((pt->entry[l2idx] & LARGE_PAGE_MASK) + 
					(v & (LARGE_PAGE_SIZE - 1))) >> PAGE_SHIFT);
		ret |= PM_PSHIFT(PAGE_SHIFT) | PM_PRESENT;
		return ret;
	}
	pt = phys_to_virt(pt->entry[l2idx] & PAGE_MASK);

	if (!(pt->entry[l1idx] & PFL1_PRESENT)) {
		return ret;
	}

	ret = PM_PFRAME((pt->entry[l1idx] & PT_PHYSMASK) >> PAGE_SHIFT);
	ret |= PM_PSHIFT(PAGE_SHIFT) | PM_PRESENT;
	return ret;
}


int ihk_mc_pt_virt_to_phys(struct page_table *pt,
                           const void *virt, unsigned long *phys)
{
	int l4idx, l3idx, l2idx, l1idx;
	unsigned long v = (unsigned long)virt;

	if (!pt) {
		pt = init_pt;
	}

	GET_VIRT_INDICES(v, l4idx, l3idx, l2idx, l1idx);

	if (!(pt->entry[l4idx] & PFL4_PRESENT)) {
		return -EFAULT;
	}
	pt = phys_to_virt(pt->entry[l4idx] & PAGE_MASK);

	if (!(pt->entry[l3idx] & PFL3_PRESENT)) {
		return -EFAULT;
	}
	pt = phys_to_virt(pt->entry[l3idx] & PAGE_MASK);

	if (!(pt->entry[l2idx] & PFL2_PRESENT)) {
		return -EFAULT;
	}
	if ((pt->entry[l2idx] & PFL2_SIZE)) {
		*phys = (pt->entry[l2idx] & LARGE_PAGE_MASK) | 
			(v & (LARGE_PAGE_SIZE - 1));
		return 0;
	}
	pt = phys_to_virt(pt->entry[l2idx] & PAGE_MASK);

	if (!(pt->entry[l1idx] & PFL1_PRESENT)) {
		return -EFAULT;
	}

	*phys = (pt->entry[l1idx] & PT_PHYSMASK) | (v & (PAGE_SIZE - 1));
	return 0;
}

int ihk_mc_pt_print_pte(struct page_table *pt, void *virt)
{
	int l4idx, l3idx, l2idx, l1idx;
	unsigned long v = (unsigned long)virt;

	if (!pt) {
		pt = init_pt;
	}

	GET_VIRT_INDICES(v, l4idx, l3idx, l2idx, l1idx);

	if (!(pt->entry[l4idx] & PFL4_PRESENT)) {
		__kprintf("0x%lX l4idx not present! \n", (unsigned long)virt);
		__kprintf("l4 entry: 0x%lX\n", pt->entry[l4idx]);
		return -EFAULT;
	}
	pt = phys_to_virt(pt->entry[l4idx] & PAGE_MASK);

	__kprintf("l3 table: 0x%lX l3idx: %d \n", virt_to_phys(pt), l3idx);
	if (!(pt->entry[l3idx] & PFL3_PRESENT)) {
		__kprintf("0x%lX l3idx not present! \n", (unsigned long)virt);
		__kprintf("l3 entry: 0x%lX\n", pt->entry[l3idx]);
		return -EFAULT;
	}
	pt = phys_to_virt(pt->entry[l3idx] & PAGE_MASK);
	
	__kprintf("l2 table: 0x%lX l2idx: %d \n", virt_to_phys(pt), l2idx);
	if (!(pt->entry[l2idx] & PFL2_PRESENT)) {
		__kprintf("0x%lX l2idx not present! \n", (unsigned long)virt);
		__kprintf("l2 entry: 0x%lX\n", pt->entry[l2idx]);
		return -EFAULT;
	}
	if ((pt->entry[l2idx] & PFL2_SIZE)) {
		return 0;
	}
	pt = phys_to_virt(pt->entry[l2idx] & PAGE_MASK);

	__kprintf("l1 table: 0x%lX l1idx: %d \n", virt_to_phys(pt), l1idx);
	if (!(pt->entry[l1idx] & PFL1_PRESENT)) {
		__kprintf("0x%lX l1idx not present! \n", (unsigned long)virt);
		__kprintf("l1 entry: 0x%lX\n", pt->entry[l1idx]);
		return -EFAULT;
	}

	__kprintf("l1 entry: 0x%lX\n", pt->entry[l1idx]);
	return 0;
}

int set_pt_large_page(struct page_table *pt, void *virt, unsigned long phys,
                      enum ihk_mc_pt_attribute attr)
{
	return __set_pt_page(pt, virt, phys, attr | PTATTR_LARGEPAGE
	                     | PTATTR_ACTIVE);
}

int ihk_mc_pt_set_large_page(page_table_t pt, void *virt,
                       unsigned long phys, enum ihk_mc_pt_attribute attr)
{
	return __set_pt_page(pt, virt, phys, attr | PTATTR_LARGEPAGE
	                     | PTATTR_ACTIVE);
}

int ihk_mc_pt_set_page(page_table_t pt, void *virt,
                       unsigned long phys, enum ihk_mc_pt_attribute attr)
{
	return __set_pt_page(pt, virt, phys, attr | PTATTR_ACTIVE);
}

int ihk_mc_pt_prepare_map(page_table_t p, void *virt, unsigned long size,
                          enum ihk_mc_pt_prepare_flag flag)
{
	int l4idx, l4e, ret = 0;
	unsigned long v = (unsigned long)virt;
	struct page_table *pt = p, *newpt;
	unsigned long l;
	enum ihk_mc_pt_attribute attr = PTATTR_WRITABLE;

	if (!pt) {
		pt = init_pt;
	}

	l4idx = ((v) >> PTL4_SHIFT) & (PT_ENTRIES - 1);

	if (flag == IHK_MC_PT_FIRST_LEVEL) {
		l4e = ((v + size) >> PTL4_SHIFT)  & (PT_ENTRIES - 1);

		for (; l4idx <= l4e; l4idx++) {
			if (pt->entry[l4idx] & PFL4_PRESENT) {
				return 0;
			} else {
				newpt = __alloc_new_pt(IHK_MC_AP_CRITICAL);
				if (!newpt) {
					ret = -ENOMEM;
				} else { 
					pt->entry[l4idx] = virt_to_phys(newpt)
						| PFL4_PDIR_ATTR;
				}
			}
		}
	} else {
		/* Call without ACTIVE flag */
		l = v + size;
		for (; v < l; v += PAGE_SIZE) {
			if ((ret = __set_pt_page(pt, (void *)v, 0, attr))) {
				break;
			}
		}
	}
	return ret;
}

struct page_table *ihk_mc_pt_create(enum ihk_mc_ap_flag ap_flag)
{
	struct page_table *pt = ihk_mc_alloc_pages(1, ap_flag);

	if(pt == NULL)
		return NULL;

	memset(pt->entry, 0, PAGE_SIZE);
	/* Copy the kernel space */
	memcpy(pt->entry + PT_ENTRIES / 2, init_pt->entry + PT_ENTRIES / 2,
	       sizeof(pt->entry[0]) * PT_ENTRIES / 2);

	return pt;
}

static void destroy_page_table(int level, struct page_table *pt)
{
	int ix;
	unsigned long entry;
	struct page_table *lower;

	if ((level < 1) || (4 < level)) {
		panic("destroy_page_table: level is out of range");
	}
	if (pt == NULL) {
		panic("destroy_page_table: pt is NULL");
	}

	if (level > 1) {
		for (ix = 0; ix < PT_ENTRIES; ++ix) {
			entry = pt->entry[ix];
			if (!(entry & PF_PRESENT)) {
				/* entry is not valid */
				continue;
			}
			if (entry & PF_SIZE) {
				/* not a page table */
				continue;
			}
			lower = (struct page_table *)phys_to_virt(entry & PT_PHYSMASK);
			destroy_page_table(level-1, lower);
		}
	}

	arch_free_page(pt);
	return;
}

void ihk_mc_pt_destroy(struct page_table *pt)
{
	const int level = 4;	/* PML4 */

	/* clear shared entry */
	memset(pt->entry + PT_ENTRIES / 2, 0, sizeof(pt->entry[0]) * PT_ENTRIES / 2);

	destroy_page_table(level, pt);
	return;
}

int ihk_mc_pt_clear_page(page_table_t pt, void *virt)
{
	return __clear_pt_page(pt, virt, 0);
}

int ihk_mc_pt_clear_large_page(page_table_t pt, void *virt)
{
	return __clear_pt_page(pt, virt, 1);
}

typedef int walk_pte_fn_t(void *args, pte_t *ptep, uint64_t base,
		uint64_t start, uint64_t end);

static int walk_pte_l1(struct page_table *pt, uint64_t base, uint64_t start,
		uint64_t end, walk_pte_fn_t *funcp, void *args)
{
	int six;
	int eix;
	int ret;
	int i;
	int error;
	uint64_t off;

	six = (start <= base)? 0: ((start - base) >> PTL1_SHIFT);
	eix = ((end == 0) || ((base + PTL2_SIZE) <= end))? PT_ENTRIES
		: (((end - base) + (PTL1_SIZE - 1)) >> PTL1_SHIFT);

	ret = -ENOENT;
	for (i = six; i < eix; ++i) {
		off = i * PTL1_SIZE;
		error = (*funcp)(args, &pt->entry[i], base+off, start, end);
		if (!error) {
			ret = 0;
		}
		else if (error != -ENOENT) {
			ret = error;
			break;
		}
	}

	return ret;
}

static int walk_pte_l2(struct page_table *pt, uint64_t base, uint64_t start,
		uint64_t end, walk_pte_fn_t *funcp, void *args)
{
	int six;
	int eix;
	int ret;
	int i;
	int error;
	uint64_t off;

	six = (start <= base)? 0: ((start - base) >> PTL2_SHIFT);
	eix = ((end == 0) || ((base + PTL3_SIZE) <= end))? PT_ENTRIES
		: (((end - base) + (PTL2_SIZE - 1)) >> PTL2_SHIFT);

	ret = -ENOENT;
	for (i = six; i < eix; ++i) {
		off = i * PTL2_SIZE;
		error = (*funcp)(args, &pt->entry[i], base+off, start, end);
		if (!error) {
			ret = 0;
		}
		else if (error != -ENOENT) {
			ret = error;
			break;
		}
	}

	return ret;
}

static int walk_pte_l3(struct page_table *pt, uint64_t base, uint64_t start,
		uint64_t end, walk_pte_fn_t *funcp, void *args)
{
	int six;
	int eix;
	int ret;
	int i;
	int error;
	uint64_t off;

	six = (start <= base)? 0: ((start - base) >> PTL3_SHIFT);
	eix = ((end == 0) || ((base + PTL4_SIZE) <= end))? PT_ENTRIES
		: (((end - base) + (PTL3_SIZE - 1)) >> PTL3_SHIFT);

	ret = -ENOENT;
	for (i = six; i < eix; ++i) {
		off = i * PTL3_SIZE;
		error = (*funcp)(args, &pt->entry[i], base+off, start, end);
		if (!error) {
			ret = 0;
		}
		else if (error != -ENOENT) {
			ret = error;
			break;
		}
	}

	return ret;
}

static int walk_pte_l4(struct page_table *pt, uint64_t base, uint64_t start,
		uint64_t end, walk_pte_fn_t *funcp, void *args)
{
	int six;
	int eix;
	int ret;
	int i;
	int error;
	uint64_t off;

	six = (start <= base)? 0: ((start - base) >> PTL4_SHIFT);
	eix = (end == 0)? PT_ENTRIES
		:(((end - base) + (PTL4_SIZE - 1)) >> PTL4_SHIFT);

	ret = -ENOENT;
	for (i = six; i < eix; ++i) {
		off = i * PTL4_SIZE;
		error = (*funcp)(args, &pt->entry[i], base+off, start, end);
		if (!error) {
			ret = 0;
		}
		else if (error != -ENOENT) {
			ret = error;
			break;
		}
	}

	return ret;
}

static int split_large_page(pte_t *ptep)
{
	struct page_table *pt;
	uint64_t phys;
	pte_t attr;
	int i;

	pt = __alloc_new_pt(IHK_MC_AP_NOWAIT);
	if (pt == NULL) {
		ekprintf("split_large_page:__alloc_new_pt failed\n");
		return -ENOMEM;
	}

	if (!(*ptep & PFL2_FILEOFF)) {
		phys = *ptep & PT_PHYSMASK;
		attr = *ptep & ~PT_PHYSMASK;
		attr &= ~PFL2_SIZE;
	}
	else {
		phys = *ptep & PAGE_MASK;	/* file offset */
		attr = *ptep & ~PAGE_MASK;
		attr &= ~PFL2_SIZE;
	}

	for (i = 0; i < PT_ENTRIES; ++i) {
		pt->entry[i] = (phys + (i * PTL1_SIZE)) | attr;
	}

	*ptep = (virt_to_phys(pt) & PT_PHYSMASK) | PFL2_PDIR_ATTR;
	return 0;
}

struct visit_pte_args {
	page_table_t pt;
	enum visit_pte_flag flags;
	int padding;
	pte_visitor_t *funcp;
	void *arg;
};

static int visit_pte_l1(void *arg0, pte_t *ptep, uintptr_t base,
		uintptr_t start, uintptr_t end)
{
	struct visit_pte_args *args = arg0;

	if ((*ptep == PTE_NULL) && (args->flags & VPTEF_SKIP_NULL)) {
		return 0;
	}

	return (*args->funcp)(args->arg, args->pt, ptep, (void *)base,
			PTL1_SIZE);
}

static int visit_pte_l2(void *arg0, pte_t *ptep, uintptr_t base,
		uintptr_t start, uintptr_t end)
{
	int error;
	struct visit_pte_args *args = arg0;
	struct page_table *pt;

	if ((*ptep == PTE_NULL) && (args->flags & VPTEF_SKIP_NULL)) {
		return 0;
	}

#ifdef USE_LARGE_PAGES
	if (((*ptep == PTE_NULL) || (*ptep & PFL2_SIZE))
			&& (start <= base)
			&& (((base + PTL2_SIZE) <= end)
				|| (end == 0))) {
		error = (*args->funcp)(args->arg, args->pt, ptep,
				(void *)base, PTL2_SIZE);
		if (error != -E2BIG) {
			return error;
		}
	}

	if (*ptep & PFL2_SIZE) {
		ekprintf("visit_pte_l2:split large page\n");
		return -ENOMEM;
	}
#endif

	if (*ptep == PTE_NULL) {
		pt = __alloc_new_pt(IHK_MC_AP_NOWAIT);
		if (!pt) {
			return -ENOMEM;
		}
		*ptep = virt_to_phys(pt) | PFL2_PDIR_ATTR;
	}
	else {
		pt = phys_to_virt(*ptep & PT_PHYSMASK);
	}

	error = walk_pte_l1(pt, base, start, end, &visit_pte_l1, arg0);
	return error;
}

static int visit_pte_l3(void *arg0, pte_t *ptep, uintptr_t base,
		uintptr_t start, uintptr_t end)
{
	int error;
	struct visit_pte_args *args = arg0;
	struct page_table *pt;

	if ((*ptep == PTE_NULL) && (args->flags & VPTEF_SKIP_NULL)) {
		return 0;
	}

#ifdef USE_LARGE_PAGES
	if (((*ptep == PTE_NULL) || (*ptep & PFL3_SIZE))
			&& (start <= base)
			&& (((base + PTL3_SIZE) <= end)
				|| (end == 0))) {
		error = (*args->funcp)(args->arg, args->pt, ptep,
				(void *)base, PTL3_SIZE);
		if (error != -E2BIG) {
			return error;
		}
	}

	if (*ptep & PFL3_SIZE) {
		ekprintf("visit_pte_l3:split large page\n");
		return -ENOMEM;
	}
#endif

	if (*ptep == PTE_NULL) {
		pt = __alloc_new_pt(IHK_MC_AP_NOWAIT);
		if (!pt) {
			return -ENOMEM;
		}
		*ptep = virt_to_phys(pt) | PFL3_PDIR_ATTR;
	}
	else {
		pt = phys_to_virt(*ptep & PT_PHYSMASK);
	}

	error = walk_pte_l2(pt, base, start, end, &visit_pte_l2, arg0);
	return error;
}

static int visit_pte_l4(void *arg0, pte_t *ptep, uintptr_t base,
		uintptr_t start, uintptr_t end)
{
	int error;
	struct visit_pte_args *args = arg0;
	struct page_table *pt;

	if ((*ptep == PTE_NULL) && (args->flags & VPTEF_SKIP_NULL)) {
		return 0;
	}

	if (*ptep == PTE_NULL) {
		pt = __alloc_new_pt(IHK_MC_AP_NOWAIT);
		if (!pt) {
			return -ENOMEM;
		}
		*ptep = virt_to_phys(pt) | PFL4_PDIR_ATTR;
	}
	else {
		pt = phys_to_virt(*ptep & PT_PHYSMASK);
	}

	error = walk_pte_l3(pt, base, start, end, &visit_pte_l3, arg0);
	return error;
}

int visit_pte_range(page_table_t pt, void *start0, void *end0,
		enum visit_pte_flag flags, pte_visitor_t *funcp, void *arg)
{
	const uintptr_t start = (uintptr_t)start0;
	const uintptr_t end = (uintptr_t)end0;
	struct visit_pte_args args;

	args.pt = pt;
	args.flags = flags;
	args.funcp = funcp;
	args.arg = arg;

	return walk_pte_l4(pt, 0, start, end, &visit_pte_l4, &args);
}

struct clear_range_args {
	int free_physical;
	uint8_t padding[4];
	struct memobj *memobj;
	struct process_vm *vm;
};

static int clear_range_l1(void *args0, pte_t *ptep, uint64_t base,
		uint64_t start, uint64_t end)
{
	struct clear_range_args *args = args0;
	uint64_t phys;
	struct page *page;
	pte_t old;

	if (*ptep == PTE_NULL) {
		return -ENOENT;
	}

	phys = *ptep & PT_PHYSMASK;
	old = xchg(ptep, PTE_NULL);

	if ((old & PFL1_DIRTY) && args->memobj) {
		memobj_flush_page(args->memobj, phys, PTL1_SIZE);
	}

	if (!(old & PFL1_FILEOFF) && args->free_physical) {
		page = phys_to_page(phys);
		if (page && page_unmap(page)) {
			ihk_mc_free_pages(phys_to_virt(phys), 1);
		}
	}
	
	remote_flush_tlb_cpumask(args->vm, base, ihk_mc_get_processor_id());

	return 0;
}

static int clear_range_l2(void *args0, pte_t *ptep, uint64_t base,
		uint64_t start, uint64_t end)
{
	struct clear_range_args *args = args0;
	uint64_t phys;
	struct page_table *pt;
	int error;
	struct page *page;
	pte_t old;

	if (*ptep == PTE_NULL) {
		return -ENOENT;
	}

	if ((*ptep & PFL2_SIZE)
			&& ((base < start) || (end < (base + PTL2_SIZE)))) {
		error = split_large_page(ptep);
		if (error) {
			ekprintf("clear_range_l2(%p,%p,%lx,%lx,%lx):"
					"split failed. %d\n",
					args0, ptep, base, start, end, error);
			return error;
		}
		if (*ptep & PFL2_SIZE) {
			panic("clear_range_l2:split");
		}
	}

	if (*ptep & PFL2_SIZE) {
		phys = *ptep & PT_PHYSMASK;
		old = xchg(ptep, PTE_NULL);

		if ((old & PFL2_DIRTY) && args->memobj) {
			memobj_flush_page(args->memobj, phys, PTL2_SIZE);
		}

		if (!(old & PFL2_FILEOFF) && args->free_physical) {
			page = phys_to_page(phys);
			if (page && page_unmap(page)) {
				ihk_mc_free_pages(phys_to_virt(phys), PTL2_SIZE/PTL1_SIZE);
			}
		}

		remote_flush_tlb_cpumask(args->vm, base, ihk_mc_get_processor_id());

		return 0;
	}

	pt = phys_to_virt(*ptep & PT_PHYSMASK);
	error = walk_pte_l1(pt, base, start, end, &clear_range_l1, args0);
	if (error && (error != -ENOENT)) {
		return error;
	}

	if ((start <= base) && ((base + PTL2_SIZE) <= end)) {
		*ptep = PTE_NULL;
		arch_free_page(pt);
	}

	return 0;
}

static int clear_range_l3(void *args0, pte_t *ptep, uint64_t base,
		uint64_t start, uint64_t end)
{
	struct page_table *pt;

	if (*ptep == PTE_NULL) {
		return -ENOENT;
	}

	pt = phys_to_virt(*ptep & PT_PHYSMASK);
	return walk_pte_l2(pt, base, start, end, &clear_range_l2, args0);
}

static int clear_range_l4(void *args0, pte_t *ptep, uint64_t base,
		uint64_t start, uint64_t end)
{
	struct page_table *pt;

	if (*ptep == PTE_NULL) {
		return -ENOENT;
	}

	pt = phys_to_virt(*ptep & PT_PHYSMASK);
	return walk_pte_l3(pt, base, start, end, &clear_range_l3, args0);
}

static int clear_range(struct page_table *pt, struct process_vm *vm, 
		uintptr_t start, uintptr_t end, int free_physical, 
		struct memobj *memobj)
{
	int error;
	struct clear_range_args args;

	if ((USER_END <= start) || (USER_END < end) || (end <= start)) {
		ekprintf("clear_range(%p,%p,%p,%x):"
				"invalid start and/or end.\n",
				pt, start, end, free_physical);
		return -EINVAL;
	}

	args.free_physical = free_physical;
	args.memobj = memobj;
	args.vm = vm;

	error = walk_pte_l4(pt, 0, start, end, &clear_range_l4, &args);
	return error;
}

int ihk_mc_pt_clear_range(page_table_t pt, struct process_vm *vm, 
		void *start, void *end)
{
#define	KEEP_PHYSICAL	0
	return clear_range(pt, vm, (uintptr_t)start, (uintptr_t)end,
			KEEP_PHYSICAL, NULL);
}

int ihk_mc_pt_free_range(page_table_t pt, struct process_vm *vm, 
		void *start, void *end, struct memobj *memobj)
{
#define	FREE_PHYSICAL	1
	return clear_range(pt, vm, (uintptr_t)start, (uintptr_t)end,
			FREE_PHYSICAL, memobj);
}

struct change_attr_args {
	pte_t clrpte;
	pte_t setpte;
};

static int change_attr_range_l1(void *arg0, pte_t *ptep, uint64_t base,
		uint64_t start, uint64_t end)
{
	struct change_attr_args *args = arg0;

	if ((*ptep == PTE_NULL) || (*ptep & PFL1_FILEOFF)) {
		return -ENOENT;
	}

	*ptep = (*ptep & ~args->clrpte) | args->setpte;
	return 0;
}

static int change_attr_range_l2(void *arg0, pte_t *ptep, uint64_t base,
		uint64_t start, uint64_t end)
{
	struct change_attr_args *args = arg0;
	int error;
	struct page_table *pt;

	if ((*ptep == PTE_NULL) || (*ptep & PFL2_FILEOFF)) {
		return -ENOENT;
	}

	if ((*ptep & PFL2_SIZE)
			&& ((base < start) || (end < (base + PTL2_SIZE)))) {
		error = split_large_page(ptep);
		if (error) {
			ekprintf("change_attr_range_l2(%p,%p,%lx,%lx,%lx):"
					"split failed. %d\n",
					arg0, ptep, base, start, end, error);
			return error;
		}
		if (*ptep & PFL2_SIZE) {
			panic("change_attr_range_l2:split");
		}
	}

	if (*ptep & PFL2_SIZE) {
		if (!(*ptep & PFL2_FILEOFF)) {
			*ptep = (*ptep & ~args->clrpte) | args->setpte;
		}
		return 0;
	}

	pt = phys_to_virt(*ptep & PT_PHYSMASK);
	return walk_pte_l1(pt, base, start, end, &change_attr_range_l1, arg0);
}

static int change_attr_range_l3(void *arg0, pte_t *ptep, uint64_t base,
		uint64_t start, uint64_t end)
{
	struct page_table *pt;

	if ((*ptep == PTE_NULL) || (*ptep & PFL3_FILEOFF)) {
		return -ENOENT;
	}

	pt = phys_to_virt(*ptep & PT_PHYSMASK);
	return walk_pte_l2(pt, base, start, end, &change_attr_range_l2, arg0);
}

static int change_attr_range_l4(void *arg0, pte_t *ptep, uint64_t base,
		uint64_t start, uint64_t end)
{
	struct page_table *pt;

	if (*ptep == PTE_NULL) {
		return -ENOENT;
	}

	pt = phys_to_virt(*ptep & PT_PHYSMASK);
	return walk_pte_l3(pt, base, start, end, &change_attr_range_l3, arg0);
}

int ihk_mc_pt_change_attr_range(page_table_t pt, void *start0, void *end0,
		enum ihk_mc_pt_attribute clrattr,
		enum ihk_mc_pt_attribute setattr)
{
	const intptr_t start = (intptr_t)start0;
	const intptr_t end = (intptr_t)end0;
	struct change_attr_args args;

	args.clrpte = attr_to_l1attr(clrattr);
	args.setpte = attr_to_l1attr(setattr);
	return walk_pte_l4(pt, 0, start, end, &change_attr_range_l4, &args);
}

static int alloc_range_l1(void *arg0, pte_t *ptep, uint64_t base,
		uint64_t start, uint64_t end)
{
	enum ihk_mc_pt_attribute *attrp = arg0;
	void *vp;

	if (*ptep == PTE_NULL) {
		/* not mapped */
		vp = ihk_mc_alloc_pages(1, IHK_MC_AP_NOWAIT);
		if (vp == NULL) {
			return -ENOMEM;
		}
		memset(vp, 0, PTL1_SIZE);

		*ptep = virt_to_phys(vp) | attr_to_l1attr(*attrp);
	}
	else if (!(*ptep & PFL1_PRESENT)) {
		kprintf("alloc_range_l1(%p,%p,%lx,%lx,%lx):inactive %lx\n",
				arg0, ptep, base, start, end, *ptep);
		return -EBUSY;
	}

	return 0;
}

static int alloc_range_l2(void *arg0, pte_t *ptep, uint64_t base,
		uint64_t start, uint64_t end)
{
	struct page_table *pt;
#ifdef USE_LARGE_PAGES
	enum ihk_mc_pt_attribute *attrp = arg0;
	void *vp;
#endif /* USE_LARGE_PAGES */

	if (*ptep != PTE_NULL) {
		if (!(*ptep & PFL2_PRESENT)) {
			kprintf("alloc_range_l2(%p,%p,%lx,%lx,%lx):inactive %lx\n",
					arg0, ptep, base, start, end, *ptep);
			return -EBUSY;
		}
		else if (*ptep & PFL2_SIZE) {
			return 0;
		}

		pt = phys_to_virt(*ptep & PT_PHYSMASK);
	}
	else {
#ifdef USE_LARGE_PAGES
		if ((start <= base) && ((base + PTL2_SIZE) <= end)) {
			vp = ihk_mc_alloc_aligned_pages(LARGE_PAGE_SIZE/PAGE_SIZE,
					LARGE_PAGE_P2ALIGN, IHK_MC_AP_NOWAIT);
			if (vp != NULL) {
				memset(vp, 0, PTL2_SIZE);

				*ptep = virt_to_phys(vp)
					| attr_to_l2attr(*attrp | PTATTR_LARGEPAGE);
				return 0;
			}
		}
#endif /* USE_LARGE_PAGES */
		pt = __alloc_new_pt(IHK_MC_AP_NOWAIT);
		if (pt == NULL) {
			return -ENOMEM;
		}

		*ptep = virt_to_phys(pt) | PFL2_PDIR_ATTR;
	}

	return walk_pte_l1(pt, base, start, end, &alloc_range_l1, arg0);
}

static int alloc_range_l3(void *arg0, pte_t *ptep, uint64_t base,
		uint64_t start, uint64_t end)
{
	struct page_table *pt;

	if (*ptep != PTE_NULL) {
		if (!(*ptep & PFL3_PRESENT)) {
			kprintf("alloc_range_l3(%p,%p,%lx,%lx,%lx):inactive %lx\n",
					arg0, ptep, base, start, end, *ptep);
			panic("alloc_range_l3:inactive");
		}
		pt = phys_to_virt(*ptep & PT_PHYSMASK);
	}
	else {
		pt = __alloc_new_pt(IHK_MC_AP_NOWAIT);
		if (pt == NULL) {
			return -ENOMEM;
		}
		*ptep = virt_to_phys(pt) | PFL3_PDIR_ATTR;
	}

	return walk_pte_l2(pt, base, start, end, &alloc_range_l2, arg0);
}

static int alloc_range_l4(void *arg0, pte_t *ptep, uint64_t base,
		uint64_t start, uint64_t end)
{
	struct page_table *pt;

	if (*ptep != PTE_NULL) {
		if (!(*ptep & PFL4_PRESENT)) {
			kprintf("alloc_range_l4(%p,%p,%lx,%lx,%lx):inactive %lx\n",
					arg0, ptep, base, start, end, *ptep);
			panic("alloc_range_l4:inactive");
		}
		pt = phys_to_virt(*ptep & PT_PHYSMASK);
	}
	else {
		pt = __alloc_new_pt(IHK_MC_AP_NOWAIT);
		if (pt == NULL) {
			return -ENOMEM;
		}
		*ptep = virt_to_phys(pt) | PFL4_PDIR_ATTR;
	}

	return walk_pte_l3(pt, base, start, end, &alloc_range_l3, arg0);
}

int ihk_mc_pt_alloc_range(page_table_t pt, void *start, void *end,
		enum ihk_mc_pt_attribute attr)
{
	return walk_pte_l4(pt, 0, (intptr_t)start, (intptr_t)end,
			&alloc_range_l4, &attr);
}

static pte_t *lookup_pte(struct page_table *pt, uintptr_t virt,
		uintptr_t *basep, size_t *sizep, int *p2alignp)
{
	int l4idx, l3idx, l2idx, l1idx;
	pte_t *ptep;
	uintptr_t base;
	size_t size;
	int p2align;

	GET_VIRT_INDICES(virt, l4idx, l3idx, l2idx, l1idx);

#ifdef USE_LARGE_PAGES
	if (use_1gb_page) {
		ptep = NULL;
		base = GET_INDICES_VIRT(l4idx, 0, 0, 0);
		size = PTL3_SIZE;
		p2align = PTL3_SHIFT - PTL1_SHIFT;
	}
	else {
		ptep = NULL;
		base = GET_INDICES_VIRT(l4idx, l3idx, 0, 0);
		size = PTL2_SIZE;
		p2align = PTL2_SHIFT - PTL1_SHIFT;
	}
#else
	ptep = NULL;
	base = GET_INDICES_VIRT(l4idx, l3idx, l2idx, l1idx);
	size = PTL1_SIZE;
	p2align = PTL1_SHIFT - PTL1_SHIFT;
#endif

	if (pt->entry[l4idx] == PTE_NULL) {
		goto out;
	}

	pt = phys_to_virt(pt->entry[l4idx] & PT_PHYSMASK);
	if ((pt->entry[l3idx] == PTE_NULL)
			|| (pt->entry[l3idx] & PFL3_SIZE)) {
#ifdef USE_LARGE_PAGES
		if (use_1gb_page) {
			ptep = &pt->entry[l3idx];
			base = GET_INDICES_VIRT(l4idx, l3idx, 0, 0);
			size = PTL3_SIZE;
			p2align = PTL3_SHIFT - PTL1_SHIFT;
		}
#endif
		goto out;
	}

	pt = phys_to_virt(pt->entry[l3idx] & PT_PHYSMASK);
	if ((pt->entry[l2idx] == PTE_NULL)
			|| (pt->entry[l2idx] & PFL2_SIZE)) {
#ifdef USE_LARGE_PAGES
		ptep = &pt->entry[l2idx];
		base = GET_INDICES_VIRT(l4idx, l3idx, l2idx, 0);
		size = PTL2_SIZE;
		p2align = PTL2_SHIFT - PTL1_SHIFT;
#endif
		goto out;
	}

	pt = phys_to_virt(pt->entry[l2idx] & PT_PHYSMASK);
	ptep = &pt->entry[l1idx];
	base = GET_INDICES_VIRT(l4idx, l3idx, l2idx, l1idx);
	size = PTL1_SIZE;
	p2align = PTL1_SHIFT - PTL1_SHIFT;

out:
	if (basep) *basep = base;
	if (sizep) *sizep = size;
	if (p2alignp) *p2alignp = p2align;

	return ptep;
}

pte_t *ihk_mc_pt_lookup_pte(page_table_t pt, void *virt, void **basep,
		size_t *sizep, int *p2alignp)
{
	pte_t *ptep;
	uintptr_t base;
	size_t size;
	int p2align;

	dkprintf("ihk_mc_pt_lookup_pte(%p,%p)\n", pt, virt);
	ptep = lookup_pte(pt, (uintptr_t)virt, &base, &size, &p2align);
	if (basep) *basep = (void *)base;
	if (sizep) *sizep = size;
	if (p2alignp) *p2alignp = p2align;
	dkprintf("ihk_mc_pt_lookup_pte(%p,%p): %p %lx %lx %d\n",
			pt, virt, ptep, base, size, p2align);
	return ptep;
}

struct set_range_args {
	page_table_t pt;
	uintptr_t phys;
	enum ihk_mc_pt_attribute attr;
	int padding;
	uintptr_t diff;
	struct process_vm *vm;
};

int set_range_l1(void *args0, pte_t *ptep, uintptr_t base, uintptr_t start,
		uintptr_t end)
{
	struct set_range_args *args = args0;
	int error;
	uintptr_t phys;

	dkprintf("set_range_l1(%lx,%lx,%lx)\n", base, start, end);

	if (*ptep != PTE_NULL) {
		error = -EBUSY;
		ekprintf("set_range_l1(%lx,%lx,%lx):page exists. %d %lx\n",
				base, start, end, error, *ptep);
		(void)clear_range(args->pt, args->vm, start, base, KEEP_PHYSICAL, NULL);
		goto out;
	}

	phys = args->phys + (base - start);
	*ptep = phys | attr_to_l1attr(args->attr);

	error = 0;
out:
	dkprintf("set_range_l1(%lx,%lx,%lx): %d %lx\n",
			base, start, end, error, *ptep);
	return error;
}

int set_range_l2(void *args0, pte_t *ptep, uintptr_t base, uintptr_t start,
		uintptr_t end)
{
	struct set_range_args *args = args0;
	int error;
	struct page_table *pt;
#ifdef USE_LARGE_PAGES
	uintptr_t phys;
#endif

	dkprintf("set_range_l2(%lx,%lx,%lx)\n", base, start, end);

	if (*ptep == PTE_NULL) {
#ifdef USE_LARGE_PAGES
		if ((start <= base) && ((base + PTL2_SIZE) <= end)
				&& ((args->diff & (PTL2_SIZE - 1)) == 0)) {
			phys = args->phys + (base - start);
			*ptep = phys | attr_to_l2attr(
					args->attr|PTATTR_LARGEPAGE);
			error = 0;
			dkprintf("set_range_l2(%lx,%lx,%lx):"
					"large page. %d %lx\n",
					base, start, end, error, *ptep);
			goto out;
		}
#endif

		pt = __alloc_new_pt(IHK_MC_AP_NOWAIT);
		if (pt == NULL) {
			error = -ENOMEM;
			ekprintf("set_range_l2(%lx,%lx,%lx):"
					"__alloc_new_pt failed. %d %lx\n",
					base, start, end, error, *ptep);
			(void)clear_range(args->pt, args->vm, start, base,
					KEEP_PHYSICAL, NULL);
			goto out;
		}

		*ptep = virt_to_phys(pt) | PFL2_PDIR_ATTR;
	}
	else if (*ptep & PFL2_SIZE) {
		error = -EBUSY;
		ekprintf("set_range_l2(%lx,%lx,%lx):"
				"page exists. %d %lx\n",
				base, start, end, error, *ptep);
		(void)clear_range(args->pt, args->vm, start, base, KEEP_PHYSICAL, NULL);
		goto out;
	}
	else {
		pt = phys_to_virt(*ptep & PT_PHYSMASK);
	}

	error = walk_pte_l1(pt, base, start, end, &set_range_l1, args0);
	if (error) {
		ekprintf("set_range_l2(%lx,%lx,%lx):"
				"walk_pte_l1 failed. %d %lx\n",
				base, start, end, error, *ptep);
		goto out;
	}

	error = 0;
out:
	dkprintf("set_range_l2(%lx,%lx,%lx): %d %lx\n",
			base, start, end, error, *ptep);
	return error;
}

int set_range_l3(void *args0, pte_t *ptep, uintptr_t base, uintptr_t start,
		uintptr_t end)
{
	struct set_range_args *args = args0;
	struct page_table *pt;
	int error;
#ifdef USE_LARGE_PAGES
	uintptr_t phys;
#endif

	dkprintf("set_range_l3(%lx,%lx,%lx)\n", base, start, end);

	if (*ptep == PTE_NULL) {
#ifdef USE_LARGE_PAGES
		if ((start <= base) && ((base + PTL3_SIZE) <= end)
				&& ((args->diff & (PTL3_SIZE - 1)) == 0)
				&& use_1gb_page) {
			phys = args->phys + (base - start);
			*ptep = phys | attr_to_l3attr(
					args->attr|PTATTR_LARGEPAGE);
			error = 0;
			dkprintf("set_range_l3(%lx,%lx,%lx):"
					"1GiB page. %d %lx\n",
					base, start, end, error, *ptep);
			goto out;
		}
#endif

		pt = __alloc_new_pt(IHK_MC_AP_NOWAIT);
		if (pt == NULL) {
			error = -ENOMEM;
			ekprintf("set_range_l3(%lx,%lx,%lx):"
					"__alloc_new_pt failed. %d %lx\n",
					base, start, end, error, *ptep);
			(void)clear_range(args->pt, args->vm, start, base,
					KEEP_PHYSICAL, NULL);
			goto out;
		}
		*ptep = virt_to_phys(pt) | PFL3_PDIR_ATTR;
	}
	else if (*ptep & PFL3_SIZE) {
		error = -EBUSY;
		ekprintf("set_range_l3(%lx,%lx,%lx):"
				"page exists. %d %lx\n",
				base, start, end, error, *ptep);
		(void)clear_range(args->pt, args->vm, start, base, KEEP_PHYSICAL, NULL);
		goto out;
	}
	else {
		pt = phys_to_virt(*ptep & PT_PHYSMASK);
	}

	error = walk_pte_l2(pt, base, start, end, &set_range_l2, args0);
	if (error) {
		ekprintf("set_range_l3(%lx,%lx,%lx):"
				"walk_pte_l2 failed. %d %lx\n",
				base, start, end, error, *ptep);
		goto out;
	}

	error = 0;
out:
	dkprintf("set_range_l3(%lx,%lx,%lx): %d\n",
			base, start, end, error, *ptep);
	return error;
}

int set_range_l4(void *args0, pte_t *ptep, uintptr_t base, uintptr_t start,
		uintptr_t end)
{
	struct set_range_args *args = args0;
	struct page_table *pt;
	int error;

	dkprintf("set_range_l4(%lx,%lx,%lx)\n", base, start, end);

	if (*ptep == PTE_NULL) {
		pt = __alloc_new_pt(IHK_MC_AP_NOWAIT);
		if (pt == NULL) {
			error = -ENOMEM;
			ekprintf("set_range_l4(%lx,%lx,%lx):"
					"__alloc_new_pt failed. %d %lx\n",
					base, start, end, error, *ptep);
			(void)clear_range(args->pt, args->vm, start, base,
					KEEP_PHYSICAL, NULL);
			goto out;
		}
		*ptep = virt_to_phys(pt) | PFL4_PDIR_ATTR;
	}
	else {
		pt = phys_to_virt(*ptep & PT_PHYSMASK);
	}

	error =  walk_pte_l3(pt, base, start, end, &set_range_l3, args0);
	if (error) {
		ekprintf("set_range_l4(%lx,%lx,%lx):"
				"walk_pte_l3 failed. %d %lx\n",
				base, start, end, error, *ptep);
		goto out;
	}

	error = 0;
out:
	dkprintf("set_range_l4(%lx,%lx,%lx): %d %lx\n",
			base, start, end, error, *ptep);
	return error;
}

int ihk_mc_pt_set_range(page_table_t pt, struct process_vm *vm, void *start, 
		void *end, uintptr_t phys, enum ihk_mc_pt_attribute attr)
{
	int error;
	struct set_range_args args;

	dkprintf("ihk_mc_pt_set_range(%p,%p,%p,%lx,%x)\n",
			pt, start, end, phys, attr);

	args.pt = pt;
	args.phys = phys;
	args.attr = attr;
	args.diff = (uintptr_t)start ^ phys;
	args.vm = vm;

	error = walk_pte_l4(pt, 0, (uintptr_t)start, (uintptr_t)end,
			&set_range_l4, &args);
	if (error) {
		ekprintf("ihk_mc_pt_set_range(%p,%p,%p,%lx,%x):"
				"walk_pte_l4 failed. %d\n",
				pt, start, end, phys, attr, error);
		goto out;
	}

	error = 0;
out:
	dkprintf("ihk_mc_pt_set_range(%p,%p,%p,%lx,%x): %d\n",
			pt, start, end, phys, attr, error);
	return error;
}

int ihk_mc_pt_set_pte(page_table_t pt, pte_t *ptep, size_t pgsize,
		uintptr_t phys, enum ihk_mc_pt_attribute attr)
{
	int error;

	dkprintf("ihk_mc_pt_set_pte(%p,%p,%lx,%lx,%x)\n",
			pt, ptep, pgsize, phys, attr);

	if (pgsize == PTL1_SIZE) {
		*ptep = phys | attr_to_l1attr(attr);
	}
#ifdef USE_LARGE_PAGES
	else if (pgsize == PTL2_SIZE) {
		*ptep = phys | attr_to_l2attr(attr | PTATTR_LARGEPAGE);
	}
	else if ((pgsize == PTL3_SIZE) && (use_1gb_page)) {
		*ptep = phys | attr_to_l3attr(attr | PTATTR_LARGEPAGE);
	}
#endif
	else {
		error = -EINVAL;
		ekprintf("ihk_mc_pt_set_pte(%p,%p,%lx,%lx,%x):"
				"page size. %d %lx\n",
				pt, ptep, pgsize, phys, attr, error, *ptep);
		panic("ihk_mc_pt_set_pte:page size");
		goto out;
	}

	error = 0;
out:
	dkprintf("ihk_mc_pt_set_pte(%p,%p,%lx,%lx,%x): %d %lx\n",
			pt, ptep, pgsize, phys, attr, error, *ptep);
	return error;
}

int arch_get_smaller_page_size(void *args, size_t cursize, size_t *newsizep,
		int *p2alignp)
{
	size_t newsize;
	int p2align;
	int error;

	if (0) {
		/* dummy */
		panic("not reached");
	}
#ifdef USE_LARGE_PAGES
	else if ((cursize > PTL3_SIZE) && use_1gb_page) {
		/* 1GiB */
		newsize = PTL3_SIZE;
		p2align = PTL3_SHIFT - PTL1_SHIFT;
	}
	else if (cursize > PTL2_SIZE) {
		/* 2MiB */
		newsize = PTL2_SIZE;
		p2align = PTL2_SHIFT - PTL1_SHIFT;
	}
#endif
	else if (cursize > PTL1_SIZE) {
		/* 4KiB : basic page size */
		newsize = PTL1_SIZE;
		p2align = PTL1_SHIFT - PTL1_SHIFT;
	}
	else {
		error = -ENOMEM;
		newsize = 0;
		p2align = -1;
		goto out;
	}

	error = 0;
	if (newsizep) *newsizep = newsize;
	if (p2alignp) *p2alignp = p2align;

out:
	dkprintf("arch_get_smaller_page_size(%p,%lx): %d %lx %d\n",
			args, cursize, error, newsize, p2align);
	return error;
}

enum ihk_mc_pt_attribute arch_vrflag_to_ptattr(unsigned long flag, uint64_t fault, pte_t *ptep)
{
	enum ihk_mc_pt_attribute attr;

	attr = common_vrflag_to_ptattr(flag, fault, ptep);

	if ((fault & PF_PROT)
			|| ((fault & PF_POPULATE) && (flag & VR_PRIVATE))) {
		attr |= PTATTR_DIRTY;
	}

	return attr;
}

struct move_args {
	uintptr_t src;
	uintptr_t dest;
	struct process_vm *vm;
};

static int move_one_page(void *arg0, page_table_t pt, pte_t *ptep, 
		void *pgaddr, size_t pgsize)
{
	int error;
	struct move_args *args = arg0;
	uintptr_t dest;
	pte_t apte;
	uintptr_t phys;
	enum ihk_mc_pt_attribute attr;

	dkprintf("move_one_page(%p,%p,%p %#lx,%p,%#lx)\n",
			arg0, pt, ptep, *ptep, pgaddr, pgsize);
	if (pte_is_fileoff(ptep, pgsize)) {
		error = -ENOTSUPP;
		kprintf("move_one_page(%p,%p,%p %#lx,%p,%#lx):fileoff. %d\n",
				arg0, pt, ptep, *ptep, pgaddr, pgsize, error);
		goto out;
	}

	dest = args->dest + ((uintptr_t)pgaddr - args->src);

	apte = PTE_NULL;
	pte_xchg(ptep, &apte);

	phys = apte & PT_PHYSMASK;
	attr = apte & ~PT_PHYSMASK;

	error = ihk_mc_pt_set_range(pt, args->vm, (void *)dest,
			(void *)(dest + pgsize), phys, attr);
	if (error) {
		kprintf("move_one_page(%p,%p,%p %#lx,%p,%#lx):"
				"set failed. %d\n",
				arg0, pt, ptep, *ptep, pgaddr, pgsize, error);
		goto out;
	}

	error = 0;
out:
	dkprintf("move_one_page(%p,%p,%p %#lx,%p,%#lx):%d\n",
			arg0, pt, ptep, *ptep, pgaddr, pgsize, error);
	return error;
}

int move_pte_range(page_table_t pt, struct process_vm *vm, 
		void *src, void *dest, size_t size)
{
	int error;
	struct move_args args;

	dkprintf("move_pte_range(%p,%p,%p,%#lx)\n", pt, src, dest, size);
	args.src = (uintptr_t)src;
	args.dest = (uintptr_t)dest;
	args.vm = vm;

	error = visit_pte_range(pt, src, src+size, VPTEF_SKIP_NULL,
			&move_one_page, &args);
	flush_tlb();	/* XXX: TLB flush */
	if (error) {
		goto out;
	}

	error = 0;
out:
	dkprintf("move_pte_range(%p,%p,%p,%#lx):%d\n",
			pt, src, dest, size, error);
	return error;
}

void load_page_table(struct page_table *pt)
{
	unsigned long pt_addr;

	if (!pt) {
		pt = init_pt;
	}

	pt_addr = virt_to_phys(pt);

	asm volatile ("movq %0, %%cr3" : : "r"(pt_addr) : "memory");
}

void ihk_mc_load_page_table(struct page_table *pt)
{
	load_page_table(pt);
}

struct page_table *get_init_page_table(void)
{
	return init_pt;
}

static unsigned long fixed_virt;
static void init_fixed_area(struct page_table *pt)
{
	fixed_virt = MAP_FIXED_START;

	return;
}

void init_text_area(struct page_table *pt)
{
	unsigned long __end, phys, virt;
	int i, nlpages;

	__end = ((unsigned long)_end + LARGE_PAGE_SIZE * 2 - 1)
		& LARGE_PAGE_MASK;
	nlpages = (__end - MAP_KERNEL_START) >> LARGE_PAGE_SHIFT;

	kprintf("TEXT: # of large pages = %d\n", nlpages);
	kprintf("TEXT: Base address = %lx\n", x86_kernel_phys_base);

	phys = x86_kernel_phys_base;
	virt = MAP_KERNEL_START;
	for (i = 0; i < nlpages; i++) {
		set_pt_large_page(pt, (void *)virt, phys, PTATTR_WRITABLE);

		virt += LARGE_PAGE_SIZE;
		phys += LARGE_PAGE_SIZE;
	}
}

void *map_fixed_area(unsigned long phys, unsigned long size, int uncachable)
{
	unsigned long poffset, paligned;
	int i, npages;
	void *v = (void *)fixed_virt;
	enum ihk_mc_pt_attribute attr;

	poffset = phys & (PAGE_SIZE - 1);
	paligned = phys & PAGE_MASK;
	npages = (poffset + size + PAGE_SIZE - 1) >> PAGE_SHIFT;

	attr = PTATTR_WRITABLE | PTATTR_ACTIVE;
#if 0	/* In the case of LAPIC MMIO, something will happen */
	attr |= PTATTR_NO_EXECUTE;
#endif
	if (uncachable) {
		attr |= PTATTR_UNCACHABLE;
	}

	kprintf("map_fixed: %lx => %p (%d pages)\n", paligned, v, npages);

	for (i = 0; i < npages; i++) {
		if(__set_pt_page(init_pt, (void *)fixed_virt, paligned, attr)){
			return NULL;
		}

		fixed_virt += PAGE_SIZE;
		paligned += PAGE_SIZE;
	}
	
	flush_tlb();

	return (char *)v + poffset;
}

void init_low_area(struct page_table *pt)
{
	set_pt_large_page(pt, 0, 0, PTATTR_NO_EXECUTE|PTATTR_WRITABLE);
}

static void init_vsyscall_area(struct page_table *pt)
{
	extern char vsyscall_page[];
	int error;

#define	VSYSCALL_ADDR	((void *)(0xffffffffff600000))
	error = __set_pt_page(pt, VSYSCALL_ADDR,
			virt_to_phys(vsyscall_page), PTATTR_ACTIVE|PTATTR_USER);
	if (error) {
		panic("init_vsyscall_area:__set_pt_page failed");
	}

	return;
}

void init_page_table(void)
{
#ifdef USE_LARGE_PAGES
	check_available_page_size();
#endif
	init_pt = arch_alloc_page(IHK_MC_AP_CRITICAL);
	ihk_mc_spinlock_init(&init_pt_lock);
	
	memset(init_pt, 0, sizeof(PAGE_SIZE));

	/* Normal memory area */
	init_normal_area(init_pt);
	init_fixed_area(init_pt);
	init_low_area(init_pt);
	init_text_area(init_pt);
	init_vsyscall_area(init_pt);

	load_page_table(init_pt);
	kprintf("Page table is now at %p\n", init_pt);
}

extern void __reserve_arch_pages(unsigned long, unsigned long,
                                 void (*)(unsigned long, unsigned long, int));

void ihk_mc_reserve_arch_pages(unsigned long start, unsigned long end,
                               void (*cb)(unsigned long, unsigned long, int))
{
	/* Reserve Text + temporal heap */
	cb(virt_to_phys(_head), virt_to_phys(get_last_early_heap()), 0);
	/* Reserve trampoline area to boot the second ap */
	cb(ap_trampoline, ap_trampoline + AP_TRAMPOLINE_SIZE, 0);
	/* Reserve the null page */
	cb(0, PAGE_SIZE, 0);
	/* Micro-arch specific */
	__reserve_arch_pages(start, end, cb);
}

void ihk_mc_set_page_allocator(struct ihk_mc_pa_ops *ops)
{
	last_page = (void *)-1;
	pa_ops = ops;
}

unsigned long virt_to_phys(void *v)
{
	unsigned long va = (unsigned long)v;
	
	if (va >= MAP_KERNEL_START) {
		return va - MAP_KERNEL_START + x86_kernel_phys_base;
	} else {
		return va - MAP_ST_START;
	}
}

void *phys_to_virt(unsigned long p)
{
	return (void *)(p + MAP_ST_START);
}

int copy_from_user(struct process *proc, void *dst, const void *src, size_t siz)
{
	struct process_vm *vm = proc->vm;
	struct vm_range *range;
	size_t pos;
	size_t wsiz;
	unsigned long pgstart = (unsigned long)src;

	wsiz = siz + (pgstart & 0x0000000000000fffUL);
	pgstart &= 0xfffffffffffff000UL;
	if(!pgstart || pgstart >= MAP_KERNEL_START)
		return -EFAULT;
	ihk_mc_spinlock_lock_noirq(&vm->memory_range_lock);
	for(pos = 0; pos < wsiz; pos += 4096, pgstart += 4096){
		range = lookup_process_memory_range(vm, pgstart, pgstart+1);
		if(range == NULL){
			ihk_mc_spinlock_unlock_noirq(&vm->memory_range_lock);
			return -EFAULT;
		}
		if((range->flag & VR_PROT_MASK) == VR_PROT_NONE){
			ihk_mc_spinlock_unlock_noirq(&vm->memory_range_lock);
			return -EFAULT;
		}
	}
	ihk_mc_spinlock_unlock_noirq(&vm->memory_range_lock);
	memcpy(dst, src, siz);
	return 0;
}

int copy_to_user(struct process *proc, void *dst, const void *src, size_t siz)
{
	struct process_vm *vm = proc->vm;
	struct vm_range *range;
	size_t pos;
	size_t wsiz;
	unsigned long pgstart = (unsigned long)dst;

	wsiz = siz + (pgstart & 0x0000000000000fffUL);
	pgstart &= 0xfffffffffffff000UL;
	if(!pgstart || pgstart >= MAP_KERNEL_START)
		return -EFAULT;
	ihk_mc_spinlock_lock_noirq(&vm->memory_range_lock);
	for(pos = 0; pos < wsiz; pos += 4096, pgstart += 4096){
		range = lookup_process_memory_range(vm, pgstart, pgstart+1);
		if(range == NULL){
			ihk_mc_spinlock_unlock_noirq(&vm->memory_range_lock);
			return -EFAULT;
		}
		if(((range->flag & VR_PROT_MASK) == VR_PROT_NONE) ||
		    !(range->flag & VR_PROT_WRITE)){
			ihk_mc_spinlock_unlock_noirq(&vm->memory_range_lock);
			return -EFAULT;
		}
	}
	ihk_mc_spinlock_unlock_noirq(&vm->memory_range_lock);
	memcpy(dst, src, siz);
	return 0;
}
