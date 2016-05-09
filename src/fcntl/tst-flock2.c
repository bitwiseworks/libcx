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

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/wait.h>

#ifdef __EMX__
#define INCL_BASE
#include <os2.h>

typedef struct pthread_barrier_t
{
  HMTX hmtx;
  HEV hev;
  size_t cnt;
  size_t cnt_max;
} pthread_barrier_t;

static void pthread_barrier_wait (pthread_barrier_t *b)
{
  APIRET arc;
  int wait = 1;
  arc = DosRequestMutexSem (b->hmtx, SEM_INDEFINITE_WAIT);
  if (arc)
    {
      puts ("DosRequestMutexSem failed");
      exit (1);
    }
  if (++b->cnt == b->cnt_max)
    {
      b->cnt = 0;
      wait = 0;
      arc = DosPostEventSem (b->hev);
      if (arc)
        {
          puts ("DosPostEventSem failed");
          exit (1);
        }
    }
  arc = DosReleaseMutexSem (b->hmtx);
  if (arc)
    {
      puts ("DosReleaseMutexSem failed");
      exit (1);
    }
  if (wait)
    {
      arc = DosWaitEventSem (b->hev, SEM_INDEFINITE_WAIT);
      if (arc)
        {
          puts ("DosWaitEventSem failed");
          exit (1);
        }
    }
}
#endif

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t lock2 = PTHREAD_MUTEX_INITIALIZER;
static int fd;


static void *
tf (void *arg)
{
  struct flock fl =
    {
      .l_type = F_WRLCK,
      .l_start = 0,
      .l_whence = SEEK_SET,
      .l_len = 10
    };
  if (TEMP_FAILURE_RETRY (fcntl (fd, F_SETLKW, &fl)) != 0)
    {
      puts ("fourth fcntl failed");
      exit (1);
    }

  pthread_mutex_unlock (&lock);

  pthread_mutex_lock (&lock2);

  return NULL;
}


static int
do_test (void)
{
  char tmp[] = "/tmp/tst-flock2-XXXXXX";

  fd = mkstemp (tmp);
  if (fd == -1)
    {
      puts ("mkstemp failed");
      return 1;
    }

  unlink (tmp);

  int i;
  for (i = 0; i < 20; ++i)
    write (fd, "foobar xyzzy", 12);

  pthread_barrier_t *b;
#ifdef __EMX__
  APIRET arc;
  arc = DosAllocSharedMem ((PPVOID) &b, NULL, sizeof (pthread_barrier_t),
                           PAG_READ | PAG_WRITE | PAG_COMMIT | OBJ_GETTABLE);
  if (arc)
    {
      puts ("DosAllocSharedMem failed");
      return 1;
    }
  b->hmtx = NULLHANDLE;
  arc = DosCreateMutexSem (NULL, &b->hmtx, DC_SEM_SHARED, FALSE);
  if (arc)
    {
      puts ("DosCreateMutexSem failed");
      return 1;
    }
  b->hev = NULLHANDLE;
  arc = DosCreateEventSem (NULL, &b->hev, DC_SEM_SHARED | DCE_AUTORESET, FALSE);
  if (arc)
    {
      puts ("DosCreateEventSem failed");
      return 1;
    }
  b->cnt = 0;
  b->cnt_max = 2;
#else
  b = mmap (NULL, sizeof (pthread_barrier_t), PROT_READ | PROT_WRITE,
	    MAP_SHARED, fd, 0);
  if (b == MAP_FAILED)
    {
      puts ("mmap failed");
      return 1;
    }

  pthread_barrierattr_t ba;
  if (pthread_barrierattr_init (&ba) != 0)
    {
      puts ("barrierattr_init failed");
      return 1;
    }

  if (pthread_barrierattr_setpshared (&ba, PTHREAD_PROCESS_SHARED) != 0)
    {
      puts ("barrierattr_setpshared failed");
      return 1;
    }

  if (pthread_barrier_init (b, &ba, 2) != 0)
    {
      puts ("barrier_init failed");
      return 1;
    }

  if (pthread_barrierattr_destroy (&ba) != 0)
    {
      puts ("barrierattr_destroy failed");
      return 1;
    }
#endif

  struct flock fl =
    {
      .l_type = F_WRLCK,
      .l_start = 0,
      .l_whence = SEEK_SET,
      .l_len = 10
    };
  if (TEMP_FAILURE_RETRY (fcntl (fd, F_SETLKW, &fl)) != 0)
    {
      puts ("first fcntl failed");
      return 1;
    }

  pid_t pid = fork ();
  if (pid == -1)
    {
      puts ("fork failed");
      return 1;
    }

  if (pid == 0)
    {
#ifdef __EMX__
      arc = DosGetSharedMem (b, PAG_READ | PAG_WRITE);
      if (arc)
        {
          puts ("DosGetSharedMem failed");
          return 1;
        }
      arc = DosOpenMutexSem (NULL, &b->hmtx);
      if (arc)
        {
          puts ("DosOpenMutexSem failed");
          return 1;
        }
      arc = DosOpenEventSem (NULL, &b->hev);
      if (arc)
        {
          puts ("DosOpenEventSem failed");
          return 1;
        }
#endif
      /* Make sure the child does not stay around indefinitely.  */
      alarm (10);

      /* Try to get the lock.  */
      if (TEMP_FAILURE_RETRY (fcntl (fd, F_SETLK, &fl)) == 0)
	{
	  puts ("child:  second flock succeeded");
	  return 1;
	}
    }

  pthread_barrier_wait (b);

  if (pid != 0)
    {
      fl.l_type = F_UNLCK;
      if (TEMP_FAILURE_RETRY (fcntl (fd, F_SETLKW, &fl)) != 0)
	{
	  puts ("third fcntl failed");
	  return 1;
	}
    }

  pthread_barrier_wait (b);

  pthread_t th;
  if (pid == 0)
    {
      if (pthread_mutex_lock (&lock) != 0)
	{
	  puts ("1st locking of lock failed");
	  return 1;
	}

      if (pthread_mutex_lock (&lock2) != 0)
	{
	  puts ("1st locking of lock2 failed");
	  return 1;
	}

      if (pthread_create (&th, NULL, tf, NULL) != 0)
	{
	  puts ("pthread_create failed");
	  return 1;
	}

      if (pthread_mutex_lock (&lock) != 0)
	{
	  puts ("2nd locking of lock failed");
	  return 1;
	}

      puts ("child locked file");
    }

  pthread_barrier_wait (b);

  if (pid != 0)
    {
      fl.l_type = F_WRLCK;
      if (TEMP_FAILURE_RETRY (fcntl (fd, F_SETLK, &fl)) == 0)
	{
	  puts ("fifth fcntl succeeded");
	  return 1;
	}

      puts ("file locked by child");
    }

  pthread_barrier_wait (b);

  if (pid == 0)
    {
      if (pthread_mutex_unlock (&lock2) != 0)
	{
	  puts ("unlock of lock2 failed");
	  return 1;
	}

      if (pthread_join (th, NULL) != 0)
	{
	  puts ("join failed");
	  return 1;
	}

      puts ("child's thread terminated");
    }

  pthread_barrier_wait (b);

  if (pid != 0)
    {
      fl.l_type = F_WRLCK;
      if (TEMP_FAILURE_RETRY (fcntl (fd, F_SETLK, &fl)) == 0)
	{
	  puts ("fifth fcntl succeeded");
	  return 1;
	}

      puts ("file still locked");
    }

  pthread_barrier_wait (b);

  if (pid == 0)
    {
      _exit (0);
    }

  int status;
  if (TEMP_FAILURE_RETRY (waitpid (pid, &status, 0)) != pid)
    {
      puts ("waitpid failed");
      return 1;
    }
  puts ("child terminated");

  if (TEMP_FAILURE_RETRY (fcntl (fd, F_SETLKW, &fl)) != 0)
    {
      puts ("sixth fcntl failed");
      return 1;
    }

  return status;
}

#define TEST_FUNCTION do_test ()
#include "../test-skeleton.c"
