/*
 * urpo: unlink rename pending operation
 * A library to allow unlink/rename of open files.
 *
 * Copyright (C) 2008-11 Yuri Dario <mc6530@mclink.it>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#define OS2EMX_PLAIN_CHAR
#define INCL_BASE
#include <os2.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>

#include "shared.h"

#include "../shared.h"

/**
 * Scan the pending structures and try operation
 *
 * @returns 0 on success.
 * @returns -1 on failure.
 */
int uropPending(void)
{
  global_lock();

  MUROP *pMUROP = gpData->urop;
  if (!pMUROP)
  {
    /* Nothing to do */
    return 0;
  }

    int i=0;
    LOG((stderr, "(%d) uropPending %d ops\n", getpid(), pMUROP->cUROP));
    // scan array
    while( i<pMUROP->cUROP) {
		int	done = 0;

		LOG((stderr, "(%d) uropPending #%d/%d\n", getpid(), i, pMUROP->cUROP));
		LOG((stderr, "(%d) uropPending old %s\n", getpid(), pMUROP->urop[i].szPathOld));
		LOG((stderr, "(%d) uropPending new %d\n", getpid(), pMUROP->urop[i].szPathNew[0]));

		// if file is missing, just remove it from list
		if (access( pMUROP->urop[i].szPathOld, F_OK) != 0) {

			done = 1;

		} else if (pMUROP->urop[i].szPathNew[0] != 0) { // is it a rename?
			// call libc directly
			int rc = _std_rename( pMUROP->urop[i].szPathOld, pMUROP->urop[i].szPathNew);
			LOG((stderr, "(%d) uropPending _std_rename %s,%s errno=%d\n", 
			getpid(), pMUROP->urop[i].szPathOld, pMUROP->urop[i].szPathNew, errno));
			// if it worked (or file deleted), reset data
			if (rc == 0 || (rc == -1 && errno == ENOENT)) {
				done = 1;
			}
		} else { // then it is an unlink

			// call libc directly
			LOG((stderr, "(%d) uropPending unlinking %s\n", getpid(), pMUROP->urop[i].szPathOld));
			int rc = _std_unlink( pMUROP->urop[i].szPathOld);
			if (rc == -1 && errno == EISDIR)
				rc = _std_rmdir( pMUROP->urop[i].szPathOld);
			LOG((stderr, "(%d) uropPending _std_unlink/rmdir %s errno=%d\n", getpid(), pMUROP->urop[i].szPathOld, errno));
			// if it worked (or file already deleted), reset data
			if (rc == 0 || (rc == -1 && errno == ENOENT)) {
				done = 1;
			}
		}

		// compact array
		if (done) {
			// move list down 1 pos
			pMUROP->cUROP--;
			LOG((stderr, "uropPending count (%d)\n", pMUROP->cUROP));
			memcpy( &pMUROP->urop[i], &pMUROP->urop[i+1], sizeof( UROP)*pMUROP->cUROP);
			// reset last element
			pMUROP->urop[ pMUROP->cUROP+1].szPathOld[0] = 0;
			pMUROP->urop[ pMUROP->cUROP+1].szPathNew[0] = 0;
		} else // go to next one
			i++;

		LOG((stderr, "(%d) uropPending do next one (%d)\n", getpid(), i));
    }

  global_unlock();

    // ok
    return 0;
}

/**
 * Add new operation to shared list
 *
 * @returns 0 on success.
 * @returns -1 on failure.
 */
int uropAdd( const char *pszPathOld, const char *pszPathNew)
{
  global_lock();

  MUROP *pMUROP = gpData->urop;
  if (!pMUROP)
  {
    /* Allocate */
    pMUROP = _ucalloc(gpData->heap, 1, sizeof(*pMUROP));
    if (!pMUROP)
    {
      TRACE("No memory\n");
      errno = ENOMEM;
      return -1;
    }

    gpData->urop = pMUROP;
    TRACE("gpData->urop %p\n", gpData->urop);
  }

    // still free space?
    if (pMUROP->cUROP == MAX_UROP) {
		LOG((stderr, "MAX_UROP reached (%d)\n", MAX_UROP));
		return -1;
    }

    // save names
    strcpy( pMUROP->urop[ pMUROP->cUROP].szPathOld, pszPathOld);
    if (pszPathNew)
	strcpy( pMUROP->urop[pMUROP->cUROP].szPathNew, pszPathNew);
    else
	pMUROP->urop[ pMUROP->cUROP].szPathNew[0] = 0;

    // increment limit
    pMUROP->cUROP++;
    LOG((stderr, "uropAdd count (%d)\n", pMUROP->cUROP));

  global_unlock();

    // done
    return 0;
}


/**
 * Dump shared list
 *
 * @returns 0 on success.
 * @returns -1 on failure.
 */
int urpoDump(void)
{
  global_lock();

  MUROP *pMUROP = gpData->urop;
  if (!pMUROP)
  {
    /* Nothing to do */
    return 0;
  }

    // scan array
    int i = 0;
    printf( "uropDump count (%d)\n", pMUROP->cUROP);
    while( i<pMUROP->cUROP) {
	// if *New!=0 it is a rename
	if (pMUROP->urop[i].szPathNew[0] != 0) {
	    LOG((stderr, "rename(%s,%s)\n", pMUROP->urop[i].szPathOld, pMUROP->urop[i].szPathNew));
	} else { // then it is an unlink
	    LOG((stderr, "unlink(%s)\n", pMUROP->urop[i].szPathOld));
	}
	i++;
    }

  global_unlock();

    // done
    return 0;
}

/**
 * Reset shared list
 *
 * @returns 0 on success.
 * @returns -1 on failure.
 */
int urpoReset(void)
{
  global_lock();

  MUROP *pMUROP = gpData->urop;
  if (!pMUROP)
  {
    /* Nothing to do */
    return 0;
  }

    // scan array
    int	i;
    for (i = 0; i<MAX_UROP; i++) {
        pMUROP->urop[i].szPathOld[0] = 0;
        pMUROP->urop[i].szPathNew[0] = 0;
    }
    pMUROP->cUROP = 0;
    printf( "uropReset count (%d)\n", pMUROP->cUROP);

  global_unlock();

    // done
    return 0;
}

/**
 * Initializes the urpo shared structures.
 * Called after successfull gpData allocation and gpData->heap creation.
 * @return NO_ERROR on success, DOS error code otherwise.
 */
int urpo_init()
{
  return 0;
}

/**
 * Uninitializes the urpo shared structures.
 * Called upon each process termination before gpData is uninitialized
 * or destroyed.
 */
void urpo_term()
{
  /* Process pending operations */
  uropPending();

  if (gpData->refcnt == 0)
  {
    /* We are the last process, free urpo structures */
    if (gpData->urop)
      free(gpData->urop);
  }
}
