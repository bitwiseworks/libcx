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
#include <process.h>
#include <umalloc.h>

#ifdef TRACE_ENABLED

#ifndef TRACE_USE_LIBC_LOG
#define TRACE_USE_LIBC_LOG 1
#endif

#ifdef TRACE_USE_LIBC_LOG
#ifndef TRACE_GROUP
#define TRACE_GROUP 0
#endif
#define __LIBC_LOG_GROUP TRACE_GROUP
#include <InnoTekLIBC/logstrict.h>
/* The below defs must be in sync with the logGroup array */
#define TRACE_GROUP_FCNTL 1
#define TRACE_GROUP_PWRITE 2
#define TRACE_GROUP_SELECT 3
#define TRACE_GROUP_MMAP 4
#endif

#ifndef TRACE_MORE
#define TRACE_MORE 1
#endif

#ifdef TRACE_USE_LIBC_LOG
void trace(unsigned traceGroup, const char *file, int line, const char *func, const char *format, ...) __printflike(5, 6);
#define TRACE_FLUSH() do {} while (0)
#define TRACE_RAW(msg, ...) trace(TRACE_GROUP, __FILE__, __LINE__, __FUNCTION__, msg, ## __VA_ARGS__)
#define TRACE_CONT(msg, ...) trace(TRACE_GROUP, NULL, -1, NULL, msg, ## __VA_ARGS__)
#else
#define TRACE_FLUSH() fflush(stdout)
#define TRACE_RAW(msg, ...) printf("*** [%d:%d] %s:%d:%s: " msg, getpid(), _gettid(), __FILE__, __LINE__, __FUNCTION__, ## __VA_ARGS__)
#define TRACE_CONT(msg, ...) printf(msg, ## __VA_ARGS__)
#endif
#define TRACE(msg, ...) do { TRACE_RAW(msg, ## __VA_ARGS__); TRACE_FLUSH(); } while(0)
#define TRACE_BEGIN(msg, ...) { do { TRACE_RAW(msg, ## __VA_ARGS__); } while(0)
#define TRACE_END() TRACE_FLUSH(); } do {} while(0)
#define TRACE_IF(cond, msg, ...) if (cond) TRACE(msg, ## __VA_ARGS__)
#define TRACE_BEGIN_IF(cond, msg, ...) if (cond) TRACE_BEGIN(msg, ## __VA_ARGS__)
#else
#define TRACE_MORE 0
#define TRACE_FLUSH() do {} while (0)
#define TRACE_RAW(msg, ...) do {} while (0)
#define TRACE(msg, ...) do {} while (0)
#define TRACE_BEGIN(msg, ...) if (0) { do {} while(0)
#define TRACE_CONT(msg, ...) do {} while (0)
#define TRACE_END() } do {} while(0)
#define TRACE_IF(cond, msg, ...) do {} while (0)
#define TRACE_BEGIN_IF(cond, msg, ...) if (0) { do {} while(0)

#endif /* TRACE_ENABLED */

#define FILE_DESC_HASH_SIZE 127 /* Prime */

#define PROC_DESC_HASH_SIZE 17 /* Prime */

/**
 * Process descriptor (hash map entry).
 */
struct ProcDesc
{
  struct ProcDesc *next;

  pid_t pid;
  struct ProcMemMap *mmap; /* process-specific data for mmap */
  struct MemMap *mmaps; /* private mmap mappings */
};

/**
 * File descriptor (hash map entry).
 */
struct FileDesc
{
  struct FileDesc *next;

  struct FcntlLock *fcntl_locks; /* Active fcntl file locks */
  unsigned long pwrite_lock; /* Mutex used in pwrite/pread */
  char path[0]; /* File name with full path (must be last!) */
};

/**
 * Global shared data structure (header).
 */
struct SharedData
{
  size_t size; /* committed size */
  Heap_t heap; /* shared heap */
  int refcnt; /* number of processes using us */
  size_t maxHeapUsed; /* max size of used heap space */
  struct ProcDesc **procs; /* Process descriptor hash map of PROC_INFO_HASH_SIZE */
  struct FileDesc **files; /* File descriptor hash map of FILE_DESC_HASH_SIZE */
  struct GlobalMemMap *mmap; /* Global data for mmap */
  struct MemMap *mmaps; /* shared mmap mappings */
  struct FcntlLocking *fcntl_locking; /* Shared data for fcntl locking */
  /* heap memory follows here */
};

/**
 * Pointer to the global shared data structure.
 * Lazily initialized upon the first call of global_lock().
 */
extern struct SharedData *gpData;

struct _EXCEPTIONREPORTRECORD;
struct _EXCEPTIONREGISTRATIONRECORD;
struct _CONTEXT;

void global_lock();
void global_unlock();

void *global_alloc(size_t size);

#define GLOBAL_NEW(ptr) (ptr) = (__typeof(ptr))global_alloc(sizeof(*ptr))
#define GLOBAL_NEW_PLUS(ptr, more) (ptr) = (__typeof(ptr))global_alloc(sizeof(*ptr) + (more))
#define GLOBAL_NEW_ARRAY(ptr, sz) (ptr) = (__typeof(ptr))global_alloc(sizeof(*ptr) * (sz))
#define RENEW_ARRAY(ptr, sz) (ptr) = (__typeof(ptr))realloc(ptr, sizeof(*ptr) * (sz))

enum HashMapOpt
{
  HashMapOpt_None = 0,
  HashMapOpt_New = 1,
  HashMapOpt_Take = 2,
};

struct ProcDesc *get_proc_desc_ex(pid_t pid, enum HashMapOpt opt);
static inline struct ProcDesc *get_proc_desc(pid_t pid) { return get_proc_desc_ex(pid, HashMapOpt_New); }
static inline struct ProcDesc *find_proc_desc(pid_t pid) { return get_proc_desc_ex(pid, HashMapOpt_None); }
static inline struct ProcDesc *take_proc_desc(pid_t pid) { return get_proc_desc_ex(pid, HashMapOpt_Take); }

struct FileDesc *get_file_desc(const char *path, int bNew);

void fcntl_locking_init();
void fcntl_locking_term();
int fcntl_locking_filedesc_init(struct FileDesc *desc);
void fcntl_locking_filedesc_term(struct FileDesc *desc);

int fcntl_locking_close(int fildes);

int pwrite_filedesc_init(struct FileDesc *desc);
void pwrite_filedesc_term(struct FileDesc *desc);

void mmap_init();
void mmap_term();
int mmap_exception(struct _EXCEPTIONREPORTRECORD *report,
                   struct _EXCEPTIONREGISTRATIONRECORD *reg,
                   struct _CONTEXT *ctx);

void print_stats();
