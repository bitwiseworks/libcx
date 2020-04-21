/*
 * Testcase for overlapped mappings of the same file.
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

#ifdef __OS2__
#include <io.h>
#endif

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif

#define FILE_SIZE (PAGE_SIZE * 4)
#define TEST_SIZE 10
#define TEST_VAL 255

unsigned char buf[FILE_SIZE];

static int do_test(void);
#define TEST_FUNCTION do_test ()
#include "../test-skeleton.c"

static int
do_test (void)
{
  int rc;
  int i, n;
  unsigned char *addr1, *addr2;
  char *fname;

  int fd = create_temp_file("tst-mmap8-", &fname);
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

  /*
   * Test 1: given these pages [0][1][2][3], first map 2-3, then map 1-2,
   * then unmap 2-3, then access page 2. It must work.
   */

  printf("Test 1\n");

  addr1 = mmap(NULL, PAGE_SIZE * 2, PROT_READ, MAP_SHARED, fd, PAGE_SIZE * 2);
  if (addr1 == MAP_FAILED)
  {
    perror("mmap failed");
    return 1;
  }

  printf("addr1 = %p\n", addr1);

  addr2 = mmap(NULL, PAGE_SIZE * 2, PROT_READ, MAP_SHARED, fd, PAGE_SIZE);
  if (addr2 == MAP_FAILED)
  {
    perror("mmap failed");
    return 1;
  }

  printf("addr2 = %p\n", addr2);

  if (munmap(addr1, PAGE_SIZE * 2) == -1)
  {
    perror("munmap failed");
    return 1;
  }

  printf("addr2[0] = %hhu\n", addr2[0]);
  printf("addr2[PAGE_SIZE] = %hhu\n", addr2[PAGE_SIZE]);

  if (munmap(addr2, PAGE_SIZE * 2) == -1)
  {
    perror("munmap failed");
    return 1;
  }

  free(fname);

  return 0;
}
