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
#define TRACE_RAW(msg, ...) printf("*** [%d:%d] %s:%d:%s: " msg, getpid(), _gettid(), __FILE__, __LINE__, __FUNCTION__, ## __VA_ARGS__)
#define TRACE(msg, ...) do { TRACE_RAW(msg, ## __VA_ARGS__); fflush(stdout); } while(0)
#define TRACE_BEGIN(msg, ...) { do { TRACE_RAW(msg, ## __VA_ARGS__); } while(0)
#define TRACE_CONT(msg, ...) printf(msg, ## __VA_ARGS__)
#define TRACE_END() fflush(stdout); } do {} while(0)
#define TRACE_IF(cond, msg, ...) if (cond) TRACE(msg, ## __VA_ARGS__)
#define TRACE_BEGIN_IF(cond, msg, ...) if (cond) TRACE_BEGIN(msg, ## __VA_ARGS__)
#else
#define TRACE_MORE 0
#define TRACE_RAW(msg, ...) do {} while (0)
#define TRACE(msg, ...) do {} while (0)
#define TRACE_BEGIN(msg, ...) if (0) { do {} while(0)
#define TRACE_CONT(msg, ...) do {} while (0)
#define TRACE_END() } do {} while(0)
#define TRACE_IF(cond, msg, ...) do {} while (0)
#define TRACE_BEGIN_IF(cond, msg, ...) if (0) { do {} while(0)
#endif

#define MUTEX_FCNTL "\\SEM32\\FCNTL_LOCKING_V1_MUTEX"
#define EVENTSEM_FCNTL "\\SEM32\\FCNTL_LOCKING_V1_EVENTSEM"
#define SHAREDMEM_FCNTL "\\SHAREMEM\\FCNTL_LOCKING_V1_DATA"

#define HEAP_SIZE 65536 /* initial size for fcntl data */

static HMTX gMutex = NULLHANDLE;
static HEV gEvSem = NULLHANDLE;

#define PID_LIST_MIN_SIZE 8

struct pid_list
{
  size_t size;
  size_t used;
  pid_t list[0]; /* PID or 0 for empty cell */
};

/**
 * File lock (linked list entry).
 */
struct file_lock
{
  struct file_lock *next;

  char type; /* 'R' = read lock, 'r' = multiple read locks, 'W' = write lock, 0 = no lock */
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
  struct file_desc *next;

  struct file_lock *locks; /* Active file locks */
  char path[0]; /* File name with fill path */
};

/**
 * Blocked process (linked list entry).
 */
struct proc_block
{
  struct proc_block *next;
  pid_t pid; /* pid of the blocked process */

  char type; /* Type of the requested lock, 'R' or 'W' */
  off_t start; /* Start of the requested lock */
  off_t end; /* End of the requested lock */
  char path[PATH_MAX]; /* File hame with full path */

  pid_t blocker; /* pid of the blocking process */
};

#define FILE_DESC_HASH_SIZE 127 /* Prime */

struct shared_data
{
  Heap_t heap;
  int refcnt; /* number of processes using us */
  struct file_desc **files; /* File descriptor hash map of FILE_DESC_HASH_SIZE */
  struct proc_block *blocked; /* Processes blocked in F_SETLKW */
};

static struct shared_data *gpData = NULL;

static int gbTerminate = 0; /* 1 after fcntl_locking_term is called */

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
        pid_t p = 0;
        assert(l->pids && l->pids->used);
        for (i = 0; i < l->pids->size; ++i)
        {
          if (l->pids->list[i] == pid)
          {
            l->pids->list[i] = 0;
            --l->pids->used;
            if (l->pids->used == 1)
            {
              /* Convert to R (blocker detection relies on this) */
              if (!p)
              {
                /* Didn't see the other pid yet, search for it */
                while (++i < l->pids->size && !l->pids->list[i]);
                assert(i < l->pids->size);
                p = l->pids->list[i];
              }
              free(l->pids);
              l->type = 'R';
              l->pid = p;
            }
            else if (l->pids->used == 0)
            {
              free(l->pids);
              l->type = 0;
              l->pid = 0;
            }
            break;
          }
          else if (l->pids->used == 2 && l->pids->list[i])
          {
            /* Save the other pid in the table */
            p = l->pids->list[i];
          }
        }
        assert(l->type == 0 || l->type == 'R' || i < l->pids->size);
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
              break;
            }
          }
          assert(i < l->pids->size);
        }
        else
        {
          /* Need more space */
          assert(l->pids->used == l->pids->size);
          size_t nsize = l->pids->size += PID_LIST_MIN_SIZE;
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
        size_t nsize = PID_LIST_MIN_SIZE;
        struct pid_list *nlist =
            _ucalloc(gpData->heap, 1, sizeof(*l->pids) + sizeof(l->pids->list[0]) * nsize);
        if (!nlist)
          return -1;
        nlist->size = nsize;
        nlist->used = 2;
        nlist->list[0] = l->pid;
        nlist->list[1] = pid;
        l->type = 'r';
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

inline static off_t lock_len(struct file_lock *l)
{
    return l->next ? l->next->start - l->start : 0;
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

  h = hash_string(path) % FILE_DESC_HASH_SIZE;
  desc = gpData->files[h];

  while (desc)
  {
    if (strcmp(desc->path, path) == 0)
      break;
    desc = desc->next;
  }

  if (!desc && bNew)
  {
    desc = _ucalloc(gpData->heap, 1, sizeof(*desc) + strlen(path) + 1);
    if (desc)
    {
      /* Initialize the new desc */
      strcpy(desc->path, path);
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

#if TRACE_ENABLED
  /*
   * Allocate a larger buffer to fit lengthy TRACE messages and disable
   * auto-flush on EOL (to avoid breaking them by stdout operations
   * from other threads/processes).
   */
  setvbuf(stdout, NULL, _IOFBF, 0x10000);
#endif

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
      gpData->files = _ucalloc(gpData->heap, FILE_DESC_HASH_SIZE, sizeof(*gpData->files));
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

  gbTerminate = 1;

  if (gpData)
  {
    if (gpData->heap)
    {
      int i;

      assert(gpData->refcnt);
      gpData->refcnt--;

      if (gpData->files)
      {
        pid_t pid = getpid();
        int bNeededMark = 0;

        /* Go through all locks to unlock any regions this process owns */
        for (i = 0; i < FILE_DESC_HASH_SIZE; ++i)
        {
          struct file_desc *desc = gpData->files[i];
          while (desc)
          {
            struct file_lock *l = desc->locks;
            while (l)
            {
              if (lock_needs_mark(l, F_UNLCK, pid))
              {
                TRACE("Will unlock [%s], type '%c', start %lld, len %lld\n",
                      desc->path, l->type, (uint64_t)l->start, (uint64_t)lock_len(l));
                rc = lock_mark(l, F_UNLCK, pid);
                TRACE_IF(rc, "rc = %d\n", rc);
                bNeededMark = 1;
              }
              l = l->next;
            }

            desc = desc->next;
          }
        }

        /* Go through all blocked processes and remove ourselves */
        if (gpData->blocked)
        {
          struct proc_block *bp = NULL, *b = gpData->blocked;
          while (b)
          {
            if (b->pid == pid)
            {
              TRACE("Will unblock [%s], type '%c', start %lld, len %lld\n",
                    b->path, b->type, (uint64_t)b->start,
                    (uint64_t)(b->end == OFF_MAX ? 0 : b->end - b->start + 1));
              struct proc_block *bn = b->next;
              if (bp)
                bp->next = bn;
              else
                gpData->blocked = bn;
              free(b);
              b = bn;
            }
            else
            {
              bp = b;
              b = b->next;
            }
          }
        }

        if (bNeededMark && gpData->blocked)
        {
          /* We unblocked something and there are blocked threads, release them */
          arc = DosPostEventSem(gEvSem);
          TRACE("DosPostEventSem = %d\n", arc);
          /* Invalidate the blocked list (see the other DosPostEventSem call) */
          gpData->blocked = NULL;
        }
      }

      if (gpData->refcnt == 0)
      {
        /* We are the last process, free structures */
        TRACE("gpData->blocked %p\n", gpData->blocked);
        if (gpData->blocked)
        {
          struct proc_block *b = gpData->blocked;
          while (b)
          {
            TRACE("WARNING! Blocked proc: pid %d, path [%s], type='%c', start %lld, len %lld\n",
                  b->pid, b->path, b->type, (uint64_t)b->start,
                  (uint64_t)(b->end == OFF_MAX ? 0 : b->end - b->start + 1));
            struct proc_block *n = b->next;
            free(b);
            b = n;
          }
        }
        TRACE("gpData->files %p\n", gpData->files);
        if (gpData->files)
        {
          for (i = 0; i < FILE_DESC_HASH_SIZE; ++i)
          {
            struct file_desc *desc = gpData->files[i];
            while (desc)
            {
              struct file_desc *next = desc->next;
              struct file_lock *l = desc->locks;
              while (l)
              {
                TRACE_BEGIN_IF(l->type, "WARNING! Forgotten lock: type '%c' start %lld, len %lld, ",
                               l->type, (uint64_t)l->start,
                               (uint64_t)lock_len(l), l->pid);
                if (l->type == 'r')
                {
                  int i;
                  TRACE_CONT("pids ");
                  for (i = 0; i < l->pids->size; ++i)
                    if (l->pids->list[i])
                      TRACE_CONT("%d ", l->pids->list[i]);
                  TRACE_CONT("\n");
                }
                else
                  TRACE_CONT("pid %d\n", l->pid);
                TRACE_END();
                struct file_lock *n = l->next;
                free(l);
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
  if (arc == ERROR_SEM_BUSY)
  {
    /* The semaphore may be owned by us, try to release it */
    arc = DosPostEventSem(gEvSem);
    TRACE("DosPostEventSem = %d\n", arc);
    arc = DosCloseEventSem(gEvSem);
  }
  TRACE("DosCloseEventSem = %d\n", arc);

  DosReleaseMutexSem(gMutex);

  arc = DosCloseMutexSem(gMutex);
  if (arc == ERROR_SEM_BUSY)
  {
    /* The semaphore may be owned by us, try to release it */
    arc = DosReleaseMutexSem(gMutex);
    TRACE("DosReleaseMutexSem = %d\n", arc);
    arc = DosCloseMutexSem(gMutex);
  }
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

  TRACE_IF(arc, "DosRequestMutexSem = %d\n", arc);
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
  int rc, bSeenOtherPid, bNoMem = 0, bNeededMark = 0;
  off_t start, end;
  struct file_desc *desc = NULL;
  struct file_lock *lll = NULL, *lb = NULL, *le = NULL;
  struct file_lock *blocker = NULL;
  struct proc_block *blocked = NULL;
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
    TRACE("start %lld (0x%llx), end %lld (0x%llx)\n",
          (uint64_t)start, (uint64_t)start, (uint64_t)end, (uint64_t)end);
    errno = EINVAL;
    return -1;
  }
  if (start > OFF_MAX || end > OFF_MAX)
  {
    TRACE("start %lld (0x%llx), end %lld (0x%llx) OFF_MAX %lld (0x%llx)\n",
          (uint64_t)start, (uint64_t)start, (uint64_t)end, (uint64_t)end,
          (uint64_t)OFF_MAX, (uint64_t)OFF_MAX);
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
        bNoMem = 1;
        break;
      }
    }

    TRACE_BEGIN_IF(TRACE_MORE, "Locks before:\n");
    {
      struct file_lock *l;
      for (l = desc->locks; l; l = l->next)
      {
        TRACE_CONT("- type '%c', start %lld, ", l->type, (uint64_t)l->start);
        if (l->type == 'r')
        {
          int i;
          TRACE_CONT("pids ");
          for (i = 0; i < l->pids->size; ++i)
            if (l->pids->list[i])
              TRACE_CONT("%d ", l->pids->list[i]);
          TRACE_CONT("\n");
        }
        else
          TRACE_CONT("pid %d\n", l->pid);
      }
    }
    TRACE_END();

    /* Search for the first overlapping region */
    assert(desc->locks);
    assert(desc->locks->start == 0);
    lb = desc->locks;
    while (lb->next && lb->next->start <= start)
      lb = lb->next;
    /*
     * Search for the last overlapping region and also check if there are any
     * regions locked by other processes (includng the blocking ones).
     */
    bSeenOtherPid = 0;
    blocker = NULL;
    le = lb;
    while (1)
    {
      if (!bSeenOtherPid)
        bSeenOtherPid = le->type == 'r' || (le->type == 'R' && le->pid != pid);
      if (!blocker)
      {
        if (le->type == 'r')
        {
          if (fl->l_type == F_WRLCK)
          {
            /* 'r' implies other PIDs hold a read lock, so write is blocked */
            assert(le->pids->used > 1);
            blocker = le;
          }
        }
        else if (fl->l_type != F_UNLCK)
        {
          if ((le->type == 'W' || (le->type == 'R' && fl->l_type == F_WRLCK)) &&
              le->pid != pid)
            blocker = le;
        }
      }
      if (le->next && le->next->start <= end)
        le = le->next;
      else
        break;
    }

    TRACE_BEGIN_IF(blocker, "Would block on type '%c', start %lld, len %lld, ",
                   blocker->type, (uint64_t)blocker->start,
                   (uint64_t)lock_len(blocker), blocker->pid);
    if (blocker->type == 'r')
    {
      int i;
      TRACE_CONT("pids ");
      for (i = 0; i < blocker->pids->size; ++i)
        if (blocker->pids->list[i])
          TRACE_CONT("%d ", blocker->pids->list[i]);
      TRACE_CONT("\n");
    }
    else
      TRACE_CONT("pid %d\n", blocker->pid);
    TRACE_END();

    if (cmd == F_GETLK)
    {
      if (blocker)
      {
        /* Copy over the blocking lock data */
        fl->l_type = blocker->type == 'W' ? F_WRLCK : F_RDLCK;
        fl->l_whence = SEEK_SET;
        fl->l_start = blocker->start;
        fl->l_len = blocker->next ? blocker->next->start - blocker->start : 0;
        fl->l_pid = blocker->type == 'r' ? blocker->pids->list[0] : blocker->pid;
      }
      else
      {
        /* No blocking locks on this file */
        fl->l_type = F_UNLCK;
      }
    }
    else
    {
      if (blocker)
      {
        if (cmd == F_SETLK)
        {
          /*
           * While POSIX lists both EACCES and EAGAIN as possible errors for a
           * blocking condition and the original kLIBC call returns EACCES, we
           * use EAGAIN here to be GNU/Linux compatible (and the POSIX conforming
           * app should check for both). This suppresses false failed tdb_brloock
           * trace log records in Samba.
           */
          errno = EAGAIN;
          rc = -1;
        }
        else
        {
          /* Block this thread due to F_SETLKW */
          struct proc_block *bp, *b;

          assert(fl->l_type != F_UNLCK);

          TRACE("Need type '%c', start %lld, len %lld\n", fl->l_type == F_WRLCK ? 'W' : 'R',
                (uint64_t)start, (uint64_t)(end == OFF_MAX ? 0 : end - start + 1));
          TRACE("Will wait (blocked head %p)\n", gpData->blocked);

          /* Check if our blocker is itself blocked on us (which would mean a deadlock) */
          b = gpData->blocked;
          while (b)
          {
            if (lock_has_pid(blocker, b->pid) && b->blocker == pid)
            {
              /* Got one; report a deadlock condition */
              TRACE("Deadlock detected\n");
              errno = EDEADLK;
              rc = -1;
              break;
            }
            b = b->next;
          }
          if (rc == -1)
            break;

          /* Initialze the blocking struct if needed */
          if (!blocked)
          {
            blocked = _ucalloc(gpData->heap, 1, sizeof(*blocked));
            if (!blocked)
            {
              bNoMem = 1;
              break;
            }
            blocked->pid = pid;
            blocked->type = fl->l_type == F_WRLCK ? 'W' : 'R';
            blocked->start = start;
            blocked->end = end;
            strncpy(blocked->path, pFH->pszNativePath, PATH_MAX - 1);
          }

          /* Remember the blocking pid */
          blocked->blocker = blocker->pid;

          /* Add the new block to the head of the blocked list */
          blocked->next = gpData->blocked;
          gpData->blocked = blocked;

          mutex_unlock();

          arc = DosWaitEventSem(gEvSem, SEM_INDEFINITE_WAIT);
          TRACE("DosWaitEventSem = %d\n", arc);

          assert(arc == NO_ERROR || arc == ERROR_INTERRUPT);

          if (arc == ERROR_INTERRUPT)
          {
            errno = EINTR;
            rc = -1;
          }

          mutex_lock();

          if (gbTerminate)
          {
            /*
             * Our process have already freed all blocked data including our
             * blocked record in fcntl_locking_term() due to unexpected
             * termination, report it as EINTR just in case (if not already
             * reporting some error).
             */
            TRACE("gbTerminate\n");
            blocked = NULL;
            if (rc != -1)
            {
              errno = EINTR;
              rc = -1;
            }
          }

          /*
           * Note: we don't need to remove our blocked record from the blocked
           * list because it must have been reset by the thread that woke us
           * up in order to invalidate it (see the comment near the
           * DosPostEventSem call below).
           */

          if (rc == -1)
            break;

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
            bNeededMark = 1;
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
                if (lock_end(ln) > end)
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
            bNeededMark = 1;
            if (bSeenOtherPid)
            {
              /* There are regions with other PIDs, we have to split */
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
            /* Note: !bSeenOtherPid case is processed later */
          }

          /* Process the remaining regions */
          if (!bSeenOtherPid)
          {
            /* No regions with other PIDs, we may join */
            if (lb->next != le)
            {
              /* Delete regions between first and last */
              struct file_lock *l = lb->next;
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
              /* The last region is fully inside the join */
              lb->next = le->next;
              lock_free(le);
            }
            else
            {
              /* Move the start point of the last region */
              le->start = end + 1;
            }
          }
          else
          {
            /* Just mark the regions with our type/pid */
            struct file_lock *l = lb->next;
            while (l != le)
            {
              assert(l);
              if (lock_needs_mark (l, fl->l_type, pid))
              {
                bNeededMark = 1;
                rc = lock_mark(l, fl->l_type, pid);
                if (rc == -1)
                  break;
              }
              l = l->next;
            }
          }
        }
        while (0);

        /* We may only get -1 above due to mem alloc failure */
        if (rc == -1)
          bNoMem = 1;
      }
    }

    break;
  }

  if (bNoMem)
  {
    /*
     * When we fail to allocate more memory in our shared heap we report this
     * as the inability to satisfy the lock or unlock request due to a system-
     * imposed limit and hence return ENOLCK. Later we may decide to grow
     * the shared heap dynamically as needed.
     */
    TRACE("No memory\n");
    errno = ENOLCK;
    rc = -1;
  }

  if (cmd != F_GETLK && fl->l_type == F_UNLCK && bNeededMark && gpData->blocked)
  {
    /*
     * We unlocked some locks and there are blocked threads,
     * release them to let recheck regions.
     */
    arc = DosPostEventSem(gEvSem);
    TRACE("DosPostEventSem = %d\n", arc);
    /*
     * Let woken up threads run. W/o this call this thread will continue to run
     * till the end of the time slice and may lock the same region again w/o
     * giving other threads any chance to execute and therefore intoduce a
     * starvation.
     */
    DosSleep(0);
    /*
     * Reset the blocked list to invalidate it - the entries in this list may
     * refer to our PID as a blocker which may be not true any more (and could
     * cause a false deadlock detection upon our new attempt to lock something
     * during the current time slice if the above yielding didn't give some
     * blocking thread a chance to run for some reason).
     */
    gpData->blocked = NULL;
  }

  TRACE_BEGIN_IF(TRACE_MORE && cmd != F_GETLK, "Locks after:\n");
  {
    struct file_lock *l;
    for (l = desc->locks; l; l = l->next)
    {
      TRACE_CONT("- type '%c', start %lld, ", l->type, (uint64_t)l->start);
      if (l->type == 'r')
      {
        int i;
        TRACE_CONT("pids ");
        for (i = 0; i < l->pids->size; ++i)
          if (l->pids->list[i])
            TRACE_CONT("%d ", l->pids->list[i]);
        TRACE_CONT("\n");
      }
      else
        TRACE_CONT("pid %d\n", l->pid);
    }
  }
  TRACE_END();

  mutex_unlock();

  if (blocked)
    free(blocked);

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
