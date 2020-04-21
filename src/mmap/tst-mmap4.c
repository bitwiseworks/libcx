/*
 * Testcase for beyond EOF mmap.
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

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/param.h>
#include <sys/mman.h>
#include <sys/stat.h>

#ifdef __OS2__
#include <io.h>
#endif

/*
 * @todo For now we can't access beyond EOF for more than within
 * the last page. See https://github.com/bitwiseworks/libcx/issues/20
 * for details.
 */
#define MAP_SIZE 4096

static int do_test(void);
#define TEST_FUNCTION do_test ()
#include "../test-skeleton.c"

static int
do_test (void)
{
  int rc;
  int n;
  unsigned char *addr;
  char *fname;
  struct stat st;
  char ch;

  int fd = create_temp_file("tst-mmap4-", &fname);
  if (fd == -1)
  {
    perror("create_temp_file failed");
    return 1;
  }

#ifdef __OS2__
  setmode (fd, O_BINARY);
#endif

  if (TEMP_FAILURE_RETRY((n = write(fd, "1", 1))) != 1)
  {
    if (n != -1)
      printf("write failed (write %d bytes instead of %d)\n", n, 1);
    else
      perror("write failed");
    return 1;
  }

  /*
   * Test 1: shared mmap read beyond EOF (should return 0)
   */

  printf("Test 1\n");

  addr = mmap(NULL, MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (addr == MAP_FAILED)
  {
    perror("mmap failed");
    return 1;
  }

  close(fd);

  if (addr[0] != '1')
  {
    printf("addr[0] is %u, must be %u\n", addr[0], '1');
    return 1;
  }

  if (addr[1] != 0)
  {
    printf("addr[1] is %u, must be %u\n", addr[1], 0);
    return 1;
  }

  if (addr[MAP_SIZE - 1] != 0)
  {
    printf("addr[MAP_SIZE-1] is %u, must be %u\n", addr[MAP_SIZE - 1], 0);
    return 1;
  }

  /*
   * Test 2: shared mmap write beyond EOF (should not be synced back to file)
   */

  printf("Test 2\n");

  addr[0] = '2'; /* this is just to cause a flush for tracing */

  addr[MAP_SIZE - 1] = '2';

  if (addr[MAP_SIZE - 1] != '2')
  {
    printf("addr[MAP_SIZE-1] is %u, must be %u\n", addr[MAP_SIZE - 1], '2');
    return 1;
  }

  if (munmap(addr, MAP_SIZE) == -1)
  {
    perror("munmap failed");
    return 1;
  }

  fd = open(fname, O_RDONLY);
  if (fd == -1)
  {
    perror("open failed");
    return 1;
  }

#ifdef __OS2__
  setmode (fd, O_BINARY);
#endif

  if (fstat(fd, &st))
  {
    perror("fstat failed");
    return 1;
  }

  if (st.st_size != 1)
  {
    printf("file size is %lld, must be %d\n", (uint64_t) st.st_size, 1);
    return 1;
  }

  if (TEMP_FAILURE_RETRY((n = read(fd, &ch, 1))) != 1)
  {
    if (n != -1)
      printf("read failed (read %d bytes instead of %d)\n", n, 1);
    else
      perror("read failed");
    return 1;
  }

  if (st.st_size != 1)
  {
    printf("ch is %u, must be %u\n", ch, '2');
    return 1;
  }

  /*
   * Test 3: shared mmap read far beyond EOF (should crash)
   */

  printf("Test 3\n");

#ifdef __OS2__
  TEST_FORK_BEGIN("child", 0, SIGSEGV);
#else
  TEST_FORK_BEGIN("child", 0, SIGBUS);
#endif
  {
    addr = mmap(NULL, MAP_SIZE * 2, PROT_READ, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED)
    {
      perror("mmap failed");
      return 1;
    }

    TEST_FORK_PRINTF("%d\n", addr[MAP_SIZE]);
  }
  TEST_FORK_END();

  close(fd);

  free(fname);

  return 0;
}
