/*
 * Testcase for fcntl advisory locking split/join logic.
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

static const char *fl_type_str(short type)
{
  return type == F_WRLCK ? "F_WRLCK" :
         type == F_RDLCK ? "F_RDLCK" :
         type == F_UNLCK ? "F_UNLCK" : "???";
}

static void set_lock(int fd, off_t start, off_t len, short type)
{
  struct flock fl;
  fl.l_start = start;
  fl.l_len = len;
  fl.l_type = type;
  fl.l_whence  = SEEK_SET;

  printf("Locking fd %d, region %lld:%lld, type %s\n", fd, start, len, fl_type_str(type));

  if (TEMP_FAILURE_RETRY(fcntl(fd, F_SETLKW, &fl)) != 0)
  {
    perror("fcntl(F_SETLKW) failed");
    exit(1);
  }
}

static void check_lock(int fd, off_t pos, off_t start, off_t len, short type)
{
  struct flock fl;

  /* This uses the LIBCx-specific extension of passing -1 in l_type to request
   * the existing lock at the given position -- this extension is intended to be
   * used only for the purposes of this test case */
  fl.l_type = -1;
  fl.l_start = pos;
  fl.l_len = 1;

  if (TEMP_FAILURE_RETRY(fcntl(fd, F_GETLK, &fl)) != 0)
  {
    perror("fcntl(F_GETLK) failed");
    exit(1);
  }

  if (fl.l_start != start)
  {
    printf("start: expected %lld, got %lld\n", start, fl.l_start);
    printf("(region %lld:%lld, type %s)\n", fl.l_start, fl.l_len, fl_type_str(fl.l_type));
    exit(1);
  }

  if (fl.l_len != len)
  {
    printf("len: expected %lld, got %lld\n", len, fl.l_len);
    printf("(region %lld:%lld, type %s)\n", fl.l_start, fl.l_len, fl_type_str(fl.l_type));
    exit(1);
  }

  if (fl.l_type != type)
  {
    printf("type: expected %s, got %s\n", fl_type_str(type), fl_type_str(fl.l_type));
    printf("(region %lld:%lld, type %s)\n", fl.l_start, fl.l_len, fl_type_str(fl.l_type));
    exit(1);
  }
}

static int do_test(void)
{
  int fd;
  struct flock fl;

  fd = create_temp_file("tst-flock-sj-", NULL);
  if (fd == -1)
    return 1;

  /* simple W lock */
  set_lock(fd, 10, 5, F_WRLCK);
  check_lock(fd, 0, 0, 10, F_UNLCK);
  check_lock(fd, 10, 10, 5, F_WRLCK);
  check_lock(fd, 15, 15, 0, F_UNLCK);

  /* simple W lock downgrade to R */
  set_lock(fd, 10, 5, F_RDLCK);
  check_lock(fd, 0, 0, 10, F_UNLCK);
  check_lock(fd, 10, 10, 5, F_RDLCK);
  check_lock(fd, 15, 15, 0, F_UNLCK);

  /* simple lock remove */
  set_lock(fd, 10, 5, F_UNLCK);
  check_lock(fd, 0, 0, 0, F_UNLCK);

  return 0;
}
