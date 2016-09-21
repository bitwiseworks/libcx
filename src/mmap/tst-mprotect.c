/*
 * Testcase for mprotect.
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
#define TEST_SIZE 10
#define TEST_VAL 255

/* Shared mapping file flush interval in ms (must match one from mmap.c) */
#define FLUSH_DELAY 1000

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

  int fd = create_temp_file("tst-mprotect-", &fname);
  if (fd == -1)
  {
    perror("create_temp_file failed");
    return 1;
  }

#ifdef __OS2__
  setmode(fd, O_BINARY);
#endif

  srand(getpid());

  for (i = 0; i < FILE_SIZE; ++i)
    buf[i] = rand() % 255;

  if ((n = write(fd, buf, FILE_SIZE)) != FILE_SIZE)
  {
    if (n != -1)
      printf("write failed (write %d bytes instead of %d)\n", n, FILE_SIZE);
    else
      perror("write failed");
    return 1;
  }

  /*
   * Test 1: create a PROT_NONE private anon mapping and try to read it
   * (should crash).
   */

  printf("Test 1\n");

  TEST_FORK_BEGIN("child 1", 0, SIGSEGV);
  {
    addr = mmap(NULL, FILE_SIZE, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (addr == MAP_FAILED)
    {
      TEST_FORK_PERROR("mmap failed");
      return 1;
    }

    /* Should crash here */
    TEST_FORK_PRINTF("*addr = %u\n", *addr);

    return 1;
  }
  TEST_FORK_END();

  /*
   * Test 2: create a PROT_NONE private anon mapping, set it to R/W and write to
   * it (should succeed).
   */

  printf("Test 2\n");

  TEST_FORK_BEGIN("child 2", 0, 0);
  {
    addr = mmap(NULL, FILE_SIZE, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (addr == MAP_FAILED)
    {
      TEST_FORK_PERROR("mmap failed");
      return 1;
    }

    if (mprotect(addr, FILE_SIZE, PROT_READ | PROT_WRITE) == -1)
    {
      TEST_FORK_PERROR("mprotect failed");
      return 1;
    }

    *addr = 1;

    if (munmap(addr, FILE_SIZE) == -1)
    {
      TEST_FORK_PERROR("munmap failed");
      return 1;
    }

    return 0;
  }
  TEST_FORK_END();

  /*
   * Test 3: create a PROT_READ | PROT_WRITE private anon mapping, set it to
   * PROT_NONE and then try writing to it (should crash).
   */

  printf("Test 3\n");

  TEST_FORK_BEGIN("child 3", 0, SIGSEGV);
  {
    addr = mmap(NULL, FILE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (addr == MAP_FAILED)
    {
      TEST_FORK_PERROR("mmap failed");
      return 1;
    }

    if (mprotect(addr, FILE_SIZE, PROT_NONE) == -1)
    {
      TEST_FORK_PERROR("mprotect failed");
      return 1;
    }

    /* Should crash here */
    *addr = 1;

    return 1;
  }
  TEST_FORK_END();

  /*
   * Test 4: create a PROT_READ private anon mapping, set it to PROT_WRITE and
   * then try reading from it (should succeed on x86 where PROT_WRITE implies
   * PROT_READ).
   */

  printf("Test 4\n");

  TEST_FORK_BEGIN("child 4", 0, 0);
  {
    addr = mmap(NULL, FILE_SIZE, PROT_READ, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (addr == MAP_FAILED)
    {
      TEST_FORK_PERROR("mmap failed");
      return 1;
    }

    if (mprotect(addr, FILE_SIZE, PROT_WRITE) == -1)
    {
      TEST_FORK_PERROR("mprotect failed");
      return 1;
    }

    /* *addr must be 0 here */
    return *addr;
  }
  TEST_FORK_END();

  /*
   * Test 5: create a PROT_READ private anon mapping, try to write to it
   * (should fail).
   */

  printf("Test 5\n");

  TEST_FORK_BEGIN("child 5", 0, SIGSEGV);
  {
    addr = mmap(NULL, FILE_SIZE, PROT_READ, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (addr == MAP_FAILED)
    {
      TEST_FORK_PERROR("mmap failed");
      return 1;
    }

    /* Should crash here */
    *addr = 1;

    return 1;
  }
  TEST_FORK_END();

  /*
   * Test 5: create a PROT_READ private anon mapping, try to write to it
   * (should fail).
   */

  printf("Test 5\n");

  TEST_FORK_BEGIN("child 5", 0, SIGSEGV);
  {
    addr = mmap(NULL, FILE_SIZE, PROT_READ, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (addr == MAP_FAILED)
    {
      TEST_FORK_PERROR("mmap failed");
      return 1;
    }

    /* Should crash here */
    *addr = 1;

    return 1;
  }
  TEST_FORK_END();

  /*
   * Test 6: create a PROT_READ private anon mapping, change it to
   * PROT_READ | PROT_WRITE, try to write to it (should succeed).
   */

  printf("Test 6\n");

  TEST_FORK_BEGIN("child 6", 0, 0);
  {
    addr = mmap(NULL, FILE_SIZE, PROT_READ, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (addr == MAP_FAILED)
    {
      TEST_FORK_PERROR("mmap failed");
      return 1;
    }

    if (mprotect(addr, FILE_SIZE, PROT_READ | PROT_WRITE) == -1)
    {
      TEST_FORK_PERROR("mprotect failed");
      return 1;
    }

    *addr = 1;

    return 0;
  }
  TEST_FORK_END();

  /*
   * Test 7: create a PROT_READ | PROT_WRITE private mapping and try to remove
   * PROT_WRITE. Should fail on OS/2 as we don't support changing protection
   * for file-backed mappings for now.
   */

  printf("Test 7\n");

  TEST_FORK_BEGIN("child 7", 0, 0);
  {
    addr = mmap(NULL, FILE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    if (addr == MAP_FAILED)
    {
      TEST_FORK_PERROR("mmap failed");
      return 1;
    }

#ifdef __OS2__
    if (mprotect(addr, FILE_SIZE, PROT_READ) == 0)
    {
      TEST_FORK_PRINTF("mprotect succeeded");
      return 1;
    }
#else
    if (mprotect(addr, FILE_SIZE, PROT_READ) == -1)
    {
      TEST_FORK_PERROR("mprotect failed");
      return 1;
    }
#endif

    if (munmap(addr, FILE_SIZE) == -1)
    {
      TEST_FORK_PERROR("munmap failed");
      return 1;
    }

    return 0;
  }
  TEST_FORK_END();

  /*
   * Test 8: create a PROT_READ | PROT_WRITE shared anon mapping and try to
   * change protection to PROT_NONE (should fail on OS/2).
   */

  printf("Test 8\n");

  TEST_FORK_BEGIN("child 8", 0, 0);
  {
    addr = mmap(NULL, FILE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
    if (addr == MAP_FAILED)
    {
      TEST_FORK_PERROR("mmap failed");
      return 1;
    }

    /* Commit memory */
    *addr = 1;

#ifdef __OS2__
    if (mprotect(addr, FILE_SIZE, PROT_NONE) == 0)
    {
      TEST_FORK_PRINTF("mprotect succeeded\n");
      return 1;
    }
#else
    if (mprotect(addr, FILE_SIZE, PROT_NONE) == -1)
    {
      TEST_FORK_PERROR("mprotect failed");
      return 1;
    }
#endif

    return 0;
  }
  TEST_FORK_END();

  /*
   * Test 9: create a PROT_READ | PROT_WRITE shared mapping and try to remove
   * PROT_WRITE. Should fail on OS/2 as we don't support changing protection
   * for file-backed mappings for now.
   */

  printf("Test 9\n");

  TEST_FORK_BEGIN("child 9", 0, 0);
  {
    addr = mmap(NULL, FILE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED)
    {
      TEST_FORK_PERROR("mmap failed");
      return 1;
    }

#ifdef __OS2__
    if (mprotect(addr, FILE_SIZE, PROT_READ) == 0)
    {
      TEST_FORK_PRINTF("mprotect succeeded");
      return 1;
    }
#else
    if (mprotect(addr, FILE_SIZE, PROT_READ) == -1)
    {
      TEST_FORK_PERROR("mprotect failed");
      return 1;
    }
#endif

    if (munmap(addr, FILE_SIZE) == -1)
    {
      TEST_FORK_PERROR("munmap failed");
      return 1;
    }

    return 0;
  }
  TEST_FORK_END();

  /*
   * Test 10: create a PROT_READ | PROT_WRITE shared mapping over a R/O file
   * (should fail).
   */

  printf("Test 10\n");

  close(fd);

  fd = open(fname, O_RDONLY | O_CREAT, 0666);
  if (fd == -1)
  {
    perror("open failed");
    return 1;
  }

  TEST_FORK_BEGIN("child 10", 0, 0);
  {
    addr = mmap(NULL, FILE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (addr != MAP_FAILED || errno != EACCES)
    {
      TEST_FORK_PERROR("mmap succeeded or errno is not EACCES");
      return 1;
    }

    return 0;
  }
  TEST_FORK_END();

  return 0;
}
