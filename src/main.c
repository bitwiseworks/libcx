/*
 * main() hook implementation for kLIBC.
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

#define OS2EMX_PLAIN_CHAR
#define INCL_BASE
#include <os2.h>

#define INCL_LOADEXCEPTQ
#define INCL_FORKEXCEPTQ
#include <exceptq.h>

#include <memory.h>
#include <stdlib.h>
#include <unistd.h>
#include <pwd.h>

#include <stdlib.h>
#include <emx/umalloc.h>

#include <InnoTekLIBC/thread.h>

#include "lend/ld32.h"

#include "shared.h"

int gFpuCwTls = -1;

/* Defined in libcx.def */
unsigned _libc__control87(unsigned new_cw, unsigned mask);

/**
 * LIBC _control87 replacement. Used to grab the new FPU CW.
 */
unsigned _control87(unsigned new_cw, unsigned mask)
{
  if (!mask)
    return _libc__control87(new_cw, mask);

  unsigned cw = _libc__control87(new_cw, mask);

  // Update our TLS with the new value.
  if (gFpuCwTls != -1)
    __libc_TLSSet(gFpuCwTls, (void *)((cw & ~mask) | (new_cw & mask)));

  return cw;
}

/**
 * LIBCx exception handler.
 *
 * @param report Report record.
 * @param reg Registration record.
 * @param ctx Context record.
 * @return XCPT_CONTINUE_SEARCH or XCPT_CONTINUE_EXECUTION.
 */
ULONG _System libcxExceptionHandler(PEXCEPTIONREPORTRECORD report,
                                    PEXCEPTIONREGISTRATIONRECORD reg,
                                    PCONTEXTRECORD ctx,
                                    PVOID unused)
{
  if (interrupt_exception(report, reg, ctx))
    return XCPT_CONTINUE_EXECUTION;

  if (mmap_exception(report, reg, ctx))
    return XCPT_CONTINUE_EXECUTION;

  // Handle FPU exceptions to protect from multiple Gpi and Win APIs changing
  // the FPU control word to a value not expected by GCC and not restoring it
  // afterwards.
  switch (report->ExceptionNum)
  {
    case XCPT_FLOAT_DENORMAL_OPERAND:
    case XCPT_FLOAT_DIVIDE_BY_ZERO:
    case XCPT_FLOAT_INEXACT_RESULT:
    case XCPT_FLOAT_INVALID_OPERATION:
    case XCPT_FLOAT_OVERFLOW:
    case XCPT_FLOAT_STACK_CHECK:
    case XCPT_FLOAT_UNDERFLOW:
    {
      unsigned cw = ctx->ctx_env[0]; // FPU Control Word
      unsigned expectedCw = (unsigned)__libc_TLSGet(gFpuCwTls);

      if (cw != expectedCw)
      {
        // Somebody has changed the FP CW behind our back (e.g. not via
        // _control87). Restore it and attempt to retry the faulty FPU
        // instrcution (so that FPU will correctly set the result of the
        // operation to Inf/NaN in case of FDIV etc). Note that the faulting
        // address does not necessarily point to the faulty FPU instruction
        // because FPU exceptions are raised only at the next FPU instruction
        // in the stream (or WAIT/FWAIT). And although we have the address
        // of the faulty FPU instruction in the saved FPU context, we may
        // not retry from there if there are some other instructions in between
        // because they could potentially change the operands, the stack etc.
        // So we only retry if the faulty FPU instruction exactply precedes
        // the faulting address so that there is nothing in between that could
        // affect the retry attempt. See Chapters 4.9, 8.6 and 8.7 in
        // https://software.intel.com/sites/default/files/managed/a4/60/253665-sdm-vol-1.pdf

        ctx->ctx_env[0] = expectedCw;
        ULONG ip = ctx->ctx_env [3]; // FPU Instruction Pointer
        if (length_disasm ((void *)ip) + ip == ctx->ctx_RegEip)
          ctx->ctx_RegEip = ip;
        return XCPT_CONTINUE_EXECUTION;
      }
    }
  }

  // If we got that far, it's very likely that we are about to crash. Check if
  // we are holding our global mutex and log it if we are. Note that we ignore
  // XCPT_ASYNC_PROCESS_TERMINATE as it is used by kLIBC to deliver POSIX
  // signals to threads that may happen while holding the mutex as well.
  if (report->ExceptionNum != XCPT_ASYNC_PROCESS_TERMINATE &&
      !(report->fHandlerFlags & (EH_NESTED_CALL | EH_UNWINDING)))
    global_lock_deathcheck();

  return XCPT_CONTINUE_SEARCH;
}

struct mainstack
{
  /** Argument count. */
  int argc;
  /** Pointer to argument vector. */
  char **argv;
  /** Pointer to environmet vector. */
  char **envp;
};

static void flush_all_streams()
{
  flushall();
}

__attribute__((__noreturn__))
extern void __main_hook_return(void *stack);

/**
 * Called right before entering main().
 * Used to install our own exception handler on the main() stack.
 * @param stack Pointer to the top of the main() stack (arguments).
 */
__attribute__((__noreturn__))
void __main_hook(struct mainstack *stack)
{
  /** New top of stack unpon 'return' from this function. */
  struct
  {
    /* main() arguments */
    struct mainstack stack;
    /* LIBCx exception handler */
    EXCEPTIONREGISTRATIONRECORD libcXcptRec;
    /* EXCEPTQ exception handler */
    EXCEPTIONREGISTRATIONRECORD exceptqXcptRec;
  } newstack;

  /* Preserve main() arguments */
  memcpy(&newstack.stack, stack, sizeof(struct mainstack));

  /* Install the EXCEPTQ trap generator */
  LoadExceptq(&newstack.exceptqXcptRec, NULL, NULL);

  /* Install the LIBCx own exception handler for various needs */
  newstack.libcXcptRec.ExceptionHandler = libcxExceptionHandler;
  newstack.libcXcptRec.prev_structure = END_OF_CHAIN;
  DosSetExceptionHandler(&newstack.libcXcptRec);

  /*
   * Register a callback to flush all buffered streams at program termination
   * which is needed to avoid data loss when I/O is redirected to TCP sockets.
   * Ssee #31 for more deatils. Note that we rely on the fact that _std_exit
   * calls atexit callbacks in reverse order so that we are called last, after
   * program callbacks that may potentially do some buffered I/O too.
   */
  atexit(flush_all_streams);

  /*
   * EXPERIMENTAL: Set the Unix user ID of this process to the user specified
   * in the LOGNAME/USER environment variable. This will e.g. make all files
   * and directories have this user's ID and group ID instead of root. It will
   * also make many tools (e.g. yum) break due to the lack of root priveleges.
   * For this reason, it's disabled by default. We will re-enable it once we've
   * got sudo and such.
   */

  if (getenv("LIBCX_SETUID"))
  {
    const char *name = getenv("LOGNAME");
    if (!name)
      name = getenv("USER");
    if (name)
    {
      struct passwd *pw = getpwnam(name);
      if (pw)
        setuid(pw->pw_uid);
    }
  }

  /*
   * Below we force high memory for the default C heap by default and also
   * allow a bunch of other modes via an environment variable. See #48 for
   * details. Note that there is one side effect (unless LIBCX_HIGHMEM=2) - the
   * heap initialization will happen here immediately rather than on demand
   * upon the first malloc. This is minor, however, since it's usually the case
   * anyway due to _CRT_init and other pre-main callbacks after we return.
   */

  static const char *highmem_var = "LIBCX_HIGHMEM";
  int highmem = 1;
  _getenv_int(highmem_var, &highmem);

  switch (highmem)
  {
    case 0:
      /* Force low memory for the default C heap */
      _um_regular_heap = _linitheap();
      _udefault (_um_regular_heap);
      break;
    case 3:
      /* Safety mode; abort if not all DLLs voted for high memory */
      if (__libc_HeapGetResult() == 0)
      {
        printf ("libcx: this EXE or some DLL is built without -Zhigh-mem, "
                "aborting due to %s=%d!\n",
                highmem_var, highmem);
        abort();
      }
      /* Fall through (to force high mem if all DLLs are safe) */
    case 1:
    default:
      /* Force high memory for the default C heap */
      _um_regular_heap = _hinitheap();
      _udefault (_um_regular_heap);
      break;
    case 2:
      /* Normal kLIBC voting procedure: do nothing */
      break;
  }

  /* Save the FPU control word for the exception handler */
  {
    gFpuCwTls = __libc_TLSAlloc();
    ASSERT(gFpuCwTls != -1);

    unsigned cw = _libc__control87(0, 0);
    __libc_TLSSet(gFpuCwTls, (void*)cw);
  }

  __main_hook_return(&newstack);
}
