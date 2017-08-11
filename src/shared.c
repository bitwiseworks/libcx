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
#include <process.h>
#include <stdarg.h>
#include <sys/builtin.h>

#include "shared.h"
#include "version.h"

#define __LIBC_LOG_GROUP TRACE_GROUP
#include <InnoTekLIBC/logstrict.h>

#include <InnoTekLIBC/fork.h>

/*
 * Debug builds are hardly compatible with release builds so
 * use a separat mutex and LIBCx shared memory block.
 */
#ifdef DEBUG
#define MUTEX_LIBCX "\\SEM32\\LIBCX_MUTEX_V" VERSION_MAJ_MIN "_debug"
#define SHAREDMEM_LIBCX "\\SHAREMEM\\LIBCX_DATA_V" VERSION_MAJ_MIN "_debug"
#else
#define MUTEX_LIBCX "\\SEM32\\LIBCX_MUTEX_V" VERSION_MAJ_MIN
#define SHAREDMEM_LIBCX "\\SHAREMEM\\LIBCX_DATA_V" VERSION_MAJ_MIN
#endif

#define HEAP_SIZE (1024 * 1024 * 2) /* 2MB - total shared data area size */
#define HEAP_INIT_SIZE 65536 /* Initial size of committed memory */
#define HEAP_INC_SIZE 65536 /* Heap increment amount */

#if defined(TRACE_ENABLED) && defined(TRACE_USE_LIBC_LOG)

static __LIBC_LOGGROUP  logGroup[] =
{
  { 1, "nogroup" },           /*  0 */
  { 1, "fcntl" },             /*  1 */
  { 1, "pwrite" },            /*  2 */
  { 1, "select" },            /*  3 */
  { 1, "mmap" },              /*  4 */
  { 1, "dosreadbugfix" },     /*  5 */
  { 1, "exeinfo" },           /*  6 */
};

static __LIBC_LOGGROUPS logGroups =
{
  0, sizeof(logGroup)/sizeof(logGroup[0]), logGroup
};

void *gLogInstance = NULL;

#endif

void *gAssertLogInstance = NULL;

HMODULE ghModule = NULLHANDLE;

SharedData *gpData = NULL;

static HMTX gMutex = NULLHANDLE;

static void APIENTRY ProcessExit(ULONG);

enum { StatsBufSize = 256 };
static int format_stats(char *buf, int size);

/*
 * @todo Currently we reserve a static block of HEAP_SIZE at LIBCx init
 * which we commit/release as needed. The disadvantage is obvious - we
 * constantly occupy the address space that could be needed for other
 * purposes, but limit the max memory size which may be not enough under
 * certain LIBCx load. But in order to allow for dynamic allocation of
 * shared memory we need to track all proccess IDs that are currently
 * using LIBCx so that we give them the newly allocated shared memory.
 * This is for later. See https://github.com/bitwiseworks/libcx/issues/9.
 */
static void *mem_alloc(Heap_t h, size_t *psize, int *pclean)
{
  APIRET arc;
  char *mem;
  size_t size;

  TRACE("psize %d\n", *psize);

  /* Round requested size up to HEAP_INC_SIZE */
  size = (*psize + HEAP_INC_SIZE - 1) / HEAP_INC_SIZE * HEAP_INC_SIZE;
  if (size + gpData->size > HEAP_SIZE)
    return NULL;

  mem = (char *)gpData + gpData->size;

  /* Commit the new block */
  arc = DosSetMem(mem, size, PAG_DEFAULT | PAG_COMMIT);
  TRACE("DosSetMem(%p, %d) = %ld\n", mem, size, arc);

  if (arc)
    return NULL;

  /* DosAllocSharedMem gives us zeroed mem */
  *pclean = _BLOCK_CLEAN;
  /* Return the actually allocated number of bytes */
  *psize = size;

  gpData->size += size;
  return mem;
}

/**
 * Initializes the shared structures.
 */
static void shared_init()
{
  /*
   * Note that due to LIBC bug #366 (http://trac.netlabs.org/libc/ticket/366)
   * it is forbidden to use many common LIBC functions (like malloc/printf/etc)
   * in this function.
   */

  APIRET arc;
  int rc;

  arc = DosExitList(EXLST_ADD, ProcessExit);
  ASSERT_MSG(arc == NO_ERROR, "%ld", arc);

#if defined(TRACE_ENABLED) && !defined(TRACE_USE_LIBC_LOG)
  /*
   * Allocate a larger buffer to fit lengthy TRACE messages and disable
   * auto-flush on EOL (to avoid breaking lines by stdout operations
   * from other threads/processes).
   */
  setvbuf(stdout, NULL, _IOFBF, 0x10000);
#endif

  while (1)
  {
    /* First, try to open the mutex */
    arc = DosOpenMutexSem(MUTEX_LIBCX, &gMutex);
    TRACE("DosOpenMutexSem = %lu\n", arc);

    if (arc == NO_ERROR)
    {
      /*
       * Init is (being) done by another process, request the mutex to
       * guarantee shared memory is already alloated, then get access to
       * it and open shared heap located in that memory.
       */
      arc = DosRequestMutexSem(gMutex, SEM_INDEFINITE_WAIT);
      ASSERT_MSG(arc == NO_ERROR, "%ld", arc);

      if (arc == NO_ERROR)
      {
        arc = DosGetNamedSharedMem((PPVOID)&gpData, SHAREDMEM_LIBCX, PAG_READ | PAG_WRITE);
        TRACE("DosGetNamedSharedMem = %lu\n", arc);
        if (arc)
        {
          /*
           * This failure means that another process was too fast to do what
           * it wanted and initiated global uninitialization before we got the
           * mutex so shared memory was already freed by this time. Retry.
           */
          DosReleaseMutexSem(gMutex);
          DosCloseMutexSem(gMutex);
          continue;
        }

        TRACE("gpData->heap = %p\n", gpData->heap);
        ASSERT(gpData->heap);

        rc = _uopen(gpData->heap);
        ASSERT_MSG(rc == 0, "%d (%d)", rc, errno);

        ASSERT(gpData->refcnt);
        gpData->refcnt++;
      }

      break;
    }

    if (arc == ERROR_SEM_NOT_FOUND)
    {
      /* We are the first process, create the mutex */
      arc = DosCreateMutexSem(MUTEX_LIBCX, &gMutex, 0, TRUE);
      TRACE("DosCreateMutexSem = %ld\n", arc);

      if (arc == ERROR_DUPLICATE_NAME)
      {
        /* Another process is faster, attempt to open the mutex again */
        continue;
      }
    }

    ASSERT_MSG(arc == NO_ERROR, "%ld", arc);

    /*
     * We are the process that successfully created the main mutex.
     * Proceed with the initial setup by allocating shared memory and
     * heap.
     */

    /* Allocate shared memory */
    arc = DosAllocSharedMem((PPVOID)&gpData, SHAREDMEM_LIBCX, HEAP_SIZE,
                            PAG_READ | PAG_WRITE | OBJ_ANY);
    TRACE("DosAllocSharedMem(OBJ_ANY) = %ld\n", arc);

    if (arc && arc != ERROR_ALREADY_EXISTS)
    {
      /* High memory may be unavailable, try w/o OBJ_ANY */
      arc = DosAllocSharedMem((PPVOID)&gpData, SHAREDMEM_LIBCX, HEAP_SIZE,
                              PAG_READ | PAG_WRITE);
      TRACE("DosAllocSharedMem = %ld\n", arc);
    }

    ASSERT_MSG(arc == NO_ERROR, "%ld", arc);

    TRACE("gpData %p\n", gpData);

    /* Commit the initial block */
    arc = DosSetMem(gpData, HEAP_INIT_SIZE, PAG_DEFAULT | PAG_COMMIT);
    ASSERT_MSG(arc == NO_ERROR, "%ld", arc);

    gpData->size = HEAP_INIT_SIZE;

    /* Create shared heap */
    gpData->heap = _ucreate(gpData + 1, HEAP_INIT_SIZE - sizeof(*gpData),
                            _BLOCK_CLEAN, _HEAP_REGULAR | _HEAP_SHARED,
                            mem_alloc, NULL);
    TRACE("gpData->heap = %p\n", gpData->heap);
    ASSERT(gpData->heap);

    rc = _uopen(gpData->heap);
    ASSERT_MSG(rc == 0, "%d (%d)", rc, errno);

    gpData->refcnt = 1;

    /* Initialize common structures */
    GLOBAL_NEW_ARRAY(gpData->procs, PROC_DESC_HASH_SIZE);
    TRACE("gpData->procs = %p\n", gpData->procs);
    ASSERT(gpData->procs);
    GLOBAL_NEW_ARRAY(gpData->files, FILE_DESC_HASH_SIZE);
    TRACE("gpData->files = %p\n", gpData->files);
    ASSERT(gpData->files);

    break;
  }

  /* Initialize individual components */
  mmap_init();
  fcntl_locking_init();

  DosReleaseMutexSem(gMutex);
}

static void shared_term()
{
  APIRET arc;
  int rc;

  TRACE("gMutex %lx, gpData %p (heap %p, refcnt %d)\n",
        gMutex, gpData, gpData ? gpData->heap : 0,
        gpData ? gpData->refcnt : 0);

  if (gAssertLogInstance)
  {
    // We're most likely crashing after an assertion, write out LIBCx stats
    char buf[StatsBufSize];
    format_stats(buf, sizeof(buf));
    __libc_LogRaw(gAssertLogInstance, __LIBC_LOG_MSGF_FLUSH, buf, sizeof(buf));
  }

  ASSERT(gMutex != NULLHANDLE);

  DosRequestMutexSem(gMutex, SEM_INDEFINITE_WAIT);

  if (gpData)
  {
    if (gpData->heap)
    {
#ifdef STATS_ENABLED
      _HEAPSTATS hst;
#endif
      int i;
      ProcDesc *proc;

      ASSERT(gpData->refcnt);
      gpData->refcnt--;

      /* Uninitialize individual components */
      fcntl_locking_term();
      mmap_term();

      /*
       * Remove the process description upon process termination (note that
       * all individual components of the desc should be already uninitialized
       * above)
       */
      proc = take_proc_desc(getpid());
      if (proc)
      {
        /* Free common per-process structures */
        TRACE("proc->files %p\n", proc->files);
        if (proc->files)
        {
          for (i = 0; i < FILE_DESC_HASH_SIZE; ++i)
          {
            FileDesc *desc = proc->files[i];
            while (desc)
            {
              FileDesc *next = desc->next;
              /* Call component-specific uninitialization */
              pwrite_filedesc_term(proc, desc);
              fcntl_locking_filedesc_term(proc, desc);
              free(desc);
              desc = next;
            }
          }
          free(proc->files);
        }
        free(proc);
      }

      if (gpData->refcnt == 0)
      {
        /* We are the last process, free common structures */
        TRACE("gpData->files %p\n", gpData->files);
        if (gpData->files)
        {
          for (i = 0; i < FILE_DESC_HASH_SIZE; ++i)
          {
            FileDesc *desc = gpData->files[i];
            while (desc)
            {
              FileDesc *next = desc->next;
              /* Call component-specific uninitialization */
              pwrite_filedesc_term(NULL, desc);
              fcntl_locking_filedesc_term(NULL, desc);
              free(desc);
              desc = next;
            }
          }
          free(gpData->files);
        }
        TRACE("gpData->procs %p\n", gpData->procs);
        if (gpData->procs)
          free(gpData->procs);
      }

      _uclose(gpData->heap);

      TRACE("reserved memory size %d\n", HEAP_SIZE);
      TRACE("committed memory size %d\n", gpData->size);
#ifdef STATS_ENABLED
      rc = _ustats(gpData->heap, &hst);
      TRACE("heap stats: %d total, %d used now, %d used max\n", hst._provided, hst._used, gpData->maxHeapUsed);
#endif

      if (gpData->refcnt == 0)
      {
#ifdef STATS_ENABLED
        ASSERT_MSG(!hst._used, "%d", hst._used);
#endif
        rc = _udestroy(gpData->heap, !_FORCE);
        TRACE("_udestroy = %d (%d)\n", rc, errno);
      }
    }

    arc = DosFreeMem(gpData);
    TRACE("DosFreeMem = %ld\n", arc);
  }

  DosReleaseMutexSem(gMutex);

  arc = DosCloseMutexSem(gMutex);
  if (arc == ERROR_SEM_BUSY)
  {
    /* The semaphore may be owned by us, try to release it */
    arc = DosReleaseMutexSem(gMutex);
    TRACE("DosReleaseMutexSem = %ld\n", arc);
    arc = DosCloseMutexSem(gMutex);
  }
  TRACE("DosCloseMutexSem = %ld\n", arc);

  DosExitList(EXLST_REMOVE, ProcessExit);
}

/**
 * Initialize/terminate DLL at load/unload.
 */
unsigned long _System _DLL_InitTerm(unsigned long hModule, unsigned long ulFlag)
{
  TRACE("hModule %lx, ulFlag %lu\n", hModule, ulFlag);

  switch (ulFlag)
  {
    /*
     * InitInstance. Note that this one is NOT called in a forked child — it's
     * assumed that the DLLs are already initialized in the parent and the child
     * receives an already initialized copy of DLL data. However, in some cases
     * this is not actually true (examples are OS/2 file handles, semaphores and
     * other resources that require special work besides data segment duplication
     * to be available in the child process) and LIBCx is one of these cases.
     * A solution here is to call shared_init() from a fork completion callback
     * (see forkCompletion() below).
     */
    case 0:
    {
      ghModule = hModule;
      if (_CRT_init() != 0)
        return 0;
      __ctordtorInit();
      shared_init();
      break;
    }

    /*
     * TermInstance. Called for everybody including forked children. We don't
     * call shared_term() from here at all and prefer a process exit hook
     * instead (see its description below).
     */
    case 1:
    {
      __ctordtorTerm();
      _CRT_term();
      ghModule = NULLHANDLE;
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
  TRACE("reason %lu\n", reason);

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

  ASSERT(gMutex != NULLHANDLE);
  ASSERT(gpData);

  arc = DosRequestMutexSem(gMutex, SEM_INDEFINITE_WAIT);

  ASSERT_MSG(arc == NO_ERROR, "%ld", arc);
}

/**
 * Releases the global mutex requested by mutex_lock().
 */
void global_unlock()
{
  APIRET arc;

  ASSERT(gMutex != NULLHANDLE);

  arc = DosReleaseMutexSem(gMutex);

  ASSERT_MSG(arc == NO_ERROR, "%ld", arc);
}

/**
 * Allocates a new block of shared LIBCX memory. Must be called under
 * global_lock().
 */
void *global_alloc(size_t size)
{
#ifdef STATS_ENABLED
  void *result = _ucalloc(gpData->heap, 1, size);
  if (result)
  {
    _HEAPSTATS hst;
    if (_ustats(gpData->heap, &hst) == 0 && gpData->maxHeapUsed < hst._used)
      gpData->maxHeapUsed = hst._used;
  }
  return result;
#else
  return _ucalloc(gpData->heap, 1, size);
#endif
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
 * Returns a process descriptor sturcture for the given process.
 * Must be called under global_lock().
 * Returns NULL when opt is HashMapOpt_New and there is not enough memory
 * to allocate a new sctructure, or when opt is not HashMapOpt_New and there
 * is no descriptor for the given process. When opt is HashMapOpt_Take and
 * the descriptor is found, it will be removed from the hash map (it's then
 * the responsibility of the caller to free the returned pointer).
 */
ProcDesc *get_proc_desc_ex(pid_t pid, enum HashMapOpt opt)
{
  size_t h;
  ProcDesc *desc, *prev;
  int rc;

  ASSERT(gpData);

  /*
   * We use identity as the hash function as we get a regularly ascending
   * sequence of PIDs on input and prime for the hash table size.
   */
  h = pid % PROC_DESC_HASH_SIZE;
  desc = gpData->procs[h];
  prev = NULL;

  while (desc)
  {
    if (desc->pid == pid)
      break;
    prev = desc;
    desc = desc->next;
  }

  if (!desc && opt == HashMapOpt_New)
  {
    GLOBAL_NEW(desc);
    if (desc)
    {
      /* Initialize the new desc */
      desc->pid = pid;
      /* Call component-specific initialization */
      /* NOTE: None at the moment. */
      /* Put to the head of the bucket */
      desc->next = gpData->procs[h];
      gpData->procs[h] = desc;
    }
  }
  else if (desc && opt == HashMapOpt_Take)
  {
    if (prev)
      prev->next = desc->next;
    else
      gpData->procs[h] = desc->next;
  }

  return desc;
}

/**
 * Returns a file descriptor sturcture for the given path.
 * The pid argument specifies the scope of the search: 0 will look up in the
 * global system-wide table, any other value - in a process-specific table.
 * Must be called under global_lock().
 * Returns NULL when opt is HashMapOpt_New and there is not enough memory
 * to allocate a new sctructure, or when opt is not HashMapOpt_New and there
 * is no descriptor for the given file. When opt is HashMapOpt_Take and
 * the descriptor is found, it will be removed from the hash map (it's then
 * the responsibility of the caller to free the returned pointer).
 */
FileDesc *get_proc_file_desc_ex(pid_t pid, const char *path, enum HashMapOpt opt)
{
  size_t h;
  FileDesc *desc, *prev;
  ProcDesc *proc;
  FileDesc **map;
  int rc;

  ASSERT(gpData);
  ASSERT(path);
  ASSERT(strlen(path) < PATH_MAX);

  if (pid)
  {
    proc = get_proc_desc_ex(pid, opt == HashMapOpt_New ? opt : HashMapOpt_None);
    if (!proc)
      return NULL;
    if (!proc->files)
    {
      /* Lazily create process-specific file desc hash map */
      GLOBAL_NEW_ARRAY(proc->files, FILE_DESC_HASH_SIZE);
      if (!proc->files)
        return NULL;
    }
    map = proc->files;
  }
  else
  {
    proc = NULL;
    map = gpData->files;
  }

  h = hash_string(path) % FILE_DESC_HASH_SIZE;
  desc = map[h];
  prev = NULL;

  while (desc)
  {
    if (strcmp(desc->path, path) == 0)
      break;
    desc = desc->next;
    prev = desc;
  }

  if (!desc && opt == HashMapOpt_New)
  {
    size_t extra_sz = proc ? sizeof(*desc->p) : sizeof(*desc->g);
    GLOBAL_NEW_PLUS(desc, extra_sz + strlen(path) + 1);
    if (desc)
    {
      /* Initialize the new desc */
      desc->path = ((char *)(desc + 1)) + extra_sz;
      strcpy(desc->path, path);
      /* Call component-specific initialization */
      rc = fcntl_locking_filedesc_init(proc, desc);
      if (rc == 0)
      {
        rc = pwrite_filedesc_init(proc, desc);
        if (rc == -1)
          fcntl_locking_filedesc_term(proc, desc);
      }
      if (rc == -1)
      {
        free(desc);
        return NULL;
      }
      /* Put to the head of the bucket */
      desc->next = map[h];
      map[h] = desc;
    }
  }
  else if (desc && opt == HashMapOpt_Take)
  {
    if (prev)
      prev->next = desc->next;
    else
      map[h] = desc->next;
  }

  return desc;
}

/**
 * LIBC close replacement. Used for performing extra processing on file
 * close.
 */
int close(int fildes)
{
  TRACE("fildes %d\n", fildes);

  if (fcntl_locking_close(fildes) == -1)
    return -1;
  return _std_close(fildes);
}

/**
 * Prints LIBCx statistics to a buffer which must be at least StatsBufSize
 * bytes long, otherwise truncation will happen.
 * @return snprintf return value.
 */
static int format_stats(char *buf, int size)
{
  int rc;
  _HEAPSTATS hst;

  rc = _ustats(gpData->heap, &hst);
  if (rc)
  {
    // Don't assert here as we might be terminating after another assert
    return snprintf(buf, size, "_ustats failed with %d (%d)\n", rc, errno);
  }

  return snprintf(buf, size,
                  "\n"
                  "LIBCx resource usage\n"
                  "--------------------\n"
                  "Reserved memory size:  %d bytes\n"
                  "Committed memory size: %d bytes\n"
                  "Heap size total:       %d bytes\n"
                  "Heap size used now:    %d bytes\n"
#ifdef STATS_ENABLED
                  "Heap size used max:    %d bytes\n"
#endif
                  ,
                  HEAP_SIZE, gpData->size,
                  hst._provided, hst._used
#ifdef STATS_ENABLED
                  , gpData->maxHeapUsed
#endif
                  );
}

/**
 * Prints LIBCx version and memory usage statistics to stdout.
 */
void print_stats()
{
  int rc;
  _HEAPSTATS hst;

  printf("LIBCx version: %d.%d.%d\n", VERSION_MAJOR, VERSION_MINOR, VERSION_BUILD);

  {
    char name[CCHMAXPATH] = {0};
    HMODULE hmod;
    ULONG obj, offset;
    APIRET arc;
    arc = DosQueryModFromEIP(&hmod, &obj, sizeof(name), name, &offset, (ULONG)print_stats);
    ASSERT_MSG(arc == NO_ERROR, "%ld", arc);
    arc = DosQueryModuleName(hmod, sizeof(name), name);
    ASSERT_MSG(arc == NO_ERROR, "%ld", arc);
    printf("LIBCx module:  %s\n", name);
  }

  global_lock();

  char buf[StatsBufSize];
  format_stats(buf, sizeof(buf));
  fputs(buf, stdout);

  global_unlock();
}

#ifdef DEBUG
/**
 * Forces LIBCx unitialization as if the process were terminated. Used
 * in some tests.
 */
void force_libcx_term()
{
  shared_term();
}

/**
 * Undoes the effect of force_libcx_init(). Used in some tests.
 */
void force_libcx_init()
{
  shared_init();
}
#endif

#if defined(TRACE_ENABLED) && defined(TRACE_USE_LIBC_LOG)

void trace(unsigned traceGroup, const char *file, int line, const char *func, const char *format, ...)
{
  va_list args;
  char *msg;
  unsigned cch;
  int n;
  ULONG ts;

  enum { MaxBuf = 513 };

  if (!gLogInstance)
  {
    __libc_LogGroupInit(&logGroups, "LIBCX_TRACE");
    gLogInstance = __libc_LogInit(0, &logGroups, "NUL");
    ASSERT(gLogInstance);

    /*
     * This is a dirty hack to write logs to the console,
     * LIBC isn't capable of it on its own (@todo fix LIBC).
     */
    typedef struct __libc_logInstance
    {
        /** Write Semaphore. */
        HMTX                    hmtx;
        /** Filehandle. */
        HFILE                   hFile;
        /** Api groups. */
        __LIBC_PLOGGROUPS       pGroups;
    } __LIBC_LOGINST, *__LIBC_PLOGINST;

    /* Sanity check */
    ASSERT(((__LIBC_PLOGINST)gLogInstance)->pGroups == &logGroups);

    DosDupHandle(1, &((__LIBC_PLOGINST)gLogInstance)->hFile);
  }

  msg = (char *)alloca(MaxBuf);
  if (!msg)
      return;

  DosQuerySysInfo(QSV_MS_COUNT, QSV_MS_COUNT, &ts, sizeof(ts));

  if (file != NULL && line != 0 && func != NULL)
    n = snprintf(msg, MaxBuf, "*** %08lx [%x:%d] %s:%d:%s: ", ts, getpid(), _gettid(), _getname(file), line, func);
  else if (file == NULL && line == 0 && func == NULL)
    n = snprintf(msg, MaxBuf, "*** %08lx [%x:%d] ", ts, getpid(), _gettid());
  else
    n = 0;
  if (n < MaxBuf)
  {
    va_start(args, format);
    n += vsnprintf(msg + n, MaxBuf - n, format, args);
    va_end(args);
  }
  if (n < MaxBuf)
    cch = n;
  else
    cch = MaxBuf - 1;

  __libc_LogRaw(gLogInstance, traceGroup | __LIBC_LOG_MSGF_FLUSH, msg, cch);
}

#endif /* defined(TRACE_ENABLED) && defined(TRACE_USE_LIBC_LOG) */

static void *get_assert_log_instance()
{
  if (gAssertLogInstance)
    return gAssertLogInstance;

  char buf[CCHMAXPATH + 128];

  // We don't query QSV_TIME_HIGH as it will remain 0 until 19-Jan-2038 and for
  // our purposes (generate an unique log name sorted by date) it's fine.
  ULONG time;
  DosQuerySysInfo(QSV_TIME_LOW, QSV_TIME_LOW, &time, sizeof(time));

  // Get log directory (boot drive if no UNIXROOT)
  const char *path = "/var/log/libcx";
  PSZ unixroot;
  if (DosScanEnv("UNIXROOT", &unixroot) != NO_ERROR)
  {
    ULONG drv;
    DosQuerySysInfo(QSV_BOOT_DRIVE, QSV_BOOT_DRIVE, &drv, sizeof(drv));

    unixroot = "C:";
    unixroot[0] = '@' + drv;
    path = "";
  }
  else
  {
    // Make sure the directory exists (no error checks here as the missing
    // directory will pop up later in __libc_LogInit anyway)
    if (strlen(unixroot) >= CCHMAXPATH)
      return NULL;
    strcpy(buf, unixroot);
    strcat(buf, path);
    DosCreateDir(buf, NULL);
  }

  // Get program name
  char name[CCHMAXPATH];
  PPIB ppib = NULL;
  DosGetInfoBlocks(NULL, &ppib);
  if (DosQueryModuleName(ppib->pib_hmte, sizeof(name), name) != NO_ERROR)
    return NULL;
  _remext(name);

  void *inst = __libc_LogInit(0, NULL, "%s%s/%s-%08lx-%04x.log",
                              unixroot, path, _getname(name), time, getpid());
  if (inst)
    if (!__atomic_cmpxchg32((uint32_t *)&gAssertLogInstance, (uint32_t)inst, 0))
      // NOTE: this must be __libc_LogTerm(inst) but it's missing (kLIBC bug)
      free(inst);

  // Bail out if we failed to create a log file at all
  if (!gAssertLogInstance)
    return NULL;

  // Write out LIBCx info
  strcpy(buf, "LIBCx version : " VERSION_MAJ_MIN_BLD "\n");
  strcat(buf, "LIBCx module  : ");
  APIRET arc = DosQueryModuleName(ghModule, CCHMAXPATH, buf + strlen(buf));
  if (arc == NO_ERROR)
    sprintf(buf + strlen(buf), " (hmod=%04lx)\n", ghModule);
  else
    sprintf(buf + strlen(buf), " <error %ld>\n", arc);
  __libc_LogRaw(gAssertLogInstance, __LIBC_LOG_MSGF_FLUSH, buf, strlen(buf));

  return gAssertLogInstance;
}

void libcx_assert(const char *string, const char *fname, unsigned int line, const char *format, ...)
{
  if (!gAssertLogInstance)
    if (!get_assert_log_instance())
      return;

  va_list args;
  char *msg = NULL;

  enum { MaxBuf = 512 };

  if (format)
  {
    msg = (char *)alloca(MaxBuf);
    if (!msg)
        return;

    va_start(args, format);

    int n = vsnprintf(msg, MaxBuf - 1, format, args);
    if (n != EOF)
    {
      // Check for truncation
      if (n >= MaxBuf - 1)
        n = MaxBuf - 2;
      // Add \n at the end if missing
      if (msg[n] != '\n')
      {
        msg[n] = '\n';
        msg[n + 1] = '\0';
      }
    }

    va_end(args);
  }

  // NOTE: This will issue a debugger breakpoint (or INT 3) and log to stderr
  // unless LIBC_STRICT_DISABLED is set.
  __libc_LogAssert(gAssertLogInstance, __LIBC_LOG_MSGF_FLUSH, NULL, fname, line, string, "%s", msg ? msg : "");
}

/*
 * Touches (reads/writes) the first word in every page of memory pointed to by
 * buf of len bytes. If buf is not on the page boundary, the word at buf is
 * touched instead. Note that the page is only touched if it's reserved (i.e.
 * neither committed, nor free).
 *
 * This function is used to work around a bug in DosRead that causes it to fail
 * when interrupted by the exception handler, see
 * https://github.com/bitwiseworks/libcx/issues/21.
 */
void touch_pages(void *buf, size_t len)
{
  APIRET arc;
  ULONG dos_len;
  ULONG dos_flags;

  volatile ULONG buf_addr = (ULONG)buf;
  ULONG buf_end = buf_addr + len;

  /*
   * Note: we need to at least perform the write operation when toucing so that
   * in case if it's our memory mapped region then it's marked dirty and with
   * PAG_WRITE is set (on read PAG_WRITE would have been removed whcih would
   * cause DosRead to fail too). And, to make sure that the touched region is
   * not corrupted by touching, we first read the target word and then simply
   * write it back.
   */

  if (!PAGE_ALIGNED(buf_addr))
  {
    dos_len = PAGE_SIZE;
    arc = DosQueryMem((PVOID)PAGE_ALIGN(buf_addr), &dos_len, &dos_flags);
    TRACE_IF(arc, "DosQueryMem = %lu\n", arc);
    if (!arc && !(dos_flags & (PAG_FREE | PAG_COMMIT)))
      *(int *)buf_addr = *(int *)buf_addr;
    buf_addr = PAGE_ALIGN(buf_addr) + PAGE_SIZE;
  }

  while (buf_addr < buf_end)
  {
    dos_len = ~0U;
    arc = DosQueryMem((PVOID)PAGE_ALIGN(buf_addr), &dos_len, &dos_flags);
    TRACE_IF(arc, "DosQueryMem = %lu\n", arc);
    if (!arc && !(dos_flags & (PAG_FREE | PAG_COMMIT)))
    {
      /* touch all pages within the reported range */
      dos_len += buf_addr;
      while (buf_addr < dos_len)
      {
        *(int *)buf_addr = *(int *)buf_addr;
        buf_addr += PAGE_SIZE;
      }
    }
    else
    {
      buf_addr += dos_len;
    }
  }
}

static int forkParentChild(__LIBC_PFORKHANDLE pForkHandle, __LIBC_FORKOP enmOperation)
{
  int rc = 0;

  switch(enmOperation)
  {
    case __LIBC_FORK_OP_FORK_CHILD:
    {
      shared_init();

      /*
       * @todo We would like to free & reset the log instance to NULL here to have it
       * properly re-initialized in the child process but this crashes the child
       * because LIBC Heap can't be used at that point. A solution would be either:
       *
       * - Re-init the log instance w/o allocation but __libc_LogInit doesn't support
       * that.
       * - Do it from the forkCompletion() callback which is called at the very end
       * of fork(), right before returning to user code, but unfortunately we can't
       * register a completion callback that would get called *after* LIBC own
       * completion callbacks (where the LIBC Heap is unlocked) and hence we can't
       * do it from there. There is a proposed fix for LIBC that makes it possible,
       * see http://trac.netlabs.org/libc/ticket/366#comment:4.
       *
       * For now, we don't do anything and this is accidentially fine for us because
       * we force logging to the console in trace() and the console handles in the
       * child process are identical to the parent (even the one we create with
       * DosDupHandle as it's inherited by default).
       */
#if defined(TRACE_ENABLED) && defined(TRACE_USE_LIBC_LOG) && 0
      /* Reset the log instance in the child process to cause a reinit */
      free(gLogInstance);
      gLogInstance = NULL;
#endif
      break;
    }
  }

  return rc;
}

_FORK_CHILD1(0xFFFFFFFF, forkParentChild);
