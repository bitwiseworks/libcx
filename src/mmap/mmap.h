/*
 * mmap implementation for kLIBC.
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

#ifndef MMAP_H
#define MMAP_H

#include <os2.h>

#include "../shared.h"

/* Width of a dirty map entry in bits */
#define DIRTYMAP_WIDTH (sizeof(*((struct FileDesc*)0)->fh->dirtymap) * 8)

/* Flush operation start delay (ms) */
#define FLUSH_DELAY 1000

/**
 * Per-process data for memory mappings.
 */
typedef struct ProcMemMap
{
  int flush_tid; /* Flush thread */
  HEV flush_sem; /* Semaphore for flush thread */
  int flush_request; /* 1 - semaphore is posted */
} ProcMemMap;

/**
 * File map's memory object (linked list entry).
 */
typedef struct FileMapMem
{
  struct FileMapMem *next;

  struct FileMap *map; /* parent file map */
  ULONG start; /* start address */
  off_t off; /* offset from the beginning of the file */
  ULONG len; /* object length */
  int refcnt; /* number of MemMap entries using it */
} FileMapMem;

/**
 * File map data.
 */
typedef struct FileMap
{
//  FileDesc *desc; /* associated file desc */
  int flags; /* Currently, MAP_SHARED or MAP_PRIVATE or 0 if not associated */
  union
  {
    SharedFileDesc *desc_g; /* associated file desc for MAP_SHARED */
    FileDesc *desc; /* associated file desc for MAP_PRIVATE */
  };
  FileMapMem *mems; /* list of memory objects of this file */
  off_t size; /* file size */
} FileMap;

/**
 * Process specific file handle.
 */
typedef struct FileHandle
{
  FileDesc *desc; /* associated file desc */
  HFILE fd; /* file handle (descriptor in LIBC) or -1 for MAP_ANONYMOUS */
  size_t dirtymap_sz; /* size of dirty array in bytes */
  uint32_t *dirtymap; /* bit array of dirty pages */
  int refcnt; /* number of MemMap entries using it */
} FileHandle;

/**
 * Memory mapping (linked list entry).
 */
typedef struct MemMap
{
  struct MemMap *next;

  ULONG start; /* start address */
  ULONG end; /* end address (exclusive) */
  int flags; /* mmap flags */
  ULONG dos_flags; /* DosAllocMem protection flags */
  struct File /* Only present for non MAP_ANONYMOUS mmaps (must be last) */
  {
    FileMapMem *fmem; /* file map's memory */
    FileHandle *fh; /* File handle */
    int refcnt; /* number of times this MemMap was returned by mmap */
  } f[0];
} MemMap;

#ifdef DEBUG
void set_mmap_full_size(int val);
MemMap *get_proc_mmaps(int pid);
#endif

#endif // MMAP_H
