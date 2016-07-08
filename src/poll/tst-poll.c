/*
 * Testcase for poll.
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
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "poll.h"

static int do_test(void);
#define TEST_FUNCTION do_test()
#include "../test-skeleton.c"

static int do_test(void)
{
  int rc;
  int fd;

  fd = create_temp_file("tst-poll-", NULL);
  if (fd == -1)
    return 1;

  struct pollfd pfd;

  /* Test regular files */

  pfd.fd = fd;
  pfd.events = POLLIN;
  pfd.revents = 0;

  rc = poll(&pfd, 1, -1);
  if (rc == -1)
  {
    perror("polling regular file for POLLIN failed");
    exit(1);
  }

  pfd.events = POLLOUT;

  rc = poll(&pfd, 1, -1);
  if (rc == -1)
  {
    perror("polling regular file for POLLOUT failed");
    exit(1);
  }

  pfd.events = POLLPRI;

  rc = poll(&pfd, 1, -1);
  if (rc == -1 )
  {
    printf("polling regular file for POLLPRI failed\n");
    exit(1);
  }

  return 0;
}
