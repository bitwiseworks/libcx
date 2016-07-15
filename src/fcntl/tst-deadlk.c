/* Copyright (C) 2016 bww bitwise works GmbH.
   This file is part of the kLIBC Extension Library.
   Authored by Dmitry Kuminov <coding@dmik.org>, 2016.

   The kLIBC Extension Library is free software; you can redistribute it
   and/or modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   The kLIBC Extension Library is distributed in the hope that it will be
   useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
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
#include <errno.h>
#include <sys/wait.h>

/* This test case is roughly based on this snippet:
   http://stackoverflow.com/questions/27694721/why-dont-i-see-deadlock-edeadlk-when-several-processes-lock-the-same-fd-with */

static int do_test(void);
#define TEST_FUNCTION do_test()
#include "../test-skeleton.c"

static int
do_test (void)
{
  int fd = create_temp_file("tst-deadlk-", NULL);
  if (fd == -1)
    {
      puts ("create_temp_file failed");
      return 1;
    }

  pid_t pid1, pid2;

  struct flock fl;

  fl.l_type   = F_WRLCK;
  fl.l_whence = SEEK_SET;
  fl.l_start  = 0;
  fl.l_len    = 2; /* lock "doors" (i.e. bytes)  0 and 1 */

  if (TEMP_FAILURE_RETRY (fcntl (fd, F_SETLK,&fl)) == -1)
    {
      perror ("fcntl failed");
      return 1;
    }

  if ((pid1 = fork ()))
    {
      /* parent */
      if ((pid2 = fork ()))
        {
          /* still parent */
          fl.l_type = F_UNLCK;
          fl.l_len  = 2; /* unlock both doors: let the fun begin :-) */

          sleep (2);

          printf("Parent %d is unlocking lock...\n", getpid());
          if (TEMP_FAILURE_RETRY (fcntl (fd, F_SETLKW, &fl)) == -1)
            {
              perror ("fcntl failed");
              return 1;
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

          return status1 ? status1 : status2 ? status2 : 0;
        }
    }

  if (pid1 == -1 || pid2 == -1)
    {
      perror ("fork failed");
      return 1;
    }

  if (!pid1 || !pid2)
    {
      /* in child 1 or child 2 */
      printf("Child %d started\n", getpid());

      int offset0 = (pid1 ? 0 : 1);  /* child1 gets 0 and 1, child 2 gets 1 and 0 */
      int offset1 = (pid1 ? 1 : 0);
      fl.l_len = 1;

      fl.l_start =  offset0; /* lock door 0 (1) as soon as parent lets us*/
      printf("Child %d locking byte %d...\n", getpid(), offset0);
      if (TEMP_FAILURE_RETRY (fcntl (fd, F_SETLKW,&fl) == -1))
        {
          printf ("Child %d fcntl failed (%s)", getpid(), strerror (errno));
          return errno == EDEADLK ? 0 : 1;
        }

      sleep (1); /* guarantee that the other child has our next door locked ... */

      printf("Child %d locking byte %d...\n", getpid(), offset1);
      fl.l_start =  offset1; /* lock door 1 (0). The second of both children who gets here closes the circle and faces deadlock */
      if (TEMP_FAILURE_RETRY (fcntl(fd, F_SETLKW,&fl) == -1))
        {
          printf ("Child %d fcntl failed (%s)\n", getpid(), strerror (errno));
          return errno == EDEADLK ? 0 : 1;
        }

      printf("Child %d terminating (and releasing its lock)\n", getpid()); /* Falling off the end of main() will release the lock anyway */
    }

  return 0;
}
