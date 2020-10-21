/*
 * Testcase for shmem API.
 * Copyright (C) 2020 bww bitwise works GmbH.
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

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <io.h>

#include "libcx/shmem.h"

#define BLOCK_SIZE (PAGE_SIZE * 4)

static int do_test(void);
#define TEST_FUNCTION do_test ()
#include "../test-skeleton.c"

static int
do_test (void)
{
  int rc, i;
  SHMEM h;
  unsigned char *addr, *addr2, *addr3;
  int status;

  /*
   * Test 1.1: simple shmem tests
   */

  printf("Test 1.1\n");

  addr = shmem_map(SHMEM_INVALID + 1, 0, 0);
  if (addr != NULL || errno != EINVAL)
  {
    printf("shmem_map returned %p and errno %d instead of NULL and errno %d\n", addr, errno, EINVAL);
    return 1;
  }

  rc = shmem_unmap(addr);
  if (rc != -1 || errno != EINVAL)
  {
    printf("shmem_unmap returned %d and errno %d instead of -1 and errno %d\n", rc, errno, EINVAL);
    return 1;
  }

  h = shmem_create(BLOCK_SIZE - 1, 0);
  if (h == SHMEM_INVALID)
  {
    perror("shmem_create failed");
    return 1;
  }

  {
    int flags;
    size_t size, act_size;
    rc = shmem_get_info(h, &flags, &size, &act_size);
    if (rc == -1)
    {
      perror("shmem_get_info failed");
      return 1;
    }

    if (flags != 0)
    {
      printf("shmem_get_info returned flags 0x%X nstead of 0x%X\n", flags, 0);
      return 1;
    }

    if (size != BLOCK_SIZE - 1 || act_size != BLOCK_SIZE)
    {
      printf("shmem_get_info returned size %u and act_size %u instead of %u and %u\n", size, act_size, BLOCK_SIZE - 1, BLOCK_SIZE);
      return 1;
    }
  }

  addr = shmem_map(h, 0, 0);
  if (!addr)
  {
    perror("shmem_map failed");
    return 1;
  }
  else
  {
    addr[PAGE_SIZE] = 123;
  }

  addr2 = shmem_map(h, PAGE_SIZE, 0);
  if (!addr2)
  {
    perror("shmem_map 2 failed");
    return 1;
  }
  else
  {
    if (addr2[0] != 123)
    {
      printf("read from addr 2 failed (must be 123, got %hu)\n", addr2[0]);
      return 1;
    }
  }

  if (shmem_unmap(addr2) == -1)
  {
    perror("shmem_unmap 2 failed");
    return 1;
  }

  rc = shmem_unmap(addr2);
  if (rc != -1 || errno != EINVAL)
  {
    printf("shmem_unmap returned %d and errno %d instead of -1 and errno %d\n", rc, errno, EINVAL);
    return 1;
  }

  addr3 = shmem_map(h, 0, PAGE_SIZE * 2);
  if (!addr3)
  {
    perror("shmem_map 3 failed");
    return 1;
  }
  else
  {
    if (addr3[PAGE_SIZE] != 123)
    {
      printf("read from addr 3 failed (must be 123, got %hu)\n", addr2[0]);
      return 1;
    }
  }

  if (shmem_unmap(addr3) == -1)
  {
    perror("shmem_unmap failed");
    return 1;
  }

  rc = shmem_close(h);
  if (rc == -1)
  {
    perror("shmem_close failed");
    return 1;
  }

  rc = shmem_close(h);
  if (rc != -1 || errno != EINVAL)
  {
    printf("shmem_close returned %d and errno %d instead of -1 and errno %d\n", rc, errno, EINVAL);
    return 1;
  }

  addr2 = shmem_map(h, PAGE_SIZE, 0);
  if (addr2 || errno != EINVAL)
  {
    printf("shmem_map 2 returned %p and errno %d instead of NULL and errno %d\n", addr, errno, EINVAL);
    return 1;
  }

  rc = shmem_open(h, 0);
  if (rc != -1 || errno != EINVAL)
  {
    printf("shmem_open closed returned %d and errno %d instead of -1 and errno %d\n", rc, errno, EINVAL);
    return 1;
  }

  if (shmem_unmap(addr) == -1)
  {
    perror("shmem_unmap failed");
    return 1;
  }

  /*
   * Test 1.2: test duplicate access permissions
   */

  printf("Test 1.2\n");

  h = shmem_create(BLOCK_SIZE - 1, 0);
  if (h == SHMEM_INVALID)
  {
    perror("shmem_create failed");
    return 1;
  }

  {
    SHMEM dup1, dup2;
    int flags;

    dup1 = shmem_duplicate(h, SHMEM_READONLY);
    if (dup1 == SHMEM_INVALID)
    {
      perror("shmem_duplicate 1 failed");
      return 1;
    }

    rc = shmem_get_info(dup1, &flags, NULL, NULL);
    if (rc == -1)
    {
      perror("shmem_get_info 1 failed");
      return 1;
    }

    if (flags != SHMEM_READONLY)
    {
      printf("shmem_get_info 1 returned flags 0x%X instead of 0x%X\n", flags, SHMEM_READONLY);
      return 1;
    }

    dup2 = shmem_duplicate(dup1, 0);
    if (dup2 == SHMEM_INVALID)
    {
      perror("shmem_duplicate 2 failed");
      return 1;
    }

    rc = shmem_get_info(dup2, &flags, NULL, NULL);
    if (rc == -1)
    {
      perror("shmem_get_info 2 failed");
      return 1;
    }

    if (flags != SHMEM_READONLY)
    {
      printf("shmem_get_info 2 returned flags 0x%X instead of 0x%X\n", flags, SHMEM_READONLY);
      return 1;
    }

    /* Note: deliberately leave two duplicates unclosed to check how they
     * are automatically closed on termination */
  }

  rc = shmem_close(h);
  if (rc == -1)
  {
    perror("shmem_close failed");
    return 1;
  }

  /*
   * Test 2: create many handles (> SHMEM_MIN_HANDLES)
   */

  printf("Test 2\n");

  SHMEM hs[20];

  for (i = 0; i < 20; ++i)
  {
    hs[i] = shmem_create(4096, 0);
    if (hs[i] == SHMEM_INVALID)
    {
      perror("shmem_create hs failed");
      return 1;
    }
  }

  for (i = 0; i < 20; ++i)
  {
    rc = shmem_close(hs[i]);
    if (rc == -1)
    {
      perror("shmem_close hs failed");
      return 1;
    }
  }

  /*
   * Test 3.1: shmem in child process
   */

  printf("Test 3.1\n");

  h = shmem_create(BLOCK_SIZE, SHMEM_PUBLIC);
  if (h == SHMEM_INVALID)
  {
    perror("shmem_create failed");
    return 1;
  }

  addr = shmem_map(h, 0, 0);
  if (!addr)
  {
    perror("shmem_map failed");
    return 1;
  }
  else
  {
    addr[PAGE_SIZE] = 123;
  }

  TEST_FORK_WITH_PIPE_BEGIN("child", 0, 0);
  {
    addr = shmem_map(h, 0, 0);
    if (addr != NULL || errno != EINVAL)
    {
      TEST_FORK_PRINTF("shmem_map returned %p and errno %d instead of NULL and errno %d\n", addr, errno, EINVAL);
      return 1;
    }

    rc = shmem_open(h, 0);
    if (rc == -1)
    {
      TEST_FORK_PERROR("shmem_open failed");
      return 1;
    }

    TEST_FORK_PING_PONG_PARENT();

    addr2 = shmem_map(h, PAGE_SIZE, 0);
    if (!addr2)
    {
      TEST_FORK_PERROR("shmem_map 2 failed");
      return 1;
    }
    else
    {
      if (addr2[0] != 123)
      {
        TEST_FORK_PRINTF("read from addr 2 failed (must be 123, got %hu)\n", addr2[0]);
        return 1;
      }
    }

    addr2[0] += 1; /* Increase for the next test */

    if (shmem_unmap(addr2) == -1)
    {
      TEST_FORK_PERROR("shmem_unmap 2 failed");
      return 1;
    }

    if (shmem_close(h) == -1)
    {
      TEST_FORK_PERROR("shmem_close failed");
      return 1;
    }

    TEST_FORK_PING_PONG_PARENT();

    return 0;
  }
  TEST_FORK_WITH_PIPE_PARENT_PART();
  {
    TEST_FORK_WAIT_CHILD_PING();

    rc = shmem_give(h, child_pid, 0);
    if (rc != -1 || errno != EPERM)
    {
      TEST_FORK_PRINTF("shmem_give public returned %d and errno %d instead of -1 and errno %d\n", rc, errno, EPERM);
      return 1;
    }

    TEST_FORK_PING_PONG_CHILD();

    rc = shmem_give(h, child_pid, 0);
    if (rc != -1 || errno != EACCES)
    {
      TEST_FORK_PRINTF("shmem_give public 2 returned %d and errno %d instead of -1 and errno %d\n", rc, errno, EACCES);
      return 1;
    }

    TEST_FORK_PING_CHILD();
  }
  TEST_FORK_WITH_PIPE_END();

  /*
   * Test 3.2: shmem in child process with access restrictions
   */

  printf("Test 3.2\n");

  {
    int flags;
    size_t size, act_size;
    rc = shmem_get_info(h, &flags, &size, &act_size);
    if (rc == -1)
    {
      perror("shmem_get_info failed");
      return 1;
    }

    if (flags != SHMEM_PUBLIC)
    {
      printf("shmem_get_info returned flags 0x%X instead of 0x%X\n", flags, SHMEM_PUBLIC);
      return 1;
    }

    if (size != BLOCK_SIZE || act_size != BLOCK_SIZE)
    {
      printf("shmem_get_info returned size %u and act_size %u instead of %u and %u\n", size, act_size, BLOCK_SIZE, BLOCK_SIZE);
      return 1;
    }
  }

  TEST_FORK_BEGIN("child", 0, SIGSEGV);
  {
    rc = shmem_open(h, SHMEM_READONLY);
    if (rc == -1)
    {
      TEST_FORK_PERROR("shmem_open failed");
      return 1;
    }

    {
      int flags;
      size_t size, act_size;
      rc = shmem_get_info(h, &flags, &size, &act_size);
      if (rc == -1)
      {
        perror("shmem_get_info failed");
        return 1;
      }

      if (flags != (SHMEM_PUBLIC | SHMEM_READONLY))
      {
        TEST_FORK_PRINTF("shmem_get_info returned flags 0x%X instead of 0x%X\n", flags, SHMEM_PUBLIC | SHMEM_READONLY);
        return 1;
      }

      if (size != BLOCK_SIZE || act_size != BLOCK_SIZE)
      {
        TEST_FORK_PRINTF("shmem_get_info returned size %u and act_size %u instead of %u and %u\n", size, act_size, BLOCK_SIZE, BLOCK_SIZE);
        return 1;
      }
    }

    addr2 = shmem_map(h, PAGE_SIZE, 0);
    if (!addr2)
    {
      TEST_FORK_PERROR("shmem_map 2 failed");
      return 1;
    }
    else
    {
      if (addr2[0] != 124)
      {
        TEST_FORK_PRINTF("read from addr 2 failed (must be 124, got %hu)\n", addr2[0]);
        return 1;
      }
    }

    addr2[0] = 1; /* This must crash */

    TEST_FORK_PRINTF("failed to crash\n");
  }
  TEST_FORK_END();

  /*
   * Test 3.3: duplicate in child process with access restrictions
   */

  printf("Test 3.3\n");

  SHMEM dup_h = shmem_duplicate(h, SHMEM_READONLY);
  if (dup_h == SHMEM_INVALID)
  {
    perror("shmem_duplicate failed");
    return 1;
  }

  TEST_FORK_BEGIN("child", 0, SIGSEGV);
  {
    rc = shmem_open(dup_h, 0);
    if (rc == -1)
    {
      TEST_FORK_PERROR("shmem_open failed");
      return 1;
    }

    addr2 = shmem_map(dup_h, PAGE_SIZE, 0);
    if (!addr2)
    {
      TEST_FORK_PERROR("shmem_map 2 failed");
      return 1;
    }
    else
    {
      if (addr2[0] != 124)
      {
        TEST_FORK_PRINTF("read from addr 2 failed (must be 124, got %hu)\n", addr2[0]);
        return 1;
      }
    }

    addr2[0] = 1; /* This must crash */

    TEST_FORK_PRINTF("failed to crash\n");
  }
  TEST_FORK_END();

  /*
   * Test 3.4: restrictive duplicate effect on self
   */

  printf("Test 3.4\n");

  TEST_FORK_BEGIN("child", 0, SIGSEGV);
  {
    rc = shmem_open(h, 0);
    if (rc == -1)
    {
      TEST_FORK_PERROR("shmem_open failed");
      return 1;
    }

    addr = shmem_map(h, 0, 0);
    if (!addr)
    {
      TEST_FORK_PERROR("shmem_map failed");
      return 1;
    }
    else
    {
      if (addr[PAGE_SIZE] != 124)
      {
        TEST_FORK_PRINTF("read from addr failed (must be 124, got %hu)\n", addr[PAGE_SIZE]);
        return 1;
      }
    }

    dup_h = shmem_duplicate(h, SHMEM_READONLY);
    if (dup_h == SHMEM_INVALID)
    {
      perror("shmem_duplicate failed");
      return 1;
    }

    addr2 = shmem_map(dup_h, PAGE_SIZE, 0);
    if (!addr2)
    {
      TEST_FORK_PERROR("shmem_map 2 failed");
      return 1;
    }
    else
    {
      if (addr2[0] != 124)
      {
        TEST_FORK_PRINTF("read from addr 2 failed (must be 124, got %hu)\n", addr2[0]);
        return 1;
      }
    }

    addr[0] = 1; /* This must succeed */

    addr[PAGE_SIZE] = 2; /* This must succeed because of having an r/w handle */
    addr2[0] = 3; /* This must succeed either (same addr as above) */

    if (shmem_unmap(addr) == -1)
    {
      TEST_FORK_PERROR("shmem_unmap failed");
      return 1;
    }

    addr2[0] = 4; /* Now this must crash (no r/w handle) */

    TEST_FORK_PRINTF("failed to crash\n");
  }
  TEST_FORK_END();

  if (addr[0] != 1)
  {
    printf("read from addr after dup failed (must be 1, got %hu)\n", addr[0]);
    return 1;
  }

  if (addr[PAGE_SIZE] != 3)
  {
    printf("read from addr after dup failed (must be 3, got %hu)\n", addr[PAGE_SIZE]);
    return 1;
  }

  /*
   * Test 3.5: restrictive duplicate effect on self (no crash)
   */

  printf("Test 3.5\n");

  TEST_FORK_BEGIN("child", 0, 0);
  {
    rc = shmem_open(h, 0);
    if (rc == -1)
    {
      TEST_FORK_PERROR("shmem_open failed");
      return 1;
    }

    addr = shmem_map(h, 0, 0);
    if (!addr)
    {
      TEST_FORK_PERROR("shmem_map failed");
      return 1;
    }
    else
    {
      if (addr[PAGE_SIZE] != 3)
      {
        TEST_FORK_PRINTF("read from addr failed (must be 3, got %hu)\n", addr[PAGE_SIZE]);
        return 1;
      }
    }

    dup_h = shmem_duplicate(h, SHMEM_READONLY);
    if (dup_h == SHMEM_INVALID)
    {
      perror("shmem_duplicate failed");
      return 1;
    }

    addr2 = shmem_map(dup_h, 0, PAGE_SIZE * 2);
    if (!addr2)
    {
      TEST_FORK_PERROR("shmem_map 2 failed");
      return 1;
    }
    else
    {
      if (addr2[PAGE_SIZE] != 3)
      {
        TEST_FORK_PRINTF("read from addr 2 failed (must be 3, got %hu)\n", addr2[PAGE_SIZE]);
        return 1;
      }
    }

    addr[0] = 1; /* This must succeed */

    addr2[0] = 2; /* This must succeed either (same addr as above) */

    if (shmem_unmap(addr) == -1)
    {
      TEST_FORK_PERROR("shmem_unmap failed");
      return 1;
    }

    addr2[0] = 3; /* This must succeed (still r/w view because of the same offset) */

    return 0;
  }
  TEST_FORK_END();

  if (addr[0] != 3)
  {
    printf("read from addr after dup failed (must be 3, got %hu)\n", addr[0]);
    return 1;
  }

  if (addr[PAGE_SIZE] != 3)
  {
    printf("read from addr after dup failed (must be 3, got %hu)\n", addr[PAGE_SIZE]);
    return 1;
  }

  /*
   * Test 3.6: restrictive duplicate effect on self (no crash either)
   */

  printf("Test 3.6\n");

  TEST_FORK_BEGIN("child", 0, 0);
  {
    rc = shmem_open(h, 0);
    if (rc == -1)
    {
      TEST_FORK_PERROR("shmem_open failed");
      return 1;
    }

    rc = shmem_open(dup_h, 0);
    if (rc == -1)
    {
      TEST_FORK_PERROR("shmem_open dup failed");
      return 1;
    }

    addr = shmem_map(dup_h, 0, 0);
    if (!addr)
    {
      TEST_FORK_PERROR("shmem_map dup failed");
      return 1;
    }
    else
    {
      if (addr[PAGE_SIZE] != 3)
      {
        TEST_FORK_PRINTF("read from addr failed (must be 3, got %hu)\n", addr[PAGE_SIZE]);
        return 1;
      }
    }

    addr2 = shmem_map(h, 0, PAGE_SIZE * 2);
    if (!addr2)
    {
      TEST_FORK_PERROR("shmem_map 2 failed");
      return 1;
    }
    else
    {
      if (addr2[PAGE_SIZE] != 3)
      {
        TEST_FORK_PRINTF("read from addr 2 failed (must be 3, got %hu)\n", addr2[PAGE_SIZE]);
        return 1;
      }
    }

    addr2[0] = 1; /* This must succeed (upgraded r/o view to r/w) */

    if (shmem_unmap(addr) == -1)
    {
      TEST_FORK_PERROR("shmem_unmap failed");
      return 1;
    }

    addr2[0] = 2; /* This must succeed (still r/w view because of the same offset) */

    return 0;
  }
  TEST_FORK_END();

  if (addr[0] != 2)
  {
    printf("read from addr after dup failed (must be 2, got %hu)\n", addr[0]);
    return 1;
  }

  if (addr[PAGE_SIZE] != 3)
  {
    printf("read from addr after dup failed (must be 3, got %hu)\n", addr[PAGE_SIZE]);
    return 1;
  }

  if (shmem_unmap(addr) == -1)
  {
    perror("shmem_unmap failed");
    return 1;
  }

  if (shmem_close(h) == -1)
  {
    perror("shmem_close failed");
    return 1;
  }

  if (shmem_close(dup_h) == -1)
  {
    perror("shmem_close dup failed");
    return 1;
  }

  /*
   * Test 5: shmem in child process with giving it access
   */

  printf("Test 5\n");

  h = shmem_create(BLOCK_SIZE, 0);
  if (h == SHMEM_INVALID)
  {
    perror("shmem_create failed");
    return 1;
  }

  addr = shmem_map(h, 0, 0);
  if (!addr)
  {
    perror("shmem_map failed");
    return 1;
  }
  else
  {
    addr[PAGE_SIZE] = 123;
  }

  TEST_FORK_WITH_PIPE_BEGIN("child", 0, 0);
  {
    addr = shmem_map(h, 0, 0);
    if (addr != NULL || errno != EINVAL)
    {
      TEST_FORK_PRINTF("shmem_map returned %p and errno %d instead of NULL and errno %d\n", addr, errno, EINVAL);
      return 1;
    }

    rc = shmem_open(h, 0);
    if (rc != -1 || errno != EACCES)
    {
      TEST_FORK_PRINTF("shmem_open returned %d and errno %d instead of -1 and errno %d\n", rc, errno, EACCES);
      return 1;
    }

    TEST_FORK_PING_PONG_PARENT(); /* Wait for shm_give... */

    rc = shmem_open(h, 0);
    if (rc != -1 || errno != EPERM)
    {
      TEST_FORK_PRINTF("shmem_open returned %d and errno %d instead of -1 and errno %d\n", rc, errno, EPERM);
      return 1;
    }

    addr2 = shmem_map(h, PAGE_SIZE, 0);
    if (!addr2)
    {
      TEST_FORK_PERROR("shmem_map 2 failed");
      return 1;
    }
    else
    {
      if (addr2[0] != 123)
      {
        TEST_FORK_PRINTF("read from addr 2 failed (must be 123, got %hu)\n", addr2[0]);
        return 1;
      }
    }

    if (shmem_unmap(addr2) == -1)
    {
      TEST_FORK_PERROR("shmem_unmap 2 failed");
      return 1;
    }

    if (shmem_close(h) == -1)
    {
      TEST_FORK_PERROR("shmem_close failed");
      return 1;
    }

    return 0;
  }
  TEST_FORK_WITH_PIPE_PARENT_PART();
  {
    TEST_FORK_WAIT_CHILD_PING();

    rc = shmem_give(h, child_pid, 0);

    TEST_FORK_PING_CHILD();
  }
  TEST_FORK_WITH_PIPE_END();

  if (shmem_unmap(addr) == -1)
  {
    perror("shmem_unmap failed");
    return 1;
  }

  if (shmem_close(h) == -1)
  {
    perror("shmem_close failed");
    return 1;
  }

  /*
   * Test 6: deliberately alloc but not free to check that it's freeed at
   * termination (otherwise we will get an assertion because of extra shared
   * heap)
   */

  printf("Test 6\n");

  h = shmem_create(BLOCK_SIZE, SHMEM_PUBLIC);
  if (h == SHMEM_INVALID)
  {
    perror("shmem_create failed");
    return 1;
  }

  addr = shmem_map(h, 0, 0);
  if (!addr)
  {
    perror("shmem_map failed");
    return 1;
  }

  return 0;
}
