#ifndef MM64_H
#define MM64_H

#include "mm.h"
#define MM64_BITS_PER_LONG 64

#ifdef __cplusplus
extern "C" {
#endif

#define PAGING64_CPU_BUS_WIDTH 57 /* 57 bit virtual address */
#define PAGING64_PAGESZ  4096     /* 4KB or 12-bits OFFSET */

#define GENMASK64(h, l) \ (((~0ULL) << (l)) & (~0ULL >> (MM64_BITS_PER_LONG  - (h) - 1)))

#define PAGING64_MAX_PGN (DIV_ROUND_UP(BIT_ULL(21),PAGING64_PAGESZ))
#define PAGING64_PAGE_ALIGNSZ(sz) (DIV_ROUND_UP(sz,PAGING64_PAGESZ)*PAGING64_PAGESZ)


/* OFFSET */
#define PAGING64_ADDR_OFFST_HIBIT 11
#define PAGING64_ADDR_OFFST_LOBIT 0

/* PT */
#define PAGING64_ADDR_PT_HIBIT 20
#define PAGING64_ADDR_PT_LOBIT 12
#define PAGING64_ADDR_PT_SHIFT 12

/* PMD */
#define PAGING64_ADDR_PMD_HIBIT 29
#define PAGING64_ADDR_PMD_LOBIT 21

/* PUD */
#define PAGING64_ADDR_PUD_HIBIT 38
#define PAGING64_ADDR_PUD_LOBIT 30

/* P4D */
#define PAGING64_ADDR_P4D_HIBIT 47
#define PAGING64_ADDR_P4D_LOBIT 39

/* PGD */
#define PAGING64_ADDR_PGD_HIBIT 56
#define PAGING64_ADDR_PGD_LOBIT 48

/* Masks */
#define PAGING64_ADDR_OFFST_MASK GENMASK64(PAGING64_ADDR_OFFST_HIBIT,PAGING64_ADDR_OFFST_LOBIT)
#define PAGING64_ADDR_PT_MASK  	GENMASK64(PAGING64_ADDR_PT_HIBIT,PAGING64_ADDR_PT_LOBIT)
#define PAGING64_ADDR_PMD_MASK	GENMASK64(PAGING64_ADDR_PMD_HIBIT,PAGING64_ADDR_PMD_LOBIT)
#define PAGING64_ADDR_PUD_MASK  GENMASK64(PAGING64_ADDR_PUD_HIBIT,PAGING64_ADDR_PUD_LOBIT)
#define PAGING64_ADDR_P4D_MASK  GENMASK64(PAGING64_ADDR_P4D_HIBIT,PAGING64_ADDR_P4D_LOBIT)
#define PAGING64_ADDR_PGD_MASK  GENMASK64(PAGING64_ADDR_PGD_HIBIT,PAGING64_ADDR_PGD_LOBIT)

/* Extract PGD Entry */
#define PAGING64_ADDR_OFFST(addr) (((addr)&PAGING64_ADDR_OFFST_MASK)>>PAGING64_ADDR_OFFST_LOBIT)
#define PAGING64_ADDR_PT(addr)    (((addr)&PAGING64_ADDR_PT_MASK)>>PAGING64_ADDR_PT_LOBIT)
#define PAGING64_ADDR_PMD(addr)   (((addr)&PAGING64_ADDR_PMD_MASK)>>PAGING64_ADDR_PMD_LOBIT)
#define PAGING64_ADDR_PUD(addr)   (((addr)&PAGING64_ADDR_PUD_MASK)>>PAGING64_ADDR_PUD_LOBIT)
#define PAGING64_ADDR_P4D(addr)   (((addr)&PAGING64_ADDR_P4D_MASK)>>PAGING64_ADDR_P4D_LOBIT)
#define PAGING64_ADDR_PGD(addr)   (((addr)&PAGING64_ADDR_PGD_MASK)>>PAGING64_ADDR_PGD_LOBIT)

// -----------------------
// 5-level table geometry                                            
// -----------------------
#define MM64_LEVELS      5                       // PGD,P4D,PUD,PMD,PT
#define MM64_ENTRIES     512                     // 2^9 entries/table
#define MM64_TABLE_BYTES (MM64_ENTRIES * 8)      // 4 KiB per table
#define MM64_VA_BITS     PAGING64_CPU_BUS_WIDTH  // 57

// --------------------------------------
// Canonical address + user/kernel split 
// --------------------------------------
#ifndef KERNEL_BASE_ADDR
#define KERNEL_BASE_ADDR (1ULL << 56)
#endif

static inline int mm64_is_canonical(addr_t addr) {
	return (addr >> MM64_VA_BITS) == 0;
}

static inline int mm64_is_kernel_addr(addr_t addr) {
	return addr >= (addr_t)KERNEL_BASE_ADDR;
}

// ---------------------------------------------------------
// macOS-style layout: one window per allocation size class 
// ---------------------------------------------------------
#define MM64_SMALL_MAX   (8ULL * 1024)
#define MM64_MEDIUM_MAX  (1ULL * 1024 * 1024)
#define MM64_SMALL_BASE  0x0ULL
#define MM64_MEDIUM_BASE (1ULL << 30)
#define MM64_LARGE_BASE  (2ULL << 30)

enum mm64_region_class {
	MM64_REGION_SMALL = 0,
	MM64_REGION_MEDIUM,
	MM64_REGION_LARGE
};

static inline int mm64_region_class(addr_t size) {
	if (size <= MM64_SMALL_MAX)
		return MM64_REGION_SMALL;
	if (size <= MM64_MEDIUM_MAX)
		return MM64_REGION_MEDIUM;
	return MM64_REGION_LARGE;
}

// -----------------------------------------------------------------
// Translation (shared by CPU/MMU path and by the DMA co-processor) 
// -----------------------------------------------------------------
/*
 * mm64_translate - virtual address -> physical address in RAM.
 * Returns  0 : ok, *pa filled
 *         -1 : not canonical / not mapped
 *         -2 : page is swapped out (caller must swap it in, then retry)
 */
int mm64_translate(struct mm_struct *mm, addr_t va, addr_t *pa);

// --------------------------
// Statistics (mm64_stats.c) 
// --------------------------
void mm64_stat_walk(void);                 // one page-table walk started
void mm64_stat_entry_read(void);           // one table entry read
void mm64_stat_table_alloc(int level);     // a table of [level] allocated
void mm64_stat_pages_mapped(unsigned long n);
void mm64_stats_print(void);               // call once at the end of main

#ifdef __cplusplus
}
#endif

#endif