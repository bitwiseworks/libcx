/*
 * Implementation of spawn2 wrapper.
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

#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <errno.h>

#include <InnoTekLIBC/backend.h>
#include <InnoTekLIBC/sharedpm.h>

#define TRACE_GROUP TRACE_GROUP_SPAWN

#include "../shared.h"

#include "libcx/spawn2.h"

#include "spawn2-internal.h"

//void wait_thread(void *arg)
//{
//  HEV hev = (HEV)arg;

//  TRACE("hev %lx\n", hev);

////  APIRET arc = DosWaitChild(DCWA_PROCESS, )

//  while (1)
//  {
//    siginfo_t siginfo;
//    int rc = __libc_Back_processWait(P_ALL, 0, &siginfo, WEXITED | WUNTRACED | WSTOPPED | WCONTINUED | WNOHANG | WNOWAIT, NULL);

//    TRACE("***** rc %d pid %d flags %x status %x\n", rc, siginfo.si_pid, siginfo.si_flags, siginfo.si_status);

//    RESULTCODES resc;
//    PID pid;
//    APIRET arc = DosWaitChild(DCWA_PROCESS, DCWW_NOWAIT, &resc, &pid, 0);
//    TRACE("***** arc %ld pid %ld\n", arc, pid);

//    DosSleep(1);
//  }
//}

#define FIXUP_PTR(ptr, diff) *(char **)(&ptr) = ((char *)(ptr) + (diff))

int main(int argc, char **argv)
{
  TRACE("argc %d argv %p\n", argc, argv);

  int rc = 127;

  if (argc < 3)
    TRACE_AND(return rc, "argc %d < 3\n", argc);

  /*
   * Note that the semaphore and memory block (which is from LIBCx shared heap)
   * are made accessible to this process buy shared_init().
   */

  APIRET arc;
  HEV hev = strtoul(argv[1], NULL, 16);
  char *mem = (char *)strtoul(argv[2], NULL, 16);

  /* Copy the data over and report the parent is ok to free it */

  Spawn2Request *req = (Spawn2Request *)mem;
//  Spawn2Request *req = NULL;

//  {
//    Spawn2Request *req_shared = (Spawn2Request *)mem;

//    size_t req_size = sizeof(Spawn2Request) + req_shared->_payload_size;
//    TRACE("copying request %p (%d bytes)\n", req_shared, req_size);

//    req = malloc(req_size);
//    if (req)
//    {
//      memcpy(req, req_shared, req_size);

//      /* Adjust the base of dynamic buffers to the new payload address */
//      int diff = req->_payload - req_shared->_payload;
//      int i;

//      TRACE("copying request to %p (diff %p)\n", req, diff);

//      if (req->name)
//        FIXUP_PTR(req->name, diff);

//      if (req->argv)
//      {
//        FIXUP_PTR(req->argv, diff);
//        for (i = 0; req->argv[i]; ++i)
//          FIXUP_PTR(req->argv[i], diff);
//      }

//      if (req->cwd)
//        FIXUP_PTR(req->cwd, diff);

//      if (req->envp)
//      {
//        FIXUP_PTR(req->envp, diff);
//        for (i = 0; req->envp[i]; ++i)
//          FIXUP_PTR(req->envp[i], diff);
//      }

//      if (req->stdfds)
//        FIXUP_PTR(req->stdfds, diff);

//      /* Signal the semaphore to indicate we are done copying */
////      TRACE("post spawn2 semaphore\n");
////      arc = DosPostEventSem(hev);
////      ASSERT_MSG(arc == NO_ERROR || arc == ERROR_ALREADY_POSTED, "%ld %lx", arc, hev);
//    }
//  }
//  ASSERT(req);

//  if (req)
//  {
    /* Leave all flags except P_2_THREADSAFE and force mode to P_OVERLAY */
    int mode = req->mode;
    mode &= ~(P_2_MODE_MASK | P_2_THREADSAFE);
//    mode |= P_OVERLAY;
// NO INHERIT SO THAT CHILD DIDNT HANG
    mode |= P_NOWAIT | P_2_NOINHERIT;

  //  rc = _beginthread(wait_thread, NULL, 0, (void *)hev);
  //  TRACE("beginthread(wrapper) rc %d errno %d\n", rc, errno);

  //  if (rc != -1)
//    {
      rc = spawn2(mode, req->name, req->argv, req->cwd, req->envp, req->stdfds);
      TRACE("spawn2(wrapped child) rc %d (%x) errno %d\n", rc, rc, errno);
//    }
//  }
//  else
//  {
//    rc = -1;
//    errno = ENOMEM;
//  }

  if (rc != -1)
  {
    if (req->stdfds)
    {
      int i;
//      for (i = 0; i < 3; ++i)
//      {
//        if (req->stdfds[i])
//        {
//          TRACE("cloclo %d\n", req->stdfds[i]);
//          if (i == 0)
//            close(req->stdfds[i]+2);
//          else
//            close(req->stdfds[i]-2);
//  //        close(req->stdfds[i]+1);
//  //        close(req->stdfds[i]-1);
//  //        close(req->stdfds[i]);
//        }
//      }
//      for (i = 3; i < 500; ++i)
//        close(i);
    }
  }

  /* Set the spawn result... */
  if (rc == -1)
  {
    req->rc = -1;
    req->err = errno;
  }
  else
  {
    req->rc = rc;
  }

  /* ...and report to the parent waiting in spawn2 ASAP */
  arc = DosPostEventSem(hev);
  ASSERT_MSG(arc == NO_ERROR || arc == ERROR_ALREADY_POSTED, "%ld %lx", arc, hev);

  if (rc != -1)
  {
#if 0
    /*
     * Wait for the child and forward its termination status to our parent,
     * grabbed from kLIBC's emx/src/lib/sys/__spawnve.c). A better solution
     * would be to put this code to some kLIBC extension func used both in
     * spawnvpe and user apps (which need P_OVERLAY functionality but want to
     * kick in between to grab the child PID etc (TODO).
     */

    pid_t pid = rc;

    TRACE("waiting for wrapped child (pid %d (%x)) to end\n", pid, pid);

    for (;;)
    {
      siginfo_t SigInfo = {0};
      do
        rc = __libc_Back_processWait(P_PID, pid, &SigInfo, WEXITED, NULL);
      while (rc == -EINTR);
      if (rc < 0)
          break;
      TRACE("__libc_Back_processWait(P_PID,%d,,WEXITED,NULL) returned %d si_code=%d si_status=%#x (%d)\n",
            pid, rc, SigInfo.si_code, SigInfo.si_status, SigInfo.si_status);
      ASSERT_MSG(SigInfo.si_pid == pid, "Expected pid %d and got %d!\n", pid, SigInfo.si_pid);
      if (    SigInfo.si_code == CLD_STOPPED
          ||  SigInfo.si_code == CLD_CONTINUED)
      {
        /* notify parent. */
        /** @todo proxy job control */
      }
      else
      {
        /*
         * Terminate the process.
         */
        int iStatus = SigInfo.si_status;
        switch (SigInfo.si_code)
        {
          default:
            ASSERT_MSG(0, "Invalid si_code=%#x si_status=%#x\n", SigInfo.si_code, SigInfo.si_status);
          case CLD_EXITED:
            __libc_spmTerm(__LIBC_EXIT_REASON_EXIT, iStatus);
            break;
          case CLD_KILLED:
            __libc_spmTerm(__LIBC_EXIT_REASON_SIGNAL_BASE + iStatus, 0);
            iStatus = 127;
            break;
          case CLD_DUMPED:
            if (iStatus == SIGSEGV || iStatus > SIGRTMAX || iStatus <= 0)
              __libc_spmTerm(__LIBC_EXIT_REASON_XCPT, 0);
            else
              __libc_spmTerm(__LIBC_EXIT_REASON_SIGNAL_BASE + iStatus, 0);
            iStatus = 127;
            break;
          case CLD_TRAPPED:
            if (iStatus <= SIGRTMAX && iStatus > 0)
              __libc_spmTerm(__LIBC_EXIT_REASON_SIGNAL_BASE + iStatus, 0);
            else
              __libc_spmTerm(__LIBC_EXIT_REASON_TRAP, 0);
            iStatus = 127;
            break;
        }

        TRACE("Calling DosExit(,0)\n");
        for (;;)
          DosExit(EXIT_PROCESS, iStatus);
        break; /* won't get here */
      }
    }

    ASSERT_MSG(0, "__libc_Back_processWait(P_PID,%d,,WEXITED,NULL) returned %d\n", pid, rc);
    __libc_spmTerm(__LIBC_EXIT_REASON_KILL + SIGABRT, 123);
    for (;;)
      DosExit(EXIT_PROCESS, 123);
    /* won't get here */
  }
#else
    pid_t pid = rc;
    int status;

    rc = waitpid(pid, &status, 0);
    TRACE("waitpid(wrapped child) rc %d (%x) status %lx errno %d\n", rc, rc, status, errno);

    if (rc != -1)
    {
      if (WIFEXITED(status))
        rc = WEXITSTATUS(status);
      else if (WIFSIGNALED(status))
        raise(WTERMSIG(status));
      else
        ASSERT_MSG(0, "invalid status %lx", status);
    }
#endif
  }

//  /*
//   * We only get here if starting the child process fails (like file not
//   * found etc). We must report the error to our parent.
//   */

//  ASSERT(rc == -1);
//  req->rc = rc;
//  req->err = errno;

//  /* Signal the semaphore to break the wait loop ASAP */
//  arc = DosPostEventSem(hev);
//  ASSERT_MSG(arc == NO_ERROR || arc == ERROR_ALREADY_POSTED, "%ld %lx", arc, hev);

  return rc;
}
