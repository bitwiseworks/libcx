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
#include <assert.h>
#include <emx/io.h>

#include "shared.h"
#include "version.h"

#include "spawn/spawn2-internal.h"

#define __LIBC_LOG_GROUP TRACE_GROUP
#include <InnoTekLIBC/logstrict.h>

#include <InnoTekLIBC/fork.h>
#include <InnoTekLIBC/FastInfoBlocks.h>

/*
 * Debug builds are hardly compatible with release builds so use a separate
 * mutex and LIBCx shared memory block. Also use a separate mutex and memory
 * block for development builds to avoid interfering with the release build.
 */
#ifdef DEBUG
#define LIBCX_DEBUG_SUFFIX "_debug"
#else
#define LIBCX_DEBUG_SUFFIX ""
#endif
#ifdef LIBCX_DEV_BUILD
#define LIBCX_DEV_SUFFIX "_dev"
#else
#define LIBCX_DEV_SUFFIX ""
#endif
#define MUTEX_LIBCX "\\SEM32\\LIBCX_MUTEX_V" VERSION_MAJ_MIN_BLD LIBCX_DEBUG_SUFFIX LIBCX_DEV_SUFFIX
#define SHAREDMEM_LIBCX "\\SHAREMEM\\LIBCX_DATA_V" VERSION_MAJ_MIN_BLD LIBCX_DEBUG_SUFFIX LIBCX_DEV_SUFFIX

#define HEAP_SIZE (1024 * 1024 * 2) /* 2MB - total shared data area size */
#define HEAP_INIT_SIZE 65536 /* Initial size of committed memory */
#define HEAP_INC_SIZE 65536 /* Heap increment amount */

#if defined(TRACE_ENABLED) && defined(TRACE_USE_LIBC_LOG)

static __LIBC_LOGGROUP  gLogGroup[] =
{
  { 1, "nogroup" },           /*  0 */
  { 1, "fcntl" },             /*  1 */
  { 1, "pwrite" },            /*  2 */
  { 1, "select" },            /*  3 */
  { 1, "mmap" },              /*  4 */
  { 1, "dosreadbugfix" },     /*  5 */
  { 1, "exeinfo" },           /*  6 */
  { 1, "close" },             /*  7 */
  { 1, "spawn" },             /*  8 */
};

static __LIBC_LOGGROUPS gLogGroups =
{
  0, sizeof(gLogGroup)/sizeof(gLogGroup[0]), gLogGroup
};

#endif

static volatile uint32_t gLogInstanceState = 0;
static void *gLogInstance = NULL;
static BOOL gLogToConsole = FALSE;
static BOOL gSeenAssertion = FALSE;

static BOOL gInFork = FALSE;

static HMODULE ghModule = NULLHANDLE;

SharedData *gpData = NULL;

static HMTX gMutex = NULLHANDLE;

static void APIENTRY ProcessExit(ULONG);

enum { StatsBufSize = 512 };
static int format_stats(char *buf, int size);

static void *get_log_instance();

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
  {
    TRACE("out of memory");
    return NULL;
  }

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

        /*
         * It's an ordinary LIBCx process. Increase coutners.
         */

        TRACE("gpData->heap = %p\n", gpData->heap);
        ASSERT(gpData->heap);

        rc = _uopen(gpData->heap);
        ASSERT_MSG(rc == 0, "%d (%d)", rc, errno);

        ASSERT(gpData->refcnt);
        gpData->refcnt++;
        TRACE("gpData->refcnt = %d\n", gpData->refcnt);
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
     * It's a process that successfully created the main mutex, i.e. the first
     * LIBCx process. Proceed with the initial setup by allocating shared
     * memory and heap.
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

  /*
   * Perform common initialization (both for the first and ordinary processes).
   */

  /* Make sure a process description for this process is created */
  ProcDesc *proc = get_proc_desc(getpid());
  ASSERT(proc);

  /* Initialize individual components */
  mmap_init(proc);
  fcntl_locking_init(proc);

  /* Check if it's a spawn2 wrapper (e.g. spawn2-wrapper.c) */
  {
    char dll[CCHMAXPATH + sizeof(SPAWN2_WRAPPERNAME) + 1];
    if (get_module_name(dll, sizeof(dll)))
    {
      strcpy(_getname(dll), SPAWN2_WRAPPERNAME);

      char exe[CCHMAXPATH + 1];
      if (_execname(exe, sizeof(exe)) == 0 && stricmp(dll, exe) == 0)
      {
        proc->flags |= Proc_Spawn2Wrapper;
        TRACE("spawn2 wrapper\n");

        /* Make sure the semaphore is available (needed for spawn2-wrapper.c) */
        ASSERT(gpData->spawn2_sem);
        global_spawn2_sem(proc);
      }
    }
  }

  DosReleaseMutexSem(gMutex);

  TRACE("done\n");
}

static void shared_term()
{
  APIRET arc;
  int rc;

  TRACE("gMutex %lx, gpData %p (heap %p, refcnt %d), gSeenAssertion %lu\n",
        gMutex, gpData, gpData ? gpData->heap : 0,
        gpData ? gpData->refcnt : 0, gSeenAssertion);

#if !defined(TRACE_ENABLED)
  if (gSeenAssertion && get_log_instance())
  {
    /*
     * We're crashing after an assertion, write out LIBCx stats (not needed in
     * trace builds as we will trace that out later anyway)
     */
    char *buf = alloca(StatsBufSize);
    if (buf)
    {
      format_stats(buf, StatsBufSize);
      __libc_LogRaw(gLogInstance, __LIBC_LOG_MSGF_FLUSH, buf, StatsBufSize);
    }
  }
#endif

  ASSERT(gSeenAssertion || gMutex != NULLHANDLE);

  DosRequestMutexSem(gMutex, SEM_INDEFINITE_WAIT);

  if (gpData)
  {
    if (gpData->heap)
    {
      int i;
      ProcDesc *proc;

      ASSERT(gpData->refcnt);
      gpData->refcnt--;

      /* Remove the process description upon process termination */
      size_t bucket = 0;
      ProcDesc *prev = NULL;
      proc = find_proc_desc_ex(getpid(), &bucket, &prev);

      /* Uninitialize individual components */
      fcntl_locking_term(proc);
      mmap_term(proc);

      TRACE("proc %p\n", proc);
      if (proc)
      {
        if (proc->spawn2_wrappers)
        {
          TRACE("proc->spawn2_wrappers %p\n", proc->spawn2_wrappers);
          free(proc->spawn2_wrappers);
        }

        if (proc->spawn2_sem)
        {
          TRACE("proc->spawn2_sem %lx (refcnt %d)\n", proc->spawn2_sem, gpData->spawn2_sem_refcnt);

          ASSERT(proc->spawn2_sem == gpData->spawn2_sem);
          DosCloseEventSem(gpData->spawn2_sem);

          ASSERT(gpData->spawn2_sem_refcnt != 0);
          --gpData->spawn2_sem_refcnt;

          if (gpData->spawn2_sem_refcnt == 0)
            gpData->spawn2_sem = NULLHANDLE;
        }

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
              free_file_desc(desc, i, NULL, NULL);
              desc = next;
            }
          }
          free(proc->files);
        }

        free_proc_desc(proc, bucket, prev);
      }

      if (gpData->refcnt == 0)
      {
        /* We are the last process, free common structures */
        TRACE("gpData->files %p\n", gpData->files);
        if (gpData->files)
        {
          /* Make sure we don't have lost SharedFileDesc data */
          for (i = 0; i < FILE_DESC_HASH_SIZE; ++i)
            ASSERT_MSG(!gpData->files[i], "%p", gpData->files[i]);
          free(gpData->files);
        }
        TRACE("gpData->procs %p\n", gpData->procs);
        if (gpData->procs)
          free(gpData->procs);
      }

#ifdef TRACE_ENABLED
      {
        char *buf = alloca(StatsBufSize);
        if (buf)
        {
          format_stats(buf, StatsBufSize);
          TRACE("%s", buf);
        }
      }
#endif

      _uclose(gpData->heap);

      if (gpData->refcnt == 0)
      {
#ifdef STATS_ENABLED
        _HEAPSTATS hst;
        rc = _ustats(gpData->heap, &hst);
        ASSERT_MSG(!rc, "%d (%d)", rc, errno);
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
  // Make sure ghModule is initialized, TRACE needs it.
  if (ghModule == NULLHANDLE)
    ghModule = hModule;

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
  /* Make sure we don't start an endless spin in get_log_instance() */
  if (gLogInstanceState == 1)
    gLogInstanceState = 2;

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
 * Releases the global mutex requested by global_lock().
 */
void global_unlock()
{
  APIRET arc;

  ASSERT(gMutex != NULLHANDLE);

  arc = DosReleaseMutexSem(gMutex);

  ASSERT_MSG(arc == NO_ERROR, "%ld", arc);
}

/**
 * Returns the spawn2 semaphore lazily creating it or making sure it's
 * available in the given process (the current process if NULL). Will return
 * NULLHANDLE if lazy creation fails. Must be called under global_lock().
 */
unsigned long global_spawn2_sem(ProcDesc *proc)
{
  APIRET arc;

  if (!proc)
    proc = find_proc_desc(getpid());

  ASSERT(proc);

  if (gpData->spawn2_sem == NULLHANDLE)
  {
    ASSERT(proc->spawn2_sem == NULLHANDLE);

    arc = DosCreateEventSem(NULL, &gpData->spawn2_sem, DC_SEM_SHARED | DCE_AUTORESET, FALSE);
    if (arc != NO_ERROR)
      return NULLHANDLE;

    ASSERT(gpData->spawn2_sem_refcnt == 0);
    gpData->spawn2_sem_refcnt = 1;

    proc->spawn2_sem = gpData->spawn2_sem;
  }
  else if (proc->spawn2_sem == NULLHANDLE)
  {
    arc = DosOpenEventSem(NULL, &gpData->spawn2_sem);
    ASSERT_MSG(arc == NO_ERROR, "%ld %lx", arc, gpData->spawn2_sem);

    ASSERT(gpData->spawn2_sem_refcnt != 0);
    ++gpData->spawn2_sem_refcnt;

    proc->spawn2_sem = gpData->spawn2_sem;
  }

  TRACE("spawn2_sem %lx (refcnt %d)\n", proc->spawn2_sem, gpData->spawn2_sem_refcnt);

  return proc->spawn2_sem;
}

/**
 * Returns a process-specific fmutex used to guard OS/2 TCP/IP DLL calls.
 * This mutex must be used to provide thread safety where it (or reentrancy)
 * is guaranteed by the respective API. Note that we assume that TCP/IP DLL
 * calls are process safe and process-reentrant and thus only guarantee thread
 * safety within a single process.
 */
_fmutex *global_tcpip_sem()
{
  _fmutex *fsem = NULL;

  global_lock();

  ProcDesc *proc = find_proc_desc(getpid());

  ASSERT(proc);

  if (proc->tcpip_fsem.hev == NULLHANDLE)
  {
    /*
     * Lazily create the mutex. Note that we never destroy it as
     * process-specific ones will be freed automatically on process termination
     */
    int rc = _fmutex_create(&proc->tcpip_fsem, 0);
    ASSERT(!rc);
  }

  fsem = &proc->tcpip_fsem;

  global_unlock();

  return fsem;
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
    if (_ustats(gpData->heap, &hst) == 0 && gpData->max_heap_used < hst._used)
      gpData->max_heap_used = hst._used;
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
 * Returns a process description sturcture for the given process.
 * Must be called under global_lock().
 * Optional o_bucket and o_prev arguments will receive the appropriate values
 * for the returned process description when they are not NULL (and may be later
 * used in e.g. a free_proc_desc_ex() call).
 * Returns NULL when opt is HashMapOpt_New and there is not enough memory
 * to allocate a new sctructure, or when opt is not HashMapOpt_New and there
 * is no descriptor for the given process.
 */
ProcDesc *get_proc_desc_ex(pid_t pid, enum HashMapOpt opt, size_t *o_bucket, ProcDesc **o_prev)
{
  size_t bucket;
  ProcDesc *desc, *prev;
  int rc;

  ASSERT(gpData);

  /*
   * We use identity as the hash function as we get a regularly ascending
   * sequence of PIDs on input and prime for the hash table size.
   */
  bucket = pid % PROC_DESC_HASH_SIZE;
  desc = gpData->procs[bucket];
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
      desc->next = gpData->procs[bucket];
      gpData->procs[bucket] = desc;

#if STATS_ENABLED
      ++gpData->num_procs;
      if (gpData->num_procs > gpData->max_procs)
        gpData->max_procs = gpData->num_procs;
#endif
    }
  }

  if (o_bucket)
    *o_bucket = bucket;
  if (o_prev)
    *o_prev = prev;

  return desc;
}

/**
 * Frees the given process description. Note that bucket, prev, and proc must be
 * valid values as received from find_proc_desc_ex in order to maintain
 * the map of remaining process descriptions.
 */
void free_proc_desc(ProcDesc *desc, size_t bucket, ProcDesc *prev)
{
  ASSERT(desc);
  ASSERT_MSG(bucket <= PROC_DESC_HASH_SIZE, "%u", bucket);

  /* Remove from the hash map */
  if (prev)
    prev->next = desc->next;
  else
    gpData->procs[bucket] = desc->next;

  free(desc);

#if STATS_ENABLED
  --gpData->num_procs;
#endif
}

/**
 * Returns a file description structure for the given process and file name.
 * Must be called under global_lock().
 * The fd argument is only used when it's not -1 and when opt is HashMapOpt_New
 * — in this case the given fd will be associated with the returned description
 * so that when close is called on that fd it will be deassociated (and will
 * cause the desc deletion if there is no other use of it). Optional o_bucket,
 * o_prev and o_proc arguments will receive the appropriate values for the
 * returned file description when they are not NULL (and may be later used in
 * e.g. a free_file_desc_ex() call). Optional o_desc_g, when not NULL, will
 * receive a global (shared) file description - this is the only way to get it
 * when no process-specific file description exists and opt is HashMapOpt_None.
 * Returns NULL when opt is HashMapOpt_New and there is not enough memory
 * to allocate a new sctructure, or when opt is not HashMapOpt_New and there
 * is no descriptor for the given file.
 */
FileDesc *get_file_desc_ex(pid_t pid, int fd, const char *path, enum HashMapOpt opt,
                           size_t *o_bucket, FileDesc **o_prev, ProcDesc **o_proc,
                           SharedFileDesc **o_desc_g)
{
  size_t bucket;
  FileDesc *desc, *prev;
  ProcDesc *proc;
  int rc;

  ASSERT(gpData);
  ASSERT(path);

  if (pid == -1)
    pid = getpid();

  enum { FDArrayInc = 4 };

  proc = get_proc_desc_ex(pid, opt == HashMapOpt_New ? opt : HashMapOpt_None, NULL, NULL);
  if (!proc)
    return NULL;

  if (!proc->files)
  {
    /* Lazily create a process-specific file desc hash map */
    GLOBAL_NEW_ARRAY(proc->files, FILE_DESC_HASH_SIZE);
    if (!proc->files)
      return NULL;
  }

  bucket = hash_string(path) % FILE_DESC_HASH_SIZE;
  desc = proc->files[bucket];
  prev = NULL;

  while (desc)
  {
    ASSERT(desc->g);
    if (strcmp(desc->g->path, path) == 0)
      break;
    prev = desc;
    desc = desc->next;
  }

  if (!desc && opt == HashMapOpt_New)
  {
    /* Initialize a new desc */
    GLOBAL_NEW(desc);
    if (desc)
    {
      /* Associate the fd with this description */
      desc->size_fds = FDArrayInc;
      GLOBAL_NEW_ARRAY(desc->fds, desc->size_fds);
      if (desc->fds)
      {
        desc->fds[0] = fd;
        memset(&desc->fds[1], 0xFF, sizeof(desc->fds[0]) * (FDArrayInc - 1));

        /* Associate with the shared part, if any */
        desc->g = gpData->files[bucket];
        while (desc->g)
        {
          if (strcmp(desc->g->path, path) == 0)
            break;
          desc->g = desc->g->next;
        }

        if (!desc->g)
        {
          /* Initialize a new shared part */
          GLOBAL_NEW_PLUS(desc->g, strlen(path) + 1);
          if (desc->g)
          {
            desc->g->refcnt = 1;
            desc->g->path = ((char *)(desc->g + 1));
            strcpy(desc->g->path, path);
          }

          TRACE("new global file desc %p for [%s]\n", desc->g, desc->g->path);
        }
        else
        {
          /* Reuse the existing shared part */
          ++desc->g->refcnt;
          ASSERT_MSG(desc->g->refcnt >= 2, "%d", desc->g->refcnt);
        }

        if (desc->g)
        {
          /* Call component-specific initialization */
          rc = fcntl_locking_filedesc_init(desc);
          if (rc == 0)
          {
            rc = pwrite_filedesc_init(desc);
            if (rc == -1)
              fcntl_locking_filedesc_term(desc);
          }

          if (rc == 0)
          {
            if (desc->g->refcnt == 1)
            {
              /* Put to the head of the bucket (shared part) */
              desc->g->next = gpData->files[bucket];
              gpData->files[bucket] = desc->g;

#if STATS_ENABLED
              ++gpData->num_shared_files;
              if (gpData->num_shared_files > gpData->max_shared_files)
                gpData->max_shared_files = gpData->num_shared_files;
#endif
            }

            /* Put to the head of the bucket */
            desc->next = proc->files[bucket];
            proc->files[bucket] = desc;

#if STATS_ENABLED
            ++gpData->num_files;
            if (gpData->num_files > gpData->max_files)
              gpData->max_files = gpData->num_files;
#endif
          }
          else
          {
            if (desc->g->refcnt == 1)
              free(desc->g);
            else
              --desc->g->refcnt;
            free(desc->fds);
            free(desc);
            desc = NULL;
          }

          TRACE("new file desc %p for g %p [%s] (refcnt %d)\n",
                desc, desc->g, desc->g->path, desc->g->refcnt);
        }
        else
        {
          free(desc->fds);
          free(desc);
          desc = NULL;
        }
      }
      else
      {
        free(desc);
        desc = NULL;
      }
    }
  }

  if (fd != -1 && desc && opt == HashMapOpt_New)
  {
    int i;

    /* Associate the fd with this file description (if not already) */
    for (i = 0; i < desc->size_fds; ++i)
      if (desc->fds[i] == -1 || desc->fds[i] == fd)
        break;

    if (i < desc->size_fds)
    {
      desc->fds[i] = fd;
    }
    else
    {
      int *fds = RENEW_ARRAY(desc->fds, desc->size_fds + FDArrayInc);
      if (fds)
      {
        desc->fds = fds;
        desc->fds[desc->size_fds] = fd;
        memset(&desc->fds[desc->size_fds + 1], 0xFF, sizeof(desc->fds[0]) * (FDArrayInc - 1));
        desc->size_fds += FDArrayInc;
      }
      else
        desc = NULL;
    }
  }
  else if (opt == HashMapOpt_None)
  {
    if (o_desc_g)
    {
      if (!desc)
      {
        /* Return a global description if there is any */
        SharedFileDesc *desc_g = gpData->files[bucket];

        while (desc_g)
        {
          if (strcmp(desc_g->path, path) == 0)
            break;
          desc_g = desc_g->next;
        }

        *o_desc_g = desc_g;
      }
      else
      {
        *o_desc_g = desc->g;
      }
    }
  }

  if (o_bucket)
    *o_bucket = bucket;
  if (o_prev)
    *o_prev = prev;
  if (o_proc)
    *o_proc = proc;

  return desc;
}

/**
 * Frees the given file description. Note that bucket, prev, and proc must be
 * valid values as received from find_file_desc_ex in order to maintain
 * the map of remaining file descriptions. An exception is when all file
 * descriptions for a given process are deleted at once in which case only
 * bucket must be valid (and equal to an index of desc in the bucket).
 */
void free_file_desc(FileDesc *desc, size_t bucket, FileDesc *prev, ProcDesc *proc)
{
  ASSERT(desc);
  ASSERT(desc->g);

  TRACE("Will free file desc %p for g %p [%s] (refcnt %d)\n",
        desc, desc->g, desc->g->path, desc->g->refcnt);

  ASSERT_MSG(!desc->fh, "%p", desc->fh);
  ASSERT_MSG(!desc->map, "%p", desc->map);

  ASSERT_MSG(bucket <= FILE_DESC_HASH_SIZE, "%u", bucket);

  /* Call component-specific uninitialization */
  pwrite_filedesc_term(desc);
  fcntl_locking_filedesc_term(desc);

  --desc->g->refcnt;
  if (desc->g->refcnt == 0)
  {
    TRACE("Will free global file desc %p\n", desc->g);

    /* Remove from the hash map (shared part) */
    SharedFileDesc *prev_g = gpData->files[bucket];
    if (prev_g == desc->g)
    {
      gpData->files[bucket] = desc->g->next;
    }
    else
    {
      while (prev_g && prev_g->next != desc->g)
        prev_g = prev_g->next;
      ASSERT(prev_g);
      prev_g->next = desc->g->next;
    }

    /* And free data (shared part) */
    free(desc->g);

#if STATS_ENABLED
    --gpData->num_shared_files;
#endif
  }

  /* Remove from the hash map */
  if (prev)
    prev->next = desc->next;
  else if (proc)
    proc->files[bucket] = desc->next;

  free(desc->fds);
  free(desc);

#if STATS_ENABLED
  --gpData->num_files;
#endif
}

int _std_close(int fildes);

/**
 * LIBC close replacement. Used for performing extra processing on file
 * close.
 */
int close(int fildes)
{
  TRACE_TO(TRACE_GROUP_CLOSE, "fildes %d\n", fildes);

  __LIBC_PFH pFH;
  int rc = 0;

  pFH = __libc_FH(fildes);
  if (pFH && pFH->pszNativePath)
  {
    TRACE_TO(TRACE_GROUP_CLOSE, "pszNativePath %s, fFlags %x\n", pFH->pszNativePath, pFH->fFlags);

    global_lock();

    size_t bucket = 0;
    FileDesc *prev = NULL;
    ProcDesc *proc = NULL;
    FileDesc *desc = find_file_desc_ex(pFH->pszNativePath, &bucket, &prev, &proc);
    if (desc)
    {
      TRACE_TO(TRACE_GROUP_CLOSE, "Found file desc %p for [%s]\n", desc, desc->g->path);

      rc = fcntl_locking_close(desc);

      if (rc == 0)
      {
        int i;
        int seen_other_fd = 0;

        /*
         * Deassociate the fd from this file description (note that there may be
         * none — e.g. if this desc was created for shared mmap in a forked
         * child and never for anything else)
         */
        for (i = 0; i < desc->size_fds; ++i)
        {
          if (desc->fds[i] == fildes)
          {
            desc->fds[i] = -1;
            break;
          }
          if (desc->fds[i] != -1)
            seen_other_fd = 1;
        }

        if (desc->fh == NULL && desc->map == NULL && !seen_other_fd)
        {
          /* Finish checking if there is any usage through fds for this desc */
          for (++i; i < desc->size_fds; ++i)
            if (desc->fds[i] != -1)
              break;
          seen_other_fd = i < desc->size_fds;

          if (!seen_other_fd)
            free_file_desc(desc, bucket, prev, proc);
        }
      }
    }

    global_unlock();
  }

  if (rc != 0)
    return rc;

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
    /* Don't assert here as we might be terminating after another assert */
    return snprintf(buf, size, "_ustats failed with %d (%d)\n", rc, errno);
  }

  size_t num_procs = 0;
  size_t num_files = 0;
  size_t num_shared_files = 0;
#ifdef STATS_ENABLED
  num_procs = gpData->num_procs;
  num_files = gpData->num_files;
  num_shared_files = gpData->num_shared_files;
#else
  int i, j;

  /* No statistics available, calculate them on the fly */
  for (i = 0; i < PROC_DESC_HASH_SIZE; ++i)
  {
    ProcDesc *proc = gpData->procs[i];
    while (proc)
    {
      ++num_procs;

      if (proc->files)
      {
        for (j = 0; j < FILE_DESC_HASH_SIZE; ++j)
        {
          FileDesc *desc = proc->files[j];
          while (desc)
          {
            ++num_files;
            desc = desc->next;
          }
        }
      }

      proc = proc->next;
    }
  }

  for (i = 0; i < FILE_DESC_HASH_SIZE; ++i)
  {
    SharedFileDesc *desc = gpData->files[i];
    while (desc)
    {
      ++num_shared_files;
      desc = desc->next;
    }
  }
#endif

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
                  "ProcDesc structs used now:       %d\n"
#ifdef STATS_ENABLED
                  "ProcDesc structs used max:       %d\n"
#endif
                  "FileDesc structs used now:       %d\n"
#ifdef STATS_ENABLED
                  "FileDesc structs used max:       %d\n"
#endif
                  "SharedFileDesc structs used now: %d\n"
#ifdef STATS_ENABLED
                  "SharedFileDesc structs used max: %d\n"
#endif
                  , HEAP_SIZE, gpData->size
                  , hst._provided, hst._used
#ifdef STATS_ENABLED
                  , gpData->max_heap_used
#endif
                  , num_procs
#ifdef STATS_ENABLED
                  , gpData->max_procs
#endif
                  , num_files
#ifdef STATS_ENABLED
                  , gpData->max_files
#endif
                  , num_shared_files
#ifdef STATS_ENABLED
                  , gpData->max_shared_files
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

  printf("LIBCx version: " VERSION_MAJ_MIN_BLD LIBCX_DEBUG_SUFFIX LIBCX_DEV_SUFFIX "\n");

  {
    char name[CCHMAXPATH] = {0};
    get_module_name(name, sizeof(name));
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

void libcx_trace(unsigned traceGroup, const char *file, int line, const char *func, const char *format, ...)
{
  if (!gLogInstance && !get_log_instance())
    return;

  va_list args;
  char *msg;
  unsigned cch;
  int n;
  ULONG ts;

  enum { MaxBuf = 513 };

  msg = (char *)alloca(MaxBuf);
  if (!msg)
      return;

  DosQuerySysInfo(QSV_MS_COUNT, QSV_MS_COUNT, &ts, sizeof(ts));

  if (gLogToConsole)
  {
    /*
     * Logging to the console differs from logging to the file: 1) we want to
     * put PID in prefix (since there may be more than one PID logging) and
     * 2) we want to visually differentiate from the normal program output so
     * we prefix it with stars too.
     */
    if (file != NULL && line != 0 && func != NULL)
      n = snprintf(msg, MaxBuf, "*** %08lx %04x:%02x %s:%d:%s: ", ts, getpid(), _gettid(), _getname(file), line, func);
    else if (file == NULL && line == 0 && func == NULL)
      n = snprintf(msg, MaxBuf, "*** %08lx %04x:%02x ", ts, getpid(), _gettid());
    else
      n = 0;
  }
  else
  {
    /*
     * Try to match the standard LIBC log file formatting as much as we can
     * TODO: Add a flag to LIBC to suppress the standard legend in the log header
     */
    if (file != NULL && line != 0 && func != NULL)
      n = snprintf(msg, MaxBuf, "%08lx %02x %s:%d:%s: ", ts, _gettid(), _getname(file), line, func);
    else if (file == NULL && line == 0 && func == NULL)
      n = snprintf(msg, MaxBuf, "%08lx %02x ", ts, _gettid());
    else
      n = 0;
  }

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

static void *get_log_instance()
{
  if (gLogInstance || gLogInstanceState == 2 || gInFork)
    return gLogInstance;

  /* Set a flag that we're going to init a new log instance */
  if (!__atomic_cmpxchg32(&gLogInstanceState, 1, 0))
  {
    /*
     * Another thread was faster in starting instance creation, wait for it
     * to complete in a simple spin loop (completion should be really quick).
     */
    while (gLogInstanceState == 1)
      DosSleep(1);
    return gLogInstance;
  }

  void *logInstance = NULL;
  __LIBC_LOGGROUPS *logGroups = NULL;

#if defined(TRACE_ENABLED)
  logGroups = &gLogGroups;
  __libc_LogGroupInit(&gLogGroups, "LIBCX_TRACE");
#endif

  /* Check if we are asked to log to console */
  {
    PSZ dummy;
    gLogToConsole = DosScanEnv("LIBCX_TRACE_TO_CONSOLE", &dummy) == NO_ERROR;
  }

  char buf[CCHMAXPATH + 128];

  if (gLogToConsole)
  {
    logInstance = __libc_LogInit(0, logGroups, "NUL");
    if (logInstance)
    {
      /*
       * This is a dirty hack to write logs to stdout,
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

      /* Sanity check (note: we use LIBC assert here to avoid recursion) */
      assert(((__LIBC_PLOGINST)logInstance)->pGroups == logGroups);

      /* Duplicate STDOUT */
      DosDupHandle(1, &((__LIBC_PLOGINST)logInstance)->hFile);
    }
  }
  else
  {
    /*
     * We don't query QSV_TIME_HIGH as it will remain 0 until 19-Jan-2038 and for
     * our purposes (generate an unique log name sorted by date) it's fine.
     */
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
      /*
       * Make sure the directory exists (no error checks here as a failure to
       * do so will pop up later in __libc_LogInit anyway).
       */
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

    logInstance = __libc_LogInit(0, logGroups, "%s%s/%s-%08lx-%04x.log",
                                 unixroot, path, _getname(name), time, getpid());
  }

  /* Bail out if we failed to create a log file at all */
  if (!logInstance)
  {
    gLogInstanceState = 0;
    return NULL;
  }

  if (!gLogToConsole)
  {
    // Write out LIBCx info
    strcpy(buf, "LIBCx version : " VERSION_MAJ_MIN_BLD LIBCX_DEBUG_SUFFIX LIBCX_DEV_SUFFIX "\n");
    strcat(buf, "LIBCx module  : ");
    APIRET arc = DosQueryModuleName(ghModule, CCHMAXPATH, buf + strlen(buf));
    if (arc == NO_ERROR)
      sprintf(buf + strlen(buf), " (hmod=%04lx)\n", ghModule);
    else
      sprintf(buf + strlen(buf), " <error %ld>\n", arc);
    __libc_LogRaw(logInstance, __LIBC_LOG_MSGF_FLUSH, buf, strlen(buf));
  }

  /*
   * Finish initialization by storing the instance and setting the state to 2
   * (this will unfreeze other threads that started instance creation, if any).
   */
  gLogInstance = logInstance;
  gLogInstanceState = 2;
  return gLogInstance;
}

void libcx_assert(const char *string, const char *fname, unsigned int line, const char *format, ...)
{
  gSeenAssertion = TRUE;

  char *msg = NULL;

  if (format)
  {
    va_list args;

    enum { MaxBuf = 512 };

    msg = (char *)alloca(MaxBuf);
    if (msg)
    {
      va_start(args, format);

      int n = vsnprintf(msg, MaxBuf - 1, format, args);
      if (n != EOF)
      {
        /* Check for truncation */
        if (n >= MaxBuf - 1)
          n = MaxBuf - 2;
        /* Add \n at the end if missing */
        if (msg[n] != '\n')
        {
          msg[n] = '\n';
          msg[n + 1] = '\0';
        }
      }

      va_end(args);
    }
  }

  if (!gLogInstance && !get_log_instance())
  {
    /* Fallback to LIBC assert */
    if (msg)
      fprintf(stderr, "Assertion info: %s", msg);
    _assert(string, fname, line);
    /* Should never get here but still... */
    return;
  }

  /*
   * NOTE: This will issue a debugger breakpoint (or INT 3) and log to stderr
   * unless LIBC_STRICT_DISABLED is set. Also note that it will also duplicate
   * the assert message output to STDERR so in case if we log to console, there
   * will be two copies of it (kLIBC bug/limitation).
   */
  __libc_LogAssert(gLogInstance, __LIBC_LOG_MSGF_FLUSH, NULL, fname, line, string, "%s", msg ? msg : "");
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

char *get_module_name(char *buf, size_t len)
{
  HMODULE hmod;
  ULONG obj, offset;
  APIRET arc;

  arc = DosQueryModFromEIP(&hmod, &obj, len, buf, &offset, (ULONG)get_module_name);
  TRACE_IF(arc, "DosQueryModFromEIP %ld\n", arc);
  ASSERT_MSG(!arc || arc == ERROR_BAD_LENGTH, "%ld %d", arc, len);
  if (arc)
    return NULL;

  arc = DosQueryModuleName(hmod, len, buf);
  TRACE_IF(arc, "DosQueryModuleName %ld\n", arc);
  ASSERT_MSG(!arc || arc == ERROR_BAD_LENGTH, "%ld %d", arc, len);
  if (arc)
    return NULL;

  return buf;
}

static void forkCompletion(void *pvArg, int rc, __LIBC_FORKCTX enmCtx)
{
  pid_t pidParent = (pid_t)pvArg;

  /*
   * It's safe to use LIBC again.
   */
  gInFork = FALSE;

  if (enmCtx != __LIBC_FORK_CTX_CHILD)
    return;

  /*
   * Reset the log instance in the child process to cause a reinit. Note that we
   * only do it when logging to a file. For console logging it's accidentally
   * fine to just leave it as is because the console handles in the child
   * process are identical to the parent (even the one we create in the dirty
   * hack in get_log_instance() with DosDupHandle as it's inherited by default).
   */
  if (!gLogToConsole)
  {
    /* Free the instance we inherit from the parent */
    if (gLogInstance)
      free(gLogInstance);

    gLogInstanceState = 0;
    gLogInstance = NULL;
  }

  gSeenAssertion = FALSE;

  /*
   * Initialize LIBCx in the forked child (note that for normal children this is
   * done in _DLL_InitTerm()).
   */
  shared_init();
}

static int forkParentChild(__LIBC_PFORKHANDLE pForkHandle, __LIBC_FORKOP enmOperation)
{
  int rc = 0;

  if (enmOperation == __LIBC_FORK_OP_EXEC_PARENT)
  {
    /*
     * Indicate that we are in forking mode so that LIBC can't be used (see
     * below).
     */
    gInFork = TRUE;

    /*
     * Register a completion function that will be executed in the child process
     * right before returning from fork(), i.e. when LIBC is fully operational
     * (as opposed to the __LIBC_FORK_OP_FORK_CHILD callback time when e.g. LIBC
     * Heap is locked and can't be used). Note that __LIBC_FORK_CTX_FLAGS_LAST
     * is vital for that as otherwise the completion callback will be executed
     * before LIBC own completion callbacks that in particular unlock the Heap.
     * Note that __LIBC_FORK_CTX_FLAGS_LAST support requires kLIBC with a patch
     * from http://trac.netlabs.org/libc/ticket/366.
     */
    rc = pForkHandle->pfnCompletionCallback(pForkHandle, forkCompletion, (void *)pForkHandle->pidParent,
                                            __LIBC_FORK_CTX_BOTH | __LIBC_FORK_CTX_FLAGS_LAST);
  }

  return rc;
}

_FORK_PARENT1(0xFFFFFFFF, forkParentChild);
