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

#include "shared.h"

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
  if (mmap_exception(report, reg, ctx))
    return XCPT_CONTINUE_EXECUTION;

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

  __main_hook_return(&newstack);
}
