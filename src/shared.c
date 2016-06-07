/*
 * System-wide shared data manipulation.
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

#include <emx/startup.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include <assert.h>

#include "shared.h"

#define MUTEX_LIBCX "\\SEM32\\LIBCX_V1_MUTEX"
#define SHAREDMEM_LIBCX "\\SHAREMEM\\LIBCX_V1_DATA"

#define HEAP_SIZE 65536 * 2 /* initial size for fcntl data */

struct SharedData *gpData = NULL;

static HMTX gMutex = NULLHANDLE;

static void APIENTRY ProcessExit(ULONG);

/**
 * Initializes the shared structures.
 * @param bKeepLock TRUE to leave the mutex locked on success.
 * @return NO_ERROR on success, DOS error code otherwise.
 */
static int shared_init(int bKeepLock)
{
  APIRET arc;
  int rc;

  arc = DosExitList(EXLST_ADD, ProcessExit);
  assert(arc == NO_ERROR);

  while (1)
  {
    /* First, try to open the mutex */
    arc = DosOpenMutexSem(MUTEX_LIBCX, &gMutex);
    TRACE("DosOpenMutexSem = %d\n", arc);

    if (arc == NO_ERROR)
    {
      /*
       * Init is (being) done by another process, request the mutex to
       * guarantee shared memory is already alloated, then get access to
       * it and open shared heap located in that memory.
       */
      arc = DosRequestMutexSem(gMutex, SEM_INDEFINITE_WAIT);
      TRACE("DosRequestMutexSem = %d\n", arc);
      assert(arc == NO_ERROR);

      if (arc == NO_ERROR)
      {
        arc = DosGetNamedSharedMem((PPVOID)&gpData, SHAREDMEM_LIBCX, PAG_READ | PAG_WRITE);
        assert(arc == NO_ERROR);

        TRACE("gpData->heap = %p\n", gpData->heap);
        assert(gpData->heap);

        rc = _uopen(gpData->heap);
        TRACE("_uopen = %d (%d)\n", rc, errno);
        assert(rc == 0);

        assert(gpData->refcnt);
        gpData->refcnt++;
      }

      break;
    }

    if (arc == ERROR_SEM_NOT_FOUND)
    {
      /* We are the first process, create the mutex */
      arc = DosCreateMutexSem(MUTEX_LIBCX, &gMutex, 0, TRUE);
      TRACE("DosCreateMutexSem = %d\n", arc);

      if (arc == ERROR_DUPLICATE_NAME)
      {
        /* Another process is faster, attempt to open the mutex again */
        continue;
      }
    }

    if (arc != NO_ERROR)
      return arc;

    /*
     * We are the process that successfully created the main mutex.
     * Proceed with the initial setup by allocating shared memory and
     * heap.
     */

#if TRACE_ENABLED
    /*
     * Allocate a larger buffer to fit lengthy TRACE messages and disable
     * auto-flush on EOL (to avoid breaking them by stdout operations
     * from other threads/processes).
     */
    setvbuf(stdout, NULL, _IOFBF, 0x10000);
#endif

    /* Allocate shared memory */
    arc = DosAllocSharedMem((PPVOID)&gpData, SHAREDMEM_LIBCX, HEAP_SIZE,
                            PAG_COMMIT | PAG_READ | PAG_WRITE | OBJ_ANY);
    TRACE("DosAllocSharedMem(OBJ_ANY) = %d\n", arc);

    if (arc && arc != ERROR_ALREADY_EXISTS)
    {
      /* High memory may be unavailable, try w/o OBJ_ANY */
      arc = DosAllocSharedMem((PPVOID)&gpData, SHAREDMEM_LIBCX, HEAP_SIZE,
                              PAG_COMMIT | PAG_READ | PAG_WRITE);
      TRACE("DosAllocSharedMem = %d\n", arc);
    }

    if (arc == NO_ERROR)
    {
      /* Create shared heap */
      gpData->heap = _ucreate(gpData + 1, HEAP_SIZE - sizeof(struct SharedData),
                              _BLOCK_CLEAN, _HEAP_REGULAR | _HEAP_SHARED,
                              NULL, NULL);
      TRACE("gpData->heap = %p\n", gpData->heap);
      assert(gpData->heap);

      rc =_uopen(gpData->heap);
      assert(rc == 0);

      gpData->refcnt = 1;

      /* Initialize common structures */
      gpData->files = _ucalloc(gpData->heap, FILE_DESC_HASH_SIZE, sizeof(*gpData->files));
      TRACE("gpData->files = %p\n", gpData->files);
      assert(gpData->files);
    }

    break;
  }

  if (arc == NO_ERROR)
  {
    /* Initialize individual components */
    arc = fcntl_locking_init();
  }

  if (!bKeepLock || arc != NO_ERROR)
    DosReleaseMutexSem(gMutex);

  return arc;
}

static void shared_term()
{
  APIRET arc;
  int rc;

  TRACE("gMutex %p, gpData %p (heap %p, refcnt %d)\n",
        gMutex, gpData, gpData ? gpData->heap : 0,
        gpData ? gpData->refcnt : 0);

  assert(gMutex != NULLHANDLE);

  DosRequestMutexSem(gMutex, SEM_INDEFINITE_WAIT);

  if (gpData)
  {
    if (gpData->heap)
    {
      int i;

      assert(gpData->refcnt);
      gpData->refcnt--;

      /* Uninitialize individual components */
      fcntl_locking_term();

      if (gpData->refcnt == 0)
      {
        /* We are the last process, free common structures */
        TRACE("gpData->files %p\n", gpData->files);
        if (gpData->files)
        {
          for (i = 0; i < FILE_DESC_HASH_SIZE; ++i)
          {
            struct FileDesc *desc = gpData->files[i];
            while (desc)
            {
              struct FileDesc *next = desc->next;
              pwrite_filedesc_term(desc);
              fcntl_locking_filedesc_term(desc);
              free(desc);
              desc = next;
            }
          }
          free(gpData->files);
        }
      }

      _uclose(gpData->heap);

      if (gpData->refcnt == 0)
      {
        rc = _udestroy(gpData->heap, !_FORCE);
        TRACE("_udestroy = %d (%d)\n", rc, errno);
      }
    }

    arc = DosFreeMem(gpData);
    TRACE("DosFreeMem = %d\n", arc);
  }

  DosReleaseMutexSem(gMutex);

  arc = DosCloseMutexSem(gMutex);
  if (arc == ERROR_SEM_BUSY)
  {
    /* The semaphore may be owned by us, try to release it */
    arc = DosReleaseMutexSem(gMutex);
    TRACE("DosReleaseMutexSem = %d\n", arc);
    arc = DosCloseMutexSem(gMutex);
  }
  TRACE("DosCloseMutexSem = %d\n", arc);

  DosExitList(EXLST_REMOVE, ProcessExit);
}

/**
 * Initialize/terminate DLL at load/unload.
 */
unsigned long _System _DLL_InitTerm(unsigned long hModule, unsigned long ulFlag)
{
  TRACE("ulFlag %d\n", ulFlag);

  switch (ulFlag)
  {
    /* InitInstance */
    case 0:
    {
      if (_CRT_init() != 0)
        return 0;
      __ctordtorInit();
      if (shared_init(FALSE) != NO_ERROR)
        return 0;
      break;
    }

    /* TermInstance */
    case 1:
    {
      __ctordtorTerm();
      _CRT_term();
      break;
    }

    default:
      return 0;
  }

  /* Success */
  return 1;
}

/**
 * Called upon any process termination (even after a crash where _DLL_InitTerm
 * is not called).
 */
static void APIENTRY ProcessExit(ULONG reason)
{
  TRACE("reason %d\n", reason);

  shared_term();

  DosExitList(EXLST_EXIT, NULL);
}

/**
 * Requests the global mutex that protects general access to gpData.
 * Must be always called before accessing gpData members.
 */
void global_lock()
{
  APIRET arc;

  assert(gMutex != NULLHANDLE);
  assert(gpData);

  arc = DosRequestMutexSem(gMutex, SEM_INDEFINITE_WAIT);
  if (arc == ERROR_INVALID_HANDLE)
  {
    /*
     * It must be a forked child, initialize ourselves (since
     * _DLL_InitTerm(,0) is not called for forks). This call
     * will leave the mutex locked.
     */
    arc = shared_init(TRUE);
  }

  TRACE_IF(arc, "DosRequestMutexSem = %d\n", arc);
  assert(arc == NO_ERROR);
}

/**
 * Releases the global mutex requested by mutex_lock().
 */
void global_unlock()
{
  APIRET arc;

  assert(gMutex != NULLHANDLE);

  arc = DosReleaseMutexSem(gMutex);

  assert(arc == NO_ERROR);
}

static size_t hash_string(const char *str)
{
  /*
   * Based on RS hash function from Arash Partow collection
   * (http://www.partow.net/programming/hashfunctions/).
   * According to https://habrahabr.ru/post/219139/ this function
   * produces few collisions and is rather fast.
   */

  size_t a = 63689;
  size_t hash = 0;

  while (*str)
  {
    hash = hash * a + (unsigned char)(*str++);
    a *= 378551 /* b */;
  }

  return hash;
}

/**
 * Returns a file descriptor sturcture for the given path.
 * Must be called under global_lock().
 * Returns NULL when @a bNew is true and there is not enough memory
 * to allocate a new sctructure, or when @a bNew is FALSE and there
 * is no descriptor for the given file.
 */
struct FileDesc *get_file_desc(const char *path, int bNew)
{
  size_t h;
  struct FileDesc *desc;
  int rc;

  assert(gpData);
  assert(path);
  assert(strlen(path) < PATH_MAX);

  h = hash_string(path) % FILE_DESC_HASH_SIZE;
  desc = gpData->files[h];

  while (desc)
  {
    if (strcmp(desc->path, path) == 0)
      break;
    desc = desc->next;
  }

  if (!desc && bNew)
  {
    desc = _ucalloc(gpData->heap, 1, sizeof(*desc) + strlen(path) + 1);
    if (desc)
    {
      /* Initialize the new desc */
      strcpy(desc->path, path);
      /* Call component-specific initialization */
      rc = fcntl_locking_filedesc_init(desc);
      if (rc == 0)
      {
        rc = pwrite_filedesc_init(desc);
        if (rc == -1)
          fcntl_locking_filedesc_term(desc);
      }
      if (rc == -1)
      {
        free(desc);
        return NULL;
      }
      /* Put to the head of the bucket */
      desc->next = gpData->files[h];
      gpData->files[h] = desc;
    }
  }

  return desc;
}
