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
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include <assert.h>
#include <emx/io.h>

#undef fcntl

#include "../shared.h"

#define PID_LIST_MIN_SIZE 8

struct PidList
{
  size_t size;
  size_t used;
  pid_t list[0]; /* PID or 0 for empty cell */
};

/**
 * Fcntl file lock (linked list entry).
 */
struct FcntlLock
{
  struct FcntlLock *next;

  char type; /* 'R' = read lock, 'r' = multiple read locks, 'W' = write lock, 0 = no lock */
  off_t start; /* start of the lock region */
  union
  {
    pid_t pid; /* owner of the R or W lock */
    struct PidList *pids; /* owners of the r locks */
  };
};

/**
 * Blocked process (linked list entry).
 */
struct ProcBlock
{
  struct ProcBlock *next;
  pid_t pid; /* pid of the blocked process */

  char type; /* Type of the requested lock, 'R' or 'W' */
  off_t start; /* Start of the requested lock */
  off_t end; /* End of the requested lock */
  char path[PATH_MAX]; /* File hame with full path */

  pid_t blocker; /* pid of the blocking process */
};

/**
 * Global fcntl locking data structure.
 */
struct FcntlLocking
{
  HEV hEvSem; /* Semaphore for blocked processes to wait on */
  struct ProcBlock *blocked; /* Processes blocked in F_SETLKW */
};

static int gbTerminate = 0; /* 1 after fcntl_locking_term is called */

static struct PidList *copy_pids(const struct PidList *list)
{
  assert(list);
  size_t size = sizeof(*list) + sizeof(list->list[0]) * list->size;
  struct PidList *nlist = _ucalloc(gpData->heap, 1, size);
  if (nlist)
    memcpy(nlist, list, size);
  return nlist;
}

static int lock_has_pid(struct FcntlLock *l, pid_t pid)
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
static int lock_needs_mark(struct FcntlLock *l, short type, pid_t pid)
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
static int lock_mark(struct FcntlLock *l, short type, pid_t pid)
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
          struct PidList *nlist =
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
        struct PidList *nlist =
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

inline static off_t lock_end(struct FcntlLock *l)
{
    return l->next ? l->next->start - 1 : OFF_MAX;
}

inline static off_t lock_len(struct FcntlLock *l)
{
    return l->next ? l->next->start - l->start : 0;
}

static void lock_free(struct FcntlLock *l)
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
static struct FcntlLock *lock_split(struct FcntlLock *l, off_t split)
{
  struct FcntlLock *ln = NULL;

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

/**
 * Initializes the fcntl portion of FileDesc.
 * Called right after the FileDesc pointer is allocated.
 * Returns 0 on success or -1 on failure.
 */
int fcntl_locking_filedesc_init(struct FileDesc *desc)
{
  /* Add one free region that covers the entire file */
  desc->fcntl_locks = _ucalloc(gpData->heap, 1, sizeof(*desc->fcntl_locks));
  if (!desc->fcntl_locks)
    return -1;
  return 0;
}

/**
 * Uninitializes the fcntl portion of FileDesc.
 * Called right before the FileDesc pointer is freed.
 */
void fcntl_locking_filedesc_term(struct FileDesc *desc)
{
  struct FcntlLock *l = desc->fcntl_locks;
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
    struct FcntlLock *n = l->next;
    free(l);
    l = n;
  }
}

/**
 * Initializes the fcntl shared structures.
 * Called after successfull gpData allocation and gpData->heap creation.
 * @return NO_ERROR on success, DOS error code otherwise.
 */
int fcntl_locking_init()
{
  APIRET arc;

  if (gpData->refcnt == 1)
  {
    /* We are the first processs, initialize fcntl structures */
    gpData->fcntl_locking = _ucalloc(gpData->heap, 1, sizeof(*gpData->fcntl_locking));
    assert(gpData->fcntl_locking);

    arc = DosCreateEventSem(NULL, &gpData->fcntl_locking->hEvSem,
                            DC_SEM_SHARED | DCE_AUTORESET, FALSE);
    TRACE("DosCreateEventSem = %d\n", arc);
    assert(arc == NO_ERROR);
  }
  else
  {
    assert(gpData->fcntl_locking);
    assert(gpData->fcntl_locking->hEvSem);
    arc = DosOpenEventSem(NULL, &gpData->fcntl_locking->hEvSem);
    TRACE("DosOpenEventSem = %d\n", arc);
    assert(arc == NO_ERROR);
  }

  return arc;
}

/**
 * Uninitializes the fcntl shared structures.
 * Called before destroying gpData->heap and gpData.
 */
void fcntl_locking_term()
{
  APIRET arc;
  int rc, i;

  gbTerminate = 1;

  if (gpData->files)
  {
    pid_t pid = getpid();
    int bNeededMark = 0;

    /* Go through all locks to unlock any regions this process owns */
    for (i = 0; i < FILE_DESC_HASH_SIZE; ++i)
    {
      struct FileDesc *desc = gpData->files[i];
      while (desc)
      {
        struct FcntlLock *l = desc->fcntl_locks;
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
    if (gpData->fcntl_locking->blocked)
    {
      struct ProcBlock *bp = NULL, *b = gpData->fcntl_locking->blocked;
      while (b)
      {
        if (b->pid == pid)
        {
          TRACE("Will unblock [%s], type '%c', start %lld, len %lld\n",
                b->path, b->type, (uint64_t)b->start,
                (uint64_t)(b->end == OFF_MAX ? 0 : b->end - b->start + 1));
          struct ProcBlock *bn = b->next;
          if (bp)
            bp->next = bn;
          else
            gpData->fcntl_locking->blocked = bn;
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

    if (bNeededMark && gpData->fcntl_locking->blocked)
    {
      /* We unblocked something and there are blocked threads, release them */
      arc = DosPostEventSem(gpData->fcntl_locking->hEvSem);
      TRACE("DosPostEventSem = %d\n", arc);
      /* Invalidate the blocked list (see the other DosPostEventSem call) */
      gpData->fcntl_locking->blocked = NULL;
    }
  }

  if (gpData->refcnt == 0)
  {
    /* We are the last process, free fcntl structures */
    TRACE("gpData->fcntl_locking->blocked %p\n", gpData->fcntl_locking->blocked);
    if (gpData->fcntl_locking->blocked)
    {
      struct ProcBlock *b = gpData->fcntl_locking->blocked;
      while (b)
      {
        TRACE("WARNING! Blocked proc: pid %d, path [%s], type='%c', start %lld, len %lld\n",
              b->pid, b->path, b->type, (uint64_t)b->start,
              (uint64_t)(b->end == OFF_MAX ? 0 : b->end - b->start + 1));
        struct ProcBlock *n = b->next;
        free(b);
        b = n;
      }
    }

    arc = DosCloseEventSem(gpData->fcntl_locking->hEvSem);
    if (arc == ERROR_SEM_BUSY)
    {
      /* The semaphore may be owned by us, try to release it */
      arc = DosPostEventSem(gpData->fcntl_locking->hEvSem);
      TRACE("DosPostEventSem = %d\n", arc);
      arc = DosCloseEventSem(gpData->fcntl_locking->hEvSem);
    }
    TRACE("DosCloseEventSem = %d\n", arc);

    free(gpData->fcntl_locking);
  }
}

static int fcntl_locking(int fildes, int cmd, struct flock *fl)
{
  APIRET arc;
  int rc, bSeenOtherPid, bNoMem = 0, bNeededMark = 0;
  off_t start, end;
  struct FileDesc *desc = NULL;
  struct FcntlLock *lll = NULL, *lb = NULL, *le = NULL;
  struct FcntlLock *blocker = NULL;
  struct ProcBlock *blocked = NULL;
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

  global_lock();

  while (1)
  {
    desc = get_file_desc(pFH->pszNativePath, cmd != F_GETLK);
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
      struct FcntlLock *l;
      for (l = desc->fcntl_locks; l; l = l->next)
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
    assert(desc->fcntl_locks);
    assert(desc->fcntl_locks->start == 0);
    lb = desc->fcntl_locks;
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
          struct ProcBlock *bp, *b;

          assert(fl->l_type != F_UNLCK);

          TRACE("Need type '%c', start %lld, len %lld\n", fl->l_type == F_WRLCK ? 'W' : 'R',
                (uint64_t)start, (uint64_t)(end == OFF_MAX ? 0 : end - start + 1));
          TRACE("Will wait (blocked head %p)\n", gpData->fcntl_locking->blocked);

          /* Check if our blocker is itself blocked on us (which would mean a deadlock) */
          b = gpData->fcntl_locking->blocked;
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
          blocked->next = gpData->fcntl_locking->blocked;
          gpData->fcntl_locking->blocked = blocked;

          global_unlock();

          arc = DosWaitEventSem(gpData->fcntl_locking->hEvSem, SEM_INDEFINITE_WAIT);
          TRACE("DosWaitEventSem = %d\n", arc);

          assert(arc == NO_ERROR || arc == ERROR_INTERRUPT);

          if (arc == ERROR_INTERRUPT)
          {
            errno = EINTR;
            rc = -1;
          }

          global_lock();

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
              struct FcntlLock *ln = NULL;
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
              struct FcntlLock *l = lb->next;
              while (l != le)
              {
                assert(l);
                struct FcntlLock *next = l->next;
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
            struct FcntlLock *l = lb->next;
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

  if (cmd != F_GETLK && fl->l_type == F_UNLCK && bNeededMark &&
      gpData->fcntl_locking->blocked)
  {
    /*
     * We unlocked some locks and there are blocked threads,
     * release them to let recheck regions.
     */
    arc = DosPostEventSem(gpData->fcntl_locking->hEvSem);
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
    gpData->fcntl_locking->blocked = NULL;
  }

  TRACE_BEGIN_IF(TRACE_MORE && cmd != F_GETLK, "Locks after:\n");
  {
    struct FcntlLock *l;
    for (l = desc->fcntl_locks; l; l = l->next)
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

  global_unlock();

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
