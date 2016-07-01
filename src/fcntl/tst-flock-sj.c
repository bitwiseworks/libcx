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

static void check_lock_fn(int fd, off_t start, off_t len, short type, int line)
{
  struct flock fl;

  /* This uses the LIBCx-specific extension of passing -1 in l_type to request
   * the existing lock at the given position -- this extension is intended to be
   * used only for the purposes of this test case */
  fl.l_type = -1;
  fl.l_start = start;
  fl.l_len = 1;

  if (TEMP_FAILURE_RETRY(fcntl(fd, F_GETLK, &fl)) != 0)
  {
    perror("fcntl(F_GETLK) failed");
    exit(1);
  }

  if (fl.l_start != start)
  {
    printf("line #%d: start: expected %lld, got %lld\n", line, start, fl.l_start);
    printf("(region %lld:%lld, type %s)\n", fl.l_start, fl.l_len, fl_type_str(fl.l_type));
    exit(1);
  }

  if (fl.l_len != len)
  {
    printf("line #%d: len: expected %lld, got %lld\n", line, len, fl.l_len);
    printf("(region %lld:%lld, type %s)\n", fl.l_start, fl.l_len, fl_type_str(fl.l_type));
    exit(1);
  }

  if (fl.l_type != type)
  {
    printf("line #%d: type: expected %s, got %s\n", line, fl_type_str(type), fl_type_str(fl.l_type));
    printf("(region %lld:%lld, type %s)\n", fl.l_start, fl.l_len, fl_type_str(fl.l_type));
    exit(1);
  }
}

#define check_lock(fd, start, len, type) check_lock_fn(fd, start, len, type, __LINE__)

static int do_test(void)
{
  int fd;
  struct flock fl;

  fd = create_temp_file("tst-flock-sj-", NULL);
  if (fd == -1)
    return 1;

  /* split:
   * ...............
   *      WWWWW
   * .....WWWWW.....
   */
  set_lock(fd, 10, 5, F_WRLCK);
  check_lock(fd, 0, 10, F_UNLCK);
  check_lock(fd, 10, 5, F_WRLCK);
  check_lock(fd, 15, 0, F_UNLCK);

  /* downgrade:
   * .....WWWWW.....
   *      RRRRR
   * .....RRRRR.....
   */
  set_lock(fd, 10, 5, F_RDLCK);
  check_lock(fd, 0, 10, F_UNLCK);
  check_lock(fd, 10, 5, F_RDLCK);
  check_lock(fd, 15, 0, F_UNLCK);

  /* simple lock remove */
  set_lock(fd, 10, 5, F_UNLCK);
  check_lock(fd, 0, 0, F_UNLCK);

  /* split:
   * .....WWWWW.....
   *      RRR
   * .....RRRWW.....
   */
  set_lock(fd, 10, 5, F_WRLCK);
  set_lock(fd, 10, 3, F_RDLCK);
  check_lock(fd, 0, 10, F_UNLCK);
  check_lock(fd, 10, 3, F_RDLCK);
  check_lock(fd, 13, 2, F_WRLCK);
  check_lock(fd, 15, 0, F_UNLCK);

  /* reset */
  set_lock(fd, 0, 0, F_UNLCK);
  check_lock(fd, 0, 0, F_UNLCK);

  /* split:
   * .....WWWWW.....
   *        RRR
   * .....WWRRR.....
   */
  set_lock(fd, 10, 5, F_WRLCK);
  set_lock(fd, 12, 3, F_RDLCK);
  check_lock(fd, 0, 10, F_UNLCK);
  check_lock(fd, 10, 2, F_WRLCK);
  check_lock(fd, 12, 3, F_RDLCK);
  check_lock(fd, 15, 0, F_UNLCK);

  /* reset */
  set_lock(fd, 0, 0, F_UNLCK);
  check_lock(fd, 0, 0, F_UNLCK);

  /* split:
   * .....WWWWW.....
   *       RRR
   * .....WRRRW.....
   */
  set_lock(fd, 10, 5, F_WRLCK);
  set_lock(fd, 11, 3, F_RDLCK);
  check_lock(fd, 0, 10, F_UNLCK);
  check_lock(fd, 10, 1, F_WRLCK);
  check_lock(fd, 11, 3, F_RDLCK);
  check_lock(fd, 14, 1, F_WRLCK);
  check_lock(fd, 15, 0, F_UNLCK);

  /* reset */
  set_lock(fd, 0, 0, F_UNLCK);
  check_lock(fd, 0, 0, F_UNLCK);

  /* split:
   * .....WWWWW.....
   *          RRR
   * .....WWWWRRR...
   */
  set_lock(fd, 10, 5, F_WRLCK);
  set_lock(fd, 14, 3, F_RDLCK);
  check_lock(fd, 0, 10, F_UNLCK);
  check_lock(fd, 10, 4, F_WRLCK);
  check_lock(fd, 14, 3, F_RDLCK);
  check_lock(fd, 17, 0, F_UNLCK);

  /* reset */
  set_lock(fd, 0, 0, F_UNLCK);
  check_lock(fd, 0, 0, F_UNLCK);

  /* join:
   * .....RRRWW.....
   *         RR
   * .....RRRRR.....
   */
  set_lock(fd, 10, 3, F_RDLCK);
  set_lock(fd, 13, 2, F_WRLCK);
  set_lock(fd, 13, 2, F_RDLCK);
  check_lock(fd, 0, 10, F_UNLCK);
  check_lock(fd, 10, 5, F_RDLCK);
  check_lock(fd, 15, 0, F_UNLCK);

  /* join:
   * .....RRRRR.....
   *       ...
   *      ..
   *          .
   * ...............
   */
  set_lock(fd, 11, 3, F_UNLCK);
  set_lock(fd, 10, 2, F_UNLCK);
  check_lock(fd, 0, 14, F_UNLCK);
  check_lock(fd, 14, 1, F_RDLCK);
  check_lock(fd, 15, 0, F_UNLCK);
  set_lock(fd, 14, 1, F_UNLCK);
  check_lock(fd, 0, 0, F_UNLCK);

  /* join:
   * .....R.R.R.....
   *     WWWWWWW
   * ....WWWWWWW....
   */
  set_lock(fd, 10, 1, F_RDLCK);
  set_lock(fd, 12, 1, F_RDLCK);
  set_lock(fd, 14, 1, F_RDLCK);
  set_lock(fd, 9, 7, F_WRLCK);
  check_lock(fd, 0, 9, F_UNLCK);
  check_lock(fd, 9, 7, F_WRLCK);
  check_lock(fd, 16, 0, F_UNLCK);

  /* @todo Add another process for 'r' test cases */

  return 0;
}
