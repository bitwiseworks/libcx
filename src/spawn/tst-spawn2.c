/*
 * Testcase for spawn2 API.
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

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "libcx/spawn2.h"

#include "../test-skeleton.c"

int wait_pid(const char *msg, int pid, int exit_code)
{
  int status;
  int rc = waitpid(pid, &status, 0);
  if (rc == -1)
    perrno_and(return 1, "%s: waitpid %x", msg, pid);
  if (!WIFEXITED(status) || WEXITSTATUS(status) != exit_code)
    perr_and(return 1, "%s: waitpid %x status %x (expected exit code %d, got %d)",
             msg, pid, status, exit_code, WEXITSTATUS(status));

  return 0;
}

int do_test(int argc, const char *const *argv)
{
  int rc;

  if (argc > 1)
  {
    // child process

    int n = atoi(argv[1]);

    switch (n)
    {
      case 5:
        return 123;

      case 6:
      {
        if (argc < 3)
          return 100;
        char *curdir = getcwd(NULL, 0);
        if (curdir == NULL)
          return 101;
        if (chdir(argv[2]) == -1)
          return 102;
        char *curdir2 = getcwd(NULL, 0);
        if (curdir2 == NULL)
          return 103;
        if (strcmp(curdir, curdir2) != 0)
          perr_and(return 1, "child: curdir: [%s] != [%s]", curdir, curdir2);
        free(curdir);
        free(curdir2);
        break;
      }

      case 7:
      {
        printf("CHILD TEST 7");
        break;
      }

      case 8:
      {
        fprintf(stderr, "CHILD TEST 8");
        break;
      }

      case 9:
      {
        fprintf(stderr, "CHILD TEST 9");
        break;
      }

      default:
        return 127;
    }

    return 0;
  }

  char exename[PATH_MAX];
  _execname(exename, PATH_MAX);

  // args check, should fail
  printf ("test 1\n");
  rc = spawn2(P_NOWAIT, NULL, NULL, NULL, NULL, NULL);
  if (rc != -1 || errno != EINVAL)
    perr_and(return 1, "test 1: rc %d, not 0; or errno %d, not EINVAL", rc, errno);

  // args check, should fail
  printf ("test 2\n");
  rc = spawn2(P_NOWAIT, argv[0], NULL, NULL, NULL, NULL);
  if (rc != -1 || errno != EINVAL)
    perr_and(return 1, "test 2: rc %d, not 0; or errno %d, not EINVAL", rc, errno);

  // args check, should fail
  printf ("test 3\n");
  {
    const char *args[] = { NULL };
    rc = spawn2(P_NOWAIT, "something|non|existent", args, NULL, NULL, NULL);
    if (rc != -1 || errno != EINVAL)
      perr_and(return 1, "test 3: rc %d, not 0; or errno %d, not EINVAL", rc, errno);
  }

  // program presence check, should fail
  printf ("test 4\n");
  {
    const char *args[] = { "blah", NULL };
    rc = spawn2(P_NOWAIT, "something|non|existent", args, NULL, NULL, NULL);
    if (rc != -1 || errno != ENOENT)
      perr_and(return 1, "test 4: rc %d, not 0; or errno %d, not ENOENT", rc, errno);
  }

  // simple spawn, should succeed
  printf ("test 5.1\n");
  {
    const char *args[] = { exename, "--direct", "5", NULL };
    int pid = spawn2(P_NOWAIT, exename, args, NULL, NULL, NULL);
    if (pid == -1)
      perrno_and(return 1, "test 5: spawn2");

    if (wait_pid("test 5.1", pid, 123))
      return 1;
  }

  // simple spawn, should fail due to P_WAIT
  printf ("test 5.2\n");
  {
    const char *args[] = { exename, "--direct", "5", NULL };
    int pid = spawn2(P_WAIT, exename, args, NULL, NULL, NULL);
    if (rc != -1 || errno != EINVAL)
      perr_and(return 1, "test 5.2: rc %d, not 0; or errno %d, not EINVAL", rc, errno);
  }

  // cwd check, should succeed
  printf ("test 6\n");
  {
    char *curdir = getcwd(NULL, 0);
    if (curdir == NULL)
      perrno_and(return 1, "test 6: getcwd");

    char *tmpdir = getenv("TEMP");
    if (tmpdir == NULL)
      tmpdir = getenv("TMPDIR");
    if (tmpdir == NULL)
      perr_and(return 1, "test 6: no TEMP/TMPDIR");

    const char *args[] = { exename, "--direct", "6", tmpdir, NULL };
    int pid = spawn2(P_NOWAIT, exename, args, tmpdir, NULL, NULL);
    if (pid == -1)
      perrno_and(return 1, "test 6: spawn2");

    if (wait_pid("test 6", pid, 0))
      return 1;

    char *curdir2 = getcwd(NULL, 0);
    if (strcmp(curdir, curdir2) != 0)
      perr_and(return 1, "test 6: curdir: [%s] != [%s]", curdir, curdir2);

    free(curdir2);
    free(curdir);
  }

  // stdout pipe check, should succeed
  printf ("test 7\n");
  {
    // forbid our own logging as we catch stdout
    char *logging = getenv("LIBCX_TRACE");
    if (logging)
      logging = strdup(logging);
    setenv("LIBCX_TRACE", "-all", 1);

    int p[2];
    rc = pipe(p);
    if (rc == -1)
      perrno_and(return 1, "test 7: pipe");

    int stdfds[3] = { 0, p[1], 0 };

    const char *args[] = { exename, "--direct", "7", NULL };
    int pid = spawn2(P_NOWAIT, exename, args, NULL, NULL, stdfds);
    if (pid == -1)
      perrno_and(return 1, "test 7: spawn2");

    rc = close(p[1]);
    if (rc == -1)
      perrno_and(return 1, "test 7: close");

    char buf[1024] = {0};
    rc = read(p[0], buf, sizeof(buf) - 1);
    if (rc == -1)
      perrno_and(return 1, "test 7: read");

    buf[rc] = 0;

    if (wait_pid("test 7", pid, 0))
      return 1;

    if (strcmp(buf, "CHILD TEST 7") != 0)
      perr_and(return 1, "test 7: expected [CHILD TEST 7] got [%s]", buf);

    close(p[0]);

    // restore logging
    if (logging)
    {
      setenv("LIBCX_TRACE", logging, 1);
      free(logging);
    }
    else
      unsetenv("LIBCX_TRACE");
  }

  // stderr pipe check, should succeed
  printf ("test 8\n");
  {
    int p[2];
    rc = pipe(p);
    if (rc == -1)
      perrno_and(return 1, "test 8: pipe");

    int stdfds[3] = { 0, 0, p[1] };

    const char *args[] = { exename, "--direct", "8", NULL };
    int pid = spawn2(P_NOWAIT, exename, args, NULL, NULL, stdfds);
    if (pid == -1)
      perrno_and(return 1, "test 8: spawn2");

    rc = close(p[1]);
    if (rc == -1)
      perrno_and(return 1, "test 8: close");

    char buf[1024] = {0};
    rc = read(p[0], buf, sizeof(buf) - 1);
    if (rc == -1)
      perrno_and(return 1, "test 8: read");

    buf[rc] = 0;

    if (wait_pid("test 8", pid, 0))
      return 1;

    if (strcmp(buf, "CHILD TEST 8") != 0)
      perr_and(return 1, "test 8: expected [CHILD TEST 8] got [%s]", buf);

    close(p[0]);
  }

  // stderr->stdout pipe check, should succeed
  printf ("test 9\n");
  {
    // forbid our own logging as we catch stdout
    char *logging = getenv("LIBCX_TRACE");
    if (logging)
      logging = strdup(logging);
    setenv("LIBCX_TRACE", "-all", 1);

    int p[2];
    rc = pipe(p);
    if (rc == -1)
      perrno_and(return 1, "test 9: pipe");

    int stdfds[3] = { 0, p[1], 1 };

    const char *args[] = { exename, "--direct", "9", NULL };
    int pid = spawn2(P_NOWAIT, exename, args, NULL, NULL, stdfds);
    if (pid == -1)
      perrno_and(return 1, "test 9: spawn2");

    rc = close(p[1]);
    if (rc == -1)
      perrno_and(return 1, "test 9: close");

    char buf[1024] = {0};
    rc = read(p[0], buf, sizeof(buf) - 1);
    if (rc == -1)
      perrno_and(return 1, "test 9: read");

    buf[rc] = 0;

    if (wait_pid("test 9", pid, 0))
      return 1;

    if (strcmp(buf, "CHILD TEST 9") != 0)
      perr_and(return 1, "test 9: expected [CHILD TEST 9] got [%s]", buf);

    close(p[0]);

    // restore logging
    if (logging)
    {
      setenv("LIBCX_TRACE", logging, 1);
      free(logging);
    }
    else
      unsetenv("LIBCX_TRACE");
  }

  return 0;
}