/*
 * Atomic pread/pwrite replacement for kLIBC.
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

#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>
#include <emx/io.h>

#define TRACE_GROUP TRACE_GROUP_PWRITE
#include "../shared.h"

/**
 * Initializes the pwrite portion of FileDesc.
 * If proc is not NULL, desc a per-process entry, otherwise a global one.
 * Called right after the FileDesc pointer is allocated.
 * Returns 0 on success or -1 on failure.
 */
int pwrite_filedesc_init(ProcDesc *proc, FileDesc *desc)
{
  if (!proc)
  {
    /* Mutex is lazily created in pread_pwrite */
    assert(!desc->g->pwrite_lock);
  }
  return 0;
}

/**
 * Uninitializes the pwrite portion of FileDesc.
 * If proc is not NULL, desc a per-process entry, otherwise a global one.
 * Called right before the FileDesc pointer is freed.
 */
void pwrite_filedesc_term(ProcDesc *proc, FileDesc *desc)
{
  if (!proc)
  {
    APIRET arc;

    arc = DosCloseMutexSem(desc->g->pwrite_lock);
    if (arc == ERROR_SEM_BUSY)
    {
      /* The semaphore may be owned by us, try to release it */
      arc = DosReleaseMutexSem(desc->g->pwrite_lock);
      TRACE("DosReleaseMutexSem = %ld\n", arc);
      arc = DosCloseMutexSem(desc->g->pwrite_lock);
    }
    TRACE("DosCloseMutexSem = %ld\n", arc);
  }
}

static ssize_t pread_pwrite(int bWrite, int fildes, void *buf,
                            size_t nbyte, off_t offset)
{
  TRACE("bWrite %d, fildes %d, buf %p, nbyte %u, offset %lld\n",
        bWrite, fildes, buf, nbyte, offset);

  int rc;
  APIRET arc;
  HMTX mutex;
  __LIBC_PFH pFH;

  pFH = __libc_FH(fildes);
  if (!pFH || !pFH->pszNativePath)
  {
    errno = !pFH ? EBADF : EINVAL;
    return -1;
  }

  TRACE("pszNativePath %s, fFlags %x\n", pFH->pszNativePath, pFH->fFlags);

  global_lock();

  {
    FileDesc *desc = get_file_desc(pFH->pszNativePath);
    if (desc)
      mutex = desc->g->pwrite_lock;

    global_unlock();

    if (!desc)
    {
      errno = ENOMEM;
      return -1;
    }
  }

  TRACE("mutex %x\n", mutex);

  arc = DosRequestMutexSem(mutex, SEM_INDEFINITE_WAIT);
  if (arc == ERROR_INVALID_HANDLE)
  {
    /* Try to open the mutex for this process */
    arc = DosOpenMutexSem(NULL, &mutex);
    TRACE("DosOpenMutexSem = %d\n", arc);
    if (arc == ERROR_INVALID_HANDLE)
    {
      /* The mutex is no longer valid, create a new one */
      /* @todo this is a temporary hack, see https://github.com/bitwiseworks/libcx/issues/7 */
      global_lock();
      FileDesc *desc = get_file_desc(pFH->pszNativePath);
      if (desc)
      {
        arc = DosCreateMutexSem(NULL, &desc->g->pwrite_lock, DC_SEM_SHARED, FALSE);
        TRACE("DosCreateMutexSem = %d\n", arc);
        if (arc == NO_ERROR)
          mutex = desc->g->pwrite_lock;
      }
      global_unlock();
      if (!desc || arc != NO_ERROR)
      {
        errno = ENOMEM;
        return -1;
      }
    }
    arc = DosRequestMutexSem(mutex, SEM_INDEFINITE_WAIT);
  }
  TRACE_IF(arc, "DosRequestMutexSem = %d\n", arc);
  assert(arc == NO_ERROR);

  TRACE("Will call %s\n", bWrite ? "_std_pwrite" : "_std_pread");

  if (bWrite)
  {
    rc = _std_pwrite(fildes, buf, nbyte, offset);
  }
  else
  {
    /* Fix DosRead bug, see touch_pages docs. */
    touch_pages(buf, nbyte);
    rc = _std_pread(fildes, buf, nbyte, offset);
  }

  TRACE("rc = %d (%s)\n", rc, strerror(rc == -1 ? errno : 0));

  DosReleaseMutexSem(mutex);

  return rc;
}

/**
 * LIBC pread replacement.
 * Makes pread thread safe with a per-file guarding mutex.
 */
ssize_t pread(int fildes, void *buf, size_t nbyte, off_t offset)
{
  return pread_pwrite(FALSE, fildes, buf, nbyte, offset);
}

/**
 * LIBC pwrite replacement.
 * Makes pwrite thread safe with a per-file guarding mutex.
 */
ssize_t pwrite(int fildes, const void *buf, size_t nbyte, off_t offset)
{
  return pread_pwrite(TRUE, fildes, (void *)buf, nbyte, offset);
}

/**
 * LIBC read replacement.
 * Override to fix DosRead bug, see touch_pages docs.
 */
ssize_t read(int fd, void *buf, size_t nbyte)
{
  TRACE("fd %d, buf %p, nbyte %u\n", fd, buf, nbyte);

  touch_pages(buf, nbyte);
  return _std_read(fd, buf, nbyte);
}

/**
 * LIBC __read replacement.
 * Override to fix DosRead bug, see touch_pages docs.
 */
int __read(int handle, void *buf, size_t nbyte)
{
  TRACE("handle %d, buf %p, nbyte %u\n", handle, buf, nbyte);

  touch_pages(buf, nbyte);
  return _libc__read(handle, buf, nbyte);
}

/**
 * LIBC _stream_read replacement.
 * Override to fix DosRead bug, see touch_pages docs.
 */
int _stream_read(int fd, void *buf, size_t nbyte)
{
  TRACE("fd %d, buf %p, nbyte %u\n", fd, buf, nbyte);

  touch_pages(buf, nbyte);
  return _libc_stream_read(fd, buf, nbyte);
}

/**
 * DOSCALLS DosRead replacement.
 * Override to fix DosRead bug, see touch_pages docs.
 */
ULONG APIENTRY DosRead(HFILE hFile, PVOID pBuffer, ULONG ulLength,
                       PULONG pulBytesRead)
{
  TRACE("hFile %d, pBuffer %p, ulLength %lu, pulBytesRead %p\n",
        hFile, pBuffer, ulLength, pulBytesRead);

  touch_pages(pBuffer, ulLength);
  return _doscalls_DosRead(hFile, pBuffer, ulLength, pulBytesRead);
}
