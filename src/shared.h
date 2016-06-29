/*
 * System-wide shared data structures.
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

#include <stdio.h>
#include <umalloc.h>

#if TRACE_ENABLED
#ifndef TRACE_MORE
#define TRACE_MORE 1
#endif
#define TRACE_RAW(msg, ...) printf("*** [%d:%d] %s:%d:%s: " msg, getpid(), _gettid(), __FILE__, __LINE__, __FUNCTION__, ## __VA_ARGS__)
#define TRACE(msg, ...) do { TRACE_RAW(msg, ## __VA_ARGS__); fflush(stdout); } while(0)
#define TRACE_BEGIN(msg, ...) { do { TRACE_RAW(msg, ## __VA_ARGS__); } while(0)
#define TRACE_CONT(msg, ...) printf(msg, ## __VA_ARGS__)
#define TRACE_END() fflush(stdout); } do {} while(0)
#define TRACE_IF(cond, msg, ...) if (cond) TRACE(msg, ## __VA_ARGS__)
#define TRACE_BEGIN_IF(cond, msg, ...) if (cond) TRACE_BEGIN(msg, ## __VA_ARGS__)
#else
#define TRACE_MORE 0
#define TRACE_RAW(msg, ...) do {} while (0)
#define TRACE(msg, ...) do {} while (0)
#define TRACE_BEGIN(msg, ...) if (0) { do {} while(0)
#define TRACE_CONT(msg, ...) do {} while (0)
#define TRACE_END() } do {} while(0)
#define TRACE_IF(cond, msg, ...) do {} while (0)
#define TRACE_BEGIN_IF(cond, msg, ...) if (0) { do {} while(0)
#endif

#if STATS_ENABLED
void *_ucalloc_stats(Heap_t h, size_t elements, size_t size);
#define _ucalloc _ucalloc_stats
#endif

#define FILE_DESC_HASH_SIZE 127 /* Prime */

/**
 * File descriptor (hash map entry).
 */
struct FileDesc
{
  struct FileDesc *next;

  struct FcntlLock *fcntl_locks; /* Active fcntl file locks */
  unsigned long pwrite_lock; /* Mutex used in pwrite/pread */
  char path[0]; /* File name with fill path (must be last!) */
};

/**
 * Global shared data structure.
 */
struct SharedData
{
  Heap_t heap; /* shared heap */
  int refcnt; /* number of processes using us */
#if STATS_ENABLED
  size_t maxHeapUsed; /* max size of used heap space */
#endif
  struct FileDesc **files; /* File descriptor hash map of FILE_DESC_HASH_SIZE */
  struct FcntlLocking *fcntl_locking; /* Shared data for fcntl locking */
};

/**
 * Pointer to the global shared data structure.
 * Lazily initialized upon the first call of global_lock().
 */
extern struct SharedData *gpData;

void global_lock();
void global_unlock();

struct FileDesc *get_file_desc(const char *path, int bNew);

int fcntl_locking_init();
void fcntl_locking_term();
int fcntl_locking_filedesc_init(struct FileDesc *desc);
void fcntl_locking_filedesc_term(struct FileDesc *desc);

int pwrite_filedesc_init(struct FileDesc *desc);
void pwrite_filedesc_term(struct FileDesc *desc);
