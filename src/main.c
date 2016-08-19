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

  __main_hook_return(&newstack);
}
