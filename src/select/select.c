/*
 * select replacement for kLIBC.
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

#include <errno.h>
#include <sys/select.h>
#include <sys/stat.h>

#include "../shared.h"

#ifndef MAX
#define MAX(a,b)	((a) > (b) ? (a) : (b))
#endif

int select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
           struct timeval *timeout)
{
  TRACE("nfds %d, readfds %x, writefds %x, exceptfds %x timeout %x (%ld.%ld)\n",
        nfds, readfds, writefds, exceptfds, timeout,
        timeout ? timeout->tv_sec : 0, timeout ? timeout->tv_usec : 0);

  int fd;
  int n_ready_fds;
  int nfds_ret;
  struct stat st;

  fd_set r_new;
  fd_set w_new;
  fd_set e_new;
  int max_fd;

  FD_ZERO (&r_new);
  FD_ZERO (&w_new);
  FD_ZERO (&e_new);

  if (readfds)
    r_new = *readfds;
  if (writefds)
    w_new = *writefds;
  if (exceptfds)
    e_new = *exceptfds;

  n_ready_fds = 0;
  max_fd = -1;

  for (fd = 0; fd < nfds; ++fd)
  {
    int n_fd_set = 0;

    if (readfds && FD_ISSET(fd, readfds))
      ++n_fd_set;
    if (writefds && FD_ISSET(fd, writefds))
      ++n_fd_set;
    if (exceptfds && FD_ISSET(fd, exceptfds))
      ++n_fd_set;

    if (n_fd_set)
    {
      if (fstat (fd, &st) != -1 && S_ISREG (st.st_mode))
      {
        /*
         * Regular filres should be always immediately ready for I/O,
         * remove this fd from the new set, no need to select.
         */
        FD_CLR(fd, &r_new);
        FD_CLR(fd, &w_new);
        FD_CLR(fd, &e_new);
        n_ready_fds += n_fd_set;
      }
      else
      {
        /* We copied requests to the new array, clear the old one */
        if (readfds)
          FD_CLR(fd, readfds);
        if (writefds)
          FD_CLR(fd, writefds);
        if (exceptfds)
          FD_CLR(fd, exceptfds);

        max_fd = MAX(max_fd, fd);
      }
    }
  }

  if (max_fd == -1 && n_ready_fds)
  {
    /* There are only regular files, no need to call select. */
    nfds_ret = 0;
  }
  else
  {
    nfds_ret = _std_select(max_fd + 1,
                           readfds ? &r_new : NULL,
                           writefds ? &w_new : NULL,
                           exceptfds ? &e_new : NULL, timeout);

    if (nfds_ret > 0)
    {
      for (fd = 0; fd < max_fd + 1; ++fd)
      {
        /* Move actual results from select() back to the caller */
        if (readfds && FD_ISSET(fd, &r_new))
          FD_SET(fd, readfds);
        if (writefds && FD_ISSET(fd, &w_new))
          FD_SET(fd, writefds);
        if (exceptfds && FD_ISSET(fd, &e_new))
          FD_SET(fd, exceptfds);

        nfds_ret += n_ready_fds;
      }
    }
  }

  /* Account for regular file fds we set ready before */
  nfds_ret += n_ready_fds;

  return nfds_ret;
}
