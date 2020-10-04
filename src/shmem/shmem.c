/*
 * Implementation of shared memory API.
 * Copyright (C) 2020 bww bitwise works GmbH.
 * This file is part of the kLIBC Extension Library.
 * Authored by Dmitry Kuminov <coding@dmik.org>, 2020.
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
#include <InnoTekLIBC/errno.h>

#define TRACE_GROUP TRACE_GROUP_SHMEM

#include "../shared.h"

#include "libcx/shmem.h"

#define SHMEM_MIN_HANDLES 16
#define SHMEM_MAX_HANDLES 32768

#define SHMEM_FREE 0x80000000

typedef struct ShmemView
{
  struct ShmemView *next;
  size_t offset; /* Offset of the mapping */
  size_t length; /* Length of the mapping */
  size_t refs; /* Reference counter for this mapping */
} ShmemView;

typedef struct ShmemProc
{
  struct ShmemProc *next;
  pid_t pid; /* pid of the process */
  int flags; /* SHMEM_ flags */
  struct ShmemView *views; /* List of mappings (views) */
} ShmemProc;

typedef struct ShmemObj
{
  PVOID addr; /* Virtual address of the memory object */
  size_t size; /* Size of the memory object as was given in `shmem_create` */
  size_t act_size; /* Actual size of the memory object */
  size_t refs; /* Reference counter for this mapping (handles and views) */
} ShmemObj;

typedef struct ShmemHandle
{
  struct ShmemObj *obj; /* Memory object this handle represents */
  int flags; /* SHMEM_ flags */
  struct ShmemProc *procs; /* List of processes using this handle */
} ShmemHandle;

/**
 * Global system-wide shmem API data structure.
 */
typedef struct ShmemData
{
  struct ShmemHandle *handles; /* Array of all available handles */
  size_t handles_size; /* Size of the handle array */
  size_t handles_count; /* Number of used array entries */
  size_t handles_free; /* Index of the first free entry */
} ShmemData;

/**
 * Emulates semantics of Windows |VirtualAlloc|/|VirtualFree| that allow to
 * commit and decommit already committed or decommitted pages.
 */
APIRET MyDosSetMem(PVOID base, ULONG length, ULONG flags)
{
  if (!(flags & (PAG_COMMIT | PAG_DECOMMIT)))
    return DosSetMem(base, length, flags);

  /* Query the current state of each range to avoid committing/decommitting
   * already commited/decommitted pages. */
  PVOID addr = base;
  APIRET arc;
  while (length)
  {
    ULONG act_len = length, act_flags, new_flags = flags;
    arc = DosQueryMem(addr, &act_len, &act_flags);
    TRACE("arc %lu addr %p act_len %lu act_flags 0x%X\n", arc, addr, act_len, act_flags);
    if (arc != NO_ERROR)
      break;
    if ((new_flags & PAG_COMMIT) && (act_flags & PAG_COMMIT))
      new_flags &= ~PAG_COMMIT;
    if ((new_flags & PAG_DECOMMIT) && !(act_flags & (PAG_COMMIT | PAG_FREE)))
      new_flags &= ~PAG_DECOMMIT;
    if ((new_flags & (PAG_COMMIT | PAG_DECOMMIT)) ||
        (new_flags & fPERM) != (act_flags & fPERM))
    {
      arc = DosSetMem(addr, act_len, new_flags);
      if (arc != NO_ERROR)
        break;
    }
    addr = addr + act_len;
    length -= act_len;
  }

  return NO_ERROR;
}

/**
 * Dereferences a given memory object. If the reference count goes to zero,
 * will free the underlying OS/2 memory object and remove the entry from the
 * list of known memory objects.
 */
static void unref_obj(ShmemObj *obj)
{
  ASSERT(obj && obj->refs);

  --obj->refs;
  if (!obj->refs)
  {
    /* Free underlying memory */
    APIRET arc = DosFreeMem(obj->addr);
    TRACE("freed mem obj addr %p size %u with arc %d\n", obj->addr, obj->size, arc);

    free(obj);
  }
}

/**
 * Removes a given proc entry from the list of procs of a the given handle and
 * frees it. Also releases the handle if it has no more procs using it (which
 * may also free the underlying memory object if it becomes unused).
 */
static void free_proc(SHMEM h, ShmemProc *proc, ShmemProc *prev_proc)
{
  struct ShmemHandle *hnd = &gpData->shmem->handles[h];

  /* Remove this proc from the list and free it */
  if (prev_proc)
    prev_proc->next = proc->next;
  else
    hnd->procs = proc->next;
  free(proc);

  if (!hnd->procs)
  {
    /* The last proc has gone, release the handle */
    unref_obj(hnd->obj);
    CLEAR_STRUCT(hnd);

    /* Make sure the free handle is always the first (smallest) available one */
    if (gpData->shmem->handles_free > h)
      gpData->shmem->handles_free = h;

    /* Decrease the total number of handles */
    ASSERT(gpData->shmem->handles_count);
    --gpData->shmem->handles_count;

    TRACE("freed unused handle, new free %u size %u count %u\n",
      gpData->shmem->handles_free, gpData->shmem->handles_size, gpData->shmem->handles_count);
  }
}

/**
 * Looks for a proc entry with a given pid. Returns a found proc or NULL if
 * there is none. Also returns a previous proc entry if @a prev is not NULL.
 */
static ShmemProc *find_proc(ShmemProc *first, pid_t pid, ShmemProc **prev)
{
  struct ShmemProc *proc = first;
  struct ShmemProc *prev_proc = NULL;
  while (proc && proc->pid != pid)
  {
    prev_proc = proc;
    proc = proc->next;
  }

  TRACE("prev_proc %p proc %p flags 0x%X views %p pid %d\n",
        prev_proc, proc, proc ? proc->flags : 0, proc ? proc->views : 0, pid);

  if (prev)
    *prev = prev_proc;

  return proc;
}

/**
 * Looks for a view entry with a given offset (assuming the list is sorted by
 * offset). Returns a view with either the same or greater offset or NULL if
 * there is none of either. Also returns a previous view entry if @a prev is not
 * NULL.
 */
static ShmemView *find_view(ShmemView *first, size_t offset, ShmemView **prev)
{
  struct ShmemView *view = first;
  struct ShmemView *prev_view = NULL;
  while (view && view->offset < offset)
  {
    prev_view = view;
    view = view->next;
  }

  TRACE("prev_view %p view %p offset %u length %u refs %u\n",
    prev_view, view, view ? view->offset : 0, view ? view->length : 0, view ? view->refs : 0);

  if (prev)
    *prev = prev_view;

  return view;
}

/**
 * Checks if the handle is valid and returns its array entry on success or NULL
 * on failure.
 */
static ShmemHandle *get_handle(SHMEM h)
{
  if (h >= gpData->shmem->handles_size || !gpData->shmem->handles[h].obj)
    return NULL;

  struct ShmemHandle *hnd = &gpData->shmem->handles[h];

  ASSERT(hnd->procs);

  TRACE("flags 0x%X obj addr %p size %u act_size %u refs %u\n",
        hnd->flags, hnd->obj->addr, hnd->obj->size, hnd->obj->act_size, hnd->obj->refs);

  return hnd;
}

/**
 * Allocates a new handle and returns its array entry on success or NULL on
 * failure.
 */
static ShmemHandle *alloc_handle(SHMEM *h)
{
  if (gpData->shmem->handles_free == gpData->shmem->handles_size)
  {
      /* Need more handles */
    size_t size = gpData->shmem->handles_size + SHMEM_MIN_HANDLES;
    ShmemHandle *handles;

    TRACE("resizing handles from %u to %u\n", gpData->shmem->handles_size, size);

    if (size > SHMEM_MAX_HANDLES ||
      !(handles = RENEW_ARRAY(gpData->shmem->handles, gpData->shmem->handles_size, size)))
    {
      errno = ENOMEM;
      return NULL;
    }

    gpData->shmem->handles_size = size;
    gpData->shmem->handles = handles;
  }

  ASSERT_MSG(gpData->shmem->handles_free < gpData->shmem->handles_size, "%u %u",
             gpData->shmem->handles_free, gpData->shmem->handles_size);

  *h = gpData->shmem->handles_free;

  /* Calculate the new free index (will wrap if needed) */
  size_t free = (gpData->shmem->handles_free + 1) % gpData->shmem->handles_size;
  while (free != *h && gpData->shmem->handles[free].obj)
    ++free;

  /* Set free to size if out of free handles (to cause array expansion later) */
  if (free == *h)
    free = gpData->shmem->handles_size;
  gpData->shmem->handles_free = free;

  /* Increase the total number of valid handles */
  ++gpData->shmem->handles_count;
  ASSERT(gpData->shmem->handles_count <= SHMEM_MAX_HANDLES);

  TRACE("allocated %u new free %u size %u count %u\n",
        *h, gpData->shmem->handles_free, gpData->shmem->handles_size, gpData->shmem->handles_count);

  struct ShmemHandle *hnd = &gpData->shmem->handles[*h];

  ASSERT_MSG(!hnd->obj && !hnd->procs, "%u %p %p", *h, hnd->obj, hnd->procs);

  return hnd;
}

/**
 * Initializes the shmem shared structures.
 * Called upon each process startup after successfull ProcDesc and gpData
 * allocation.
 */
void shmem_data_init(ProcDesc *proc)
{
  if (gpData->refcnt == 1)
  {
    /* We are the first processs, initialize shmem structures */
    GLOBAL_NEW(gpData->shmem);
    ASSERT(gpData->shmem);

    gpData->shmem->handles_size = SHMEM_MIN_HANDLES;
    GLOBAL_NEW_ARRAY(gpData->shmem->handles, gpData->shmem->handles_size);
    ASSERT(gpData->shmem->handles);
  }
}

/**
 * Uninitializes the shem shared structures.
 * Called upon each process termination before gpData destruction. Note that
 * @a proc may be NULL on critical failures.
 */
void shmem_data_term(ProcDesc *proc)
{
  TRACE("gpData->shmem->handles_count %u\n", gpData->shmem->handles_count);

  if (gpData->shmem->handles_count)
  {
    /* There are some handles, release ourselves from the ones we use */
    for (size_t h = 0, count = gpData->shmem->handles_count; count; ++h)
    {
      struct ShmemHandle *hnd = &gpData->shmem->handles[h];
      if (hnd->obj)
      {
        /* A valid handle */
        ASSERT(hnd->procs);

        --count;

        ShmemProc *prev_proc;
        ShmemProc *proc = find_proc(hnd->procs, getpid(), &prev_proc);

        if (proc)
        {
          TRACE("releasing handle %u flags 0x%X obj addr %p size %u refs %u\n",
                h, hnd->flags, hnd->obj->addr, hnd->obj->size, hnd->obj->refs);

          /* Free all views */
          ShmemView *view = proc->views;
          while (view)
          {
            ShmemView *prev_view = view;
            view = view->next;
            free(prev_view);

            /* Each view holds an implicit ref to the memory object, release it */
            unref_obj(hnd->obj);
          }

          free_proc(h, proc, prev_proc);
        }
      }
    }
  }

  if (gpData->refcnt == 0)
  {
    /* We are the last process, free shmem structures */
    free(gpData->shmem->handles);
    free(gpData->shmem);
  }
}

SHMEM shmem_create(size_t size, int flags)
{
  TRACE("size %u flags 0x%X\n", size, flags);

  /* validate arguments */
  if (!size || (flags & ~SHMEM_PUBLIC))
  {
    errno = EINVAL;
    return SHMEM_INVALID;
  }

  /* First, try to allocate an OS/2 memory object */
  PVOID addr = NULL;
  ULONG dos_flags = PAG_READ | PAG_EXECUTE;
  if (!(flags & SHMEM_READONLY))
    dos_flags |= PAG_WRITE;
  if (flags & SHMEM_PUBLIC)
    dos_flags |= OBJ_GETTABLE;
  else
    dos_flags |= OBJ_GIVEABLE;
  APIRET arc = DosAllocSharedMem(&addr, NULL, size, dos_flags | OBJ_ANY);
  if (arc)
    arc = DosAllocSharedMem(&addr, NULL, size, dos_flags);

  TRACE("arc %lu dos_flags 0x%X addr %p\n", arc, dos_flags, addr);

  if (arc)
  {
    errno = __libc_native2errno(arc);
    return SHMEM_INVALID;
  }

  ULONG act_size = ~0;
  ULONG act_flags;
  arc = DosQueryMem(addr, &act_size, &act_flags);

  TRACE("arc %lu act_size %lu act_flags 0x%X\n", arc, act_size, act_flags);
  ASSERT_MSG(!arc, "%lu", arc);

  SHMEM h = SHMEM_INVALID;

  global_lock();

  do
  {
    struct ShmemObj *obj;
    if (!(GLOBAL_NEW(obj)))
    {
      errno = ENOMEM;
      break;
    }

    struct ShmemProc *proc;
    if (!(GLOBAL_NEW(proc)))
    {
      free(obj);
      errno = ENOMEM;
      break;
    }

    struct ShmemHandle *hnd = alloc_handle(&h);
    if (!hnd)
    {
      free(obj);
      free(proc);
      errno = ENOMEM;
      break;
    }

    obj->addr = addr;
    obj->size = size;
    obj->act_size = act_size;
    obj->refs = 1; /* Handle reference */

    proc->pid = getpid();
    proc->flags = flags;

    hnd->obj = obj;
    hnd->flags = flags;
    hnd->procs = proc;
  }
  while (0);

  global_unlock();

  /* Free the OS/2 memory object on failure */
  if (h == SHMEM_INVALID)
    DosFreeMem(addr);

  TRACE("h %u errno %d\n", h, errno);
  return h;
}

int shmem_give(SHMEM h, pid_t pid, int flags)
{
  TRACE("h %d pid %d flags 0x%X\n", h, pid, flags);

  /* validate arguments */
  if (h == SHMEM_INVALID || pid == -1 || (flags & ~SHMEM_READONLY))
  {
    errno = EINVAL;
    return -1;
  }

  int rc = -1;

  global_lock();

  do
  {
    struct ShmemHandle *hnd = get_handle(h);

    if (!hnd)
    {
      errno = EINVAL;
      break;
    }

    struct ShmemProc *proc = find_proc(hnd->procs, pid, NULL);

    if (proc && !(proc->flags & SHMEM_FREE))
    {
      /* The handle is already used in this process (and not freed) */
      errno = EPERM;
      break;
    }

    if (!proc && !(GLOBAL_NEW(proc)))
    {
      errno = ENOMEM;
      break;
    }

    if (proc->flags & SHMEM_FREE)
    {
      /* The target process already used this handle before and the memory
       * object is still alive (because of some mappings around), just reuse it */
      ASSERT (proc->views);
      proc->flags &= ~SHMEM_FREE;
    }
    else
    {
      /* Give access to the memory object (obey the handle restriction) */
      ULONG dos_flags = PAG_READ | PAG_EXECUTE;
      if (!(flags & SHMEM_READONLY) && !(hnd->flags & SHMEM_READONLY))
        dos_flags |= PAG_WRITE;
      APIRET arc = DosGiveSharedMem(hnd->obj->addr, pid, dos_flags);

      TRACE("arc %lu dos_flags 0x%X\n", arc, dos_flags);

      if (arc)
      {
        /* Free the new, unused proc entry */
        free(proc);
        errno = __libc_native2errno(arc);
        break;
      }

      proc->pid = pid;

      /* Insert this proc to the beginning of the list */
      proc->next = hnd->procs;
      hnd->procs = proc;
    }

    /* Memorize handle flags for this process (to obey restrictions) */
    proc->flags |= flags;

    /* Success */
    rc = 0;
  }
  while (0);

  global_unlock();

  TRACE("rc %d errno %d\n", rc, errno);
  return rc;
}

int shmem_open(SHMEM h, int flags)
{
  TRACE("h %d flags 0x%X\n", h, flags);

  /* validate arguments */
  if (h == SHMEM_INVALID || (flags & ~SHMEM_READONLY))
  {
    errno = EINVAL;
    return -1;
  }

  int rc = -1;

  global_lock();

  do
  {
    struct ShmemHandle *hnd = get_handle(h);

    if (!hnd)
    {
      errno = EINVAL;
      break;
    }

    struct ShmemProc *proc = find_proc(hnd->procs, getpid(), NULL);

    if (proc && !(proc->flags & SHMEM_FREE))
    {
      /* The handle is already used in this process (and not freed) */
      errno = EPERM;
      break;
    }

    if (!proc && !(GLOBAL_NEW(proc)))
    {
      errno = ENOMEM;
      break;
    }

    if (proc->flags & SHMEM_FREE)
    {
      /* This process already used this handle before and the memory object is
       * still alive (because of some mappings around), just reuse it */
      ASSERT (proc->views);
      proc->flags &= ~SHMEM_FREE;
    }
    else
    {
      /* Get access to the memory object (obey the handle restriction) */
      ULONG dos_flags = PAG_READ | PAG_EXECUTE;
      if (!(flags & SHMEM_READONLY) && !(hnd->flags & SHMEM_READONLY))
        dos_flags |= PAG_WRITE;
      APIRET arc = DosGetSharedMem(hnd->obj->addr, dos_flags);

      TRACE("arc %lu dos_flags 0x%X\n", arc, dos_flags);

      if (arc)
      {
        /* Free the new, unused proc entry */
        free(proc);
        errno = __libc_native2errno(arc);
        break;
      }

      proc->pid = getpid();

      /* Insert this proc to the beginning of the list */
      proc->next = hnd->procs;
      hnd->procs = proc;
    }

    /* Memorize handle flags for this process (to obey restrictions) */
    proc->flags |= flags;

    /* Success */
    rc = 0;
  }
  while (0);

  global_unlock();

  TRACE("rc %d errno %d\n", rc, errno);
  return rc;
}

SHMEM shmem_duplicate(SHMEM h, int flags)
{
  TRACE("h %d flags 0x%X\n", h, flags);

  /* validate arguments */
  if (h == SHMEM_INVALID || (flags & ~SHMEM_READONLY))
  {
    errno = EINVAL;
    return -1;
  }

  SHMEM dup_h = SHMEM_INVALID;

  global_lock();

  do
  {
    struct ShmemHandle *hnd = get_handle(h);
    if (!hnd)
    {
      errno = EINVAL;
      break;
    }

    struct ShmemProc *proc;
    if (!(GLOBAL_NEW(proc)))
    {
      errno = ENOMEM;
      break;
    }

    struct ShmemHandle *dup_hnd = alloc_handle(&dup_h);
    if (!dup_hnd)
    {
      free(proc);
      errno = ENOMEM;
      break;
    }

    ++hnd->obj->refs; /* Handle reference */
    ASSERT(hnd->obj->refs);

    proc->pid = getpid();
    /* Ihnerit selected source handle's process-specific flags */
    proc->flags = flags | (proc->flags & SHMEM_READONLY);

    dup_hnd->obj = hnd->obj;
    /* Ihnerit selected source handle's flags */
    dup_hnd->flags = flags | (hnd->flags & SHMEM_READONLY);
    dup_hnd->procs = proc;
  }
  while (0);

  global_unlock();

  TRACE("dup_h %u errno %d\n", dup_h, errno);
  return dup_h;
}

int shmem_close(SHMEM h)
{
  TRACE("h %d\n", h);

  /* validate arguments */
  if (h == SHMEM_INVALID)
  {
    errno = EINVAL;
    return -1;
  }

  int rc = -1;

  global_lock();

  do
  {
    struct ShmemHandle *hnd = get_handle(h);

    if (!hnd)
    {
      errno = EINVAL;
      break;
    }

    struct ShmemProc *prev_proc;
    struct ShmemProc *proc = find_proc(hnd->procs, getpid(), &prev_proc);

    if (!proc || (proc->flags & SHMEM_FREE))
    {
      /* The handle is not used in this process */
      errno = EINVAL;
      break;
    }

    if (proc->views)
    {
      /* The handle still has some mappings in this process, just mark it as freed */
      proc->flags |= SHMEM_FREE;
    }
    else
    {
      /* No views, okay to get rid of the proc entry (may release handle & mem) */
      free_proc(h, proc, prev_proc);
    }

    /* Success */
    rc = 0;
  }
  while (0);

  global_unlock();

  TRACE("rc %d errno %d\n", rc, errno);
  return rc;
}

void *shmem_map(SHMEM h, off_t offset, size_t length)
{
  TRACE("h %d offset %llu length %u\n", h, offset, length);

  /* validate arguments */
  if (h == SHMEM_INVALID || !PAGE_ALIGNED(offset) ||
     (length != 0 && offset + length <= offset))
  {
    errno = EINVAL;
    return NULL;
  }

  void *addr = NULL;

  global_lock();

  do
  {
    struct ShmemHandle *hnd = get_handle(h);

    if (!hnd)
    {
      errno = EINVAL;
      break;
    }

    if (!length)
      length = hnd->obj->size - offset;

    /* Check if the range is valid */
    if (offset + length > hnd->obj->size)
    {
      errno = ERANGE;
      break;
    }

    struct ShmemProc *proc = find_proc(hnd->procs, getpid(), NULL);

    if (!proc || (proc->flags & SHMEM_FREE))
    {
      /* The handle is not used in this process */
      errno = EINVAL;
      break;
    }

    /* Commit memory (may fail due to out-of-memory or such). Note that we obey
     * the restriction of the specified handle which will cause a change of
     * access for all other mappings, which is documented. */
    ULONG dos_flags = PAG_COMMIT | PAG_READ | PAG_EXECUTE;
    if (!(proc->flags & SHMEM_READONLY) && !(hnd->flags & SHMEM_READONLY))
      dos_flags |= PAG_WRITE;
    APIRET arc = MyDosSetMem(hnd->obj->addr + offset, length, dos_flags);
    if (arc)
    {
      errno = __libc_native2errno(arc);
      break;
    }

    /* Look for an existing mapping */
    struct ShmemView *prev_view;
    struct ShmemView *view = find_view(proc->views, offset, &prev_view);

    if (view && view->offset == offset)
    {
      /* The same offset, increase the reference counter. Note that we merge
       * ranges of different lengths but starting at the same offset because
       * `shmem_unmap` doesn't take a length argument and does not support
       * partial unmapping anyway. */
      ++view->refs;
      ASSERT(view->refs);

      /* Merge the ranges by updating the length if the new one is bigger */
      if (view->length < length)
        view->length = length;
    }
    else
    {
      /* Allocate a new view */
      struct ShmemView *new_view;
      GLOBAL_NEW(new_view);
      if (!new_view)
      {
        errno = ENOMEM;
        break;
      }

      new_view->offset = offset;
      new_view->length = length;
      new_view->refs = 1;

      /* Insert the view before the one we found */
      if (prev_view)
      {
        new_view->next = view;
        prev_view->next = new_view;
      }
      else
      {
        new_view->next = proc->views;
        proc->views = new_view;
      }

      ++hnd->obj->refs; /* View reference */
      ASSERT(hnd->obj->refs);
    }

    /* Success */
    addr = hnd->obj->addr + offset;
  }
  while (0);

  global_unlock();

  TRACE("addr %p errno %d\n", addr, errno);
  return addr;
}

int shmem_unmap(void *addr)
{
  TRACE("addr %p\n", addr);

  /* validate arguments */
  if (!addr)
  {
    errno = EINVAL;
    return -1;
  }

  int rc = -1;

  global_lock();

  do
  {
    struct ShmemHandle *hnd = NULL;
    SHMEM h;

    size_t offset = 0;

    struct ShmemProc *prev_proc;
    struct ShmemProc *proc;

    struct ShmemView *prev_view;
    struct ShmemView *view;

    /* Look for a first handle with a view matching the given address */
    for (int count = gpData->shmem->handles_count, i = 0; count; ++i)
    {
      struct ShmemHandle *try_hnd = &gpData->shmem->handles[i];
      if (try_hnd->obj)
      {
        /* A valid handle */
        --count;

        if (try_hnd->obj->addr <= addr && (offset = addr - try_hnd->obj->addr) < try_hnd->obj->size)
        {
          proc = find_proc(try_hnd->procs, getpid(), &prev_proc);
          if (proc && proc->views)
          {
            view = find_view(proc->views, offset, &prev_view);
            if (view)
            {
              hnd = try_hnd;
              h = i;
              break;
            }
          }
        }
      }
    }

    if (!hnd)
    {
      /* No handle with a mapping of addr in this process */
      errno = EINVAL;
      break;
    }

    TRACE("h %u obj addr %p size %u refs %u offset %u\n",
          h, hnd->obj->addr, hnd->obj->size, hnd->obj->refs, offset);

    /* Decrease the view's reference counter */
    ASSERT(view->refs);
    --view->refs;

    if (!view->refs)
    {
      /* The last reference has gone, remove this view from the list and free
       * it. Note that we don't uncommit view's pages as that's impossible for
       * shared memory on OS/2. */
      if (prev_view)
        prev_view->next = view->next;
      else
        proc->views = view->next;
      free(view);

      /* Release the view's reference from the memory object */
      unref_obj(hnd->obj);

      if (!proc->views && (proc->flags & SHMEM_FREE))
      {
        /* The last view has gone and the handle itself is freed, okay to get
         * rid of the proc entry (may release handle & mem) */
        free_proc(h, proc, prev_proc);
      }
    }

    /* Success */
    rc = 0;
  }
  while (0);

  global_unlock();

  TRACE("rc %d errno %d\n", rc, errno);
  return rc;
}

int shmem_get_info(SHMEM h, int *flags, size_t *size, size_t *act_size)
{
  TRACE("h %d flags %p size %p act_size %p\n", h, flags, size, act_size);

  /* validate arguments */
  if (h == SHMEM_INVALID)
  {
    errno = EINVAL;
    return -1;
  }

  int rc = -1;

  global_lock();

  do
  {
    struct ShmemHandle *hnd = get_handle(h);

    if (!hnd)
    {
      errno = EINVAL;
      break;
    }

    struct ShmemProc *proc = find_proc(hnd->procs, getpid(), NULL);

    if (!proc || (proc->flags & SHMEM_FREE))
    {
      /* The handle is not used in this process */
      errno = EINVAL;
      break;
    }

    if (size)
      *size = hnd->obj->size;

    if (act_size)
      *act_size = hnd->obj->act_size;

    if (flags)
    {
      *flags = hnd->flags;
      /* Also expose selected process-specific flags */
      *flags |= (proc->flags & SHMEM_READONLY);
    }

    /* Success */
    rc = 0;
  }
  while (0);

  global_unlock();

  TRACE("rc %d errno %d\n", rc, errno);
  return rc;
}

size_t shmem_max_handles()
{
  return SHMEM_MAX_HANDLES;
}
