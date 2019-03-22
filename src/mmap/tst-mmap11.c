/*
 * Testcase for multi-threaded and multi-process concurrent mmap access.
 * Copyright (C) 2019 bww bitwise works GmbH.
 * This file is part of the kLIBC Extension Library.
 * Authored by Dmitry Kuminov <coding@dmik.org>, 2019.
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
#include <io.h>
#include <sys/param.h>
#include <sys/mman.h>

#define INCL_BASE
#include <os2.h>

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif

#define FILE_SIZE (PAGE_SIZE * 300)

unsigned char buf[FILE_SIZE];

static int do_test(void);
#define TEST_FUNCTION do_test ()
#include "../test-skeleton.c"

void thread_func(void *arg)
{
  unsigned char *addr = arg;

  int i;

  /*
   * Test 1: simple mmap
   */

  printf("pid %d tid %d: Test 1\n", getpid(), _gettid());

  for (i = 0; i < FILE_SIZE; ++i)
  {
    if (addr[i] != buf[i])
    {
      printf("tid %d: addr[%d] is %u, must be %u\n", _gettid(), i, addr[i], buf[i]);
      exit(-1);
    }
  }

  printf("pid %d tid %d: Test END\n", getpid(), _gettid());
}


static int
do_test (void)
{
  int rc;
  int i, n;
  unsigned char *addr;
  char *fname;

  int fd = create_temp_file("tst-mmap11-", &fname);
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
      perrno("write failed");
    return 1;
  }

  /*
   * Test 1: threads
   */

  printf("Test 1\n");

  addr = mmap(NULL, FILE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (addr == MAP_FAILED)
  {
    perror("mmap failed");
    exit(-1);
  }

#if DEBUG
  // May be slow due to logging so limit the number of threads
  int tids[5] = {0};
#else
  int tids[50] = {0};
#endif

  for (i = 0; i < sizeof(tids)/sizeof(tids[0]); ++i)
  {
    tids[i] = _beginthread (thread_func, NULL, 0, addr);
    if (tids[i] == -1)
    {
      perrno("_beginthread");
      return 1;
    }
  }

  for (i = 0; i < sizeof(tids)/sizeof(tids[0]); ++i)
  {
    APIRET arc = DosWaitThread((PTID)&tids[i], DCWW_WAIT);
    if (arc && arc != ERROR_INVALID_THREADID)
    {
      perr("DosWaitThread returned %ld", arc);
      return 1;
    }
  }

  if (munmap(addr, FILE_SIZE) == -1)
  {
    perrno("munmap failed");
    return 1;
  }

  close(fd);

  /*
   * Test 2: processes
   */

  printf("Test 2\n");

  fd = open(fname, O_RDONLY);
  if (fd == -1)
  {
    perror("open failed");
    return 1;
  }

  addr = mmap(NULL, FILE_SIZE, PROT_READ, MAP_SHARED, fd, 0);
  if (addr == MAP_FAILED)
  {
    perror("mmap failed");
    exit(-1);
  }

  int pids[7] = {0};

  for (i = 0; i < sizeof(pids)/sizeof(pids[0]); ++i)
  {
    int pid;

    switch ((pid = fork()))
    {
      case -1:
        perror("fork failed");
        exit(-1);

      case 0:
      {
        /* Child */

        thread_func(addr);

        return 0;
      }

      default:
        pids[i] = pid;
    }
  }

  rc = 0;

  for (i = 0; i < sizeof(pids)/sizeof(pids[0]); ++i)
  {
    int status;

    if (waitpid(pids[i], &status, 0) == -1)
    {
      perror("waitpid failed");
      rc = -1;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status))
    {
      printf("child crashed or returned non-zero (status %x)\n", status);
      rc = -1;
    }
  }

  if (munmap(addr, FILE_SIZE) == -1)
  {
    perrno("munmap failed");
    return -1;
  }

  close(fd);

  free(fname);

  return rc;
}
