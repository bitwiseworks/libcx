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

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "shared.h"

/* global variables */
static HMTX	hmtxUROP = NULL;
static MUROP*	pMUROP = NULL;
static int	refCount;

/**
 * initialize global dll loading/unloading.
 *
 * @returns 1 on success.
 * @returns 0 on failure.
 */
unsigned long _System _DLL_InitTerm(unsigned long hModule, unsigned long ulFlag)
{
    APIRET rc;

    switch (ulFlag) {
    case 0:
        {
            if (_CRT_init() != 0)
              return 0UL;
            __ctordtorInit();

	    // init shared mem
	    uropInit();
            LOG((stderr, "(%d) urpo dll init pending ops %d (data=%x)\n", getpid(), pMUROP->cUROP));
            break;
        }

    case 1:
        {
	    // flush buffers
	    uropPending();

            __ctordtorTerm();
            _CRT_term();
            break;
        }

    default:
         return 0UL;
    }

    // success
    return 1UL;
}


/* 
 * Perform cleanup at process exit
 */
VOID APIENTRY _cleanupPending(VOID)
{
    int rc;
    rc = uropPending();
    rc = DosCloseMutexSem( hmtxUROP);
    LOG((stderr, "(%d) _cleanupPending DosCloseMutexSem rc=%d\n", getpid(), rc));
	rc = DosExitList(EXLST_EXIT, (PFNEXITLIST) NULL);
}

/* 
 * Cleanup semaphore in case of early termination.
 * If this code is called, it means we did not have the time to call DosReleaseMutexSem
 * because this handler is removed at function exit.
 */
VOID APIENTRY _cleanupSemaphore(VOID)
{
	APIRET 	rc;
	PID 		pidOwner = -1;
	TID     	tidOwner = -1;
	ULONG	ulCount  = -1;
	LOG((stderr, "(%d) FATAL ERROR _cleanupSemaphore called!\n", getpid()));
	rc = DosQueryMutexSem(hmtxUROP, &pidOwner, &tidOwner, &ulCount);
	LOG((stderr, "(%d) _cleanupSemaphore DosQueryMutexSem rc=%d pidOwner=%d tidOwner=%d ulCount=%d\n", 
		getpid(), rc, pidOwner, tidOwner, ulCount));

    rc = DosReleaseMutexSem( hmtxUROP);
    LOG((stderr, "(%d) _cleanupSemaphore DosReleaseMutexSem rc=%d\n", getpid(), rc));

    rc = DosCloseMutexSem( hmtxUROP);
    LOG((stderr, "(%d) _cleanupSemaphore DosCloseMutexSem rc=%d\n", getpid(), rc));
	rc = DosExitList(EXLST_EXIT, (PFNEXITLIST) NULL);
}

/**
 * Initiates the pending structures data.
 *
 * @returns 0 on success.
 * @returns -1 on failure.
 */
int uropInit(void)
{
    APIRET	rc;

    rc = DosGetNamedSharedMem( (PPVOID)&pMUROP, (PSZ)SHMEM_UROP, PAG_READ | PAG_WRITE);
    if (rc) {
	// init memory
	rc = DosAllocSharedMem( (PPVOID)&pMUROP, (PSZ)SHMEM_UROP, sizeof(MUROP), PAG_READ | PAG_WRITE | PAG_EXECUTE | PAG_COMMIT | OBJ_ANY);
	if (rc == ERROR_ALREADY_EXISTS) // really?
	    return -1;
	if (rc)
	    rc = DosAllocSharedMem( (PPVOID)&pMUROP, (PSZ)SHMEM_UROP, sizeof(MUROP), PAG_READ | PAG_WRITE | PAG_EXECUTE | PAG_COMMIT);
	if (rc)
	    return -1;
    }

    // open the mutex for this process
    rc = DosOpenMutexSem( SEM_UROP, &hmtxUROP);          
    if (rc) 
    {
	// create unowned semaphore, registered for fork()
	rc = DosCreateMutexSemEx( SEM_UROP, &hmtxUROP, 0, FALSE);
	if (rc)
	    return -1;
    }

    // success
    return 0;
}

/**
 * Scan the pending structures and try operation
 *
 * @returns 0 on success.
 * @returns -1 on failure.
 */
int uropPending(void)
{
	APIRET	rc;

	// install cleanup code for data: do it always since forked() code does not init again
	// rc = DosExitList(EXLST_ADD | 0x5000, (PFNEXITLIST) _cleanupPending);

	// install cleanup code for semaphore
	rc = DosExitList(EXLST_ADD | 0x4000, (PFNEXITLIST) _cleanupSemaphore);

    rc = DosGetNamedSharedMem( (PPVOID)&pMUROP, (PSZ)SHMEM_UROP, PAG_READ | PAG_WRITE);
    LOG((stderr, "(%d) uropPending DosGetNamedSharedMem %x rc=%d\n", getpid(), pMUROP, rc));
    if (rc) {
        rc = uropInit();
        if (rc) {
            LOG((stderr, "uropPending uropInit failed rc=%d\n", rc));

			// remove cleanup code for semaphore
			rc = DosExitList(EXLST_REMOVE, (PFNEXITLIST) _cleanupSemaphore);

            return -1;
        }
    }

    // get semaphore
    rc = DosRequestMutexSem( hmtxUROP,(ULONG) SEM_INDEFINITE_WAIT);
    if (rc == ERROR_INVALID_HANDLE) {
        // open the mutex for this process
        rc = DosOpenMutexSem( SEM_UROP, &hmtxUROP);
		rc = DosRequestMutexSem( hmtxUROP,(ULONG) SEM_INDEFINITE_WAIT);
    }
    if (rc) {
		LOG((stderr, "(%d) uropPending DosRequestMutexSem failed rc=%d\n", getpid(), rc));

		// remove cleanup code for semaphore
		rc = DosExitList(EXLST_REMOVE, (PFNEXITLIST) _cleanupSemaphore);

		return -1;
    }
    LOG((stderr, "(%d) uropPending DosRequestMutexSem\n", getpid()));

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
			rc = _std_rename( pMUROP->urop[i].szPathOld, pMUROP->urop[i].szPathNew);
			LOG((stderr, "(%d) uropPending _std_rename %s,%s errno=%d\n", 
			getpid(), pMUROP->urop[i].szPathOld, pMUROP->urop[i].szPathNew, errno));
			// if it worked (or file deleted), reset data
			if (rc == 0 || (rc == -1 && errno == ENOENT)) {
				done = 1;
			}
		} else { // then it is an unlink

			// call libc directly
			LOG((stderr, "(%d) uropPending unlinking %s\n", getpid(), pMUROP->urop[i].szPathOld));
			rc = _std_unlink( pMUROP->urop[i].szPathOld);
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

    // release semaphore
    rc = DosReleaseMutexSem( hmtxUROP);
    LOG((stderr, "(%d) uropPending DosReleaseMutexSem rc=%d\n", getpid(), rc));

	// remove cleanup code for semaphore
	rc = DosExitList(EXLST_REMOVE, (PFNEXITLIST) _cleanupSemaphore);

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
    APIRET	rc;
    int 		i=0;

	// install cleanup code for data: do it always since forked() code does not init again
	// rc = DosExitList(EXLST_ADD | 0x5000, (PFNEXITLIST) _cleanupPending);

	// install cleanup code for semaphore
	rc = DosExitList(EXLST_ADD | 0x4000, (PFNEXITLIST) _cleanupSemaphore);

    rc = DosGetNamedSharedMem( (PPVOID)&pMUROP, (PSZ)SHMEM_UROP, PAG_READ | PAG_WRITE);
    if (rc) {
        rc = uropInit();
        if (rc) {
            LOG((stderr, "uropAdd uropInit failed rc=%d\n", rc));

			// remove cleanup code for semaphore
			rc = DosExitList(EXLST_REMOVE, (PFNEXITLIST) _cleanupSemaphore);

            return -1;
        }
    }

    // get semaphore
    rc = DosRequestMutexSem( hmtxUROP,(ULONG) SEM_INDEFINITE_WAIT);
    if (rc == ERROR_INVALID_HANDLE) {
        // open the mutex for this process
        rc = DosOpenMutexSem( SEM_UROP, &hmtxUROP);
		rc = DosRequestMutexSem( hmtxUROP,(ULONG) SEM_INDEFINITE_WAIT);
    }
    if (rc) {
		LOG((stderr, "(%d) uropAdd DosRequestMutexSem failed rc=%d\n", getpid(), rc));

		// remove cleanup code for semaphore
		rc = DosExitList(EXLST_REMOVE, (PFNEXITLIST) _cleanupSemaphore);

		return -1;
    }
    LOG((stderr, "(%d) uropAdd DosRequestMutexSem\n", getpid()));

    // still free space?
    if (pMUROP->cUROP == MAX_UROP) {
		rc = DosReleaseMutexSem( hmtxUROP);
		LOG((stderr, "MAX_UROP reached (%d)\n", MAX_UROP));

		// remove cleanup code for semaphore
		rc = DosExitList(EXLST_REMOVE, (PFNEXITLIST) _cleanupSemaphore);
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

    // release semaphore
    rc = DosReleaseMutexSem( hmtxUROP);
    LOG((stderr, "(%d) uropAdd DosReleaseMutexSem rc=%d\n", getpid(), rc));

	// remove cleanup code for semaphore
	rc = DosExitList(EXLST_REMOVE, (PFNEXITLIST) _cleanupSemaphore);

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
    APIRET	rc;
    int 		i=0;

    rc = DosGetNamedSharedMem( (PPVOID)&pMUROP, (PSZ)SHMEM_UROP, PAG_READ | PAG_WRITE);
    if (rc) {
        rc = uropInit();
        if (rc) {
            LOG((stderr, "uropDump uropInit failed rc=%d\n", rc));
            return -1;
        }
    }

    // get semaphore
    rc = DosRequestMutexSem( hmtxUROP,(ULONG) SEM_INDEFINITE_WAIT);
    if (rc == ERROR_INVALID_HANDLE) {
        // open the mutex for this process
        rc = DosOpenMutexSem( SEM_UROP, &hmtxUROP);
	rc = DosRequestMutexSem( hmtxUROP,(ULONG) SEM_INDEFINITE_WAIT);
    }
    if (rc) {
	LOG((stderr, "uropAdd DosRequestMutexSem failed rc=%d\n", rc));
	return -1;
    }

    // scan array
    i = 0;
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

    // release semaphore
    rc = DosReleaseMutexSem( hmtxUROP);
    if (rc) {
	LOG((stderr, "(%d) uropDump DosReleaseMutexSem rc=%d\n", getpid(), rc));
    }

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
    APIRET	rc;
    int		i=0;

    rc = DosGetNamedSharedMem( (PPVOID)&pMUROP, (PSZ)SHMEM_UROP, PAG_READ | PAG_WRITE);
    if (rc) {
        rc = uropInit();
        if (rc) {
            LOG((stderr, "uropReset uropInit failed rc=%d\n", rc));
            return -1;
        }
    }

    // get semaphore
    rc = DosRequestMutexSem( hmtxUROP,(ULONG) SEM_INDEFINITE_WAIT);
    if (rc == ERROR_INVALID_HANDLE) {
        // open the mutex for this process
        rc = DosOpenMutexSem( SEM_UROP, &hmtxUROP);
	rc = DosRequestMutexSem( hmtxUROP,(ULONG) SEM_INDEFINITE_WAIT);
    }
    if (rc) {
	LOG((stderr, "uropAdd DosRequestMutexSem failed rc=%d\n", rc));
	return -1;
    }

    // scan array
    for( i = 0; i<MAX_UROP; i++) {
        pMUROP->urop[i].szPathOld[0] = 0;
        pMUROP->urop[i].szPathNew[0] = 0;
    }
    pMUROP->cUROP = 0;
    printf( "uropReset count (%d)\n", pMUROP->cUROP);

    // release semaphore
    rc = DosReleaseMutexSem( hmtxUROP);
    if (rc) {
	LOG((stderr, "(%d) uropReset DosReleaseMutexSem rc=%d\n", getpid(), rc));
    }

    // done
    return 0;
}

