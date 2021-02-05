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
#include <sys/socket.h>
#include <emx/io.h>

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
  int *buf;
  int p[2];

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

  buf = NULL;

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

      buf = (int*)shmem_map(h1, 0, 0);
      if (buf == NULL)
      {
        TEST_FORK_PERROR("shmem_map(h1) failed");
        return 1;
      }

      TEST_FORK_PRINTF("here 3 (source %d, target %d)\n", buf[0], buf[1]);

      __LIBC_PFH pFH = __libc_FH(buf[1]);
      if (!pFH || ((pFH->fFlags & __LIBC_FH_TYPEMASK) != F_SOCKET))
      {
        TEST_FORK_PRINTF("fd %d is not F_SOCKET (pFH %p name [%s] flags 0x%x)\n", buf[1],
                         pFH, pFH ? pFH->pszNativePath : NULL, pFH ? pFH->fFlags : 0);
        return 1;
      }

      rc = TEMP_FAILURE_RETRY(write(buf[1], "\x12\x34\x56\x78", 4));
      if (rc == -1)
      {
        TEST_FORK_PERROR("write(socket) failed");
        return 1;
      }

      return 0;
    }
    TEST_FORK_WITH_PIPE_PARENT_PART();
    {
      rc = socketpair(AF_LOCAL, SOCK_STREAM, 0, p);
      if (rc == -1)
      {
        perror("socketpair failed");
        return 1;
      }

      printf("here 1 (socketpair %d, %d)\n", p[0], p[1]);

      LIBCX_HANDLE handles[] = { {LIBCX_HANDLE_SHMEM, 0, h1}, {LIBCX_HANDLE_SHMEM, 0, h2},
                                 {LIBCX_HANDLE_FD, 0, p[1]} };
      rc = libcx_send_handles(handles, sizeof(handles)/sizeof(*handles), child_pid,
                              n == 2 ? LIBCX_HANDLE_CLOSE : 0);
      if (rc == -1)
      {
        perror("libcx_send_handles(h1,h2,fd) failed");
        return 1;
      }

      int flags;
      size_t size, act_size;

      __LIBC_PFH pFH;

      if (n == 1)
      {
        rc = shmem_get_info(h1, &flags, &size, &act_size);
        if (rc == -1)
        {
          perror("shmem_get_info(h1) failed");
          return 1;
        }

        pFH = __libc_FH(p[1]);
        if (!pFH)
        {
          printf("socket was already closed\n");
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

        pFH = __libc_FH(p[1]);
        if (pFH)
        {
          printf("socket was NOT already closed\n");
          return 1;
        }
      }

      if (buf == NULL)
      {
        buf = (int*)shmem_map(h1, 0, 0);
        if (buf == NULL)
        {
          perror("shmem_map(h1) failed");
          return 1;
        }
      }

      if (!(handles[2].flags & LIBCX_HANDLE_NEW))
      {
        printf("handle 2 is not LIBCX_HANDLE_NEW\n");
        return 1;
      }

      /* Pass socket to the child */

      buf[0] = p[1];
      buf[1] = handles[2].value;

      TEST_FORK_PING_CHILD();

      int data = 0;
      rc = TEMP_FAILURE_RETRY(read(p[0], &data, 4));
      if (rc != 4 || data != 0x78563412)
      {
        if (rc < 0)
          perror("read(socket) failed");
        else if (rc != 4)
          printf("read %d bytes instead of 4\n", rc);
        else
          printf("read value 0x%x instead of 0x78563412\n", data);
        return 1;
      }

      close(p[1]);
      close(p[0]);
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

  buf = NULL;

  for (int n = 1; n <= 2; ++n)
  {
    printf("Test 1.2.%d\n", n);

    TEST_FORK_WITH_PIPE_BEGIN("child", 0, 0);
    {
      TEST_FORK_PRINTF("here 1\n");

      LIBCX_HANDLE handles[] = { {LIBCX_HANDLE_SHMEM, 0, h1}, {LIBCX_HANDLE_SHMEM, 0, h2} };
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

      TEST_FORK_WAIT_PARENT_PING();

      TEST_FORK_PRINTF("here 2\n");

      buf = (int*)shmem_map(h1, 0, 0);
      if (buf == NULL)
      {
        TEST_FORK_PERROR("shmem_map(h1) failed");
        return 1;
      }

      LIBCX_HANDLE handles2[] = { {LIBCX_HANDLE_FD, 0, buf[1]} };
      rc = libcx_take_handles(handles2, sizeof(handles2)/sizeof(*handles2), parent_pid,
                              n == 2 ? LIBCX_HANDLE_CLOSE : 0);
      if (rc == -1)
      {
        TEST_FORK_PERROR("libcx_take_handles(p1) failed");
        return 1;
      }

      if (!(handles2[0].flags & LIBCX_HANDLE_NEW))
      {
        TEST_FORK_PRINTF("handle2 0 is not LIBCX_HANDLE_NEW\n");
        return 1;
      }

      int fd = handles2[0].value;

      TEST_FORK_PRINTF("here 3 (source %d, target %d)\n", buf[1], fd);

      __LIBC_PFH pFH;
      pFH = __libc_FH(fd);
      if (!pFH || ((pFH->fFlags & __LIBC_FH_TYPEMASK) != F_SOCKET))
      {
        TEST_FORK_PRINTF("fd %d is not F_SOCKET (pFH %p name [%s] flags 0x%x)\n", fd,
                         pFH, pFH ? pFH->pszNativePath : NULL, pFH ? pFH->fFlags : 0);
        return 1;
      }

      int data = 0;
      rc = TEMP_FAILURE_RETRY(read(fd, &data, 4));
      if (rc != 4 || data != 0x78563412)
      {
        if (rc < 0)
          TEST_FORK_PERROR("read(socket) failed");
        else if (rc != 4)
          TEST_FORK_PRINTF("read %d bytes instead of 4\n", rc);
        else
          TEST_FORK_PRINTF("read value 0x%x instead of 0x78563412\n", data);
        return 1;
      }

      return 0;
    }
    TEST_FORK_WITH_PIPE_PARENT_PART();
    {
      rc = socketpair(AF_LOCAL, SOCK_STREAM, 0, p);
      if (rc == -1)
      {
        perror("socketpair failed");
        return 1;
      }

      printf("here 1 (socketpair %d, %d)\n", p[0], p[1]);

      if (buf == NULL)
      {
        buf = (int*)shmem_map(h1, 0, 0);
        if (buf == NULL)
        {
          perror("shmem_map(h1) failed");
          return 1;
        }
      }

      /* Pass socket to the child */

      buf[1] = p[1];

      TEST_FORK_PING_CHILD();

      rc = TEMP_FAILURE_RETRY(write(p[0], "\x12\x34\x56\x78", 4));
      if (rc == -1)
      {
        perror("write(socket) failed");
        return 1;
      }
    }
    TEST_FORK_WITH_PIPE_END();

    int flags;
    size_t size, act_size;

    __LIBC_PFH pFH;

    if (n == 1)
    {
      rc = shmem_get_info(h1, &flags, &size, &act_size);
      if (rc == -1)
      {
        perror("shmem_get_info(h1) failed");
        return 1;
      }

      pFH = __libc_FH(p[1]);
      if (!pFH)
      {
        printf("socket was already closed\n");
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

      pFH = __libc_FH(p[1]);
      if (pFH)
      {
        printf("socket was NOT already closed\n");
        return 1;
      }
    }

    close(p[1]);
    close(p[0]);
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

    rc = socketpair(AF_LOCAL, SOCK_STREAM, 0, p);
    if (rc == -1)
    {
      TEST_FORK_PERROR("socketpair failed");
      return 1;
    }

    TEST_FORK_PRINTF("here 1 (h3 %d, h4 %d, p0 %d, p1 %d)\n", h3, h4, p[0], p[1]);

    TEST_FORK_WAIT_PARENT_PING();

    SHMEM *mem = shmem_map(h1, 0, 0);
    if (mem == NULL)
    {
      TEST_FORK_PERROR("shmem_map(h1) failed");
      return 1;
    }

    mem[0] = h3;
    mem[1] = h4;
    mem[2] = p[0];
    mem[3] = p[1];

    TEST_FORK_PRINTF("here 2\n");

    TEST_FORK_PING_PARENT();

    TEST_FORK_WAIT_PARENT_PING();

    close(p[1]);

    rc = TEMP_FAILURE_RETRY(write(p[0], "\x12\x34\x56\x78", 4));
    if (rc == -1)
    {
      TEST_FORK_PERROR("write(socket) failed");
      return 1;
    }

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
    p[0] = mem[2];
    p[1] = mem[3];

    printf("here 1 (h3 %d, h4 %d, p0 %d, p1 %d)\n", h3, h4, p[0], p[1]);

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

    LIBCX_HANDLE handles2[] = { {LIBCX_HANDLE_SHMEM, 0, h3}, {LIBCX_HANDLE_SHMEM, 0, h4},
                                {LIBCX_HANDLE_FD, 0, p[1]} };
    rc = libcx_take_handles(handles2, sizeof(handles2)/sizeof(*handles2), child_pid, 0);
    if (rc == -1)
    {
      perror("libcx_take_handles(h3,h4,p1) failed");
      return 1;
    }

    TEST_FORK_PING_CHILD();

    int fd = handles2[2].value;
    if (!(handles2[2].flags & LIBCX_HANDLE_NEW))
    {
      printf("handle 2 is not LIBCX_HANDLE_NEW\n");
      return 1;
    }

    printf("here 2 (socket %d)\n", fd);

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

    __LIBC_PFH pFH = __libc_FH(fd);
    if (!pFH || ((pFH->fFlags & __LIBC_FH_TYPEMASK) != F_SOCKET))
    {
      TEST_FORK_PRINTF("fd %d is not F_SOCKET (pFH %p name [%s] flags 0x%x)\n", fd,
                       pFH, pFH ? pFH->pszNativePath : NULL, pFH ? pFH->fFlags : 0);
      return 1;
    }

    int data = 0;
    rc = TEMP_FAILURE_RETRY(read(fd, &data, 4));
    if (rc != 4 || data != 0x78563412)
    {
      if (rc < 0)
        perror("read(socket) failed");
      else if (rc != 4)
        printf("read %d bytes instead of 4\n", rc);
      else
        printf("read value 0x%x instead of 0x78563412\n", data);
      return 1;
    }
  }
  TEST_FORK_WITH_PIPE_END();

  return 0;
}
