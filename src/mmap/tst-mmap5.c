/*
 * Testcase for mmap + pread.
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

#define FILE_SIZE (PAGE_SIZE * 2)
#define TEST_SIZE 10
#define TEST_VAL 255

unsigned char buf[FILE_SIZE];
unsigned char buf2[TEST_SIZE * 2];

static int do_test(void);
#define TEST_FUNCTION do_test ()
#include "../test-skeleton.c"

static int
do_test (void)
{
  int rc;
  int i, n;
  unsigned char *addr;
  char *fname;

  int fd = create_temp_file("tst-mmap5-1-", &fname);
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

  int fd2 = create_temp_file("tst-mmap5-2-", NULL);
  if (fd2 == -1)
  {
    perror("create_temp_file failed");
    return 1;
  }

#ifdef __OS2__
  setmode (fd2, O_BINARY);
#endif

  rc = ftruncate(fd2, TEST_SIZE);
  if (rc == -1)
  {
    perror("ftruncate failed");
    return 1;
  }

  /*
   * Test 1: simple mmap
   */

  printf("Test 1\n");

  addr = mmap(NULL, TEST_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd2, 0);
  if (addr == MAP_FAILED)
  {
    perror("mmap failed");
    return 1;
  }

  rc = pread(fd, addr, TEST_SIZE, 0);
  if (rc == -1)
  {
    perror("pread failed");
    return 1;
  }

  for (i = 0; i < TEST_SIZE; ++i)
  {
    if (addr[i] != buf[i])
    {
      printf("addr[%d] is %u, must be %u\n", i, addr[i], buf[i]);
      return 1;
    }
  }

  if (munmap(addr, FILE_SIZE) == -1)
  {
    perror("munmap failed");
    return 1;
  }

  free(fname);

  return 0;
}
