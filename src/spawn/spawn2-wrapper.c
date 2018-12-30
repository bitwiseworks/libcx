/*
 * Implementation of spawn2 wrapper.
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

#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <errno.h>

#include <InnoTekLIBC/backend.h>
#include <InnoTekLIBC/sharedpm.h>

#define TRACE_GROUP TRACE_GROUP_SPAWN

#include "../shared.h"

#include "libcx/spawn2.h"

#include "spawn2-internal.h"

int main(int argc, char **argv)
{
  TRACE("argc %d argv %p\n", argc, argv);

  int rc = 127;

  if (argc < 3)
    TRACE_AND(return rc, "argc %d < 3\n", argc);

  /*
   * Note that the semaphore and memory block (which is from LIBCx shared heap)
   * are made accessible to this process buy shared_init().
   */

  APIRET arc;
  HEV hev = strtoul(argv[1], NULL, 16);
  char *mem = (char *)strtoul(argv[2], NULL, 16);

  /* Copy the data over and report the parent is ok to free it */

  Spawn2Request *req = (Spawn2Request *)mem;

  /* Force mode to P_WAIT and remove P_2_THREADSAFE + disable inheritance */
  int mode = req->mode;
  mode &= ~(P_2_MODE_MASK | P_2_THREADSAFE);
  mode |= P_NOWAIT | P_2_NOINHERIT;

  rc = spawn2(mode, req->name, req->argv, req->cwd, req->envp, req->stdfds);
  TRACE("spawn2(wrapped child) rc %d (%x) errno %d\n", rc, rc, errno);

  /* Set the spawn result... */
  if (rc == -1)
  {
    req->rc = -1;
    req->err = errno;
  }
  else
  {
    req->rc = rc;
  }

  /* ...close the handles to give the child full control over them... */
  if (req->stdfds)
  {
    int i;
    for (i = 0; i < 3; ++i)
      if (req->stdfds[i] > 2) /* only close non-stdio ones */
        close(req->stdfds[i]);
  }

  /* ...and report to the parent waiting in spawn2 ASAP */
  arc = DosPostEventSem(hev);
  ASSERT_MSG(arc == NO_ERROR || arc == ERROR_ALREADY_POSTED, "%ld %lx", arc, hev);

  if (rc != -1)
  {
    pid_t pid = rc;
    int status;

    rc = waitpid(pid, &status, 0);
    TRACE("waitpid(wrapped child) rc %d (%x) status %x errno %d\n", rc, rc, status, errno);

    if (rc != -1)
    {
      if (WIFEXITED(status))
        rc = WEXITSTATUS(status);
      else if (WIFSIGNALED(status))
        raise(WTERMSIG(status));
      else
        ASSERT_MSG(0, "invalid status %x", status);
    }
  }

  return rc;
}
