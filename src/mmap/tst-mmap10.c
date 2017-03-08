/*
 * Testcase for big mmap allocation.
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

#define WIN_SIZE (1024U * 1024U * 32U) // 32M

static int do_test(void);
#define TEST_FUNCTION do_test ()
#include "../test-skeleton.c"

static int
do_test (void)
{
  int rc;
  int i, n;
  unsigned char **addr;
  unsigned int max_size;
  off_t off;
  char *fname;

  int fd = create_temp_file("tst-mmap10-", &fname);
  if (fd == -1)
  {
    perror("create_temp_file failed");
    return 1;
  }

  max_size = (1024U * 1024U * 1024U); // 1G
  addr = (unsigned char **)malloc(max_size / WIN_SIZE * sizeof(*addr));

  printf("Test 1: %i iterations (up to 0x%x bytes)\n", max_size / WIN_SIZE, max_size);

  for (i = 0, off = 0; i < max_size / WIN_SIZE; ++i, off += WIN_SIZE)
  {
    printf("Iteration %d, off 0x%llx, len 0x%x\n", i, off, WIN_SIZE);

    addr[i] = mmap(NULL, WIN_SIZE, PROT_READ, MAP_PRIVATE, fd, off);
    if (addr[i] == MAP_FAILED)
    {
      perror("mmap failed");
      return 1;
    }
  }

  for (i = 0; i < max_size / WIN_SIZE; ++i)
  {
    if (munmap(addr[i], WIN_SIZE) == -1)
    {
      perror("munmap failed");
      return 1;
    }
  }

  free(addr);
  free(fname);

  return 0;
}
