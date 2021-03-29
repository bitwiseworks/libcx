/*
 * Testcase for spawn2 API.
 * Copyright (C) 2017 bww bitwise works GmbH.
 * This file is part of the kLIBC Extension Library.
 * Authored by Dmitry Kuminov <coding@dmik.org>, 2017.
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

#define INCL_BASE
#include <os2.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <alloca.h>
#include <sys/fmutex.h>
#include <sys/socket.h>

#include "libcx/spawn2.h"

#include "../test-skeleton.c"

static int wait_pid(const char *msg, int pid, int exit_code, int exit_signal)
{
  int status;
  int rc = waitpid(pid, &status, 0);
  if (rc == -1)
    perrno_and(return 1, "%s: waitpid %x", msg, pid);

  if (exit_signal)
  {
    if (!WIFSIGNALED(status) || WTERMSIG(status) != exit_signal)
      perr_and(return 1, "%s: waitpid %x status %x (expected signal %d, got %d)",
               msg, pid, status, exit_signal, WTERMSIG(status));
  }
  else
  {
    if (!WIFEXITED(status) || WEXITSTATUS(status) != exit_code)
      perr_and(return 1, "%s: waitpid %x status %x (expected exit code %d, got %d)",
               msg, pid, status, exit_code, WEXITSTATUS(status));
  }

  return 0;
}

static char *suppress_logging()
{
  char *logging = getenv("LIBCX_TRACE");
  if (logging)
    logging = strdup(logging);
  setenv("LIBCX_TRACE", "-all", 1);
  return logging;
}

static void restore_logging(char *logging)
{
  if (logging)
  {
    setenv("LIBCX_TRACE", logging, 1);
    free(logging);
  }
  else
    unsetenv("LIBCX_TRACE");
}

static void test12_thread(void *arg);
static volatile unsigned int test12_done = 0;

static char exename[PATH_MAX] = {0};

#define SPAWN2TEST54_VAR "SPAWN2TEST54"
#define SPAWN2TEST54_VAL "TEST54"

#define SPAWN2TEST55_BLP_VAL "D:/NONEXISTENT"

int do_test(int argc, const char *const *argv)
{
  int rc;

  if (argc > 1)
  {
    // child process

    int n = atoi(argv[1]);

    switch (n)
    {
      case 5:
      {
        int nn = argc > 2 ? atoi(argv[2]) : 0;

        switch (nn)
        {
          case 1: // crash?
            __builtin_trap(); // generates SIGILL
            return 123;

          case 4: // environment test
          {
            char *val = getenv(SPAWN2TEST54_VAR);
            if (!val || strcmp(val, SPAWN2TEST54_VAL))
              perr_and(return 1, "child: test 5.4: [%s] != [%s]", val, SPAWN2TEST54_VAL);

            // also make sure there are no duplicates
            int len = strlen(SPAWN2TEST54_VAR);
            int cnt = 0;
            char **e = environ;
            while (*e)
            {
              if (strnicmp(*e, SPAWN2TEST54_VAR, len) == 0 && (*e)[len] == '=')
                ++cnt;
              ++e;
            }
            if (cnt != 1)
              perr_and(return 1, "child: test 5.4: cnt of [%s] %d != 1", SPAWN2TEST54_VAR, cnt);

            return 0;
          }

          case 5: // BEGINLIBPATH test
          {
            char *val = getenv(SPAWN2TEST54_VAR);
            if (!val || strcmp(val, SPAWN2TEST54_VAL))
              perr_and(return 1, "child: test 5.5: [%s] != [%s]", val, SPAWN2TEST54_VAL);

            val = getenv("BEGINLIBPATH");
            if (val)
              perr_and(return 1, "child: test 5.5: BEGINLIBPATH env var = [%s]", val);

            char blp[1024];
            APIRET arc;

            arc = DosQueryExtLIBPATH(blp, BEGIN_LIBPATH);
            if (arc != NO_ERROR)
              perr_and(return 1, "child: test 5.5: DosQueryExtLIBPATH 1 returned %ld", arc);

            if (!strstr(blp, ";" SPAWN2TEST55_BLP_VAL ";"))
              perr_and(return 1, "child: test 5.5: [%s] doesn't contain [%s]", blp, SPAWN2TEST54_VAL);

            return 0;
          }

          case 6: // environment removal test
          {
            char *val = getenv(SPAWN2TEST54_VAR);
            if (val)
              perr_and(return 1, "child: test 5.6: [%s] is set to [%s], should be unset", SPAWN2TEST54_VAR, val);

            return 0;
          }

          default:
            return 123;
        }
      }

      case 6:
      {
        if (argc < 3)
          return 100;
        char *curdir = getcwd(NULL, 0);
        if (curdir == NULL)
          return 101;
        if (chdir(argv[2]) == -1)
          return 102;
        char *curdir2 = getcwd(NULL, 0);
        if (curdir2 == NULL)
          return 103;
        if (strcmp(curdir, curdir2) != 0)
          perr_and(return 1, "child: curdir: [%s] != [%s]", curdir, curdir2);
        free(curdir);
        free(curdir2);
        break;
      }

      case 7:
      {
        printf("CHILD TEST 7");
        break;
      }

      case 8:
      {
        fprintf(stderr, "CHILD TEST 8");
        break;
      }

      case 9:
      {
        fprintf(stderr, "CHILD TEST 9");
        break;
      }

      case 10:
      {
        int ofd = 2; // stderr by default

        int nn = argc > 2 ? atoi(argv[2]) : 0;
        if (nn == 2)
          ofd = 1234; // hard-coded fd

        char buf[1024];
        rc = read(0, buf, sizeof(buf) - 1);
        if (rc == -1)
          return 100;

        buf[rc] = 0;

        write(ofd, buf, strlen(buf));
        break;
      }

      case 11:
      {
        if (argc < 3)
          return 100;

        const char *s = argv[2];
        char *se = NULL;
        int i;
        for (i = 0; i < 10; ++i)
        {
          int fd = strtol(s, &se, 10);
          if (s == se)
            return 101;
          s = se;

          int f = fcntl(fd, F_GETFD);
          if (f != -1)
            return 102;
        }

        break;
      }

      case 12:
      {
        char buf[1024];
        rc = read(0, buf, sizeof(buf) - 1);
        if (rc == -1)
          return 100;

        buf[rc] = 0;

        // process numbers mismatch due to P_2_THREADSAFE
#if 0
        strcat(buf, " ");
        _itoa(_getpid(), buf + strlen(buf), 16);
#endif

        write(2, buf, strlen(buf));
        break;
      }

      default:
        return 127;
    }

    return 0;
  }

  _execname(exename, PATH_MAX);

  // args check, should fail
  printf("test 1\n");
  rc = spawn2(P_NOWAIT, NULL, NULL, NULL, NULL, NULL);
  if (rc != -1 || errno != EINVAL)
    perr_and(return 1, "test 1: rc %d, not 0; or errno %d, not EINVAL", rc, errno);

  // args check, should fail
  printf("test 2\n");
  rc = spawn2(P_NOWAIT, argv[0], NULL, NULL, NULL, NULL);
  if (rc != -1 || errno != EINVAL)
    perr_and(return 1, "test 2: rc %d, not 0; or errno %d, not EINVAL", rc, errno);

  // args check, should fail
  printf("test 3\n");
  {
    const char *args[] = { NULL };
    rc = spawn2(P_NOWAIT, "something|non|existent", args, NULL, NULL, NULL);
    if (rc != -1 || errno != EINVAL)
      perr_and(return 1, "test 3: rc %d, not 0; or errno %d, not EINVAL", rc, errno);
  }

  // program presence check, should fail
  printf("test 4\n");
  {
    const char *args[] = { "blah", NULL };
    rc = spawn2(P_NOWAIT, "something|non|existent", args, NULL, NULL, NULL);
    if (rc != -1 || errno != ENOENT)
      perr_and(return 1, "test 4: rc %d, not 0; or errno %d, not ENOENT", rc, errno);
  }

  int iter = 1;
  for (; iter <= 2; ++iter)
  {
    int flags = iter == 1 ? 0 : P_2_THREADSAFE;

    // simple spawn, should succeed
    printf("test 5.1 (iter %d)\n", iter);
    {
      const char *args[] = { exename, "--direct", "5", NULL };
      int pid = spawn2(P_NOWAIT | flags, exename, args, NULL, NULL, NULL);
      if (pid == -1)
        perrno_and(return 1, "test 5.1: spawn2");

      if (wait_pid("test 5.1", pid, 123, 0))
        return 1;
    }

    // simple spawn (P_WAIT), should succeed
    printf("test 5.2 (iter %d)\n", iter);
    {
      const char *args[] = { exename, "--direct", "5", NULL };
      int rc = spawn2(P_WAIT | flags, exename, args, NULL, NULL, NULL);
      if (rc != 123)
        perr_and(return 1, "test 5.2: rc %d not 123 (errno %d)", rc, errno);
    }

    // simple spawn, should trap
    printf("test 5.3 (iter %d)\n", iter);
    {
      const char *args[] = { exename, "--direct", "5", "1", NULL };
      int pid = spawn2(P_NOWAIT | flags, exename, args, NULL, NULL, NULL);
      if (pid == -1)
        perrno_and(return 1, "test 5.3: spawn2");

      if (wait_pid("test 5.3", pid, 0, SIGILL))
        return 1;
    }

    // environment test, should succeed
    printf ("test 5.4.1 (iter %d)\n", iter);
    {
      char **e;
      const char **envp;
      int envc, i;
      APIRET arc;

      envc = 0;
      for (e = environ; *e; ++e)
        ++envc;

      envp = (const char **)alloca(sizeof(char *) * (envc + 3));
      for (i = 0; i < envc; ++i)
        envp[i] = environ[i];
      envp[i++] = SPAWN2TEST54_VAR "=" SPAWN2TEST54_VAL;
      envp[i] = NULL;

      const char *args[] = { exename, "--direct", "5", "4", NULL };
      int pid = spawn2(P_NOWAIT | flags, exename, args, NULL, envp, NULL);
      if (pid == -1)
        perrno_and(return 1, "test 5.4.1: spawn2");

      if (wait_pid("test 5.4.1", pid, 0, 0))
        return 1;
    }

    // environment test with P_2_APPENDENV, should succeed
    printf ("test 5.4.2 (iter %d)\n", iter);
    {
      // Simulate another value to check for override
      putenv(SPAWN2TEST54_VAR "=" SPAWN2TEST54_VAL SPAWN2TEST54_VAL);

      const char *args[] = { exename, "--direct", "5", "4", NULL };
      const char *envp[] = { SPAWN2TEST54_VAR "=" SPAWN2TEST54_VAL,
                             NULL };
      int pid = spawn2(P_NOWAIT | P_2_APPENDENV | flags, exename, args, NULL, envp, NULL);
      if (pid == -1)
        perrno_and(return 1, "test 5.4.2: spawn2");

      if (wait_pid("test 5.4.2", pid, 0, 0))
        return 1;

      unsetenv(SPAWN2TEST54_VAR);
    }

    // BEGINLIBPATH override test should succeed
    printf ("test 5.5 (iter %d)\n", iter);
    {
      char blp_before[1024], blp_after[1024]; // OS/2 limit
      char blp[1024 + 256], *blp_val;
      APIRET arc;

      arc = DosQueryExtLIBPATH(blp_before, BEGIN_LIBPATH);
      if (arc != NO_ERROR)
        perr_and(return 1, "test 5.4: DosQueryExtLIBPATH 1 returned %ld", arc);

      strcpy(blp, "BEGINLIBPATH=");
      blp_val = blp + strlen(blp);
      strcpy(blp_val, blp_before);
      strcat(blp_val, ";" SPAWN2TEST55_BLP_VAL);

      const char *args[] = { exename, "--direct", "5", "5", NULL };
      const char *envp[] = { SPAWN2TEST54_VAR "=" SPAWN2TEST54_VAL, blp, NULL };
      int pid = spawn2(P_NOWAIT | P_2_APPENDENV | flags, exename, args, NULL, envp, NULL);
      if (pid == -1)
        perrno_and(return 1, "test 5.5: spawn2");

      if (wait_pid("test 5.5", pid, 0, 0))
        return 1;

      arc = DosQueryExtLIBPATH(blp_after, BEGIN_LIBPATH);
      if (arc != NO_ERROR)
        perr_and(return 1, "test 5.5: DosQueryExtLIBPATH 2 returned %ld", arc);

      if (strcmp(blp_before, blp_after) != 0)
        perr_and(return 1, "test 5.5: [%s] != [%s]", blp_before, blp_after);
    }

    // environment test with P_2_APPENDENV and variable removal, should succeed
    printf ("test 5.6 (iter %d)\n", iter);
    {
      // Make sure the variable is there
      putenv(SPAWN2TEST54_VAR "=" SPAWN2TEST54_VAL);

      const char *args[] = { exename, "--direct", "5", "6", NULL };
      const char *envp[] = { SPAWN2TEST54_VAR, NULL };
      int pid = spawn2(P_NOWAIT | P_2_APPENDENV | flags, exename, args, NULL, envp, NULL);
      if (pid == -1)
        perrno_and(return 1, "test 5.6: spawn2");

      if (wait_pid("test 5.6", pid, 0, 0))
        return 1;

      unsetenv(SPAWN2TEST54_VAR);
    }

    // cwd check, should succeed
    printf ("test 6 (iter %d)\n", iter);
    {
      char *curdir = getcwd(NULL, 0);
      if (curdir == NULL)
        perrno_and(return 1, "test 6: getcwd");

      char *tmpdir = getenv("TEMP");
      if (tmpdir == NULL)
        tmpdir = getenv("TMPDIR");
      if (tmpdir == NULL)
        perr_and(return 1, "test 6: no TEMP/TMPDIR");

      const char *args[] = { exename, "--direct", "6", tmpdir, NULL };
      int pid = spawn2(P_NOWAIT | flags, exename, args, tmpdir, NULL, NULL);
      if (pid == -1)
        perrno_and(return 1, "test 6: spawn2");

      if (wait_pid("test 6", pid, 0, 0))
        return 1;

      char *curdir2 = getcwd(NULL, 0);
      if (strcmp(curdir, curdir2) != 0)
        perr_and(return 1, "test 6: curdir: [%s] != [%s]", curdir, curdir2);

      free(curdir2);
      free(curdir);
    }

    // stdout pipe check, should succeed
    printf ("test 7 (iter %d)\n", iter);
    {
      // forbid our own logging as we catch stdout
      char *logging = suppress_logging();

      int p[2];
      rc = pipe(p);
      if (rc == -1)
        perrno_and(return 1, "test 7: pipe");

      int stdfds[3] = { 0, p[1], 0 };

      const char *args[] = { exename, "--direct", "7", NULL };
      int pid = spawn2(P_NOWAIT | flags, exename, args, NULL, NULL, stdfds);
      if (pid == -1)
        perrno_and(return 1, "test 7: spawn2");

      rc = close(p[1]);
      if (rc == -1)
        perrno_and(return 1, "test 7: close");

      char buf[1024];
      int len = 0;
      for (;;)
      {
        rc = read(p[0], buf + len, sizeof(buf) - len - 1);
        if (rc <= 0)
          break;
        len += rc;
      }
      if (rc == -1)
        perrno_and(return 1, "test 7: read");

      buf[len] = 0;

      if (wait_pid("test 7", pid, 0, 0))
        return 1;

      if (strcmp(buf, "CHILD TEST 7") != 0)
        perr_and(return 1, "test 7: expected [CHILD TEST 7] got [%s]", buf);

      close(p[0]);

      restore_logging(logging);
    }

    // stderr pipe check, should succeed
    printf ("test 8 (iter %d)\n", iter);
    {
      int p[2];
      rc = pipe(p);
      if (rc == -1)
        perrno_and(return 1, "test 8: pipe");

      int stdfds[3] = { 0, 0, p[1] };

      const char *args[] = { exename, "--direct", "8", NULL };
      int pid = spawn2(P_NOWAIT | flags, exename, args, NULL, NULL, stdfds);
      if (pid == -1)
        perrno_and(return 1, "test 8: spawn2");

      rc = close(p[1]);
      if (rc == -1)
        perrno_and(return 1, "test 8: close");

      char buf[1024] = {0};
      rc = read(p[0], buf, sizeof(buf) - 1);
      if (rc == -1)
        perrno_and(return 1, "test 8: read");

      buf[rc] = 0;

      if (wait_pid("test 8", pid, 0, 0))
        return 1;

      if (strcmp(buf, "CHILD TEST 8") != 0)
        perr_and(return 1, "test 8: expected [CHILD TEST 8] got [%s]", buf);

      close(p[0]);
    }

    // stderr->stdout pipe check, should succeed
    printf ("test 9 (iter %d)\n", iter);
    {
      // forbid our own logging as we catch stdout
      char *logging = suppress_logging();

      int p[2];
      rc = pipe(p);
      if (rc == -1)
        perrno_and(return 1, "test 9: pipe");

      int stdfds[3] = { 0, p[1], 1 };

      const char *args[] = { exename, "--direct", "9", NULL };
      int pid = spawn2(P_NOWAIT | flags, exename, args, NULL, NULL, stdfds);
      if (pid == -1)
        perrno_and(return 1, "test 9: spawn2");

      rc = close(p[1]);
      if (rc == -1)
        perrno_and(return 1, "test 9: close");

      char buf[1024] = {0};
      rc = read(p[0], buf, sizeof(buf) - 1);
      if (rc == -1)
        perrno_and(return 1, "test 9: read");

      buf[rc] = 0;

      if (wait_pid("test 9", pid, 0, 0))
        return 1;

      if (strcmp(buf, "CHILD TEST 9") != 0)
        perr_and(return 1, "test 9: expected [CHILD TEST 9] got [%s]", buf);

      close(p[0]);

      restore_logging(logging);
    }

    // stderr->stdout pipe check in P_2_XREDIR mode, should succeed
    printf ("test 9.2 (iter %d)\n", iter);
    {
      // forbid our own logging as we catch stdout
      char *logging = suppress_logging();

      int p[2];
      rc = pipe(p);
      if (rc == -1)
        perrno_and(return 1, "test 9: pipe");

      int stdfds[] = { p[1], 1, p[1], 2, -1 };

      const char *args[] = { exename, "--direct", "9", NULL };
      int pid = spawn2(P_NOWAIT | P_2_XREDIR | flags, exename, args, NULL, NULL, stdfds);
      if (pid == -1)
        perrno_and(return 1, "test 9.2: spawn2");

      rc = close(p[1]);
      if (rc == -1)
        perrno_and(return 1, "test 9.2: close");

      char buf[1024] = {0};
      rc = read(p[0], buf, sizeof(buf) - 1);
      if (rc == -1)
        perrno_and(return 1, "test 9.2: read");

      buf[rc] = 0;

      if (wait_pid("test 9.2", pid, 0, 0))
        return 1;

      if (strcmp(buf, "CHILD TEST 9") != 0)
        perr_and(return 1, "test 9.2: expected [CHILD TEST 9] got [%s]", buf);

      close(p[0]);

      restore_logging(logging);
    }

    // stdin+stderr pipe check, should succeed
    printf ("test 10 (iter %d)\n", iter);
    {
      int pi[2];
      rc = pipe(pi);
      if (rc == -1)
        perrno_and(return 1, "test 10: pipe in");

      int po[2];
      rc = pipe(po);
      if (rc == -1)
        perrno_and(return 1, "test 10: pipe out");

      int stdfds[3] = { pi[0], 0, po[1] };

      const char *args[] = { exename, "--direct", "10", NULL };
      int pid = spawn2(P_NOWAIT | flags, exename, args, NULL, NULL, stdfds);
      if (pid == -1)
        perrno_and(return 1, "test 10: spawn2");

      rc = close(po[1]);
      if (rc == -1)
        perrno_and(return 1, "test 10: close out");

      rc = close(pi[0]);
      if (rc == -1)
        perrno_and(return 1, "test 10: close in");

      const char *test_str = "CHILD 10 TEST PING";
      rc = write(pi[1], test_str, strlen(test_str));
      if (rc == -1)
        perrno_and(return 1, "test 10: write");

      char buf[1024] = {0};
      rc = read(po[0], buf, sizeof(buf) - 1);
      if (rc == -1)
        perrno_and(return 1, "test 10: read");

      buf[rc] = 0;

      if (wait_pid("test 10", pid, 0, 0))
        return 1;

      if (strcmp(buf, test_str) != 0)
        perr_and(return 1, "test 10: expected [%s] got [%s]", test_str, buf);

      close(po[0]);
      close(pi[1]);
    }

    // stdin+fixed fd socketpair check in P_2_XREDIR mode, should succeed
    printf ("test 10.2 (iter %d)\n", iter);
    {
      int pi[2];
      rc = socketpair(AF_UNIX, SOCK_STREAM, 0, pi);
      if (rc == -1)
        perrno_and(return 1, "test 10.2: socketpair in");

      int po[2];
      rc = socketpair(AF_UNIX, SOCK_STREAM, 0, po);
      if (rc == -1)
        perrno_and(return 1, "test 10.2: socketpair out");

      int stdfds[] = { pi[0], 0, po[1], 1234, -1 };

      const char *args[] = { exename, "--direct", "10", "2", NULL };
      int pid = spawn2(P_NOWAIT | P_2_XREDIR | flags, exename, args, NULL, NULL, stdfds);
      if (pid == -1)
        perrno_and(return 1, "test 10.2: spawn2");

      rc = close(po[1]);
      if (rc == -1)
        perrno_and(return 1, "test 10.2: close out");

      rc = close(pi[0]);
      if (rc == -1)
        perrno_and(return 1, "test 10.2: close in");

      const char *test_str = "CHILD 10 TEST PING";
      rc = write(pi[1], test_str, strlen(test_str));
      if (rc == -1)
        perrno_and(return 1, "test 10.2: write");

      char buf[1024] = {0};
      rc = read(po[0], buf, sizeof(buf) - 1);
      if (rc == -1)
        perrno_and(return 1, "test 10.2: read");

      buf[rc] = 0;

      if (wait_pid("test 10.2", pid, 0, 0))
        return 1;

      if (strcmp(buf, test_str) != 0)
        perr_and(return 1, "test 10.2: expected [%s] got [%s]", test_str, buf);

      close(po[0]);
      close(pi[1]);
    }

    // P_2_NOINHERIT check, should succeed
    printf ("test 11 (iter %d)\n", iter);
    {
      enum { fds_num = 50, fds_num_child = 10 };
      int fds[fds_num] = {0};
      char fds_str[fds_num_child * 5] = {0};
      char *s = fds_str;
      int i;
      for (i = 0; i < fds_num; ++i)
      {
        fds[i] = open(exename, O_RDONLY);
        if (fds[i] == -1)
          perrno_and(return 1, "open %d", i);

        // pass only last 10 fds to the child (to protect from child's own fds
        // due to logging etc)
        if (i >= fds_num - fds_num_child)
        {
          _itoa (fds[i], s, 10);
          strcat(s, " ");
          s += strlen(s);
        }
      }

      const char *args[] = { exename, "--direct", "11", fds_str, NULL };
      int pid = spawn2(P_NOWAIT | P_2_NOINHERIT | flags, exename, args, NULL, NULL, NULL);
      if (pid == -1)
        perrno_and(return 1, "test 11: spawn2");

      if (wait_pid("test 11", pid, 0, 0))
        return 1;

      for (i = 0; i < fds_num; ++i)
        close(fds[i]);
    }
  }

  // multithreaded access to spawn2, should succeed
  printf ("test 12\n");
  {
    enum { tid_cnt = 25 };

    int tids[tid_cnt];
    int i;

    for (i = 0; i < tid_cnt; ++i)
    {
      tids[i] = _beginthread(test12_thread, NULL, 0, (void *)i);
      if (tids[i] == -1)
        perrno_and(return 1, "test 12.%d: _beginthread", i);
    }

    for (i = 0; i < tid_cnt; ++i)
    {
      TID tid = tids[i];
      DosWaitThread(&tid, DCWW_WAIT);
    }

    unsigned int test12_done_load = __atomic_load_n(&test12_done, __ATOMIC_RELAXED);
    if (test12_done_load != tid_cnt)
      perr_and(return 1, "test 12: done threads %d, not %d", test12_done_load, tid_cnt);
  }

  return 0;
}

static void test12_thread(void *arg)
{
  int i = (int)arg;
  char i_str[5] = {0};
  _itoa(i, i_str, 10);

  int rc;

  int pi[2] = {-1};
  int po[2] = {-1};

  do
  {
    rc = pipe(pi);
    if (rc == -1)
      perrno_and(return, "test 12.%d: pipe in", i);

    rc = pipe(po);
    if (rc == -1)
      perrno_and(break, "test 12.%d: pipe out", i);

    rc = fcntl(pi[1], F_GETFD);
    if (rc == -1)
      perrno_and(break, "test 12.%d: fcntl get in", i);
    rc = fcntl(pi[1], F_SETFD, rc | FD_CLOEXEC);
    if (rc == -1)
      perrno_and(break, "test 12.%d: fcntl set in", i);

    rc = fcntl(po[0], F_GETFD);
    if (rc == -1)
      perrno_and(break, "test 12.%d: fcntl get out", i);
    rc = fcntl(po[0], F_SETFD, rc | FD_CLOEXEC);
    if (rc == -1)
      perrno_and(break, "test 12.%d: fcntl set out", i);

    int stdfds[3] = { pi[0], 0, po[1] };

    const char *args[] = { exename, "--direct", "12", NULL };
    int pid = spawn2(P_NOWAIT | P_2_THREADSAFE /*| P_2_NOINHERIT*/, exename, args, NULL, NULL, stdfds);
    if (pid == -1)
      perrno_and(break, "test 12.%d: spawn2", i);

    rc = close(po[1]);
    if (rc == -1)
      perrno_and(break, "test 12.%d: close out", i);
    po[1] = -1;

    rc = close(pi[0]);
    if (rc == -1)
      perrno_and(break, "test 12.%d: close in", i);
    pi[0] = -1;

    char test_buf[1024] = "CHILD 12 TEST PING ";
    strcat(test_buf, i_str);

    rc = write(pi[1], test_buf, strlen(test_buf));
    if (rc == -1)
      perrno_and(break, "test 12.%d: write", i);

    char buf[1024] = {0};
    rc = read(po[0], buf, sizeof(buf) - 1);
    if (rc == -1)
      perrno_and(break, "test 12.%d: read", i);

    buf[rc] = 0;

    // process numbers mismatch due to P_2_THREADSAFE
#if 0
    strcat(test_buf, " ");
    _itoa(pid + 1, test_buf + strlen(test_buf), 16);
#endif

    if (strcmp(buf, test_buf) != 0)
      perr_and(break, "test 12.%d: expected [%s] got [%s]", i, test_buf, buf);

    if (wait_pid("test 12", pid, 0, 0))
      break;

    __atomic_add_fetch(&test12_done, 1, __ATOMIC_RELAXED);
  }
  while (0);
}
