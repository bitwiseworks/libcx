/*
 * Testcase for select.
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

static int do_test(void);
#define TEST_FUNCTION do_test()
#include "../test-skeleton.c"

static int do_test(void)
{
  int rc;
  int fd;

  fd = create_temp_file("tst-select-", NULL);
  if (fd == -1)
    return 1;

  fd_set rset;
  fd_set wset;
  fd_set eset;
  struct  timeval tm;

  /*
   * Test select sleep
   */

  tm.tv_sec = 0;
  tm.tv_usec = 300000;
  rc = select(0, NULL, NULL, NULL, &tm);
  if (rc == -1)
  {
    perror("select sleep failed");
    return 1;
  }

  /*
   * Test regular files
   */

  FD_ZERO (&rset);
  FD_ZERO (&wset);
  FD_ZERO (&eset);

  FD_SET(fd, &rset);

  rc = select(fd + 1, &rset, &wset, &eset, NULL);
  if (rc == -1)
  {
    perror("select regular file for reading failed");
    return 1;
  }
  if (rc != 1 || !FD_ISSET(fd, &rset))
  {
    printf("select regular file returned %d and FD_ISSET(reading) %d\n",
           rc, !!FD_ISSET(fd, &rset));
    return 1;
  }

  FD_ZERO (&rset);
  FD_ZERO (&wset);
  FD_ZERO (&eset);

  FD_SET(fd, &wset);

  rc = select(fd + 1, &rset, &wset, &eset, NULL);
  if (rc == -1)
  {
    perror("select regular file for writing failed");
    return 1;
  }
  if (rc != 1 || !FD_ISSET(fd, &wset))
  {
    printf("select regular file returned %d and FD_ISSET(writing) %d\n",
           rc, !!FD_ISSET(fd, &wset));
    return 1;
  }

  FD_ZERO (&rset);
  FD_ZERO (&wset);
  FD_ZERO (&eset);

  FD_SET(fd, &eset);

  rc = select(fd + 1, &rset, &wset, &eset, NULL);
  if (rc == -1)
  {
    perror("select regular file for exceptions failed");
    return 1;
  }
  if (rc != 1 || !FD_ISSET(fd, &eset))
  {
    printf("select regular file returned %d and FD_ISSET(exceptions) %d\n",
           rc, !!FD_ISSET(fd, &eset));
    return 1;
  }

  FD_ZERO (&rset);
  FD_ZERO (&wset);
  FD_ZERO (&eset);

  FD_SET(fd, &rset);
  FD_SET(fd, &wset);

  rc = select(fd + 1, &rset, &wset, &eset, NULL);
  if (rc == -1)
  {
    perror("select regular file for reading/writing failed");
    return 1;
  }
  if (rc != 2 || !FD_ISSET(fd, &rset) || !FD_ISSET(fd, &wset))
  {
    printf("select regular file returned %d and FD_ISSET(r) %d FD_ISSET(w) %d\n",
           rc, !!FD_ISSET(fd, &rset), !!FD_ISSET(fd, &wset));
    return 1;
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

  FD_ZERO (&rset);
  FD_ZERO (&wset);
  FD_ZERO (&eset);

  FD_SET (p[0], &rset);

  tm.tv_sec = 3;
  tm.tv_usec = 0;

  rc = select(p[0] + 1, &rset, &wset, &eset, &tm);
  if (rc == -1)
  {
    perror("select socket for reading failed");
    return 1;
  }
  if (rc != 1 || !FD_ISSET(p[0], &rset))
  {
    printf("select socket returned %d and FD_ISSET(r) %d\n",
           rc, !!FD_ISSET(p[0], &rset));
    return 1;
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
   * Test sockets
   */

  FD_ZERO (&rset);
  FD_ZERO (&wset);
  FD_ZERO (&eset);

  FD_SET (p[0], &rset);
  FD_SET (fd, &eset);

  tm.tv_sec = 3;
  tm.tv_usec = 0;

  rc = select(p[0] + 1, &rset, &wset, &eset, &tm);
  if (rc == -1)
  {
    perror("select socket/file failed");
    return 1;
  }
  if (rc != 1 || FD_ISSET(p[0], &rset) || !FD_ISSET(fd, &eset))
  {
    printf("select socket/file returned %d and FD_ISSET(socket) %d FD_ISSET(file) %d\n",
           rc, !!FD_ISSET(p[0], &rset), !!FD_ISSET(p[0], &eset));
    return 1;
  }

  return 0;
}
