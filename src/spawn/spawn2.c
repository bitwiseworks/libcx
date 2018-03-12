/*
 * Implementation of spawn2 API.
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

// Redefine fd_set to fit OPEN_MAX file descriptors
#define FD_SETSIZE OPEN_MAX
#include <sys/syslimits.h>

#include <stdlib.h>
#include <io.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <errno.h>

// for STREAM_LOCK
#include <sys/fmutex.h>
#include <emx/io.h>

#define TRACE_GROUP TRACE_GROUP_SPAWN

#include "../shared.h"

#include "libcx/spawn2.h"

#include "spawn2-internal.h"

int spawn2(int mode, const char *name, const char * const argv[],
           const char *cwd, const char * const envp[], int stdfds[3])
{
  TRACE("mode %x name [%s] argv %p cwd [%s] envp %p stdfds %p\n",
        mode, name, argv, cwd, envp, stdfds);

  TRACE_BEGIN_IF(argv, "argv [");
    const char * const *a;
    for (a = argv; *a; ++a)
      TRACE_CONT("[%s]", *a);
    TRACE_CONT("]\n");
  TRACE_END();

  TRACE_BEGIN_IF(envp, "envp [");
    const char * const *e;
    for (e = envp; *e; ++e)
      TRACE_CONT("[%s]", *e);
    TRACE_CONT("]\n");
  TRACE_END();

  TRACE_BEGIN_IF(stdfds, "stdfds [");
    int i;
    for (i = 0; i < 3; ++i)
      TRACE_CONT("%d,", stdfds[i]);
    TRACE_CONT("]\n");
  TRACE_END();

  if (!name || !*name || !argv || !*argv)
    SET_ERRNO_AND(return -1, EINVAL);

  int have_stdfds = 0;
  int stdfds_copy[3] = {0};

  if (stdfds)
  {
    /*
     * We accept stdfds[1] = 1 and stdfds[2] = 2 and treat it as 0 (no
     * redirection). This is for compatibility with apps that blindly supply
     * standard handles for redirection (which makes no practical sense
     * but can be explained from a programmatic POV).
     */

    if ((stdfds[0] == 1 || stdfds[0] == 2))
      SET_ERRNO_AND(return -1, EINVAL);

    memcpy(stdfds_copy, stdfds, sizeof(stdfds_copy));

    if (stdfds_copy[1] == 1)
      stdfds_copy[1] = 0;
    if (stdfds_copy[2] == 2)
      stdfds_copy[2] = 0;

    have_stdfds = stdfds_copy[0] || stdfds_copy[1] || stdfds_copy[2];
  }

  if (mode & P_2_THREADSAFE)
  {
    TRACE("using wrapper\n");

    if ((mode & P_2_MODE_MASK) != P_WAIT && (mode & P_2_MODE_MASK) != P_NOWAIT)
      SET_ERRNO_AND(return -1, EINVAL);

    char w_exe[CCHMAXPATH + sizeof(SPAWN2_WRAPPERNAME) + 1];
    if (!get_module_name (w_exe, sizeof(w_exe)))
      SET_ERRNO_AND(return -1, ENOMEM);

    strcpy(_getname(w_exe), SPAWN2_WRAPPERNAME);

    TRACE("w_exe [%s]\n", w_exe);

    /* Calculate how much dynamic memory is needed for the request */

    ULONG payload_size = strlen(name) + 1;

    int rc = 0, i;
    int argc = 0;
    int envc = 0;

    for (; argv[argc]; ++argc)
      payload_size += strlen(argv[argc]) + 1;
    payload_size += sizeof(char *) * (argc + 1);

    if (cwd)
      payload_size += strlen(cwd) + 1;

    if (envp)
    {
      for (; envp[envc]; ++envc)
        payload_size += strlen(envp[envc]) + 1;
      payload_size += sizeof(char *) * (envc + 1);
    }

    if (have_stdfds)
      payload_size += sizeof(stdfds_copy);

    /* Allocate the request from LIBCx shared memory */
    size_t req_size = sizeof(Spawn2Request) + payload_size;
    TRACE("request size %d bytes\n", req_size);

    HEV spawn2_sem;
    char *mem;

    global_lock();

    spawn2_sem = global_spawn2_sem(NULL);
    if (spawn2_sem)
      mem = global_alloc(req_size);

    global_unlock();

    if (!spawn2_sem || !mem)
      SET_ERRNO_AND(return -1, ENOMEM);

    /* Fill up request data */

    Spawn2Request *req = (Spawn2Request *)mem;

    req->_payload_size = payload_size;
    char *payload = req->_payload;

    req->mode = mode;

    size_t len = strlen(name) + 1;
    memcpy(payload, name, len);
    req->name = payload;
    payload += len;

    req->argv = (const char * const *)payload;
    payload += sizeof(char *) * (argc + 1);

    for (i = 0; i < argc; ++i)
    {
      len = strlen(argv[i]) + 1;
      memcpy(payload, argv[i], len);
      ((char **)req->argv)[i] = payload;
      payload += len;
    }

    ((char **)req->argv)[i] = NULL;

    if (cwd)
    {
      len = strlen(cwd) + 1;
      memcpy(payload, cwd, len);
      req->cwd = payload;
      payload += len;
    }
    else
      req->cwd = NULL;

    if (envp)
    {
      req->envp = (const char * const *)payload;
      payload += sizeof(char *) * (envc + 1);

      for (i = 0; i < envc; ++i)
      {
        len = strlen(envp[i]) + 1;
        memcpy(payload, envp[i], len);
        ((char **)req->envp)[i] = payload;
        payload += len;
      }

      ((char **)req->envp)[i] = NULL;
    }
    else
      req->envp = NULL;

    int inherited [3] = {0};

    if (have_stdfds)
    {
      len = sizeof(stdfds_copy);
      memcpy(payload, stdfds_copy, len);
      req->stdfds = (int *)payload;
      payload += len;

      /* Make sure stdio file handles are inherited by the wrapper */
      for (i = 0; i < 3; ++i)
      {
        if (stdfds_copy[i])
        {
          int f = rc = fcntl(stdfds_copy[i], F_GETFD);
          if (f != -1 && (f & FD_CLOEXEC))
          {
            TRACE("enable inheritance for %d\n", stdfds_copy[i]);
            rc = fcntl(stdfds_copy[i], F_SETFD, f & ~FD_CLOEXEC);
            if (rc != -1)
              inherited[i] = 1;
          }
          if (rc == -1)
            break;
        }
      }
    }
    else
      req->stdfds = NULL;

    if (rc != -1)
    {
      ASSERT_MSG(payload - (char *)mem - sizeof(Spawn2Request) == payload_size,
                 "%d != %d", payload - (char *)mem - sizeof(Spawn2Request), payload_size);

      char sem_str[10];
      _ultoa(spawn2_sem, sem_str, 16);

      char mem_str[10];
      _ultoa((unsigned long)mem, mem_str, 16);

      const char *w_argv[] = { w_exe, sem_str, mem_str, NULL };

      rc = spawn2(P_NOWAIT, w_exe, w_argv, NULL, NULL, NULL);
      TRACE("spawn2(wrapper) rc %d (%x) errno %d\n", rc, rc, errno);
    }

    /* Restore stdio file handle inheritance */
    if (have_stdfds)
    {
      for (i = 0; i < 3; ++i)
      {
        if (inherited[i])
        {
          int f = fcntl(stdfds_copy[i], F_GETFD);
          if (f != -1)
            f = fcntl(stdfds_copy[i], F_SETFD, f | FD_CLOEXEC);
          ASSERT_MSG(f != -1, "%d %d", f, errno);
        }
      }
    }

    if (rc != -1)
    {
      APIRET arc = NO_ERROR;
      ULONG tries = 20;

      TRACE("waiting for wrapper to kick in\n");
      do
      {
        arc = DosWaitEventSem(spawn2_sem, 500);
        ASSERT_MSG(arc == NO_ERROR || arc == ERROR_TIMEOUT || arc == ERROR_INTERRUPT,
                   "%ld %lx", arc, spawn2_sem);

        if (req->rc)
          break;

        if (!--tries)
          break;
      }
      while (1);

      global_lock ();

      switch (req->rc)
      {
        case -1:
          TRACE("wrapper reported errno %d\n", req->err);
          rc = req->rc;
          errno = req->err;
          break;

        case 0:
          TRACE("wrapper timed out with arc %ld tries %d\n", arc, tries);
          rc = -1;
          errno = arc == ERROR_INTERRUPT ? EINTR : ETIMEDOUT;
          break;

        default:
        {
          TRACE("wrapped child pid %d (%x)\n", req->rc, req->rc);

          /*
           * Save the wrapped child PID (not really used so far but might be
           * needed later to implement waiting on it).
           */
          ProcDesc *proc = find_proc_desc(rc);
          if (proc)
            proc->child_pid = rc;

          break;
        }
      }

      /*
       * kLIBC spawn will eventually make a copy of the arguments so it should
       * be safe to free request memory now.
       */
      free(mem);

      global_unlock ();

      if (rc != -1)
      {
        if ((mode & P_2_MODE_MASK) == P_WAIT)
        {
          int status;
          rc = waitpid(rc, &status, 0);
          TRACE("waitpid(wrapper) rc %d status %x errno %d\n", rc, status, errno);

          rc = WEXITSTATUS(status);
        }
      }
    }
    else
    {
      global_lock ();
      free(mem);
      global_unlock ();
    }

    return rc;
  }

  int rc = 0;
  int rc_errno = 0;

  char *curdir = NULL;
  int dupfds[3] = {0};
  fd_set *clofds = NULL;

  char **envp_copy = (char **)envp;

  enum { PseudoEnvCnt = 3 };
  const char *pseudo_env[PseudoEnvCnt] = { "BEGINLIBPATH", "ENDLIBPATH", "LIBPATHSTRICT" };
  const ULONG pseudo_eid[PseudoEnvCnt] = { BEGIN_LIBPATH, END_LIBPATH, LIBPATHSTRICT };
  char *pseudo_eold[PseudoEnvCnt] = { NULL, NULL, NULL };

  // Change the current directory if needed
  if (cwd)
  {
    if ((curdir = getcwd(NULL, 0)) == NULL)
    {
      rc = -1;
      rc_errno = errno;
    }
    else
    {
      TRACE("curdir [%s]\n", curdir);

      if ((rc = chdir(cwd)) == -1)
      {
        rc_errno = errno;
        free(curdir);
        curdir = NULL;
      }
    }
  }

  if (rc != -1)
  {
    // Duplicate stdio (0,1,2) if needed
    if (have_stdfds)
    {
      int i;
      for (i = 0; i < 3; ++i)
      {
        /*
         * Note that we don't have to handle value of 1 in stdfds[2] specially:
         * if stdfds[1] was provided, 1 was already redirected to it, so
         * redirecting to 1 is equivalent to redirecting to stdfds[1]; if it
         * was not, then we'll redirect 2 to 1 which is simply stdout in this
         * case - still just what we need. We do need special handling for 2 in
         * stdfds[1] though (by using a value of stdfds[2] as the target of
         * redirection if it's provided).
         */

        if (stdfds_copy[i])
        {
          /* Secially handle stdfds[1] = 2 */
          int target = stdfds_copy[i];
          if (i == 1 && target == 2 && stdfds_copy[2])
          {
            target = stdfds_copy[2];
            /* Break cirular redirection (stdout remains itself) */
            if (target == 1)
              continue;
          }

          int fd = rc = dup(i);
          if (rc != -1)
          {
            int f = rc = fcntl(fd, F_GETFD);
            if (rc != -1)
            {
              rc = fcntl(fd, F_SETFD, f | FD_CLOEXEC);
              if (rc != -1)
                if ((rc = dup2(target, i)) != -1)
                  dupfds[i] = fd;
            }
          }
          if (rc == -1)
          {
            rc_errno = errno;
            if (fd != -1)
              close(fd);
            break;
          }
        }
      }
    }

    if (rc != -1)
    {
      // Disable inheritance if needed
      if (mode & P_2_NOINHERIT)
      {
        clofds = malloc(sizeof(*clofds));
        if (clofds == NULL)
        {
          rc = -1;
          rc_errno = ENOMEM;
        }
        else
        {
          FD_ZERO(clofds);

          int fd;
          for (fd = 3; fd < OPEN_MAX; ++fd)
          {
            int f = fcntl(fd, F_GETFD);
            if (f != -1)
            {
              if (!(f & FD_CLOEXEC))
              {
                if ((rc = fcntl(fd, F_SETFD, f | FD_CLOEXEC)) != -1)
                  FD_SET(fd, clofds);
              }
            }
            if (rc == -1)
            {
              rc_errno = errno;
              break;
            }
          }
        }
      }

      if (rc != -1 && envp)
      {
        // Process special pseudo-env vars

        int i, j, envc = 0, pseudo_cnt = 0;
        char buf[1024]; // Documented maximum for pseudo-env vars

        for (i = 0; envp[i]; ++i)
        {
          char *val = strchr(envp[i], '=');
          if (val)
          {
            int var_len = val - envp[i];
            ++val; // go to the value (skip '=')

            for (j = 0; j < PseudoEnvCnt; ++j)
            {
              if (strnicmp(envp[i], pseudo_env[j], var_len) == 0 &&
                  envp[i][var_len] == '=' && pseudo_env[j][var_len] == '\0')
              {
                TRACE("found special pseudo-env var [%s]\n", envp[i]);
                ++pseudo_cnt;

                // accept only the first occurence
                if (pseudo_eold[j])
                {
                  TRACE("duplicate var; ignored\n");
                  continue;
                }

                // LIBPATHSTRICT only returns T or nothing so set the zero terminator
                if (pseudo_eid[j] == LIBPATHSTRICT)
                  memset(buf, '\0', 4);

                APIRET arc = DosQueryExtLIBPATH(buf, pseudo_eid[j]);
                if (arc != NO_ERROR)
                {
                  rc = -1;
                  rc_errno = EOVERFLOW;
                  break;
                }

                pseudo_eold[j] = malloc(strlen(buf) + 1);
                if (pseudo_eold[j] == NULL)
                {
                  rc = -1;
                  rc_errno = ENOMEM;
                  break;
                }

                strcpy(pseudo_eold[j], buf);

                arc = DosSetExtLIBPATH(val, pseudo_eid[j]);
                if (arc != NO_ERROR)
                {
                  rc = -1;
                  rc_errno = EOVERFLOW;
                  break;
                }
              }
            }
          }
        }

        envc = i;

        TRACE("envc %d, pseudo_cnt %d\n", envc, pseudo_cnt);

        if (pseudo_cnt && pseudo_cnt == envc)
        {
          // Ignore the given environment completely if it only contains
          // pseudo-env vars as they could confuse programs if passed on.
          envc = 0;
          envp_copy = NULL;
        }
        else if (pseudo_cnt || (mode & P_2_APPENDENV))
        {
          int environc = 0;
          for (; environ[environc]; ++environc)
            ;

          envp_copy = malloc(sizeof(char *) * (envc + environc + 1));
          if (envp_copy == NULL)
          {
            rc = -1;
            rc_errno = ENOMEM;
          }
          else
          {
            // Don't bother ourselves handling duplicates here, it appears that
            // DosExecPgm does that anyway - in a manner where the first
            // occurrence of the variable takes precedence (all other ones are
            // ignored). So, in order for envp vars to override the current
            // environment we put them first.

            for (envc = 0, i = 0; envp[i]; ++i)
            {
              if (pseudo_cnt)
              {
                // Omit special pseudo-env vars from the environment as they
                // could confuse programs if passed on.

                char *val = strchr(envp[i], '=');
                if (val)
                {
                  int var_len = val - envp[i];
                  ++val; // go to the value (skip '=')

                  for (j = 0; j < PseudoEnvCnt; ++j)
                  {
                    if (strnicmp(envp[i], pseudo_env[j], var_len) == 0 &&
                        envp[i][var_len] == '=' && pseudo_env[j][var_len] == '\0')
                      break;
                  }

                  if (j < PseudoEnvCnt)
                    continue;
                }
              }

              envp_copy[envc++] = (char *)envp[i];
            }

            for (i = 0; i < environc; ++i)
              envp_copy[envc + i] = environ[i];

            envp_copy[environc + envc] = NULL;
          }
        }

        TRACE("envc %d, envp_copy %p\n", envc, envp_copy);
      }

      if (rc != -1)
      {
        if (envp_copy)
          rc = spawnvpe(mode, name, (char * const *)argv, envp_copy);
        else
          rc = spawnvp(mode, name, (char * const *)argv);

        if (rc == -1)
          rc_errno = errno;

        TRACE_ERRNO_IF(rc == -1, "spawn*");
      }
    }
  }

  if (envp)
  {
    // Restore special pseudo-env vars
    int j;
    for (j = 0; j < PseudoEnvCnt; ++j)
    {
      if (pseudo_eold[j])
      {
        TRACE("restoring special pseudo-env var [%s=%s]\n", pseudo_env[j], pseudo_eold[j]);

        DosSetExtLIBPATH(pseudo_eold[j], pseudo_eid[j]);
        free(pseudo_eold[j]);
      }
    }

    if (envp_copy && envp_copy != (char **)envp)
      free(envp_copy);
  }

  // Restore inheritance
  if (clofds)
  {
    int fd;
    for (fd = 3; fd < OPEN_MAX; ++fd)
    {
      if (FD_ISSET(fd, clofds))
      {
        int f = fcntl(fd, F_GETFD);
        if (f != -1)
          fcntl(fd, F_SETFD, f & ~FD_CLOEXEC);
      }
    }

    free(clofds);
  }

  // Restore stdio (0,1,2)
  if (have_stdfds)
  {
    int i;
    for (i = 0; i < 3; ++i)
    {
      if (dupfds[i])
      {
        dup2(dupfds[i], i);
        close(dupfds[i]);
      }
    }
  }

  // Restore the current directory
  if (curdir)
  {
    chdir(curdir);
    free(curdir);
  }

  if (rc_errno)
    errno = rc_errno;

  TRACE_ERRNO_IF(rc == -1, "return");
  TRACE("return %d (%x)\n", rc, rc);

  return rc;
}
