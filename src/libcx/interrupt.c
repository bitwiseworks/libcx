/*
 * LIBCx interrupt service implementation.
 * Copyright (C) 2020 bww bitwise works GmbH.
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

#define OS2EMX_PLAIN_CHAR
#define INCL_BASE
#include <os2.h>

#include <exceptq.h>

#include <errno.h>
#include <sys/smutex.h>

#include "../shared.h"

#define REQ_RES_CRASH 0x1
#define REQ_RES_WAITING 0x2
#define REQ_RES_WAITING_RELEASE 0x4

/* exceptq.h is bogus and doesn't declare this when no INCL_LIBLOADEXCEPTQ is defined */
BOOL LibLoadExceptq(EXCEPTIONREGISTRATIONRECORD* pExRegRec);

typedef struct InterruptResult
{
  struct InterruptResult *next;
  struct InterruptResult *wait_next;

  int rc;
  pid_t pid;
  _smutex mutex;
  int8_t flags; /* REQ_RES_* */
} InterruptResult;

typedef struct InterruptRequest
{
  struct InterruptRequest *next;

  pid_t pid;
  INTERRUPT_WORKER *worker;
  void *data;
  InterruptResult *result;
} InterruptRequest;

typedef struct Interrupts
{
  int tid; /* Worker thread ID (if request handling is active) or 0 */
  InterruptRequest *first; /* Head of the pending request list */
  InterruptRequest *last; /* Tail of the pending request list */
  InterruptRequest *active; /* List of requests being handled by the worker */
  InterruptResult *results; /* List of results this process has to release */
  InterruptResult *wait_results; /* List of results this process has to wait for release */
} Interrupts;

static VOID APIENTRY interrupt_worker(ULONG data);

static int release_result(InterruptResult *res)
{
  TRACE("Releasing result %p from process 0x%x\n", res, res->pid);

  ASSERT(res->pid != getpid());

  int delete = TRUE;

  ProcDesc *proc = find_proc_desc(res->pid);
  if (proc)
  {
    /* Look for a result in the target proc's wait_results. */
    InterruptResult *tgt_res = proc->interrupts->wait_results;
    InterruptResult *tgt_res_prev = NULL;
    while (tgt_res)
    {
      if (tgt_res == res)
      {
        if (__atomic_test_bit(&tgt_res->flags, REQ_RES_WAITING_RELEASE))
        {
          /*
           * The target process is waiting on the release of this result, let it
           * go (note that it is responsible for deletion of the result then).
           */
          TRACE("Releasing result mutex\n");
          _smutex_release(&res->mutex);
          delete = FALSE;

          /* The waited on result is always the first one */
          ASSERT(!tgt_res_prev);
        }

        /* Remove from list */
        if (tgt_res_prev)
          tgt_res_prev->next = tgt_res->next;
        else
          proc->interrupts->wait_results = tgt_res->next;

        break;
      }

      tgt_res_prev = tgt_res;
      tgt_res = tgt_res->wait_next;
    }
  }

  return delete;
}

/**
 * Does preliminary uninitialization of interrupt shared structures.
 * Called by LIBC on TID 1 when other threads are still active.
 */
static void interrupt_pre_term()
{
  TID tid = 0;
  int have_requests = FALSE;

  global_lock();

  ASSERT(gpProcDesc);

  if (gpProcDesc->interrupts->tid)
    tid = gpProcDesc->interrupts->tid;
  else if (gpProcDesc->interrupts->first)
    have_requests = TRUE;

  global_unlock();

  if (tid)
  {
    TRACE("Waiting for interrupt worker TID %d\n", tid);
    APIRET arc = DosWaitThread(&tid, DCWW_WAIT);
    TRACE("arc %lu\n", arc);
  }
  else if (have_requests)
  {
    /* We are still operational here and can serve interrupt requests */
    interrupt_worker(0);
  }

  global_lock();

  /*
   * Wait for a release of results in the requesting process to make sure we
   * won't go away before they finish.
   *
   * NOTE: This loop is advanced in release_result by changing wait_results.
   */

  while (gpProcDesc->interrupts->wait_results)
  {
    InterruptResult *res = gpProcDesc->interrupts->wait_results;

    /* TODO: call GLOBAL_MEM_SET_OWNER(res, getpid()) when it's ready */

    __atomic_set_bit(&res->flags, REQ_RES_WAITING_RELEASE);

    global_unlock();

    ASSERT(res->pid == getpid());

    if (__atomic_test_bit(&res->flags, REQ_RES_WAITING))
    {
      /* The requesting process hasn't got the result yet, let it do so */
      TRACE("Letting requesting process get result %p\n", res);
      while (__atomic_test_bit(&res->flags, REQ_RES_WAITING))
        DosSleep(0);
    }

    TRACE("Waiting for request release for result %p\n", res);
    _smutex_request(&res->mutex);
    TRACE("Done waiting\n");

    global_lock();

    /* It's our responsibility to delete wait_result */
    free(res);
  }

  global_unlock();
}

/**
 * Initializes the interrupt shared structures.
 * Called upon each process startup after successfull ProcDesc and gpData
 * allocation.
 */
void interrupt_init(ProcDesc *proc, int forked)
{
    /* Initialize our part of ProcDesc */
  GLOBAL_NEW(proc->interrupts);
  ASSERT(proc->interrupts);

  /* NOTE: The callback is already there for forked children */
  if (!forked)
    atexit(interrupt_pre_term);
}

/**
 * Uninitializes the interrupt shared structures.
 * Called upon each process termination before gpData destruction. Note that
 * @a proc may be NULL on critical failures.
 */
void interrupt_term(ProcDesc *proc)
{
  if (proc)
  {
    TRACE("interrupts first %p last %p active %p results %p wait_results %p (flags 0x%x)\n",
          proc->interrupts->first, proc->interrupts->last,
          proc->interrupts->active, proc->interrupts->results,
          proc->interrupts->wait_results,
          proc->interrupts->wait_results ? proc->interrupts->wait_results->flags : 0);

    /*
     * We may have unhandled active or pending interrupt requests in case of
     * abnormal termination (e.g. a crash in either the worker thread or
     * elsewhere, accordingly). Release waiting parties to avoid deadlocks.
     */

    /* First, join both lists (order is irrelevant) */
    InterruptRequest *req = proc->interrupts->first;
    if (req)
      proc->interrupts->last->next = proc->interrupts->active;
    else
      req = proc->interrupts->active;

    while (req)
    {
      if (req->result)
      {
        /* Signal the waiting party */
        _smutex_release(&req->result->mutex);

        /* TODO: call GLOBAL_MEM_SET_OWNER(req->result, req->pid) when it's ready */
      }

      InterruptRequest *req_prev = req;
      req = req->next;
      free(req_prev);
    }

    /* If there are any unreleased results, release them now. */
    InterruptResult *res = proc->interrupts->results;
    while (res)
    {
      InterruptResult *res_prev = res;
      res = res->next;
      /* Ignore results with no pid, they haven't been processed yet */
      if (res_prev->pid)
      {
        if (release_result(res_prev))
          free(res_prev);
      }
    }

    free(proc->interrupts);
  }
}

/**
 * Thread function that calls the interrupt request worker.
 *
 * NOTE: No mmap and FPU control word changing calls or other calls (directly or
 * through another DLL) that require a LIBCx exception handler are allowed here
 * as this exception handler is not installed by this thread function.
 */
static VOID APIENTRY interrupt_worker(ULONG data)
{
  EXCEPTIONREGISTRATIONRECORD xcptRec;

  /* Install the EXCEPTQ trap generator */
  LibLoadExceptq(&xcptRec);

  TRACE("BEGIN\n");

  while (1)
  {
    global_lock();

    if (!gpProcDesc || !gpProcDesc->interrupts->first)
    {
      /* No more requests, mark as finished and bail out */
      if (gpProcDesc)
        gpProcDesc->interrupts->tid = 0;
      global_unlock ();
      break;
    }

    /* Take current requests */
    gpProcDesc->interrupts->active = gpProcDesc->interrupts->first;
    gpProcDesc->interrupts->first = NULL;
    gpProcDesc->interrupts->last = NULL;

    /* TODO: call GLOBAL_MEM_SET_OWNER when it's ready: */
    /*
    InterruptRequest *req = gpProcDesc->interrupts->active;
    while (req)
    {
      GLOBAL_MEM_SET_OWNER(req->result, getpid());
      req = req->next;
    }
    */

    global_unlock();

    /*
     * Active is only accessed by either this thread or by TID 1 on process
     * termination when there are no other threads so it's safe w/o the lock.
     */
    InterruptRequest *req = gpProcDesc->interrupts->active;
    ASSERT(req);

    while (req)
    {
      TRACE("Calling req %p worker %p data %p result %p\n", req, req->worker, req->data, req->result);

      /* Handle the request */
      ASSERT(req->worker);
      if (req->result)
      {
        req->result->rc = req->worker(req->pid, req->data);
        TRACE("Fihished with rc %d\n", req->result->rc);

        global_lock();

        /* Mark the result as produced by this process */
        req->result->pid = getpid();

        /* Put the result into the list of results to be waited for release */
        req->result->wait_next = gpProcDesc->interrupts->wait_results;
        gpProcDesc->interrupts->wait_results = req->result;

        /* Signal the waiting party */
        _smutex_release(&req->result->mutex);

        /* TODO: call GLOBAL_MEM_SET_OWNER(req->result, req->pid) when it's ready */

        global_unlock();
      }
      else
      {
        req->worker(req->pid, req->data);
        TRACE("Fihished\n");
      }

      InterruptRequest *req_prev = req;
      req = req->next;
      free(req_prev);

      /* Forget the deleted request */
      gpProcDesc->interrupts->active = req;
    }
  }

  TRACE("END\n");

  UninstallExceptq(&xcptRec);
}

/**
 * System exception handler for the interrupt service.
 *
 * @return 1 to retry execution, 0 to call other handlers.
 */
int interrupt_exception(struct _EXCEPTIONREPORTRECORD *report,
                        struct _EXCEPTIONREGISTRATIONRECORD *reg,
                        struct _CONTEXT *ctx)
{
  /* Ignore nested & unwinding exceptions. */
  if (report->fHandlerFlags & (EH_NESTED_CALL | EH_UNWINDING))
    return 0;

  if (report->ExceptionNum != XCPT_SIGNAL ||
      report->ExceptionInfo[0] != XCPT_SIGNAL_KILLPROC)
    return 0;

  TRACE("XCPT_SIGNAL_KILLPROC [flags %lx nested %p addr %p]\n",
        report->fHandlerFlags, report->NestedExceptionReportRecord, report->ExceptionAddress);

  /*
   * NOTE: According to my tests, XCPT_SIGNAL_KILLPROC delivery is sequential
   * (never nests itself or any other exception).
   */

  global_lock();

  TRACE("gpProcDesc %p interrupts->tid %d interrupts->first %p\n",
        gpProcDesc, gpProcDesc ? gpProcDesc->interrupts->tid : 0,
        gpProcDesc ? gpProcDesc->interrupts->first : 0);

  if (!gpProcDesc || !gpProcDesc->interrupts->first)
  {
    /* It's not from `interrupt_request`, continue search */
    global_unlock();
    return 0;
  }

  /*
   * Serve the request on a separate thread to avoid unexpected (and potentially
   * unsupported) reentrancy. Note that we can't use _beginthread here for
   * reentrancy reasons (it makes LIBC calls, e.g. heap halloc, that modify its
   * internal structures, use non-reentrant locks etc).
   */
  TID tid;
  APIRET arc = DosCreateThread(&tid, interrupt_worker, 0, CREATE_READY,  512 * 1024);

  TRACE("Worker TID %d (arc %lu)\n", tid, arc);
  ASSERT(!arc && tid != 0);
  gpProcDesc->interrupts->tid = tid;

  global_unlock();

  /* Continue execution */
  return 1;
}

/**
 * Places an interrupt request into the request queue of the target process.
 *
 * The worker function will be called with the specified @a data argument as
 * soon as the target process receives the interrupt signal. If @a result is not
 * NULL, this function will not return until the worker function completes in
 * the target process and stores the result of its execution in the provided @a
 * result variable.
 *
 * Note that the worker function must be as simple and as fast as possible as it
 * intervents the application and this intervention should be kept low profile.
 * Also, the worker function is not allowed to use mmap API or make FPU control
 * word changing calls or other calls (directly or through another DLL) that
 * require a LIBCx exception handler as it's not installed on a helper thread
 * where the worker function is called.
 *
 * Fails with ECANCELED if this function was waiting for the result but the
 * target process crashed (in the worker function or elsewhere) before
 * completing.
 *
 * TODO: Use GLOBAL_MEM_SET_OWNER on data and result->data when its ready.
 *
 * @return     0 on success or -1 and sets errno on failure.
 */
int interrupt_request(pid_t pid, INTERRUPT_WORKER *worker, void *data, INTERRUPT_RESULT *result)
{
  TRACE("pid 0x%x worker %p data %p result %p\n", pid, worker, data, result);

  int rc = -1;

  if (pid == getpid() || !worker)
  {
    errno = EINVAL;
    return rc;
  }

  InterruptResult *req_result = NULL;

  global_lock();

  do
  {
    ProcDesc *proc = find_proc_desc(pid);
    if (!proc)
    {
      errno = ESRCH;
      break;
    }

    InterruptRequest *req = NULL;

    GLOBAL_NEW(req);
    if (!req)
    {
      errno = ENOMEM;
      break;
    }

    if (result)
    {
      GLOBAL_NEW(req->result);
      if (!req->result)
      {
        free(req);
        errno = ENOMEM;
        break;
      }

      req_result = req->result;
    }

    rc = 0;

    req->pid = getpid();
    req->worker = worker;
    req->data = data;

    int have_first_req = !proc->interrupts->first;

    if (req_result)
    {
      /* Prepare for result waiting */
      _smutex_request(&req_result->mutex);
      __atomic_set_bit(&req_result->flags, REQ_RES_WAITING);
    }

    /* Raise an interrupt signal unless already done so */
    if (have_first_req && !proc->interrupts->tid)
    {
      APIRET arc = DosKillProcess(DKP_PROCESS, pid);
      TRACE("DosKillProcess returned %lu\n", arc);
      if (arc)
      {
        /* Free newly created structs on failure to signal */
        if (req_result)
          free(req_result);
        free(req);
        errno = ESRCH;
        rc = -1;
        break;
      }
    }

    /* Append req to the list of requests for the target process */
    if (!proc->interrupts->first)
      proc->interrupts->first = req;
    else
      proc->interrupts->last->next = req;
    proc->interrupts->last = req;

    /* TODO: call GLOBAL_MEM_SET_OWNER(req, pid) when it's ready */
    /* TODO: call GLOBAL_MEM_SET_OWNER(req_result, pid) when it's ready */
  }
  while (0);

  global_unlock();

  if (rc == 0 && result)
  {
    ASSERT(req_result);

    /* Wait for the result */
    TRACE("Waiting for result\n");
    _smutex_request(&req_result->mutex);

    TRACE("Ready with rc %d flags 0x%x\n", req_result->rc, req_result->flags);

    __atomic_clear_bit(&req_result->flags, REQ_RES_WAITING);

    if (__atomic_test_bit(&req_result->flags, REQ_RES_CRASH))
    {
      errno = ECANCELED;
      rc = -1;
    }
    else
    {
      /* TODO: call GLOBAL_MEM_SET_OWNER(req_result, getpid()) when it's ready */

      /* Put the result into the list of results to be released */
      req_result->next = gpProcDesc->interrupts->results;
      gpProcDesc->interrupts->results = req_result;

      /* Pass the result to the caller */
      *result = req_result;
    }
  }

  return rc;
}

/**
 * Returns the result code of the given request result.
 */
int interrupt_request_rc(INTERRUPT_RESULT result)
{
  ASSERT (result);
  return ((InterruptResult *)result)->rc;
}

/**
 * Releases the given request result.
 *
 * Must alwasy be called when the result is processed and not needed any more.
 */
void interrupt_request_release(INTERRUPT_RESULT result)
{
  ASSERT (result);

  global_lock();

  InterruptResult *release_res = (InterruptResult *)result;

  InterruptResult *res = gpProcDesc->interrupts->results;
  InterruptResult *res_prev = NULL;
  while (res && res != release_res)
  {
    res_prev = res;
    res = res->next;
  }

  ASSERT(res == release_res);

  if (res_prev)
    res_prev->next = res->next;
  else
    gpProcDesc->interrupts->results = res->next;

  if (release_result(res))
    free(res);

  global_unlock();
}
