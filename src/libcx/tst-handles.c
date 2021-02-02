/*
 * Testcase for handles API.
 * Copyright (C) 2021 bww bitwise works GmbH.
 * This file is part of the kLIBC Extension Library.
 * Authored by Dmitry Kuminov <coding@dmik.org>, 2020.
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

// @todo Temporary!
#define OS2EMX_PLAIN_CHAR
#define INCL_BASE
#include <os2.h>

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <io.h>

#include "libcx/handles.h"
#include "libcx/shmem.h"

static int do_test(void);
#define TEST_FUNCTION do_test ()
#include "../test-skeleton.c"

#define BLOCK_SIZE (PAGE_SIZE * 4)

static int
do_test (void)
{
  SHMEM h1, h2;

  int rc;

  /*
   * Test 1.1: parent sends handles to child
   * (0 - no flags, 1 - LIBCX_HANDLE_CLOSE)
   */

  h1 = shmem_create(BLOCK_SIZE, 0);
  if (h1 == SHMEM_INVALID)
  {
    perror("shmem_create(h1) failed");
    return 1;
  }

  h2 = shmem_create(BLOCK_SIZE, SHMEM_PUBLIC);
  if (h2 == SHMEM_INVALID)
  {
    perror("shmem_create(h2) failed");
    return 1;
  }

  for (int n = 1; n <= 2; ++n)
  {
    printf("Test 1.1.%d\n", n);

    TEST_FORK_WITH_PIPE_BEGIN("child", 0, 0);
    {
      TEST_FORK_PRINTF("here 1\n");

      TEST_FORK_WAIT_PARENT_PING();

      TEST_FORK_PRINTF("here 2\n");

      int flags;
      size_t size, act_size;

      rc = shmem_get_info(h1, &flags, &size, &act_size);
      if (rc == -1)
      {
        TEST_FORK_PERROR("shmem_get_info(h1) failed");
        return 1;
      }
      if (flags != 0)
      {
        TEST_FORK_PRINTF("shmem_get_info(h1) returned flags 0x%X nstead of 0x%X\n", flags, 0);
        return 1;
      }

      rc = shmem_get_info(h2, &flags, &size, &act_size);
      if (rc == -1)
      {
        TEST_FORK_PERROR("shmem_get_info(h2) failed");
        return 1;
      }
      if (flags != SHMEM_PUBLIC)
      {
        TEST_FORK_PRINTF("shmem_get_info(h2) returned flags 0x%X nstead of 0x%X\n", flags, SHMEM_PUBLIC);
        return 1;
      }

      return 0;
    }
    TEST_FORK_WITH_PIPE_PARENT_PART();
    {
      LIBCX_HANDLE handles[2] = { {LIBCX_HANDLE_SHMEM, 0, h1}, {LIBCX_HANDLE_SHMEM, 0, h2} };
      rc = libcx_send_handles(handles, sizeof(handles)/sizeof(*handles), child_pid,
                              n == 2 ? LIBCX_HANDLE_CLOSE : 0);
      if (rc == -1)
      {
        perror("libcx_send_handles(h1,h2) failed");
        return 1;
      }

      int flags;
      size_t size, act_size;

      if (n == 1)
      {
        rc = shmem_get_info(h1, &flags, &size, &act_size);
        if (rc == -1)
        {
          perror("shmem_get_info(h1) failed");
          return 1;
        }
      }
      else
      {
        rc = shmem_get_info(h1, &flags, &size, &act_size);
        if (rc != -1 || errno != EINVAL)
        {
          printf("shmem_get_info(h1) succeeded or failed not with EINVAL (%d, %d)\n", rc, errno);
          return 1;
        }
      }

      TEST_FORK_PING_CHILD();
    }
    TEST_FORK_WITH_PIPE_END();
  }

  /*
   * Test 1.2: child takes handles from parent
   * (0 - no flags, 1 - LIBCX_HANDLE_CLOSE)
   */

  h1 = shmem_create(BLOCK_SIZE, 0);
  if (h1 == SHMEM_INVALID)
  {
    perror("shmem_create(h1) failed");
    return 1;
  }

  h2 = shmem_create(BLOCK_SIZE, SHMEM_PUBLIC);
  if (h2 == SHMEM_INVALID)
  {
    perror("shmem_create(h2) failed");
    return 1;
  }

  for (int n = 1; n <= 2; ++n)
  {
    printf("Test 1.2.%d\n", n);

    TEST_FORK_WITH_PIPE_BEGIN("child", 0, 0);
    {
      LIBCX_HANDLE handles[2] = { {LIBCX_HANDLE_SHMEM, 0, h1}, {LIBCX_HANDLE_SHMEM, 0, h2} };
      rc = libcx_take_handles(handles, sizeof(handles)/sizeof(*handles), parent_pid,
                              n == 2 ? LIBCX_HANDLE_CLOSE : 0);
      if (rc == -1)
      {
        TEST_FORK_PERROR("libcx_take_handles(h1,h2) failed");
        return 1;
      }

      int flags;
      size_t size, act_size;

      rc = shmem_get_info(h1, &flags, &size, &act_size);
      if (rc == -1)
      {
        TEST_FORK_PERROR("shmem_get_info(h1) failed");
        return 1;
      }
      if (flags != 0)
      {
        TEST_FORK_PRINTF("shmem_get_info(h1) returned flags 0x%X nstead of 0x%X\n", flags, 0);
        return 1;
      }

      rc = shmem_get_info(h2, &flags, &size, &act_size);
      if (rc == -1)
      {
        TEST_FORK_PERROR("shmem_get_info(h2) failed");
        return 1;
      }
      if (flags != SHMEM_PUBLIC)
      {
        TEST_FORK_PRINTF("shmem_get_info(h2) returned flags 0x%X nstead of 0x%X\n", flags, SHMEM_PUBLIC);
        return 1;
      }

      return 0;
    }
    TEST_FORK_WITH_PIPE_END();

    int flags;
    size_t size, act_size;

    if (n == 1)
    {
      rc = shmem_get_info(h1, &flags, &size, &act_size);
      if (rc == -1)
      {
        perror("shmem_get_info(h1) failed");
        return 1;
      }
    }
    else
    {
      rc = shmem_get_info(h1, &flags, &size, &act_size);
      if (rc != -1 || errno != EINVAL)
      {
        printf("shmem_get_info(h1) succeeded or failed not with EINVAL (%d, %d)\n", rc, errno);
        return 1;
      }
    }
  }

  /*
   * Test 1.3: parent takes handles from child
   */

  printf("Test 1.3\n");

  h1 = shmem_create(BLOCK_SIZE, 0);
  if (h1 == SHMEM_INVALID)
  {
    perror("shmem_create(h1) failed");
    return 1;
  }

  h2 = shmem_create(BLOCK_SIZE, SHMEM_PUBLIC);
  if (h2 == SHMEM_INVALID)
  {
    perror("shmem_create(h2) failed");
    return 1;
  }

  TEST_FORK_WITH_PIPE_BEGIN("child", 0, 0);
  {
    SHMEM h3, h4;

    h3 = shmem_create(BLOCK_SIZE, 0);
    if (h3 == SHMEM_INVALID)
    {
      TEST_FORK_PERROR("shmem_create(h3) failed");
      return 1;
    }

    h4 = shmem_create(BLOCK_SIZE, SHMEM_PUBLIC);
    if (h4 == SHMEM_INVALID)
    {
      TEST_FORK_PERROR("shmem_create(h4) failed");
      return 1;
    }

    TEST_FORK_PRINTF("here 1 (h3 %d, h4 %d)\n", h3, h4);

    TEST_FORK_WAIT_PARENT_PING();

    SHMEM *mem = shmem_map(h1, 0, 0);
    if (mem == NULL)
    {
      TEST_FORK_PERROR("shmem_map(h1) failed");
      return 1;
    }

    mem[0] = h3;
    mem[1] = h4;

    TEST_FORK_PRINTF("here 2\n", h3, h4);

    TEST_FORK_PING_PARENT();

    TEST_FORK_WAIT_PARENT_PING();

    return 0;
  }
  TEST_FORK_WITH_PIPE_PARENT_PART();
  {
    LIBCX_HANDLE handles[2] = { {LIBCX_HANDLE_SHMEM, 0, h1}, {LIBCX_HANDLE_SHMEM, 0, h2} };
    rc = libcx_send_handles(handles, sizeof(handles)/sizeof(*handles), child_pid, 0);
    if (rc == -1)
    {
      perror("libcx_send_handles(h1,h2) failed");
      return 1;
    }

    TEST_FORK_PING_CHILD();

    TEST_FORK_WAIT_CHILD_PING();

    SHMEM *mem = shmem_map(h1, 0, 0);
    if (mem == NULL)
    {
      perror("shmem_map(h1) failed");
      return 1;
    }

    SHMEM h3, h4;
    h3 = mem[0];
    h4 = mem[1];

    printf("here 1 (h3 %d, h4 %d)\n", h3, h4);

    int flags;
    size_t size, act_size;

    rc = shmem_get_info(h3, &flags, &size, &act_size);
    if (rc != -1 || errno != EINVAL)
    {
      printf("shmem_get_info(h3) succeeded or failed not with EINVAL (%d, %d)\n", rc, errno);
      return 1;
    }
    rc = shmem_get_info(h4, &flags, &size, &act_size);
    if (rc != -1 || errno != EINVAL)
    {
      printf("shmem_get_info(h4) succeeded or failed not with EINVAL (%d, %d)\n", rc, errno);
      return 1;
    }

    LIBCX_HANDLE handles2[2] = { {LIBCX_HANDLE_SHMEM, 0, h3}, {LIBCX_HANDLE_SHMEM, 0, h4} };
    rc = libcx_take_handles(handles2, sizeof(handles2)/sizeof(*handles2), child_pid, 0);
    if (rc == -1)
    {
      perror("libcx_take_handles(h3,h4) failed");
      return 1;
    }

    TEST_FORK_PING_CHILD();

    printf("here 2\n");

    rc = shmem_get_info(h3, &flags, &size, &act_size);
    if (rc == -1)
    {
      perror("shmem_get_info(h3) failed");
      return 1;
    }
    rc = shmem_get_info(h4, &flags, &size, &act_size);
    if (rc == -1)
    {
      perror("shmem_get_info(h4) failed");
      return 1;
    }
  }
  TEST_FORK_WITH_PIPE_END();

  return 0;
}
