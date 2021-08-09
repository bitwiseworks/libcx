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
#include <emx/umalloc.h> /* for _um_realloc */
#include <string.h> /* for TRACE_ERRNO */
#include <sys/param.h> /* PAGE_SIZE */
#include <sys/fmutex.h>

/** Executes statement(s) syntactically wrapped as a func call. */
#define do_(stmt) if (1) { stmt; } else do {} while (0)

#ifdef TRACE_ENABLED

#ifndef TRACE_USE_LIBC_LOG
#define TRACE_USE_LIBC_LOG 1
#endif

#ifdef TRACE_USE_LIBC_LOG
#ifndef TRACE_GROUP
#define TRACE_GROUP 0
#endif
/* The below defs must be in sync with the gLogGroup array */
#define TRACE_GROUP_FCNTL 1
#define TRACE_GROUP_PWRITE 2
#define TRACE_GROUP_SELECT 3
#define TRACE_GROUP_MMAP 4
#define TRACE_GROUP_DOSREADBUGFIX 5
#define TRACE_GROUP_EXEINFO 6
#define TRACE_GROUP_CLOSE 7
#define TRACE_GROUP_SPAWN 8
#define TRACE_GROUP_SHMEM 9
#endif

#ifndef TRACE_MORE
#define TRACE_MORE 1
#endif

#define TRACE_FLAG_MASK  0xFF000000
#define TRACE_FLAG_NOSTD 0x10000000

#ifdef TRACE_USE_LIBC_LOG
void libcx_trace(unsigned traceGroup, const char *file, int line, const char *func, const char *format, ...) __printflike(5, 6);
#define TRACE_FLUSH() do {} while (0)
#define TRACE_RAW(msg, ...) libcx_trace(TRACE_GROUP, __FILE__, __LINE__, __FUNCTION__, msg, ## __VA_ARGS__)
#define TRACE_CONT(msg, ...) libcx_trace(TRACE_GROUP, NULL, -1, NULL, msg, ## __VA_ARGS__)
#define TRACE_TO(grp, msg, ...) libcx_trace(grp, __FILE__, __LINE__, __FUNCTION__, msg, ## __VA_ARGS__)
#else
#define TRACE_FLUSH() fflush(stdout)
#define TRACE_RAW(msg, ...) printf("*** [%d:%d] %s:%d:%s: " msg, getpid(), _gettid(), __FILE__, __LINE__, __FUNCTION__, ## __VA_ARGS__)
#define TRACE_CONT(msg, ...) printf(msg, ## __VA_ARGS__)
#define TRACE_TO(grp, msg, ...) TRACE_RAW(msg, ## __VA_ARGS__)
#endif

#define TRACE(msg, ...) do { TRACE_RAW(msg, ## __VA_ARGS__); TRACE_FLUSH(); } while(0)
#define TRACE_BEGIN(msg, ...) { do { TRACE_RAW(msg, ## __VA_ARGS__); } while(0)
#define TRACE_END() TRACE_FLUSH(); } do {} while(0)
#define TRACE_IF(cond, msg, ...) if (cond) TRACE(msg, ## __VA_ARGS__)
#define TRACE_BEGIN_IF(cond, msg, ...) if (cond) TRACE_BEGIN(msg, ## __VA_ARGS__)

#define TRACE_ERRNO(msg, ...) TRACE(msg ": %s (%d)\n", ##__VA_ARGS__, strerror(errno), errno)
#define TRACE_ERRNO_IF(cond, msg, ...) if (cond) TRACE_ERRNO(msg, ## __VA_ARGS__)

#define TRACE_AND(stmt, msg, ...) do_(TRACE(msg, ##__VA_ARGS__); stmt)
#define TRACE_ERRNO_AND(stmt, msg, ...) do_(TRACE_ERRNO(msg, ##__VA_ARGS__); stmt)

#define TRACE_PERR(rc) do { int _rc = (rc); if (rc < 0) TRACE(#rc " = %d, errno %d (%s)\n", _rc, errno, strerror(errno)); else TRACE(#rc " = %d\n", _rc); } while(0)

#else

#define TRACE_MORE 0
#define TRACE_FLUSH() do {} while (0)
#define TRACE_RAW(msg, ...) do {} while (0)
#define TRACE_TO(grp, msg, ...) do {} while (0)
#define TRACE(msg, ...) do {} while (0)
#define TRACE_BEGIN(msg, ...) if (0) { do {} while(0)
#define TRACE_CONT(msg, ...) do {} while (0)
#define TRACE_END() } do {} while(0)
#define TRACE_IF(cond, msg, ...) do {} while (0)
#define TRACE_BEGIN_IF(cond, msg, ...) if (0) { do {} while(0)
#define TRACE_ERRNO(msg, ...) do {} while (0)
#define TRACE_ERRNO_IF(msg, ...) do {} while (0)
#define TRACE_AND(stmt, msg, ...) do_(stmt)
#define TRACE_ERRNO_AND(stmt, msg, ...) do_(stmt)
#define TRACE_PERR(rc) do {} while (0)

#endif /* TRACE_ENABLED */

#ifndef ASSERT_USE_LIBC_LOG
#define ASSERT_USE_LIBC_LOG 1
#endif

#if ASSERT_USE_LIBC_LOG
void libcx_assert(const char *string, const char *fname, unsigned int line, const char *func, const char *format, ...) __printflike(5, 6);
#define ASSERT_MSG(cond, msg, ...) do { if (!(cond)) { libcx_assert(#cond, __FILE__, __LINE__, __FUNCTION__, msg, ## __VA_ARGS__); } } while(0)
#define ASSERT_NO_PERR(rc) do { int _rc = (rc); if (_rc != 0) { libcx_assert(#rc " = 0", __FILE__, __LINE__, __FUNCTION__, "%d (errno %d, %s)", _rc, errno, strerror(errno)); } } while(0)
#define ASSERT(cond) ASSERT_MSG(cond, NULL)
#define ASSERT_FAILED() ASSERT(FALSE)
#else
#include <assert.h>
#define ASSERT_MSG(cond, msg, ...) do { if (!(cond)) { fprintf(stderr, "Assertion info: " msg, ## __VA_ARGS__); fflush(stderr); _assert(#cond, __FILE__, __LINE__); } } while(0)
#define ASSERT_NO_PERR(rc) do { int _rc = (rc); if (_rc != 0) { fprintf(stderr, "Assertion info: %d (errno %d, %s)", _rc, errno, strerror(errno)); fflush(stderr); _assert(#rc " = 0", __FILE__, __LINE__); } } while(0)
#define ASSERT(cond) assert(cond)
#define ASSERT_FAILED() ASSERT(FALSE)
#endif

/** Set errno and execute the given statement (does tracing in debug builds). */
#define SET_ERRNO_AND(stmt, code) do_(TRACE("setting errno to %d\n", (code)); errno = (code); stmt)
/** Set errno (does tracing in debug builds). */
#define SET_ERRNO(code) SET_ERRNO_AND(code, (void)0)

/**
 * Suppresses the ERROR_INTERRUPT return value in Dos API calls by retrying the
 * operation as long as this code is returned. This is primarily intended to
 * avoid unnecessary interrupts of system calls that may happen during POSIX
 * signal delivery (e.g. SIGCHLD). Note that the macro cannot be used in
 * assignment expressions so call it with the whole assignment as an argument.
 */
#define DOS_NI(expr) while((expr) == ERROR_INTERRUPT)

/** Divides count by bucket_sz and rounds the result up. */
#define DIVIDE_UP(count, bucket_sz) (((count) + (bucket_sz - 1)) / (bucket_sz))
/** Rounds count up to be a multiple of bucket_sz */
#define ROUND_UP(count, bucket_sz) (DIVIDE_UP(count, bucket_sz) * bucket_sz)
/** Same as ROUND_UP but optimized for when bucket_sz is a power of 2 */
#define ROUND_UP_2(count, bucket_sz) (((count) + (bucket_sz - 1)) & ~(bucket_sz - 1))

/** Returns 1 if addr is page-aligned and 0 otherwise. */
#define PAGE_ALIGNED(addr) (!(((ULONG)addr) & (PAGE_SIZE - 1)))
/** Returns addr aligned to page boundary. */
#define PAGE_ALIGN(addr) (((ULONG)addr) & ~(PAGE_SIZE - 1))
/** Returns length rounded up to a nearest page boundary. */
#define PAGE_ROUND_UP(length) (ROUND_UP_2(length), PAGE_SIZE)
/** Returns the number of pages needed for count bytes. */
#define NUM_PAGES(count) DIVIDE_UP((count), PAGE_SIZE)

#define FILE_DESC_HASH_SIZE 127 /* Prime */

#define PROC_DESC_HASH_SIZE 17 /* Prime */

/**
 * Global system-wide file description (hash map entry).
 */
typedef struct SharedFileDesc
{
  struct SharedFileDesc *next;

  int refcnt; /* Number of FileDesc sturcts using us */

  char *path; /* File name with full path (follows the struct) */
  struct FileMap *map; /* Per-file mmap data */
  struct FcntlLock *fcntl_locks; /* Active fcntl file locks */
  unsigned long pwrite_lock; /* Mutex used in pwrite/pread */
} SharedFileDesc;

/**
 * Process-specific file description (hash map entry).
 */
typedef struct FileDesc
{
  struct FileDesc *next;

  SharedFileDesc *g; /* Global file description */

  struct FileMap *map; /* Per-file mmap data */
  struct FileHandle *fh; /* File handle for mmap */

  int *fds; /* Open FDs for this file (-1 for free entry) */
  size_t size_fds; /* Current size of fds array */

} FileDesc;

/**
 * Enums for ProcDesc::flags.
 */
enum
{
  Proc_Spawn2Wrapper = 0x01, /* Process is a spawn2 wrapper */
};

/**
 * Process descriptor (hash map entry).
 */
typedef struct ProcDesc
{
  struct ProcDesc *next;

  pid_t pid;
  FileDesc **files; /* Process-specific file descrition hash map of FILE_DESC_HASH_SIZE */
  struct ProcMemMap *mmap; /* Process-specific data for mmap */
  struct MemMap *mmaps; /* Process-visible memory mapings */
  int flags; /* Process-specific flags */
  unsigned long spawn2_sem; /* Global spawn2_sem if open in this process */
  struct SpawnWrappers *spawn2_wrappers; /* spawn2 wrapper->wrapped mappings */
  _fmutex tcpip_fsem; /* Mutex for making thread-safe TCP/IP DLL calls */
  struct Interrupts *interrupts; /* Interrupt request data for this process */
} ProcDesc;

/**
 * Global system-wide data structure (header).
 */
typedef struct SharedData
{
  size_t size; /* Committed size */
  Heap_t heap; /* Shared heap */
  int refcnt; /* Number of processes using us */
  ProcDesc **procs; /* Process description hash map of PROC_INFO_HASH_SIZE */
  SharedFileDesc **files; /* File description hash map of FILE_DESC_HASH_SIZE */
  struct FcntlLocking *fcntl_locking; /* Shared data for fcntl locking */
  unsigned long spawn2_sem; /* Signals spawn2 wrapper events */
  int spawn2_sem_refcnt; /* Number of processes using it */
  struct ShmemData *shmem; /* shmem API data structure */
#ifdef STATS_ENABLED
  size_t max_heap_used; /* Max size of used heap space */
  size_t num_procs; /* Number of ProcDesc structs */
  size_t max_procs; /* Max number of ProcDesc structs */
  size_t num_files; /* Number of FileDesc structs (all processes) */
  size_t max_files; /* Max number of FileDesc structs (all processes) */
  size_t num_shared_files; /* Number of SharedFileDesc structs */
  size_t max_shared_files; /* Max number of SharedFileDesc structs */
#endif
  /* Heap memory follows here */
} SharedData;

/**
 * Pointer to the global shared data structure.
 * Lazily initialized upon the first call of global_lock().
 */
extern SharedData *gpData;

/**
 * Current (this) process description for fast reference.
 * Always valid for the duration of LIBCx lifetime but indiviual fields may
 * still need `global_lock` or some other means of serialized access.
 */
extern ProcDesc *gpProcDesc;

/**
 * TLS to save the FPU CW.
 */
extern int gFpuCwTls;

struct _EXCEPTIONREPORTRECORD;
struct _EXCEPTIONREGISTRATIONRECORD;
struct _CONTEXT;

void global_lock();
void global_unlock();
int global_lock_info(pid_t *pid, int *tid, unsigned *count);
void global_lock_deathcheck();

unsigned long global_spawn2_sem(ProcDesc *proc);
_fmutex *global_tcpip_sem();

void *global_alloc(size_t size);

void *crealloc(void *ptr, size_t old_size, size_t new_size);

#define GLOBAL_NEW(ptr) (ptr) = (__typeof(ptr))global_alloc(sizeof(*(ptr)))
#define GLOBAL_NEW_PLUS(ptr, more) (ptr) = (__typeof(ptr))global_alloc(sizeof(*(ptr)) + (more))
#define GLOBAL_NEW_ARRAY(ptr, sz) (ptr) = (__typeof(ptr))global_alloc(sizeof(*(ptr)) * (sz))
#define GLOBAL_NEW_PLUS_ARRAY(ptr, arr, sz) (ptr) = (__typeof(ptr))global_alloc(sizeof(*(ptr)) + sizeof(*(arr)) * (sz))

#define NEW(ptr) (ptr) = (__typeof(ptr))calloc(1, sizeof(*(ptr)))
#define NEW_PLUS(ptr, more) (ptr) = (__typeof(ptr))calloc(1, sizeof(*(ptr)) + (more))
#define NEW_ARRAY(ptr, sz) (ptr) = (__typeof(ptr))calloc((sz), sizeof(*(ptr)))
#define NEW_PLUS_ARRAY(ptr, arr, sz) (ptr) = (__typeof(ptr))calloc(1, sizeof(*(ptr)) + sizeof(*(arr)) * (sz))

#define RENEW_PLUS(ptr, old_more, new_more) ((__typeof(ptr))crealloc(ptr, sizeof(*(ptr)) + (old_more), sizeof(*(ptr)) + (new_more)))
#define RENEW_ARRAY(ptr, old_sz, new_sz) ((__typeof(ptr))crealloc(ptr, sizeof(*(ptr)) * (old_sz), sizeof(*(ptr)) * (new_sz)))
#define RENEW_PLUS_ARRAY(ptr, arr, old_sz, new_sz) ((__typeof(ptr))crealloc(ptr, sizeof(*(ptr)) + sizeof(*(arr)) * (old_sz), sizeof(*(ptr)) + sizeof(*(arr)) * (new_sz)))

#define COPY_STRUCT(to, from) memcpy((to), (from), sizeof(*(from)))
#define COPY_STRUCT_PLUS(to, from, more) memcpy((to), (from), sizeof(*(from)) + (more))
#define COPY_ARRAY(to, from, sz) memcpy((to), (from), sizeof(*(from)) * (sz))
#define COPY_STRUCT_PLUS_ARRAY(to, from, arr, sz) memcpy((to), (from), sizeof(*(from)) + sizeof(*(arr)) * (sz))

#define CLEAR_STRUCT(ptr) bzero((ptr), sizeof(*(ptr)))

enum HashMapOpt
{
  HashMapOpt_None = 0,
  HashMapOpt_New = 1,
  HashMapOpt_Take = 2,
};

ProcDesc *get_proc_desc_ex(pid_t pid, enum HashMapOpt opt);
static inline ProcDesc *get_proc_desc(pid_t pid) { return get_proc_desc_ex(pid, HashMapOpt_New); }
static inline ProcDesc *find_proc_desc(pid_t pid) { return get_proc_desc_ex(pid, HashMapOpt_None); }
static inline ProcDesc *take_proc_desc(pid_t pid) { return get_proc_desc_ex(pid, HashMapOpt_Take); }

FileDesc *get_file_desc_ex(pid_t pid, int fd, const char *path, enum HashMapOpt opt, size_t *o_bucket, FileDesc **o_prev, ProcDesc **o_proc, SharedFileDesc **o_desc_g);
static inline FileDesc *get_file_desc(int fd, const char *path) { return get_file_desc_ex(-1, fd, path, HashMapOpt_New, NULL, NULL, NULL, NULL); }
static inline FileDesc *find_file_desc(const char *path, SharedFileDesc **o_desc_g) { return get_file_desc_ex(-1, -1, path, HashMapOpt_None, NULL, NULL, NULL, o_desc_g); }
static inline FileDesc *find_file_desc_ex(const char *path, size_t *o_bucket, FileDesc **o_prev, ProcDesc **o_proc) { return get_file_desc_ex(-1, -1, path, HashMapOpt_None, o_bucket, o_prev, o_proc, NULL); }
void free_file_desc(FileDesc *desc, size_t bucket, FileDesc *prev, ProcDesc *proc);

void fcntl_locking_init(ProcDesc *proc);
void fcntl_locking_term(ProcDesc *proc);
int fcntl_locking_filedesc_init(FileDesc *desc);
void fcntl_locking_filedesc_term(FileDesc *desc);

int fcntl_locking_close(FileDesc *desc);

int pwrite_filedesc_init(FileDesc *desc);
void pwrite_filedesc_term(FileDesc *desc);

void mmap_init(ProcDesc *proc);
void mmap_term(ProcDesc *proc);
int mmap_exception(struct _EXCEPTIONREPORTRECORD *report,
                   struct _EXCEPTIONREGISTRATIONRECORD *reg,
                   struct _CONTEXT *ctx);

void shmem_data_init(ProcDesc *proc);
void shmem_data_term(ProcDesc *proc);

void interrupt_init(ProcDesc *proc, int forked);
void interrupt_term(ProcDesc *proc);
int interrupt_exception(struct _EXCEPTIONREPORTRECORD *report,
                        struct _EXCEPTIONREGISTRATIONRECORD *reg,
                        struct _CONTEXT *ctx);

typedef int INTERRUPT_WORKER (pid_t pid, void *data);
typedef struct InterruptResult *INTERRUPT_RESULT;
int interrupt_request(pid_t pid, INTERRUPT_WORKER *worker, void *data, INTERRUPT_RESULT *result);
int interrupt_request_rc(INTERRUPT_RESULT result);
void interrupt_request_release(INTERRUPT_RESULT result);

void print_stats();

void touch_pages(void *buf, size_t len);

char *get_module_name(char *buf, size_t len);

#ifdef DEBUG
void force_libcx_term();
void force_libcx_init();
#endif

#ifdef APIENTRY /* <os2.h> included? */

ULONG APIENTRY _doscalls_DosRead(HFILE hFile, PVOID pBuffer, ULONG ulLength,
                                 PULONG pulBytesRead);

#endif
