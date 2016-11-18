/*
 * Testcase for partial/multiple mmap/munmap.
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

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif

#define FILE_SIZE (PAGE_SIZE * 4)

unsigned char buf[FILE_SIZE];

static int do_test(void);
#define TEST_FUNCTION do_test ()
#include "../test-skeleton.c"

static int
do_test (void)
{
  int rc;
  int i, n, off;
  unsigned char *addr1, *addr2, *addr3;
  char *fname;

  int fd = create_temp_file("tst-mmap7-1-", &fname);
  if (fd == -1)
  {
    perror("create_temp_file failed");
    return 1;
  }

#ifdef __OS2__
  setmode (fd, O_BINARY);
#endif

  srand(getpid());

  for (i = 0; i < FILE_SIZE; ++i)
    buf[i] = rand() % 255;

  if (TEMP_FAILURE_RETRY((n = write(fd, buf, FILE_SIZE))) != FILE_SIZE)
  {
    if (n != -1)
      printf("write failed (write %d bytes instead of %d)\n", n, FILE_SIZE);
    else
      perror("write failed");
    return 1;
  }

  for (n = 1; n <= 4; ++n)
  {
    TEST_FORK_BEGIN("child", 0, n < 3 ? 0 : SIGSEGV);
    {
      TEST_FORK_PRINTF("Test loop %d\n", n);
      int flags = (n == 1 || n == 3) ? MAP_SHARED : MAP_PRIVATE;

      /*
       * Test 1: create several file mappings
       */

      TEST_FORK_PRINTF("Test 1.1\n");

      off = 0;
      addr1 = mmap(NULL, 10, PROT_READ | PROT_WRITE, flags, fd, off);
      if (addr1 == MAP_FAILED)
      {
        TEST_FORK_PERROR("mmap failed");
        return 1;
      }

      for (i = 0; i < PAGE_SIZE; ++i)
      {
        if (addr1[i - off] != buf[i])
        {
          TEST_FORK_PRINTF("addr1[%d] is %u, must be %u\n", i - off, addr1[i - off], buf[i]);
          return 1;
        }
      }

      TEST_FORK_PRINTF("Test 1.2\n");

      off = PAGE_SIZE * 2;
      addr2 = mmap(NULL, 20, PROT_READ | PROT_WRITE, flags, fd, off);
      if (addr2 == MAP_FAILED)
      {
        TEST_FORK_PERROR("mmap failed");
        return 1;
      }

      for (i = off; i < off + PAGE_SIZE; ++i)
      {
        if (addr2[i - off] != buf[i])
        {
          TEST_FORK_PRINTF("addr2[%d] is %u, must be %u\n", i - off, addr2[i - off], buf[i]);
          return 1;
        }
      }

      TEST_FORK_PRINTF("Test 1.3\n");

      off = PAGE_SIZE;
      addr3 = mmap(NULL, PAGE_SIZE * 2, PROT_READ | PROT_WRITE, flags, fd, off);
      if (addr3 == MAP_FAILED)
      {
        TEST_FORK_PERROR("mmap failed");
        return 1;
      }

      for (i = off; i < off + PAGE_SIZE; ++i)
      {
        if (addr3[i - off] != buf[i])
        {
          TEST_FORK_PRINTF("addr3[%d] is %u, must be %u\n", i - off, addr3[i - off], buf[i]);
          return 1;
        }
      }

      /*
       * Test 2: unmap several regions
       */

      TEST_FORK_PRINTF("Test 2\n");

      if (munmap(addr3, PAGE_SIZE) == -1)
      {
        TEST_FORK_PERROR("munmap failed");
        return 1;
      }

      if (munmap(addr1, FILE_SIZE) == -1)
      {
        TEST_FORK_PERROR("munmap failed");
        return 1;
      }

      if (n < 3)
        return 0;

      /* Test 3: write somewhere (should crash) */

      TEST_FORK_PRINTF("Test 3\n");
      *addr1 = 0;
    }
    TEST_FORK_END();
  }

  free(fname);

  return 0;
}
