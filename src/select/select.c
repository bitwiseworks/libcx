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

#define TRACE_GROUP TRACE_GROUP_SELECT
#include "../shared.h"

#ifndef MAX
#define MAX(a,b)	((a) > (b) ? (a) : (b))
#endif

int select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
           struct timeval *timeout)
{
  TRACE("nfds %d, readfds %p, writefds %p, exceptfds %p timeout %p (%ld.%ld)\n",
        nfds, readfds, writefds, exceptfds, timeout,
        timeout ? timeout->tv_sec : 0, timeout ? timeout->tv_usec : 0);

  int fd;
  int n_ready_fds;
  int nfds_ret;
  struct stat st;

  fd_set regular_fds;

  fd_set r_new;
  fd_set w_new;
  fd_set e_new;
  int max_fd;

  FD_ZERO (&regular_fds);

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
        TRACE("fd %d is regular file\n", fd);
        FD_CLR(fd, &r_new);
        FD_CLR(fd, &w_new);
        FD_CLR(fd, &e_new);
        n_ready_fds += n_fd_set;
        /* Remember regular fd for later */
        FD_SET(fd, &regular_fds);
      }
      else
      {
        /*
         * Note that while it seems that we could reset this non-regular fd in
         * the original sets since we copied it to the new sets (in order to
         * simply set it back if it's ready after the actual select), this can't
         * be done — _std_select may fail and many apps expect that the sets
         * remain unchanged in such a case (e.g. in case of EINTR many apps will
         * simply call select again with the same arguments and if we reset some
         * fds here these apps may not get needed notifications). Instead, we
         * remember regular fds in regular_fds above and then reset not ready
         * non-regular ones after _std_select returns success.
         */
        max_fd = MAX(max_fd, fd);
      }
    }
  }

  TRACE("n_ready_fds %d, max_fd %d\n", n_ready_fds, max_fd);

  if (max_fd == -1 && n_ready_fds)
  {
    /* There are only regular files, no need to call select. */
    nfds_ret = n_ready_fds;
  }
  else
  {
    struct timeval t_new;

    if (n_ready_fds)
    {
      /*
       * Regular files must end select immediately but we want to
       * check for other descriptors too, so use zero wait time.
       */
      t_new.tv_sec = 0;
      t_new.tv_usec = 0;
      timeout = &t_new;
    }

    TRACE("calling LIBC select: nfds %d, readfds %p, writefds %p, exceptfds %p timeout %p (%ld.%ld)\n",
          max_fd + 1,
          readfds ? &r_new : NULL, writefds ? &w_new : NULL, exceptfds ? &e_new : NULL,
          timeout, timeout ? timeout->tv_sec : 0, timeout ? timeout->tv_usec : 0);

    nfds_ret = _std_select(max_fd + 1,
                           readfds ? &r_new : NULL,
                           writefds ? &w_new : NULL,
                           exceptfds ? &e_new : NULL, timeout);

    TRACE("nfds_ret %d (%s)\n", nfds_ret, strerror(nfds_ret == -1 ? errno : 0));

    if (nfds_ret >= 0)
    {
      /*
       * Copy actual results from select() back to the caller. Note that we
       * can't use a bulk copy op as this will overwrite the ready state of
       * regular fds if there are any. Instead, we set/clear individual bits
       * for all non-regular ones.
       */
      for (fd = 0; fd <= max_fd; ++fd)
      {
        if (!FD_ISSET(fd, &regular_fds))
        {
          if (readfds)
          {
            if (FD_ISSET(fd, &r_new))
              FD_SET(fd, readfds);
            else
              FD_CLR(fd, readfds);
          }
          if (writefds)
          {
            if (FD_ISSET(fd, &w_new))
              FD_SET(fd, writefds);
            else
              FD_CLR(fd, writefds);
          }
          if (exceptfds)
          {
            if (FD_ISSET(fd, &e_new))
              FD_SET(fd, exceptfds);
            else
              FD_CLR(fd, exceptfds);
          }
        }
      }

      /* Account for regular file fds we set ready before */
      nfds_ret += n_ready_fds;
    }
  }

  TRACE("nfds_ret %d (%s)\n", nfds_ret, strerror(nfds_ret == -1 ? errno : 0));

  return nfds_ret;
}
