/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 *
 * demacOprx release
 * Source Code License Grant: The authors hereby grant to Licensee
 * personal permission to use and modify the Licensed Source Code
 * for the sole purpose of studying while attending the course CO2018.
 *
 * System Library
 * Memory Module Library libmem.c
 */

// #ifdef MM_PAGING

#include "string.h"
#include "mm64.h"
#include "syscall.h"
#include "libmem.h"
#include "dma.h"
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>

static pthread_mutex_t mmvm_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t kmm_lock = PTHREAD_MUTEX_INITIALIZER;

#ifdef MM64
#define LIBMEM_PAGESZ PAGING64_PAGESZ
#define LIBMEM_PAGE_SHIFT PAGING64_ADDR_PT_SHIFT
#define LIBMEM_PAGE_ALIGN(sz) PAGING64_PAGE_ALIGNSZ(sz)
#else
#define LIBMEM_PAGESZ PAGING_PAGESZ
#define LIBMEM_PAGE_SHIFT PAGING_ADDR_PGN_LOBIT
#define LIBMEM_PAGE_ALIGN(sz) PAGING_PAGE_ALIGNSZ(sz)
#endif

static addr_t get_page_num(addr_t addr)
{
#ifdef MM64
  return addr >> PAGING64_ADDR_PT_SHIFT;
#else
  return PAGING_PGN(addr);
#endif
}

static addr_t get_page_offset(addr_t addr)
{
#ifdef MM64
  return addr & (PAGING64_PAGESZ - 1);
#else
  return PAGING_OFFST(addr);
#endif
}

static int is_valid_user_region(struct pcb_t *caller, struct vm_area_struct *cur_vma,
                                struct vm_rg_struct *currg, addr_t offset)
{
  if (caller == NULL || caller->mm == NULL || cur_vma == NULL || currg == NULL)
    return 0;

  if (currg->rg_start >= currg->rg_end)
    return 0;

  addr_t target_addr = currg->rg_start + offset;
  if (target_addr < currg->rg_start || target_addr >= currg->rg_end)
    return 0;

  if (target_addr < cur_vma->vm_start || target_addr >= cur_vma->vm_end)
    return 0;

#ifdef KERNEL_BASE_ADDR
  if (target_addr >= (addr_t)KERNEL_BASE_ADDR)
    return 0;
#endif

  return 1;
}

static int _enlist_vm_freerg_list(struct mm_struct *mm, struct vm_rg_struct *rg_elmt)
{
  if (mm == NULL || mm->mmap == NULL || rg_elmt == NULL)
    return -1;

  if (rg_elmt->rg_start >= rg_elmt->rg_end)
    return -1;

  rg_elmt->rg_next = mm->mmap->vm_freerg_list;
  mm->mmap->vm_freerg_list = rg_elmt;

  return 0;
}

static int _get_free_vmrg_area(struct pcb_t *caller, int vmaid, addr_t size,
                               struct vm_rg_struct *newrg)
{
  if (caller == NULL || caller->mm == NULL || newrg == NULL || size == 0)
    return -1;

  struct vm_area_struct *cur_vma = get_vma_by_num(caller->mm, vmaid);
  if (cur_vma == NULL)
    return -1;

  struct vm_rg_struct *rgit = cur_vma->vm_freerg_list;
  struct vm_rg_struct *prev = NULL;

  while (rgit != NULL)
  {
    if (rgit->rg_start < rgit->rg_end &&
        (rgit->rg_end - rgit->rg_start) >= size)
    {
      newrg->vmaid = vmaid;
      newrg->rg_start = rgit->rg_start;
      newrg->rg_end = rgit->rg_start + size;
      newrg->rg_next = NULL;

      if (rgit->rg_start + size < rgit->rg_end)
      {
        rgit->rg_start += size;
      }
      else
      {
        if (prev == NULL)
          cur_vma->vm_freerg_list = rgit->rg_next;
        else
          prev->rg_next = rgit->rg_next;

        free(rgit);
      }

      return 0;
    }

    prev = rgit;
    rgit = rgit->rg_next;
  }

  return -1;
}

/* enlist_vm_freerg_list - add new rg to freerg_list */
int enlist_vm_freerg_list(struct mm_struct *mm, struct vm_rg_struct *rg_elmt)
{
  pthread_mutex_lock(&mmvm_lock);
  int ret = _enlist_vm_freerg_list(mm, rg_elmt);
  pthread_mutex_unlock(&mmvm_lock);
  return ret;
}

/* get_symrg_byid - get mem region by region ID */
struct vm_rg_struct *get_symrg_byid(struct mm_struct *mm, int rgid)
{
  if (mm == NULL || rgid < 0 || rgid >= PAGING_MAX_SYMTBL_SZ)
    return NULL;

  return &mm->symrgtbl[rgid];
}

/* __alloc - allocate a region memory */
int __alloc(struct pcb_t *caller, int vmaid, int rgid, addr_t size, addr_t *alloc_addr)
{
  if (caller == NULL || caller->mm == NULL || alloc_addr == NULL || size == 0)
    return -1;

  if (rgid < 0 || rgid >= PAGING_MAX_SYMTBL_SZ)
    return -1;

  pthread_mutex_lock(&mmvm_lock);

  struct vm_rg_struct rgnode = {0};
  struct vm_area_struct *cur_vma = get_vma_by_num(caller->mm, vmaid);
  if (cur_vma == NULL)
  {
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

  if (_get_free_vmrg_area(caller, vmaid, size, &rgnode) == 0)
  {
    caller->mm->symrgtbl[rgid].vmaid = vmaid;
    caller->mm->symrgtbl[rgid].rg_start = rgnode.rg_start;
    caller->mm->symrgtbl[rgid].rg_end = rgnode.rg_end;
    caller->mm->symrgtbl[rgid].rg_next = NULL;

    *alloc_addr = rgnode.rg_start;
    pthread_mutex_unlock(&mmvm_lock);
    return 0;
  }

  addr_t old_sbrk = cur_vma->sbrk;
  addr_t inc_sz = LIBMEM_PAGE_ALIGN(size);

  struct sc_regs regs = {0};
  regs.a1 = SYSMEM_INC_OP;
  regs.a2 = (arg_t)vmaid;
  regs.a3 = (arg_t)inc_sz;

  if (_syscall(caller->krnl, caller->pid, 17, &(regs)) < 0)
  {
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

  if (cur_vma->sbrk < old_sbrk + inc_sz)
  {
    cur_vma->sbrk = old_sbrk + inc_sz;
    if (cur_vma->vm_end < cur_vma->sbrk)
      cur_vma->vm_end = cur_vma->sbrk;
  }

  caller->mm->symrgtbl[rgid].vmaid = vmaid;
  caller->mm->symrgtbl[rgid].rg_start = old_sbrk;
  caller->mm->symrgtbl[rgid].rg_end = old_sbrk + size;
  caller->mm->symrgtbl[rgid].rg_next = NULL;

  if (old_sbrk + size < cur_vma->sbrk)
  {
    struct vm_rg_struct *rem_rg = malloc(sizeof(struct vm_rg_struct));
    if (rem_rg != NULL)
    {
      rem_rg->vmaid = vmaid;
      rem_rg->rg_start = old_sbrk + size;
      rem_rg->rg_end = cur_vma->sbrk;
      rem_rg->rg_next = NULL;
      _enlist_vm_freerg_list(caller->mm, rem_rg);
    }
  }

  *alloc_addr = old_sbrk;

  pthread_mutex_unlock(&mmvm_lock);
  return 0;
}

/* __free - remove a region memory */
int __free(struct pcb_t *caller, int vmaid, int rgid)
{
  if (caller == NULL || caller->mm == NULL || rgid < 0 || rgid >= PAGING_MAX_SYMTBL_SZ)
    return -1;

  pthread_mutex_lock(&mmvm_lock);

  struct vm_rg_struct *rgnode = get_symrg_byid(caller->mm, rgid);
  if (rgnode == NULL || rgnode->rg_start >= rgnode->rg_end)
  {
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

  struct vm_rg_struct *freerg_node = malloc(sizeof(struct vm_rg_struct));
  if (freerg_node == NULL)
  {
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

  freerg_node->vmaid = (rgnode->vmaid != 0) ? rgnode->vmaid : vmaid;
  freerg_node->rg_start = rgnode->rg_start;
  freerg_node->rg_end = rgnode->rg_end;
  freerg_node->rg_next = NULL;

  rgnode->rg_start = 0;
  rgnode->rg_end = 0;
  rgnode->rg_next = NULL;

  if (_enlist_vm_freerg_list(caller->mm, freerg_node) < 0)
  {
    free(freerg_node);
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

  pthread_mutex_unlock(&mmvm_lock);
  return 0;
}

/* liballoc - PAGING-based allocate a region memory */
int liballoc(struct pcb_t *proc, addr_t size, uint32_t reg_index)
{
  addr_t addr = 0;
  int val = __alloc(proc, 0, (int)reg_index, size, &addr);
  if (val < 0)
    return -1;

  if (reg_index < 10)
    proc->regs[reg_index] = addr;

#ifdef IODUMP
#ifdef PAGETBL_DUMP
  print_pgtbl(proc, 0, -1);
#endif
#endif

  return val;
}

/* libfree - PAGING-based free a region memory */
int libfree(struct pcb_t *proc, uint32_t reg_index)
{
  int val = __free(proc, 0, (int)reg_index);
  if (val < 0)
    return -1;

#ifdef IODUMP
#ifdef PAGETBL_DUMP
  print_pgtbl(proc, 0, -1);
#endif
#endif

  return 0;
}

/* pg_getpage - get the page in ram */
int pg_getpage(struct mm_struct *mm, int pgn, int *fpn, struct pcb_t *caller)
{
  if (mm == NULL || caller == NULL || caller->krnl == NULL || fpn == NULL || pgn < 0)
    return -1;

  addr_t pte = pte_get_entry(mm, (addr_t)pgn, caller);

  if (!PAGING_PAGE_PRESENT(pte))
  {
    addr_t tgtfpn = 0;

    if (MEMPHY_get_freefp(caller->krnl->mram, &tgtfpn) < 0)
    {
      addr_t vicpgn = 0;
      addr_t swpfpn = 0;

      if (find_victim_page(mm, &vicpgn) < 0)
        return -1;

      addr_t vicpte = pte_get_entry(mm, vicpgn, caller);
      addr_t vicfpn = PAGING_PTE_FPN(vicpte);

      if (caller->krnl->active_mswp == NULL ||
          MEMPHY_get_freefp(caller->krnl->active_mswp, &swpfpn) < 0)
        return -1;

      struct sc_regs regs = {0};
      regs.a1 = SYSMEM_SWP_OP;
      regs.a2 = vicfpn;
      regs.a3 = swpfpn;
      if (_syscall(caller->krnl, caller->pid, 17, &(regs)) < 0)
      {
        MEMPHY_put_freefp(caller->krnl->active_mswp, swpfpn);
        return -1;
      }

      pte_set_swap(caller, vicpgn, (int)caller->krnl->active_mswp_id, swpfpn);
      tgtfpn = vicfpn;
    }

    if (pte & PAGING_PTE_SWAPPED_MASK)
    {
      int swptyp = (int)PAGING_PTE_SWPTYP(pte);
      addr_t swpoff = PAGING_PTE_SWP(pte);

      if (caller->krnl->mswp != NULL &&
          swptyp >= 0 && swptyp < PAGING_MAX_MMSWP &&
          caller->krnl->mswp[swptyp] != NULL)
      {
        __swap_cp_page(caller->krnl->mswp[swptyp], swpoff, caller->krnl->mram, tgtfpn);
        MEMPHY_put_freefp(caller->krnl->mswp[swptyp], swpoff);
      }
    }

    pte_set_fpn(mm, (addr_t)pgn, tgtfpn, caller);
    enlist_pgn_node(&mm->fifo_pgn, (addr_t)pgn);
    *fpn = (int)tgtfpn;
    return 0;
  }

  *fpn = (int)PAGING_PTE_FPN(pte);
  return 0;
}

/* pg_getval - read value at given offset */
int pg_getval(struct mm_struct *mm, int addr, BYTE *data, struct pcb_t *caller)
{
  if (mm == NULL || data == NULL || caller == NULL || caller->krnl == NULL || addr < 0)
    return -1;

  addr_t uaddr = (addr_t)addr;
  int pgn = (int)get_page_num(uaddr);
  addr_t off = get_page_offset(uaddr);
  int fpn = -1;

  if (pg_getpage(mm, pgn, &fpn, caller) < 0 || fpn < 0)
    return -1;

  addr_t phyaddr = ((addr_t)fpn * LIBMEM_PAGESZ) + off;

  struct sc_regs regs = {0};
  regs.a1 = SYSMEM_IO_READ;
  regs.a2 = phyaddr;
  regs.a3 = 0;

  if (_syscall(caller->krnl, caller->pid, 17, &(regs)) < 0)
    return -1;

  *data = (BYTE)regs.a3;
  return 0;
}

/* pg_setval - write value to given offset */
int pg_setval(struct mm_struct *mm, int addr, BYTE value, struct pcb_t *caller)
{
  if (mm == NULL || caller == NULL || caller->krnl == NULL || addr < 0)
    return -1;

  addr_t uaddr = (addr_t)addr;
  int pgn = (int)get_page_num(uaddr);
  addr_t off = get_page_offset(uaddr);
  int fpn = -1;

  if (pg_getpage(mm, pgn, &fpn, caller) < 0 || fpn < 0)
    return -1;

  addr_t phyaddr = ((addr_t)fpn * LIBMEM_PAGESZ) + off;

  struct sc_regs regs = {0};
  regs.a1 = SYSMEM_IO_WRITE;
  regs.a2 = phyaddr;
  regs.a3 = (arg_t)(uint8_t)value;

  if (_syscall(caller->krnl, caller->pid, 17, &(regs)) < 0)
    return -1;

  return 0;
}

/* __read - read value in region memory */
int __read(struct pcb_t *caller, int vmaid, int rgid, addr_t offset, BYTE *data)
{
  if (caller == NULL || caller->mm == NULL || data == NULL)
    return -1;

  pthread_mutex_lock(&mmvm_lock);

  struct vm_rg_struct *currg = get_symrg_byid(caller->mm, rgid);
  struct vm_area_struct *cur_vma = get_vma_by_num(caller->mm, vmaid);

  if (!is_valid_user_region(caller, cur_vma, currg, offset))
  {
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

  addr_t addr = currg->rg_start + offset;
  int ret = pg_getval(caller->mm, (int)addr, data, caller);

  pthread_mutex_unlock(&mmvm_lock);
  return ret;
}

/* libread - PAGING-based read a region memory */
int libread(
    struct pcb_t *proc,
    uint32_t source,
    addr_t offset,
    uint32_t *destination)
{
  if (proc == NULL || destination == NULL)
    return -1;

  BYTE data = 0;
  int val = __read(proc, 0, (int)source, offset, &data);
  if (val < 0)
    return -1;

  *destination = (uint32_t)(uint8_t)data;

#ifdef IODUMP
#ifdef PAGETBL_DUMP
  print_pgtbl(proc, 0, -1);
#endif
#endif

  return val;
}

/* __write - write a region memory */
int __write(struct pcb_t *caller, int vmaid, int rgid, addr_t offset, BYTE value)
{
  if (caller == NULL || caller->mm == NULL)
    return -1;

  pthread_mutex_lock(&mmvm_lock);

  struct vm_rg_struct *currg = get_symrg_byid(caller->mm, rgid);
  struct vm_area_struct *cur_vma = get_vma_by_num(caller->mm, vmaid);

  if (!is_valid_user_region(caller, cur_vma, currg, offset))
  {
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

  addr_t addr = currg->rg_start + offset;
  int ret = pg_setval(caller->mm, (int)addr, value, caller);

  pthread_mutex_unlock(&mmvm_lock);
  return ret;
}

/* libwrite - PAGING-based write a region memory */
int libwrite(
    struct pcb_t *proc,
    BYTE data,
    uint32_t destination,
    addr_t offset)
{
  int val = __write(proc, 0, (int)destination, offset, data);
  if (val < 0)
    return -1;

#ifdef IODUMP
#ifdef PAGETBL_DUMP
  print_pgtbl(proc, 0, -1);
#endif
#endif

  return val;
}

/* ================= TV3: Kernel space & syscall ================= */

int libkmem_malloc(struct pcb_t *caller, uint32_t size, uint32_t reg_index)
{
  return 0;
}

addr_t __kmalloc(struct pcb_t *caller, int vmaid, int rgid, addr_t size, addr_t *alloc_addr)
{
  return 0;
}

int libkmem_copy_from_user(struct pcb_t *caller, uint32_t source, uint32_t destination, uint32_t offset, uint32_t size)
{
  return 0;
}

int libkmem_copy_to_user(struct pcb_t *caller, uint32_t source, uint32_t destination, uint32_t offset, uint32_t size)
{
  return 1;
}

int __read_kernel_mem(struct pcb_t *caller, int vmaid, int rgid, addr_t offset, BYTE *data)
{
  return 0;
}

int __write_kernel_mem(struct pcb_t *caller, int vmaid, int rgid, addr_t offset, BYTE value)
{
  return 0;
}

int __read_user_mem(struct pcb_t *caller, int vmaid, int rgid, addr_t offset, BYTE *data)
{
  return 0;
}

int __write_user_mem(struct pcb_t *caller, int vmaid, int rgid, addr_t offset, BYTE value)
{
  return 0;
}

/* ================= TV5: Co-processor DMA ================= */

int libmem_dma_memcpy(struct pcb_t *caller, uint32_t src, uint32_t dst, uint32_t offset, uint32_t size)
{
  return 0;
}

/* ================= TV2: Cleanup & Page Replacement & Free Region ================= */

/* free_pcb_memph - collect all memphy of pcb */
int free_pcb_memph(struct pcb_t *caller)
{
  if (caller == NULL || caller->mm == NULL || caller->krnl == NULL)
    return -1;

  pthread_mutex_lock(&mmvm_lock);

  struct pgn_t *pg = caller->mm->fifo_pgn;
  while (pg != NULL)
  {
    addr_t pte = pte_get_entry(caller->mm, pg->pgn, caller);
    if (PAGING_PAGE_PRESENT(pte))
    {
      addr_t fpn = PAGING_PTE_FPN(pte);
      if (caller->krnl->mram != NULL)
        MEMPHY_put_freefp(caller->krnl->mram, fpn);
    }
    else if (pte & PAGING_PTE_SWAPPED_MASK)
    {
      int swptyp = (int)PAGING_PTE_SWPTYP(pte);
      addr_t swpoff = PAGING_PTE_SWP(pte);
      if (caller->krnl->mswp != NULL &&
          swptyp >= 0 && swptyp < PAGING_MAX_MMSWP &&
          caller->krnl->mswp[swptyp] != NULL)
      {
        MEMPHY_put_freefp(caller->krnl->mswp[swptyp], swpoff);
      }
    }

    struct pgn_t *next = pg->pg_next;
    free(pg);
    pg = next;
  }

  caller->mm->fifo_pgn = NULL;

  pthread_mutex_unlock(&mmvm_lock);
  return 0;
}

/* find_victim_page - find victim page (FIFO) */
int find_victim_page(struct mm_struct *mm, addr_t *retpgn)
{
  if (mm == NULL || retpgn == NULL || mm->fifo_pgn == NULL)
    return -1;

  struct pgn_t *pg = mm->fifo_pgn;
  if (pg->pg_next == NULL)
  {
    *retpgn = pg->pgn;
    mm->fifo_pgn = NULL;
    free(pg);
    return 0;
  }

  struct pgn_t *prev = NULL;
  while (pg->pg_next != NULL)
  {
    prev = pg;
    pg = pg->pg_next;
  }

  *retpgn = pg->pgn;
  prev->pg_next = NULL;
  free(pg);

  return 0;
}

/* get_free_vmrg_area - get a free vm region */
int get_free_vmrg_area(struct pcb_t *caller, int vmaid, int size, struct vm_rg_struct *newrg)
{
  pthread_mutex_lock(&mmvm_lock);
  int ret = _get_free_vmrg_area(caller, vmaid, (addr_t)size, newrg);
  pthread_mutex_unlock(&mmvm_lock);
  return ret;
}

// #endif
