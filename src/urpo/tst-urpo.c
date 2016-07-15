/*
 * Testcase for URPO functionality.
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

static int do_test(void);
#define TEST_FUNCTION do_test()
#include "../test-skeleton.c"

static int do_test(void)
{
  int rc;
  int fd;
  int status;

  char tmp[PATH_MAX + 32];

  if (getenv("TMP"))
    strncpy(tmp, getenv("TMP"), PATH_MAX);
  else if (getenv("TEMP"))
    strncpy(tmp, getenv("TEMP"), PATH_MAX);
  else
    strcpy(tmp, "/tmp");

  char *tmp_mid = tmp + strlen(tmp);

  /*
   * Test unlink
   */

  strcpy(tmp_mid, "/tst-urpo-1-XXXXXX");
  fd = mkstemp(tmp);
  if (fd == -1)
  {
    perror("mkstemp failed");
    return 1;
  }

  pid_t pid = fork();
  if (pid == 0)
  {
    /* Child */

    printf("%s\n", tmp);

    int rc = unlink(tmp);
    if (rc == -1)
    {
      perror("child 1 unlink failed");
      exit(1);
    }

    exit(0);
  }

  /* Parent */

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

  return 0;
}
