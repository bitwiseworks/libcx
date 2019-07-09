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
#include <emx/io.h>

#define TRACE_GROUP TRACE_GROUP_PWRITE
#include "../shared.h"

/**
 * Initializes the pwrite portion of SharedFileDesc and FileDesc.
 * Called right after the SharedFileDesc and/or FileDesc pointers are allocated.
 * Returns 0 on success or -1 on failure.
 */
int pwrite_filedesc_init(FileDesc *desc)
{
  if (desc->g->refcnt == 1)
  {
    /* Mutex is lazily created in pread_pwrite */
    ASSERT_MSG(!desc->g->pwrite_lock, "%lx", desc->g->pwrite_lock);
  }
  return 0;
}

/**
 * Uninitializes the pwrite portion of SharedFileDesc and FileDesc.
 * Called right before the SharedFileDesc and/or FileDesc pointers are freed.
 */
void pwrite_filedesc_term(FileDesc *desc)
{
  if (desc->g->refcnt == 1)
  {
    APIRET arc;

    if (desc->g->pwrite_lock != NULLHANDLE)
    {
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
}

ssize_t _std_pread(int fildes, void *buf, size_t nbyte, off_t offset);
ssize_t _std_pwrite(int fildes, const void *buf, size_t nbyte, off_t offset);

static ssize_t pread_pwrite(int bWrite, int fildes, void *buf,
                            size_t nbyte, off_t offset)
{
  TRACE("bWrite %d, fildes %d, buf %p, nbyte %u, offset %lld\n",
        bWrite, fildes, buf, nbyte, offset);

  int rc = 0;
  APIRET arc = NO_ERROR;
  HMTX mutex = NULLHANDLE;
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
    FileDesc *desc = get_file_desc(fildes, pFH->pszNativePath);
    if (desc)
    {
      if (desc->g->pwrite_lock == NULLHANDLE)
      {
        /* Lazily create a mutex for pread/pwrite serialization */
        arc = DosCreateMutexSem(NULL, &desc->g->pwrite_lock, DC_SEM_SHARED, FALSE);
        TRACE("DosCreateMutexSem = %lu\n", arc);
      }
      mutex = desc->g->pwrite_lock;
    }

    global_unlock();

    if (!desc || arc != NO_ERROR)
    {
      errno = !desc ? ENOMEM : ENFILE;
      return -1;
    }
  }

  ASSERT(mutex);

  DOS_NI(arc = DosRequestMutexSem(mutex, SEM_INDEFINITE_WAIT));
  if (arc == ERROR_INVALID_HANDLE)
  {
    /* Try to open the mutex for this process */
    arc = DosOpenMutexSem(NULL, &mutex);
    TRACE("DosOpenMutexSem = %lu\n", arc);
    if (arc == NO_ERROR)
      DOS_NI(arc = DosRequestMutexSem(mutex, SEM_INDEFINITE_WAIT));
  }

  ASSERT_MSG(arc == NO_ERROR, "%ld", arc);

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

/* Our DosRead bug fix spams a lot, move it to a different group */
#undef TRACE_GROUP
#define TRACE_GROUP TRACE_GROUP_DOSREADBUGFIX

/*
 * Chunk size to prevent JFS from screwing up (found out experimentally, 128 MB
 * and more cause the same hangs on subsequental runs as with no chunks, so be
 * on the safe side).
 */
#define DOS_READ_MAX_CHUNK (32U * 1024U * 1024U) /* 32 MB */

ssize_t _std_read(int fd, void *buf, size_t nbyte);

/**
 * LIBC read replacement.
 * Override to fix DosRead bug, see touch_pages docs.
 * Also override to fix another DosRead bug, see #36.
 */
ssize_t read(int fd, void *buf, size_t nbyte)
{
  TRACE("fd %d, buf %p, nbyte %u\n", fd, buf, nbyte);

  touch_pages(buf, nbyte);

  if (nbyte < DOS_READ_MAX_CHUNK)
    return _std_read(fd, buf, nbyte);

  int rc, final_rc = 0;
  size_t chunk = DOS_READ_MAX_CHUNK;
  do
  {
    rc = _std_read(fd, buf, chunk);
    if (rc >= 0)
      final_rc += rc;
    else
      final_rc = rc;
    if (rc < 0 || rc < chunk || nbyte == chunk)
      break;
    nbyte -= chunk;
    buf += chunk;
    if (nbyte < chunk)
      chunk = nbyte;
  }
  while (1);

  return final_rc;
}

int _libc__read(int handle, void *buf, size_t nbyte);

/**
 * LIBC __read replacement.
 * Override to fix DosRead bug, see touch_pages docs.
 * Also override to fix another DosRead bug, see #36.
 */
int __read(int handle, void *buf, size_t nbyte)
{
  TRACE("handle %d, buf %p, nbyte %u\n", handle, buf, nbyte);

  touch_pages(buf, nbyte);

  if (nbyte < DOS_READ_MAX_CHUNK)
    return _libc__read(handle, buf, nbyte);

  int rc, final_rc = 0;
  size_t chunk = DOS_READ_MAX_CHUNK;
  do
  {
    rc = _libc__read(handle, buf, chunk);
    if (rc >= 0)
      final_rc += rc;
    else
      final_rc = rc;
    if (rc < 0 || rc < chunk || nbyte == chunk)
      break;
    nbyte -= chunk;
    buf += chunk;
    if (nbyte < chunk)
      chunk = nbyte;
  }
  while (1);

  return final_rc;
}

int _libc_stream_read(int fd, void *buf, size_t nbyte);

/**
 * LIBC _stream_read replacement.
 * Override to fix DosRead bug, see touch_pages docs.
 * Also override to fix another DosRead bug, see #36.
 */
int _stream_read(int fd, void *buf, size_t nbyte)
{
  TRACE("fd %d, buf %p, nbyte %u\n", fd, buf, nbyte);

  touch_pages(buf, nbyte);

  if (nbyte < DOS_READ_MAX_CHUNK)
    return _libc_stream_read(fd, buf, nbyte);

  int rc, final_rc = 0;
  size_t chunk = DOS_READ_MAX_CHUNK;
  do
  {
    rc = _libc_stream_read(fd, buf, chunk);
    if (rc >= 0)
      final_rc += rc;
    else
      final_rc = rc;
    if (rc < 0 || rc < chunk || nbyte == chunk)
      break;
    nbyte -= chunk;
    buf += chunk;
    if (nbyte < chunk)
      chunk = nbyte;
  }
  while (1);

  return final_rc;
}

size_t _std_fread(void *buf, size_t size, size_t count, FILE *stream);

/**
 * LIBC fread replacement.
 * Override to fix DosRead bug, see touch_pages docs.
 * Also override to fix another DosRead bug, see #36.
 */
size_t fread(void *buf, size_t size, size_t count, FILE *stream)
{
  TRACE("stream %p, buf %p, size %u, count %u\n", stream, buf, size, count);

  size_t nbyte = size * count;

  touch_pages(buf, nbyte);

  if (nbyte < DOS_READ_MAX_CHUNK)
    return _std_fread(buf, size, count, stream);

  int rc, final_rc = 0;
  size_t chunk = DOS_READ_MAX_CHUNK;
  do
  {
    rc = _std_fread(buf, 1, chunk, stream);
    if (rc >= 0)
      final_rc += rc;
    else
      final_rc = rc;
    if (rc < 0 || rc < chunk || nbyte == chunk)
      break;
    nbyte -= chunk;
    buf += chunk;
    if (nbyte < chunk)
      chunk = nbyte;
  }
  while (1);

  return final_rc;
}

/**
 * DOSCALLS DosRead replacement.
 * Override to fix DosRead bug, see touch_pages docs.
 * Also override to fix another DosRead bug, see #36.
 */
ULONG APIENTRY DosRead(HFILE hFile, PVOID pBuffer, ULONG ulLength,
                       PULONG pulBytesRead)
{
  TRACE("hFile %lu, pBuffer %p, ulLength %lu, pulBytesRead %p\n",
        hFile, pBuffer, ulLength, pulBytesRead);

  touch_pages(pBuffer, ulLength);

  if (ulLength < DOS_READ_MAX_CHUNK)
    return _doscalls_DosRead(hFile, pBuffer, ulLength, pulBytesRead);

  APIRET arc;
  ULONG ulChunk = DOS_READ_MAX_CHUNK, ulRead = 0;
  *pulBytesRead = 0;
  do
  {
    arc = _doscalls_DosRead(hFile, pBuffer, ulChunk, &ulRead);
    *pulBytesRead += ulRead;
    if (arc || ulRead < ulChunk || ulLength == ulChunk)
      break;
    ulLength -= ulChunk;
    pBuffer += ulChunk;
    if (ulLength < ulChunk)
      ulChunk = ulLength;
  }
  while (1);

  return arc;
}
