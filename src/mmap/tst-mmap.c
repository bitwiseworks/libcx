/*
 * Testcase for shared mmap.
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
  unsigned char *addr, *addr2;
  char *fname;
  int status;

  int fd = create_temp_file("tst-mmap-", &fname);
  if (fd == -1)
    {
      puts("create_temp_file failed");
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
   * Test 1: simple mmap
   */

  printf("Test 1\n");

  addr = mmap(NULL, FILE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (addr == MAP_FAILED)
  {
    perror("mmap failed");
    exit(-1);
  }

  addr2 = mmap(NULL, FILE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (addr2 == MAP_FAILED)
  {
    perror("mmap 2 failed");
    exit(-1);
  }

  for (i = PAGE_SIZE - TEST_SIZE; i < PAGE_SIZE + TEST_SIZE; ++i)
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

  if (munmap(addr2, FILE_SIZE) == -1)
  {
    perror("munmap 2 failed");
    return 1;
  }

  /*
   * Test 2: mmap at offset
   */

  printf("Test 2\n");

  enum { Offset = PAGE_SIZE };

  addr = mmap(NULL, FILE_SIZE - Offset, PROT_READ | PROT_WRITE, MAP_SHARED, fd, Offset);
  if (addr == MAP_FAILED)
  {
    perror("mmap failed");
    exit(-1);
  }

  for (i = PAGE_SIZE - TEST_SIZE; i < PAGE_SIZE + TEST_SIZE; ++i)
  {
    if (addr[i] != buf[i + Offset])
    {
      printf("addr[%d] is %u, must be %u\n", i, addr[i], buf[i + Offset]);
      return 1;
    }
  }

  /*
   * Test 3: modify mmap (should change the underlying file and be visible
   * in children)
   */

  printf("Test 3\n");

  switch (fork())
  {
    case -1:
      perror("fork failed");
      exit(-1);

    case 0:

      for (i = PAGE_SIZE - TEST_SIZE; i < PAGE_SIZE + TEST_SIZE; ++i)
      {
        if (addr[i] != buf[i + Offset])
        {
          printf("child: addr[%d] is %u, must be %u\n", i, addr[i], buf[i + Offset]);
          return 1;
        }
      }

      for (i = PAGE_SIZE - TEST_SIZE; i < PAGE_SIZE + TEST_SIZE; ++i)
        addr[i] = TEST_VAL;

      if (munmap(addr, FILE_SIZE - Offset) == -1)
      {
        perror("child: munmap failed");
        return 1;
      }

      forget_temp_file (fname, fd);

      return 0;
  }

  if (wait(&status) == -1)
  {
    perror("wait failed");
    return 1;
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status))
  {
    printf("child crashed or returned non-zero (status %x)\n", status);
    return 1;
  }

  for (i = PAGE_SIZE - TEST_SIZE; i < PAGE_SIZE + TEST_SIZE; ++i)
  {
    if (addr[i] != TEST_VAL)
      printf("addr[%d] is %u, must be %u\n", i, addr[i], TEST_VAL);
  }

  if (munmap(addr, FILE_SIZE - Offset) == -1)
  {
    perror("munmap failed");
    return 1;
  }

  close(fd);

  fd = open(fname, O_RDONLY);
  if (fd == -1)
  {
    perror("open failed");
    return 1;
  }

  if (lseek(fd, Offset + PAGE_SIZE - TEST_SIZE, SEEK_SET) == -1)
  {
    perror("lseek failed");
    return 1;
  }

  if (TEMP_FAILURE_RETRY((n = read(fd, buf2, TEST_SIZE * 2))) != TEST_SIZE * 2)
  {
    if (n != -1)
      printf("read failed (read %d bytes instead of %d)\n", n, TEST_SIZE * 2);
    else
      perror("read failed");
    return 1;
  }

  for (i = 0; i < TEST_SIZE * 2; ++i)
  {
    if (buf2[i] != TEST_VAL)
    {
      printf("buf2[%d] is %u, must be %u\n", i, buf2[i], TEST_VAL);
      return 1;
    }
  }

  close(fd);

  free(fname);

  return 0;
}
