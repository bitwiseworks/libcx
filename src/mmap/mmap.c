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
#define DIRTYMAP_WIDTH (sizeof(*((struct FileDesc*)0)->p->fh->dirtymap) * 8)

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
 * File map's memory object (linked list entry).
 */
typedef struct FileMapMem
{
  struct FileMapMem *next;

  struct FileMap *map; /* parent file map */
  ULONG start; /* start address */
  ULONG len; /* object length */
  int refcnt; /* number of MemMap entries using it */
} FileMapMem;

/**
 * File map.
 */
typedef struct FileMap
{
  FileDesc *desc; /* associated file desc */
  FileMapMem *mems; /* list of memory objects of this file */
  off_t size; /* file size */
} FileMap;

/**
 * Process specific file handle.
 */
typedef struct FileHandle
{
  FileDesc *desc; /* associated file desc */
  HFILE fd; /* file handle (descriptor in LIBC) or -1 for MAP_ANONYMOUS */
  size_t dirtymap_sz; /* size of dirty array in bytes */
  uint32_t *dirtymap; /* bit array of dirty pages */
  int refcnt; /* number of MemMap entries using it */
} FileHandle;

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
  FileMapMem *fmem; /* file map's memory or NULL for MAP_ANONYMOUS */
  FileHandle *fh; /* File handle or NULL for MAP_ANONYMOUS */
  off_t off; /* offset into the file or 0 for MAP_ANONYMOUS */
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

/*
 * Frees the given file map's memory structure. Also frees the parent
 * file map if this is the last mem struct associated with it.
 */
static void free_file_handle(FileHandle *fh)
{
  APIRET arc;

  TRACE("freeing file handle %p (fd %ld)\n", fh, fh->fd);
  assert(!fh->refcnt);

  if (fh->dirtymap_sz)
    free(fh->dirtymap);
  DosClose(fh->fd);

  if (fh->desc)
  {
    assert(fh->desc->p->fh == fh);
    fh->desc->p->fh = NULL;
  }

  free(fh);
}

/*
 * Frees the given file map's memory structure. Also frees the parent
 * file map if this is the last mem struct associated with it.
 */
static void free_file_map_mem(FileMapMem *mem)
{
  APIRET arc;

  TRACE("freeing file mem %p\n", mem);
  assert(!mem->refcnt);

  if (mem->start)
  {
    arc = DosFreeMem((PVOID)mem->start);
    TRACE("DosFreeMem = %ld\n", arc);
    assert(!arc);
  }

  FileMap *fmap = mem->map;
  FileMapMem *m = fmap->mems;

  if (m == mem)
  {
    /* Remove from head */
    fmap->mems = mem->next;
  }
  else
  {
    /* Remove elsewhere */
    while (m && m->next != mem)
      m = m->next;
    assert(m);
    m->next = mem->next;
  }

  free(mem);

  if (!fmap->mems)
  {
    /* No more memory objects, free the file map itself */
    TRACE("freeing file map %p\n", fmap);
    if (fmap->desc)
    {
      assert(fmap->desc->map == fmap);
      fmap->desc->map = NULL;
    }
    free(fmap);
  }
}

static MemMap *find_mmap(MemMap *head, ULONG addr, MemMap **prev_out);

void *mmap(void *addr, size_t len, int prot, int flags,
           int fildes, off_t off)
{
  APIRET arc;
  MemMap *mmap;
  MemMap *first = NULL, *prev = NULL;
  FileHandle *fh = NULL;
  FileMap *fmap = NULL;
  FileMapMem *fmem = NULL;
  FileDesc *fdesc = NULL;
  FileDesc *proc_fdesc = NULL;
  ProcDesc *pdesc;
  ULONG dos_flags;

  TRACE("addr %p, len %u, prot %x=%c%c%c, flags %x=%c%c%c%c, fildes %d, off %lld\n",
        addr, len, prot,
        prot & PROT_READ ? 'R' : '-',
        prot & PROT_WRITE ? 'W' : '-',
        prot & PROT_EXEC ? 'X' : '-',
        flags,
        flags & MAP_SHARED ? 'S' : '-',
        flags & MAP_PRIVATE ? 'P' : '-',
        flags & MAP_FIXED ? 'F' : '-',
        flags & MAP_ANON ? 'A' : '-',
        fildes, off);

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

  /* Round len up to the next page boundary */
  len = PAGE_ALIGN(len + PAGE_SIZE - 1);

  /* Check for technical overflow */
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

  dos_flags = 0;
  if (prot & PROT_READ)
    dos_flags |= PAG_READ;
  if (prot & PROT_WRITE)
    dos_flags |= PAG_WRITE;
  if (prot & PROT_EXEC)
    dos_flags |= PAG_EXECUTE;

  pdesc = find_proc_desc(getpid());
  assert(pdesc);

  if (!(flags & MAP_ANON))
  {
    __LIBC_PFH pFH;
    FILESTATUS3L st;
    ULONG fmap_flags;
    size_t dirtymap_sz = 0;

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

    arc = DosQueryFileInfo(fildes, FIL_STANDARDL, &st, sizeof(st));
    TRACE_IF(arc, "DosQueryFileInfo = %lu\n", arc);
    if (arc)
    {
      global_unlock();
      errno = EOVERFLOW;
      return MAP_FAILED;
    }

    TRACE("file size %llu (%llx)\n", st.cbFile, st.cbFile);
    if (st.cbFile > 0xFFFFFFFF)
    {
      /* Technical overflow */
      errno = EOVERFLOW;
      return MAP_FAILED;
    }

    global_lock();

    /* Get the associated file descriptor that stores the file handle */
    proc_fdesc = get_proc_file_desc(getpid(), pFH->pszNativePath);

    /* Get the associated file descriptor that stores the file map */
    if (flags & MAP_SHARED)
      fdesc = get_file_desc(pFH->pszNativePath);
    else
      fdesc = proc_fdesc;

    if (!fdesc || !proc_fdesc)
    {
      global_unlock();
      errno = ENOMEM;
      return MAP_FAILED;
    }

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

    fmap = fdesc->map;
    if (!fmap)
    {
      /* Create a new file map struct and its initial mem struct */
      TRACE ("allocating initial file map & mem object\n");
      GLOBAL_NEW(fmap);
      if (fmap)
        GLOBAL_NEW(fmem);
      if (!fmap || !fmem)
      {
        if (fmap)
          free(fmap);
        global_unlock();
        errno = ENOMEM;
        return MAP_FAILED;
      }

      fmem->len = off + len;

      arc = DosMyAllocMem((PPVOID)&fmem->start, fmem->len, fmap_flags);
      if (arc)
      {
        free(fmem);
        free(fmap);
        global_unlock();
        errno = ENOMEM;
        return MAP_FAILED;
      }

      fmem->map = fmap;
      fmap->mems = fmem;
    }
    else
    {
      assert(fmap->mems);

      /* Check if the requested region fits the longest memory object we have */
      if (off + len > fmap->mems->len)
      {
        /* Need a bigger object */
        TRACE ("allocating bigger mem object (need more than %ld bytes)\n", fmap->mems->len);
        GLOBAL_NEW(fmem);
        if (!fmem)
        {
          global_unlock();
          errno = ENOMEM;
          return MAP_FAILED;
        }

        fmem->len = off + len;

        arc = DosMyAllocMem((PPVOID)&fmem->start, fmem->len, fmap_flags);
        if (arc)
        {
          free(fmem);
          global_unlock();
          errno = ENOMEM;
          return MAP_FAILED;
        }

        /* Always insert the new, bigger mem at the beginning */
        fmem->map = fmap;
        fmem->next = fmap->mems;
        fmap->mems = fmem;
      }

      fmem = fmap->mems;
    }

    /* Update fmap with the latest file size */
    fmap->size = st.cbFile;

    assert(fmap->mems == fmem);
    TRACE("fmap %p (top mem %p (start %lx, len %lx))\n", fmap, fmem, fmem->start, fmem->len);

    /* Find the first mapping for this file map in this process */
    first = find_mmap(pdesc->mmaps, fmem->start, &prev);
    assert(!first || first->start == fmem->start);

    /*
     * Now check if there is a very fast route: the new mappping completely
     * replaces a single exising one.
     */
    mmap = NULL;
    if (first)
    {
      ULONG e = fmem->start + off + len;
      if (off == 0 && first->end <= e &&
          (!first->next || first->next->start >= e))
      {
        assert(first->fh);
        mmap = first;
      }
    }
    else
    {
      ULONG e = fmem->start + off + len;
      MemMap *m = prev ? prev->next : pdesc->mmaps;
      if (m && m->fmem == fmem &&
          m->start < e && m->end <= e &&
          (!m->next || m->next->start >= e))
      {
        assert(m->fh);
        mmap = m;
      }
    }
    if (mmap)
    {
      TRACE("fully replacing mmap %p (start %lx, end %lx)\n", mmap, mmap->start, mmap->end);
      /* Copy the new flags over */
      mmap->flags = flags;
      mmap->dos_flags = dos_flags;
      /* Fix start/end/off */
      mmap->start = fmem->start + off;
      mmap->end = mmap->start + len;
      mmap->off = off;
      goto success;
    }

    /*
     * Calculate the number of bytes (in 4 byte words) needed for the dirty page
     * bitmap. Note that the dirty map is only needed for writable shared
     * mappings bound to files and that we only track pages within the file size.
     */
    if (flags & MAP_SHARED && prot & PROT_WRITE)
      dirtymap_sz = DIVIDE_UP(NUM_PAGES(fmap->size), DIRTYMAP_WIDTH) * (DIRTYMAP_WIDTH / 8);
    TRACE("dirty map size %u bytes\n", dirtymap_sz);

    fh = proc_fdesc->p->fh;
    if (!fh)
    {
      /* Create a new file handle */
      GLOBAL_NEW(fh);
      if (fh && dirtymap_sz)
      {
        fh->dirtymap_sz = dirtymap_sz;
        fh->dirtymap = global_alloc(dirtymap_sz);
      }
      if (!fh || (dirtymap_sz && !fh->dirtymap))
      {
        if (fh)
          free(fh);
        if (!fdesc->map)
          free_file_map_mem(fmem);
        global_unlock();
        errno = ENOMEM;
        return MAP_FAILED;
      }

      /* Make own file handle since POSIX allows the original one to be closed */
      fh->fd = -1;
      arc = DosDupHandle(fildes, &fh->fd);
      TRACE_IF(arc, "DosDupHandle = %lu\n", arc);
      if (arc)
      {
        if (dirtymap_sz)
          free(fh->dirtymap);
        free(fh);
        if (!fdesc->map)
          free_file_map_mem(fmem);
        global_unlock();
        errno = EMFILE;
        return MAP_FAILED;
      }

      /* Prevent the system critical-error handler */
      ULONG mode = OPEN_FLAGS_FAIL_ON_ERROR;
      /* Also disable inheritance for private mappings */
      if (flags & MAP_PRIVATE)
        mode |= OPEN_FLAGS_NOINHERIT;
      arc = DosSetFHState(fh->fd, mode);
      TRACE_IF(arc, "DosSetFHState = %lu\n", arc);
      assert(!arc);

      TRACE("dup fd %ld\n", fh->fd);
    }
    else
    {
      /* Resize the dirty map if needed */
      if (fh->dirtymap_sz < dirtymap_sz)
      {
        TRACE("increasing dirty map (old size %u bytes)\n", fh->dirtymap_sz);
        assert(fh->dirtymap);
        uint32_t *dirtymap = realloc(fh->dirtymap, dirtymap_sz);
        if (!dirtymap)
        {
          if (!fdesc->map)
            free_file_map_mem(fmem);
          global_unlock();
          errno = ENOMEM;
          return MAP_FAILED;
        }
        fh->dirtymap_sz = dirtymap_sz;
        fh->dirtymap = dirtymap;
      }
    }

    assert(fh);
  }
  else
  {
    global_lock();
  }

  /* Allocate a new MemMap entry */
  GLOBAL_NEW(mmap);
  if (!mmap)
  {
    /* Free fh if it's not used */
    if (fh && !proc_fdesc->p->fh)
      free_file_handle(fh);
    /* Free fmap if it's not used */
    if (fmap && !fdesc->map)
      free_file_map_mem(fmem);
    global_unlock();
    errno = ENOMEM;
    return MAP_FAILED;
  }

  mmap->flags = flags;
  mmap->fmem = fmem;
  mmap->fh = fh;
  mmap->off = off;

  mmap->dos_flags = dos_flags;

  /*
   * Use PAG_READ for PROT_NONE since DosMemAlloc doesn't support no protection
   * mode. We will handle this case in mmap_exception() by refusing to ever
   * commit such a page and letting the process crash (as required by POSIX).
   */
  if (dos_flags == 0)
    dos_flags |= PAG_READ;

  if (fmap)
  {
    /* Use the file map */
    mmap->start = fmem->start + off;
    mmap->end = mmap->start + len;
    if (flags & MAP_SHARED && fdesc->map)
    {
      /* This is an existing file map, check if it's mapped in this process */
      ULONG mem_flags, mem_len = fmem->len;

      arc = DosQueryMem((PVOID)fmem->start, &mem_len, &mem_flags);
      TRACE_IF(arc, "DosQueryMem = %ld\n", arc);
      assert(!arc);
      if (mem_flags & PAG_FREE)
      {
        /*
         * Get access the file map region. Don't set PAG_WRITE to cause an
         * exception upon the first write to a page(see mmap_exception()).
         */
        arc = DosGetSharedMem((PVOID)fmem->start, PAG_READ | PAG_EXECUTE);
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
    {
      mmap->end = mmap->start + len;
      /* Find the closest mmap (to maintain sorted order) */
      first = find_mmap(pdesc->mmaps, mmap->start, &prev);
    }
  }

  /* Check for a memory allocation failure */
  if (arc)
  {
    free(mmap);
    /* Free fh if it's not used */
    if (fh && !proc_fdesc->p->fh)
      free_file_handle(fh);
    /* Free fmap if it's not used */
    if (fmap && !fdesc->map)
      free_file_map_mem(fmem);
    global_unlock();
    errno = ENOMEM;
    return MAP_FAILED;
  }

  /* Check if aligned to page size */
  assert(mmap->start && PAGE_ALIGNED(mmap->start));

  /* Now insert the new mapping into the sorted list */
  if (fmap && fdesc->map)
  {
    /*
     * The same file map's memory object is used by more than one mapping
     * so overlaps are possible. Split and join regions to avoid them. Note
     * that the simplest case when the new mapping fully replaces a single
     * existing one is handled much earlier and does not appear here.
     */
    if (first && first->start < mmap->start && first->end > mmap->end)
    {
      /* Special: split in 3 pieces */
      TRACE("splitting mmap %p (start %lx, end %lx) in 3 pieces\n",
            first, first->start, first->end);

      MemMap *last;

      GLOBAL_NEW(last);
      if (!last)
      {
        free(mmap);
        global_unlock();
        errno = ENOMEM;
        return MAP_FAILED;
      }

      COPY_STRUCT(last, first);

      /* Reference file maps' memory object */
      assert(last->fmem->refcnt);
      ++last->fmem->refcnt;

      /* Reference file handle */
      assert(last->fh->refcnt);
      ++last->fh->refcnt;

      /* Fix list */
      mmap->next = last;
      first->next = first;

      /* Fix start/end/off */
      first->end = mmap->start;
      last->start = mmap->end;
      last->off += last->start - first->start;
      assert(last->off == last->start - fmem->start);
    }
    else
    {
      /*
       * Rest of the cases (including splits in two pieces and multiple joins).
       * Find the start and the end mmaps of the span (both either excluding the
       * new mmap completely or intersecting with it in the middle, matching
       * ends don't require a split so such mmaps will be treated as
       * intermediate and freed).
       */
      MemMap *last;

      /* Set first to the element we should insert mmap after (or NULL) */
      first = first && first->start < mmap->start ? first : prev;

      /* Find the element that will follow mmap afer all joins (or NULL) */
      last = first ? first : pdesc->mmaps;
      while (last && last->start < mmap->end && last->end <= mmap->end)
      {
        MemMap *n = last->next;
        if (last != first)
        {
          /*
           * Free the intermediate mapping. Note that we don't flush dirty pages
           * since the dirty map will remain intact and will be flushed
           * asynchronously anyway. Also no need to free FileHandle or FileMap
           * as the new mmap is using them. So no need to call to free_mmap().
           */
          TRACE("eating mmap %p (start %lx, end %lx)\n", last, last->start, last->end);
          assert(last->fmem == fmem);
          assert(last->fh == fh);

          /* Dereference file handle */
          assert(last->fh->refcnt);
          --(last->fh->refcnt);

          /* Dereference file map's memory object */
          assert(last->fmem->refcnt);
          --(last->fmem->refcnt);

          free(last);
        }
        last = n;
      }

      /* Deal with the start mmap */
      if (first)
      {
        if (first->end > mmap->start)
        {
          TRACE("adjusting end of mmap %p (start %lx, end %lx) to %lx\n",
                first, first->start, first->end, mmap->start);
          assert(first->start < mmap->start);
          first->end = mmap->start;
        }
        mmap->next = first->next;
        first->next = mmap;
      }
      else
      {
        mmap->next = pdesc->mmaps;
        pdesc->mmaps = mmap;
      }
      /* Deal with the end mmap */
      if (last)
      {
        if (last->start < mmap->end)
        {
          TRACE("adjusting start of mmap %p (start %lx, end %lx) to %lx\n",
                last, last->start, last->end, mmap->end);
          assert(last->end > mmap->end);
          last->start = mmap->end;
        }
      }
      mmap->next = last;
    }
  }
  else
  {
    /*
     * Anonymous maps are simple: overlaps are not possible as each mapping is
     * always backed up by its own unique memory object.
     */
    assert(!first);
    if (prev)
    {
      /* Insert in the middle/end */
      mmap->next = prev->next;
      prev->next = mmap;
    }
    else
    {
      /* Insert at the head */
      mmap->next = pdesc->mmaps;
      pdesc->mmaps = mmap;
    }
  }

  if (fmap)
  {
    assert(fmap->mems == fmem);

    /* Reference the file handle as it's used now */
    if (!proc_fdesc->p->fh)
    {
      /* If it's a newly allocated fh entry, associate it with its file desc */
      fh->refcnt = 1;
      fh->desc = proc_fdesc;
      proc_fdesc->p->fh = fh;
    }
    else
    {
      ++(fh->refcnt);
      assert(fh->refcnt);
      assert(fh->desc == proc_fdesc);
      assert(proc_fdesc->p->fh == fh);
    }

    /* Reference the file map's memory object as it is used now */
    if (!fdesc->map)
    {
      /* If it's a newly allocated fmap entry, associate it with its file desc */
      fmem->refcnt = 1;
      fmap->desc = fdesc;
      fdesc->map = fmap;
    }
    else
    {
      ++(fmem->refcnt);
      assert(fmem->refcnt);
      assert(fmap->desc == fdesc);
      assert(fdesc->map == fmap);
    }
  }

success:

  TRACE("mmap %p (%lx..%lx (%lu), fmem %p (%lx), fmap %p\n",
        mmap, mmap->start, mmap->end, mmap->end - mmap->start, mmap->fmem,
        mmap->fmem ? mmap->fmem->start : 0,
        mmap->fmem ? mmap->fmem->map : 0);

  global_unlock();

  return (void *)mmap->start;
}

/**
 * Searches for a mapping containing the given address in the sorted list of
 * process mappings. Returns the found mapping or NULL. Returns the previous
 * mapping in the list if @a prev_out is not NULL (it will always return a
 * mapping preceeding addr, if any, even when no containig mapping is found).
 * Must be called from under global_lock().
 */
static MemMap *find_mmap(MemMap *head, ULONG addr, MemMap **prev_out)
{
  MemMap *m = NULL, *pm = NULL;

  m = head;
  while (m && m->start < addr && m->end <= addr)
  {
    pm = m;
    m = m->next;
  }

  /*
   * Reset m if it's the next region. Note that pm will still contain the
   * "previous" one, i.e. the one addr should be insterted after.
   */
  if (m && m->start > addr)
    m = NULL;

  TRACE_IF(!m, "mapping not found\n");
  TRACE_IF(m, "found m %p (%lx..%lx (%ld), flags %x=%c%c%c%c, dos_flags %lx, fmem %p (%lx))\n",
           m, m->start, m->end, m->end - m->start, m->flags,
           m->flags & MAP_SHARED ? 'S' : '-',
           m->flags & MAP_PRIVATE ? 'P' : '-',
           m->flags & MAP_FIXED ? 'F' : '-',
           m->flags & MAP_ANON ? 'A' : '-',
           m->dos_flags, m->fmem, m->fmem ? m->fmem->start : 0);

  if (prev_out)
    *prev_out = pm;

  return m;
}

/**
 * Flush dirty pages of the given mapping to the underlying file. If @a off
 * is not 0, it specifies the offest of the first page to flush from the
 * beginning of the given mapping. If @a len is not 0, it specifies the length
 * of the region to flush. If it is 0, all pages up to the end of the mapping
 * are flushed.
 *
 * For shared mappings, this method also flushes changes to related memory
 * objects representing the same file.
 */
static void flush_dirty_pages(MemMap *m, ULONG off, ULONG len)
{
  TRACE("m %p (fmem %p), off %lu, len %lu\n", m, m->fmem, off, len);
  assert(m && m->flags & MAP_SHARED && m->dos_flags & PAG_WRITE && m->fmem);
  assert(off + len <= (m->end - m->start));

  ULONG page, end;
  size_t i, j;
  uint32_t bit;
  APIRET arc;

  off = PAGE_ALIGN(off);
  if (!len)
    len = m->end - m->start - off;

  /* Return early if offset is completely beyond EOF */
  if (off + m->off >= m->fmem->map->size)
    return;

  /* Make sure we don't sync beyond EOF */
  if (off + m->off + len > m->fmem->map->size)
    len = m->fmem->map->size - off - m->off;

  page = m->start + off;
  end = page + len;

  /* Note: dirty map is file map object-based, hence + m->off) */
  i = ((off + m->off) / PAGE_SIZE) / DIRTYMAP_WIDTH;
  j = ((off + m->off) / PAGE_SIZE) % DIRTYMAP_WIDTH;
  bit = 0x1 << j;

  for (; page < end; ++i, j = 0, bit = 0x1)
  {
    /* Check a block of DIRTYMAP_WIDTH pages at once to quickly skip clean ones */
    if (!m->fh->dirtymap[i])
    {
      page += PAGE_SIZE * (DIRTYMAP_WIDTH - j);
    }
    else
    {
      for (; page < end && bit; page += PAGE_SIZE, bit <<= 1)
      {
        if (m->fh->dirtymap[i] & bit)
        {
          ULONG nesting, write;
          LONGLONG pos;

          pos = page - m->start + m->off;
          write = page + PAGE_SIZE <= end ? PAGE_SIZE : end - page;

          TRACE("writing %lu bytes from addr %lx to fd %ld at offset %llu\n",
                write, page, m->fh->fd, pos);

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

          arc = DosSetFilePtrL(m->fh->fd, pos, FILE_BEGIN, &pos);
          ASSERT_MSG(!arc, "%ld\n", arc);

          arc = DosWrite(m->fh->fd, (PVOID)page, write, &write);
          ASSERT_MSG(!arc, "%ld\n", arc);

          /*
           * Now propagate changes to all related mem objects. Note that since
           * objects are stored in size descending order, we stop when we
           * find an object too small to receive any changes. Also note that we
           * only copy to committed memory, uncommitted will be fetched directly
           * from the file upon the first access attempt.
           */
          FileMapMem *fm = m->fmem->map->mems;
          while (fm && fm->len > pos)
          {
            if (fm != m->fmem)
            {
              ULONG p = fm->start + pos;
              ULONG l = PAGE_SIZE, f;

              arc = DosQueryMem((PVOID)p, &l, &f);
              ASSERT_MSG(!arc, "%ld\n", arc);

              if (f & PAG_FREE)
              {
                TRACE("getting shared memory %lx of mem %p\n", fm->start, fm);
                arc = DosGetSharedMem((PVOID)fm->start, PAG_READ | PAG_EXECUTE);
                ASSERT_MSG(!arc, "%ld\n", arc);
              }

              arc = DosQueryMem((PVOID)p, &l, &f);
              assert(!arc);

              if (f & PAG_COMMIT)
              {
                l = pos + write <= fm->len ? write : fm->len - pos;
                TRACE("copying %lu bytes from addr %lx to addr %lx of mem %p (%lx)\n",
                      l, page, p, fm, fm->start);

                if (!(f & PAG_WRITE))
                {
                  /* memcpy needs PAG_WRITE, avoid unneeded mmap exceptions */
                  arc = DosSetMem((PVOID)p, l, (f & fPERM) | PAG_WRITE);
                  ASSERT_MSG(!arc, "%ld\n", arc);
                }

                memcpy((void *)p, (void *)page, l);

                if (!(f & PAG_WRITE))
                {
                  /* Restore PAG_WRITE */
                  arc = DosSetMem((PVOID)p, l, (f & fPERM));
                  ASSERT_MSG(!arc, "%ld\n", arc);
                }
              }
            }

            fm = fm->next;
          }

          /*
           * Reset PAG_WRITE (to cause another exception upon a new write)
           * and the dirty bit on success.
           */
          arc = DosSetMem((PVOID)page, PAGE_SIZE, m->dos_flags & ~PAG_WRITE);
          ASSERT_MSG(!arc, "%ld\n", arc);

          m->fh->dirtymap[i] &= ~bit;

          DosExitMustComplete(&nesting);
        }
      }
    }
  }
}

/**
 * Frees the given mapping and removes it from the list of process mappings.
 * @a prev is either the previous element or NULL if @a m is the head.
 */
static void free_mmap(ProcDesc *desc, MemMap *m, MemMap *prev)
{
  APIRET arc;

  assert(m);

  TRACE("%s mapping %p (start %lx, end %lx, fmem %p (start %lx, refcnt %d)\n",
        m->flags & MAP_SHARED ? "Shared" : "Private",
        m, m->start, m->end, m->fmem, m->fmem ? m->fmem->start : 0, m->fmem ? m->fmem->refcnt : 0);

  if (m->flags & MAP_SHARED && m->dos_flags & PAG_WRITE && m->fmem)
    flush_dirty_pages(m, 0, 0);

  if (m->fmem)
  {
    /* Free the file handle if we are the last user */
    assert(m->fh->refcnt);
    --(m->fh->refcnt);
    if (!m->fh->refcnt)
      free_file_handle(m->fh);

    /* Free the file map's memory object if we are the last user */
    assert(m->fmem->refcnt);
    --(m->fmem->refcnt);
    if (!m->fmem->refcnt)
      free_file_map_mem(m->fmem);
  }
  else
  {
    /* Release process-specific memory */
    arc = DosFreeMem((PVOID)m->start);
    TRACE("DosFreeMem = %ld\n", arc);
    assert(!arc);
  }

  /* Remove this mapping from this process' mapping list */
  if (prev)
    prev->next = m->next;
  else if (desc)
    desc->mmaps = m->next;

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

  /* Round len up to the next page boundary */
  len = PAGE_ALIGN(len + PAGE_SIZE - 1);

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

  m = find_mmap(desc->mmaps, (ULONG)addr, &pm);
  if (m && m->start < (ULONG)addr && m->end > addr_end)
  {
    /* Special case: cut from the middle */
    MemMap *nm;

    GLOBAL_NEW(nm);
    if (nm)
    {
      COPY_STRUCT(nm, m);
      if (nm->fmem)
      {
        /* Rreference file map's memory object */
        assert(nm->fmem->refcnt);
        ++nm->fmem->refcnt;

        /* Rreference file handle */
        assert(nm->fh->refcnt);
        ++nm->fh->refcnt;
      }

      /* Fix pointers */
      nm->next = m->next;
      nm->start = addr_end;
      nm->end = m->end;
      m->end = (ULONG)addr;
      m->next = nm;
    }
    else
    {
      errno = ENOMEM;
      rc = -1;
    }
  }
  else
  {
    /* Start with the next region if no match */
    if (!m)
      m = pm ? pm->next : desc->mmaps;
    if (m)
    {
      /* Proces first intersection */
      if (m->start < (ULONG)addr)
      {
        TRACE("adjusting end of mmap %p (start %lx, end %lx) to %lx\n",
              m, m->start, m->end, (ULONG)addr);
        m->end = (ULONG)addr;
        pm = m;
        m = m->next;
      }
      /* Process middle */
      while (m && m->start < addr_end && m->end <= addr_end)
      {
        MemMap *n = m->next;
        free_mmap(desc, m, pm);
        m = n;
      }
      /* Proces last intersection */
      if (m && m->start < addr_end && m->end > addr_end)
      {
        TRACE("adjusting start of mmap %p (start %lx, end %lx) to %lx\n",
              m, m->start, m->end, addr_end);
        m->start = addr_end;
      }
    }
  }

  /*
   * Note that if no regions match the given range at all, we will simply
   * return a success. This is a POSIX requirement.
   */

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
        if (m->flags & MAP_SHARED && m->dos_flags & PAG_WRITE && m->fmem)
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
      free_mmap(NULL, m, NULL);
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
      m = find_mmap(desc->mmaps, addr, NULL);

    /*
     * Note that we only do something if the found mmap is not PROT_NONE and
     * let the application crash otherwise (see also mmap()). Also note that we
     * ignore excpetions for file-bound mappings that address memory beyond the
     * file's last page.
     */
    if (m && m->dos_flags & fPERM &&
        (!m->fmem || m->fmem->map->size > PAGE_ALIGN(addr - m->fmem->start)))
    {
      APIRET arc;
      ULONG len = PAGE_SIZE;
      ULONG dos_flags;
      ULONG page_addr = PAGE_ALIGN(addr);

      arc = DosQueryMem((PVOID)page_addr, &len, &dos_flags);
      TRACE_IF(arc, "DosQueryMem = %lu\n", arc);
      if (!arc)
      {
        TRACE("off %lx, dos_flags %lx, len %ld\n", addr - m->start, dos_flags, len);
        if (!(dos_flags & (PAG_FREE | PAG_COMMIT)))
        {
          /* First access to the allocated but uncommitted page, commit it */
          int revoke_write = 0;

          dos_flags = m->dos_flags;
          if (m->fmem)
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
                /* Note: dirty map is file map object-based, hence + m->off) */
                ULONG pn = (page_addr - m->start + m->off) / PAGE_SIZE;
                size_t i = pn / DIRTYMAP_WIDTH;
                uint32_t bit = 0x1 << (pn % DIRTYMAP_WIDTH);
                m->fh->dirtymap[i] |= bit;
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
            if (m->fmem)
            {
              /*
               * Read file contents into memory. Note that if we fail here,
               * we simply let the exception go further and hopefully crash
               * and abort the application.
               */
              ULONG read = PAGE_SIZE;
              LONGLONG pos = page_addr - m->start + m->off;
              TRACE("Reading %lu bytes to addr %lx from fd %ld at offset %llu\n",
                    read, page_addr, m->fh->fd, pos);
              arc = DosSetFilePtrL(m->fh->fd, pos, FILE_BEGIN, &pos);
              TRACE_IF(arc, "DosSetFilePtrL = %ld\n", arc);
              if (!arc)
              {
                /* Page is committed, so calling original DosRead is safe. */
                arc = _doscalls_DosRead(m->fh->fd, (PVOID)page_addr, read, &read);
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
              m->flags & MAP_SHARED && m->dos_flags & PAG_WRITE && m->fmem)
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
              /* Note: dirty map is file map object-based, hence + m->off) */
              ULONG pn = (page_addr - m->start + m->off) / PAGE_SIZE;
              size_t i = pn / DIRTYMAP_WIDTH;
              uint32_t bit = 0x1 << (pn % DIRTYMAP_WIDTH);
              m->fh->dirtymap[i] |= bit;
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
  ULONG addr_end;
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

  /* Round len up to the next page boundary */
  len = PAGE_ALIGN(len + PAGE_SIZE - 1);

  /* Check if len within bounds */
  if (0 - ((uintptr_t)addr) < len)
  {
    TRACE("len out of bounds\n");
    errno = ENOMEM; /* Differs from munmap but per POSIX */
    return -1;
  }

  addr_end = ((ULONG)addr) + len;

  global_lock();

  desc = find_proc_desc(getpid());
  assert(desc);

  m = find_mmap(desc->mmaps, (ULONG)addr, NULL);
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
  if (m->flags & MAP_SHARED && m->dos_flags & PAG_WRITE && m->fmem)
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
  ProcDesc *desc;
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

  /* Round len up to the next page boundary */
  len = PAGE_ALIGN(len + PAGE_SIZE - 1);

  /* Check if len within bounds */
  if (0 - ((uintptr_t)addr) < len)
  {
    TRACE("len out of bounds\n");
    errno = ENOMEM; /* Differs from munmap but per POSIX */
    return -1;
  }

  addr_end = ((ULONG)addr) + len;

  global_lock();

  desc = find_proc_desc(getpid());
  assert(desc);

  m = find_mmap(desc->mmaps, (ULONG)addr, NULL);
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

  /* Round len up to the next page boundary */
  len = PAGE_ALIGN(len + PAGE_SIZE - 1);

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

  /* Round len up to the next page boundary */
  len = PAGE_ALIGN(len + PAGE_SIZE - 1);

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
    m = find_mmap(desc->mmaps, next_addr, NULL);
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

      if (m->fmem)
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
              m, m->start, m->fmem, m->fmem ? m->fmem->start : 0, pForkHandle->pidChild);
        if (m->fmem)
        {
          start = m->fmem->start;
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
        TRACE("restoring mapping %p (start %lx, fmem %p, fmem->start %lx)\n",
              m, m->start, m->fmem, m->fmem ? m->fmem->start : 0);
        GLOBAL_NEW(newm);
        if (!newm)
        {
          ok = FALSE;
          break;
        }

        /*
         * Copy the fields. Note that the dirty map, if any, will remain reset
         * in the copy as it won't be copied over - this is because dirty maps
         * are per process (as well as memory protection flags).
         */
        TRACE("new mmap %p\n", newm);
        COPY_STRUCT(newm, m);

        if (newm->fmem)
        {
          FileDesc *fdesc = newm->fmem->map->desc;
          int dirtymap_sz = 0;
          FileHandle *fh;
          FileDesc *proc_fdesc;

          if (m->dos_flags & PAG_WRITE)
            dirtymap_sz = DIVIDE_UP(NUM_PAGES(newm->fmem->map->size), DIRTYMAP_WIDTH) * (DIRTYMAP_WIDTH / 8);

          assert(fdesc);
          proc_fdesc = get_proc_file_desc(getpid(), fdesc->path);
          if (!proc_fdesc)
          {
            ok = FALSE;
            break;
          }

          fh = proc_fdesc->p->fh;
          if (!fh)
          {
            /* Create a new file handle for this process */
            TRACE("new file handle (fd %ld, dirty map size %u bytes)\n", newm->fh->fd, dirtymap_sz);
            GLOBAL_NEW(fh);
            if (fh && dirtymap_sz)
            {
              fh->dirtymap_sz = dirtymap_sz;
              fh->dirtymap = global_alloc(dirtymap_sz);
            }
            if (!fh || (dirtymap_sz && !fh->dirtymap))
            {
              ok = FALSE;
              break;
            }

            fh->fd = newm->fh->fd;
          }

          newm->fh = fh;

          /* Reference file handle */
          ++(newm->fh->refcnt);
          assert(newm->fh->refcnt);

          /* Reference file map */
          ++(newm->fmem->refcnt);
          assert(newm->fmem->refcnt);
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
