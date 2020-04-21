/*
 * Testcase for memchr and mmap.
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

static int do_test(void);
#define TEST_FUNCTION do_test ()
#include "../test-skeleton.c"


#if 0

static int
do_test (void)
{
}

#else

#define ASSERT assert
#define HAVE_SYS_MMAN_H 1
#define HAVE_MPROTECT 1
#define HAVE_MAP_ANONYMOUS 1

/* Return a pointer to a zero-size object in memory (that is, actually, a
   pointer to a page boundary where the previous page is readable and writable
   and the next page is neither readable not writable), if possible.
   Return NULL otherwise.  */

static void *
zerosize_ptr (void)
{
/* Use mmap and mprotect when they exist.  Don't test HAVE_MMAP, because it is
   not defined on HP-UX 11 (since it does not support MAP_FIXED).  */
#if HAVE_SYS_MMAN_H && HAVE_MPROTECT
# if HAVE_MAP_ANONYMOUS
  const int flags = MAP_ANONYMOUS | MAP_PRIVATE;
  const int fd = -1;
# else /* !HAVE_MAP_ANONYMOUS */
  const int flags = MAP_FILE | MAP_PRIVATE;
  int fd = open ("/dev/zero", O_RDONLY, 0666);
  if (fd >= 0)
# endif
    {
      int pagesize = getpagesize ();
      char *two_pages =
        (char *) mmap (NULL, 2 * pagesize, PROT_READ | PROT_WRITE,
                       flags, fd, 0);
      if (two_pages != (char *)(-1)
          && mprotect (two_pages + pagesize, pagesize, PROT_NONE) == 0)
//          && munmap (two_pages + pagesize, pagesize) == 0)
        return two_pages + pagesize;
    }
#endif
  return NULL;
}

/* Calculating void * + int is not portable, so this wrapper converts
   to char * to make the tests easier to write.  */
#define MEMCHR (char *) memchr

static int
do_test (void)
{
  size_t n = 0x100000;
  char *input = malloc (n);
  ASSERT (input);

  input[0] = 'a';
  input[1] = 'b';
  memset (input + 2, 'c', 1024);
  memset (input + 1026, 'd', n - 1028);
  input[n - 2] = 'e';
  input[n - 1] = 'a';

  /* Basic behavior tests.  */
  ASSERT (MEMCHR (input, 'a', n) == input);

  ASSERT (MEMCHR (input, 'a', 0) == NULL);
  printf ("Test 1\n");
  ASSERT (MEMCHR (zerosize_ptr (), 'a', 0) == NULL);
  printf ("Test 2\n");

  ASSERT (MEMCHR (input, 'b', n) == input + 1);
  ASSERT (MEMCHR (input, 'c', n) == input + 2);
  ASSERT (MEMCHR (input, 'd', n) == input + 1026);

  ASSERT (MEMCHR (input + 1, 'a', n - 1) == input + n - 1);
  ASSERT (MEMCHR (input + 1, 'e', n - 1) == input + n - 2);
  ASSERT (MEMCHR (input + 1, 0x789abc00 | 'e', n - 1) == input + n - 2);

  ASSERT (MEMCHR (input, 'f', n) == NULL);
  ASSERT (MEMCHR (input, '\0', n) == NULL);

  /* Check that a very long haystack is handled quickly if the byte is
     found near the beginning.  */
  {
    size_t repeat = 10000;
    for (; repeat > 0; repeat--)
      {
        ASSERT (MEMCHR (input, 'c', n) == input + 2);
      }
  }

  /* Alignment tests.  */
  {
    int i, j;
    for (i = 0; i < 32; i++)
      {
        for (j = 0; j < 256; j++)
          input[i + j] = j;
        for (j = 0; j < 256; j++)
          {
            ASSERT (MEMCHR (input + i, j, 256) == input + i + j);
          }
      }
  }

  printf ("Test 3.1\n");

  /* Check that memchr() does not read past the first occurrence of the
     byte being searched.  See the Austin Group's clarification
     <http://www.opengroup.org/austin/docs/austin_454.txt>.
     Test both '\0' and something else, since some implementations
     special-case searching for NUL.
  */
  {
    char *page_boundary = (char *) zerosize_ptr ();
    /* Too small, and we miss cache line boundary tests; too large,
       and the test takes cubically longer to complete.  */
    int limit = 257;

    if (page_boundary != NULL)
      {
        for (n = 1; n <= limit; n++)
          {
            printf ("Test 3.2.1: %d\n", n);
            char *mem = page_boundary - n;
            printf ("Test 3.2.2\n");
            memset (mem, 'X', n);
            printf ("Test 3.2.3\n");
            ASSERT (MEMCHR (mem, 'U', n) == NULL);
            printf ("Test 3.2.4\n");
            ASSERT (MEMCHR (mem, 0, n) == NULL);

            printf ("Test 3.2.5\n");
            {
              size_t i;
              size_t k;

              for (i = 0; i < n; i++)
                {
                  mem[i] = 'U';
                  for (k = i + 1; k < n + limit; k++)
                    ASSERT (MEMCHR (mem, 'U', k) == mem + i);
                  mem[i] = 0;
                  for (k = i + 1; k < n + limit; k++)
                    ASSERT (MEMCHR (mem, 0, k) == mem + i);
                  mem[i] = 'X';
                }
            }
          }
      }
  }

  free (input);

  return 0;
}

#endif
