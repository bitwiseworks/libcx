/*
 * Testcase for madvise.
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
#ifdef __OS2__
#include <io.h>
#endif
#include <sys/param.h>
#include <sys/mman.h>

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif

#define FILE_SIZE (PAGE_SIZE * 2)
#define FILE_VAL 1
#define TEST_SIZE 10
#define TEST_VAL 255

/* Shared mapping file flush interval in ms (must match one from mmap.c) */
#define FLUSH_DELAY 2000

unsigned char buf[FILE_SIZE];
unsigned char buf_chk[FILE_SIZE];
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
  int status;

  int fd = create_temp_file("tst-madvise-", &fname);
  if (fd == -1)
    {
      puts("create_temp_file failed");
      return 1;
    }

#ifdef __OS2__
  setmode(fd, O_BINARY);
#endif

  for (i = 0; i < FILE_SIZE; ++i)
    buf[i] = FILE_VAL;

  if ((n = write(fd, buf, FILE_SIZE)) != FILE_SIZE)
  {
    if (n != -1)
      printf("write failed (write %d bytes instead of %d)\n", n, FILE_SIZE);
    else
      perror("write failed");
    return 1;
  }

  // This is not supported for now, see README.md.
#if 0
  /*
   * Test 1: create shared mmap, modify underlying file, MADV_DONTNEED a page
   * then read it to check that it gets new file contents.
   */

  printf("Test 1\n");

  addr = mmap(NULL, FILE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (addr == MAP_FAILED)
  {
    perror("mmap failed");
    exit(-1);
  }

  for (i = 0; i < FILE_SIZE; ++i)
  {
    if (addr[i] != buf[i])
    {
      printf("addr[%d] is %u, must be %u\n", i, addr[i], buf[i]);
      return 1;
    }
  }

  for (i = 0; i < PAGE_SIZE; ++i)
    buf[i] = TEST_VAL;

  if (lseek(fd, PAGE_SIZE, SEEK_SET) == -1)
  {
    perror("lseek failed");
    return 1;
  }

  if ((n = write(fd, buf, PAGE_SIZE)) != PAGE_SIZE)
  {
    if (n != -1)
      printf("write failed (write %d bytes instead of %d)\n", n, PAGE_SIZE);
    else
      perror("write failed");
    return 1;
  }

  rc = madvise(addr + PAGE_SIZE, PAGE_SIZE, MADV_DONTNEED);
  if (rc == -1)
  {
    perror("madvise failed");
    return 1;
  }

  for (i = PAGE_SIZE; i < PAGE_SIZE * 2; ++i)
  {
    if (addr[i] != TEST_VAL)
    {
      printf("addr[%d] is %u, must be %u\n", i, addr[i], TEST_VAL);
      return 1;
    }
  }

  if (munmap(addr, FILE_SIZE) == -1)
  {
    perror("munmap failed");
    return 1;
  }

#endif

  /*
   * Test 2: create private mmap, modify underlying file, MADV_DONTNEED a page
   * then read it to check that it gets new file contents.
   */

  printf("Test 2\n");

  addr = mmap(NULL, FILE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
  if (addr == MAP_FAILED)
  {
    perror("mmap failed");
    exit(-1);
  }

  for (i = 0; i < FILE_SIZE; ++i)
  {
    if (addr[i] != buf[i])
    {
      printf("addr[%d] is %u, must be %u\n", i, addr[i], buf[i]);
      return 1;
    }
  }

  for (i = 0; i < PAGE_SIZE; ++i)
    buf[i] = TEST_VAL;

  if (lseek(fd, PAGE_SIZE, SEEK_SET) == -1)
  {
    perror("lseek failed");
    return 1;
  }

  if ((n = write(fd, buf, PAGE_SIZE)) != PAGE_SIZE)
  {
    if (n != -1)
      printf("write failed (write %d bytes instead of %d)\n", n, PAGE_SIZE);
    else
      perror("write failed");
    return 1;
  }

  rc = madvise(addr + PAGE_SIZE, PAGE_SIZE, MADV_DONTNEED);
  if (rc == -1)
  {
    perror("madvise failed");
    return 1;
  }

  for (i = PAGE_SIZE; i < PAGE_SIZE * 2; ++i)
  {
    if (addr[i] != TEST_VAL)
    {
      printf("addr[%d] is %u, must be %u\n", i, addr[i], TEST_VAL);
      return 1;
    }
  }

  if (munmap(addr, FILE_SIZE) == -1)
  {
    perror("munmap failed");
    return 1;
  }

  /*
   * Test 3: create private anonymous mapping, should read 0
   */

  printf("Test 3\n");

  addr = mmap(NULL, FILE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  if (addr == MAP_FAILED)
  {
    perror("mmap failed");
    exit(-1);
  }

  rc = madvise(addr + PAGE_SIZE, PAGE_SIZE, MADV_DONTNEED);
  if (rc == -1)
  {
    perror("madvise failed");
    return 1;
  }

  for (i = PAGE_SIZE; i < PAGE_SIZE * 2; ++i)
  {
    if (addr[i] != 0)
    {
      printf("addr[%d] is %u, must be %u\n", i, addr[i], 0);
      return 1;
    }
  }

  if (munmap(addr, FILE_SIZE) == -1)
  {
    perror("munmap failed");
    return 1;
  }

  return 0;
}
