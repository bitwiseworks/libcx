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
#include <sys/param.h>
#include <fcntl.h>

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
#define FLUSH_DELAY 1000

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
  int flush_request; /* 1 - semaphore is posted, 2 - waiting for final worker */
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
  ULONG dos_flags; /* DosAllocMem protection flags */
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

void *mmap(void *addr, size_t len, int prot, int flags,
           int fildes, off_t off)
{
  APIRET arc;
  struct MemMap *mmap;
  int fd = -1;
  size_t dirtymap_sz = 0;
  ULONG dos_flags;

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

    /* Check flags/prot compatibility with file access */
    TRACE("file mode %x\n", pFH->fFlags & O_ACCMODE);
    if ((pFH->fFlags & O_ACCMODE) == O_WRONLY ||
        (flags & MAP_SHARED && prot & PROT_WRITE &&
         (pFH->fFlags & O_ACCMODE) != O_RDWR))
    {
      TRACE("Invalid file access mode\n");
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

  mmap->dos_flags = 0;
  if (prot & PROT_READ)
    mmap->dos_flags |= PAG_READ;
  if (prot & PROT_WRITE)
    mmap->dos_flags |= PAG_WRITE;
  if (prot & PROT_EXEC)
    mmap->dos_flags |= PAG_EXECUTE;

  dos_flags = mmap->dos_flags;

  /*
   * Use PAG_READ for PROT_NONE since DosMemAlloc doesn't support no protection
   * mode. We will handle this case in mmap_exception() by refusing to ever
   * commit such a page and letting the process crash (as required by POSIX).
   */
  if (mmap->dos_flags == 0)
    dos_flags |= PAG_READ;

  if (flags & MAP_SHARED)
  {
    /* Shared mmap specific data */
    mmap->sh->pid = getpid();
    mmap->sh->refcnt = 1;

    dos_flags |= OBJ_GIVEABLE;

    arc = DosAllocSharedMem((PPVOID)&mmap->start, NULL, len, dos_flags | OBJ_ANY);
    TRACE("DosAllocSharedMem(OBJ_ANY) = %lu\n", arc);
    if (arc)
    {
      /* High memory may be unavailable, try w/o OBJ_ANY */
      arc = DosAllocSharedMem((PPVOID)&mmap->start, NULL, len, dos_flags);
      TRACE("DosAllocSharedMem = %lu\n", arc);
    }
  }
  else
  {
    arc = DosAllocMem((PPVOID)&mmap->start, len, dos_flags | OBJ_ANY);
    TRACE("DosAllocMem(OBJ_ANY) = %lu\n", arc);
    if (arc)
    {
      /* High memory may be unavailable, try w/o OBJ_ANY */
      arc = DosAllocMem((PPVOID)&mmap->start, len, dos_flags);
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
  TRACE_IF(m, "found m %p in %s (start %p, end %p, flags %x=%c%c%c%c, dos_flags %lx, fd %d, off %llu, pid %x, refcnt %d)\n",
           m, head == &gpData->mmaps ? "SHARED" : "PRIVATE", m->start, m->end, m->flags,
           m->flags & MAP_SHARED ? 'S' : '-',
           m->flags & MAP_PRIVATE ? 'P' : '-',
           m->flags & MAP_FIXED ? 'F' : '-',
           m->flags & MAP_ANON ? 'A' : '-',
           m->dos_flags, m->fd, (uint64_t)m->off,
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

/**
 * Flush dirty pages of the given mapping to the underlying file. If @a off
 * is not 0, it specifies the offest of the first page to flush from the
 * beginning of the given mapping. If @a len is not 0, it specifies the length
 * of the region to flush. If it is 0, all pages up to the end of the mapping
 * are flushed.
 */
static void flush_dirty_pages(struct MemMap *m, ULONG off, ULONG len)
{
  TRACE("m %p, off %lu, len %lu\n", m, off, len);
  assert(m && m->flags & MAP_SHARED && m->dos_flags & PAG_WRITE && m->fd != -1);
  assert(off + len <= (m->end - m->start));

  /* Calculate the number of 4 byte words in the dirty page bitmap */
  size_t dirtymap_len = DIVIDE_UP(NUM_PAGES(m->end - m->start), DIRTYMAP_WIDTH);
  ULONG page, end;
  size_t i, j;
  uint32_t bit;
  APIRET arc;

  off = PAGE_ALIGN(off);

  page = m->start + off;
  end = len ? m->start + off + len : m->end;
  i = (off / PAGE_SIZE) / DIRTYMAP_WIDTH;
  j = (off / PAGE_SIZE) % DIRTYMAP_WIDTH;
  bit = 0x1 << j;

  for (; page < end; ++i, j = 0, bit = 0x1)
  {
    /* Check a block of DIRTYMAP_WIDTH pages at once to quickly skip clean ones */
    if (!m->sh->dirty[i])
    {
      page += PAGE_SIZE * (DIRTYMAP_WIDTH - j);
    }
    else
    {
      for (; page < end && bit; page += PAGE_SIZE, bit <<= 1)
      {
        if (m->sh->dirty[i] & bit)
        {
          ULONG nesting, written;
          LONGLONG pos;

          TRACE("Writing %d bytes from addr %p to fd %d at offset %llu\n",
                PAGE_SIZE, page, m->fd, (uint64_t)m->off + (page - m->start));

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
              arc = DosSetMem((PVOID)page, PAGE_SIZE, m->dos_flags & ~PAG_WRITE);
              TRACE_IF(arc, "DosSetMem = %ld\n", arc);
              if (!arc)
                m->sh->dirty[i] &= ~bit;
            }
          }

          DosExitMustComplete(&nesting);
        }
      }
    }
  }

  /*
   * Attach our pid to this mapping if it's currently unowned. This is used to
   * optimize the detection of access in is_shared_accessible().
   */
  if (m->sh->pid == -1)
    m->sh->pid = getpid();
}

/**
 * Returns 1 if the given shared mapping is used in this process and 0
 * otherwise.
 */
static int is_shared_accessible(struct MemMap *m)
{
  assert(m && m->flags & MAP_SHARED);

  APIRET arc;
  pid_t pid = getpid();
  ULONG len = m->end - m->start;
  ULONG dos_flags;

  if (m->sh->pid == pid)
    return 1;

  arc = DosQueryMem((PVOID)m->start, &len, &dos_flags);
  return !arc && !(dos_flags & PAG_FREE);
}

/**
 * Writes ditry pages in shared mappings of this process to their respecitve
 * files. Returns 0 if all shared mappings were processed and 1 if there are
 * some mappings this process doesn't have access to.
 */
static int flush_own_mappings()
{
  struct MemMap *m;
  int seen_foreign = 0;

  m = gpData->mmaps;
  while (m)
  {
    struct MemMap *n = m->next;
    if (m->dos_flags & PAG_WRITE && m->fd != -1)
    {
      /* It's a writable shared mapping, flush if it's used in this process */
      if (is_shared_accessible(m))
        flush_dirty_pages(m, 0, 0);
      else if (!seen_foreign)
        seen_foreign = 1;
    }
    m = n;
  }

  TRACE("seen_foreign %d\n", seen_foreign);

  return seen_foreign;
}

static void release_mapping(struct MemMap *m, struct MemMap *pm, struct MemMap **head)
{
  APIRET arc;

  assert(m);

  TRACE("%s mapping %p (start %lx, end %lx, refcnt %d)\n",
        m->flags & MAP_SHARED ? "Shared" : "Private",
        m, m->start, m->end, m->sh->refcnt);

  if (m->flags & MAP_SHARED && m->dos_flags & PAG_WRITE && m->fd != -1)
    flush_dirty_pages(m, 0, 0);
  arc = DosFreeMem((PVOID)m->start);
  TRACE("DosFreeMem = %ld\n", arc);
  assert(!arc);

  /* Dereference and free the mapping when appropriate */
  if (m->flags & MAP_SHARED)
  {
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

    if (m->flags & MAP_SHARED && m->dos_flags & PAG_WRITE && m->fd != -1 &&
        gpData->mmap->flush_request == 2)
    {
      /*
       * Some workers are waiting for the flush request to finish,
       * it could be the releted mapping that holding them. Make a kick
       * (this will cause another accesibility check).
       */
      arc = DosPostEventSem(gpData->mmap->flush_coop_sem);
      TRACE_IF(arc, "DosPostEventSem = %ld\n", arc);

      /* Revert the request state back to the normal one (no coop wait) */
      gpData->mmap->flush_request = 1;
    }
  }
  else
  {
    /* Detach our pid from this mapping */
    if (m->sh->pid == getpid())
      m->sh->pid = -1;
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
    TRACE("addr not page-aligned\n");
    errno = EINVAL;
    return -1;
  }

  /* Check if len within bounds */
  if (0 - ((uintptr_t)addr) < len)
  {
    TRACE("len out of bounds\n");
    errno = EINVAL;
    return -1;
  }

  ULONG addr_end = ((ULONG)addr) + len;

  struct MemMap *m = NULL, *pm = NULL;
  struct MemMap **head;
  pid_t pid = getpid();

  global_lock();

  m = find_mmap((ULONG)addr, addr_end, &pm, &head);
  if (!m || (m->flags & MAP_SHARED && !is_shared_accessible(m)))
  {
    global_unlock();

    /* POSIX requires to return silently when no matching region is found */
    return rc;
  }

  if (m->start == (ULONG)addr && m->end == addr_end)
  {
    /* Simplest case: the whole region is unmapped */
    release_mapping(m, pm, head);
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

    if (gpData->mmap->flush_request)
    {
      /* The request hasn't been handled by another thread yet, process it */
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
         * Now go to sleep to give other workers an opportunity to actually
         * pick up the request.
         */
        gpData->mmap->flush_request = 2;

        global_unlock();

        arc = DosWaitEventSem(gpData->mmap->flush_coop_sem, SEM_INDEFINITE_WAIT);
        TRACE_IF(arc, "DosWaitEventSem = %ld\n", arc);

        continue;
      }
      else
      {
        /*
         * We are the thread that finishes execution of this flush request,
         * wake the ones waiting on completion if any (see above) and reset
         * the flag and.
         */
        if (gpData->mmap->flush_request == 2)
        {
          arc = DosPostEventSem(gpData->mmap->flush_coop_sem);
          TRACE_IF(arc, "DosPostEventSem = %ld\n", arc);
        }

        gpData->mmap->flush_request = 0;

      }
    }

    global_unlock();
  }

  /* Should never reach here, the thread is killed at process termination */
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
    free(desc->mmap);
  }

  /* Dereference and free shared map structures when appropriate */
  pm = NULL;
  m = gpData->mmaps;
  while (m)
  {
    struct MemMap *n = m->next;
    if (is_shared_accessible(m))
      release_mapping(m, pm, &gpData->mmaps);
    else
      pm = m;
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

/**
 * Schedules a background flush of dirty pages to the underlying file.
 * @param immediate 1 for an immediate request, 0 for adeferred one
 * (delayed by FLUSH_DELAY).
 */
static void schedule_flush_dirty(int immediate)
{
  struct ProcDesc *desc;
  APIRET arc;
  pid_t pid = getpid();

  TRACE("immediate %d, flush_request %d\n", immediate, gpData->mmap->flush_request);

  desc = find_proc_desc(pid);
  assert(desc);

  if (desc->mmap->flush_tid == -1)
  {
    /* Lazily start a worker thread */
    desc->mmap->flush_tid = _beginthread(mmap_flush_thread, NULL, 0, NULL);
    TRACE_IF(desc->mmap->flush_tid == -1, "_beginthread = %s\n", strerror(errno));
    assert(desc->mmap->flush_tid != -1);
  }

  /*
   * Note: we do nothing if a request is already being delivered: the flush
   * thread will flush all new dirty pages. However, if it's an immediate
   * request, we don't want to wait until a timer is fired and post the
   * semaphore manually if it's not already posted.
   */

  if (immediate)
  {
    ULONG cnt = 0;

    if (gpData->mmap->flush_request)
    {
      arc = DosQueryEventSem(gpData->mmap->flush_sem, &cnt);
      TRACE_IF(arc, "DosQueryEventSem = %ld\n", arc);
      assert(!arc);
      TRACE("cnt %u\n", cnt);
    }

    if (cnt == 0)
    {
      arc = DosPostEventSem(gpData->mmap->flush_sem);
      TRACE_IF(arc, "DosPostEventSem = %ld\n", arc);
      assert(!arc);
    }

    if (!gpData->mmap->flush_request)
      gpData->mmap->flush_request = 1;
  }
  else if (!gpData->mmap->flush_request)
  {
    arc = DosAsyncTimer(FLUSH_DELAY, (HSEM)gpData->mmap->flush_sem, NULL);
    TRACE_IF(arc, "DosAsyncTimer = %ld\n", arc);
    assert(!arc);

    gpData->mmap->flush_request = 1;
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

    TRACE("addr %p, info %lx\n", addr, report->ExceptionInfo[0]);

    global_lock();

    m = find_mmap(addr, addr + 1, NULL, NULL);

    /*
     * Note that we only do something if the found mmap is not PROT_NONE and
     * let the application crash otherwise (see also mmap()).
     */
    if (m && m->dos_flags & fPERM)
    {
      APIRET arc;
      ULONG len = 1;
      ULONG dos_flags;
      ULONG page_addr = PAGE_ALIGN(addr);
      arc = DosQueryMem((PVOID)page_addr, &len, &dos_flags);
      TRACE_IF(arc, "DosQueryMem = %lu\n", arc);
      if (!arc)
      {
        TRACE("dos_flags %lx\n", dos_flags);
        if (!(dos_flags & (PAG_FREE | PAG_COMMIT)))
        {
          /* First access to the allocated but uncommitted page */
          int revoke_write = 0;
          if (m->flags & MAP_SHARED && m->dos_flags & PAG_WRITE && m->fd != -1)
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
              ULONG pn = (page_addr - m->start) / PAGE_SIZE;
              size_t i = pn / DIRTYMAP_WIDTH;
              uint32_t bit = 0x1 << (pn % DIRTYMAP_WIDTH);
              m->sh->dirty[i] |= bit;
              TRACE("Marked bit 0x%x at idx %u as dirty\n", bit, i);
              schedule_flush_dirty(0 /* immediate */);
            }
            else
            {
              /* DosRead needs PAG_WRITE, so schedule removal for later */
              revoke_write = 1;
            }
          }
          arc = DosSetMem((PVOID)page_addr, len, m->dos_flags | PAG_COMMIT);
          TRACE_IF(arc, "DosSetMem = %ld\n", arc);
          if (!arc)
          {
            if (m->fd != -1)
            {
              /*
               * Read file contents into memory. Note that if we fail here,
               * we simply let the exception go further and hopefully crash
               * and abort the application.
               */
              ULONG read;
              LONGLONG pos;
              TRACE("Reading %d bytes to addr %p from fd %d at offset %llu\n",
                    PAGE_SIZE, page_addr, m->fd, (uint64_t)m->off + (page_addr - m->start));
              arc = DosSetFilePtrL(m->fd, m->off + (page_addr - m->start), FILE_BEGIN, &pos);
              TRACE_IF(arc, "DosSetFilePtrL = %ld\n", arc);
              if (!arc)
              {
                arc = DosRead(m->fd, (PVOID)page_addr, PAGE_SIZE, &read);
                TRACE_IF(arc, "DosRead = %ld\n", arc);
              }
            }
            if (!arc)
            {
              if (revoke_write)
              {
                dos_flags = m->dos_flags & ~PAG_WRITE;
                arc = DosSetMem((PVOID)page_addr, len, m->dos_flags & ~PAG_WRITE);
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
        else if (dos_flags & PAG_COMMIT)
        {
          if (report->ExceptionInfo[0] == XCPT_WRITE_ACCESS &&
              !(dos_flags & PAG_WRITE) &&
              m->flags & MAP_SHARED && m->dos_flags & PAG_WRITE && m->fd != -1)
          {
            /*
             * First write access to a writable shared mapping page. Mark the
             * page as dirty and set PAG_WRITE to let the app continue (also
             * see above).
             */
            arc = DosSetMem((PVOID)page_addr, len, m->dos_flags);
            TRACE_IF(arc, "DosSetMem = %ld\n", arc);
            if (!arc)
            {
              ULONG pn = (page_addr - m->start) / PAGE_SIZE;
              size_t i = pn / DIRTYMAP_WIDTH;
              uint32_t bit = 0x1 << (pn % DIRTYMAP_WIDTH);
              m->sh->dirty[i] |= bit;
              TRACE("Marked bit 0x%x at idx %u as dirty\n", bit, i);
              schedule_flush_dirty(0 /* immediate */);

              retry = 1;
            }
          }
        }
      }
    }

    global_unlock();

    TRACE("%sretrying\n", retry ? "" : "not ");
  }

  return retry;
}

int msync(void *addr, size_t len, int flags)
{
  APIRET arc;

  TRACE("addr %p, len %u, flags %x=%c%c\n", addr, len,
        flags,
        (flags & 0x1) == MS_SYNC ? 'S' : (flags & 0x1) == MS_ASYNC ? 'A' : '-',
        flags & MS_INVALIDATE ? 'I' : '-');

  /* Check if aligned to page size */
  if (!PAGE_ALIGNED(addr))
  {
    TRACE("addr not page-aligned\n");
    errno = EINVAL;
    return -1;
  }

  /* Check if len within bounds */
  if (0 - ((uintptr_t)addr) < len)
  {
    TRACE("len out of bounds\n");
    errno = ENOMEM; /* Differs from munmap but per POSIX */
    return -1;
  }

  ULONG addr_end = ((ULONG)addr) + len;

  struct MemMap *m;

  global_lock();

  m = find_mmap((ULONG)addr, addr_end, NULL, NULL);
  if (!m)
  {
    global_unlock();

    errno = ENOMEM;
    return -1;
  }

  /*
   * Only do real work on shared writable file mappings and silently
   * ignore other types (POSIX doesn't specify any particular behavior
   * in such a case).
   */
  if (m->flags & MAP_SHARED && m->dos_flags & PAG_WRITE && m->fd != -1)
  {
    if ((flags & 0x1) == MS_ASYNC)
    {
      /*
       * We could add the requested region to some list and ask
       * mmap_flush_thread() to flush only it, but that doesn't make too much
       * practical sense given that the request is served asynchronously so that
       * the caller doesn't know when it exactly happens anyway. It looks more
       * practical to simply forse a complete flush which may take longer
       * but will save some cycles on doing one flush loop instead of two.
       */
      schedule_flush_dirty(1 /* immediate */);
    }
    else
    {
      flush_dirty_pages(m, (ULONG)addr - m->start, len);
    }
  }

  global_unlock();

  return 0;
}

int madvise(void *addr, size_t len, int flags)
{
  struct MemMap *m;
  ULONG addr_end;
  APIRET arc;
  int rc = 0;

  TRACE("addr %p, len %u, flags %x=%c\n", addr, len,
        flags,
        flags & MADV_DONTNEED ? 'D' : '-');

  /* Check if aligned to page size */
  if (!PAGE_ALIGNED(addr))
  {
    TRACE("addr not page-aligned\n");
    errno = EINVAL;
    return -1;
  }

  /* Check if len within bounds */
  if (0 - ((uintptr_t)addr) < len)
  {
    TRACE("len out of bounds\n");
    errno = ENOMEM; /* Differs from munmap but per POSIX */
    return -1;
  }

  addr_end = ((ULONG)addr) + len;

  global_lock();

  m = find_mmap((ULONG)addr, addr_end, NULL, NULL);
  if (!m)
  {
    global_unlock();

    errno = ENOMEM;
    return -1;
  }

  if (flags & MADV_DONTNEED)
  {
    ULONG query_len = len;
    ULONG dos_flags;
    arc = DosQueryMem(addr, &query_len, &dos_flags);
    TRACE_IF(arc, "DosQueryMem = %lu\n", arc);
    if (!arc)
    {
      TRACE("dos_flags %lx\n", dos_flags);
      if (dos_flags & PAG_COMMIT)
      {
        /*
         * @todo On OS/2 you can't uncommit shared memory (or remove PAG_READ
         * from it) so this will always fail on shared mappings.
         */
        arc = DosSetMem(addr, len, PAG_DECOMMIT);
        TRACE_IF(arc, "DosSetMem = %ld\n", arc);
        if (arc)
        {
          errno = EINVAL;
          rc = -1;
        }
      }
    }
  }

  global_unlock();

  return rc;
}

int posix_madvise(void *addr, size_t len, int advice)
{
  TRACE("addr %p, len %u, advice %x\n", addr, len, advice);

  /* Check if aligned to page size */
  if (!PAGE_ALIGNED(addr))
  {
    TRACE("addr not page-aligned\n");
    errno = EINVAL;
    return -1;
  }

  /* Check if len within bounds */
  if (0 - ((uintptr_t)addr) < len)
  {
    TRACE("len out of bounds\n");
    errno = ENOMEM; /* Differs from munmap but per POSIX */
    return -1;
  }

  /*
   * This is barely a no-op. Note that although it also has POSIX_MADV_DONTNEED
   * which sounds similar to MADV_DONTNEED, these advices are very different in
   * that the POSIX version does NOT change access semantics as opposed to
   * madvise(). For this reason, POSIX_MADV_DONTNEED is also a no-op on OS/2.
   */
  return 0;
}

int mprotect(const void *addr, size_t len, int prot)
{
  struct MemMap *m;
  ULONG addr_end, next_addr;
  ULONG dos_flags;
  struct MemMap **found;
  int found_size, found_cnt = 0;
  int rc = 0;

  TRACE("addr %p, len %u, prot %x\n", addr, len, prot);

  /* Check if aligned to page size */
  if (!PAGE_ALIGNED(addr))
  {
    TRACE("addr not page-aligned\n");
    errno = EINVAL;
    return -1;
  }

  /* Check if len within bounds */
  if (0 - ((uintptr_t)addr) < len)
  {
    TRACE("len out of bounds\n");
    errno = ENOMEM; /* Differs from munmap but per POSIX */
    return -1;
  }

  found_size = 16;
  NEW_ARRAY(found, found_size);
  assert(found);

  addr_end = ((ULONG)addr) + len;

  dos_flags = 0;
  if (prot & PROT_READ)
    dos_flags |= PAG_READ;
  if (prot & PROT_WRITE)
    dos_flags |= PAG_WRITE;
  if (prot & PROT_EXEC)
    dos_flags |= PAG_EXECUTE;

  global_lock();

  next_addr = (ULONG)addr;

  while (next_addr < addr_end)
  {
    m = find_mmap(next_addr, next_addr + 1, NULL, NULL);
    if (m)
    {
      /*
       * We prevent changing protection for mappings backed up by files.
       * First, because their protection is bound to the underlying file's
       * access mode (that was checked and confirmed at creation). Second,
       * because we already (ab)use protection flags on our own (to implement
       * on-demand page read semantics as well as asynchronous dirty page
       * writes) so messing up with them makes things too complex to manage.
       * Third, _std_mprotect() is written so that it commits pages unless
       * PAG_NONE is requested and this will also break things up (e.g.
       * on-demand page reading). And forth, it makes little sense to change
       * permissions of such mappings in real life (at least that's the case
       * in the absense of real life examples).
       *
       * @todo We may re-consider things later given that on e.g. Linux
       * it is possible to change protection of such mappings (though the
       * consequences of such a change are not fully documented, testing is
       * needed). But this will at least require to drop _std_mprotect usage
       * to avoid unnecessary committing of pages in the given range that
       * belong to file-bound mappings.
       *
       * Note also that although we let mprotect change protection of shared
       * anonymous mappings, it's not actually possible to set PROT_NONE
       * on them because a shared page can't be decommitted on OS/2 (and
       * protection can't be set to "no read, no write" for any kind of pages
       * there, both private and shared, either). We will simply return EINVAL
       * in such a case.
       */

      if (m->fd != -1)
      {
        errno = EACCES;
        rc = -1;
        break;
      }

      if (m->dos_flags != dos_flags)
      {
        /* Checks passed, store it for further processing */
        if (found_cnt == found_size)
        {
          found_size *= 2;
          RENEW_ARRAY(found, found_size);
          assert(found);
        }
        found[found_cnt++] = m;
      }

      next_addr = PAGE_ALIGN(m->end + PAGE_SIZE - 1);
    }
    else
    {
      next_addr += PAGE_SIZE;
    }
  }

  TRACE("found %d affected mappings, rc %d (%s)\n",
        found_cnt, rc, strerror(rc == -1 ? errno : 0));

  if (!rc)
  {
    /*
     * Now, let LIBC mprotect do the job if no affected mappings were found
     * at all or if all affected mappings passed mode change checks.
     */
    rc = _std_mprotect(addr, len, prot);
    if (rc == -1 && errno < 0)
    {
       /*
        * There is a bug in _std_mprotect of LIBC <= 0.6.6: it sets a
        * negative errno, fix it here.
        */
      errno = -errno;
    }
    TRACE("_std_mprotect = %d (%s)\n", rc, strerror(rc == -1 ? errno: 0));

    if (!rc && found_cnt)
    {
      /* Record new mode in affected mappings on success */
      while (found_cnt)
      {
        m = found[--found_cnt];

        TRACE("mapping %p, changing dos_flags from %x to %x\n",
              m, m->dos_flags, dos_flags);
        m->dos_flags = dos_flags;
      }
    }
  }

  global_unlock();

  free(found);

  return rc;
}

static int forkParent(__LIBC_PFORKHANDLE pForkHandle, __LIBC_FORKOP enmOperation)
{
  APIRET arc;
  struct MemMap *m;

  if (enmOperation != __LIBC_FORK_OP_FORK_PARENT)
    return 0;

  global_lock();

  /* Give shared mappings that we have access to to our forked child process */
  m = gpData->mmaps;
  while (m)
  {
    if (is_shared_accessible(m))
    {
      ULONG dos_flags = m->dos_flags;
      TRACE("giving mapping %p (start %p) to pid %x\n", m, m->start, pForkHandle->pidChild);
      if (m->dos_flags & PAG_WRITE && m->fd != -1)
      {
        /*
         * This is a writable shared mapping, remove PAG_WRITE to get an exception
         * on the first write in child (see mmap_exception above).
         */
        dos_flags &= ~PAG_WRITE;
      }
      arc = DosGiveSharedMem((PVOID)m->start, pForkHandle->pidChild, dos_flags);
      TRACE_IF(arc, "DosGiveSharedMem = %ld\n", arc);
      assert(!arc);
      ++(m->sh->refcnt);
    }
    m = m->next;
  }

  global_unlock();

  return 0;
}

_FORK_PARENT1(0, forkParent);
