/*
 * mmap implementation for kLIBC.
 * Copyright (C) 2016 bww bitwise works GmbH.
 * This file is part of the kLIBC Extension Library.
 * Authored by Dmitry Kuminov <coding@dmik.org>, 2016.
 *
 * The kLIBC Extension Library is free software; you can redistribute it
 * and/or modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * The kLIBC Extension Library is distributed in the hope that it will be
 * useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with the GNU C Library; if not, see
 * <http://www.gnu.org/licenses/>.
 */

#define OS2EMX_PLAIN_CHAR
#define INCL_BASE
#include <os2.h>

#include <errno.h>
#include <process.h>
#include <sys/types.h>
#include <assert.h>
#include <stdlib.h>

#include <InnoTekLIBC/fork.h>

#include <sys/mman.h>

#define TRACE_GROUP TRACE_GROUP_MMAP
#include "../shared.h"

#define PAGE_ALIGNED(addr) (!(((ULONG)addr) & 4095))
#define PAGE_ALIGN(addr) (((ULONG)addr) & ~4095)

/**
 * Memory mapping (linked list entry).
 */
struct MemMap
{
  struct MemMap *next;

  ULONG start; /* start address */
  ULONG end; /* end address (exclusive) */
  int flags; /* mmap flags */
  ULONG dosFlags; /* DosAllocMem protection flags */
  int fd; /* -1 for MAP_ANONYMOUS */
  off_t off;
  /* Additional fields for shared mmaps */
  struct Shared
  {
    pid_t pid; /* Creator PID */
    int refcnt; /* Number of PIDs using it */
  } sh[0];
};

/* Temporary import from MMAP.DLL */
void *mmap_mmap(void *addr, size_t len, int prot, int flags,
                int fildes, off_t off);
int mmap_munmap(void *addr, size_t len);

void *mmap(void *addr, size_t len, int prot, int flags,
           int fildes, off_t off)
{
  APIRET arc;
  struct MemMap *mmap;

  /* For now we only support MAP_ANOPN mappings */
  if (!(flags & MAP_ANON) || fildes != -1)
    return mmap_mmap(addr, len, prot, flags, fildes, off);

  TRACE("addr %p, len %u, prot %x=%c%c%c, flags %x=%c%c%c%c, fildes %d, off %llu\n",
        addr, len, prot,
        prot & PROT_READ ? 'R' : '-',
        prot & PROT_WRITE ? 'W' : '-',
        prot & PROT_EXEC ? 'X' : '-',
        flags,
        flags & MAP_SHARED ? 'S' : '-',
        flags & MAP_PRIVATE ? 'P' : '-',
        flags & MAP_FIXED ? 'F' : '-',
        flags & MAP_ANON ? 'A' : '-',
        fildes, (uint64_t)off);

  /* Input validation */
  if ((flags & (MAP_PRIVATE | MAP_SHARED)) == (MAP_PRIVATE | MAP_SHARED) ||
      (flags & (MAP_PRIVATE | MAP_SHARED)) == 0)
  {
    errno = EINVAL;
    return MAP_FAILED;
  }

  /* MAP_FIXED is not supported */
  if (flags & MAP_FIXED)
  {
    errno = EINVAL;
    return MAP_FAILED;
  }

  global_lock();

  /* Allocate a new entry */
  GLOBAL_NEW_PLUS(mmap, flags & MAP_SHARED ? sizeof(*mmap->sh) : 0);
  if (!mmap)
  {
    global_unlock();
    errno = ENOMEM;
    return MAP_FAILED;
  }

  mmap->flags = flags;
  mmap->fd = fildes;
  mmap->off = off;

  mmap->dosFlags = 0;
  if (prot & PROT_READ)
    mmap->dosFlags |= PAG_READ;
  if (prot & PROT_WRITE)
    mmap->dosFlags |= PAG_WRITE;
  if (prot & PROT_EXEC)
    mmap->dosFlags |= PAG_EXECUTE;

  if (flags & MAP_SHARED)
  {
    /* Shared mmap specific data */
    mmap->sh->pid = getpid();
    mmap->sh->refcnt = 1;

    arc = DosAllocSharedMem((PPVOID)&mmap->start, NULL, len, mmap->dosFlags | OBJ_ANY | OBJ_GIVEABLE);
    TRACE("DosAllocSharedMem(OBJ_ANY) = %lu\n", arc);
    if (arc)
    {
      /* High memory may be unavailable, try w/o OBJ_ANY */
      arc = DosAllocSharedMem((PPVOID)&mmap->start, NULL, len, mmap->dosFlags | OBJ_GIVEABLE);
      TRACE("DosAllocSharedMem = %lu\n", arc);
    }
  }
  else
  {
    arc = DosAllocMem((PPVOID)&mmap->start, len, mmap->dosFlags | OBJ_ANY);
    TRACE("DosAllocMem(OBJ_ANY) = %lu\n", arc);
    if (arc)
    {
      /* High memory may be unavailable, try w/o OBJ_ANY */
      arc = DosAllocMem((PPVOID)&mmap->start, len, mmap->dosFlags);
      TRACE("DosAllocMem = %lu\n", arc);
    }
  }
  if (arc)
  {
    free(mmap);
    global_unlock();
    errno = ENOMEM;
    return MAP_FAILED;
  }

  mmap->end = mmap->start + len;
  TRACE("mmap->start %lx\n", mmap->start);

  /* Check if aligned to page size */
  assert(mmap->start && PAGE_ALIGNED(mmap->start));

  if (flags & MAP_SHARED)
  {
    mmap->next = gpData->mmaps;
    gpData->mmaps = mmap;
  }
  else
  {
    struct ProcDesc *desc = get_proc_desc(getpid());
    if (!desc)
    {
      DosFreeMem((PVOID)mmap->start);
      free(mmap);
      global_unlock();
      errno = ENOMEM;
      return MAP_FAILED;
    }

    mmap->next = desc->mmaps;
    desc->mmaps = mmap;
  }

  global_unlock();

  return (void *)mmap->start;
}

/**
 * Searches for a mapping containign the given address range. Returns the
 * found mapping or NULL. Returns the previous mapping in the list if pm_out
 * is not NULL and the head of the list (private or shared) if head_out is
 * not NULL. Must be called from under global_lock().
 */
static struct MemMap *find_mmap(ULONG addr, ULONG addr_end, struct MemMap **pm_out, struct MemMap ***head_out)
{
  struct ProcDesc *desc;
  struct MemMap *m = NULL, *pm = NULL;
  struct MemMap **head = NULL;

  /* First, search for the address in our private mmaps */
  desc = find_proc_desc(getpid());
  if (desc)
  {
    head = &desc->mmaps;
    m = *head;
    while (m && !(m->start <= addr && m->end >= addr_end))
    {
      pm = m;
      m = m->next;
    }
  }

  if (!m)
  {
    /* Second, search for the address in our shared mmaps */
    head = &gpData->mmaps;
    m = *head;
    while (m && !(m->start <= addr && m->end >= addr_end))
    {
      pm = m;
      m = m->next;
    }
  }

  TRACE_IF(!m, "mapping not found\n");
  TRACE_IF(m, "found m %p in %s (start %lx, end %lx, flags %x=%c%c%c%c, dosFlags %lx, fd %d, off %llu, pid %x, refcnt %d)\n",
           m, head == &gpData->mmaps ? "SHARED" : "PRIVATE", m->start, m->end, m->flags,
           m->flags & MAP_SHARED ? 'S' : '-',
           m->flags & MAP_PRIVATE ? 'P' : '-',
           m->flags & MAP_FIXED ? 'F' : '-',
           m->flags & MAP_ANON ? 'A' : '-',
           m->dosFlags, m->fd, (uint64_t)m->off,
           m->flags & MAP_SHARED ? m->sh->pid : 0,
           m->flags & MAP_SHARED ? m->sh->refcnt : 0);

  assert(!m ||
         (head != &gpData->mmaps && m->flags & MAP_PRIVATE) ||
         (head == &gpData->mmaps && m->flags & MAP_SHARED));

  if (pm_out)
    *pm_out = pm;
  if (head_out)
    *head_out = head;

  return m;
}

int munmap(void *addr, size_t len)
{
  APIRET arc;
  int rc = 0;

  TRACE("addr %p, len %u\n", addr, len);

  /* Check if aligned to page size */
  if (!PAGE_ALIGNED(addr))
  {
    errno = EINVAL;
    return -1;
  }

  /* Check if len within bounds */
  if (0 - ((uintptr_t)addr) < len)
  {
    errno = EINVAL;
    return -1;
  }

  ULONG addr_end = ((ULONG)addr) + len;

  struct MemMap *m = NULL, *pm = NULL;
  struct MemMap **head;

  global_lock();

  m = find_mmap((ULONG)addr, addr_end, &pm, &head);
  if (!m)
  {
    global_unlock();

    /* Temporarily fall back to MMAP.DLL */
    return mmap_munmap(addr, len);
  }

  /* Note that when no mapping is found, POSIX requires to return success */
  if (m)
  {
    if (m->start == (ULONG)addr && m->end == addr_end)
    {
      /* Simplest case: the whole region is unmapped */
      arc = DosFreeMem((PVOID)addr);
      TRACE("DosFreeMem = %ld\n", arc);
      /* Dereference and free the mapping when appropriate */
      if (m->flags & MAP_SHARED)
      {
        TRACE("Releasing shared mapping %p\n", m);
        assert(m->sh->refcnt);
        --(m->sh->refcnt);
      }
      if (m->flags & MAP_PRIVATE || m->sh->refcnt == 0)
      {
        TRACE("Removing mapping %p\n", m);
        if (pm)
          pm->next = m->next;
        else
          *head = m->next;
        free(m);
      }
    }
    else
    {
      /*
       * @todo We can't partially free memory regions on OS/2 so in order to
       * meet POSIX requirements of generating SIGSEGV for unmapped memory
       * we should come up with a different solution (like marking pages with
       * PAGE_GUARD and do some r/w trickery). Note also that POSIX allows
       * munmap to unmap two or more adjacent regions which is not explicitly
       * supported by DosFreeMem either but we can achieve this by hand (this
       * will require to change the while statemet above and also to sort
       * the mmap list by start addr). For now just fail with ENOMEM as if it
       * would require a split of the region but there is no memory for it.
       */
       errno = ENOMEM;
       rc = -1;
    }
  }

  global_unlock();

  return rc;
}

/**
 * Uninitializes the mmap structures.
 * Called upon each process termination before gpData is uninitialized
 * or destroyed.
 */
void mmap_term()
{
  struct ProcDesc *desc;
  struct MemMap *m, *pm;
  APIRET arc;

  /* Free all private mmap structures */
  desc = find_proc_desc(getpid());
  if (desc)
  {
    m = desc->mmaps;
    while (m)
    {
      struct MemMap *n = m->next;
      TRACE("Removing private mapping %p (start %lx)\n", m, m->start);
      arc = DosFreeMem((PVOID)m->start);
      TRACE("DosFreeMem = %ld\n", arc);
      free(m);
      m = n;
    }
    desc->mmaps = NULL;
  }

  /* Dereference and free shared map structures when appropriate */
  pm = NULL;
  m = gpData->mmaps;
  while (m)
  {
    struct MemMap *n = m->next;
    ULONG len = m->end - m->start;
    ULONG dosFlags;
    arc = DosQueryMem((PVOID)m->start, &len, &dosFlags);
    if (!arc && !(dosFlags & PAG_FREE))
    {
      /* This mapping is used in this process, releasea it */
      TRACE("Releasing shared mapping %p (start %lx, refcnt %d)\n", m, m->start, m->sh->refcnt);
      arc = DosFreeMem((PVOID)m->start);
      TRACE("DosFreeMem = %ld\n", arc);
      assert(m->sh->refcnt);
      --(m->sh->refcnt);
      if (m->sh->refcnt == 0)
      {
        TRACE("Removing shared mapping %p\n", m);
        if (pm)
          pm->next = n;
        else
          gpData->mmaps = n;
        free(m);
      }
    }
    m = n;
  }

  TRACE_IF(gpData->refcnt == 0, "gpData->mmaps = %p\n", gpData->mmaps);
  assert(gpData->refcnt > 0 || gpData->mmaps == NULL);
}

/**
 * System exception handler for mmap.
 * @return 1 to retry execution, 0 to call other handlers.
 */
int mmap_exception(struct _EXCEPTIONREPORTRECORD *report,
                   struct _EXCEPTIONREGISTRATIONRECORD *reg,
                   struct _CONTEXT *ctx)
{
  int retry = 0;

  if (report->ExceptionNum == XCPT_ACCESS_VIOLATION)
  {
    struct MemMap *m;

    ULONG addr = report->ExceptionInfo[1];

    TRACE("addr %lx, info %lx\n", addr, report->ExceptionInfo[0]);

    global_lock();

    m = find_mmap(addr, addr + 1, NULL, NULL);

    if (m)
    {
      APIRET arc;
      ULONG len = 1;
      ULONG dosFlags;
      ULONG pageAddr = PAGE_ALIGN(addr);
      arc = DosQueryMem((PVOID)pageAddr, &len, &dosFlags);
      if (!arc)
      {
        TRACE("dosFlags %lx\n", dosFlags);
        if (!arc && !(dosFlags & (PAG_FREE | PAG_COMMIT)))
        {
          arc = DosSetMem((PVOID)pageAddr, len, m->dosFlags | PAG_COMMIT);
          if (!arc)
            retry = 1;
        }
      }
    }

    global_unlock();

    TRACE_IF(retry, "retrying\n");
  }

  return retry;
}

static int forkParent(__LIBC_PFORKHANDLE pForkHandle, __LIBC_FORKOP enmOperation)
{
  if (enmOperation != __LIBC_FORK_OP_FORK_PARENT)
    return 0;

  APIRET arc;
  struct MemMap *m;

  global_lock();

  /* Give MAP_SHARED mappings to the child process (and all grandchildren) */
  m = gpData->mmaps;
  while (m)
  {
    assert(m->flags && MAP_SHARED);
    TRACE("giving mapping %p (start %lx) to pid %x\n", m, m->start, pForkHandle->pidChild);
    arc = DosGiveSharedMem((PVOID)m->start, pForkHandle->pidChild, m->dosFlags);
    TRACE_IF(arc, "DosGiveSharedMem = %ld\n", arc);
    ++(m->sh->refcnt);
    m = m->next;
  }

  global_unlock();

  return 0;
}

_FORK_PARENT1(0, forkParent);
