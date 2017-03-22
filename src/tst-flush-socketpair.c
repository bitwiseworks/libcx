/*
 * Testcase for flushing streams bound to TCP sockets.
 * Copyright (C) 2016 bww bitwise works GmbH.
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

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <process.h>
#include <sys/socket.h>
#include <sys/fcntl.h>
#include <errno.h>

#define USE_POLL
#ifdef USE_POLL
#include <sys/poll.h>
#else
#include <sys/select.h>
#endif

const char child_str[] = "I'm a real child, pid 0x%04x\n";

int main_child()
{
  printf(child_str, getpid());

  return 0;
}

int main_parent(int argc, const char** argv)
{
  int p[2];
  int pid, status;
  ssize_t n, cnt;
#ifdef USE_POLL
  struct pollfd pfd;
#else
  fd_set readfds;
#endif
  char buf[128];

  if (socketpair(AF_UNIX, SOCK_STREAM, 0, p) < 0)
  {
    perror("socketpair");
    return 1;
  }

  pid = fork();
  if (!pid)
  {
    // child

    dup2(p[1], 1);
    close(p[0]);
    close(p[1]);

    execlp(argv[0], argv[0], "child", NULL);
    perror("exec");
    return 1;
  }

  // parent
  if (pid < 0)
  {
    perror("fork");
    return 1;
  }

  close(p[1]);

  printf("child pid 0x%x\n", pid);

  cnt = 0;

#ifdef USE_POLL
  pfd.fd = p[0];
  pfd.events = POLLIN;

  while (1)
  {
    if (poll(&pfd, 1, -1) < 0)
    {
      if (errno == EINTR)
        continue;
      perror("poll");
      break;
    }

    if (!(pfd.revents & (POLLOUT|POLLIN|POLLHUP|POLLERR|POLLNVAL)))
      continue;

    n = read(pfd.fd, buf, sizeof(buf));
    if (n <= 0)
    {
      close(pfd.fd);
      if (n < 0)
        perror("read");
      break;
    }
    else
    {
      cnt += n;
      printf("read %d bytes from child [%.*s]\n", n, n, buf);
    }
  }
#else
  while (1)
  {
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(p[0], &readfds);

    if (select(p[0] + 1, &readfds, NULL, NULL, NULL) < 0)
    {
      if (errno == EINTR)
        continue;
      perror("select");
      break;
    }

    if (!FD_ISSET(p[0], &readfds))
      continue;

    n = read(p[0], buf, sizeof(buf));
    if (n <= 0)
    {
      close(p[0]);
      if (n < 0)
        perror("read");
      break;
    }
    else
    {
      cnt += n;
      printf("read %d bytes from child [%.*s]\n", n, n, buf);
    }
  }
#endif

  while ((pid = waitpid(-1, &status, 0)) < 0)
  {
    if (errno == EINTR)
      continue;
    else
    {
      perror("waitpid");
      break;
    }
  }

  printf("child exit status 0x%x (pid 0x%x)\n", status, pid);

  if (cnt != (ssize_t)strlen(child_str))
  {
    printf("FAILED (read %d bytes from child instead of %d)\n", cnt, strlen(child_str));
    return 1;
  }

  printf("SUCCEEDED\n");
  return 0;
}

int main(int argc, const char** argv)
{
  if (argc > 1 && strcmp(argv[1], "child") == 0)
    return main_child();
  return main_parent(argc, argv);
}
