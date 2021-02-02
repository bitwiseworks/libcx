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

typedef struct ShmemView
{
  struct ShmemView *next;
  size_t offset; /* Offset of the mapping */
  size_t length; /* Length of the mapping */
  int readonly; /* 1 if this view is mapped as read-only, or 0 */
  size_t refs; /* Reference counter for this mapping */
} ShmemView;

typedef struct ShmemProcHnd
{
  struct ShmemProcHnd *next;
  SHMEM h; /* Handle */
  int flags; /* SHMEM_ flags */
} ShmemProcHnd;

typedef struct ShmemProc
{
  struct ShmemProc *next;
  pid_t pid; /* pid of the process */
  ShmemView *views; /* List of mappings (views) */
  ShmemProcHnd *handles; /* List of handles (duplicates) */
  size_t rw_views; /* Number of read-write mappings */
} ShmemProc;

typedef struct ShmemObj
{
  struct ShmemObj *prev;
  struct ShmemObj *next;
  PVOID addr; /* Virtual address of the memory object */
  size_t size; /* Size of the memory object as was given in `shmem_create` */
  size_t act_size; /* Actual size of the memory object */
  ShmemProc *procs; /* Processes using this memory object */
} ShmemObj;

typedef struct ShmemHandle
{
  ShmemObj *obj; /* Memory object this handle represents */
  int flags; /* SHMEM_ flags */
  size_t refs; /* Reference counter for this handle (procs) */
} ShmemHandle;

/**
 * Global system-wide shmem API data structure.
 */
typedef struct ShmemData
{
  ShmemObj *objects; /* List of all memory objects */
  ShmemHandle *handles; /* Array of all available handles */
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
 * Removes a given proc entry from the list of procs of a given memory object
 * and frees the object in the current process. Also removes the object from
 * the list and frees the entry if it has no more procs using it. Returns the
 * next object entry or NULL if there is none.
 */
static ShmemObj *free_proc(ShmemObj *obj, ShmemProc *proc, ShmemProc *prev_proc)
{
  TRACE("freeing proc %p views %p handles %p and mem obj addr %p size %u\n",
        proc, proc->views, proc->handles, obj->addr, obj->size);

  ASSERT(!proc->handles && !proc->views);

  /* Remove this proc from the list and free it */
  if (prev_proc)
    prev_proc->next = proc->next;
  else
    obj->procs = proc->next;
  free(proc);

  /* Free the memory object in the current process */
  APIRET arc = DosFreeMem(obj->addr);
  ASSERT_MSG(!arc, "%u", arc);

  ShmemObj *next = obj->next;

  if (!obj->procs)
  {
    /* The last proc has gone, remove the object form the list */
    TRACE("freeing mem obj %p\n", obj);

    if (obj->prev)
      obj->prev->next = next;
    else
      gpData->shmem->objects = next;
    if (next)
      next->prev = obj->prev;
    free(obj);
  }

  return next;
}

/**
 * Looks for a proc entry with a given pid. Returns a found entry or NULL if
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

  TRACE("prev_proc %p proc %p views %p handles %p rw_views %u pid %d\n",
        prev_proc, proc, proc ? proc->views : 0, proc ? proc->handles : 0,
        proc ? proc->rw_views : 0, pid);
  /* For an existing proc, there must be either views or handles */
  ASSERT(!proc || proc->views || proc->handles);

  if (prev)
    *prev = prev_proc;

  return proc;
}

/**
 * Looks for a proc handle entry with a given value. Returns a found entry or
 * NULL if there is none. Also returns a previous handle entry if @a prev is not
 * NULL.
 */
static ShmemProcHnd *find_proc_handle(ShmemProcHnd *first, SHMEM h, ShmemProcHnd **prev)
{
  struct ShmemProcHnd *hnd = first;
  struct ShmemProcHnd *prev_hnd = NULL;
  while (hnd && hnd->h != h)
  {
    prev_hnd = hnd;
    hnd = hnd->next;
  }

  TRACE("prev_hnd %p hnd %p h %u flags 0x%X\n",
        prev_hnd, hnd, hnd ? hnd->h : SHMEM_INVALID, hnd ? hnd->flags : 0);

  if (prev)
    *prev = prev_hnd;

  return hnd;
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

  TRACE("prev_view %p view %p offset %u length %u readonly %d refs %u\n",
    prev_view, view, view ? view->offset : 0, view ? view->length : 0,
    view ? view->readonly : 0, view ? view->refs : 0);

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

  ASSERT(hnd->obj && hnd->refs);

  TRACE("flags 0x%X refs %u obj %p addr %p size %u act_size %u\n",
        hnd->flags, hnd->refs, hnd->obj, hnd->obj->addr, hnd->obj->size, hnd->obj->act_size);

  return hnd;
}

/**
 * Allocates a new handle and returns its array entry on success or NULL on
 * failure. If hnd is not NULL it is expected to be an address of a valid
 * pointer to a handles array entry that willb be adjusted in case if
 * allocation causes a reallocation that moves memory.
 */
static ShmemHandle *alloc_handle(SHMEM *h, ShmemHandle **old_hnd)
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

    if (old_hnd && *old_hnd && handles != gpData->shmem->handles)
    {
      /* Adjust the existing pointer */
      ASSERT(*old_hnd >= gpData->shmem->handles &&
             *old_hnd < gpData->shmem->handles + gpData->shmem->handles_size);
      TRACE("adjusting old_hnd from %p to %p\n", *old_hnd, handles + (*old_hnd - gpData->shmem->handles));
      *old_hnd = handles + (*old_hnd - gpData->shmem->handles);
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

  ASSERT_MSG(!hnd->obj && !hnd->refs, "%u %p %u", *h, hnd->obj, hnd->refs);

  return hnd;
}

/**
 * Releases a single process reference of a given handle. Will free the handle
 * if there are no more processes using it.
 */
static ShmemHandle *unref_handle(SHMEM h)
{
  ShmemHandle *hnd = &gpData->shmem->handles[h];
  ASSERT(hnd->obj);

  ASSERT(hnd->refs);
  --hnd->refs;

  if (!hnd->refs)
  {
    /* The last proc has gone, free the handle */
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
  TRACE("gpData->shmem->objects %p gpData->shmem->handles_count %u\n",
        gpData->shmem->objects, gpData->shmem->handles_count);

  ASSERT (!gpData->shmem->handles_count || gpData->shmem->objects);

  /* Free all handles and views belonging to this process */
  ShmemObj *obj = gpData->shmem->objects;
  while (obj)
  {
    ShmemProc *prev_proc;
    ShmemProc *proc = find_proc(obj->procs, getpid(), &prev_proc);

    if (proc)
    {
      /* Free all open handles */
      ShmemProcHnd *proc_hnd = proc->handles;
      while (proc_hnd)
      {
        TRACE("releasing handle %u\n", proc_hnd->h);
        ASSERT(get_handle(proc_hnd->h));

        unref_handle(proc_hnd->h);

        ShmemProcHnd *prev_proc_hnd = proc_hnd;
        proc_hnd = proc_hnd->next;
        free(prev_proc_hnd);
      }
      proc->handles = NULL;

      /* Free all mapped views */
      ShmemView *view = proc->views;
      while (view)
      {
        ShmemView *prev_view = view;
        view = view->next;
        free(prev_view);
      }
      proc->views = NULL;

      obj = free_proc(obj, proc, prev_proc);
    }
    else
    {
      obj = obj->next;
    }
  }

  if (gpData->refcnt == 0)
  {
    /* We are the last process, free shmem structures */
    ASSERT_MSG(!gpData->shmem->handles_count, "%u", gpData->shmem->handles_count);
    ASSERT_MSG(!gpData->shmem->objects, "%p\n", gpData->shmem->objects);

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
    ShmemObj *obj;
    if (!(GLOBAL_NEW(obj)))
    {
      errno = ENOMEM;
      break;
    }

    ShmemProc *proc;
    if (!(GLOBAL_NEW(proc)))
    {
      free(obj);
      errno = ENOMEM;
      break;
    }

    ShmemProcHnd *proc_hnd;
    if (!(GLOBAL_NEW(proc_hnd)))
    {
      free(proc);
      free(obj);
      errno = ENOMEM;
      break;
    }

    ShmemHandle *hnd = alloc_handle(&h, NULL);
    if (!hnd)
    {
      free(proc_hnd);
      free(proc);
      free(obj);
      errno = ENOMEM;
      break;
    }

    proc_hnd->h = h;
    proc_hnd->flags = flags;

    proc->pid = getpid();
    proc->handles = proc_hnd;

    obj->addr = addr;
    obj->size = size;
    obj->act_size = act_size;
    obj->procs = proc;

    /* Insert obj to the beginning of the list */
    if (gpData->shmem->objects)
      gpData->shmem->objects->prev = obj;
    obj->next = gpData->shmem->objects;
    gpData->shmem->objects = obj;

    hnd->obj = obj;
    hnd->flags = flags;
    hnd->refs = 1;
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
    ShmemHandle *hnd = get_handle(h);
    if (!hnd)
    {
      errno = EINVAL;
      break;
    }

    ShmemProcHnd *proc_hnd = NULL;
    ShmemProc *proc = find_proc(hnd->obj->procs, pid, NULL);

    if (proc && proc->handles)
    {
      proc_hnd = find_proc_handle(proc->handles, h, NULL);

      if (proc_hnd)
      {
        /* The handle is already used in the target process (and not freed) */
        errno = EPERM;
        break;
      }
    }

    if (!(GLOBAL_NEW(proc_hnd)))
    {
      errno = ENOMEM;
      break;
    }

    if (!proc)
    {
      if (!(GLOBAL_NEW(proc)))
      {
        free(proc_hnd);
        errno = ENOMEM;
        break;
      }

      /* Give access to the memory object (obey the handle restriction) */
      ULONG dos_flags = PAG_READ | PAG_EXECUTE;
      if (!(flags & SHMEM_READONLY) && !(hnd->flags & SHMEM_READONLY))
        dos_flags |= PAG_WRITE;
      APIRET arc = DosGiveSharedMem(hnd->obj->addr, pid, dos_flags);

      TRACE("arc %lu dos_flags 0x%X\n", arc, dos_flags);

      if (arc)
      {
        /* Free the new, unused proc_handle and proc entry */
        free(proc_hnd);
        free(proc);
        errno = __libc_native2errno(arc);
        break;
      }

      proc->pid = pid;

      /* Insert proc to the beginning of the list */
      proc->next = hnd->obj->procs;
      hnd->obj->procs = proc;
    }

    proc_hnd->h = h;
    proc_hnd->flags = flags;

    /* Insert proc_hnd to the beginning of the list */
    proc_hnd->next = proc->handles;
    proc->handles = proc_hnd;

    /* Add proc reference to the handle */
    ++hnd->refs;
    ASSERT(hnd->refs);

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
    ShmemHandle *hnd = get_handle(h);
    if (!hnd)
    {
      errno = EINVAL;
      break;
    }

    ShmemProc *proc = find_proc(hnd->obj->procs, getpid(), NULL);

    ShmemProcHnd *proc_hnd = NULL;
    if (proc && proc->handles)
    {
      proc_hnd = find_proc_handle(proc->handles, h, NULL);

      if (proc_hnd)
      {
        /* The handle is already used in this process (and not freed) */
        errno = EPERM;
        break;
      }
    }

    if (!(GLOBAL_NEW(proc_hnd)))
    {
      errno = ENOMEM;
      break;
    }

    if (!proc)
    {
      if (!(GLOBAL_NEW(proc)))
      {
        free(proc_hnd);
        errno = ENOMEM;
        break;
      }

      /* Get access to the memory object (obey the handle restriction) */
      ULONG dos_flags = PAG_READ | PAG_EXECUTE;
      if (!(flags & SHMEM_READONLY) && !(hnd->flags & SHMEM_READONLY))
        dos_flags |= PAG_WRITE;
      APIRET arc = DosGetSharedMem(hnd->obj->addr, dos_flags);

      TRACE("arc %lu dos_flags 0x%X\n", arc, dos_flags);

      if (arc)
      {
        /* Free the new, unused proc_handle and proc entry */
        free(proc_hnd);
        free(proc);
        errno = __libc_native2errno(arc);
        break;
      }

      proc->pid = getpid();

      /* Insert proc to the beginning of the list */
      proc->next = hnd->obj->procs;
      hnd->obj->procs = proc;
    }

    proc_hnd->h = h;
    proc_hnd->flags = flags;

    /* Insert proc_hnd to the beginning of the list */
    proc_hnd->next = proc->handles;
    proc->handles = proc_hnd;

    /* Add proc reference to the handle */
    ++hnd->refs;
    ASSERT(hnd->refs);

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
    ShmemHandle *hnd = get_handle(h);
    if (!hnd)
    {
      errno = EINVAL;
      break;
    }

    ShmemProc *proc = find_proc(hnd->obj->procs, getpid(), NULL);

    ShmemProcHnd *proc_hnd = NULL;
    if (proc && proc->handles)
      proc_hnd = find_proc_handle(proc->handles, h, NULL);

    if (!proc || !proc_hnd)
    {
      /* The handle is not used in this process */
      errno = EINVAL;
      break;
    }

    ShmemProcHnd *dup_proc_hnd;
    if (!(GLOBAL_NEW(dup_proc_hnd)))
    {
      errno = ENOMEM;
      break;
    }

    ShmemHandle *dup_hnd = alloc_handle(&dup_h, &hnd);
    if (!dup_hnd)
    {
      free(dup_proc_hnd);
      errno = ENOMEM;
      break;
    }

    dup_proc_hnd->h = dup_h;
    /* Ihnerit selected source handle's process-specific flags */
    dup_proc_hnd->flags = flags | (proc_hnd->flags & SHMEM_READONLY);

    dup_hnd->obj = hnd->obj;
    /* Ihnerit selected source handle's flags */
    dup_hnd->flags = flags | (hnd->flags & SHMEM_READONLY);
    dup_hnd->refs = 1;

    /* Insert dup_proc_hnd to the beginning of the list */
    dup_proc_hnd->next = proc->handles;
    proc->handles = dup_proc_hnd;
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
    ShmemHandle *hnd = get_handle(h);
    if (!hnd)
    {
      errno = EINVAL;
      break;
    }

    ShmemProc *prev_proc;
    ShmemProc *proc = find_proc(hnd->obj->procs, getpid(), &prev_proc);

    ShmemProcHnd *prev_proc_hnd;
    ShmemProcHnd *proc_hnd = NULL;
    if (proc && proc->handles)
      proc_hnd = find_proc_handle(proc->handles, h, &prev_proc_hnd);

    if (!proc || !proc_hnd)
    {
      /* The handle is not used in this process */
      errno = EINVAL;
      break;
    }

    /* Remove the handle from the list and free it */
    if (prev_proc_hnd)
      prev_proc_hnd->next = proc_hnd->next;
    else
      proc->handles = proc_hnd->next;
    free(proc_hnd);

    /* Get rid of the proc entry if no handles and views (will also free mem) */
    if (!proc->handles && !proc->views)
      free_proc(hnd->obj, proc, prev_proc);

    unref_handle(h);

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
    ShmemHandle *hnd = get_handle(h);
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

    ShmemProc *proc = find_proc(hnd->obj->procs, getpid(), NULL);

    ShmemProcHnd *proc_hnd = NULL;
    if (proc && proc->handles)
      proc_hnd = find_proc_handle(proc->handles, h, NULL);

    if (!proc || !proc_hnd)
    {
      /* The handle is not used in this process */
      errno = EINVAL;
      break;
    }

    int readonly = (proc_hnd->flags & SHMEM_READONLY) || (hnd->flags & SHMEM_READONLY);
    TRACE("readonly %d\n", readonly);

    /* Commit memory (may fail due to out-of-memory or such). Note that we obey
     * the restriction of the specified handle only if there are no unrestricted
     * read-write views (as this would break their access). */
    ULONG dos_flags = PAG_COMMIT | PAG_READ | PAG_EXECUTE;
    if (!readonly || proc->rw_views)
      dos_flags |= PAG_WRITE;
    APIRET arc = MyDosSetMem(hnd->obj->addr + offset, length, dos_flags);
    if (arc)
    {
      errno = __libc_native2errno(arc);
      break;
    }

    /* Look for an existing mapping */
    ShmemView *prev_view;
    ShmemView *view = find_view(proc->views, offset, &prev_view);

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

      /* Upgrade the read-only view to read-write to satisfy the request */
      if (view->readonly && !readonly)
      {
        view->readonly = 0;
        ++proc->rw_views;
        ASSERT(proc->rw_views);
      }
    }
    else
    {
      /* Allocate a new view */
      ShmemView *new_view;
      GLOBAL_NEW(new_view);
      if (!new_view)
      {
        errno = ENOMEM;
        break;
      }

      new_view->offset = offset;
      new_view->length = length;
      new_view->refs = 1;

      /* Memorize the read-only status and correct rw_views accordingly */
      new_view->readonly = readonly;
      if (!readonly)
      {
        ++proc->rw_views;
        ASSERT(proc->rw_views);
      }

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
    size_t offset = 0;

    ShmemProc *prev_proc;
    ShmemProc *proc;

    ShmemView *prev_view;
    ShmemView *view;

    /* Look for a view matching the given address */
    ShmemObj *obj = gpData->shmem->objects;
    while (obj)
    {
      if (obj->addr <= addr && (offset = addr - obj->addr) < obj->size)
      {
        proc = find_proc(obj->procs, getpid(), &prev_proc);
        if (proc && proc->views)
        {
          view = find_view(proc->views, offset, &prev_view);
          if (view)
            break;
        }
      }

      obj = obj->next;
    }

    if (!view)
    {
      /* No object with a mapping of addr in this process */
      errno = EINVAL;
      break;
    }

    TRACE("obj addr %p size %u offset %u\n", obj->addr, obj->size, offset);

    /* Decrease the view's reference counter */
    ASSERT(view->refs);
    --view->refs;

    if (!view->refs)
    {
      /* The last reference has gone, remove this view from the list and free
       * it. Note that we don't uncommit view's pages as that's impossible for
       * shared memory on OS/2. */
      if (!view->readonly)
      {
        /* This is a read-write view, decrease rw_views */
        ASSERT(proc->rw_views);
        --proc->rw_views;
      }

      if (prev_view)
        prev_view->next = view->next;
      else
        proc->views = view->next;
      free(view);

      if (proc->views && proc->rw_views == 0)
      {
        /* There're views but none of them is read-write, switch all to read-only */
        view = proc->views;
        while (view)
        {
          TRACE("switching to r/o addr %p length %u\n", obj->addr + view->offset, view->length);
          APIRET arc = MyDosSetMem(obj->addr + view->offset, view->length,
                                   PAG_COMMIT | PAG_READ | PAG_EXECUTE);
          ASSERT_MSG(!arc, "%lu", arc);

          view = view->next;
        }
      }

      /* Get rid of the proc entry if no handles and views (will also free mem) */
      if (!proc->views && !proc->handles)
        free_proc(obj, proc, prev_proc);
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
    ShmemHandle *hnd = get_handle(h);
    if (!hnd)
    {
      errno = EINVAL;
      break;
    }

    ShmemProc *proc = find_proc(hnd->obj->procs, getpid(), NULL);

    ShmemProcHnd *proc_hnd = NULL;
    if (proc && proc->handles)
      proc_hnd = find_proc_handle(proc->handles, h, NULL);

    if (!proc || !proc_hnd)
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
      *flags |= (proc_hnd->flags & SHMEM_READONLY);
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
