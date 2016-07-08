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
#include <sys/socket.h>

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

  struct pollfd pfd[2];

  /*
   * Test regular files
   */

  pfd[0].fd = fd;
  pfd[0].events = POLLIN;
  pfd[0].revents = 0;

  rc = poll(pfd, 1, -1);
  if (rc == -1)
  {
    perror("poll regular file for POLLIN failed");
    exit(1);
  }
  if (rc != 1 || (pfd[0].revents & POLLIN) != POLLIN)
  {
    printf("poll regular file for POLLIN returned %d and revents %x\n", rc, pfd[0].revents);
    exit(1);
  }

  pfd[0].events = POLLOUT;

  rc = poll(pfd, 1, -1);
  if (rc == -1)
  {
    perror("poll regular file for POLLOUT failed");
    exit(1);
  }
  if (rc != 1 || (pfd[0].revents & POLLOUT) != POLLOUT)
  {
    printf("poll regular file for POLLOUT returned %d revents %x\n", rc, pfd[0].revents);
    exit(1);
  }

  pfd[0].events = POLLPRI;

  rc = poll(pfd, 1, -1);
  if (rc == -1)
  {
    printf("poll regular file for POLLPRI failed\n");
    exit(1);
  }
  if (rc != 1 || (pfd[0].revents & POLLPRI) != POLLPRI)
  {
    printf("poll regular file for POLLPRI returned %d revents %x\n", rc, pfd[0].revents);
    exit(1);
  }

  /*
   * Test sockets
   */

  int p[2];

  rc = socketpair(AF_LOCAL, SOCK_STREAM, 0, p);
  if (rc == -1)
  {
    perror("socketpair failed");
    return 1;
  }

  pid_t pid = fork ();
  if (pid == -1)
  {
    perror("fork failed");
    return 1;
  }

  if (pid == 0)
  {
    /* Child */

    rc = write(p[1], "Hello", 6);
    if (rc == -1)
    {
      perror("child: write failed");
      exit(1);
    }

    sleep(1);
    exit(0);
  }

  /* Parent */

  pfd[0].fd = p[0];
  pfd[0].events = POLLIN;
  pfd[0].revents = 0;

  rc = poll(pfd, 1, 3000);
  if (rc == -1)
  {
    printf("poll socket for POLLIN failed\n");
    exit(1);
  }
  if (rc != 1 || (pfd[0].revents & POLLIN) != POLLIN)
  {
    printf("poll socket for POLLIN returned %d and revents %x\n", rc, pfd[0].revents);
    exit(1);
  }
  else
  {
     char buf[6] = {0};
     rc = read(p[0], buf, 6);
     if (rc != 6 || strncmp(buf, "Hello", 6))
     {
       printf("read after select failed or wrong, rc %d, %*.*s\n",
              rc, 6, 6, buf);
       exit(1);
     }
  }

  int status;
  if (TEMP_FAILURE_RETRY(waitpid(pid, &status, 0)) != pid)
  {
    perror("waitpid failed");
    return 1;
  }

  if (!WIFEXITED(status) || WEXITSTATUS(status))
  {
    puts("child 1 terminated abnormally or with error");
    return 1;
  }

  /*
   * Test sockets & files
   */

  pfd[1].fd = fd;
  pfd[1].events = POLLPRI;
  pfd[1].revents = 0;

  rc = poll(pfd, 2, 3000);
  if (rc == -1)
  {
    printf("poll socket/file for POLLIN/POLLPRI failed\n");
    exit(1);
  }
  if (rc != 1 || pfd[0].revents || (pfd[1].revents & POLLPRI) != POLLPRI)
  {
    printf("poll socket for POLLIN returned %d and socket.revents %x file.revents %d\n",
           rc, pfd[0].revents, pfd[1].revents);
    exit(1);
  }

  return 0;
}
