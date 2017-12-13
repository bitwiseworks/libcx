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

// Redefine fd_set to fit OPEN_MAX file descriptors
#define FD_SETSIZE OPEN_MAX
#include <sys/syslimits.h>

#include <stdlib.h>
#include <io.h>
#include <fcntl.h>
#include <sys/select.h>
#include <errno.h>

#define TRACE_GROUP TRACE_GROUP_SPAWN

#include "../shared.h"

#include "libcx/spawn2.h"

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

  if (stdfds)
  {
    if ((stdfds[0] == 1 && stdfds[0] == 2) ||
        (stdfds[1] == 1 && stdfds[0] == 2) ||
        (stdfds[2] == 2))
      SET_ERRNO_AND(return -1, EINVAL);
  }

  if ((mode & 0x7) == P_WAIT || (mode & 0x7) == P_OVERLAY || (mode & 0x7) == P_DETACH)
    SET_ERRNO_AND(return -1, EINVAL);

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
    if (stdfds)
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
  if (stdfds)
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
  TRACE("return %x\n", rc);

  return rc;
}
