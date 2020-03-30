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

// for _lmalloc()
#include <emx/umalloc.h>

#define TRACE_GROUP TRACE_GROUP_SPAWN
#define TRACE_MORE 0

#include "../shared.h"

#include "libcx/spawn2.h"

#include "spawn2-internal.h"

typedef struct Pair
{
  pid_t wrapper_pid;
  pid_t child_pid;
} Pair;

typedef struct SpawnWrappers
{
  int size;
  Pair pairs[0];
} SpawnWrappers;

enum { InitialPairArraySize = 10 };

static
int __spawn2(int mode, const char *name, const char * const argv[],
             const char *cwd, const char * const envp[], const int stdfds[],
             Spawn2Request *req)
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
    for (i = 0; ((mode & P_2_XREDIR) && stdfds[i] != -1) ||
                (!(mode & P_2_XREDIR) && i < 3); ++i)
      TRACE_CONT("%d,", stdfds[i]);
    TRACE_CONT("]\n");
  TRACE_END();

  if (!name || !*name || !argv || !*argv || (!stdfds && (mode & P_2_XREDIR)))
    SET_ERRNO_AND(return -1, EINVAL);

  int num_redirs = 0;
  if (stdfds)
  {
    if (!(mode & P_2_XREDIR))
    {
      /* Use extended mode to implement simple redirection */

      ASSERT(!req); /* The wrapper never does that */

      if ((stdfds[0] == 1 || stdfds[0] == 2))
        SET_ERRNO_AND(return -1, EINVAL);
      if ((stdfds[0] == -1 || stdfds[1] == -1 || stdfds[2] == -1))
        SET_ERRNO_AND(return -1, EBADF);

      /*
       * We accept stdfds[1] = 1 and stdfds[2] = 2 which is effectively the
       * same as 0 (no redirection, but inherit the handle from parent). This
       * is for compatibility with apps that blindly supply standard handles
       * for redirection (which makes no practical sense but can be explained
       * from a programmatic POV).
       *
       * Note that if all 3 fds are 0, this is effectively the same as stdfds
       * being NULL, i.e. no special redirection processing is needed at all.
       * We skop this case leaving num_redirs as 0 (no redirection).
       */

      if (stdfds[0] || (stdfds[1] && stdfds[1] != 1) || (stdfds[2] && stdfds[2] != 2))
      {
        int idx = 0;
        int fds[7];
        for (int i = 0; i < 3; ++i)
        {
          /*
           * Apply special handling (0 means inheritance and 1 and 2 are child
           * handles, not parent ones).
           */
          int sfd = stdfds[i];
          if (!sfd)
            sfd = i;
          else if (i == 1 && sfd == 2)
            sfd = stdfds[2] ? stdfds[2] : 2;
          else if (i == 2 && sfd == 1)
            sfd = stdfds[1] ? stdfds[1] : 1;

          fds[idx++] = sfd; /* source (parent) fd */
          fds[idx++] = i; /* target (child) fd */
        }
        fds[idx] = -1;
        return __spawn2(mode | P_2_XREDIR, name, argv, cwd, envp, fds, NULL);
      }
    }
    else
    {
      const int *pfd = stdfds;
      while (*pfd++ != -1)
        ++num_redirs;
      num_redirs /= 2;
    }
  }

  int type = mode & P_2_MODE_MASK;

  if (mode & P_2_THREADSAFE)
  {
    TRACE("using wrapper\n");

    if (type != P_WAIT && type != P_NOWAIT && type != P_SESSION && type != P_PM)
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

    int *inherited = NULL;
    if (num_redirs)
    {
      payload_size += sizeof(stdfds[0]) * (num_redirs * 2 + 1);
      inherited = malloc(sizeof(*inherited) * num_redirs);
      if (!inherited)
        SET_ERRNO_AND(return -1, ENOMEM);
      for (i = 0; i < num_redirs; ++i)
        inherited[i] = -1;
    }

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
    {
      free(inherited);
      SET_ERRNO_AND(return -1, ENOMEM);
    }

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

    if (num_redirs)
    {
      const int *pfd = stdfds;
      int *pl = (int *)payload;
      req->stdfds = pl;

      for (i = 0; i < num_redirs; ++i)
      {
        /* Make sure the source file handles are inherited by the wrapper */
        int f = rc = fcntl(*pfd, F_GETFD);
        if (f != -1 && (f & FD_CLOEXEC))
        {
          TRACE("enable wrapper inheritance for fd %d\n", *pfd);
          rc = fcntl(*pfd, F_SETFD, f & ~FD_CLOEXEC);
          if (rc != -1)
            inherited[i] = *pfd;
        }
        if (rc == -1)
          break;
        /* copy both source and target fds */
        *pl++ = *pfd++;
        *pl++ = *pfd++;
      }
      if (rc != -1)
      {
        *pl++ = -1;
        payload = (char *)pl;
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

      rc = __spawn2(type == P_SESSION || type == P_PM ? type : P_NOWAIT, w_exe, w_argv, NULL, NULL, NULL, req);
      TRACE("__spawn2(wrapper) rc %d (%x) errno %d\n", rc, rc, errno);
    }

    /* Restore stdio file handle inheritance */
    if (num_redirs)
    {
      for (i = 0; i < num_redirs; ++i)
      {
        if (inherited[i] != -1)
        {
          int f = fcntl(inherited[i], F_GETFD);
          if (f != -1)
            f = fcntl(inherited[i], F_SETFD, f | FD_CLOEXEC);
          ASSERT_MSG(f != -1, "%d %d", f, errno);
        }
      }
      free(inherited);
    }

    if (rc != -1)
    {
      APIRET arc = NO_ERROR;
      ULONG tries = 20;

      int child_pid = -1;
      int rc_errno = 0;

      TRACE("waiting for wrapper to kick in\n");
      do
      {
        DOS_NI(arc = DosWaitEventSem(spawn2_sem, 500));
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
          rc_errno = req->err;
          break;

        case 0:
          TRACE("wrapper timed out with arc %ld tries %d\n", arc, tries);
          rc = -1;
          rc_errno = arc == ERROR_INTERRUPT ? EINTR : ETIMEDOUT;
          break;

        default:
        {
          TRACE("wrapped child pid %d (%x)\n", req->rc, req->rc);

          child_pid = req->rc;

          /*
           * Save the wrapper->wrapped mapping in ths PID's ProcDesc. This is
           * used in our waitpid overrides. We don't do this for P_UNRELATED
           * children as they are... well, unrelated (can't be waited on etc).
           */
          if (type != P_SESSION || !(req->mode & P_UNRELATED))
          {
            ProcDesc *proc = find_proc_desc(getpid());
            ASSERT(proc);

            int idx = 0;

            if (!proc->spawn2_wrappers)
            {
              GLOBAL_NEW_PLUS_ARRAY(proc->spawn2_wrappers, proc->spawn2_wrappers->pairs, InitialPairArraySize);
              if (proc->spawn2_wrappers)
                proc->spawn2_wrappers->size = InitialPairArraySize;
            }
            else
            {
              for (idx = 0; idx < proc->spawn2_wrappers->size; ++idx)
              {
                if (proc->spawn2_wrappers->pairs[idx].wrapper_pid == 0)
                  break;
              }
              if (idx == proc->spawn2_wrappers->size)
              {
                SpawnWrappers *wrappers = RENEW_PLUS_ARRAY(proc->spawn2_wrappers, proc->spawn2_wrappers->pairs,
                                                           proc->spawn2_wrappers->size + InitialPairArraySize);
                if (wrappers)
                {
                  /* Realloc doesn't zero new memory, do it by hand */
                  for (int i = wrappers->size; i < wrappers->size + InitialPairArraySize; ++i)
                    memset(&wrappers->pairs[i], 0, sizeof(wrappers->pairs[i]));
                  wrappers->size = proc->spawn2_wrappers->size + InitialPairArraySize;
                  proc->spawn2_wrappers = wrappers;
                }
              }
            }

            if (!proc->spawn2_wrappers || idx >= proc->spawn2_wrappers->size)
            {
              TRACE("No memory for spawn2_wrappers\n");
              rc = -1;
              rc_errno = ENOMEM;
            }
            else
            {
              ASSERT_MSG(!proc->spawn2_wrappers->pairs[idx].wrapper_pid && !proc->spawn2_wrappers->pairs[idx].child_pid,
                         "%d %d", proc->spawn2_wrappers->pairs[idx].wrapper_pid, proc->spawn2_wrappers->pairs[idx].child_pid);
              proc->spawn2_wrappers->pairs[idx].wrapper_pid = rc;
              proc->spawn2_wrappers->pairs[idx].child_pid = child_pid;

              TRACE("Added wrapper->wrapped pair %d->%d\n", rc, child_pid);
            }
          }

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
        if (type == P_WAIT)
        {
          int status;
          rc = waitpid(rc, &status, 0);
          TRACE("waitpid(wrapper) rc %d status %x errno %d\n", rc, status, errno);

          rc = WEXITSTATUS(status);
        }
        else
        {
          /* Always return the PID of the wrapped child, not the wrapper! */
          ASSERT (child_pid != -1);
          rc = child_pid;
        }
      }
      else
      {
        errno = rc_errno;
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
  int *dups = NULL;
  int *inherited = NULL;
  fd_set *noclofds = NULL;
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
    /* Process redirected file handles */
    if (num_redirs)
    {
      dups = malloc(sizeof(*dups) * num_redirs);
      if (dups)
      {
        inherited = malloc(sizeof(*inherited) * num_redirs);
        if (inherited)
          noclofds = malloc(sizeof(*noclofds));
      }
      if (!dups || !inherited || !noclofds)
      {
        rc_errno = ENOMEM;
        rc = -1;
      }
      else
      {
        FD_ZERO(noclofds);

        const int *pfd = stdfds;
        for (int i = 0; i < num_redirs; ++i)
        {
          inherited[i] = -1;
          dups[i] = -1;

          /*
           * Duplicate (save) all target file handles upfront to account for
           * cross-redirections like (1->2, 2->1) unless they are simply
           * inherited.
           */
          if (rc != -1)
          {
            int sfd = *pfd++;
            int tfd = *pfd++;

            if (FD_ISSET(tfd, noclofds))
            {
              TRACE("target fd %d given twice\n", tfd);
              rc_errno = EINVAL;
              rc = -1;
              continue;
            }
            FD_SET(tfd, noclofds);

            /* No need to duplicate an inherited file handle */
            if (sfd == tfd)
              continue;

            dups[i] = rc = dup(tfd);
            if (rc == -1 && errno == EBADF)
            {
              /* No need to duplicate a non-existent file handle */
              rc = 0;
              continue;
            }

            TRACE("save target fd %d as %d\n", tfd, dups[i]);

            int f = rc = fcntl(dups[i], F_GETFD);
            if (rc != -1)
            {
              rc = fcntl(dups[i], F_SETFD, f | FD_CLOEXEC);
              if (rc != -1)
              {
                /*
                 * Remember CLOEXEC flag of the original handle to properly
                 * restore it later (dup creates fds with inheritance enabled).
                 */
                f = rc = fcntl(tfd, F_GETFD);
                if (rc != -1 && (f & FD_CLOEXEC))
                  inherited[i] = dups[i];
              }
            }
            if (rc == -1)
              rc_errno = errno;
          }
        }

        if (rc != -1)
        {
          const int *pfd = stdfds;
          for (int i = 0; i < num_redirs; ++i)
          {
            int sfd = *pfd++;
            int tfd = *pfd++;

            if (sfd == tfd)
            {
              /* Same file number, just enable inheritance */
              int f = rc = fcntl(sfd, F_GETFD);
              if (f != -1 && (f & FD_CLOEXEC))
              {
                TRACE("enable redir inheritance for fd %d\n", sfd);
                rc = fcntl(sfd, F_SETFD, f & ~FD_CLOEXEC);
                if (rc != -1)
                  inherited[i] = sfd;
              }
              if (rc == -1)
              {
                rc_errno = errno;
                break;
              }
            }
            else
            {
              TRACE("duplicate source fd %d to target fd %d\n", sfd, tfd);

              if (FD_ISSET(sfd, noclofds))
              {
                /*
                 * Source fd is among targets so it could be already replaced
                 * by another source fd. Find the saved original for it. Note
                 * that there is no need to replace if that target was simply
                 * inherited (i.e. matched the source) whcih is indicated by
                 * its dups entry being -1 (i.e. it was not saved).
                 */
                for (int j = 0; j < i; ++j)
                {
                  if (dups[j] != -1 && stdfds[j * 2 + 1] == sfd)
                  {
                    sfd = dups[j];
                    TRACE("replacing source fd with saved copy %d\n", sfd);
                  }
                }
              }

              rc = dup2(sfd, tfd);
              if (rc == -1)
              {
                rc_errno = errno;
                break;
              }
            }

            /*
             * No need to disable inheritance for dups later as it's already
             * done in the first loop (note that we can't put dups to noclofds
             * there as noclofds is used to find duplicates in target handles).
             */
            if (dups[i] != -1)
              FD_SET(dups[i], noclofds);
          }
        }
      }
    }

    if (rc != -1)
    {
      /* Disable inheritance if requested */
      if (mode & (P_2_NOINHERIT | P_2_XREDIR))
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

          /* Leave std handles intact if there is no redirection */
          int fd = num_redirs ? 0 : 3;
          for (; fd < FD_SETSIZE; ++fd)
          {
            /* Ignore redirected/inherited handles */
            if (noclofds && FD_ISSET(fd, noclofds))
              continue;

            int f = fcntl(fd, F_GETFD);
            if (f != -1)
            {
              if (!(f & FD_CLOEXEC))
              {
                TRACE_IF(TRACE_MORE, "disable inheritance for fd %d\n", fd);
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
          /*
           * Ignore the given environment completely if it only contains
           * pseudo-env vars as they could confuse programs if passed on.
           */
          envc = 0;
          envp_copy = NULL;
        }
        else if (pseudo_cnt || (mode & P_2_APPENDENV))
        {
          int environc = 0;
          if (mode & P_2_APPENDENV)
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
            for (envc = 0, i = 0; envp[i]; ++i)
            {
              if (pseudo_cnt)
              {
                /*
                 * Omit special pseudo-env vars from the environment as they
                 * could confuse programs if passed on.
                 */
                char *val = strchr(envp[i], '=');
                if (val)
                {
                  int var_len = val - envp[i];
                  ++val; /* go to the value (skip '=') */

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

            /*
             * Append the existing environment on P_2_APPENDENV (and if there
             * is any). Note that we have to skip the duplicates as although
             * DosExecPgm does that on its own, we don't know what spawn() does
             * to them (it might pass it to the kLIBC child process bypassing
             * DosExecPgm which'll result in old vars overriding the new ones,
             * see https://github.com/bitwiseworks/mozilla-os2/issues/262).
             */
            if (environc)
            {
              /*
               * Assume that the old env doesn't contain duplicates, so only
               * search among the new vars. If the new env contains duplicates,
               * the behavior is "undefined", so don't check for that too.
               */
              int ec = envc;
              for (i = 0; i < environc; ++i)
              {
                char *end = strchr(environ[i], '=');
                int len = end ? end - environ[i] : strlen(environ[i]);

                for (j = 0; j < ec; ++j)
                  if (strnicmp(environ[i], envp_copy[j], len) == 0 &&
                      (envp_copy[j][len] == '=' || envp_copy[j][len] == '\0'))
                    break;

                /* Add the old var if there was no match among the new ones */
                if (j == ec)
                  envp_copy[envc++] = environ[i];
              }
            }

            envp_copy[envc] = NULL;
          }
        }

        TRACE("envc %d, envp_copy %p\n", envc, envp_copy);

        TRACE_BEGIN_IF(envp_copy && (char **)envp != envp_copy, "envp_copy [");
          char **e;
          for (e = envp_copy; *e; ++e)
            TRACE_CONT("[%s]", *e);
          TRACE_CONT("]\n");
        TRACE_END();
      }

      if (rc != -1)
      {
        if (type == P_SESSION || type == P_PM)
        {
          /*
           * kLIBC spawn* doesn't support P_SESSION or P_PM (yet). DO it on our
           * own. Note that we use low memory for DosStartSession array
           * arguments as it is not high memory aware.
           */

          char *comspec = NULL;

          {
            const char *dot = strrchr(name, '.');
            if (dot)
            {
              if (stricmp(dot, ".cmd") == 0 || stricmp(dot, ".bat") == 0)
              {
                comspec = getenv("COMSPEC");
                if (!comspec)
                  comspec = "cmd.exe";
              }
            }
          }

          /*
           * Resolve symlinks & other Unix-isms as DosStartSession can't stand
           * them. Note that while DosStartSession does search in PATH if no path
           * information is given, spawn2 should not do this and _fullpath will
           * guarantee this as well (by prepending the current path in this case).
           */
          char name_buf[PATH_MAX];
          int name_len = 0;
          rc = _fullpath(name_buf, name, sizeof(name_buf));
          if (rc == -1)
            rc_errno = errno;
          else
            name_len = strlen(name_buf);

          char *name_real = name_buf;

          /* Flatten arguments */
          char *arg_flat = NULL;

          if (rc != -1)
          {
            int arg_size = 0;
            const char * const *a;

            /* Skip the program name in arguments, DosStartSession doesn't need it */
            for (a = argv + 1; *a; ++a)
              arg_size += strlen(*a) + 3 /* quotes + space/zero */;
            if (comspec)
              arg_size += name_len + 8 /* [/c ] + quotes * 2 + space/zero */;

            if (arg_size)
            {
              arg_flat = _lmalloc(arg_size);
              if (!arg_flat)
              {
                rc = -1;
                rc_errno = ENOMEM;
              }
              else
              {
                char *ap = arg_flat;
                int quotes;

                if (comspec)
                {
                  /* The /C argument should be a single string, put it in outer quotes */
                  memcpy(ap, "/c \"", 4);
                  ap += 4;
                  quotes = strchr(name_real, ' ') != NULL;
                  if (quotes)
                    *(ap++) = '"';
                  memcpy(ap, name_real, name_len);
                  ap += name_len;
                  if (quotes)
                    *(ap++) = '"';
                  *(ap++) = ' ';
                  name_real = comspec;
                }

                for (a = argv + 1; *a; ++a)
                {
                  const char *v = *a;
                  quotes = strchr(*a, ' ') != NULL;
                  if (quotes)
                    *(ap++) = '"';
                  while (*v)
                    *(ap++) = *(v++);
                  if (quotes)
                    *(ap++) = '"';
                  *(ap++) = ' ';
                }

                /* Replace the last space with zero (and a closing quote for /C) */
                if (comspec)
                  memcpy(--ap, "\"\0", 2);
                else
                  *(--ap) = '\0';
              }
            }
          }

          /* Flatten environment */
          char *env_flat = NULL;
          if (rc != -1 && envp_copy)
          {
            int env_size = 0;
            char **e;

            for (e = envp_copy; *e; ++e)
              env_size += strlen(*e) + 1;
            env_size += 1;

            env_flat = _lmalloc(env_size);
            if (!env_flat)
            {
              rc = -1;
              rc_errno = ENOMEM;
            }
            else
            {
              char *ep = env_flat;
              for (e = envp_copy; *e; ++e)
              {
                const char *v = *e;
                while (*v)
                  *(ep++) = *(v++);
                *(ep++) = '\0';
              }
              *ep = '\0';
            }
          }

          if (rc != -1)
          {
            TRACE("name_real [%s] arg_flat [%s] env_flat %p\n", name_real, arg_flat, env_flat);

            /*
             * Note: If it's a wrapper (req != NULL), use the wrapped child's
             * EXE name as the session title as otherwise OS/2 will show the
             * wrapper's EXE name which is totally confusing.
             */

            int reqMode = req ? req->mode : mode;
            int reqType = reqMode & P_2_MODE_MASK;

            STARTDATA data;
            data.Length = sizeof(data);
            data.Related = reqMode & P_UNRELATED ? SSF_RELATED_INDEPENDENT : SSF_RELATED_CHILD;
            data.FgBg = reqMode & P_BACKGROUND ? SSF_FGBG_BACK : SSF_FGBG_FORE;
            data.TraceOpt = SSF_TRACEOPT_NONE;
            data.PgmTitle = req ? _getname(req->name) : NULL;
            data.PgmName = name_real;
            data.PgmInputs = arg_flat;
            data.TermQ = NULL;
            data.Environment = env_flat;
            data.InheritOpt = SSF_INHERTOPT_PARENT;
            data.IconFile = NULL;
            data.PgmHandle = NULLHANDLE;
            data.InitXPos = data.InitYPos = data.InitXSize = data.InitYSize = 0;
            data.Reserved = 0;
            data.ObjectBuffer = NULL;
            data.ObjectBuffLen = 0;

            // NOTE: We maintain EMX spawn compatibility here.

            switch (reqMode & P_2_TYPE_MASK)
            {
              case P_FULLSCREEN:
                data.SessionType = SSF_TYPE_FULLSCREEN;
                break;
              case P_WINDOWED:
                data.SessionType = SSF_TYPE_WINDOWABLEVIO;
                break;
              default:
                if (reqType == P_PM)
                  data.SessionType = SSF_TYPE_PM;
                else
                  data.SessionType = SSF_TYPE_DEFAULT;
                break;
            }

            switch (reqMode & P_2_TYPE_MASK)
            {
              case P_MINIMIZE:
                data.PgmControl = SSF_CONTROL_MINIMIZE;
                break;
              case P_MAXIMIZE:
                data.PgmControl = SSF_CONTROL_MAXIMIZE;
                break;
              default:
                data.PgmControl = SSF_CONTROL_VISIBLE;
                break;
            }

            if (reqMode & P_NOCLOSE)
              data.PgmControl |= SSF_CONTROL_NOAUTOCLOSE;

            if (reqType != P_DEBUG)
              data.TraceOpt = SSF_TRACEOPT_NONE;
            else if (reqMode & P_DEBUGDESC)
              data.TraceOpt = SSF_TRACEOPT_TRACEALL;
            else
              data.TraceOpt = SSF_TRACEOPT_TRACE;

            ULONG ulSid = 0, ulPid = 0;
            APIRET arc = DosStartSession(&data, &ulSid, &ulPid);

            TRACE("DosStartSession returned %ld (0 and 457 are ok)\n", arc);

            if (arc != NO_ERROR && arc != ERROR_SMG_START_IN_BACKGROUND)
            {
              rc = -1;

              /*
               * Unfortunately, __libc_native2errno is not exported by kLIBC yet,
               * (see https://github.com/bitwiseworks/libc/issues/16) so do some
               * handling on our own...
               */
              switch (arc)
              {
                case ERROR_FILE_NOT_FOUND:
                case ERROR_PATH_NOT_FOUND:
                  rc_errno = ENOENT; break;
                default:
                  rc_errno = EINVAL;
              }
            }
            else
            {
              TRACE("DosStartSession pid %ld (independent? %d)\n", ulPid, data.Related == SSF_RELATED_INDEPENDENT);
              /*
               * For unrelated sessions ulPid might be not zero althnough it doesn't
               * represent a valid PID, make sure it is reset.
               */
              if (data.Related == SSF_RELATED_INDEPENDENT)
                ulPid = 0;
              rc = ulPid;
            }
          }

          if (env_flat)
            free(env_flat);
          if (arg_flat)
            free(arg_flat);
        }
        else
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
  }

  if (envp)
  {
    /* Restore special pseudo-env vars */
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

  /* Restore inheritance */
  if (clofds)
  {
    /* Leave std handles intact if there is no redirection */
    int fd = num_redirs ? 0 : 3;
    for (; fd < FD_SETSIZE; ++fd)
    {
      if (FD_ISSET(fd, clofds))
      {
        TRACE_IF(TRACE_MORE, "re-enable inheritance for fd %d\n", fd);
        int f = fcntl(fd, F_GETFD);
        if (f != -1)
          fcntl(fd, F_SETFD, f & ~FD_CLOEXEC);
      }
    }

    free(clofds);
  }

  /* Restore redirected file handles */
  if (num_redirs)
  {
    if (dups && inherited && noclofds)
    {
      const int *pfd = stdfds;
      for (int i = 0; i < num_redirs; ++i)
      {
        if (inherited[i] != -1)
        {
          /* Disable temporarily enable inheritance */
          int fd = inherited[i];
          int f = fcntl(fd, F_GETFD);
          if (f != -1)
            f = fcntl(fd, F_SETFD, f | FD_CLOEXEC);
          ASSERT_MSG(f != -1, "%d %d %d", i, fd, errno);
        }

        int sfd = *pfd++;
        int tfd = *pfd++;
        int dfd = dups[i];

        if (dfd != -1)
        {
          TRACE("restore target fd %d from %d\n", tfd, dfd);
          if (dup2(dfd, tfd) != -1)
            close(dfd);
          else
            ASSERT_MSG(0, "%d %d %d %d", i, dfd, tfd, errno);
        }
        else if (sfd != tfd)
        {
          /* It's a redirection but the target fd didn't exist, clean it up */
          TRACE("close temporary target fd %d\n", tfd);
          close(tfd);
        }
      }
    }

    free(noclofds);
    free(inherited);
    free(dups);
  }

  /* Restore the current directory */
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

int spawn2(int mode, const char *name, const char * const argv[],
           const char *cwd, const char * const envp[], const int stdfds[])
{
  return __spawn2(mode, name, argv, cwd, envp, stdfds, NULL);
}

/*
 * Override the waitpid family to replace the wrapped child with the wrapper
 * child. This replacement is necessary as the waitpid family can only operate
 * on direct children.
 */

pid_t _std_wait4(pid_t pid, int *piStatus, int fOptions, struct rusage *pRUsage);
int _std_waitid(idtype_t enmIdType, id_t Id, siginfo_t *pSigInfo, int fOptions);
int _libc___waitpid(int pid, int *status, int options);
ULONG APIENTRY _doscalls_DosWaitChild (ULONG ulAction, ULONG ulWait, PRESULTCODES pReturnCodes, PPID ppidOut, PID pidIn);

// NOTE: Must be called under global_lock
static void lookup_wrapper_pid(pid_t pid, pid_t *wrapper_pid, pid_t *child_pid)
{
  ProcDesc *proc = find_proc_desc(getpid());
  if (proc && proc->spawn2_wrappers)
  {
    int i;
    for (i = 0; i < proc->spawn2_wrappers->size; ++i)
    {
      if (proc->spawn2_wrappers->pairs[i].wrapper_pid == pid ||
          proc->spawn2_wrappers->pairs[i].child_pid == pid)
      {
        *wrapper_pid = proc->spawn2_wrappers->pairs[i].wrapper_pid;
        *child_pid = proc->spawn2_wrappers->pairs[i].child_pid;
        break;
      }
    }
  }
}

// NOTE: Must be called under global_lock
static void cleanup_wrapper_pid(pid_t wrapper_pid)
{
  ProcDesc *proc = find_proc_desc(getpid());
  if (proc && proc->spawn2_wrappers)
  {
    int i;
    for (i = 0; i < proc->spawn2_wrappers->size; ++i)
    {
      if (proc->spawn2_wrappers->pairs[i].wrapper_pid == wrapper_pid)
      {
        TRACE("Removing wrapper->wrapped pair %d->%d\n", proc->spawn2_wrappers->pairs[i].wrapper_pid, proc->spawn2_wrappers->pairs[i].child_pid);

        proc->spawn2_wrappers->pairs[i].wrapper_pid = 0;
        proc->spawn2_wrappers->pairs[i].child_pid = 0;
        break;
      }
    }
  }
}

pid_t wait4(pid_t pid, int *piStatus, int fOptions, struct rusage *pRUsage)
{
  TRACE("pid %d piStatus %p fOptions 0x%x pRUsage %p\n", pid, piStatus, fOptions, pRUsage);

  pid_t wrapper_pid = -1, child_pid = -1;

  if (pid > 0)
  {
    /* It could be the P_2_THRADSAFE wrapped process, look for a wrapper */
    global_lock();
    lookup_wrapper_pid(pid, &wrapper_pid, &child_pid);
    global_unlock();
    TRACE("wrapper_pid %d, child_pid %d\n", wrapper_pid, child_pid);

    if (pid == child_pid)
      pid = wrapper_pid;
  }

  pid_t rc = _std_wait4(pid, piStatus, fOptions, pRUsage);

  /* Make sure the caller never sees the wrapper PID on return */
  if (rc > 0)
  {
    global_lock();

    if (wrapper_pid == -1)
      lookup_wrapper_pid (rc, &wrapper_pid, &child_pid);

    if (rc == wrapper_pid)
    {
      cleanup_wrapper_pid (wrapper_pid);
      rc = child_pid;
    }

    global_unlock ();
  }

  return rc;
}

pid_t wait(int *piStatus)
{
  return wait4(-1, piStatus, 0, NULL);
}

pid_t wait3(int *piStatus, int fOptions, struct rusage *pRUsage)
{
  return wait4(-1, piStatus, fOptions, pRUsage);
}

pid_t waitpid(pid_t pid, int *piStatus, int fOptions)
{
  return wait4(pid, piStatus, fOptions, NULL);
}

int waitid(idtype_t enmIdType, id_t Id, siginfo_t *pSigInfo, int fOptions)
{
  TRACE("enmIdType %d Id %lld pSigInfo %p fOptions 0x%x\n", enmIdType, Id, pSigInfo, fOptions);

  pid_t wrapper_pid = -1, child_pid = -1;

  if (enmIdType == P_PID && Id > 0)
  {
    /* It could be the P_2_THRADSAFE wrapped process, look for a wrapper */
    global_lock();
    lookup_wrapper_pid(Id, &wrapper_pid, &child_pid);
    global_unlock ();
    TRACE("wrapper_pid %d, child_pid %d\n", wrapper_pid, child_pid);

    if (Id == child_pid)
      Id = wrapper_pid;
  }

  siginfo_t SigInfo = {0};
  int rc = _std_waitid(enmIdType, Id, &SigInfo, fOptions);

  /* Make sure the caller never sees the wrapper PID on return */
  if (rc == 0)
  {
    global_lock();

    if (wrapper_pid == -1)
      lookup_wrapper_pid (rc, &wrapper_pid, &child_pid);

    if (SigInfo.si_pid == wrapper_pid)
    {
      cleanup_wrapper_pid (wrapper_pid);
      SigInfo.si_pid = child_pid;
    }

    global_unlock ();

    if (pSigInfo)
      *pSigInfo = SigInfo;
  }

  return rc;
}

int __waitpid(int pid, int *status, int options)
{
  TRACE("pid %d status %p options 0x%x\n", pid, status, options);

  pid_t wrapper_pid = 0, child_pid = 0;

  if (pid != 0)
  {
    /* It could be the P_2_THRADSAFE wrapped process, look for a wrapper */
    global_lock();
    lookup_wrapper_pid(pid, &wrapper_pid, &child_pid);
    global_unlock ();
    TRACE("wrapper_pid %d, child_pid %d\n", wrapper_pid, child_pid);

    if (pid == child_pid)
      pid = wrapper_pid;
  }

  int rc = _libc___waitpid(pid, status, options);

  /* Make sure the caller never sees the wrapper PID on return */
  if (rc != 0)
  {
    global_lock();

    if (wrapper_pid == 0)
      lookup_wrapper_pid (rc, &wrapper_pid, &child_pid);

    if (rc == wrapper_pid)
    {
      cleanup_wrapper_pid (wrapper_pid);
      rc = child_pid;
    }

    global_unlock ();
  }

  return rc;
}

ULONG APIENTRY DosWaitChild (ULONG ulAction, ULONG ulWait, PRESULTCODES pReturnCodes, PPID ppidOut, PID pidIn)
{
  TRACE("ulAction %lu ulWait %lu pReturnCodes %p ppidOut %p pidIn %ld\n", ulAction, ulWait, pReturnCodes, ppidOut, pidIn);

  pid_t wrapper_pid = 0, child_pid = 0;

  if (ulAction == DCWA_PROCESS && pidIn != 0)
  {
    /* It could be the P_2_THRADSAFE wrapped process, look for a wrapper */
    global_lock();
    lookup_wrapper_pid(pidIn, &wrapper_pid, &child_pid);
    global_unlock ();
    TRACE("wrapper_pid %d, child_pid %d\n", wrapper_pid, child_pid);

    if (pidIn == child_pid)
      pidIn = wrapper_pid;
  }

  PID pidOut = ppidOut ? *ppidOut : 0;
  APIRET arc = _doscalls_DosWaitChild(ulAction, ulWait, pReturnCodes, &pidOut, pidIn);

  /* Make sure the caller never sees the wrapper PID on return */
  if (arc == NO_ERROR && pidOut)
  {
    global_lock();

    if (wrapper_pid == -1)
      lookup_wrapper_pid (pidOut, &wrapper_pid, &child_pid);

    if (pidOut == wrapper_pid)
    {
      cleanup_wrapper_pid (wrapper_pid);
      pidOut = child_pid;
    }

    global_unlock ();
  }

  if (ppidOut)
    *ppidOut = pidOut;

  return arc;
}
