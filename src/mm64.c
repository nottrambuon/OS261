/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 */

/* LamiaAtrium release
 * Source Code License Grant: The authors hereby grant to Licensee
 * personal permission to use and modify the Licensed Source Code
 * for the sole purpose of studying while attending the course CO2018.
 */

/*
 * PAGING based Memory Management
 * Memory management unit mm/mm.c
 */

#include "mm64.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>

#if defined(MM64)

// Protects the page-table trees (CPU threads and the DMA thread share them)
static pthread_mutex_t pt_lock = PTHREAD_MUTEX_INITIALIZER;

/*
 * init_pte - Initialize PTE entry
 */
int init_pte(
    addr_t *pte,
    int pre,        // present
    addr_t fpn,     // FPN
    int drt,        // dirty
    int swp,        // swap
    int swptyp,     // swap type
    addr_t swpoff   // swap offset
) {
    if (pre != 0) {
        if (swp == 0) { // Non swap ~ page online
            if (fpn == 0)
                return -1;  // Invalid setting

            /* Valid setting with FPN */
            SETBIT(*pte, PAGING_PTE_PRESENT_MASK);
            CLRBIT(*pte, PAGING_PTE_SWAPPED_MASK);
            CLRBIT(*pte, PAGING_PTE_DIRTY_MASK);

            SETVAL(*pte, fpn, PAGING_PTE_FPN_MASK, PAGING_PTE_FPN_LOBIT);
        }
        else { // page swapped
            SETBIT(*pte, PAGING_PTE_PRESENT_MASK);
            SETBIT(*pte, PAGING_PTE_SWAPPED_MASK);
            CLRBIT(*pte, PAGING_PTE_DIRTY_MASK);

            SETVAL(*pte, swptyp, PAGING_PTE_SWPTYP_MASK, PAGING_PTE_SWPTYP_LOBIT);
            SETVAL(*pte, swpoff, PAGING_PTE_SWPOFF_MASK, PAGING_PTE_SWPOFF_LOBIT);
        }
    }

    return 0;
}


/*
 * get_pd_from_address - split a virtual address into the 5 table indexes
 * @addr  : virtual address
 * @pgd   : index in page global directory   (bits 56-48)
 * @p4d   : index in page level-4 directory  (bits 47-39)
 * @pud   : index in page upper directory    (bits 38-30)
 * @pmd   : index in page middle directory   (bits 29-21)
 * @pt    : index in page table              (bits 20-12)
 */
int get_pd_from_address(addr_t addr, addr_t* pgd, addr_t* p4d, addr_t* pud, addr_t* pmd, addr_t* pt) {
	*pgd = PAGING64_ADDR_PGD(addr);
	*p4d = PAGING64_ADDR_P4D(addr);
	*pud = PAGING64_ADDR_PUD(addr);
	*pmd = PAGING64_ADDR_PMD(addr);
	*pt  = PAGING64_ADDR_PT(addr);

	return 0;
}

/*
 * get_pd_from_pagenum - same as above but from a page number (addr >> 12)
 */
int get_pd_from_pagenum(addr_t pgn, addr_t* pgd, addr_t* p4d, addr_t* pud, addr_t* pmd, addr_t* pt) {
	return get_pd_from_address(pgn << PAGING64_ADDR_PT_SHIFT, pgd, p4d, pud, pmd, pt);
}

// ------------------
// Core: 5-level walk
// ------------------

/* Remember the first table of each level in mm->p4d/pud/pmd/pt (for dumps) */
static void remember_first(struct mm_struct *mm, int level, addr_t *table) {
    addr_t **slot = NULL;

    switch (level) {
        case 1: slot = &mm->p4d; break;
        case 2: slot = &mm->pud; break;
        case 3: slot = &mm->pmd; break;
        case 4: slot = &mm->pt;  break;
    }

    if (slot != NULL && *slot == NULL)
        *slot = table;
}

/*
 * walk - find the leaf PTE slot of page [pgn]
 * @create : 1 = allocate missing tables on the way, 0 = just look
 * return  : pointer to the PTE inside the PT table, or NULL
 * Caller must hold pt_lock.
 */
static addr_t *walk(struct mm_struct *mm, addr_t pgn, int create) {
    addr_t idx[MM64_LEVELS];
    addr_t *table;

    if (mm == NULL || mm->pgd == NULL)
        return NULL;
    if ((pgn >> (MM64_VA_BITS - PAGING64_ADDR_PT_SHIFT)) != 0)
        return NULL;                      /* not a canonical address */

    mm64_stat_walk();
    get_pd_from_pagenum(pgn, &idx[0], &idx[1], &idx[2], &idx[3], &idx[4]);

    table = mm->pgd;
    for (int lv = 0; lv < MM64_LEVELS - 1; lv++) { // PGD, P4D, PUD, PMD
        addr_t *entry = &table[idx[lv]];

        mm64_stat_entry_read();
        if (*entry == 0) {
            addr_t *next;

            if (!create)
                return NULL;
                
            next = calloc(MM64_ENTRIES, sizeof(addr_t));
            if (next == NULL)
                return NULL;
            
            *entry = (addr_t)(uintptr_t)next;
            mm64_stat_table_alloc(lv + 1);
            remember_first(mm, lv + 1, next);
        }
        table = (addr_t *)(uintptr_t)*entry;
    }

    mm64_stat_entry_read(); // the leaf PTE itself
    return &table[idx[MM64_LEVELS - 1]];
}

// ----------
// PTE access
// ----------
// Make a PTE point to RAM frame [fpn] (page online)
static void set_fpn_bits(addr_t *pte, addr_t fpn) {
    SETBIT(*pte, PAGING_PTE_PRESENT_MASK);
    CLRBIT(*pte, PAGING_PTE_SWAPPED_MASK);
    CLRBIT(*pte, PAGING_PTE_SWPOFF_MASK);  // drop stale swap offset
    SETVAL(*pte, fpn, PAGING_PTE_FPN_MASK, PAGING_PTE_FPN_LOBIT);
}

/*
 * pte_set_swap - Set PTE entry for swapped page
 * @caller : the process owning the page (its mm holds the page table)
 * @pgn    : page number
 * @swptyp : swap device id
 * @swpoff : frame number inside the swap device
 */
int pte_set_swap(struct pcb_t *caller, addr_t pgn, int swptyp, addr_t swpoff) {
    addr_t *pte;  

    if (caller == NULL)
        return -1;

    pthread_mutex_lock(&pt_lock);
    pte = walk(caller->mm, pgn, 1);
    if (pte != NULL) {
        CLRBIT(*pte, PAGING_PTE_PRESENT_MASK);
        SETBIT(*pte, PAGING_PTE_SWAPPED_MASK);

        SETVAL(*pte, swptyp, PAGING_PTE_SWPTYP_MASK, PAGING_PTE_SWPTYP_LOBIT);
        SETVAL(*pte, swpoff, PAGING_PTE_SWPOFF_MASK, PAGING_PTE_SWPOFF_LOBIT);
    }
    pthread_mutex_unlock(&pt_lock);

    return (pte != NULL) ? 0 : -1;
}

/*
 * pte_set_fpn - Set PTE entry for on-line page
 * @mm    : page table owner (process mm, or kernel mm)
 * @pgn   : page number
 * @fpn   : frame page number (FPN)
 */
int pte_set_fpn(struct mm_struct *mm, addr_t pgn, addr_t fpn, struct pcb_t *caller) {
    addr_t *pte;

    if (mm == NULL && caller != NULL)
        mm = caller->mm;

    // PTE has a 13-bit FPN field: refuse instead of silently truncating
    if (fpn > (addr_t)PAGING_PTE_FPN_MASK)
        return -1;

    pthread_mutex_lock(&pt_lock);
    pte = walk(mm, pgn, 1);

    if (pte != NULL)
        set_fpn_bits(pte, fpn);

    pthread_mutex_unlock(&pt_lock);

    return (pte != NULL) ? 0 : -1;
}

/* Get PTE page table entry (0 = no entry / not mapped)
 * @mm     : page table owner
 * @pgn    : page number
 * @ret    : page table entry value
 **/
addr_t pte_get_entry(struct mm_struct *mm, addr_t pgn, struct pcb_t *caller) {
    addr_t *pte;
    addr_t val = 0;

    if (mm == NULL && caller != NULL)
        mm = caller->mm;

    pthread_mutex_lock(&pt_lock);
    pte = walk(mm, pgn, 0);  // reading never allocates tables
    if (pte != NULL)
        val = *pte;

    pthread_mutex_unlock(&pt_lock);

    return val;
}

/* Set PTE page table entry to a raw value
 * @mm      : page table owner
 * @pgn     : page number
 * @pte_val : value to store
 **/
int pte_set_entry(struct mm_struct *mm, addr_t pgn, uint64_t pte_val, struct pcb_t *caller) {
    addr_t *pte;

    if (mm == NULL && caller != NULL)
        mm = caller->mm;

    pthread_mutex_lock(&pt_lock);
    pte = walk(mm, pgn, 1);
    
    if (pte != NULL)
        *pte = (addr_t)pte_val;

    pthread_mutex_unlock(&pt_lock);

    return (pte != NULL) ? 0 : -1;
}

// -------
// Mapping
// -------

/*
 * vmap_pgd_memset - reserve the page-table path for a range of pages.
 * It only creates the (empty) tables; PTEs stay 0 = "not present".
 * Used by SYSMEM_MAP_OP so later mappings do not need to allocate tables.
 */
int vmap_pgd_memset(
    struct pcb_t *caller,   // process call
    addr_t addr,            // start address which is aligned to pagesz
    int pgnum               // num of mapping page
) {
    addr_t pgn = addr >> PAGING64_ADDR_PT_SHIFT;
    int pgit;
    int ret = 0;

    if (caller == NULL)
        return -1;

    pthread_mutex_lock(&pt_lock);
    for (pgit = 0; pgit < pgnum; pgit++) {
        if (walk(caller->mm, pgn + pgit, 1) == NULL) {
            ret = -1;
            break;
        }
    }
    pthread_mutex_unlock(&pt_lock);

    return ret;
}

/*
 * vmap_page_range - map a range of page at aligned address
 * Maps frames[0..] to pages starting at [addr]. ret_rg gets the real mapped
 * range. Returns 0 if all [pgnum] pages are mapped, -1 otherwise.
 */
addr_t vmap_page_range(
    struct pcb_t *caller,           // process call
    addr_t addr,                    // start address which is aligned to pagesz // start
    int pgnum,                      // num of mapping page
    struct framephy_struct *frames, // list of the mapped frames
    struct vm_rg_struct *ret_rg     // return mapped region, the real mapped fp
) {                                 // no guarantee all given pages are mapped
    struct framephy_struct *fpit = frames;
    addr_t pgn = addr >> PAGING64_ADDR_PT_SHIFT;
    int pgit = 0;

    if (caller == NULL || caller->mm == NULL)
        return (addr_t)-1;

    pthread_mutex_lock(&pt_lock);
    while (fpit != NULL && pgit < pgnum) {
        addr_t *pte = walk(caller->mm, pgn + pgit, 1);

        if (pte == NULL || fpit->fpn > (addr_t)PAGING_PTE_FPN_MASK)
            break;
        
        set_fpn_bits(pte, fpit->fpn);
        // Tracking for page replacement (FIFO): enqueue the new usage page
        enlist_pgn_node(&caller->mm->fifo_pgn, pgn + pgit);

        pgit++;
        fpit = fpit->fp_next;
    }
    pthread_mutex_unlock(&pt_lock);

    mm64_stat_pages_mapped(pgit);

    if (ret_rg != NULL) {
        ret_rg->vmaid = 0;
        ret_rg->rg_start = addr;
        ret_rg->rg_end = addr + (addr_t)pgit * PAGING64_PAGESZ;
        ret_rg->rg_next = NULL;
    }

    return (pgit == pgnum) ? 0 : (addr_t)-1;
}

/*
 * get_frame - get one free RAM frame for [caller].
 * If RAM is full, swap the oldest page (FIFO victim) of the caller out to
 * the active swap device and reuse its frame.
 */
static int get_frame(struct pcb_t *caller, addr_t *fpn) {
    struct krnl_t *krnl = caller->krnl;
    addr_t vicpgn, vicpte, vicfpn, swpfpn;

    if (MEMPHY_get_freefp(krnl->mram, fpn) == 0)
        return 0;

    // RAM is full: swap a victim out
    if (find_victim_page(caller->mm, &vicpgn) < 0)
        return -1;
    
    vicpte = pte_get_entry(caller->mm, vicpgn, caller);

    if (!PAGING_PAGE_PRESENT(vicpte))
        return -1;

    vicfpn = PAGING_PTE_FPN(vicpte);

    if (krnl->active_mswp == NULL || MEMPHY_get_freefp(krnl->active_mswp, &swpfpn) < 0)
        return -1;

    __swap_cp_page(krnl->mram, vicfpn, krnl->active_mswp, swpfpn);
    pte_set_swap(caller, vicpgn, (int)krnl->active_mswp_id, swpfpn);

    *fpn = vicfpn;
    return 0;
}

/*
 * alloc_pages_range - allocate req_pgnum of frame in ram
 * @caller    : caller
 * @req_pgnum : request page num
 * @frm_lst   : frame list (output)
 * return 0 on success, (addr_t)-3000 if out of memory
 */
addr_t alloc_pages_range(struct pcb_t *caller, int req_pgnum, struct framephy_struct **frm_lst) {
    struct framephy_struct *head = NULL;
    struct framephy_struct **tail = &head;
    int pgit;

    for (pgit = 0; pgit < req_pgnum; pgit++) {
        addr_t fpn;
        struct framephy_struct *node = NULL;

        if (get_frame(caller, &fpn) == 0)
            node = malloc(sizeof(struct framephy_struct));

        if (node == NULL) {
            // Out of memory: give back the frames taken so far
            while (head != NULL) {
                struct framephy_struct *next = head->fp_next;
                MEMPHY_put_freefp(caller->krnl->mram, head->fpn);
                free(head);
                head = next;
            }

            *frm_lst = NULL;
            return (addr_t)-3000;
        }

        node->fpn = fpn;
        node->fp_next = NULL;
        node->owner = caller->mm;
        *tail = node;
        tail = &node->fp_next;
    }

    *frm_lst = head;
    return 0;
}

/*
 * vm_map_ram - do the mapping all vm are to ram storage device
 * @caller    : caller
 * @astart    : vm area start (unused)
 * @aend      : vm area end   (unused)
 * @mapstart  : start mapping point
 * @incpgnum  : number of mapped page
 * @ret_rg    : returned region
 * return 0 on success, (addr_t)-1 on failure
 */
addr_t vm_map_ram(struct pcb_t *caller, addr_t astart, addr_t aend, addr_t mapstart, int incpgnum, struct vm_rg_struct *ret_rg) {
    struct framephy_struct *frm_lst = NULL;
    addr_t ret;

    (void)astart;
    (void)aend;

    if (alloc_pages_range(caller, incpgnum, &frm_lst) != 0)
        return (addr_t)-1;

    ret = vmap_page_range(caller, mapstart, incpgnum, frm_lst, ret_rg);

    /* The list only carried frame numbers to vmap_page_range: free the nodes */
    while (frm_lst != NULL) {
        struct framephy_struct *next = frm_lst->fp_next;
        free(frm_lst);
        frm_lst = next;
    }

    return (ret == 0) ? 0 : (addr_t)-1;
}

/* Swap copy content page from source frame to destination frame
 * @mpsrc  : source memphy
 * @srcfpn : source physical page number (FPN)
 * @mpdst  : destination memphy
 * @dstfpn : destination physical page number (FPN)
 **/
int __swap_cp_page(struct memphy_struct *mpsrc, addr_t srcfpn, struct memphy_struct *mpdst, addr_t dstfpn) {
    int cellidx;

    if (mpsrc == NULL || mpdst == NULL)
        return -1;

    for (cellidx = 0; cellidx < PAGING64_PAGESZ; cellidx++)     {
        addr_t addrsrc = srcfpn * PAGING64_PAGESZ + cellidx;
        addr_t addrdst = dstfpn * PAGING64_PAGESZ + cellidx;
        BYTE data;

        MEMPHY_read(mpsrc, addrsrc, &data);
        MEMPHY_write(mpdst, addrdst, data);
    }

    return 0;
}

// -----------------------------------------------------------
// Address translation (CPU through MMU, and DMA co-processor)
// -----------------------------------------------------------
/*
 * mm64_translate - virtual address -> RAM physical address
 *   0  : ok, *pa = frame * 4096 + offset
 *  -1  : not canonical or not mapped
 *  -2  : page is in swap (page fault: swap it in and try again)
 */
int mm64_translate(struct mm_struct *mm, addr_t va, addr_t *pa) {
    addr_t *p;
    addr_t pte = 0;

    if (pa == NULL || !mm64_is_canonical(va))
        return -1;

    pthread_mutex_lock(&pt_lock);
    p = walk(mm, va >> PAGING64_ADDR_PT_SHIFT, 0);
    if (p != NULL)
        pte = *p;
    pthread_mutex_unlock(&pt_lock);

    if (!PAGING_PAGE_PRESENT(pte))
        return (pte & PAGING_PTE_SWAPPED_MASK) ? -2 : -1;

    *pa = ((addr_t)PAGING_PTE_FPN(pte) << PAGING64_ADDR_PT_SHIFT) | (va & (PAGING64_PAGESZ - 1));
    return 0;
}

// --------------------------
// Memory management instance
// --------------------------
/*
 *Initialize a empty Memory Management instance
 * @mm:     self mm
 * @caller: mm owner (may be NULL for the kernel mm)
 */
int init_mm(struct mm_struct *mm, struct pcb_t *caller) {
    struct vm_area_struct *vma0;
    struct vm_rg_struct *first_rg;

    (void)caller;
    if (mm == NULL)
        return -1;

    memset(mm, 0, sizeof(*mm));         // pgd/p4d/.../symrgtbl/fifo = empty

    // Root of the 5-level tree: only the PGD exists at start
    mm->pgd = calloc(MM64_ENTRIES, sizeof(addr_t));
    if (mm->pgd == NULL)
        return -1;
    mm64_stat_table_alloc(0);

    // By default the owner comes with at least one vma
    vma0 = calloc(1, sizeof(struct vm_area_struct));
    if (vma0 == NULL)
        return -1;

    vma0->vm_id = 0;
    vma0->vm_start = 0;
    vma0->vm_end = vma0->vm_start;
    vma0->sbrk = vma0->vm_start;
    vma0->vm_freerg_list = NULL;
    vma0->vm_next = NULL;
    vma0->vm_mm = mm;                   // point vma owner backward

    first_rg = init_vm_rg(vma0->vm_start, vma0->vm_end);
    enlist_vm_rg_node(&vma0->vm_freerg_list, first_rg);

    mm->mmap = vma0;

    return 0;
}

struct vm_rg_struct *init_vm_rg(addr_t rg_start, addr_t rg_end) {
    struct vm_rg_struct *rgnode = malloc(sizeof(struct vm_rg_struct));

    rgnode->vmaid = 0;
    rgnode->rg_start = rg_start;
    rgnode->rg_end = rg_end;
    rgnode->rg_next = NULL;

    return rgnode;
}

int enlist_vm_rg_node(struct vm_rg_struct **rglist, struct vm_rg_struct *rgnode) {
    rgnode->rg_next = *rglist;
    *rglist = rgnode;

    return 0;
}

int enlist_pgn_node(struct pgn_t **plist, addr_t pgn) {
    struct pgn_t *pnode = malloc(sizeof(struct pgn_t));

    pnode->pgn = pgn;
    pnode->pg_next = *plist;
    *plist = pnode;

    return 0;
}

int print_list_fp(struct framephy_struct *ifp) {
    struct framephy_struct *fp = ifp;

    printf("print_list_fp: ");
    if (fp == NULL) {
        printf("NULL list\n");
        return -1;
    }
    printf("\n");
    
    while (fp != NULL)     {
        printf("fp[" FORMAT_ADDR "]\n", fp->fpn);
        fp = fp->fp_next;
    }
    printf("\n");
    
    return 0;
}

int print_list_rg(struct vm_rg_struct *irg) {
    struct vm_rg_struct *rg = irg;

    printf("print_list_rg: ");
    if (rg == NULL) {
        printf("NULL list\n");
        return -1;
    }
    printf("\n");
    
    while (rg != NULL) {
        printf("rg[" FORMAT_ADDR "->"  FORMAT_ADDR "]\n", rg->rg_start, rg->rg_end);
        rg = rg->rg_next;
    }
    printf("\n");

    return 0;
}

int print_list_vma(struct vm_area_struct *ivma) {
    struct vm_area_struct *vma = ivma;

    printf("print_list_vma: ");
    if (vma == NULL) {
        printf("NULL list\n");
        return -1;
    }
    printf("\n");
    
    while (vma != NULL) {
        printf("va[" FORMAT_ADDR "->" FORMAT_ADDR "]\n", vma->vm_start, vma->vm_end);
        vma = vma->vm_next;
    }
    printf("\n");

    return 0;
}

int print_list_pgn(struct pgn_t *ip) {
    printf("print_list_pgn: ");
    if (ip == NULL) {
        printf("NULL list\n");
        return -1;
    }
    printf("\n");

    while (ip != NULL) {
        printf("va[" FORMAT_ADDR "]-\n", ip->pgn);
        ip = ip->pg_next;
    }
    printf("\n");

    return 0;
}

/*
 * print_pgtbl - dump the 5-level path of address [start]
 * Prints the base address of the PGD/P4D/PUD/PMD/PT tables met when
 * translating [start] (0 = table does not exist yet).
 * Define PGTBL_PTE_DUMP to also list every present PTE in [start, end).
 */
int print_pgtbl(struct pcb_t *caller, addr_t start, addr_t end) {
    addr_t idx[MM64_LEVELS];
    addr_t *tbl[MM64_LEVELS] = { NULL, NULL, NULL, NULL, NULL };

    if (caller == NULL || caller->mm == NULL)
        return -1;

    get_pd_from_address(start, &idx[0], &idx[1], &idx[2], &idx[3], &idx[4]);

    pthread_mutex_lock(&pt_lock);
    tbl[0] = caller->mm->pgd;

    for (int lv = 1; lv < MM64_LEVELS; lv++) {
        addr_t e = (tbl[lv - 1] != NULL) ? tbl[lv - 1][idx[lv - 1]] : 0;
        tbl[lv] = (addr_t *)(uintptr_t)e;
    }
    pthread_mutex_unlock(&pt_lock);

    printf(" PDG=" FORMATX_ADDR " P4D=" FORMATX_ADDR " PUD=" FORMATX_ADDR " PMD=" FORMATX_ADDR " PT=" FORMATX_ADDR "\n",
        (void*)tbl[0], (void*)tbl[1], (void*)tbl[2],
        (void*)tbl[3], (void*)tbl[4]);

#ifdef PGTBL_PTE_DUMP
{
    addr_t limit = end;
    addr_t pgn;

    if (caller->mm->mmap != NULL && limit > caller->mm->mmap->sbrk)
        limit = caller->mm->mmap->sbrk;   // only the mapped part

    for (pgn = start >> PAGING64_ADDR_PT_SHIFT;
        pgn < (limit + PAGING64_PAGESZ - 1) >> PAGING64_ADDR_PT_SHIFT; pgn++)

    {
        addr_t pte = pte_get_entry(caller->mm, pgn, caller);
        if (pte != 0)
            printf("   pgn=%llu pte=0x%llx\n", (unsigned long long)pgn, (unsigned long long)pte);
    }
}
#else
(void)end;
#endif

    return 0;
}

#endif  //def MM64