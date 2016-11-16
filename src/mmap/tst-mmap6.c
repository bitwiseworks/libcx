/*
 * Testcase for two shared mmaps (samba).
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

/*
 * Inspired by http://trac.netlabs.org/samba/browser/trunk/server/lib/replace/test/shared_mmap.c
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
#define TEST_VAL 255
#define TEST_OFF1 0x123
#define TEST_OFF2 (PAGE_SIZE + TEST_OFF1)

unsigned char buf[FILE_SIZE];
unsigned char buf2[FILE_SIZE];

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
  int count = 7;

  int fd = create_temp_file("tst-mmap6-", &fname);
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

  forget_temp_file(fname, fd);
  close(fd);

  /*
   * Test 1
   */

  printf("Test 1\n");

  if (fork() == 0)
  {
    fd = open(fname, O_RDWR);
    if (fd == -1)
    {
      perror("child: open failed");
      return 1;
    }
#ifdef __OS2__
    setmode (fd, O_BINARY);
#endif

    addr = mmap(NULL, FILE_SIZE, PROT_READ | PROT_WRITE, MAP_FILE | MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED)
    {
      perror("child: mmap failed");
      return 1;
    }

    while (count-- && addr[TEST_OFF1] != TEST_VAL)
      sleep(1);

    if (count <= 0)
    {
      printf("child: failed, count = %d\n", count);
      return 1;
    }

    printf("child: before writing test value\n");
    addr[TEST_OFF2] = TEST_VAL;
    printf("child: after writing test value\n");

    return 0;
  }

  fd = open(fname, O_RDWR);
  if (fd == -1)
  {
    perror("open failed");
    return 1;
  }
#ifdef __OS2__
  setmode (fd, O_BINARY);
#endif

  /*
   * Make sure child goes first (this is to have predictable tracing).
   */
  sleep(1);

  addr = mmap(NULL, FILE_SIZE * 2, PROT_READ | PROT_WRITE, MAP_FILE | MAP_SHARED, fd, 0);
  if (addr == MAP_FAILED)
  {
    perror("mmap failed");
    return 1;
  }

  printf("parent: before writing test value\n");
  addr[TEST_OFF1] = TEST_VAL;
  printf("parent: after writing test value\n");

  while (count-- && addr[TEST_OFF2] != TEST_VAL)
    sleep(1);

  if (count <= 0)
  {
    printf("parent: failed, count = %d\n", count);
    return 1;
  }

  if (munmap(addr, FILE_SIZE) == -1)
  {
    perror("munmap failed");
    return 1;
  }

  close(fd);

  /* Now read the file and see if the contents matches our changes */

  fd = open(fname, O_RDONLY);
  if (fd == -1)
  {
    perror("open failed");
    return 1;
  }
#ifdef __OS2__
  setmode (fd, O_BINARY);
#endif

  if (TEMP_FAILURE_RETRY((n = read(fd, buf2, FILE_SIZE))) != FILE_SIZE)
  {
    if (n != -1)
      printf("read failed (read %d bytes instead of %d)\n", n, FILE_SIZE);
    else
      perror("read failed");
    return 1;
  }

  for (i = 0; i < FILE_SIZE; ++i)
  {
    if (i == TEST_OFF1 || i == TEST_OFF2)
    {
      if (buf2[i] != TEST_VAL)
      {
        printf("buf2[0x%x] is %u, must be %u\n", i, buf2[i], TEST_VAL);
        return 1;
      }
    }
    else if (buf2[i] != buf[i])
    {
      printf("buf2[0x%x] is %u, must be %u\n", i, buf2[i], buf[i]);
      return 1;
    }
  }

  close(fd);

  free(fname);

  return 0;
}
