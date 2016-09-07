/*
 * Testcase for anonymous private mmap.
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

#define _BSD_SOURCE             /* Get MAP_ANONYMOUS definition */
#include <sys/wait.h>
#include <sys/mman.h>
#include <stdlib.h>

static int do_test(void);
#define TEST_FUNCTION do_test()
#include "../test-skeleton.c"

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif
#define MEM_SIZE (PAGE_SIZE * 10)

static int do_test(void)
{
  int status;
  char *mem;

  mem = (char *)mmap(NULL, MEM_SIZE, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mem == MAP_FAILED)
  {
    perror ("mmap failed");
    exit(-1);
  }

  if (*mem != 0)
  {
    printf("value is %d, must be 0\n", *mem);
    exit(-1);
  }

  *mem = 1;

  if (*mem != 1)
  {
    printf("value is %d, must be 1\n", *mem);
    exit(-1);
  }

  mem[PAGE_SIZE * 2] = 2;
  if (mem[PAGE_SIZE * 2] != 2)
  {
    printf("value is %d, must be 2\n", mem[PAGE_SIZE * 2]);
    exit(-1);
  }

  if (munmap(mem, MEM_SIZE) == -1)
  {
    perror ("munmap failed");
    exit(-1);
  }

  switch (fork())
  {
    case -1:
      perror("fork failed");
      exit(-1);
    case 0:
      mem = (char *)mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
      if (mem == MAP_FAILED)
      {
        perror ("mmap failed");
        exit(-1);
      }

      if (*mem != 0)
      {
        printf("value is %d, must be 0\n", *mem);
        exit(-1);
      }

      *mem = 1;

      if (*mem != 1)
      {
        printf("value is %d, must be 1\n", *mem);
        exit(-1);
      }

      /* This should crash the child with SIGSEGV */
      mem[PAGE_SIZE] = 2;
      exit(EXIT_SUCCESS);
  }

  if (wait(&status) == -1)
  {
    perror("wait failed");
    exit(-1);
  }
  if (WIFEXITED(status) || !WIFSIGNALED(status) || WTERMSIG(status) != SIGSEGV)
  {
    printf("child ended sucessfully or crashed not with SIGSEGV (status %x)\n", status);
    exit(-1);
  }

  return 0;
}
