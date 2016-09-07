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
#include <emx/io.h>

#include <InnoTekLIBC/fork.h>

#include <sys/mman.h>

#define TRACE_GROUP TRACE_GROUP_MMAP
#include "../shared.h"

#define DIVIDE_UP(count, bucket_sz) (((count) + (bucket_sz - 1)) / (bucket_sz))

#define PAGE_ALIGNED(addr) (!(((ULONG)addr) & (PAGE_SIZE - 1)))
#define PAGE_ALIGN(addr) (((ULONG)addr) & ~(PAGE_SIZE - 1))
#define NUM_PAGES(count) DIVIDE_UP((count), PAGE_SIZE)

/* Width of a dirty map entry in bits */
#define DIRTYMAP_WIDTH (sizeof(*((struct MemMap*)0)->sh->dirty) * 8)

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
    uint32_t dirty[0]; /* Bit array of dirty pages */
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
  int fd = -1;
  size_t dirtymap_sz = 0;

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
      (flags & (MAP_PRIVATE | MAP_SHARED)) == 0 ||
      ((flags & MAP_ANON) && fildes != -1) ||
      (!(flags & MAP_ANON) && fildes == -1) ||
      len == 0)
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

  if (!(flags & MAP_ANON))
  {
    __LIBC_PFH pFH;
    FILESTATUS3L st;
    ULONG mode;

    pFH = __libc_FH(fildes);
    TRACE_IF(pFH, "pszNativePath [%s], fFlags %x\n", pFH->pszNativePath, pFH->fFlags);
    if (!pFH || (pFH->fFlags & __LIBC_FH_TYPEMASK) != F_FILE)
    {
      /* We only support regular files now */
      errno = !pFH ? EBADF : ENODEV;
      return MAP_FAILED;
    }

    arc = DosQueryFHState(fildes, &mode);
    TRACE_IF(arc, "DosQueryFHState = %lu\n", arc);
    if (!arc)
    {
      /* Check flags/prot compatibility with file access */
      TRACE("file mode %lx\n", mode);
      if ((mode & 0x7) == OPEN_ACCESS_WRITEONLY ||
          (flags & MAP_SHARED && prot & PROT_WRITE &&
           (mode & 0x7) != OPEN_ACCESS_READWRITE))
      {
        errno = EACCES;
        return MAP_FAILED;
      }

      /* Make own file handle since POSIX allows the original one to be closed */
      arc = DosDupHandle(fildes, (PHFILE)&fd);
      TRACE_IF(arc, "DosDupHandle = %lu\n", arc);
      if (arc)
      {
        errno = EMFILE;
        return MAP_FAILED;
      }

      /* Prevent the system critical-error handler */
      mode = OPEN_FLAGS_FAIL_ON_ERROR;
      /* Also disable inheritance for private mappings */
      if (flags & MAP_PRIVATE)
        mode |= OPEN_FLAGS_NOINHERIT;
      arc = DosSetFHState(fd, mode);
      TRACE_IF(arc, "DosSetFHState = %lu\n", arc);
    }

    if (arc)
    {
      /* This is an unexpected error, return "mmap not supported" */
      if (fd != -1)
        DosClose(fd);
      errno = ENODEV;
      return MAP_FAILED;
    }

    arc = DosQueryFileInfo(fd, FIL_STANDARDL, &st, sizeof(st));
    if (arc)
    {
      DosClose(fd);
      errno = EOVERFLOW;
      return MAP_FAILED;
    }

    TRACE("dup fd %d, file size %llu\n", fd, st.cbFile);

    if (off + len > st.cbFile)
    {
      DosClose(fd);
      errno = EOVERFLOW;
      return MAP_FAILED;
    }

    /* Calculate the number of bytes (in 4 byte words) needed for the dirty page bitmap */
    if (flags & MAP_SHARED)
      dirtymap_sz = DIVIDE_UP(NUM_PAGES(len), DIRTYMAP_WIDTH) * (DIRTYMAP_WIDTH / 8);
  }

  global_lock();

  /* Allocate a new entry */
  TRACE_IF(flags & MAP_SHARED, "dirty map size %u bytes\n", dirtymap_sz);
  GLOBAL_NEW_PLUS(mmap, flags & MAP_SHARED ? sizeof(*mmap->sh) + dirtymap_sz : 0);
  if (!mmap)
  {
    if (fd != -1)
      DosClose(fd);
    global_unlock();
    errno = ENOMEM;
    return MAP_FAILED;
  }

  mmap->flags = flags;
  mmap->fd = fd;
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
    if (fd != -1)
      DosClose(fd);
    global_unlock();
    errno = ENOMEM;
    return MAP_FAILED;
  }

  mmap->end = mmap->start + len;
  TRACE("mmap->start %p, mmap->end %p\n", mmap->start, mmap->end);

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
      if (fd != -1)
        DosClose(fd);
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
  TRACE_IF(m, "found m %p in %s (start %p, end %p, flags %x=%c%c%c%c, dosFlags %lx, fd %d, off %llu, pid %x, refcnt %d)\n",
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

static void flush_dirty_pages(struct MemMap *m)
{
  TRACE("mapping %p\n", m);
  assert(m && m->flags & MAP_SHARED && m->dosFlags & PAG_WRITE && m->fd != -1);

  /* Calculate the number of 4 byte words in the dirty page bitmap */
  size_t dirtymap_len = DIVIDE_UP(NUM_PAGES(m->end - m->start), DIRTYMAP_WIDTH);
  ULONG base, page;
  size_t i, j;
  APIRET arc;

  base = m->start;
  for (i = 0; i < dirtymap_len; ++i)
  {
    /* Checking blocks of 32 pages at once lets us quickly skip clean ones */
    if (m->sh->dirty[i])
    {
      uint32_t bit = 0x1;
      for (j = 0; j < DIRTYMAP_WIDTH; ++j, bit <<= 1)
      {
        if (m->sh->dirty[i] & bit)
        {
          ULONG written;
          LONGLONG pos;
          page = base + PAGE_SIZE * j;
          TRACE("Writing %d bytes from addr %p to fd %d at offset %llu\n",
                PAGE_SIZE, page, m->fd, (uint64_t)m->off + (page - m->start));
          arc = DosSetFilePtrL(m->fd, m->off + (page - m->start), FILE_BEGIN, &pos);
          TRACE_IF(arc, "DosSetFilePtrL = %ld\n", arc);
          if (!arc)
          {
            arc = DosWrite(m->fd, (PVOID)page, PAGE_SIZE, &written);
            TRACE_IF(arc, "DosWrite = %ld\n", arc);

            /* Reset the dirty bit on success */
            if (!arc)
              m->sh->dirty[i] &= ~bit;
          }
        }
      }
    }
    base += PAGE_SIZE * DIRTYMAP_WIDTH;
  }
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

    /* POSIX requires to return silently when no matching region is found */
    return rc;
  }

  if (m)
  {
    if (m->start == (ULONG)addr && m->end == addr_end)
    {
      /* Simplest case: the whole region is unmapped */
      if (m->flags & MAP_SHARED && m->dosFlags & PAG_WRITE && m->fd != -1)
        flush_dirty_pages(m);
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
        if (m->fd != -1)
          DosClose(m->fd);
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
      TRACE("Removing private mapping %p (start %p)\n", m, m->start);
      arc = DosFreeMem((PVOID)m->start);
      TRACE("DosFreeMem = %ld\n", arc);
      if (m->fd != -1)
        DosClose(m->fd);
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
      /* This mapping is used in this process, release it */
      TRACE("Releasing shared mapping %p (start %p, refcnt %d)\n", m, m->start, m->sh->refcnt);
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
        if (m->fd != -1)
          DosClose(m->fd);
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

    TRACE("addr %p, info %lx\n", addr, report->ExceptionInfo[0]);

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
        if (!arc)
        {
          if (!(dosFlags & (PAG_FREE | PAG_COMMIT)))
          {
            /* First access to the allocated but uncommitted page */
            int revoke_write = 0;
            if (m->flags & MAP_SHARED && m->dosFlags & PAG_WRITE && m->fd != -1)
            {
              /*
               * First access to a writable shared mapping page. If it's a read
               * attempt, then we commit the page and remove PAG_WRITE so that
               * we will get an exception when the page is first written to
               * to mark it dirty. If it's a write attempt, we simply mark it
               * as dirty right away.
               */
              if (report->ExceptionInfo[0] == XCPT_WRITE_ACCESS)
              {
                ULONG pn = (pageAddr - m->start) / PAGE_SIZE;
                size_t i = pn / DIRTYMAP_WIDTH;
                uint32_t bit = 0x1 << (pn % DIRTYMAP_WIDTH);
                m->sh->dirty[i] |= bit;
                TRACE("Marked bit 0x%x at idx %u as dirty\n", bit, i);
              }
              else
              {
                /* DosRead needs PAG_WRITE, so schedule removal for later */
                revoke_write = 1;
              }
            }
            arc = DosSetMem((PVOID)pageAddr, len, m->dosFlags | PAG_COMMIT);
            TRACE_IF(arc, "DosSetMem = %ld\n", arc);
            if (!arc)
            {
              if (m->fd != -1)
              {
                /*
                 * Read file contents into memory. Note that if we fail here,
                 * there is nothing we can do as POSIX doesn't imply such
                 * a case. @todo We may consider let the exception abort
                 * the application instead.
                 */
                ULONG read;
                LONGLONG pos;
                TRACE("Reading %d bytes to addr %p from fd %d at offset %llu\n",
                      PAGE_SIZE, pageAddr, m->fd, (uint64_t)m->off + (pageAddr - m->start));
                arc = DosSetFilePtrL(m->fd, m->off + (pageAddr - m->start), FILE_BEGIN, &pos);
                TRACE_IF(arc, "DosSetFilePtrL = %ld\n", arc);
                if (!arc)
                {
                  arc = DosRead(m->fd, (PVOID)pageAddr, PAGE_SIZE, &read);
                  TRACE_IF(arc, "DosRead = %ld\n", arc);
                }
              }
              if (!arc)
              {
                if (revoke_write)
                {
                  dosFlags = m->dosFlags & ~PAG_WRITE;
                  arc = DosSetMem((PVOID)pageAddr, len, m->dosFlags & ~PAG_WRITE);
                  TRACE_IF(arc, "DosSetMem = %ld\n", arc);
                }
                if (!arc)
                {
                  /* We successfully committed and read the page, let the app retry */
                  retry = 1;
                }
              }
            }
          }
          else if (dosFlags & PAG_COMMIT)
          {
            if (report->ExceptionInfo[0] == XCPT_WRITE_ACCESS &&
                m->flags & MAP_SHARED && m->dosFlags & PAG_WRITE && m->fd != -1)
            {
              /*
               * First write access to a writable shared mapping page. Mark the
               * page as dirty and set PAG_WRITE to let the app continue (also
               * see above).
               */
              arc = DosSetMem((PVOID)pageAddr, len, m->dosFlags);
              if (!arc)
              {
                ULONG pn = (pageAddr - m->start) / PAGE_SIZE;
                size_t i = pn / DIRTYMAP_WIDTH;
                uint32_t bit = 0x1 << (pn % DIRTYMAP_WIDTH);
                m->sh->dirty[i] |= bit;
                TRACE("Marked bit 0x%x at idx %u as dirty\n", bit, i);

                retry = 1;
              }
            }
          }
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
    ULONG dosFlags = m->dosFlags;
    TRACE("giving mapping %p (start %p) to pid %x\n", m, m->start, pForkHandle->pidChild);
    if (m->dosFlags & PAG_WRITE && m->fd != -1)
    {
      /*
       * This is a writable shared mapping, remove PAG_WRITE to get an exception
       * on the first write in child (see mmap_exception above).
       */
      dosFlags &= ~PAG_WRITE;
    }
    arc = DosGiveSharedMem((PVOID)m->start, pForkHandle->pidChild, dosFlags);
    TRACE_IF(arc, "DosGiveSharedMem = %ld\n", arc);
    ++(m->sh->refcnt);
    m = m->next;
  }

  global_unlock();

  return 0;
}

_FORK_PARENT1(0, forkParent);
