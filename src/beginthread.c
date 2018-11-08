/*
 * _beginthread override for kLIBC.
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

#define INCL_LIBLOADEXCEPTQ
#define INCL_FORKEXCEPTQ
#include <exceptq.h>

#include <memory.h>
#include <stdlib.h>
#include <errno.h>

#include "shared.h"

/* Defined in main.c */
extern
ULONG _System libcxExceptionHandler(PEXCEPTIONREPORTRECORD report,
                                    PEXCEPTIONREGISTRATIONRECORD reg,
                                    PCONTEXTRECORD ctx,
                                    PVOID unused);

struct threaddata
{
  void (*start)(void *arg);
  void *arg;
};

/* Defined in libcx.def */
extern int _libc_beginthread(void (*start)(void *arg), void *stack,
                             unsigned stack_size, void *arg_list);

/**
 * LIBCx thread wrapper.
 *
 * @param arg Thread function argument.
 */
static void threadWrapper(void *d)
{
  /*
   * Use an array of registartion records to guarantee their order on the
   * stack: DosSetExceptionHandler requires the registration record of an inner
   * exception handler to be located deeper on the stack (i.e. have a lower
   * address given that the stack grows down) than the registration record of
   * an outer one. Note that using nested scopes *does not* guarantee the order
   * — the GCC optimizer is free to rearrange variables on the stack.
   */
  EXCEPTIONREGISTRATIONRECORD xcptRec[2];

  /* Install the EXCEPTQ trap generator (outer, higher address) */
  LibLoadExceptq(&xcptRec[1]);

  /* Install the LIBCx own exception handler for various needs (inner, lower address) */
  xcptRec[0].ExceptionHandler = libcxExceptionHandler;
  xcptRec[0].prev_structure = END_OF_CHAIN;
  DosSetExceptionHandler(&xcptRec[0]);

  /* Thread data is on the heap, make a copy on the stack and free it */
  struct threaddata data = *(struct threaddata *)d;
  free(d);

  data.start(data.arg);

  DosUnsetExceptionHandler(&xcptRec[0]);

  UninstallExceptq(&xcptRec[1]);
}

/**
 * Overrides kLIBC _beginthread to install exception handlers on additional
 * threads.
 *
 * @param start Thread function.
 * @param stack Stack pointer.
 * @param stack_size Stack size.
 * @param arg_list Thread function argument.
 * @return Thread ID or -1 on failure.
 */
int _beginthread(void (*start)(void *arg), void *stack, unsigned stack_size,
                 void *arg_list)
{
  struct threaddata *d = (struct threaddata *)malloc(sizeof(*d));
  if (!d)
  {
    errno = ENOMEM;
    return -1;
  }

  d->start = start;
  d->arg = arg_list;

  int rc = _libc_beginthread(threadWrapper, stack, stack_size, d);
  if (rc == -1)
    free(d);

  return rc;
}
