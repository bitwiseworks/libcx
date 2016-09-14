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
#include <string.h>
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

/* Flush operation start delay (ms) */
#define FLUSH_DELAY 2000

/**
 * Per-process data for memory mappings.
 */
struct ProcMemMap
{
  int flush_tid; /* Flush thread */
};

/**
 * Global data for memory mappings.
 */
struct GlobalMemMap
{
  HEV flush_sem; /* Semaphore for flush thread */
  HEV flush_coop_sem; /* Semaphore for flush thread cooperation */
  int flush_request : 1; /* 1 - semaphore is posted */
};

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
    pid_t pid; /* Creator/owner PID */
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
  TRACE("mmap %p, mmap->start %p, mmap->end %p\n", mmap, mmap->start, mmap->end);

  /* Check if aligned to page size */
  assert(mmap->start && PAGE_ALIGNED(mmap->start));

  if (flags & MAP_SHARED)
  {
    mmap->next = gpData->mmaps;
    gpData->mmaps = mmap;
  }
  else
  {
    struct ProcDesc *desc = find_proc_desc(getpid());
    assert(desc);

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
  ULONG base, page, nesting;
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
          ULONG written, nesting;
          LONGLONG pos;

          /*
           * Make sure we are not interrupted by process termination and other
           * async signals in the middle of writing out the dirty page and
           * resetting the dirty bit to have them in sync. Note that it's OK to
           * be interrupted prior to writing out all dirty pages because he
           * remaining ones will be written out from mmap_term() on thread 1 at
           * process termination anyway, given that the dirty bit correctly
           * reflects their state.
           */
          DosEnterMustComplete(&nesting);

          page = base + PAGE_SIZE * j;
          TRACE("Writing %d bytes from addr %p to fd %d at offset %llu\n",
                PAGE_SIZE, page, m->fd, (uint64_t)m->off + (page - m->start));
          arc = DosSetFilePtrL(m->fd, m->off + (page - m->start), FILE_BEGIN, &pos);
          TRACE_IF(arc, "DosSetFilePtrL = %ld\n", arc);
          if (!arc)
          {
            arc = DosWrite(m->fd, (PVOID)page, PAGE_SIZE, &written);
            TRACE_IF(arc, "DosWrite = %ld\n", arc);

            /*
             * Reset PAG_WRITE (to cause another exception upon a new write)
             * and the dirty bit on success.
             */
            if (!arc)
            {
              arc = DosSetMem((PVOID)page, PAGE_SIZE, m->dosFlags & ~PAG_WRITE);
              TRACE_IF(arc, "DosSetMem = %ld\n", arc);
              if (!arc)
                m->sh->dirty[i] &= ~bit;
            }
          }

          DosExitMustComplete(&nesting);
        }
      }
    }
    base += PAGE_SIZE * DIRTYMAP_WIDTH;
  }
}

/**
 * Writes ditry pages in shared mappings of this process to their respecitve
 * files. Returns 0 if all shared mappings were processed and 1 if there are
 * some mappings this process doesn't have access to.
 */
static int flush_own_mappings()
{
  struct MemMap *m;
  APIRET arc;
  pid_t pid = getpid();
  int seen_foreign = 0;

  m = gpData->mmaps;
  while (m)
  {
    struct MemMap *n = m->next;
    if (m->dosFlags & PAG_WRITE && m->fd != -1)
    {
      /* It's a writable shared mapping */
      ULONG len = m->end - m->start;
      ULONG dosFlags;
      if (m->sh->pid != pid)
        arc = DosQueryMem((PVOID)m->start, &len, &dosFlags);
      if (m->sh->pid == pid || (!arc && !(dosFlags & PAG_FREE)))
      {
        /* This mapping is used in this process, flush it */
        flush_dirty_pages(m);
        /* Attach our pid to this mapping if it's unowned */
        if (m->sh->pid == -1)
          m->sh->pid = pid;
      }
      else if (!seen_foreign)
        seen_foreign = 1;
    }
    m = n;
  }

  TRACE("seen_foreign %d\n", seen_foreign);

  return seen_foreign;
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
  pid_t pid = getpid();

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
      else
      {
        /* Detach our pid from this mapping */
        if (m->sh->pid == pid)
          m->sh->pid = -1;
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

static void mmap_flush_thread(void *arg)
{
  APIRET arc;

  TRACE("Started\n");

  while (1)
  {
    arc = DosWaitEventSem(gpData->mmap->flush_sem, SEM_INDEFINITE_WAIT);
    TRACE_IF(arc, "DosWaitEventSem = %ld\n", arc);

    global_lock();

    /* Only go further if the request hasn't been handled by another thread yet */
    if (gpData->mmap->flush_request)
    {
      TRACE("got flush request\n");

      if (flush_own_mappings() == 1)
      {
        /*
         * There are mappings we can't flush, so wake up another PID's flush
         * thread.
         */
        arc = DosPostEventSem(gpData->mmap->flush_sem);
        TRACE_IF(arc, "DosPostEventSem = %ld\n", arc);

        /*
         * Now wait until other workers handle all the remaining ones (to
         * avoid giving timeslices to us over and over again).
         */
        global_unlock();

        arc = DosWaitEventSem(gpData->mmap->flush_coop_sem, SEM_INDEFINITE_WAIT);
        TRACE_IF(arc, "DosWaitEventSem = %ld\n", arc);

        continue;
      }
      else
      {
        /*
         * We are the thread that finishes completion this flush reques,
         * reset the flag and wake the ones waiting on completion (see above).
         */
        gpData->mmap->flush_request = 0;
      }
    }

    global_unlock();
  }

  TRACE("Stopped\n");
}

/**
 * Initializes the mmap structures.
 * Called after successfull gpData allocation and gpData->heap creation.
 */
void mmap_init()
{
  struct ProcDesc *desc;
  APIRET arc;

  if (gpData->refcnt == 1)
  {
    /* We are the first processs, initialize global mmap structures */
    GLOBAL_NEW(gpData->mmap);
    assert(gpData->mmap);

    arc = DosCreateEventSem(NULL, &gpData->mmap->flush_sem,
                            DC_SEM_SHARED | DCE_POSTONE, FALSE);
    TRACE("DosCreateEventSem = %d\n", arc);
    assert(arc == NO_ERROR);

    arc = DosCreateEventSem(NULL, &gpData->mmap->flush_coop_sem,
                            DC_SEM_SHARED | DCE_AUTORESET, FALSE);
    TRACE("DosCreateEventSem = %d\n", arc);
    assert(arc == NO_ERROR);
  }
  else
  {
    assert(gpData->mmap);
    assert(gpData->mmap->flush_sem);

    arc = DosOpenEventSem(NULL, &gpData->mmap->flush_sem);
    TRACE("DosOpenEventSem = %d\n", arc);
    assert(arc == NO_ERROR);

    arc = DosOpenEventSem(NULL, &gpData->mmap->flush_coop_sem);
    TRACE("DosOpenEventSem = %d\n", arc);
    assert(arc == NO_ERROR);
  }

  /* Initialize our part of ProcDesc */
  desc = get_proc_desc(getpid());
  assert(desc);

  GLOBAL_NEW(desc->mmap);
  assert(desc->mmap);
  desc->mmap->flush_tid = -1;
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
  pid_t pid = getpid();

  /* Free all private mmap structures */
  desc = find_proc_desc(pid);
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
    free(desc->mmap);
  }

  /* Dereference and free shared map structures when appropriate */
  pm = NULL;
  m = gpData->mmaps;
  while (m)
  {
    struct MemMap *n = m->next;
    ULONG len = m->end - m->start;
    ULONG dosFlags;
    if (m->sh->pid != pid)
      arc = DosQueryMem((PVOID)m->start, &len, &dosFlags);
    if (m->sh->pid == pid || (!arc && !(dosFlags & PAG_FREE)))
    {
      /* This mapping is used in this process, release it */
      TRACE("Releasing shared mapping %p (start %p, refcnt %d)\n", m, m->start, m->sh->refcnt);
      if (m->dosFlags & PAG_WRITE && m->fd != -1)
        flush_dirty_pages(m);
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
      else
      {
        /* Detach our pid from this mapping */
        if (m->sh->pid == pid)
          m->sh->pid = -1;
      }
    }
    m = n;
  }

  TRACE_IF(gpData->refcnt == 0, "gpData->mmaps = %p\n", gpData->mmaps);
  assert(gpData->refcnt > 0 || gpData->mmaps == NULL);

  arc = DosCloseEventSem(gpData->mmap->flush_coop_sem);
  if (arc == ERROR_SEM_BUSY)
  {
    /* The semaphore may be owned by us, try to release it */
    arc = DosPostEventSem(gpData->mmap->flush_coop_sem);
    TRACE("DosPostEventSem = %ld\n", arc);
    arc = DosCloseEventSem(gpData->mmap->flush_coop_sem);
  }
  TRACE("DosCloseEventSem = %ld\n", arc);

  arc = DosCloseEventSem(gpData->mmap->flush_sem);
  if (arc == ERROR_SEM_BUSY)
  {
    /* The semaphore may be owned by us, try to release it */
    arc = DosPostEventSem(gpData->mmap->flush_sem);
    TRACE("DosPostEventSem = %ld\n", arc);
    arc = DosCloseEventSem(gpData->mmap->flush_sem);
  }
  TRACE("DosCloseEventSem = %ld\n", arc);

  if (gpData->refcnt == 0)
  {
    /* We are the last process, free global mmap structures */
    free(gpData->mmap);
  }
}

static void schedule_flush_dirty()
{
  struct ProcDesc *desc;
  APIRET arc;
  pid_t pid = getpid();

  TRACE("flush_request %d\n", !!gpData->mmap->flush_request);

  desc = find_proc_desc(pid);
  assert(desc);

  if (desc->mmap->flush_tid == -1)
  {
    /* Lazily start a worker thread */
    desc->mmap->flush_tid = _beginthread(mmap_flush_thread, NULL, 0, NULL);
    TRACE_IF(desc->mmap->flush_tid == -1, "_beginthread = %s\n", strerror(errno));
    assert(desc->mmap->flush_tid != -1);
  }

  if (gpData->mmap->flush_request)
    return;

  gpData->mmap->flush_request = 1;

  arc = DosAsyncTimer(FLUSH_DELAY, (HSEM)gpData->mmap->flush_sem, NULL);
  TRACE_IF(arc, "DosAsyncTimer = %ld\n", arc);
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
                schedule_flush_dirty();
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
                !(dosFlags & PAG_WRITE) &&
                m->flags & MAP_SHARED && m->dosFlags & PAG_WRITE && m->fd != -1)
            {
              /*
               * First write access to a writable shared mapping page. Mark the
               * page as dirty and set PAG_WRITE to let the app continue (also
               * see above).
               */
              arc = DosSetMem((PVOID)pageAddr, len, m->dosFlags);
              TRACE_IF(arc, "DosSetMem = %ld\n", arc);
              if (!arc)
              {
                ULONG pn = (pageAddr - m->start) / PAGE_SIZE;
                size_t i = pn / DIRTYMAP_WIDTH;
                uint32_t bit = 0x1 << (pn % DIRTYMAP_WIDTH);
                m->sh->dirty[i] |= bit;
                TRACE("Marked bit 0x%x at idx %u as dirty\n", bit, i);
                schedule_flush_dirty();

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
  APIRET arc;
  struct MemMap *m;

  if (enmOperation != __LIBC_FORK_OP_FORK_PARENT)
    return 0;

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
