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
#include <stdlib.h>
#include <string.h>
#include <emx/io.h>
#include <sys/param.h>
#include <fcntl.h>

#include <InnoTekLIBC/fork.h>

#include <sys/mman.h>

#define TRACE_GROUP TRACE_GROUP_MMAP

#include "mmap.h"

#define OBJ_MY_SHARED 0x80000000

#ifdef DEBUG
/* Indicates that mmaps should be allocated for full file size at once */
static int mmap_full_size = 0;
#endif

static APIRET DosMyAllocMem(PPVOID addr, ULONG size, ULONG flags)
{
  APIRET arc;

  ASSERT(!(flags & OBJ_ANY));

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

/**
 * Checks if there is any usage through fds for a given file description
 * and frees it if not.
 */
static void maybe_free_file_desc(FileDesc *desc)
{
  int i;
  for (i = 0; i < desc->size_fds; ++i)
    if (desc->fds[i] != -1)
      break;

  if (i == desc->size_fds)
  {
    /* This desc is not used any more, free it */
    size_t bucket = 0;
    FileDesc *prev = NULL;
    ProcDesc *proc = NULL;
    FileDesc *fdesc = find_file_desc_ex(desc->g->path, &bucket, &prev, &proc);
    ASSERT_MSG(fdesc == desc, "%p %p", fdesc, desc);
    free_file_desc(fdesc, bucket, prev, proc);
  }
}

/*
 * Frees the given file handle structure. Also deassociates it
 * from the associated file description if it's not NULL.
 */
static void free_file_handle(FileHandle *fh)
{
  APIRET arc;

  TRACE("freeing file handle %p (fd %ld)\n", fh, fh->fd);
  ASSERT(!fh->refcnt);

  if (fh->dirtymap_sz)
    free(fh->dirtymap);
  DosClose(fh->fd);

  if (fh->desc)
  {
    ASSERT(fh->desc->fh == fh);
    fh->desc->fh = NULL;

    if (fh->desc->map == NULL)
      maybe_free_file_desc(fh->desc);
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
  ASSERT_MSG(!mem->refcnt, "%d", mem->refcnt);

  if (mem->start)
  {
    arc = DosFreeMem((PVOID)mem->start);
    ASSERT_MSG(arc == NO_ERROR, "%ld", arc);
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
    ASSERT(m);
    m->next = mem->next;
  }

  free(mem);

  if (!fmap->mems)
  {
    /* No more memory objects, free the file map itself */
    TRACE("freeing file map %p\n", fmap);
    if (fmap->flags)
    {
      /* Deassociate from the appropriate file description */
      if (fmap->flags & MAP_SHARED)
      {
        ASSERT(fmap->desc_g->map == fmap);
        fmap->desc_g->map = NULL;
      }
      else
      {
        ASSERT(fmap->desc->map == fmap);
        fmap->desc->map = NULL;

        if (fmap->desc->fh == NULL)
          maybe_free_file_desc(fmap->desc);
      }
    }

    free(fmap);
  }
}

static MemMap *find_mmap(MemMap *head, ULONG addr, MemMap **prev_out);

/*
 * Clones the given mapping. Used in region splitting. Note that this clone is
 * never to be used directly: its next and start/end fields must be fixed
 * accordingly. Returns NULL if no memory left to create a clone.
 */
static MemMap *clone_file_mmap(MemMap *m)
{
  ASSERT(m->f->fmem);
  ASSERT(m->f->fh);

  MemMap *nm;
  GLOBAL_NEW_PLUS(nm, sizeof(*nm->f));
  if (!nm)
    return NULL;

  COPY_STRUCT_PLUS(nm, m, sizeof(*nm->f));

  /* Reference file maps' memory object */
  ASSERT(nm->f->fmem->refcnt);
  ++nm->f->fmem->refcnt;

  /* Reference file handle */
  ASSERT(nm->f->fh->refcnt);
  ++nm->f->fh->refcnt;

  return nm;
}

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
  ProcDesc *pdesc;
  ULONG dos_flags;
  int maybe_overlaps = 0;

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
    errno = !(flags & MAP_ANON) && fildes == -1 ? EBADF : EINVAL;
    return MAP_FAILED;
  }

  /* Round len up to the next page boundary */
  len = PAGE_ALIGN(len + PAGE_SIZE - 1);

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
  ASSERT(pdesc);

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
      errno = EOVERFLOW;
      return MAP_FAILED;
    }

    TRACE("file size %llu (%llx)\n", st.cbFile, st.cbFile);

    global_lock();

    /* Get the associated file description that stores the file handle */
    fdesc = get_file_desc(fildes, pFH->pszNativePath);
    if (!fdesc)
    {
      global_unlock();
      errno = ENOMEM;
      return MAP_FAILED;
    }

    /* Note: use PAG_GUARD to avoid races (see mmap_exception) */
    fmap_flags = PAG_READ | PAG_WRITE | PAG_EXECUTE | PAG_GUARD;
    if (flags & MAP_SHARED)
    {
      /*
       * Don't set PAG_WRITE to cause an exception upon the first
       * write to a page (see mmap_exception()).
       */
      fmap_flags &= ~PAG_WRITE;
      fmap_flags |= OBJ_MY_SHARED | OBJ_GIVEABLE | OBJ_GETTABLE;
    }

    fmap = flags & MAP_SHARED ? fdesc->g->map : fdesc->map;
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

#ifdef DEBUG
      if (mmap_full_size)
      {
        fmem->off = 0;
        fmem->len = MAX(NUM_PAGES(st.cbFile) * PAGE_SIZE, off + len);
      }
      else
#endif
      {
        fmem->off = off;
        fmem->len = len;
      }

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
      ASSERT(fmap->mems);

      /* Search for a memory object that can fit the requested range */
      fmem = fmap->mems;
      while (fmem)
      {
        if (fmem->off <= off && fmem->off + fmem->len >= off + len)
          break;
        fmem = fmem->next;
      }

      if (!fmem)
      {
        /* Need a new object */
        TRACE ("allocating new mem object\n");
        GLOBAL_NEW(fmem);
        if (!fmem)
        {
          global_unlock();
          errno = ENOMEM;
          return MAP_FAILED;
        }

        fmem->off = off;
        fmem->len = len;

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
      else
      {
        /* We're reusing the same memory object, overlaps are possible */
        maybe_overlaps = 1;
      }
    }

    /* Update fmap with the latest file size */
    fmap->size = st.cbFile;

    TRACE("fmap %p (top mem %p (start %lx, off %llx, len %lx))\n",
          fmap, fmem, fmem->start, fmem->off, fmem->len);

    /*
     * Search for a mapping containing the requested offset in this process to
     * find where to insert it in the sorted list and to check for region
     * overlaps.
     */
    first = find_mmap(pdesc->mmaps, fmem->start + (off - fmem->off), &prev);

    if (maybe_overlaps && first && first->start == fmem->start + (off - fmem->off) &&
        first->end == fmem->start + (off - fmem->off) + len)
    {
      /* Very fast route: the new mappping fully matches the existing one. */
      TRACE("fully replacing mmap %p (start %lx, end %lx)\n", first, first->start, first->end);
      ASSERT(first->f->fmem == fmem);
      ASSERT(first->f->fh);

      mmap = first;

      /* Copy the new flags over */
      mmap->flags = flags;
      mmap->dos_flags = dos_flags;
      /* Increase the usage count of the existing MemMap */
      ASSERT(mmap->f->refcnt);
      ++mmap->f->refcnt;
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

    fh = fdesc->fh;
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
        if (!fmap->flags)
          free_file_map_mem(fmem);
        global_unlock();
        errno = ENOMEM;
        return MAP_FAILED;
      }

      /* Make own file handle since POSIX allows the original one to be closed */
      fh->fd = -1;
      arc = DosDupHandle(fildes, &fh->fd);
      TRACE_IF(arc, "DosDupHandle = %lu\n", arc);
      if (arc != NO_ERROR)
      {
        if (arc == ERROR_TOO_MANY_OPEN_FILES)
        {
          // Try to increase the file handle limit
          LONG increment = 0;
          ULONG curMax = 0;
#if defined(TRACE_ENABLED)
          arc = DosSetRelMaxFH(&increment, &curMax);
          TRACE_IF(arc, "DosSetRelMaxFH = %lu\n", arc);
          TRACE_IF(!arc, "curMax before %ld\n", curMax);
#endif
          increment = 100;
          arc = DosSetRelMaxFH(&increment, &curMax);
          TRACE_IF(arc, "DosSetRelMaxFH = %lu\n", arc);
          TRACE_IF(!arc, "curMax after %ld\n", curMax);
          // And try to dup again
          if (arc == NO_ERROR)
            arc = DosDupHandle(fildes, &fh->fd);
        }
        // Re-evaluate the error check
        if (arc != NO_ERROR)
        {
          if (dirtymap_sz)
            free(fh->dirtymap);
          free(fh);
          if (!fmap->flags)
            free_file_map_mem(fmem);
          global_unlock();
          errno = EMFILE;
          return MAP_FAILED;
        }
      }

      /* Prevent the system critical-error handler */
      ULONG mode = OPEN_FLAGS_FAIL_ON_ERROR;
      /* Also disable inheritance for private mappings */
      if (flags & MAP_PRIVATE)
        mode |= OPEN_FLAGS_NOINHERIT;
      arc = DosSetFHState(fh->fd, mode);
      ASSERT_MSG(arc == NO_ERROR, "%ld", arc);

      TRACE("dup fd %ld\n", fh->fd);
    }
    else
    {
      /* Resize the dirty map if needed */
      if (fh->dirtymap_sz < dirtymap_sz)
      {
        TRACE("increasing dirty map (old size %u bytes)\n", fh->dirtymap_sz);
        ASSERT(fh->dirtymap);
        uint32_t *dirtymap = crealloc(fh->dirtymap, fh->dirtymap_sz, dirtymap_sz);
        if (!dirtymap)
        {
          if (!fmap->flags)
            free_file_map_mem(fmem);
          global_unlock();
          errno = ENOMEM;
          return MAP_FAILED;
        }
        fh->dirtymap_sz = dirtymap_sz;
        fh->dirtymap = dirtymap;
      }
    }

    ASSERT(fh);
  }
  else
  {
    global_lock();
  }

  /* Allocate a new MemMap entry */
  GLOBAL_NEW_PLUS(mmap, flags & MAP_ANON ? 0 : sizeof(*mmap->f));
  if (!mmap)
  {
    /* Free fh if it's not used */
    if (fh && !fdesc->fh)
      free_file_handle(fh);
    /* Free fmap if it's not used */
    if (fmap && !fmap->flags)
      free_file_map_mem(fmem);
    global_unlock();
    errno = ENOMEM;
    return MAP_FAILED;
  }

  mmap->flags = flags;
  mmap->dos_flags = dos_flags;

  /*
   * Use PAG_READ for PROT_NONE since DosMemAlloc doesn't support no protection
   * mode. We will handle this case in mmap_exception() by refusing to ever
   * commit such a page and letting the process crash (as required by POSIX).
   */
  if (dos_flags == 0)
    dos_flags |= PAG_READ;

  if (!(flags & MAP_ANON))
  {
    /* Use the file map */
    ASSERT(fmem && fh);
    mmap->f->fmem = fmem;
    mmap->f->fh = fh;
    mmap->f->refcnt = 1;
    mmap->start = fmem->start + (off - fmem->off);
    mmap->end = mmap->start + len;
    if (flags & MAP_SHARED && fmap->flags)
    {
      /* This is an existing file map, check if it's mapped in this process */
      ULONG mem_flags, mem_len = fmem->len;

      arc = DosQueryMem((PVOID)fmem->start, &mem_len, &mem_flags);
      ASSERT_MSG(arc == NO_ERROR, "%ld", arc);
      if (mem_flags & PAG_FREE)
      {
        /*
         * Get access the file map region. Don't set PAG_WRITE to cause an
         * exception upon the first write to a page(see mmap_exception()).
         * Note: use PAG_GUARD to avoid races (see mmap_exception).
         */
        arc = DosGetSharedMem((PVOID)fmem->start, PAG_READ | PAG_EXECUTE | PAG_GUARD);
        ASSERT_MSG(arc == NO_ERROR, "%ld", arc);
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
    if (fh && !fdesc->fh)
      free_file_handle(fh);
    /* Free fmap if it's not used */
    if (fmap && !fmap->flags)
      free_file_map_mem(fmem);

    global_unlock();

    TRACE("No memory\n");
    errno = ENOMEM;
    return MAP_FAILED;
  }

  /* Check if aligned to page size */
  ASSERT(mmap->start && PAGE_ALIGNED(mmap->start));

  /* Now insert the new mapping into the sorted list */
  if (fmap && maybe_overlaps)
  {
    /*
     * The same memory object is used by more than one mapping so overlaps are
     * possible. We deal with overlaps by splitting the mmap regions and
     * increasing their usage counters accordingly. Note that the simplest case
     * when the new mapping fully replaces a single existing one is handled
     * earlier in the code and does not appear here.
     */
    MemMap *last = prev ? prev->next : pdesc->mmaps;

    if (!first && (!last || last->start >= mmap->end))
    {
      /* No overlaps at all, we are done */
      TRACE("no overlaps\n");
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
    else if (first && first->start < mmap->start && first->end > mmap->end)
    {
      /* Special: split in 3 pieces */
      TRACE("splitting mmap %p (start %lx, end %lx) at %lx and %lx\n",
            first, first->start, first->end, mmap->start, mmap->end);

      MemMap *last = clone_file_mmap(first);
      if (!last)
        goto failure;

      /* Fix list */
      mmap->next = last;
      first->next = mmap;

      /* Fix start/end */
      first->end = mmap->start;
      last->start = mmap->end;

      /* Increase the usage count of the middle MemMap */
      ASSERT(mmap->f->refcnt);
      ++mmap->f->refcnt;
    }
    else
    {
      /*
       * Rest of the cases (including splits in two pieces and multiple
       * overlaps). Find the start and the end mmaps of the span (both either
       * excluding the new mmap completely or intersecting with it in the
       * middle, matching ends don't require a split so they will be treated as
       * completely overlapped). All overallped mappings in betweeen will get
       * their usage count increased and all gaps between them (if any) will be
       * filled by newly created mmaps.
       */
      MemMap *p_last, *m;

      /*
       * Find a region containing the end of the new mmap (or NULL). Also fill
       * all the gaps between regions fully overlapped by the new mmap.
       */
      last = first ? first : prev ? prev : pdesc->mmaps;
      ASSERT(last);
      p_last = NULL;
      while (1)
      {
        /* Fill the gap if any */
        ULONG gap_start = 0, gap_end = 0;

        if (pdesc->mmaps == last && last->start > mmap->start)
        {
          /* Special case: no region in front of mmap */
          ASSERT(last);
          gap_start = mmap->start;
          gap_end = last->start;
          ASSERT(gap_end < mmap->end);
        }
        else if (!last)
        {
          /* Special case: no region after mmap */
          ASSERT(p_last);
          gap_start = p_last->end;
          gap_end = mmap->end;
          ASSERT(gap_start > mmap->start);
        }
        else if (p_last && p_last->end < last->start)
        {
          /* Normal case: gap between prev and current region */
          gap_start = MAX(p_last->end, mmap->start);
          gap_end = MIN(last->start, mmap->end);
        }

        if (gap_end)
        {
          TRACE("filling gap (start %lx, end %lx)\n", gap_start, gap_end);
          ASSERT(gap_start != gap_end);

          m = clone_file_mmap(mmap);
          if (!m)
            goto failure;

          /* Fix list */
          m->next = last;
          if (p_last)
          {
            ASSERT(p_last->next == last);
            p_last->next = m;
          }
          else
          {
            ASSERT(pdesc->mmaps == last);
            pdesc->mmaps = m;
          }

          /* Fix start/end */
          m->start = gap_start;
          m->end = gap_end;

          /* Fix p_last in case if we break below before updating it with last */
          p_last = m;
        }

        /* Note that overlapping is only done for full overlaps */
        if (last && last->start >= mmap->start && last->end <= mmap->end)
        {
          TRACE("overlapping mmap %p (start %lx, end %lx)\n", last, last->start, last->end);
          ASSERT(last->f->fmem == fmem);
          ASSERT(last->f->fh == fh);

          /* Increase the usage count of the existing MemMap */
          ASSERT(last->f->refcnt);
          ++last->f->refcnt;
        }

        /* Break the loop if it's a last intersection (or no more regions) */
        if (!last || last->end >= mmap->end)
        {
          if (last && last->start >= mmap->end)
          {
            /* Reset last to indicate there is no intersection */
            p_last = last;
            last = NULL;
          }
          break;
        }

        p_last = last;
        last = last->next;
      }

      /* Fix p_last when we got only one intersecting region */
      if (first && last == first)
      {
        ASSERT(!p_last);
        p_last = prev;
      }

      /* Deal with the partial overlap at the beginning */
      if (first && first->start < mmap->start)
      {
        TRACE("splitting first mmap %p (start %lx, end %lx) at %lx\n",
              first, first->start, first->end, mmap->start);

        /* Split the partial overlap */
        m = clone_file_mmap(mmap);
        if (!m)
          goto failure;

        /* Fix list */
        m->next = first->next;
        first->next = m;

        /* Fix start/end */
        m->end = first->end;
        first->end = m->start;

        /* Increase the usage count of the overlapping MemMap */
        ASSERT(m->f->refcnt);
        ++m->f->refcnt;
      }

      /* Deal with the partial overlap at the end */
      if (last && last->end > mmap->end)
      {
        TRACE("splitting last mmap %p (start %lx, end %lx) at %lx\n",
              last, last->start, last->end, mmap->end);

        /* Split the partial overlap */
        m = clone_file_mmap(mmap);
        if (!m)
          goto failure;

        /* Fix list (assume correct p_last with fixes applied above) */
        m->next = last;
        if (p_last)
        {
          ASSERT(p_last->next == last);
          p_last->next = m;
        }
        else
        {
          ASSERT(pdesc->mmaps == last);
          pdesc->mmaps = m;
        }

        /* Fix start/end */
        m->start = last->start;
        last->start = m->end;

        /* Increase the usage count of the overlapping MemMap */
        ASSERT(m->f->refcnt);
        ++m->f->refcnt;

        /* Update first if it's affected as it might be used below */
        if (last == first)
          first = m;
      }

      /* Find a cloned region that starts where mmap would */
      m = first ? (first->start == mmap->start ? first : first->next) :
                  prev ? prev->next : pdesc->mmaps;
      ASSERT(m->start == mmap->start);

      /*
       * Now Free the original new mmap. We've cloned everything during overlaps
       * processing so it's no longer needed.
       */
      free(mmap);
      mmap = m;

      /* Bypass fmem and fh reference counter increase */
      goto success;
    }
  }
  else
  {
    /*
     * Anonymous maps are simple: overlaps are not possible as each mapping is
     * always backed up by its own unique memory object. Also, we come here
     * when a new memory object for a file map has just been allocated (so no
     * overlaps too).
     */
    ASSERT_MSG(!first, "%p", first);
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
    /* Reference the file handle as it's used now */
    if (!fdesc->fh)
    {
      /* If it's a newly allocated fh entry, associate it with its file desc */
      fh->refcnt = 1;
      fh->desc = fdesc;
      fdesc->fh = fh;
    }
    else
    {
      ++(fh->refcnt);
      ASSERT(fh->refcnt);
      ASSERT(fh->desc == fdesc);
      ASSERT(fdesc->fh == fh);
    }

    /* Reference the file map's memory object as it is used now */
    if (!fmap->flags)
    {
      /* If it's a newly allocated fmap entry, associate it with its file desc */
      fmem->refcnt = 1;
      fmap->flags = mmap->flags & MAP_SHARED ? MAP_SHARED : MAP_PRIVATE;
      if (mmap->flags & MAP_SHARED)
      {
        fmap->desc_g = fdesc->g;
        fdesc->g->map = fmap;
      }
      else
      {
        fmap->desc = fdesc;
        fdesc->map = fmap;
      }
    }
    else
    {
      ++(fmem->refcnt);
      ASSERT(fmem->refcnt);
      if (mmap->flags & MAP_SHARED)
      {
        ASSERT(fmap->flags == MAP_SHARED);
        ASSERT(fmap->desc_g == fdesc->g && fdesc->g->map == fmap);
      }
      else
      {
        ASSERT(fmap->flags == MAP_PRIVATE);
        ASSERT(fmap->desc == fdesc && fdesc->map == fmap);
      }
    }
  }

success:

  TRACE_IF(mmap->flags & MAP_ANON, "mmap %p (%lx..%lx (%lu))\n",
           mmap, mmap->start, mmap->end, mmap->end - mmap->start);
  TRACE_IF(!(mmap->flags & MAP_ANON), "mmap %p (%lx..%lx (%lu)), refcnt %d, fmem %p (%lx), fmap %p\n",
           mmap, mmap->start, mmap->end, mmap->end - mmap->start, mmap->f->refcnt,
           mmap->f->fmem, mmap->f->fmem->start, mmap->f->fmem->map);

  global_unlock();

  return (void *)mmap->start;

failure:

  free(mmap);

  global_unlock();

  TRACE("No memory\n");
  errno = ENOMEM;
  return MAP_FAILED;
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
  while (m && m->end <= addr)
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
  TRACE_IF(m, "found m %p (%lx..%lx (%ld), flags %x=%c%c%c%c, dos_flags %lx, refcnt %d, fmem %p (%lx))\n",
           m, m->start, m->end, m->end - m->start, m->flags,
           m->flags & MAP_SHARED ? 'S' : '-',
           m->flags & MAP_PRIVATE ? 'P' : '-',
           m->flags & MAP_FIXED ? 'F' : '-',
           m->flags & MAP_ANON ? 'A' : '-',
           m->dos_flags,
           m->flags & MAP_ANON ? 0 : m->f->refcnt,
           m->flags & MAP_ANON ? 0 : m->f->fmem,
           m->flags & MAP_ANON ? 0 : m->f->fmem->start);

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
  ASSERT(m && !(m->flags & MAP_ANON) && m->flags & MAP_SHARED && m->dos_flags & PAG_WRITE);
  ASSERT(off + len <= (m->end - m->start));
  TRACE("m %p (fmem %p), off %lu, len %lu\n", m, m->f->fmem, off, len);

  ULONG page, end;
  size_t i, j;
  uint32_t bit;
  APIRET arc;
  LONGLONG pos;

  if (len)
    len += off - PAGE_ALIGN(off);
  off = PAGE_ALIGN(off);
  if (!len)
    len = m->end - m->start - off;

  /* Find out the start position in the file */
  pos = m->f->fmem->off + m->start - m->f->fmem->start + off;

  /* Return early if pos is completely beyond EOF */
  if (pos >= m->f->fmem->map->size)
    return;

  /* Make sure we don't sync beyond EOF */
  if (pos + len > m->f->fmem->map->size)
    len = m->f->fmem->map->size - pos;

  page = m->start + off;
  end = page + len;

  i = (pos / PAGE_SIZE) / DIRTYMAP_WIDTH;
  j = (pos / PAGE_SIZE) % DIRTYMAP_WIDTH;
  bit = 0x1 << j;

  for (; page < end; ++i, j = 0, bit = 0x1)
  {
    /* Check a block of DIRTYMAP_WIDTH pages at once to quickly skip clean ones */
    if (!m->f->fh->dirtymap[i])
    {
      page += PAGE_SIZE * (DIRTYMAP_WIDTH - j);
    }
    else
    {
      for (; page < end && bit; page += PAGE_SIZE, bit <<= 1)
      {
        if (m->f->fh->dirtymap[i] & bit)
        {
          ULONG nesting, write, written;

          pos = m->f->fmem->off + page - m->f->fmem->start;
          write = page + PAGE_SIZE <= end ? PAGE_SIZE : end - page;

          TRACE("writing %lu bytes from addr %lx to fd %ld at offset %llx\n",
                write, page, m->f->fh->fd, pos);

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

          arc = DosSetFilePtrL(m->f->fh->fd, pos, FILE_BEGIN, &pos);
          ASSERT_MSG(arc == NO_ERROR, "%ld", arc);

          arc = DosWrite(m->f->fh->fd, (PVOID)page, write, &written);
          ASSERT_MSG(arc == NO_ERROR && write == written, "%ld (%lu != %lu)", arc, write, written);

          /*
           * Now propagate changes to all related mem objects. Also note that we
           * only copy to committed memory, uncommitted will be fetched directly
           * from the file upon the first access attempt.
           */
          FileMapMem *fm = m->f->fmem->map->mems;
          while (fm)
          {
            if (fm != m->f->fmem && fm->off <= pos && fm->off + fm->len > pos)
            {
              ULONG p = pos - fm->off + fm->start;
              ULONG l = PAGE_SIZE, f;

              arc = DosQueryMem((PVOID)p, &l, &f);
              ASSERT_MSG(arc == NO_ERROR, "%ld", arc);

              if (f & PAG_FREE)
              {
                TRACE("getting shared memory %lx of mem %p\n", fm->start, fm);
                /* Note: use PAG_GUARD to avoid races (see mmap_exception) */
                arc = DosGetSharedMem((PVOID)fm->start, PAG_READ | PAG_EXECUTE | PAG_GUARD);
                ASSERT_MSG(arc == NO_ERROR, "%ld", arc);
              }

              arc = DosQueryMem((PVOID)p, &l, &f);
              ASSERT_MSG(arc == NO_ERROR, "%ld", arc);

              if (f & PAG_COMMIT)
              {
                TRACE("copying %lu bytes from addr %lx to addr %lx of mem %p (%lx)\n",
                      write, page, p, fm, fm->start);
                ASSERT(pos + write <= fm->off + fm->len);

                if (!(f & PAG_WRITE))
                {
                  /* memcpy needs PAG_WRITE, avoid unneeded mmap exceptions */
                  arc = DosSetMem((PVOID)p, l, (f & fPERM) | PAG_WRITE);
                  ASSERT_MSG(arc == NO_ERROR, "%ld", arc);
                }

                memcpy((void *)p, (void *)page, write);

                if (!(f & PAG_WRITE))
                {
                  /* Restore PAG_WRITE */
                  arc = DosSetMem((PVOID)p, l, (f & fPERM));
                  ASSERT_MSG(arc == NO_ERROR, "%ld", arc);
                }
              }
            }

            fm = fm->next;
          }

          /*
           * Reset PAG_WRITE (to cause another exception upon a new write)
           * and the dirty bit on success.
           */
          ULONG dos_flags = m->dos_flags & ~PAG_WRITE;

          /*
           * Use PAG_READ if the above results in 0 since DosSetMem doesn't
           * support no protection mode.
           */
          if (dos_flags == 0)
            dos_flags |= PAG_READ;

          arc = DosSetMem((PVOID)page, PAGE_SIZE, dos_flags);
          ASSERT_MSG(arc == NO_ERROR, "%ld 0x%lx 0x%lx", arc, page, dos_flags);

          m->f->fh->dirtymap[i] &= ~bit;

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

  ASSERT(m);

  TRACE("%s mapping %p (%lx..%lx)\n",
        m->flags & MAP_SHARED ? "shared" : "private",
        m, m->start, m->end);

  if (!(m->flags & MAP_ANON))
  {
    ASSERT(m->f->fmem);
    ASSERT(m->f->fh);
    ASSERT(m->f->fh->desc);
  }

  TRACE_IF(!(m->flags & MAP_ANON), "file mapping: [%s], refcnt %d, fmem %p (%lx, refcnt %d), fh %p (%lu, refcnt %d)\n",
           m->f->fh->desc->g->path, m->f->refcnt,
           m->f->fmem, m->f->fmem->start, m->f->fmem->refcnt,
           m->f->fh, m->f->fh->fd, m->f->fh->refcnt);

  if (!(m->flags & MAP_ANON) && m->flags & MAP_SHARED && m->dos_flags & PAG_WRITE)
    flush_dirty_pages(m, 0, 0);

  if (!(m->flags & MAP_ANON))
  {
    /* Free the file handle if we are the last user */
    ASSERT(m->f->fh->refcnt);
    --(m->f->fh->refcnt);
    if (!m->f->fh->refcnt)
      free_file_handle(m->f->fh);

    /* Free the file map's memory object if we are the last user */
    ASSERT(m->f->fmem->refcnt);
    --(m->f->fmem->refcnt);
    if (!m->f->fmem->refcnt)
      free_file_map_mem(m->f->fmem);
  }
  else
  {
    /* Release process-specific memory */
    if (!prev && !desc)
    {
      /*
       * All mappings are being released at process termination in order, don't check for errors
       * as we might be releasing regions twice etc.
       */
      DosFreeMem((PVOID)m->start);
    }
    else
    {
      /*
       * Some (possibly partial) mapping is being released. Check if there are other mappings
       * referring to the underlying memory object and skip freeing it if so.
       */
      ULONG len = ~0;
      ULONG mem_flags;
      arc = DosQueryMem((PVOID)m->start, &len, &mem_flags);
      ASSERT_MSG(arc == NO_ERROR, "%ld", arc);
      if (mem_flags & PAG_BASE)
      {
        /* We're at the beginning of the region, check if there is any mapping after us */
        TRACE("beginning of memory object (len %lx), next mmapping %p (%lx..%lx)\n",
              len, m->next, m->next ? m->next->start : 0, m->next ? m->next->end : 0);
        if (!m->next || m->next->start >= m->start + len)
        {
          arc = DosFreeMem((PVOID)m->start);
          ASSERT_MSG(arc == NO_ERROR, "%ld", arc);
        }
      }
      else
      {
        /* We need to find the beginning of the memory object (it's always on a 64K boundary */
        ULONG start = m->start & 0xFFFF0000;
        while (start)
        {
          len = ~0;
          arc = DosQueryMem((PVOID)start, &len, &mem_flags);
          ASSERT_MSG(arc == NO_ERROR, "%ld", arc);
          if (mem_flags & PAG_BASE)
            break;
          start -= 0x10000;
        }
        ASSERT_MSG(start && (mem_flags & PAG_BASE), "%lx %lx", start, mem_flags);
        TRACE("middle of memory object %lx (len %lx), prev mmapping %p (%lx..%lx), next mmapping %p (%lx..%lx)\n",
              start, len, prev, prev ? prev->start : 0, prev ? prev->end : 0,
              m->next, m->next ? m->next->start : 0, m->next ? m->next->end : 0);
        if ((!prev || prev->end <= start) && (!m->next || m->next->start >= start + len))
        {
          arc = DosFreeMem((PVOID)start);
          ASSERT_MSG(arc == NO_ERROR, "%ld", arc);
        }
      }
    }
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

  global_lock();

  desc = find_proc_desc(getpid());
  ASSERT(desc);

  m = find_mmap(desc->mmaps, (ULONG)addr, &pm);

  if (m && m->start < (ULONG)addr && m->end > addr_end)
  {
    /* Special case: cut from the middle */
    TRACE("splitting mmap %p (start %lx, end %lx, refcnt %d) at %lx and %lx\n",
          m, m->start, m->end, m->flags & MAP_ANON ? 0 : m->f->refcnt,
          (ULONG)addr, addr_end);

    MemMap *nm;

    GLOBAL_NEW_PLUS(nm, m->flags & MAP_ANON ? 0 : sizeof(*nm->f));
    if (nm)
    {
      COPY_STRUCT_PLUS(nm, m, m->flags & MAP_ANON ? 0 : sizeof(*nm->f));
      if (!(nm->flags & MAP_ANON))
      {
        /* Rreference file map's memory object */
        ASSERT(nm->f->fmem->refcnt);
        ++nm->f->fmem->refcnt;

        /* Rreference file handle */
        ASSERT(nm->f->fh->refcnt);
        ++nm->f->fh->refcnt;
      }

      /* Fix pointers */
      nm->next = m->next;
      nm->start = addr_end;
      m->end = (ULONG)addr;
      m->next = nm;

      if (!(nm->flags & MAP_ANON) && nm->f->refcnt > 1)
      {
        /* We need one more region (middle) */
        MemMap *nm2 = clone_file_mmap(m);
        if (nm2)
        {
          /* Fix pointers */
          nm2->next = nm;
          nm2->start = m->end;
          nm2->end = nm->start;
          m->next = nm2;

          /* Decrease the usage count of the middle MemMap */
          --nm2->f->refcnt;
          ASSERT(nm2->f->refcnt);
        }
        else
          rc = -1;
      }
    }
    else
      rc = -1;
  }
  else do
  {
    /* Start with the next region if no match */
    if (!m)
      m = pm ? pm->next : desc->mmaps;

    if (m)
    {
      /* Proces first intersection */
      if (m->start < (ULONG)addr)
      {
        TRACE("splitting mmap %p (start %lx, end %lx, refcnt %d) at %lx\n",
              m, m->start, m->end, m->flags & MAP_ANON ? 0 : m->f->refcnt,
              (ULONG)addr);

        m->end = (ULONG)addr;
        pm = m;
        m = m->next;

        if (!(pm->flags & MAP_ANON) && pm->f->refcnt > 1)
        {
          /* We need one more region */
          MemMap *nm = clone_file_mmap(pm);
          if (!nm)
          {
            rc = -1;
            break;
          }

          /* Fix pointers */
          nm->next = m;
          nm->start = pm->end;
          nm->end = m->start;
          pm->next = nm;

          /* Decrease the usage count of the middle MemMap */
          --nm->f->refcnt;
          ASSERT(nm->f->refcnt);
        }
      }

      /* Process middle */
      while (m && m->end <= addr_end)
      {
        TRACE("unmapping mmap %p (start %lx, end %lx, refcnt %d)\n",
              m, m->start, m->end, m->flags & MAP_ANON ? 0 : m->f->refcnt);

        MemMap *n = m->next;

        if (!(m->flags & MAP_ANON) && m->f->refcnt > 1)
        {
          /* Decrease the usage count of the middle MemMap */
          --m->f->refcnt;
          ASSERT(m->f->refcnt);
          pm = m;
        }
        else
        {
          free_mmap(desc, m, pm);
        }

        m = n;
      }

      /* Proces last intersection */
      if (m && m->start < addr_end)
      {
        TRACE("splitting mmap %p (start %lx, end %lx, refcnt %d) at %lx\n",
              m, m->start, m->end, m->flags & MAP_ANON ? 0 : m->f->refcnt,
              (ULONG)addr_end);

        if (!(m->flags & MAP_ANON) && m->f->refcnt > 1)
        {
          /* We need one more region */
          MemMap *nm = clone_file_mmap(m);
          if (!nm)
          {
            rc = -1;
            break;
          }

          /* Fix pointers */
          nm->next = m;
          nm->start = m->start;
          nm->end = addr_end;
          if (pm)
            pm->next = nm;
          else
            desc->mmaps = nm;

          /* Decrease the usage count of the middle MemMap */
          --nm->f->refcnt;
          ASSERT(nm->f->refcnt);
        }

        m->start = addr_end;
      }
    }
  }
  while (0);

  /*
   * Note that if no regions match the given range at all, we will simply
   * return a success. This is a POSIX requirement.
   */

  global_unlock();

  if (rc == -1)
  {
    TRACE("No memory\n");
    errno = ENOMEM;
  }

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
    DOS_NI(arc = DosWaitEventSem(desc->mmap->flush_sem, SEM_INDEFINITE_WAIT));
    TRACE_IF(arc, "DosWaitEventSem = %ld\n", arc);

    global_lock();

    if (desc->mmap->flush_request)
    {
      TRACE("got flush request\n");

      m = desc->mmaps;
      while (m)
      {
        if (!(m->flags & MAP_ANON) && m->flags & MAP_SHARED && m->dos_flags & PAG_WRITE)
          flush_dirty_pages(m, 0, 0);
        m = m->next;
      }

      desc->mmap->flush_request = 0;
    }

    global_unlock();
  }

  /* Should never reach here: the thread gets killed at process termination */
  TRACE("Stopped\n");
  ASSERT(0);
}

/**
 * Initializes the mmap structures.
 * Called upon each process startup after successfull gpData and ProcDesc struct
 * allocation.
 */
void mmap_init(ProcDesc *proc)
{
  APIRET arc;

  /* Initialize our part of ProcDesc */
  GLOBAL_NEW(proc->mmap);
  ASSERT(proc->mmap);
  proc->mmap->flush_tid = -1;

  /* Note: we need a shared semaphore for DosAsyncTimer */
  arc = DosCreateEventSem(NULL, &proc->mmap->flush_sem, DC_SEM_SHARED | DCE_AUTORESET, FALSE);
  ASSERT_MSG(arc == NO_ERROR, "%ld", arc);
}

/**
 * Uninitializes the mmap structures.
 * Called upon each process termination before gpData destruction. Note that
 * @a proc may be NULL on critical failures.
 */
void mmap_term(ProcDesc *proc)
{
  MemMap *m;
  APIRET arc;

  /* Free our part of ProcDesc */
  if (proc)
  {
    m = proc->mmaps;
    while (m)
    {
      MemMap *n = m->next;
      free_mmap(NULL, m, NULL);
      m = n;
    }
    proc->mmaps = NULL;

    arc = DosCloseEventSem(proc->mmap->flush_sem);
    if (arc == ERROR_SEM_BUSY)
    {
      /* The semaphore may be owned by us, try to release it */
      arc = DosPostEventSem(proc->mmap->flush_sem);
      TRACE("DosPostEventSem = %ld\n", arc);
      arc = DosCloseEventSem(proc->mmap->flush_sem);
    }
    TRACE("DosCloseEventSem = %ld\n", arc);

    free(proc->mmap);
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
    ASSERT(desc->mmap->flush_tid != -1);
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
      ASSERT_MSG(arc == NO_ERROR, "%ld", arc);
      TRACE("cnt %lu\n", cnt);
    }

    if (cnt == 0)
    {
      arc = DosPostEventSem(desc->mmap->flush_sem);
      ASSERT_MSG(arc == NO_ERROR, "%ld", arc);
    }

    if (!desc->mmap->flush_request)
      desc->mmap->flush_request = 1;
  }
  else if (!desc->mmap->flush_request)
  {
    arc = DosAsyncTimer(FLUSH_DELAY, (HSEM)desc->mmap->flush_sem, NULL);
    ASSERT_MSG(arc == NO_ERROR, "%ld", arc);

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

  if (report->ExceptionNum == XCPT_ACCESS_VIOLATION ||
      report->ExceptionNum == XCPT_GUARD_PAGE_VIOLATION)
  {
    BOOL isGuard = report->ExceptionNum == XCPT_GUARD_PAGE_VIOLATION;

    ProcDesc *desc;
    MemMap *m = NULL;

    ULONG addr = report->ExceptionInfo[1];

    TRACE("%s [flags %lx nested %p addr %p]: addr %lx, info %lx\n",
          isGuard ? "XCPT_GUARD_PAGE_VIOLATION" : "XCPT_ACCESS_VIOLATION",
          report->fHandlerFlags, report->NestedExceptionReportRecord, report->ExceptionAddress,
          addr, report->ExceptionInfo[0]);

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

    TRACE_IF(m && !(m->flags & MAP_ANON), "file size %llu\n", m->f->fmem->map->size);

    if (m && m->dos_flags & fPERM &&
        (m->flags & MAP_ANON ||
         m->f->fmem->map->size > m->f->fmem->off + PAGE_ALIGN(addr - m->f->fmem->start)))
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

        if (isGuard)
        {
          /*
           * This is an effect of PAG_GUARD on a page of a file-based mapping.
           * It is used to protect from races when a thread of another process
           * kicks in by accessing the page right after the first accessing
           * thread of this process set the PAG_COMMIT flag but *before* it
           * finished reading page contents with DosRead. Letting other
           * processes continue would make them see incomplete page contents
           * leading to integrity loss and various side effects (e.g.
           * https://github.com/bitwiseworks/qtbase-os2/issues/72). But due to
           * PAG_GUARD, all other threads of all other processes will end up
           * here and since this code (as well as DosSetMem + DosRead) is under
           * global_lock, it will only run after DosRead populating page
           * contents in the first thread has done its job (and called
           * global_unlock). In other words, this place acts as a sync point
           * after which it's 100% safe to access the page in all threads.
           * Note that a similar inter-thread race within one process is
           * protected from using DosEnterCritSec below.
           */
          retry = 1;
        }
        else if (!(dos_flags & (PAG_FREE | PAG_COMMIT)))
        {
          /* First access to the allocated but uncommitted page, commit it */
          int revoke_write = 0;

          dos_flags = m->dos_flags;

          if (!(m->flags & MAP_ANON))
          {
            if (m->flags & MAP_SHARED && m->dos_flags & PAG_WRITE)
            {
              /*
               * First access to a writable shared mapping page. If it's a read
               * attempt, then we commit the page and remove PAG_WRITE so that
               * we will get an exception when the page is first written to
               * to mark it dirty. If it's a write attempt, we simply commit it
               * and mark as dirty right away.
               */
              if (report->ExceptionInfo[0] == XCPT_WRITE_ACCESS)
              {
                LONGLONG pn = (m->f->fmem->off + (page_addr - m->f->fmem->start)) / PAGE_SIZE;
                size_t i = pn / DIRTYMAP_WIDTH;
                uint32_t bit = 0x1 << (pn % DIRTYMAP_WIDTH);
                m->f->fh->dirtymap[i] |= bit;
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

          /*
           * Protect from inter-thread races between DosSetMem(PAG_COMMIT) and
           * DosRead. Note that PAG_GUARD used for inter-process race
           * protection (see above) doesn't serve well in this case because the
           * first concurring thread would reset this flag for this process
           * leaving all other threads unprotected and free to go (to read
           * inconsistent data etc.).
           */
          DosEnterCritSec();

          TRACE("Committing page at addr %lx\n", page_addr);
          arc = DosSetMem((PVOID)page_addr, len, dos_flags | PAG_COMMIT);
          TRACE_IF(arc, "DosSetMem = %ld\n", arc);
          if (!arc)
          {
            if (!(m->flags & MAP_ANON))
            {
              /*
               * Read file contents into memory. Note that if we fail here,
               * we simply let the exception go further and hopefully crash
               * and abort the application.
               */
              ULONG read = PAGE_SIZE;
              LONGLONG pos = m->f->fmem->off + page_addr - m->f->fmem->start;
              TRACE("Reading %lu bytes to addr %lx from fd %ld at offset %llx\n",
                    read, page_addr, m->f->fh->fd, pos);
              arc = DosSetFilePtrL(m->f->fh->fd, pos, FILE_BEGIN, &pos);
              TRACE_IF(arc, "DosSetFilePtrL = %ld\n", arc);
              if (!arc)
              {
                /* Page is committed, so calling original DosRead is safe. */
                arc = _doscalls_DosRead(m->f->fh->fd, (PVOID)page_addr, read, &read);
                TRACE_IF(arc, "DosRead = %ld\n", arc);
              }
            }
            if (!arc)
            {
              if (revoke_write)
              {
                TRACE("Revoking PAG_WRITE\n");
                dos_flags = m->dos_flags & ~PAG_WRITE;

                /*
                 * Use PAG_READ if the above results in 0 since DosSetMem doesn't
                 * support no protection mode.
                 */
                if (dos_flags == 0)
                  dos_flags |= PAG_READ;

                arc = DosSetMem((PVOID)page_addr, len, dos_flags);
                TRACE_IF(arc, "DosSetMem = %ld page_addr = 0x%lx dos_flags = 0x%lx\n",
                         arc, page_addr, dos_flags);
              }
              if (!arc)
              {
                /* We successfully committed and read the page, let the app retry */
                retry = 1;
              }
            }
          }

          DosExitCritSec();
        }
        else if (dos_flags & PAG_COMMIT)
        {
          if ((report->ExceptionInfo[0] == XCPT_WRITE_ACCESS && dos_flags & PAG_WRITE) ||
              (report->ExceptionInfo[0] == XCPT_READ_ACCESS && dos_flags & PAG_READ))
          {
            /* Some other thread/process was faster and did what's necessary */
            TRACE("already have necessary permissions\n");
            if (dos_flags & PAG_GUARD)
            {
              /* Optimization: avoid unneededd XCPT_GUARD_PAGE_VIOLATION */
              arc = DosSetMem((PVOID)page_addr, len, dos_flags & fPERM);
              TRACE_IF(arc, "DosSetMem = %ld\n", arc);
            }
            if (!arc)
              retry = 1;
          }
          else if (report->ExceptionInfo[0] == XCPT_WRITE_ACCESS &&
                   !(dos_flags & PAG_WRITE) &&
                   !(m->flags & MAP_ANON) && m->flags & MAP_SHARED && m->dos_flags & PAG_WRITE)
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
              LONGLONG pn = (m->f->fmem->off + (page_addr - m->f->fmem->start)) / PAGE_SIZE;
              size_t i = pn / DIRTYMAP_WIDTH;
              uint32_t bit = 0x1 << (pn % DIRTYMAP_WIDTH);
              m->f->fh->dirtymap[i] |= bit;
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

static void sync_map(ProcDesc *desc, MemMap *m, ULONG addr, size_t len, int flags)
{
  /*
   * Only do real work on shared writable file mappings and silently
   * ignore other types (POSIX doesn't specify any particular behavior
   * in such a case).
   */
  if (!(m->flags & MAP_ANON) && m->flags & MAP_SHARED && m->dos_flags & PAG_WRITE)
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
      ULONG off = 0;
      if (addr > m->start)
        off = addr - m->start;
      else
        len -= m->start - addr;
      if (off + len > (m->end - m->start))
        len = m->end - m->start - off;
      flush_dirty_pages(m, off, len);
    }
  }
}

int msync(void *addr, size_t len, int flags)
{
  ProcDesc *desc;
  MemMap *m, *pm;
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
  ASSERT(desc);

  m = find_mmap(desc->mmaps, (ULONG)addr, &pm);

  /* Start with the next region if no match */
  if (!m)
    m = pm ? pm->next : desc->mmaps;
  if (m)
  {
    /* Proces first intersection */
    if (m->start < (ULONG)addr)
    {
      sync_map(desc, m, (ULONG)addr, len, flags);
      m = m->next;
    }
    /* Process middle */
    while (m && m->start < addr_end && m->end <= addr_end)
    {
      sync_map(desc, m, (ULONG)addr, len, flags);
      m = m->next;
    }
    /* Proces last intersection */
    if (m && m->start < (ULONG)addr_end && m->end > addr_end)
    {
      sync_map(desc, m, (ULONG)addr, len, flags);
    }
  }

  global_unlock();

  return 0;
}

static int advise_map(ProcDesc *desc, MemMap *m, ULONG addr, size_t len, int flags)
{
  APIRET arc;
  int rc = 0;

  if (addr < m->start)
  {
    addr = m->start;
    len -= m->start - addr;
  }
  if (addr + len > m->end)
    len = m->end - addr;

  TRACE("m %p, adjusted addr %lx, len %u\n", m, addr, len);

  if (flags & MADV_DONTNEED)
  {
    ULONG query_len = len;
    ULONG dos_flags;
    arc = DosQueryMem((PVOID)addr, &query_len, &dos_flags);
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
        arc = DosSetMem((PVOID)addr, len, PAG_DECOMMIT);
        TRACE_IF(arc, "DosSetMem = %ld\n", arc);
        if (arc)
        {
          errno = EINVAL;
          rc = -1;
        }
      }
    }
  }

  return rc;
}

int madvise(void *addr, size_t len, int flags)
{
  ProcDesc *desc;
  MemMap *m, *pm;
  ULONG addr_end;
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
  ASSERT(desc);

  m = find_mmap(desc->mmaps, (ULONG)addr, &pm);

  /* Start with the next region if no match */
  if (!m)
    m = pm ? pm->next : desc->mmaps;
  if (m)
  {
    /* Proces first intersection */
    if (m->start < (ULONG)addr)
    {
      rc = advise_map(desc, m, (ULONG)addr, len, flags);
      m = m->next;
    }
    /* Process middle */
    while (rc == 0 && m && m->start < addr_end && m->end <= addr_end)
    {
      rc = advise_map(desc, m, (ULONG)addr, len, flags);
      m = m->next;
    }
    /* Proces last intersection */
    if (rc == 0 && m && m->start < (ULONG)addr_end && m->end > addr_end)
    {
      rc = advise_map(desc, m, (ULONG)addr, len, flags);
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

static int protect_map(ProcDesc *desc, MemMap *m, ULONG addr, size_t len, ULONG dos_flags)
{
  int rc = 0;

  if (addr < m->start)
  {
    addr = m->start;
    len -= m->start - addr;
  }
  if (addr + len > m->end)
    len = m->end - addr;

  TRACE("m %p, adjusted addr %lx, len %u, dos_flags %lx\n", m, addr, len, dos_flags);

  if (dos_flags == -1)
  {
    /*
     * This is check mode. We will return 0 if the check is passed for the
     * given region and -1 otherwise.
     */
    if (!(m->flags & MAP_ANON))
      return -1;

    /*
     * Also fail the check if there is a partial mprotect: we don't support
     * that ATM (see #75).
     */
    if (addr > m->start || addr + len < m->end)
      return -1;

    return 0;
  }

  if (m->dos_flags != dos_flags)
  {
    TRACE("changing dos_flags from %lx to %lx\n", m->dos_flags, dos_flags);
    m->dos_flags = dos_flags;
  }

  return 0;
}

int _std_mprotect(const void *addr, size_t len, int prot);

int mprotect(const void *addr, size_t len, int prot)
{
  ProcDesc *desc;
  MemMap *m, *pm, *mmap;
  ULONG addr_end;
  ULONG dos_flags;
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
  ASSERT(desc);

  mmap = find_mmap(desc->mmaps, (ULONG)addr, &pm);

  /* Start with the next region if no match */
  if (!mmap)
    mmap = pm ? pm->next : desc->mmaps;

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
  m = mmap;
  if (m)
  {
    /* Proces first intersection */
    if (m->start < (ULONG)addr)
    {
      rc = protect_map(desc, m, (ULONG)addr, len, -1);
      m = m->next;
    }
    /* Process middle */
    while (rc == 0 && m && m->start < addr_end && m->end <= addr_end)
    {
      rc = protect_map(desc, m, (ULONG)addr, len, -1);
      m = m->next;
    }
    /* Proces last intersection */
    if (rc == 0 && m && m->start < (ULONG)addr_end && m->end > addr_end)
    {
      rc = protect_map(desc, m, (ULONG)addr, len, -1);
    }
  }

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

    if (!rc)
    {
      /* Record new mode in affected mappings on success */
      m = mmap;
      if (m)
      {
        /* Proces first intersection */
        if (m->start < (ULONG)addr)
        {
          protect_map(desc, m, (ULONG)addr, len, dos_flags);
          m = m->next;
        }
        /* Process middle */
        while (rc == 0 && m && m->start < addr_end && m->end <= addr_end)
        {
          protect_map(desc, m, (ULONG)addr, len, dos_flags);
          m = m->next;
        }
        /* Proces last intersection */
        if (rc == 0 && m && m->start < (ULONG)addr_end && m->end > addr_end)
        {
          protect_map(desc, m, (ULONG)addr, len, dos_flags);
        }
      }
    }
  }
  else
  {
    /* Some region is incompatible, fail */
    errno = EACCES;
  }

  global_unlock();

  return rc;
}

int _std_ftruncate(int fildes, __off_t length);

/**
 * LIBC fcntl replacement. Changes the recorded file size in mmap structures
 * to avoid writing beyond EOF and enable writing within the new file size.
 *
 * Note that for now this belongs to mmap as only mmap needs  this override.
 * Once it's needed for something else, it should be moved to shared.c with
 * adding necessary callbacks (similar to close()).
 */
int ftruncate(int fildes, __off_t length)
{
  __LIBC_PFH pFH;
  int rc;

  TRACE("fd %d, length %lld\n", fildes, length);

  pFH = __libc_FH(fildes);
  if (!pFH || !pFH->pszNativePath)
  {
    errno = !pFH ? EBADF : EINVAL;
    return -1;
  }

  global_lock();

  rc = _std_ftruncate(fildes, length);
  if (rc == 0)
  {
    /* Update file size in FileMap structs */
    FileDesc *fdesc;
    SharedFileDesc *fdesc_g;

    fdesc = find_file_desc(pFH->pszNativePath, &fdesc_g);

    if (fdesc_g && fdesc_g->map)
    {
      TRACE("updating size for [%s] in shared fmap %p from %llu to %llu\n",
            pFH->pszNativePath, fdesc_g->map, fdesc_g->map->size, length);
      fdesc_g->map->size = length;
    }

    if (fdesc && fdesc->map) {
      TRACE("updating size for [%s] in private fmap %p from %llu to %llu\n",
            pFH->pszNativePath, fdesc->map, fdesc->map->size, length);
      fdesc->map->size = length;
    }
  }

  TRACE("returning %d\n", rc);

  global_unlock();

  return rc;
}

#ifdef DEBUG
/**
 * Used to force mmap() allocate memory objects for file mappings at least
 * as big as the initial file size rather than as the requested length of the
 * mapping. Used in some tests.
 */
void set_mmap_full_size(int val)
{
  mmap_full_size = val;
}

/**
 * Returns a list of all memory mappings of this process. Used in some tests.
 * If pid is -1, returns the mappings of the current process. Note that
 * global_lock() must be used for the duration of use of the returned value
 * to provide atomic access.
 */
MemMap *get_proc_mmaps(int pid)
{
  ProcDesc *desc = find_proc_desc(pid == -1 ? getpid() : pid);
  ASSERT(desc);
  return desc->mmaps;
}
#endif

static void forkCompletion(void *pvArg, int rc, __LIBC_FORKCTX enmCtx);

static int forkParentChild(__LIBC_PFORKHANDLE pForkHandle, __LIBC_FORKOP enmOperation)
{
  int rc = 0;

  if (enmOperation == __LIBC_FORK_OP_EXEC_PARENT)
  {
    /*
     * Register a completion function that will be executed in the child process
     * right before returning from fork(), i.e. when LIBC is fully operational
     * (as opposed to the __LIBC_FORK_OP_FORK_CHILD callback time when e.g. LIBC
     * Heap is locked and can't be used, see shared.c for more details).
     */
    rc = pForkHandle->pfnCompletionCallback(pForkHandle, forkCompletion, (void *)pForkHandle->pidParent,
                                            __LIBC_FORK_CTX_CHILD | __LIBC_FORK_CTX_FLAGS_LAST);
  }
  else if (enmOperation == __LIBC_FORK_OP_FORK_PARENT)
  {
    ProcDesc *desc;
    APIRET arc;
    MemMap *m;

    global_lock();

    desc = find_proc_desc(getpid());
    ASSERT(desc);

    /* Give access to shared mappings to the forked child process */
    m = desc->mmaps;
    while (m)
    {
      if (m->flags & MAP_SHARED)
      {
        ULONG start = m->start;
        ULONG dos_flags = m->dos_flags;

        TRACE_IF((m->flags & MAP_ANON), "giving mapping %p (start %lx) to pid %x\n",
                 m, m->start, pForkHandle->pidChild);
        TRACE_IF(!(m->flags & MAP_ANON), "giving mapping %p (start %lx, fmem %p, fmem->start %lx) to pid %x\n",
                 m, m->start, m->f->fmem, m->f->fmem->start, pForkHandle->pidChild);
        if (!(m->flags & MAP_ANON))
        {
          start = m->f->fmem->start;
          /*
           * Don't set PAG_WRITE to cause an exception upon the first
           * write to a page (see mmap_exception()).
           * Note: use PAG_GUARD to avoid races (see mmap_exception).
           */
          dos_flags = PAG_READ | PAG_EXECUTE | PAG_GUARD;
        }
        /*
         * Note that if there is more than 1 user of the file map, we will give
         * the same region multiple times, but this should not hurt.
         */
        arc = DosGiveSharedMem((PVOID)start, pForkHandle->pidChild, dos_flags | PAG_GUARD);
        ASSERT_MSG(arc == NO_ERROR, "%ld", arc);
      }
      m = m->next;
    }

    global_unlock();
  }

  return rc;
}

_FORK_PARENT1(0, forkParentChild);

static void forkCompletion(void *pvArg, int rc, __LIBC_FORKCTX enmCtx)
{
  ASSERT(enmCtx == __LIBC_FORK_CTX_CHILD);

  pid_t pidParent = (pid_t)pvArg;

  ProcDesc *desc;
  MemMap *m;

  ProcDesc *pdesc;
  MemMap *newm;
  BOOL ok = TRUE;

  global_lock();

  pdesc = find_proc_desc(pidParent);
  ASSERT(pdesc);

  desc = find_proc_desc(getpid());
  ASSERT(desc);

  /* Copy parent's shared mappings to our own list */
  m = pdesc->mmaps;
  while (m)
  {
    if (m->flags & MAP_SHARED)
    {
      TRACE_IF(m->flags & MAP_ANON, "restoring mapping %p (start %lx)\n", m, m->start);
      TRACE_IF(!(m->flags & MAP_ANON), "restoring mapping %p (start %lx, fmem %p, fmem->start %lx)\n",
               m, m->start, m->f->fmem, m->f->fmem->start);
      GLOBAL_NEW_PLUS(newm, m->flags & MAP_ANON ? 0 : sizeof(*newm->f));
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
      COPY_STRUCT_PLUS(newm, m, m->flags & MAP_ANON ? 0 : sizeof(*newm->f));

      if (!(newm->flags & MAP_ANON))
      {
        const char *path = newm->f->fmem->map->desc_g->path;
        TRACE("new file mapping for [%s]\n", path);

        int dirtymap_sz = 0;
        FileHandle *fh;
        FileDesc *fdesc;

        if (m->dos_flags & PAG_WRITE)
          dirtymap_sz = DIVIDE_UP(NUM_PAGES(newm->f->fmem->map->size), DIRTYMAP_WIDTH) * (DIRTYMAP_WIDTH / 8);

        /* Get a file descrition for this process (will create a new one if needed) */
        fdesc = get_file_desc(-1, path);
        if (!fdesc)
        {
          ok = FALSE;
          break;
        }

        fh = fdesc->fh;
        if (!fh)
        {
          /* Create a new file handle for this process */
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
            ok = FALSE;
            break;
          }

          fh->fd = newm->f->fh->fd;

          /* Associate the new file handle with the description */
          fh->desc = fdesc;
          fdesc->fh = fh;

          TRACE("new file handle %p (fd %ld, dirty map size %u bytes)\n", fh, fh->fd, dirtymap_sz);
        }

        newm->f->fh = fh;

        /* Reference file handle */
        ++(newm->f->fh->refcnt);
        ASSERT(newm->f->fh->refcnt);

        /* Reference file map */
        ++(newm->f->fmem->refcnt);
        ASSERT(newm->f->fmem->refcnt);
      }

      /* Add the new entry the list */
      newm->next = desc->mmaps;
      desc->mmaps = newm;
    }
    m = m->next;
  }

  global_unlock();

  /*
   * A failure here means out of memory when copying mappings so we have to
   * abort the process as there is no way to recover from this.
   */
  ASSERT(ok);
}
