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

/* Width of a dirty map entry in bits */
#define DIRTYMAP_WIDTH (sizeof(*((struct MemMap*)0)->dirty) * 8)

/* Flush operation start delay (ms) */
#define FLUSH_DELAY 1000

/**
 * Per-process data for memory mappings.
 */
typedef struct ProcMemMap
{
  int flush_tid; /* Flush thread */
  HEV flush_sem; /* Semaphore for flush thread */
  int flush_request; /* 1 - semaphore is posted */
} ProcMemMap;

/**
 * File mapping.
 */
typedef struct FileMap
{
  FileDesc *desc; /* associated file desc */
  ULONG start; /* start address */
  off_t len; /* file length */
  int refcnt; /* number of MemMap entries using it */
} FileMap;

/**
 * Memory mapping (linked list entry).
 */
typedef struct MemMap
{
  struct MemMap *next;

  ULONG start; /* start address */
  ULONG end; /* end address (exclusive) */
  int flags; /* mmap flags */
  ULONG dos_flags; /* DosAllocMem protection flags */
  FileMap *fmap; /* file map or NULL for MAP_ANONYMOUS */
  HFILE fd; /* file handle (descriptor in LIBC) or -1 for MAP_ANONYMOUS */
  off_t off; /* offset into the file or 0 for MAP_ANONYMOUS */
  uint32_t dirty[0]; /* Bit array of dirty pages (must be last!) */
} MemMap;

#define OBJ_MY_SHARED 0x80000000

static APIRET DosMyAllocMem(PPVOID addr, ULONG size, ULONG flags)
{
  APIRET arc;

  assert(!(flags & OBJ_ANY));

  if (flags & OBJ_MY_SHARED)
  {
    flags &= ~OBJ_MY_SHARED;
    arc = DosAllocSharedMem(addr, NULL, size, flags | OBJ_ANY);
    TRACE("DosAllocSharedMem(OBJ_ANY) = %lu\n", arc);
    if (arc)
    {
      /* High memory may be unavailable, try w/o OBJ_ANY */
      arc = DosAllocSharedMem(addr, NULL, size, flags);
      TRACE("DosAllocSharedMem = %lu\n", arc);
    }
  }
  else
  {
    arc = DosAllocMem(addr, size, flags | OBJ_ANY);
    TRACE("DosAllocMem(OBJ_ANY) = %lu\n", arc);
    if (arc)
    {
      /* High memory may be unavailable, try w/o OBJ_ANY */
      arc = DosAllocMem(addr, size, flags);
      TRACE("DosAllocMem = %lu\n", arc);
    }
  }

  return arc;
}

static void free_file_map(FileMap *fmap)
{
  APIRET arc;

  assert(!fmap->refcnt);

  if (fmap->start)
  {
    arc = DosFreeMem((PVOID)fmap->start);
    TRACE("DosFreeMem = %ld\n", arc);
    assert(!arc);
  }
  if (fmap->desc)
  {
    assert(fmap->desc->map == fmap);
    fmap->desc->map = NULL;
  }
  free(fmap);
}

void *mmap(void *addr, size_t len, int prot, int flags,
           int fildes, off_t off)
{
  APIRET arc;
  MemMap *mmap;
  HFILE fd = -1;
  size_t dirtymap_sz = 0;
  FileMap *fmap = NULL;
  FileDesc *fdesc = NULL;
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
      !PAGE_ALIGNED(off) ||
      len == 0)
  {
    errno = EINVAL;
    return MAP_FAILED;
  }

  /* Technical overflow */
  if (off + len > 0xFFFFFFFF)
  {
    errno = EOVERFLOW;
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

    global_lock();

    /* Get the associated file descriptor that stores fmap */
    fdesc = get_proc_file_desc(flags & MAP_SHARED ? 0 : getpid(), pFH->pszNativePath);
    if (!fdesc)
    {
      global_unlock();
      errno = ENOMEM;
      return MAP_FAILED;
    }

    /* Make own file handle since POSIX allows the original one to be closed */
    arc = DosDupHandle(fildes, &fd);
    TRACE_IF(arc, "DosDupHandle = %lu\n", arc);
    if (arc)
    {
      global_unlock();
      errno = EMFILE;
      return MAP_FAILED;
    }

    fmap = fdesc->map;
    if (!fmap)
    {
      /* Need a new file map */
      FILESTATUS3L st;
      ULONG mode;
      ULONG fmap_flags;

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
        DosClose(fd);
        global_unlock();
        errno = ENODEV;
        return MAP_FAILED;
      }

      arc = DosQueryFileInfo(fd, FIL_STANDARDL, &st, sizeof(st));
      if (arc)
      {
        DosClose(fd);
        global_unlock();
        errno = EOVERFLOW;
        return MAP_FAILED;
      }

      TRACE("dup fd %ld, file size %llu\n", fd, st.cbFile);

      /* Create a file map struct */
      GLOBAL_NEW(fmap);
      if (!fmap)
      {
        DosClose(fd);
        global_unlock();
        errno = ENOMEM;
        return MAP_FAILED;
      }

      fmap->len = st.cbFile;

      fmap_flags = PAG_READ | PAG_WRITE | PAG_EXECUTE;
      if (flags & MAP_SHARED)
      {
        /*
         * Don't set PAG_WRITE to cause an exception upon the first
         * write to a page (see mmap_exception()).
         */
        fmap_flags &= ~PAG_WRITE;
        fmap_flags |= OBJ_MY_SHARED | OBJ_GIVEABLE | OBJ_GETTABLE;
      }
      arc = DosMyAllocMem((PPVOID)&fmap->start, st.cbFile, fmap_flags);
      if (arc)
      {
        free_file_map(fmap);
        DosClose(fd);
        global_unlock();
        errno = ENOMEM;
        return MAP_FAILED;
      }
    }

    if (off + len > PAGE_ALIGN(fmap->len + PAGE_SIZE - 1))
    {
      /*
       * @todo Since we return part of the file map region to the caller, we
       * can't go beyond EOF more than for the remaining length of the last
       * page. Strictly speaking, it's a violation of POSIX, see
       * https://github.com/bitwiseworks/libcx/issues/20 for more info. For now
       * we simpl return a mmap failure.
       */
      TRACE("requested len %u is far beyond EOF\n", len);
      if (!fdesc->map)
      {
        free_file_map(fmap);
        DosClose(fd);
      }
      global_unlock();
      errno = EOVERFLOW;
      return MAP_FAILED;
    }
  }
  else
  {
    global_lock();
  }

  /*
   * Calculate the number of bytes (in 4 byte words) needed for the dirty page
   * bitmap (this only includes pages prior to EOF). Note that the dirty map
   * is only needed for writable shared mappings bound to files.
   */
  if (fmap && flags & MAP_SHARED && prot & PROT_WRITE)
  {
    dirtymap_sz = DIVIDE_UP(NUM_PAGES(len), DIRTYMAP_WIDTH) * (DIRTYMAP_WIDTH / 8);
    TRACE("dirty map size %u bytes\n", dirtymap_sz);
  }

  /* Allocate a new MemMap entry */
  GLOBAL_NEW_PLUS(mmap, dirtymap_sz);
  if (!mmap)
  {
    /* Free fmap if it's not used */
    if (fmap && !fdesc->map)
    {
      free_file_map(fmap);
      DosClose(fd);
    }
    global_unlock();
    errno = ENOMEM;
    return MAP_FAILED;
  }

  mmap->flags = flags;
  mmap->fmap = fmap;
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

  if (fmap)
  {
    /* Use the file map */
    mmap->start = fmap->start + off;
    mmap->end = mmap->start + len;
    if (flags & MAP_SHARED && fdesc->map)
    {
      /* This is an existing file map, check if it's mapped in this process */
      ULONG fmap_flags, fmap_len = fmap->len;

      arc = DosQueryMem((PVOID)fmap->start, &fmap_len, &fmap_flags);
      TRACE_IF(arc, "DosQueryMem = %ld\n", arc);
      assert(!arc);
      if (fmap_flags & PAG_FREE)
      {
        /*
         * Get access the file map region. Don't set PAG_WRITE to cause an
         * exception upon the first write to a page(see mmap_exception()).
         */
        arc = DosGetSharedMem((PVOID)fmap->start, PAG_READ | PAG_EXECUTE);
        TRACE("DosGetSharedMem = %lu\n", arc);
        assert(!arc);
      }
    }

    arc = NO_ERROR;
  }
  else
  {
    /* Allocate an anonymous map */
    if (flags & MAP_SHARED)
      dos_flags |= OBJ_MY_SHARED | OBJ_GIVEABLE;
    arc = DosMyAllocMem((PPVOID)&mmap->start, len, dos_flags);
    if (!arc)
      mmap->end = mmap->start + len;
  }

  /* Check for a memory allocation failure */
  if (arc)
  {
    free(mmap);
    /* Free fmap if it's not used */
    if (fmap && !fdesc->map)
    {
      free_file_map(fmap);
      DosClose(fd);
    }
    global_unlock();
    errno = ENOMEM;
    return MAP_FAILED;
  }

  if (fmap)
  {
    /* Reference the file map as it is used now */
    if (!fdesc->map)
    {
      /* If it's a newly allocated fmap entry, associate it with its file desc */
      fmap->refcnt = 1;
      fmap->desc = fdesc;
      fdesc->map = fmap;
    }
    else
    {
      ++(fmap->refcnt);
      assert(fmap->refcnt);
      assert(fmap->desc == fdesc);
      assert(fdesc->map == fmap);
    }
  }

  TRACE("mmap %p, mmap->start %lx, mmap->end %lx\n", mmap, mmap->start, mmap->end);
  TRACE_IF(fmap, "fmap %p, fmap->start %lx, fmap->len %llu\n",
           fmap, fmap->start, (uint64_t)fmap->len);

  /* Check if aligned to page size */
  assert(mmap->start && PAGE_ALIGNED(mmap->start));

  ProcDesc *desc = find_proc_desc(getpid());
  assert(desc);

  mmap->next = desc->mmaps;
  desc->mmaps = mmap;

  global_unlock();

  return (void *)mmap->start;
}

/**
 * Searches for a mapping containing the given address range of a process.
 * If @a desc is NULL, the current process is assumed. Returns the found
 * mapping or NULL. Returns the previous mapping in the list if @a pm_out
 * is not NULL. Must be called from under global_lock().
 */
static MemMap *find_mmap(ProcDesc *desc, ULONG addr, ULONG addr_end, MemMap **pm_out)
{
  MemMap *m = NULL, *pm = NULL;

  if (!desc)
  {
    desc = find_proc_desc(getpid());
    assert(desc);
  }

  m = desc->mmaps;
  while (m && !(m->start <= addr && m->end >= addr_end))
  {
    pm = m;
    m = m->next;
  }

  TRACE_IF(!m, "mapping not found\n");
  TRACE_IF(m, "found m %p (start %lx, end %lx, flags %x=%c%c%c%c, dos_flags %lx, fmap %p, off %llu)\n",
           m, m->start, m->end, m->flags,
           m->flags & MAP_SHARED ? 'S' : '-',
           m->flags & MAP_PRIVATE ? 'P' : '-',
           m->flags & MAP_FIXED ? 'F' : '-',
           m->flags & MAP_ANON ? 'A' : '-',
           m->dos_flags, m->fmap, (uint64_t)m->off);

  if (pm_out)
    *pm_out = pm;

  return m;
}

/**
 * Flush dirty pages of the given mapping to the underlying file. If @a off
 * is not 0, it specifies the offest of the first page to flush from the
 * beginning of the given mapping. If @a len is not 0, it specifies the length
 * of the region to flush. If it is 0, all pages up to the end of the mapping
 * are flushed.
 */
static void flush_dirty_pages(MemMap *m, ULONG off, ULONG len)
{
  TRACE("m %p, off %lu, len %lu\n", m, off, len);
  assert(m && m->flags & MAP_SHARED && m->dos_flags & PAG_WRITE && m->fmap);
  assert(off + len <= (m->end - m->start));

  ULONG page, end;
  size_t i, j;
  uint32_t bit;
  APIRET arc;

  off = PAGE_ALIGN(off);
  if (!len)
    len = m->end - m->start - off;

  /* Return early if offset is completely beyond EOF */
  if (off + m->off >= m->fmap->len)
    return;

  /* Make sure we don't sync beyond EOF */
  if (off + m->off + len > m->fmap->len)
    len = m->fmap->len - off - m->off;

  page = m->start + off;
  end = page + len;
  i = (off / PAGE_SIZE) / DIRTYMAP_WIDTH;
  j = (off / PAGE_SIZE) % DIRTYMAP_WIDTH;
  bit = 0x1 << j;

  for (; page < end; ++i, j = 0, bit = 0x1)
  {
    /* Check a block of DIRTYMAP_WIDTH pages at once to quickly skip clean ones */
    if (!m->dirty[i])
    {
      page += PAGE_SIZE * (DIRTYMAP_WIDTH - j);
    }
    else
    {
      for (; page < end && bit; page += PAGE_SIZE, bit <<= 1)
      {
        if (m->dirty[i] & bit)
        {
          ULONG nesting, write;
          LONGLONG pos;

          pos = page - m->start + m->off;
          write = page + PAGE_SIZE <= end ? PAGE_SIZE : end - page;

          TRACE("Writing %lu bytes from addr %lx to fd %d at offset %llu\n",
                write, page, m->fd, pos);

          /*
           * Make sure we are not interrupted by process termination and other
           * async signals in the middle of writing out the dirty page and
           * resetting the dirty bit to have them in sync. Note that it's OK to
           * be interrupted prior to writing out all dirty pages because the
           * remaining ones will be written out from mmap_term() on thread 1 at
           * process termination anyway, given that the dirty bit correctly
           * reflects their state.
           */
          DosEnterMustComplete(&nesting);

          arc = DosSetFilePtrL(m->fd, pos, FILE_BEGIN, &pos);
          TRACE_IF(arc, "DosSetFilePtrL = %ld\n", arc);
          assert(!arc);
          if (!arc)
          {
            arc = DosWrite(m->fd, (PVOID)page, write, &write);
            TRACE_IF(arc, "DosWrite = %ld\n", arc);
            assert(!arc);
            /*
             * Reset PAG_WRITE (to cause another exception upon a new write)
             * and the dirty bit on success.
             */
            arc = DosSetMem((PVOID)page, PAGE_SIZE, m->dos_flags & ~PAG_WRITE);
            TRACE_IF(arc, "DosSetMem = %ld\n", arc);
            assert(!arc);
            m->dirty[i] &= ~bit;
          }

          DosExitMustComplete(&nesting);
        }
      }
    }
  }
}

/**
 * Releases and frees the given mapping.
 * If @a pm is not NULL, it must specify the previous list entry and
 * if @a head is not NULL, it must specify a variable storing the head
 * of the list.
 */
static void release_mapping(MemMap *m, MemMap *pm, MemMap **head)
{
  APIRET arc;

  assert(m);

  TRACE("%s mapping %p (start %lx, end %lx, fmap %p, fmap->start %lx fmap->refcnt %d)\n",
        m->flags & MAP_SHARED ? "Shared" : "Private",
        m, m->start, m->end, m->fmap, m->fmap ? m->fmap->start : 0, m->fmap ? m->fmap->refcnt : 0);

  if (m->flags & MAP_SHARED && m->dos_flags & PAG_WRITE && m->fmap)
    flush_dirty_pages(m, 0, 0);

  if (m->fmap)
  {
    /* Free the file map if we are the last user */
    assert(m->fmap->refcnt);
    --(m->fmap->refcnt);
    if (!m->fmap->refcnt)
      free_file_map(m->fmap);
  }
  else
  {
    /* Release process-specific memory */
    arc = DosFreeMem((PVOID)m->start);
    TRACE("DosFreeMem = %ld\n", arc);
    assert(!arc);
  }

  /* Remove this mapping from this process' mapping list */
  if (pm)
    pm->next = m->next;
  else if (head)
    *head = m->next;

  free(m);
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

  ProcDesc *desc;
  MemMap *m = NULL, *pm = NULL;
  pid_t pid = getpid();

  global_lock();

  desc = find_proc_desc(getpid());
  assert(desc);

  m = find_mmap(desc, (ULONG)addr, addr_end, &pm);
  if (!m)
  {
    global_unlock();

    /* POSIX requires to return silently when no matching region is found */
    return 0;
  }

  if (m->start == (ULONG)addr && m->end == addr_end)
  {
    /* Simplest case: the whole region is unmapped */
    release_mapping(m, pm, &desc->mmaps);
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
  ProcDesc *desc;
  MemMap *m;
  APIRET arc;

  TRACE("Started\n");

  desc = (ProcDesc *)arg;

  while (1)
  {
    arc = DosWaitEventSem(desc->mmap->flush_sem, SEM_INDEFINITE_WAIT);
    TRACE_IF(arc, "DosWaitEventSem = %ld\n", arc);

    global_lock();

    if (desc->mmap->flush_request)
    {
      TRACE("got flush request\n");

      m = desc->mmaps;
      while (m)
      {
        if (m->flags & MAP_SHARED && m->dos_flags & PAG_WRITE && m->fmap)
          flush_dirty_pages(m, 0, 0);
        m = m->next;
      }

      desc->mmap->flush_request = 0;
    }

    global_unlock();
  }

  /* Should never reach here: the thread gets killed at process termination */
  TRACE("Stopped\n");
  assert(0);
}

/**
 * Initializes the mmap structures.
 * Called after successfull gpData allocation and gpData->heap creation.
 */
void mmap_init()
{
  ProcDesc *desc;
  APIRET arc;

  /* Initialize our part of ProcDesc */
  desc = get_proc_desc(getpid());
  assert(desc);

  GLOBAL_NEW(desc->mmap);
  assert(desc->mmap);
  desc->mmap->flush_tid = -1;

  /* Note: we need a shared semaphore for DosAsyncTimer */
  arc = DosCreateEventSem(NULL, &desc->mmap->flush_sem, DC_SEM_SHARED | DCE_AUTORESET, FALSE);
  TRACE("DosCreateEventSem = %ld\n", arc);
  assert(arc == NO_ERROR);
}

/**
 * Uninitializes the mmap structures.
 * Called upon each process termination before gpData is uninitialized
 * or destroyed.
 */
void mmap_term()
{
  ProcDesc *desc;
  MemMap *m;
  APIRET arc;

  /* Free our part of ProcDesc */
  desc = find_proc_desc(getpid());
  if (desc)
  {
    m = desc->mmaps;
    while (m)
    {
      MemMap *n = m->next;
      release_mapping(m,  NULL, NULL);
      m = n;
    }
    desc->mmaps = NULL;

    arc = DosCloseEventSem(desc->mmap->flush_sem);
    if (arc == ERROR_SEM_BUSY)
    {
      /* The semaphore may be owned by us, try to release it */
      arc = DosPostEventSem(desc->mmap->flush_sem);
      TRACE("DosPostEventSem = %ld\n", arc);
      arc = DosCloseEventSem(desc->mmap->flush_sem);
    }
    TRACE("DosCloseEventSem = %ld\n", arc);

    free(desc->mmap);
  }
}

/**
 * Schedules a background flush of dirty pages to an underlying file.
 * Set @a immediate to 1 for an immediate request, 0 for a deferred one
 * (delayed by FLUSH_DELAY).
 */
static void schedule_flush_dirty(ProcDesc *desc, int immediate)
{
  APIRET arc;
  pid_t pid = getpid();

  TRACE("immediate %d, flush_request %d\n", immediate, desc->mmap->flush_request);

  if (desc->mmap->flush_tid == -1)
  {
    /* Lazily start a worker thread */
    desc->mmap->flush_tid = _beginthread(mmap_flush_thread, NULL, 0, desc);
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

    if (desc->mmap->flush_request)
    {
      arc = DosQueryEventSem(desc->mmap->flush_sem, &cnt);
      TRACE_IF(arc, "DosQueryEventSem = %ld\n", arc);
      assert(!arc);
      TRACE("cnt %lu\n", cnt);
    }

    if (cnt == 0)
    {
      arc = DosPostEventSem(desc->mmap->flush_sem);
      TRACE_IF(arc, "DosPostEventSem = %ld\n", arc);
      assert(!arc);
    }

    if (!desc->mmap->flush_request)
      desc->mmap->flush_request = 1;
  }
  else if (!desc->mmap->flush_request)
  {
    arc = DosAsyncTimer(FLUSH_DELAY, (HSEM)desc->mmap->flush_sem, NULL);
    TRACE_IF(arc, "DosAsyncTimer = %ld\n", arc);
    assert(!arc);

    desc->mmap->flush_request = 1;
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
    ProcDesc *desc;
    MemMap *m;

    ULONG addr = report->ExceptionInfo[1];

    TRACE("addr %lx, info %lx\n", addr, report->ExceptionInfo[0]);

    global_lock();

    desc = find_proc_desc(getpid());
    if (desc)
      m = find_mmap(desc, addr, addr + 1, NULL);

    /*
     * Note that we only do something if the found mmap is not PROT_NONE and
     * let the application crash otherwise (see also mmap()).
     */
    if (m && m->dos_flags & fPERM)
    {
      APIRET arc;
      ULONG len = PAGE_SIZE;
      ULONG dos_flags;
      ULONG page_addr = PAGE_ALIGN(addr);
      arc = DosQueryMem((PVOID)page_addr, &len, &dos_flags);
      TRACE_IF(arc, "DosQueryMem = %lu\n", arc);
      if (!arc)
      {
        TRACE("dos_flags %lx, len %ld\n", dos_flags, len);
        if (!(dos_flags & (PAG_FREE | PAG_COMMIT)))
        {
          /* First access to the allocated but uncommitted page, commit it */
          int revoke_write = 0;

          dos_flags = m->dos_flags;
          if (m->fmap)
          {
            if (m->flags & MAP_SHARED && m->dos_flags & PAG_WRITE)
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
                m->dirty[i] |= bit;
                TRACE("Marked bit 0x%x at idx %u as dirty\n", bit, i);
                schedule_flush_dirty(desc, 0 /* immediate */);
              }
              else
              {
                /* DosRead needs PAG_WRITE, so schedule removal for later */
                revoke_write = 1;
              }
            }
            else if (!(m->dos_flags & PAG_WRITE))
            {
              /* This is a read-only mapping but DosRead needs PAG_WRITE */
              dos_flags |= PAG_WRITE;
              revoke_write = 1;
            }
          }
          TRACE("Committing page at addr %lx\n", page_addr);
          arc = DosSetMem((PVOID)page_addr, len, dos_flags | PAG_COMMIT);
          TRACE_IF(arc, "DosSetMem = %ld\n", arc);
          if (!arc)
          {
            if (m->fmap)
            {
              /*
               * Read file contents into memory. Note that if we fail here,
               * we simply let the exception go further and hopefully crash
               * and abort the application.
               */
              ULONG read = PAGE_SIZE;
              LONGLONG pos = page_addr - m->start + m->off;
              TRACE("Reading %lu bytes to addr %lx from fd %ld at offset %llu\n",
                    read, page_addr, m->fd, pos);
              arc = DosSetFilePtrL(m->fd, pos, FILE_BEGIN, &pos);
              TRACE_IF(arc, "DosSetFilePtrL = %ld\n", arc);
              if (!arc)
              {
                /* Page is committed, so calling original DosRead is safe. */
                arc = _doscalls_DosRead(m->fd, (PVOID)page_addr, read, &read);
                TRACE_IF(arc, "DosRead = %ld\n", arc);
              }
            }
            if (!arc)
            {
              if (revoke_write)
              {
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
              m->flags & MAP_SHARED && m->dos_flags & PAG_WRITE && m->fmap)
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
              m->dirty[i] |= bit;
              TRACE("Marked bit 0x%x at idx %u as dirty\n", bit, i);
              schedule_flush_dirty(desc, 0 /* immediate */);

              /* We successfully marked the dirty page, let the app retry */
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
  ProcDesc *desc;
  MemMap *m;
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

  global_lock();

  desc = find_proc_desc(getpid());
  assert(desc);

  m = find_mmap(desc, (ULONG)addr, addr_end, NULL);
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
  if (m->flags & MAP_SHARED && m->dos_flags & PAG_WRITE && m->fmap)
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
      schedule_flush_dirty(desc, 1 /* immediate */);
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
  MemMap *m;
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

  m = find_mmap(NULL, (ULONG)addr, addr_end, NULL);
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
  ProcDesc *desc;
  MemMap *m;
  ULONG addr_end, next_addr;
  ULONG dos_flags;
  MemMap **found;
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

  desc = find_proc_desc(getpid());

  next_addr = (ULONG)addr;

  while (next_addr < addr_end)
  {
    m = find_mmap(desc, next_addr, next_addr + 1, NULL);
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

      if (m->fmap)
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

        TRACE("mapping %p, changing dos_flags from %lx to %lx\n",
              m, m->dos_flags, dos_flags);
        m->dos_flags = dos_flags;
      }
    }
  }

  global_unlock();

  free(found);

  return rc;
}

static int forkParentChild(__LIBC_PFORKHANDLE pForkHandle, __LIBC_FORKOP enmOperation)
{
  ProcDesc *desc;
  APIRET arc;
  MemMap *m;

  if (enmOperation == __LIBC_FORK_OP_FORK_PARENT)
  {
    global_lock();

    desc = find_proc_desc(getpid());
    assert(desc);

    /* Give access to shared mappings to the forked child process */
    m = desc->mmaps;
    while (m)
    {
      if (m->flags & MAP_SHARED)
      {
        ULONG start = m->start;
        ULONG dos_flags = m->dos_flags;

        TRACE("giving mapping %p (start %lx, fmap %p, fmap->start %lx) to pid %x\n",
              m, m->start, m->fmap, m->fmap ? m->fmap->start : 0, pForkHandle->pidChild);
        if (m->fmap)
        {
          start = m->fmap->start;
          /*
           * Don't set PAG_WRITE to cause an exception upon the first
           * write to a page (see mmap_exception()).
           */
          dos_flags = PAG_READ | PAG_EXECUTE;
        }
        /*
         * Note that if there is more than 1 user of the file map, we will give
         * the same region multiple times, but this should not hurt.
         */
        arc = DosGiveSharedMem((PVOID)start, pForkHandle->pidChild, dos_flags);
        TRACE_IF(arc, "DosGiveSharedMem = %ld\n", arc);
        assert(!arc);
      }
      m = m->next;
    }

    global_unlock();
  }
  else if (enmOperation == __LIBC_FORK_OP_FORK_CHILD)
  {
    ProcDesc *pdesc;
    MemMap *newm;
    BOOL ok = TRUE;

    global_lock();

    pdesc = find_proc_desc(pForkHandle->pidParent);
    assert(pdesc);

    desc = find_proc_desc(getpid());
    assert(desc);

    /* Copy parent's shared mappings to our own list */
    m = pdesc->mmaps;
    while (m)
    {
      if (m->flags & MAP_SHARED)
      {
        TRACE("restoring mapping %p (start %lx, fmap %p, fmap->start %lx)\n",
              m, m->start, m->fmap, m->fmap ? m->fmap->start : 0);
        GLOBAL_NEW(newm);
        if (!newm)
        {
          ok = FALSE;
          break;
        }
        /*
         * Copy the fields. Note that the dirty map, if any, will remain reset
         * in the copy as it won't be copied over - this is fine as dirty maps
         * are per process (as well as memory protection flags).
         */
        COPY_STRUCT(newm, m);
        if (newm->fmap)
        {
          /* Reference the file map */
          ++(newm->fmap->refcnt);
          assert(newm->fmap->refcnt);
        }
        /* Add the new entry the list */
        newm->next = desc->mmaps;
        desc->mmaps = newm;
      }
      m = m->next;
    }

    global_unlock();

    /* We have to abort fork() if copying or aliasing some mapping fails */
    if (!ok)
      return -ENOMEM;
  }

  return 0;
}

_FORK_PARENT1(0, forkParentChild);
_FORK_CHILD1(0, forkParentChild);
