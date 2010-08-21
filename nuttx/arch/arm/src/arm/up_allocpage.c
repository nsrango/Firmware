/****************************************************************************
 * arch/arm/src/arm/up_allocpage.c
 * Allocate a new page and map it to the fault address of a task.
 *
 *   Copyright (C) 2010 Gregory Nutt. All rights reserved.
 *   Author: Gregory Nutt <spudmonkey@racsa.co.cr>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name NuttX nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <debug.h>

#include <nuttx/sched.h>

#ifdef CONFIG_PAGING

#include <nuttx/page.h>
#include "pg_macros.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/****************************************************************************
 * Private Types
 ****************************************************************************/

#if CONFIG_PAGING_NPAGED < 256
typedef uint8_t  pgndx_t;
#elif CONFIG_PAGING_NPAGED < 65536
typedef uint16_t pgndx_t;
#else
typedef uint32_t pgndx_t;
#endif

#if PG_POOL_MAXL1NDX < 256
typedef uint8_t  L1ndx_t;
#elif PG_POOL_MAXL1NDX < 65536
typedef uint16_t L1ndx_t;
#else
typedef uint32_t L1ndx_t;
#endif

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* Free pages in memory are managed by indices ranging from up to
 * CONFIG_PAGING_NPAGED.  Initially all pages are free so the page can be
 * simply allocated in order: 0, 1, 2, ... .  After all CONFIG_PAGING_NPAGED
 * pages have be filled, then they are blindly freed and re-used in the
 * same order 0, 1, 2, ... because we don't know any better.  No smart "least
 * recently used" kind of logic is supported.
 */

static pgndx_t g_pgndx;

/* After CONFIG_PAGING_NPAGED have been allocated, the pages will be re-used.
 * In order to re-used the page, we will have un-map the page from its previous
 * mapping.  In order to that, we need to be able to map a physical address to
 * to an index into the PTE where it was mapped.  The following table supports
 * this backward lookup - it is indexed by the page number index, and holds
 * another index to the mapped virtual page.
 */

static L1ndx_t g_ptemap[CONFIG_PAGING_NPAGED];

/* The contents of g_ptemap[] are not valid until g_pgndx has wrapped at
 * least one time.
 */

static bool g_pgwrap;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: up_allocpage()
 *
 * Description:
 *  This architecture-specific function will set aside page in memory and map
 *  the page to its correct virtual address.  Architecture-specific context
 *  information saved within the TCB will provide the function with the
 *  information needed to identify the virtual miss address.
 *
 *  This function will return the allocated physical page address in vpage.
 *  The size of the underlying physical page is determined by the
 *  configuration setting CONFIG_PAGING_PAGESIZE.
 *
 *  NOTE 1: This function must always return a page allocation. If all
 *  available pages are in-use (the typical case), then this function will
 *  select a page in-use, un-map it, and make it available.
 *
 *  NOTE 2: Allocating and filling a page is a two step process.  up_allocpage()
 *  allocates the page, and up_fillpage() fills it with data from some non-
 *  volatile storage device.  This distinction is made because up_allocpage()
 *  can probably be implemented in board-independent logic whereas up_fillpage()
 *  probably must be implemented as board-specific logic.
 *
 * Input Parameters:
 *   tcb - A reference to the task control block of the task that needs to
 *         have a page fill.  Architecture-specific logic can retrieve page
 *         fault information from the architecture-specific context
 *         information in this TCB to perform the mapping.
 *
 * Returned Value:
 *   This function will return zero (OK) if the allocation was successful.
 *   A negated errno value may be returned if an error occurs.  All errors,
 *   however, are fatal.
 *
 * Assumptions:
 *   - This function is called from the normal tasking context (but with
 *     interrupts disabled).  The implementation must take whatever actions
 *     are necessary to assure that the operation is safe within this
 *     context.
 *
 ****************************************************************************/

int up_allocpage(FAR _TCB *tcb, FAR void **vpage)
{
  uintptr_t vaddr;
  uintptr_t paddr;
  uint32_t *pte;
  unsigned int pgndx;
  unsigned int l2ndx;
  
  /* Since interrupts are disabled, we don't need to anything special. */

  DEBUGASSERT(tcb && vpage);

  /* Get the virtual address that caused the fault */

  vaddr = tcb->xcp.far;
  DEBUGASSERT(vaddr >= PG_PAGED_VBASE && vaddr < PG_PAGED_VEND);

  /* Verify that this virtual address was previously unmapped */

#if CONFIG_DEBUG
  pte = (uint32_t*)PG_POOL_L2VADDR(vaddr);
  DEBUGASSERT(*pte == 0);
#endif

  /* Allocate page memory to back up the mapping.  Start by getting the
   * index of the next page that we are going to allocate.
   */

  pgndx = g_pgndx++;
  if (p_pgndx >= CONFIG_PAGING)
    {
      g_pgndx  = 0;
      g_pgwrap = true;
    }

  /* Then convert the index to a (physical) page address. */

  paddr = PG_POOL_PGPADDR(pgndx);

  /* Was this physical page previously mapped? If so, then we need to un-map
   * it.
   */

  if (g_pgwrap)
    {
      /* Yes.. Get a pointer to the L2 entry corresponding to the previous
       * mapping -- then zero it!  
       */

       l2ndx = g_ptemap[pgndx];
       pte = (uint32_t*)PG_POOL_NDX2L2VADDR(l2ndx);
      *pte = 0;

      /* Invalidate the TLB corresponding to the virtual address */

      tlb_inst_invalidate_single(PG_POOL_NDX2VA(l2ndx))
    }

  /* Now setup up the new mapping.  Get a pointer to the L2 entry
   * corresponding to the new mapping.  Then set it map to the newly
   * allocated page address.
   */

  pte = (uint32_t*)PG_POOL_VA2L2VADDR(va)
  *pte = (paddr | MMU_L2_TEXTFLAGS);

 /* Finally, return the virtual address of allocated page */

 *vpage = (void*)PG_POOL_PHYS2VIRT(paddr);
 return OK;
}

#endif /* CONFIG_PAGING */