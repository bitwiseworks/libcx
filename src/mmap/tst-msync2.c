/*
 * Testcase for automatic mmap sync.
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

  int fd = create_temp_file("tst-mmap2-1-", &fname);
  if (fd == -1)
    {
      puts("create_temp_file failed");
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
   * Test 1: create shared mmap, start child, create another shared mmap there
   * and make sure that changes are flushed to disk after FLUSH_DELAY
   */

  printf("Test 1.1\n");

  addr = mmap(NULL, FILE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (addr == MAP_FAILED)
  {
    perror("mmap failed");
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

  /* Activate mamp flush thread */
  addr[0] = buf[0];

  switch (fork())
  {
    case -1:
      perror("fork failed");
      exit(-1);

    case 0:
    {
      unsigned char *addr2;

      printf("Test 1.2\n");

      int fd2 = create_temp_file("tst-mmap2-2-", &fname);
      if (fd2 == -1)
        {
          puts("create_temp_file in child failed");
          return 1;
        }

#ifdef __OS2__
      setmode (fd2, O_BINARY);
#endif

      srand(getpid());

      for (i = 0; i < FILE_SIZE; ++i)
        buf[i] = rand() % 255;

      if ((n = write(fd2, buf, FILE_SIZE)) != FILE_SIZE)
      {
        if (n != -1)
          printf("write in child failed (write %d bytes instead of %d)\n", n, FILE_SIZE);
        else
          perror("write in child failed");
        return 1;
      }

      addr2 = mmap(NULL, FILE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd2, 0);
      if (addr2 == MAP_FAILED)
      {
        perror("mmap failed");
        exit(-1);
      }

      for (i = PAGE_SIZE - TEST_SIZE; i < PAGE_SIZE + TEST_SIZE; ++i)
      {
        if (addr2[i] != buf[i])
        {
          printf("child: addr2[%d] is %u, must be %u\n", i, addr2[i], buf[i]);
          return 1;
        }
      }

      /* Activate mamp flush thread */
      addr2[0] = buf[0];

      switch (fork())
      {
        case -1:
          perror("fork in child failed");
          exit(-1);

        case 0:
        {
          for (i = PAGE_SIZE - TEST_SIZE; i < PAGE_SIZE + TEST_SIZE; ++i)
          {
            if (addr2[i] != buf[i])
            {
              printf("grandchild: addr2[%d] is %u, must be %u\n", i, addr2[i], buf[i]);
              return 1;
            }
          }

          for (i = PAGE_SIZE - TEST_SIZE; i < PAGE_SIZE + TEST_SIZE; ++i)
            addr2[i] = TEST_VAL;

          /* Let mmap auto-sync code flush changed memory back to the file */
          usleep((FLUSH_DELAY + 500) * 1000);

          /* Now check the file contents */
          if (lseek(fd2, 0, SEEK_SET) == -1)
          {
            perror("lseek in grandchild failed");
            return 1;
          }

          /* Now check the file contents */
          if ((n = read(fd2, buf_chk, FILE_SIZE)) != FILE_SIZE)
          {
            if (n != -1)
              printf("read in grandchild failed (read %d bytes instead of %d)\n", n, FILE_SIZE);
            else
              perror("read in grandchild failed");
            return 1;
          }

          for (i = 0; i < FILE_SIZE; ++i)
          {
            if (addr2[i] != buf_chk[i])
            {
              printf("grandchild: buf_chk[%d] is %u, must be %u\n", i, buf_chk[i], addr2[i]);
              return 1;
            }
          }

          if (munmap(addr2, FILE_SIZE) == -1)
          {
            perror("child: munmap failed");
            return 1;
          }

          return 0;
        }
      }

      if (wait(&status) == -1)
      {
        perror("wait in child failed");
        return 1;
      }
      if (!WIFEXITED(status) || WEXITSTATUS(status))
      {
        printf("grandchild crashed or returned non-zero (status %x)\n", status);
        return 1;
      }

      return 0;
    }
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

  if (munmap(addr, FILE_SIZE) == -1)
  {
    perror("munmap failed");
    return 1;
  }

  free(fname);

  return 0;
}
