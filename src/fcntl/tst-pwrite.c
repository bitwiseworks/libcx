/* Copyright (C) 2002-2016 Free Software Foundation, Inc.
   This file is part of the GNU C Library.
   Contributed by Ulrich Drepper <drepper@redhat.com>, 2002.

   The GNU C Library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   The GNU C Library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with the GNU C Library; if not, see
   <http://www.gnu.org/licenses/>.  */

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/stat.h>

#ifndef _GNU_SOURCE
#define TEMP_FAILURE_RETRY
#endif

#define FILE_SIZE 128
#define ITERATIONS 1000000

char buf[FILE_SIZE];

static int
do_test (void)
{
  int rc;
  int i, n;
  char tmp [] = "/tmp/tst-pwrite-XXXXXX";

  int fd = mkstemp (tmp);
  if (fd == -1)
    {
      puts ("mkstemp failed");
      return 1;
    }

#ifdef __OS2__
  setmode (fd, O_BINARY);
#endif

  for (i = 0; i < FILE_SIZE; ++i)
    buf[i] = i;

  if (TEMP_FAILURE_RETRY (pwrite(fd, buf, FILE_SIZE, 0)) != FILE_SIZE)
    {
      puts ("pwrite failed");
      return 1;
    }

  struct stat st;
  pid_t pid1, pid2;

  if ((pid1 = fork ()))
    {
      /* parent */
      if ((pid2 = fork ()))
        {
          /* still parent */
          int rc = 0;
          int iter;
          for (iter = 0; iter < ITERATIONS; ++iter)
          {
            if (TEMP_FAILURE_RETRY ((n = pread(fd, buf, FILE_SIZE, 0))) != FILE_SIZE)
              {
                printf ("pread failed (read %d instead of %d)\n", n, FILE_SIZE);
                rc = 1;
                break;
              }

            for (i = 0; i < FILE_SIZE; ++i)
              {
                if (buf[i] != i)
                {
                  printf ("integrity of %s broken (offset %d)\n", tmp, i);
                  rc = 1;
                  break;
                }
              }

            if (fstat(fd, &st))
              {
                puts ("fstat failed");
                rc = 1;
                break;
              }

            if ((int)st.st_size != FILE_SIZE)
              {
                printf ("file size is wrong (%d instead of %d)\n", (int)st.st_size, FILE_SIZE);
                rc = 1;
                break;
              }
          }

          if (rc == 1)
          {
            kill (pid1, SIGKILL);
            kill (pid2, SIGKILL);
          }

          int status1, status2;

          if (TEMP_FAILURE_RETRY (waitpid (pid1, &status1, 0)) != pid1)
            {
              perror ("waitpid failed");
              return 1;
            }
          if (TEMP_FAILURE_RETRY (waitpid (pid2, &status2, 0)) != pid2)
            {
              perror ("waitpid failed");
              return 1;
            }

          unlink (tmp);

          return status1 ? status1 : status2 ? status2 : 0;
        }
    }

  if (pid1 == -1 || pid2 == -1)
    {
      perror ("fork failed");
      return 1;
    }

  /* in child 1 or child 2 */
  printf("Child %d (%d) started\n", getpid(), !!pid1);

  /* child 1 writes to even blocks of 4 bytes, child 2 - to odd block */

#if 0
  int start = pid1 ? 0 : FILE_SIZE / 2;
  int len = FILE_SIZE / 2;
#else
  srand (getpid ());
#endif

  int iter;
  for (iter = 0; iter < ITERATIONS; ++iter)
  {
    int len = 4;
    int start = rand () % (FILE_SIZE / (2 * len));
    if (pid1)
      start = start * (2 * len);
    else
      start = start * (2 * len) + len;
    for (i = start; i < start + len; ++i)
      {
        if (TEMP_FAILURE_RETRY (n = pwrite(fd, buf + start, len, start)) != len)
          {
            printf ("pwrite failed in child %d (wrote %d instead of %d)\n", !!pid1, n, len);
            return 1;
          }
      }
  }

  return 0;
}

#ifndef _GNU_SOURCE
int main ()
{
  return do_test ();
}
#else
#define TEST_FUNCTION do_test ()
#include "../test-skeleton.c"
#endif
