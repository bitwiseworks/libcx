/*
 * fcntl advisory locking replacement for kLIBC.
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

/*
 * We use different parameter list for fcntl (see below)
 * so rename the origina libc version.
 */
#define fcntl fcntl_libc

#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include <assert.h>
#include <emx/io.h>
#include <umalloc.h>

#undef fcntl

#define TRACE_ENABLED 0
#if TRACE_ENABLED
#define TRACE_MORE 1
#define TRACE(msg, ...) do { printf("*** [%d:%d] %s:%d:%s: " msg, getpid(), _gettid(), __FILE__, __LINE__, __FUNCTION__, ## __VA_ARGS__); fflush(stdout); } while (0)
#define TRACE_IF(cond, msg, ...) do { if (cond) { printf("*** [%d:%d] %s:%d:%s: " msg, getpid(), _gettid(), __FILE__, __LINE__, __FUNCTION__, ## __VA_ARGS__); fflush(stdout); } } while (0)
#else
#define TRACE_MORE 0
#define TRACE(msg, ...) do {} while (0)
#define TRACE_IF(cond, msg, ...) do {} while (0)
#endif

#define MUTEX_FCNTL "\\SEM32\\FCNTL_LOCKING_V1_MUTEX"
#define EVENTSEM_FCNTL "\\SEM32\\FCNTL_LOCKING_V1_EVENTSEM"
#define SHAREDMEM_FCNTL "\\SHAREMEM\\FCNTL_LOCKING_V1_DATA"

#define HEAP_SIZE 65536 /* initial size for fcntl data */

static HMTX gMutex = NULLHANDLE;
static HEV gEvSem = NULLHANDLE;

struct pid_list
{
  size_t size;
  size_t used;
  pid_t list[0]; /* PID or 0 for empty cell */
};

/**
 * File lock descriptor (linked list entry).
 */
struct file_lock
{
  struct file_lock *next;

  char type; /* R = read lock, r = multiple read locks, W = write lock, \0 = no lock */
  off_t start; /* start of the lock region */
  union
  {
    pid_t pid; /* owner of the R or W lock */
    struct pid_list *pids; /* owners of the r locks */
  };
};

/**
 * File descriptor (hash map entry).
 */
struct file_desc
{
  char path[PATH_MAX];
  struct file_desc *next;

  struct file_lock *locks;
};

#define FILE_LOCKS_HASH_SIZE 127

struct shared_data
{
  Heap_t heap;
  int refcnt; /* number of processes using us */
  struct file_desc **files; /* File descriptor hash map */
  size_t blocked; /* number of threads blocked in F_SETLKW */
};

static struct shared_data *gpData = NULL;

static struct pid_list *copy_pids(const struct pid_list *list)
{
  assert(list);
  size_t size = sizeof(*list) + sizeof(list->list[0]) * list->size;
  struct pid_list *nlist = _ucalloc(gpData->heap, 1, size);
  if (nlist)
    memcpy(nlist, list, size);
  return nlist;
}

static int lock_has_pid(struct file_lock *l, pid_t pid)
{
  assert(l->type != 0 && pid != 0);
  if (l->type == 'r')
  {
    int i;
    assert(l->pids && l->pids->used);
    for (i = 0; i < l->pids->size; ++i)
      if (l->pids->list[i] == pid)
        return 1;
    return 0;
  }
  return l->pid == pid;
}

/**
 * Checks if the given lock region l needs modification to convert it to
 * the given lock type for pid. Deesn't check for possible blocking and such.
 * Returns 1 if the modification is needed and 0 otherwise.
 */
static int lock_needs_mark(struct file_lock *l, short type, pid_t pid)
{
  return (type == F_UNLCK && l->type != 0 && lock_has_pid(l, pid)) ||
         (type == F_WRLCK && l->type != 'W') ||
         (type == F_RDLCK && ((l->type != 'r' && l->type != 'R') || !lock_has_pid(l, pid)));
}

/**
 * Marks the given lock region l with type and pid. Doesn't check for
 * possible blocking and such.
 * Returns 0 on success or -1 if there is no memory.
 */
static int lock_mark(struct file_lock *l, short type, pid_t pid)
{
  switch (type)
  {
    case F_UNLCK:
      assert(l->type != 0);
      if (l->type == 'r')
      {
        int i;
        assert(l->pids && l->pids->used);
        for (i = 0; i < l->pids->size; ++i)
        {
          if (l->pids->list[i] == pid)
          {
            l->pids->list[i] = 0;
            if (--l->pids->used == 0)
            {
              free(l->pids);
              l->type = 0;
              l->pid = 0;
            }
          }
        }
        assert(i < l->pids->size);
      }
      else
      {
        l->type = 0;
        l->pid = 0;
      }
      break;
    case F_WRLCK:
      assert(l->type != 'W' && l->type != 'r');
      assert((l->type == 0 && l->pid == 0) || l->pid == pid);
      l->type = 'W';
      l->pid = pid;
      break;
    case F_RDLCK:
      if (l->type == 'r')
      {
        int i;
        assert(l->pids && l->pids->used);
        if (l->pids->used < l->pids->size)
        {
          for (i = 0; i < l->pids->size; ++i)
          {
            if (l->pids->list[i] == 0)
            {
              l->pids->list[i] = pid;
              ++l->pids->used;
            }
          }
          assert(i < l->pids->size);
        }
        else
        {
          /* Need more space */
          assert(l->pids->used == l->pids->size);
          size_t nsize = l->pids->size += 8;
          struct pid_list *nlist =
              realloc(l->pids, sizeof(*l->pids) + sizeof(l->pids->list[0]) * nsize);
          if (!nlist)
            return -1;
          l->pids->list[l->pids->size] = pid;
          l->pids->size = nsize;
          ++l->pids->used;
        }
      }
      else if (l->type == 'R')
      {
        assert(l->pid && l->pid != pid);
        size_t nsize = 8;
        struct pid_list *nlist =
            _ucalloc(gpData->heap, 1, sizeof(*l->pids) + sizeof(l->pids->list[0]) * nsize);
        if (!nlist)
          return -1;
        nlist->size = nsize;
        nlist->used = 2;
        nlist->list[0] = l->pid;
        nlist->list[1] = pid;
        l->type == 'r';
        l->pids = nlist;
      }
      else
      {
        assert((l->type == 0 && l->pid == 0) || l->pid == pid);
        l->type = 'R';
        l->pid = pid;
      }
      break;
    default:
      assert(0);
  }

  return 0;
}

inline static off_t lock_end(struct file_lock *l)
{
    return l->next ? l->next->start - 1 : OFF_MAX;
}

static void lock_free(struct file_lock *l)
{

  if (l->type == 'r')
    free(l->pids);
  free(l);
}

/**
 * Splits the given lock region at the given split point.
 * Returns the newly created region or NULL if there is
 * not enough memory.
 */
static struct file_lock *lock_split(struct file_lock *l, off_t split)
{
  struct file_lock *ln = NULL;

  assert(l);
  assert(l->start < split && split <= lock_end(l));

  ln = _ucalloc(gpData->heap, 1, sizeof(*ln));
  if (!ln)
    return NULL;

  ln->start = split;
  ln->type = l->type;
  if (l->type == 'r')
  {
    ln->pids = copy_pids(l->pids);
    if (!ln->pids)
    {
      free(ln);
      return NULL;
    }
  }
  else
    ln->pid = l->pid;
  ln->next = l->next;
  l->next = ln;

  return ln;
}

static size_t hash_string(const char *str)
{
  /*
   * Based on RS hash function from Arash Partow collection
   * (http://www.partow.net/programming/hashfunctions/).
   * According to https://habrahabr.ru/post/219139/ this function
   * produces few collisions and is rather fast.
   */

  size_t a = 63689;
  size_t hash = 0;

  while (*str)
  {
    hash = hash * a + (unsigned char)(*str++);
    a *= 378551 /* b */;
  }

  return hash;
}

/**
 * Returns a file descriptor sturcture for the given path.
 * Returns NULL when @a bNew is true and there is not enough memory
 * to allocate a new sctructure, or when @a bNew is FALSE and there
 * is no descriptor for the given file.
 */
static struct file_desc *get_desc(const char *path, int bNew)
{
  size_t h;
  struct file_desc *desc;

  assert(gpData);
  assert(path);
  assert(strlen(path) < PATH_MAX);

  h = hash_string(path) % FILE_LOCKS_HASH_SIZE;
  desc = gpData->files[h];

  while (desc)
  {
    if (strcmp(desc->path, path) == 0)
      break;
    desc = desc->next;
  }

  if (!desc && bNew)
  {
    desc = _ucalloc(gpData->heap, 1, sizeof(*desc));
    if (desc)
    {
      /* Initialize the new desc */
      strncpy(desc->path, path, PATH_MAX - 1);
      /* Add one free region that covers the entire file */
      desc->locks = _ucalloc(gpData->heap, 1, sizeof(*desc->locks));
      if (!desc->locks)
      {
        free(desc);
        return NULL;
      }
      /* Put to the head of the bucket */
      desc->next = gpData->files[h];
      gpData->files[h] = desc;
    }
  }

  return desc;
}

static void APIENTRY ProcessExit(ULONG);

/**
 * Initializes the shared structures.
 * @param bLock TRUE to leave the mutex locked on success.
 * @return NO_ERROR on success, DOS error code otherwise.
 */
static int fcntl_locking_init(int bLock)
{
  APIRET arc;
  int rc;

  arc = DosExitList(EXLST_ADD, ProcessExit);
  assert(arc == NO_ERROR);

  while (1)
  {
    arc = DosOpenMutexSem(MUTEX_FCNTL, &gMutex);
    TRACE("DosOpenMutexSem = %d\n", arc);
    if (arc == NO_ERROR)
    {
      /*
       * Init is (being) done by another process, request the mutex to
       * guarantee shared memory is already created, then get access to
       * it and open shared heap located in that memory.
       */
      arc = DosRequestMutexSem(gMutex, SEM_INDEFINITE_WAIT);
      TRACE("DosRequestMutexSem = %d\n", arc);
      assert(arc == NO_ERROR);

      if (arc == NO_ERROR)
      {
        arc = DosOpenEventSem(EVENTSEM_FCNTL, &gEvSem);
        TRACE("DosOpenEventSem = %d\n", arc);
        assert(arc == NO_ERROR);

        arc = DosGetNamedSharedMem((PPVOID)&gpData, SHAREDMEM_FCNTL, PAG_READ | PAG_WRITE);
        assert(arc == NO_ERROR);

        TRACE("gpData->heap = %p\n", gpData->heap);
        assert(gpData->heap);

        rc = _uopen(gpData->heap);
        TRACE("_uopen = %d (%d)\n", rc, errno);
        assert(rc == 0);

        assert(gpData->refcnt);
        gpData->refcnt++;

        if (!bLock)
          DosReleaseMutexSem(gMutex);
      }

      return arc;
    }
    if (arc == ERROR_SEM_NOT_FOUND)
    {
      /* We are the first process, create the mutex */
      arc = DosCreateMutexSem(MUTEX_FCNTL, &gMutex, 0, TRUE);
      TRACE("DosCreateMutexSem = %d\n", arc);

      if (arc == ERROR_DUPLICATE_NAME)
      {
        /* Another process is faster, attempt to open the mutex again */
        continue;
      }
    }
    break;
  }

  if (arc != NO_ERROR)
    return arc;

  /* Create event semaphore */
  arc = DosCreateEventSem(EVENTSEM_FCNTL, &gEvSem, DCE_AUTORESET, FALSE);
  TRACE("DosCreateEventSem = %d\n", arc);

  if (arc == NO_ERROR)
  {
    /* Allocate shared memory */
    arc = DosAllocSharedMem((PPVOID)&gpData, SHAREDMEM_FCNTL, HEAP_SIZE,
                            PAG_COMMIT | PAG_READ | PAG_WRITE | OBJ_ANY);
    TRACE("DosAllocSharedMem(OBJ_ANY) = %d\n", arc);

    if (arc && arc != ERROR_ALREADY_EXISTS)
    {
      /* High memory may be unavailable, try w/o OBJ_ANY */
      arc = DosAllocSharedMem((PPVOID)&gpData, SHAREDMEM_FCNTL, HEAP_SIZE,
                              PAG_COMMIT | PAG_READ | PAG_WRITE);
      TRACE("DosAllocSharedMem = %d\n", arc);
    }

    if (arc == NO_ERROR)
    {
      /* Create shared heap */
      gpData->heap = _ucreate(gpData + 1, HEAP_SIZE - sizeof(struct shared_data),
                              _BLOCK_CLEAN, _HEAP_REGULAR | _HEAP_SHARED,
                              NULL, NULL);
      TRACE("gpData->heap = %p\n", gpData->heap);
      assert(gpData->heap);

      rc =_uopen(gpData->heap);
      assert(rc == 0);

      gpData->refcnt = 1;

      /* Initialize structures */
      gpData->files = _ucalloc(gpData->heap, FILE_LOCKS_HASH_SIZE, sizeof(*gpData->files));
      TRACE("gpData->files = %p\n", gpData->files);
      assert(gpData->files);
    }
  }

  if (!bLock || arc != NO_ERROR)
    DosReleaseMutexSem(gMutex);

  return arc;
}

static void fcntl_locking_term()
{
  APIRET arc;
  int rc;

  TRACE("gMutex %p, gpData %p (heap %p, refcnt %d)\n",
        gMutex, gpData, gpData ? gpData->heap : 0,
        gpData ? gpData->refcnt : 0);

  assert(gMutex != NULLHANDLE);

  DosRequestMutexSem(gMutex, SEM_INDEFINITE_WAIT);

  if (gpData)
  {
    if (gpData->heap)
    {
      int i;

      assert(gpData->refcnt);
      gpData->refcnt--;

      if (gpData->files)
      {
        /* Go through all locks to unlock any regions this process owns */
        int unblocked = 0;

        for (i = 0; i < FILE_LOCKS_HASH_SIZE; ++i)
        {
          pid_t pid = getpid();
          struct file_desc *desc = gpData->files[i];
          while (desc)
          {
            struct file_lock *l = desc->locks;
            while (l)
            {
              if (lock_needs_mark(l, F_UNLCK, pid))
              {
                TRACE("Will unlock [%s], type '%c', start %lld, len %lld\n",
                      desc->path, l->type, l->start, lock_end(l) - l->start + 1);
                rc = lock_mark(l, F_UNLCK, pid);
                TRACE("rc = %d\n", rc);
                unblocked = 1;
              }
              l = l->next;
            }

            desc = desc->next;
          }
        }

        if (unblocked && gpData->blocked)
        {
          /* We unblocked something and there are blocked threads, release them */
          arc = DosPostEventSem(gEvSem);
          TRACE("DosPostEventSem = %d\n", arc);
        }
      }

      if (gpData->refcnt == 0)
      {
        /* We are the last process, free structures */
        if (gpData->files)
        {
          for (i = 0; i < FILE_LOCKS_HASH_SIZE; ++i)
          {
            struct file_desc *desc = gpData->files[i];
            while (desc)
            {
              struct file_desc *next = desc->next;
              struct file_lock *l = desc->locks;
              while (l)
              {
                struct file_lock *n = l->next;
                free (l);
                l = n;
              }
              free(desc);
              desc = next;
            }
          }
          free(gpData->files);
        }
      }

      _uclose(gpData->heap);

      if (gpData->refcnt == 0)
      {
        rc = _udestroy(gpData->heap, !_FORCE);
        TRACE("_udestroy = %d (%d)\n", rc, errno);
      }
    }

    arc = DosFreeMem(gpData);
    TRACE("DosFreeMem = %d\n", arc);
  }

  arc = DosCloseEventSem(gEvSem);
  TRACE("DosCloseEventSem = %d\n", arc);

  DosReleaseMutexSem(gMutex);

  arc = DosCloseMutexSem(gMutex);
  TRACE("DosCloseMutexSem = %d\n", arc);

  DosExitList(EXLST_REMOVE, ProcessExit);
}

static void mutex_lock()
{
  APIRET arc;

  assert(gMutex != NULLHANDLE);
  assert(gpData);

  arc = DosRequestMutexSem(gMutex, SEM_INDEFINITE_WAIT);
  if (arc == ERROR_INVALID_HANDLE)
  {
    /*
     * It must be a forked child, initialize ourselves (since
     * _DLL_InitTerm(,0) is not called for forks). This call
     * will leave the mutex locked.
     */
    arc = fcntl_locking_init(TRUE);
  }

  assert(arc == NO_ERROR);
}

static void mutex_unlock()
{
  APIRET arc;

  assert(gMutex != NULLHANDLE);

  arc = DosReleaseMutexSem(gMutex);

  assert(arc == NO_ERROR);
}

static int fcntl_locking(int fildes, int cmd, struct flock *fl)
{
  APIRET arc;
  int rc, bWouldBlock, bSeenR;
  off_t start, end;
  struct file_desc *desc;
  struct file_lock *l, *lb, *le;
  struct stat st;
  __LIBC_PFH pFH;

  pid_t pid = getpid();

  TRACE("fd %d, cmd %d=%s, type %d=%s, whence %d=%s, start %lld, len %lld, pid %d\n",
        fildes, cmd, cmd == F_GETLK ? "F_GETLK" :
                     cmd == F_SETLK ? "F_SETLK" :
                     cmd == F_SETLKW ? "F_SETLKW" : "?",
        fl->l_type, fl->l_type == F_RDLCK ? "F_RDLCK" :
                    fl->l_type == F_UNLCK ? "F_UNLCK" :
                    fl->l_type == F_WRLCK ? "F_WRLCK" : "?",
        fl->l_whence, fl->l_whence == SEEK_SET ? "SEEK_SET" :
                      fl->l_whence == SEEK_CUR ? "SEEK_CUR" :
                      fl->l_whence == SEEK_END ? "SEEK_END" : "?",
        (uint64_t)fl->l_start, (uint64_t)fl->l_len, fl->l_pid);

  /*
   * Get the native path from LIBC (it should always be a fully resolved
   * absolute canonicalized path of PATH_MAX-1 length that we use as a
   * key in the locks hash map).
   */
  pFH = __libc_FH(fildes);
  if (!pFH || !pFH->pszNativePath)
  {
    errno = !pFH ? EBADF : EINVAL;
    return -1;
  }

  TRACE("pszNativePath %s, fFlags %x\n", pFH->pszNativePath, pFH->fFlags);

  /* Check for proper file access */
  if ((fl->l_type == F_WRLCK && (pFH->fFlags & O_ACCMODE) == O_RDONLY) ||
      (fl->l_type == F_RDLCK && (pFH->fFlags & O_ACCMODE) == O_WRONLY))
  {
    errno = EBADF;
    return -1;
  }

  /* Normalize & check start and length */
  switch (fl->l_whence)
  {
    case SEEK_SET:
      start = 0;
      break;
    case SEEK_CUR:
      start = tell(fildes);
      if (start == -1)
        return -1;
      break;
    case SEEK_END:
      if (fstat(fildes, &st) == -1)
        return -1;
      start = st.st_size;
      break;
    default:
      TRACE("Bad whence\n");
      errno = EINVAL;
      return -1;
  }
  if (fl->l_len >= 0)
  {
    start += fl->l_start;
    if (fl->l_len)
      end = start + fl->l_len - 1;
    else
      end = OFF_MAX;
  }
  else
  {
    start += fl->l_start + fl->l_len;
    end = start - 1;
  }
  if (start < 0 || end < 0)
  {
    TRACE("start %lld (0x%llx), end %lld (0x%llx)\n", start, start, end, end);
    errno = EINVAL;
    return -1;
  }
  if (start > OFF_MAX || end > OFF_MAX)
  {
    TRACE("start %lld (0x%llx), end %lld (0x%llx) OFF_MAX %lld (0x%llx)\n",
          start, start, end, end, OFF_MAX, OFF_MAX);
    errno = EOVERFLOW;
    return -1;
  }

  rc = 0; /* be optimistic :) */

  mutex_lock();

  while (1)
  {
    desc = get_desc(pFH->pszNativePath, cmd != F_GETLK);
    if (!desc)
    {
      if (cmd == F_GETLK)
      {
        /* No any locks on this file */
        fl->l_type = F_UNLCK;
        break;
      }
      else
      {
        TRACE("No memory\n");
        errno = ENOLCK;
        rc = -1;
        break;
      }
    }

#if TRACE_MORE
    TRACE("Locks before:\n");
    for (l = desc->locks; l; l = l->next)
    {
      TRACE("- type '%c', start %lld, pid %d\n", l->type, l->start, l->pid);
    }
#endif

    /* Search for the first overlapping region */
    assert(desc->locks);
    assert(desc->locks->start == 0);
    lb = desc->locks;
    while (lb->next && lb->next->start <= start)
      lb = lb->next;
    /*
     * Search for the last overlapping region and also see if there are
     * any blolcking and/or 'r' regions.
     */
    bSeenR = 0;
    bWouldBlock = 0;
    le = lb;
    while (1)
    {
      if (le->type == 'r')
      {
        bSeenR++;
        if (!bWouldBlock && fl->l_type == F_WRLCK)
        {
          /* 'r' implies other PIDs hold a read lock, so write is blocked */
          bWouldBlock = 1;
          l = le; /* remember the blocking region */
        }
      }
      if (!bWouldBlock)
      {
        bWouldBlock = (le->type == 'W' ||
                       (le->type == 'R' && fl->l_type == F_WRLCK)) &&
                      le->pid != pid;
        l = le; /* remember the blocking region */
      }
      if (le->next && le->next->start <= end)
        le = le->next;
      else
        break;
    }

    TRACE_IF(bWouldBlock, "Would block! type '%c' start %lld, len %lld, pid %d\n",
             l->type, (uint64_t)l->start, (uint64_t)lock_end(l) - l->start + 1, l->pid);

    if (cmd == F_GETLK)
    {
      if (bWouldBlock)
      {
        /* Copy over the blocking lock data */
        assert(l);
        fl->l_type = l->type == 'W' ? F_WRLCK : F_RDLCK;
        fl->l_whence = SEEK_SET;
        fl->l_start = l->start;
        fl->l_len = l->next ? l->next->start - l->start : 0;
        fl->l_pid = l->type == 'r' ? l->pids->list[0] : l->pid;
      }
      else
      {
        /* No blocking locks on this file */
        fl->l_type = F_UNLCK;
      }
    }
    else
    {
      if (bWouldBlock)
      {
        if (cmd == F_SETLK)
        {
          errno = EACCES;
          rc = -1;
        }
        else
        {
          /* Block this thread due to F_SETLKW */
          ++gpData->blocked;
          TRACE("Will wait (total blocked %d)\n", gpData->blocked);

          mutex_unlock();

          arc = DosWaitEventSem(gEvSem, SEM_INDEFINITE_WAIT);
          TRACE("DosWaitEventSem = %d\n", arc);

          if (arc == ERROR_INTERRUPT)
          {
            /* We've been interrupted, bail out now */
            errno = EINTR;
            TRACE("rc=-1 errno=%d\n", errno);
            return -1;
          }

          assert(arc == NO_ERROR);

          mutex_lock();

          assert(gpData->blocked);
          --gpData->blocked;

          /* Recheck for blocking regions */
          continue;
        }
      }
      else
      {
        /*
         * We are good to set/clear a new lock as requested.
         */
        do
        {
          /* Process the first region */
          if (lock_needs_mark(lb, fl->l_type, pid))
          {
            if (lb->start == start && lock_end(lb) == end)
            {
              /* Simplest case (all done in one step) */
              rc = lock_mark(lb, fl->l_type, pid);
              break;
            }
            else
            {
              /* Split the non-aligned region (head) */
              struct file_lock *ln = NULL;
              if (lb->start < start)
              {
                ln = lock_split(lb, start);
                if (!ln)
                {
                  rc = -1;
                  break;
                }
              }
              if (lb == le)
              {
                if (!ln)
                  ln = lb;
                if (lock_end(le) > end)
                {
                  /* Split the non-aligned region (tail) */
                  if (!lock_split(ln, end + 1))
                  {
                    rc = -1;
                    break;
                  }
                }
                /* All done */
                rc = lock_mark(ln, fl->l_type, pid);
                break;
              }
              /* Update lb, it is used later */
              if (ln)
                lb = ln;
              rc = lock_mark(lb, fl->l_type, pid);
              if (rc == -1)
                break;
            }
          }

          if (lb == le)
            break;

          /* Process the last region */
          if (lock_needs_mark(le, fl->l_type, pid))
          {
            if (bSeenR)
            {
              if (lock_end(le) > end)
              {
                /* Split the non-aligned last region */
                if (!lock_split(le, end + 1))
                {
                  rc = -1;
                  break;
                }
              }
              rc = lock_mark(le, fl->l_type, pid);
              if (rc == -1)
                break;
            }
            /* Note: !bSeenR case is processed later */
          }

          /* Process the remaining regions */
          if (!bSeenR)
          {
            /* No 'r' locks, only leave one region */
            if (lb->next != le)
            {
              l = lb->next;
              while (l != le)
              {
                assert(l);
                struct file_lock *next = l->next;
                lock_free(l);
                l = next;
              }
              lb->next = le;
            }
            if (lock_end(le) == end)
            {
              lb->next = le->next;
              lock_free(le);
            }
            else
            {
              le->start = end + 1;
            }
          }
          else
          {
            /* Just mark the regions with our type/pid */
            l = lb->next;
            while (l != le)
            {
              assert(l);
              if (lock_needs_mark (l, fl->l_type, pid))
              {
                rc = lock_mark(l, fl->l_type, pid);
                if (rc == -1)
                  break;
              }
              l = l->next;
            }
          }
        }
        while (0);

        if (rc == -1)
        {
          TRACE("No memory\n");
          errno = ENOLCK;
        }
      }
    }

    break;
  }

  if (gpData->blocked)
  {
    /* There are blocked threads, release them to let recheck regions */
    arc = DosPostEventSem(gEvSem);
    TRACE("DosPostEventSem = %d\n", arc);
  }

  mutex_unlock();

#if TRACE_MORE
  TRACE("Locks after:\n");
  for (l = desc->locks; l; l = l->next)
  {
    TRACE("- type '%c', start %lld, pid %d\n", l->type, l->start, l->pid);
  }
#endif
  TRACE_IF(rc, "rc=%d errno=%d\n", rc, errno);

  return rc;
}

/**
 * LIBC fcntl replacement. It will handle locking by calling fcntl_locking
 * and forward any other calls to LIBC fcntl.
 *
 * Note: the original fcntl definition is `int fcntl(int fildes, int cmd, ...)`
 * but we can't cleanly pass control from one vararg function to another one.
 * We could use ABI-dependent assembly but given that fcntl on OS/2 takes one
 * command argument max and it's either an int or a pointer, we use uniptr_t
 * that fits both types instead of ellipsis.
 */
int fcntl(int fildes, int cmd, intptr_t *arg)
{
  switch (cmd)
  {
    case F_GETLK:
    case F_SETLK:
    case F_SETLKW:
      return fcntl_locking(fildes, cmd, (struct flock *)arg);
    default:
      return _std_fcntl(fildes, cmd, arg);
  }
}

/**
 * Initialize/terminate DLL at load/unload.
 */
unsigned long _System _DLL_InitTerm(unsigned long hModule, unsigned long ulFlag)
{
  TRACE("ulFlag %d\n", ulFlag);

  switch (ulFlag)
  {
    /* InitInstance */
    case 0:
    {
      if (_CRT_init() != 0)
        return 0;
      __ctordtorInit();
      if (fcntl_locking_init(FALSE) != NO_ERROR)
        return 0;
      break;
    }

      /* TermInstance */
    case 1:
    {
      __ctordtorTerm();
      _CRT_term();
      break;
    }

    default:
      return 0;
  }

  /* Success */
  return 1;
}

/**
 * Called upon any process termination (even after a crash where _DLL_InitTerm
 * is not called).
 */
static void APIENTRY ProcessExit(ULONG reason)
{
  TRACE("reason %d\n", reason);

  fcntl_locking_term();

  DosExitList(EXLST_EXIT, NULL);
}
