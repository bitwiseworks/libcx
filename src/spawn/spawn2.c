/*
 * Implementation of spawn2 API.
 * Copyright (C) 2017 bww bitwise works GmbH.
 * This file is part of the kLIBC Extension Library.
 * Authored by Dmitry Kuminov <coding@dmik.org>, 2017.
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

#define INCL_BASE
#include <os2.h>

// Redefine fd_set to fit OPEN_MAX file descriptors
#define FD_SETSIZE OPEN_MAX
#include <sys/syslimits.h>

#include <stdlib.h>
#include <io.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <errno.h>

// for STREAM_LOCK
#include <sys/fmutex.h>
#include <emx/io.h>

#define TRACE_GROUP TRACE_GROUP_SPAWN

#include "../shared.h"

#include "libcx/spawn2.h"

#include "spawn2-internal.h"

int spawn2 (int mode, const char *name, const char * const argv[],
            const char *cwd, const char * const envp[], int stdfds[3])
{
  TRACE("mode %x name [%s] argv %p cwd [%s] envp %p stdfds %p\n",
        mode, name, argv, cwd, envp, stdfds);

  TRACE_BEGIN_IF(argv, "argv [");
    const char * const *a;
    for (a = argv; *a; ++a)
      TRACE_CONT("[%s]", *a);
    TRACE_CONT("]\n");
  TRACE_END();

  TRACE_BEGIN_IF(envp, "envp [");
    const char * const *e;
    for (e = envp; *e; ++e)
      TRACE_CONT("[%s]", *e);
    TRACE_CONT("]\n");
  TRACE_END();

  TRACE_BEGIN_IF(stdfds, "stdfs [");
    int i;
    for (i = 0; i < 3; ++i)
      TRACE_CONT("%d,", stdfds[i]);
    TRACE_CONT("]\n");
  TRACE_END();

  if (!name || !*name || !argv || !*argv)
    SET_ERRNO_AND(return -1, EINVAL);

  int have_stdfds = 0;

  if (stdfds)
  {
    if ((stdfds[0] == 1 || stdfds[0] == 2) ||
        (stdfds[1] == 1 || stdfds[1] == 2) ||
        (stdfds[2] == 2))
      SET_ERRNO_AND(return -1, EINVAL);

    have_stdfds = (stdfds[0] || stdfds[1] || stdfds[2]);
  }

  if (mode & P_2_THREADSAFE)
  {
    TRACE("using wrapper\n");

    if ((mode & P_2_MODE_MASK) != P_WAIT && (mode & P_2_MODE_MASK) != P_NOWAIT)
      SET_ERRNO_AND(return -1, EINVAL);

    char w_exe[CCHMAXPATH + sizeof(SPAWN2_WRAPPERNAME) + 1];
    if (!get_module_name (w_exe, sizeof(w_exe)))
      SET_ERRNO_AND(return -1, ENOMEM);

    strcpy(_getname(w_exe), SPAWN2_WRAPPERNAME);

    TRACE("w_exe [%s]\n", w_exe);

    /* Calculate how much dynamic memory is needed for the request */

    ULONG payload_size = strlen(name) + 1;

    int rc = 0, i;
    int argc = 0;
    int envc = 0;

    for (; argv[argc]; ++argc)
      payload_size += strlen(argv[argc]) + 1;
    payload_size += sizeof(char *) * (argc + 1);

    if (cwd)
      payload_size += strlen(cwd) + 1;

    if (envp)
    {
      for (; envp[envc]; ++envc)
        payload_size += strlen(envp[envc]) + 1;
      payload_size += sizeof(char *) * (envc + 1);
    }

    if (have_stdfds)
      payload_size += sizeof(*stdfds) * 3;

    /* Allocate the request from LIBCx shared memory */
    size_t req_size = sizeof(Spawn2Request) + payload_size;
    TRACE("request size %d bytes\n", req_size);

    HEV spawn2_sem;
    char *mem;

    global_lock();

    spawn2_sem = global_spawn2_sem(NULL);
    if (spawn2_sem)
      mem = global_alloc(req_size);

    global_unlock();

    if (!spawn2_sem || !mem)
      SET_ERRNO_AND(return -1, ENOMEM);

    /* Fill up request data */

    Spawn2Request *req = (Spawn2Request *)mem;

    req->_payload_size = payload_size;
    char *payload = req->_payload;

    req->mode = mode;

    size_t len = strlen(name) + 1;
    memcpy(payload, name, len);
    req->name = payload;
    payload += len;

    req->argv = (const char * const *)payload;
    payload += sizeof(char *) * (argc + 1);

    for (i = 0; i < argc; ++i)
    {
      len = strlen(argv[i]) + 1;
      memcpy(payload, argv[i], len);
      ((char **)req->argv)[i] = payload;
      payload += len;
    }

    ((char **)req->argv)[i] = NULL;

    if (cwd)
    {
      len = strlen(cwd) + 1;
      memcpy(payload, cwd, len);
      req->cwd = payload;
      payload += len;
    }
    else
      req->cwd = NULL;

    if (envp)
    {
      req->envp = (const char * const *)payload;
      payload += sizeof(char *) * (envc + 1);

      for (i = 0; i < envc; ++i)
      {
        len = strlen(envp[i]) + 1;
        memcpy(payload, envp[i], len);
        ((char **)req->envp)[i] = payload;
        payload += len;
      }

      ((char **)req->envp)[i] = NULL;
    }
    else
      req->envp = NULL;

    int inherited [3] = {0};

    if (have_stdfds)
    {
      len = sizeof(*stdfds) * 3;
      memcpy(payload, stdfds, len);
      req->stdfds = (int *)payload;
      payload += len;

      /* Make sure stdio file handles are inherited by the wrapper */
      for (i = 0; i < 3; ++i)
      {
        if (stdfds[i])
        {
          int f = rc = fcntl(stdfds[i], F_GETFD);
          if (f != -1 && (f & FD_CLOEXEC))
          {
            TRACE("enable inheritance for %d\n", stdfds[i]);
            rc = fcntl(stdfds[i], F_SETFD, f & ~FD_CLOEXEC);
            if (rc != -1)
              inherited[i] = 1;
          }
          if (rc == -1)
            break;
        }
      }
    }
    else
      req->stdfds = NULL;

    if (rc != -1)
    {
      ASSERT_MSG(payload - (char *)mem - sizeof(Spawn2Request) == payload_size,
                 "%d != %d", payload - (char *)mem - sizeof(Spawn2Request), payload_size);

      char sem_str[10];
      _ultoa(spawn2_sem, sem_str, 16);

      char mem_str[10];
      _ultoa((unsigned long)mem, mem_str, 16);

      const char *w_argv[] = { w_exe, sem_str, mem_str, NULL };

      rc = spawn2(P_NOWAIT, w_exe, w_argv, NULL, NULL, NULL);
      TRACE("spawn2(wrapper) rc %d (%x) errno %d\n", rc, rc, errno);
    }

    /* Restore stdio file handle inheritance */
    if (have_stdfds)
    {
      for (i = 0; i < 3; ++i)
      {
        if (inherited[i])
        {
          int f = fcntl(stdfds[i], F_GETFD);
          if (f != -1)
            f = fcntl(stdfds[i], F_SETFD, f | FD_CLOEXEC);
          ASSERT_MSG(f != -1, "%d %d", f, errno);
        }
      }
    }

    if (rc != -1)
    {
      APIRET arc = NO_ERROR;
      ULONG tries = 20;

      TRACE("waiting for wrapper to kick in\n");
      do
      {
        arc = DosWaitEventSem(spawn2_sem, 500);
        ASSERT_MSG(arc == NO_ERROR || arc == ERROR_TIMEOUT || arc == ERROR_INTERRUPT,
                   "%ld %lx", arc, spawn2_sem);

        if (req->rc)
          break;

        if (!--tries)
          break;
      }
      while (1);

      global_lock ();

      switch (req->rc)
      {
        case -1:
          TRACE("wrapper reported errno %d\n", req->err);
          rc = req->rc;
          errno = req->err;
          break;

        case 0:
          TRACE("wrapper timed out with arc %ld tries %d\n", arc, tries);
          rc = -1;
          errno = arc == ERROR_INTERRUPT ? EINTR : ETIMEDOUT;
          break;

        default:
        {
          TRACE("wrapped child pid %d (%x)\n", req->rc, req->rc);

          /*
           * Save the wrapped child PID (not really used so far but might be
           * needed later to implement waiting on it).
           */
          ProcDesc *proc = find_proc_desc(rc);
          if (proc)
            proc->child_pid = rc;

          break;
        }
      }

      /*
       * kLIBC spawn will eventually make a copy of the arguments so it should
       * be safe to free request memory now.
       */
      free(mem);

      global_unlock ();

      if (rc != -1)
      {
        if ((mode & P_2_MODE_MASK) == P_WAIT)
        {
          int status;
          rc = waitpid(rc, &status, 0);
          TRACE("waitpid(wrapper) rc %d status %x errno %d\n", rc, status, errno);

          rc = WEXITSTATUS(status);
        }
      }
    }
    else
    {
      global_lock ();
      free(mem);
      global_unlock ();
    }

    return rc;
  }

  int rc = 0;
  int rc_errno = 0;

  char *curdir = NULL;
  int dupfds[3] = {0};
  fd_set *clofds = NULL;

  // Change the current directory if needed
  if (cwd)
  {
    if ((curdir = getcwd(NULL, 0)) == NULL)
    {
      rc = -1;
      rc_errno = errno;
    }
    else
    {
      TRACE("curdir [%s]\n", curdir);

      if ((rc = chdir(cwd)) == -1)
      {
        rc_errno = errno;
        free(curdir);
        curdir = NULL;
      }
    }
  }

  if (rc != -1)
  {
    // Duplicate stdio (0,1,2) if needed
    if (have_stdfds)
    {
      int i;
      for (i = 0; i < 3; ++i)
      {
        // Note that we don't have to handle value of 1 in stdfds[2] specially:
        // if stdfds[1] was provided, 1 it was already redirected to it, so
        // redirecting to 1 is equivalent to redirecting to stdfds[1]; if it
        // was not, then we'll redirect 2 to 1 which is simply stdout in this
        // case - still just what we need.

        if (stdfds[i])
        {
          int fd = rc = dup(i);
          if (rc != -1)
          {
            int f = rc = fcntl(fd, F_GETFD);
            if (rc != -1)
            {
              rc = fcntl(fd, F_SETFD, f | FD_CLOEXEC);
              if (rc != -1)
                if ((rc = dup2(stdfds[i], i)) != -1)
                  dupfds[i] = fd;
            }
          }
          if (rc == -1)
          {
            rc_errno = errno;
            if (fd != -1)
              close(fd);
            break;
          }
        }
      }
    }

    if (rc != -1)
    {
      // Disable inheritance if needed
      if (mode & P_2_NOINHERIT)
      {
        clofds = malloc(sizeof(*clofds));
        if (clofds == NULL)
        {
          rc = -1;
          rc_errno = ENOMEM;
        }
        else
        {
          FD_ZERO(clofds);

          int fd;
          for (fd = 3; fd < OPEN_MAX; ++fd)
          {
            int f = fcntl(fd, F_GETFD);
            if (f != -1)
            {
              if (!(f & FD_CLOEXEC))
              {
                if ((rc = fcntl(fd, F_SETFD, f | FD_CLOEXEC)) != -1)
                  FD_SET(fd, clofds);
              }
            }
            if (rc == -1)
            {
              rc_errno = errno;
              break;
            }
          }
        }
      }

      if (rc != -1)
      {
        if (envp)
          rc = spawnvpe(mode, name, (char * const *)argv, (char * const *)envp);
        else
          rc = spawnvp(mode, name, (char * const *)argv);

        if (rc == -1)
          rc_errno = errno;

        TRACE_ERRNO_IF(rc == -1, "spawn*");
      }
    }
  }

  // Restore inheritance
  if (clofds)
  {
    int fd;
    for (fd = 3; fd < OPEN_MAX; ++fd)
    {
      if (FD_ISSET(fd, clofds))
      {
        int f = fcntl(fd, F_GETFD);
        if (f != -1)
          fcntl(fd, F_SETFD, f & ~FD_CLOEXEC);
      }
    }

    free(clofds);
  }

  // Restore stdio (0,1,2)
  if (have_stdfds)
  {
    int i;
    for (i = 0; i < 3; ++i)
    {
      if (dupfds[i])
      {
        dup2(dupfds[i], i);
        close(dupfds[i]);
      }
    }
  }

  // Restore the current directory
  if (curdir)
  {
    chdir(curdir);
    free(curdir);
  }

  if (rc_errno)
    errno = rc_errno;

  TRACE_ERRNO_IF(rc == -1, "return");
  TRACE("return %d (%x)\n", rc, rc);

  return rc;
}
