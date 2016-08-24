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
  pid_t pid; /* Creator PID */
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

  /* For now we only support MAP_SHARED and non-file mappigs */
  if ((flags & (MAP_SHARED | MAP_ANON)) != (MAP_SHARED | MAP_ANON) ||
      fildes != -1)
    return mmap_mmap(addr, len, prot, flags, fildes, off);

  TRACE("addr %p, len %u, prot %x=%c%c%c, flags %x=%c%c, fildes %d, off %llu\n",
        addr, len, prot,
        prot & PROT_READ ? 'R' : '-',
        prot & PROT_WRITE ? 'W' : '-',
        prot & PROT_EXEC ? 'X' : '-',
        flags,
        flags & MAP_FIXED ? 'F' : '-',
        flags & MAP_ANON ? 'A' : '-',
        fildes, (uint64_t)off);

  /* Input validation */
  if ((flags & (MAP_PRIVATE | MAP_SHARED)) == (MAP_PRIVATE | MAP_SHARED))
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

  /* Allocate a new entry */
  GLOBAL_NEW(mmap);
  if (!mmap)
  {
    errno = ENOMEM;
    return MAP_FAILED;
  }

  mmap->flags = flags;
  mmap->fd = fildes;
  mmap->off = off;
  mmap->pid = getpid();

  mmap->dosFlags = 0;
  if (prot & PROT_READ)
    mmap->dosFlags |= PAG_READ;
  if (prot & PROT_WRITE)
    mmap->dosFlags |= PAG_WRITE;
  if (prot & PROT_EXEC)
    mmap->dosFlags |= PAG_EXECUTE;

  arc = DosAllocSharedMem((PPVOID)&mmap->start, NULL, len, mmap->dosFlags | OBJ_ANY | OBJ_GIVEABLE);
  TRACE("DosAllocSharedMem(OBJ_ANY) = %lu\n", arc);
  if (arc)
  {
    /* High memory may be unavailable, try w/o OBJ_ANY */
    arc = DosAllocSharedMem((PPVOID)&mmap->start, NULL, len, mmap->dosFlags | OBJ_GIVEABLE);
    TRACE("DosAllocSharedMem = %lu\n", arc);
  }
  if (arc)
  {
    global_free(mmap);
    errno = ENOMEM;
    return MAP_FAILED;
  }

  mmap->end = mmap->start + len;
  TRACE("mmap->start %lx\n", mmap->start);

  /* Check if aligned to page size */
  assert(mmap->start && PAGE_ALIGNED(mmap->start));

  global_lock();

  mmap->next = gpData->mmaps;
  gpData->mmaps = mmap;

  global_unlock();

  return (void *)mmap->start;
}

int munmap(void *addr, size_t len)
{
  APIRET arc;

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

  struct MemMap *m;

  global_lock();

  /* Search for the address in our mmaps */
  m = gpData->mmaps;
  while (m && !(m->start <= (ULONG)addr && m->end >= addr_end))
    m = m->next;

  TRACE_IF(!m, "mapping not found\n");
  TRACE_IF(m, "found m %p (start %lx, end %lx, flags %x=%c%c, dosFlags %lx, fd %d, off %llu, pid %d)\n",
           m, m->start, m->end, m->flags,
           m->flags & MAP_FIXED ? 'F' : '-',
           m->flags & MAP_ANON ? 'A' : '-',
           m->dosFlags, m->fd, (uint64_t)m->off, m->pid);

  if (!m)
  {
    global_unlock();

    /* Temporarily fall back to MMAP.DLL */
    return mmap_munmap(addr, len);
  }

  /* @todo only support MAP_SHARED for now */
  if (m && m->flags & MAP_SHARED)
  {
    if (m->start == (ULONG)addr && m->end == addr_end)
    {
      /* Simplest case: the whole region is unmapped */
      arc = DosFreeMem((PVOID)addr);
      TRACE("DosFreeMem=%ld\n", arc);
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
       * the mmap list by start addr). For now just succeed cause this is
       * the requirement of POSI as well.
       */
    }
  }

  global_unlock();

  return 0;
}

/**
 * Uninitializes the mmap shared structures.
 * Called upon each process termination before gpData is uninitialized
 * or destroyed.
 */
void mmap_term()
{
  if (gpData->refcnt == 0)
  {
    /* We are the last process, free mmap structures */
    struct MemMap *m = gpData->mmaps;
    while (m)
    {
      struct MemMap *n = m->next;
      global_free(m);
      m = n;
    }
    gpData->mmaps = NULL;
  }
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

    /* Search for the address in our mmaps */
    m = gpData->mmaps;
    while (m && !(m->start <= addr && m->end > addr))
      m = m->next;

    TRACE_IF(!m, "mapping not found\n");
    TRACE_IF(m, "found m %p (start %lx, end %lx, flags %x=%c%c, dosFlags %lx, fd %d, off %llu, pid %d)\n",
             m, m->start, m->end, m->flags,
             m->flags & MAP_FIXED ? 'F' : '-',
             m->flags & MAP_ANON ? 'A' : '-',
             m->dosFlags, m->fd, (uint64_t)m->off, m->pid);

    /* @todo only support MAP_SHARED for now */
    if (m && m->flags & MAP_SHARED)
    {
      APIRET arc;
      ULONG len = 1;
      ULONG dosFlags;
      ULONG pageAddr = PAGE_ALIGN(addr);
      arc = DosQueryMem((PVOID)pageAddr, &len, &dosFlags);
      if (!arc)
      {
        TRACE("dosFlags %lx\n", dosFlags);
        if (!arc && !(dosFlags & PAG_COMMIT))
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
  while (m && m->flags && MAP_SHARED)
  {
    TRACE("giving mapping %lx to pid %d\n", m->start, pForkHandle->pidChild);
    arc = DosGiveSharedMem((PVOID)m->start, pForkHandle->pidChild, m->dosFlags);
    TRACE_IF(arc, "DosGiveSharedMem=%ld\n", arc);
    m = m->next;
  }

  global_unlock();

  return 0;
}

_FORK_PARENT1(0, forkParent);
