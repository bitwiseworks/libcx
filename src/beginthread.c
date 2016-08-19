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
extern int libc__beginthread(void (*start)(void *arg), void *stack,
                             unsigned stack_size, void *arg_list);

/**
 * LIBCx thread wrapper.
 *
 * @param arg Thread function argument.
 */
static void threadWrapper(void *d)
{
  EXCEPTIONREGISTRATIONRECORD exceptqXcptRec;

  /* Install the EXCEPTQ trap generator */
  LibLoadExceptq(&exceptqXcptRec);

  /* Use new block to ensure proper order on stack */
  {
    EXCEPTIONREGISTRATIONRECORD libcXcptRec;

    /* Install the LIBCx own exception handler for various needs */
    libcXcptRec.ExceptionHandler = libcxExceptionHandler;
    libcXcptRec.prev_structure = END_OF_CHAIN;
    DosSetExceptionHandler(&libcXcptRec);

    /* Thread data is on heap, make a copy on stack and free it */
    struct threaddata data = *(struct threaddata *)d;
    free(d);

    data.start(data.arg);

    DosUnsetExceptionHandler(&libcXcptRec);
  }

  UninstallExceptq(&exceptqXcptRec);
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

  int rc = libc__beginthread(threadWrapper, stack, stack_size, d);
  if (rc == -1)
    free(d);

  return rc;
}
