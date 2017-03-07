/*
 * Testcase for edge cases of internal overlapped mappings splitter.
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

#include "mmap.h"

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif

#define PAGES 10

#define FILE_SIZE (PAGE_SIZE * PAGES)

unsigned char buf[FILE_SIZE];

static int do_test(void);
#define TEST_FUNCTION do_test ()
#include "../test-skeleton.c"

static int fd = -1;
static int flags = 0;

static unsigned char *
mmap_pages(int start, int len)
{
  unsigned char *addr = mmap(NULL, PAGE_SIZE * len, PROT_READ, flags, fd, PAGE_SIZE * start);
  if (addr == MAP_FAILED)
  {
    printf("mmap(%d,%d) failed: %s\n", start, len, strerror(errno));
    exit(1);
  }

  return addr;
}

static void
munmap_pages(unsigned char *addr, int len)
{
  if (munmap(addr, PAGE_SIZE * len) == -1)
  {
    printf("munmap(%p,%d) failed: %s\n", addr, len, strerror(errno));
    exit(1);
  }
}

typedef struct test_req
{
  int start;
  int len;
  int cnt;
} test_req;

#define ARRAY_SIZE(a) (sizeof(a)/sizeof(a[0]))

static void
test_mappings(unsigned char *addr, test_req *list, int num)
{
#ifdef DEBUG
  int ok = 0;

  global_lock();

  MemMap *mmaps = get_proc_mmaps(-1);

  do
  {
    if (!addr)
    {
      /* Check for no mmaps! */
      if (mmaps)
      {
        printf("ERROR: mmaps is %p, not NULL\n", mmaps);
        break;
      }
    }
    else
    {
      test_req *r = list;
      MemMap *m = mmaps;
      int n;

      for (n = 0; n < num; ++n)
      {
        unsigned char *start = addr + r->start * PAGE_SIZE;
        unsigned char *end = start + r->len * PAGE_SIZE;

        if (!m)
        {
          printf("ERROR: mmaps is NULL for #%d (start %d, len %d)\n", n, r->start, r->len);
          break;
        }

        if (m->flags & MAP_ANON)
        {
          printf("ERROR: flags %x include %x\n", m->flags, MAP_ANON);
          break;
        }

        if (m->start != (ULONG)start)
        {
          printf("ERROR: start must be %p, not %p\n", start, m->start);
          break;
        }
        if (m->end != (ULONG)end)
        {
          printf("ERROR: start must be %p, not %p\n", end, m->end);
          break;
        }

        if (m->f->refcnt != r->cnt)
        {
          printf("ERROR: refcnt must be %d, not %d\n", r->cnt, m->f->refcnt);
          break;
        }

        ++r;
        m = m->next;
      }

      if (m)
      {
        printf("ERROR: mmaps is %p, not NULL\n", m);
        break;
      }
    }

    ok = 1;
  }
  while(0);

  global_unlock();

  if (!ok)
    exit(1);
#endif
}

/*
 * We only test the layout of mappings in debug builds, no access to it in
 * release builds.
 */
#ifdef DEBUG
#define TEST_MAPPINGS(addr, ...) \
  do \
  { \
    test_req list[] = { __VA_ARGS__ }; \
    test_mappings(addr, list, ARRAY_SIZE(list)); \
  } \
  while(0)
#define TEST_MAPPINGS_NONE() test_mappings(NULL, NULL, 0)
#else
#define TEST_MAPPINGS(addr, ...)
#define TEST_MAPPINGS_NONE()
#endif

/*
 * In release builds we should unmap all addresses as they may be from different
 * memory objects and not doing so will leave a temporary file as removal will
 * fail because of an open file handle (duplicate) in mmap structures.
 */
#ifdef DEBUG
#define MUNMAP_ADDRS(...)
#else
#define MUNMAP_ADDRS(...) \
  do \
  { \
    unsigned char *addrs[] = { __VA_ARGS__ }; \
    int i = 0; \
    for (i = 0; i < ARRAY_SIZE(addrs); ++i) \
      munmap(addrs[i], PAGES); \
  } while(0)
#endif

static int
do_test (void)
{
  int i, n;
  char *fname;

  fd = create_temp_file("tst-mmap9-", &fname);
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

#ifdef DEBUG
  set_mmap_full_size(1);
#endif

  for (i = 0; i < 2; ++i)
  {
    if (i == 0)
    {
      flags = MAP_PRIVATE;
      printf("Testing private mappings...\n");
    }
    else
    {
      flags = MAP_SHARED;
      printf("Testing shared mappings...\n");
    }

    printf("Test [0],[9],[5-6]\n");
    {
      unsigned char *a0 = mmap_pages(0, 1);
      unsigned char *a2 = mmap_pages(9, 1);
      unsigned char *a1 = mmap_pages(5, 2);

      TEST_MAPPINGS(a0,
        { 0, 1, 1 },
        { 5, 2, 1 },
        { 9, 1, 1 },
      );

      munmap_pages(a0, PAGES);
      TEST_MAPPINGS_NONE();

      MUNMAP_ADDRS(a1, a2);
    }

    printf("Test [0],[9],[1-8]\n");
    {
      unsigned char *a0 = mmap_pages(0, 1);
      unsigned char *a2 = mmap_pages(9, 1);
      unsigned char *a1 = mmap_pages(1, 8);

      TEST_MAPPINGS(a0,
        { 0, 1, 1 },
        { 1, 8, 1 },
        { 9, 1, 1 },
      );

      munmap_pages(a0, PAGES);
      TEST_MAPPINGS_NONE();

      MUNMAP_ADDRS(a1, a2);
    }

    printf("Test [0-5],[0-5]\n");
    {
      unsigned char *a0 = mmap_pages(0, 6);
      unsigned char *a1 = mmap_pages(0, 6);

      TEST_MAPPINGS(a0,
        { 0, 6, 2 },
      );

      munmap_pages(a0, PAGES);

      TEST_MAPPINGS(a0,
        { 0, 6, 1 },
      );

      munmap_pages(a0, PAGES);
      TEST_MAPPINGS_NONE();

      MUNMAP_ADDRS(a1);
    }

    printf("Test [0-5],[1-3]\n");
    {
      unsigned char *a0 = mmap_pages(0, 6);
      unsigned char *a1 = mmap_pages(1, 3);

      TEST_MAPPINGS(a0,
        { 0, 1, 1 },
        { 1, 3, 2 },
        { 4, 2, 1 },
      );

      munmap_pages(a0, PAGES);

      TEST_MAPPINGS(a0,
        { 1, 3, 1 },
      );

      munmap_pages(a0, PAGES);
      TEST_MAPPINGS_NONE();

      MUNMAP_ADDRS(a1);
    }

    printf("Test [1-2],[3-4]\n");
    {
      unsigned char *a0 = mmap_pages(1, 2);
      unsigned char *a1 = mmap_pages(3, 2);

      TEST_MAPPINGS(a0,
        { 0, 2, 1 },
        { 2, 2, 1 },
      );

      munmap_pages(a0, PAGES);
      TEST_MAPPINGS_NONE();

      MUNMAP_ADDRS(a1);
    }

    printf("Test [3-4],[1-2]\n");
    {
      unsigned char *a1 = mmap_pages(3, 2);
      unsigned char *a0 = mmap_pages(1, 2);

      TEST_MAPPINGS(a0,
        { 0, 2, 1 },
        { 2, 2, 1 },
      );

      munmap_pages(a0, PAGES);
      TEST_MAPPINGS_NONE();

      MUNMAP_ADDRS(a1);
    }

    printf("Test [0-1],[1-2]\n");
    {
      unsigned char *a0 = mmap_pages(0, 2);
      unsigned char *a1 = mmap_pages(1, 2);

      TEST_MAPPINGS(a0,
        { 0, 1, 1 },
        { 1, 1, 2 },
        { 2, 1, 1 },
      );

      munmap_pages(a0, PAGES);

      TEST_MAPPINGS(a0,
        { 1, 1, 1 }
      );

      munmap_pages(a0, PAGES);
      TEST_MAPPINGS_NONE();

      MUNMAP_ADDRS(a1);
    }

    printf("Test [1-2],[0-1]\n");
    {
      unsigned char *a1 = mmap_pages(1, 2);
      unsigned char *a0 = mmap_pages(0, 2);

      TEST_MAPPINGS(a0,
        { 0, 1, 1 },
        { 1, 1, 2 },
        { 2, 1, 1 },
      );

      munmap_pages(a0, PAGES);

      TEST_MAPPINGS(a0,
        { 1, 1, 1 }
      );

      munmap_pages(a0, PAGES);
      TEST_MAPPINGS_NONE();

      MUNMAP_ADDRS(a1);
    }

    printf("Test [0-1],[0-3]\n");
    {
      unsigned char *a0 = mmap_pages(0, 2);
      unsigned char *a1 = mmap_pages(0, 4);

      TEST_MAPPINGS(a0,
        { 0, 2, 2 },
        { 2, 2, 1 },
      );

      munmap_pages(a0, PAGES);

      TEST_MAPPINGS(a0,
        { 0, 2, 1 }
      );

      munmap_pages(a0, PAGES);
      TEST_MAPPINGS_NONE();

      MUNMAP_ADDRS(a1);
    }

    printf("Test [0-3],[0-1]\n");
    {
      unsigned char *a1 = mmap_pages(0, 4);
      unsigned char *a0 = mmap_pages(0, 2);

      TEST_MAPPINGS(a0,
        { 0, 2, 2 },
        { 2, 2, 1 },
      );

      munmap_pages(a0, PAGES);

      TEST_MAPPINGS(a0,
        { 0, 2, 1 }
      );

      munmap_pages(a0, PAGES);
      TEST_MAPPINGS_NONE();

      MUNMAP_ADDRS(a1);
    }

    printf("Test [6-9],[8-9]\n");
    {
      unsigned char *a0 = mmap_pages(6, 4);
      unsigned char *a1 = mmap_pages(8, 2);

      TEST_MAPPINGS(a0,
        { 0, 2, 1 },
        { 2, 2, 2 },
      );

      munmap_pages(a0, PAGES);

      TEST_MAPPINGS(a0,
        { 2, 2, 1 }
      );

      munmap_pages(a0, PAGES);
      TEST_MAPPINGS_NONE();

      MUNMAP_ADDRS(a1);
    }

    printf("Test [8-9],[6-9]\n");
    {
      unsigned char *a1 = mmap_pages(8, 2);
      unsigned char *a0 = mmap_pages(6, 4);

      TEST_MAPPINGS(a0,
        { 0, 2, 1 },
        { 2, 2, 2 },
      );

      munmap_pages(a0, PAGES);

      TEST_MAPPINGS(a0,
        { 2, 2, 1 }
      );

      munmap_pages(a0, PAGES);
      TEST_MAPPINGS_NONE();

      MUNMAP_ADDRS(a1);
    }

    printf("Test [1],[4-5],[7],[8],[1-8]\n");
    {
      unsigned char *a0 = mmap_pages(1, 1);
      unsigned char *a1 = mmap_pages(4, 2);
      unsigned char *a2 = mmap_pages(7, 1);
      unsigned char *a3 = mmap_pages(8, 1);
      unsigned char *a4 = mmap_pages(1, 8);

      TEST_MAPPINGS(a0,
        { 0, 1, 2 },
        { 1, 2, 1 },
        { 3, 2, 2 },
        { 5, 1, 1 },
        { 6, 1, 2 },
        { 7, 1, 2 },
      );

      munmap_pages(a0, PAGES);

      TEST_MAPPINGS(a0,
        { 0, 1, 1 },
        { 3, 2, 1 },
        { 6, 1, 1 },
        { 7, 1, 1 },
      );

      munmap_pages(a0, PAGES);
      TEST_MAPPINGS_NONE();

      MUNMAP_ADDRS(a1, a2, a3, a4);
    }

    printf("Test [1],[4-5],[7],[8],[0-9]\n");
    {
      unsigned char *a1 = mmap_pages(1, 1);
      unsigned char *a2 = mmap_pages(4, 2);
      unsigned char *a3 = mmap_pages(7, 1);
      unsigned char *a4 = mmap_pages(8, 1);
      unsigned char *a0 = mmap_pages(0, 10);

      TEST_MAPPINGS(a0,
        { 0, 1, 1 },
        { 1, 1, 2 },
        { 2, 2, 1 },
        { 4, 2, 2 },
        { 6, 1, 1 },
        { 7, 1, 2 },
        { 8, 1, 2 },
        { 9, 1, 1 },
      );

      munmap_pages(a0, 9);

      TEST_MAPPINGS(a0,
        { 1, 1, 1 },
        { 4, 2, 1 },
        { 7, 1, 1 },
        { 8, 1, 1 },
        { 9, 1, 1 },
      );

      munmap_pages(a0, PAGES);
      TEST_MAPPINGS_NONE();

      MUNMAP_ADDRS(a1, a2, a3, a4);
    }

    printf("Test [1],[4-5],[7],[8],[3-8]\n");
    {
      unsigned char *a0 = mmap_pages(1, 1);
      unsigned char *a1 = mmap_pages(4, 2);
      unsigned char *a2 = mmap_pages(7, 1);
      unsigned char *a3 = mmap_pages(8, 1);
      unsigned char *a4 = mmap_pages(3, 6);

      TEST_MAPPINGS(a0,
        { 0, 1, 1 },
        { 2, 1, 1 },
        { 3, 2, 2 },
        { 5, 1, 1 },
        { 6, 1, 2 },
        { 7, 1, 2 },
      );

      munmap_pages(a0, PAGES);

      TEST_MAPPINGS(a0,
        { 3, 2, 1 },
        { 6, 1, 1 },
        { 7, 1, 1 },
      );

      munmap_pages(a0, PAGES);
      TEST_MAPPINGS_NONE();

      MUNMAP_ADDRS(a1, a2, a3, a4);
    }

    printf("Test [1],[4-5],[7],[8],[2-8]\n");
    {
      unsigned char *a0 = mmap_pages(1, 1);
      unsigned char *a1 = mmap_pages(4, 2);
      unsigned char *a2 = mmap_pages(7, 1);
      unsigned char *a3 = mmap_pages(8, 1);
      unsigned char *a4 = mmap_pages(2, 7);

      TEST_MAPPINGS(a0,
        { 0, 1, 1 },
        { 1, 2, 1 },
        { 3, 2, 2 },
        { 5, 1, 1 },
        { 6, 1, 2 },
        { 7, 1, 2 },
      );

      munmap_pages(a0, PAGES);

      TEST_MAPPINGS(a0,
        { 3, 2, 1 },
        { 6, 1, 1 },
        { 7, 1, 1 },
      );

      munmap_pages(a0, PAGES);
      TEST_MAPPINGS_NONE();

      MUNMAP_ADDRS(a1, a2, a3, a4);
    }

    printf("Test [1],[4-5],[1-2]\n");
    {
      unsigned char *a0 = mmap_pages(1, 1);
      unsigned char *a1 = mmap_pages(4, 2);
      unsigned char *a2 = mmap_pages(1, 2);

      TEST_MAPPINGS(a0,
        { 0, 1, 2 },
        { 1, 1, 1 },
        { 3, 2, 1 },
      );

      munmap_pages(a0, PAGES);

      TEST_MAPPINGS(a0,
        { 0, 1, 1 },
      );

      munmap_pages(a0, PAGES);
      TEST_MAPPINGS_NONE();

      MUNMAP_ADDRS(a1, a2);
    }

    printf("Test [1],[4-5],[1-3]\n");
    {
      unsigned char *a0 = mmap_pages(1, 1);
      unsigned char *a1 = mmap_pages(4, 2);
      unsigned char *a2 = mmap_pages(1, 3);

      TEST_MAPPINGS(a0,
        { 0, 1, 2 },
        { 1, 2, 1 },
        { 3, 2, 1 },
      );

      munmap_pages(a0, PAGES);

      TEST_MAPPINGS(a0,
        { 0, 1, 1 },
      );

      munmap_pages(a0, PAGES);
      TEST_MAPPINGS_NONE();

      MUNMAP_ADDRS(a1, a2);
    }

    printf("Test [1],[4-5],[1-4]\n");
    {
      unsigned char *a0 = mmap_pages(1, 1);
      unsigned char *a1 = mmap_pages(4, 2);
      unsigned char *a2 = mmap_pages(1, 4);

      TEST_MAPPINGS(a0,
        { 0, 1, 2 },
        { 1, 2, 1 },
        { 3, 1, 2 },
        { 4, 1, 1 },
      );

      munmap_pages(a0, PAGES);

      TEST_MAPPINGS(a0,
        { 0, 1, 1 },
        { 3, 1, 1 },
      );

      munmap_pages(a0, PAGES);
      TEST_MAPPINGS_NONE();

      MUNMAP_ADDRS(a1, a2);
    }

    printf("Test [1],[4-5],[0-4]\n");
    {
      unsigned char *a1 = mmap_pages(1, 1);
      unsigned char *a2 = mmap_pages(4, 2);
      unsigned char *a0 = mmap_pages(0, 5);

      TEST_MAPPINGS(a0,
        { 0, 1, 1 },
        { 1, 1, 2 },
        { 2, 2, 1 },
        { 4, 1, 2 },
        { 5, 1, 1 },
      );

      munmap_pages(a0, PAGES);

      TEST_MAPPINGS(a0,
        { 1, 1, 1 },
        { 4, 1, 1 },
      );

      munmap_pages(a0, PAGES);
      TEST_MAPPINGS_NONE();

      MUNMAP_ADDRS(a1, a2);
    }

    printf("Test [1],[4-5],[0-2]\n");
    {
      unsigned char *a1 = mmap_pages(1, 1);
      unsigned char *a2 = mmap_pages(4, 2);
      unsigned char *a0 = mmap_pages(0, 3);

      TEST_MAPPINGS(a0,
        { 0, 1, 1 },
        { 1, 1, 2 },
        { 2, 1, 1 },
        { 4, 2, 1 },
      );

      munmap_pages(a0, PAGES);

      TEST_MAPPINGS(a0,
        { 1, 1, 1 },
      );

      munmap_pages(a0, PAGES);
      TEST_MAPPINGS_NONE();

      MUNMAP_ADDRS(a1, a2);
    }

    printf("Test [1-2],[4-5],[2-4]\n");
    {
      unsigned char *a0 = mmap_pages(1, 2);
      unsigned char *a1 = mmap_pages(4, 2);
      unsigned char *a2 = mmap_pages(2, 3);

      TEST_MAPPINGS(a0,
        { 0, 1, 1 },
        { 1, 1, 2 },
        { 2, 1, 1 },
        { 3, 1, 2 },
        { 4, 1, 1 },
      );

      munmap_pages(a0, PAGES);

      TEST_MAPPINGS(a0,
        { 1, 1, 1 },
        { 3, 1, 1 },
      );

      munmap_pages(a0, PAGES);
      TEST_MAPPINGS_NONE();

      MUNMAP_ADDRS(a1, a2);
    }

    printf("Test [1-2],[4-5],[0-4]\n");
    {
      unsigned char *a1 = mmap_pages(1, 2);
      unsigned char *a2 = mmap_pages(4, 2);
      unsigned char *a0 = mmap_pages(0, 5);

      TEST_MAPPINGS(a0,
        { 0, 1, 1 },
        { 1, 2, 2 },
        { 3, 1, 1 },
        { 4, 1, 2 },
        { 5, 1, 1 },
      );

      munmap_pages(a0, PAGES);

      TEST_MAPPINGS(a0,
        { 1, 2, 1 },
        { 4, 1, 1 },
      );

      munmap_pages(a0, PAGES);
      TEST_MAPPINGS_NONE();

      MUNMAP_ADDRS(a1, a2);
    }

    printf("Test [1-2],[4-5],[2-6]\n");
    {
      unsigned char *a0 = mmap_pages(1, 2);
      unsigned char *a1 = mmap_pages(4, 2);
      unsigned char *a2 = mmap_pages(2, 5);

      TEST_MAPPINGS(a0,
        { 0, 1, 1 },
        { 1, 1, 2 },
        { 2, 1, 1 },
        { 3, 2, 2 },
        { 5, 1, 1 },
      );

      munmap_pages(a0, PAGES);

      TEST_MAPPINGS(a0,
        { 1, 1, 1 },
        { 3, 2, 1 },
      );

      munmap_pages(a0, PAGES);
      TEST_MAPPINGS_NONE();

      MUNMAP_ADDRS(a1, a2);
    }

    printf("Test [1-2],[4-5],[7-8],[3-4]\n");
    {
      unsigned char *a0 = mmap_pages(1, 2);
      unsigned char *a1 = mmap_pages(4, 2);
      unsigned char *a2 = mmap_pages(7, 2);
      unsigned char *a3 = mmap_pages(3, 2);

      TEST_MAPPINGS(a0,
        { 0, 2, 1 },
        { 2, 1, 1 },
        { 3, 1, 2 },
        { 4, 1, 1 },
        { 6, 2, 1 },
      );

      munmap_pages(a0, PAGES);

      TEST_MAPPINGS(a0,
        { 3, 1, 1 },
      );

      munmap_pages(a0, PAGES);
      TEST_MAPPINGS_NONE();

      MUNMAP_ADDRS(a1, a2, a3);
    }

    printf("Test [1-2],[4-5],[7-8],[5-6]\n");
    {
      unsigned char *a0 = mmap_pages(1, 2);
      unsigned char *a1 = mmap_pages(4, 2);
      unsigned char *a2 = mmap_pages(7, 2);
      unsigned char *a3 = mmap_pages(5, 2);

      TEST_MAPPINGS(a0,
        { 0, 2, 1 },
        { 3, 1, 1 },
        { 4, 1, 2 },
        { 5, 1, 1 },
        { 6, 2, 1 },
      );

      munmap_pages(a0, PAGES);

      TEST_MAPPINGS(a0,
        { 4, 1, 1 },
      );

      munmap_pages(a0, PAGES);
      TEST_MAPPINGS_NONE();

      MUNMAP_ADDRS(a1, a2, a3);
    }

    /*
     * Now some more specific munmap tests.
     */

    printf("Test +[1-8],-[2-7]\n");
    {
      unsigned char *a0 = mmap_pages(1, 8);
      munmap_pages(a0 + PAGE_SIZE, 6);

      TEST_MAPPINGS(a0,
        { 0, 1, 1 },
        { 7, 1, 1 },
      );

      munmap_pages(a0, PAGES);
      TEST_MAPPINGS_NONE();
    }

    printf("Test +[0-9],-[0-9]\n");
    {
      unsigned char *a0 = mmap_pages(0, 10);
      munmap_pages(a0, PAGES);
      TEST_MAPPINGS_NONE();
    }

    printf("Test +[0-1],+[2-3],+[4-5],-[1-4]\n");
    {
      unsigned char *a0 = mmap_pages(0, 2);
      unsigned char *a1 = mmap_pages(2, 2);
      unsigned char *a2 = mmap_pages(4, 2);
      munmap_pages(a0 + PAGE_SIZE, 4);

      TEST_MAPPINGS(a0,
        { 0, 1, 1 },
        { 5, 1, 1 },
      );

      munmap_pages(a0, PAGES);
      TEST_MAPPINGS_NONE();

      MUNMAP_ADDRS(a1, a2);
    }

    printf("Test +[0-1],+[2-3],+[4-5],-[0-4]\n");
    {
      unsigned char *a0 = mmap_pages(0, 2);
      unsigned char *a1 = mmap_pages(2, 2);
      unsigned char *a2 = mmap_pages(4, 2);
      munmap_pages(a0, 5);

      TEST_MAPPINGS(a0,
        { 5, 1, 1 },
      );

      munmap_pages(a0, PAGES);
      TEST_MAPPINGS_NONE();

      MUNMAP_ADDRS(a1, a2);
    }

    printf("Test +[0-1],+[2-3],+[4-5],-[1-5]\n");
    {
      unsigned char *a0 = mmap_pages(0, 2);
      unsigned char *a1 = mmap_pages(2, 2);
      unsigned char *a2 = mmap_pages(4, 2);
      munmap_pages(a0 + PAGE_SIZE, 5);

      TEST_MAPPINGS(a0,
        { 0, 1, 1 },
      );

      munmap_pages(a0, PAGES);
      TEST_MAPPINGS_NONE();

      MUNMAP_ADDRS(a1, a2);
    }
  }

  free(fname);

  return 0;
}
