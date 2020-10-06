/*
 * Public shared memory API.
 * Copyright (C) 2020 bww bitwise works GmbH.
 * This file is part of the kLIBC Extension Library.
 * Authored by Dmitry Kuminov <coding@dmik.org>, 2020.
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

#ifndef LIBCX_SHMEM_H
#define LIBCX_SHMEM_H

#include <sys/types.h>

#define SHMEM_INVALID -1

#define SHMEM_READONLY 0x1
#define SHMEM_PUBLIC 0x10

typedef int SHMEM;

__BEGIN_DECLS

/**
 * Creates a new shared memory object of a given size.
 *
 * The requested size is rounded up to a next page boundary (4096 bytes) to
 * match the memory allocation granularity. The actual size of the created
 * memory object may be obtained later with `shmem_get_info` after this function
 * succeeds.
 *
 * The created memory object is represented with a handle that is used to
 * control the object lifetime and access the underlying memory. The memory
 * object handle is a system-wide resource that exists as long as there are
 * processes that make use of it.
 *
 * A process that created the shared memory object must call `shmem_close` when
 * it no longer needs it.
 *
 * Other processes may gain access to the shared memory object by calling
 * `shmem_give` or `shmem_open` with a handle that they receive from the
 * creating process (or any other process having access to the handle) via some
 * form of IPC (inter-process communication) whcih is beyond the scope of this
 * API. When a process that gained access to the shared memory object via its
 * handle doesn't need it any more, it must also call `shmem_close` to indicate
 * that.
 *
 * Note that in order for `shmem_open` to succeed in another process,
 * SHMEM_PUBLIC must be set in @a flags to indicate that this memory object is
 * available to any process in the system. This is insecure, so it is
 * recommended to not use this flag and use `shmem_give` instead to explicitly
 * give access only to trusted processes.
 *
 * In order to access the underlying shared memory, a process having access to
 * its handle must call `shmem_map`.
 *
 * Access permissions to the memory object are process-specific and all mappings
 * of a given memory object share the same permissions within a given process.
 * By default, all mappings have read-write permissions but may be restricted to
 * read-only for certain processes when calling `shmem_give` or `shmem_open` on
 * the returned handle or for entire handles with `shmem_duplicate`. This
 * function itself always returns a read-write handle and does not allow
 * restricting it to read-only (does not accept SHMEM_READONLY in @a flags)
 * because this API is designed so that restrictions, once put, cannot be lifted
 * later and creating a read-only memory object which no-one can modify is
 * pointless.
 *
 * Note that creating a shared memory object with this function only reserves an
 * address range for it in the virtual address space of this and all other
 * processes. No physical memory is consumed until this (or some other) process
 * actually maps the object with `shmem_map`.
 *
 * Note that if a process that has access to the handle and terminates or
 * crashes without calling `shmem_unmap` and/or `shmem_close`, the handle be
 * automatically released by this API at process termination. This prevents
 * memory objects from getting stalled and leaking shared memory. Avoiding to
 * explicitly unmap and close unneeded handles is, however, considered a bad
 * practice as this delays reuse of the underlying resources by other tasks and
 * processes.
 *
 * @param[in]  size   Requested size of the shared memory object.
 * @param[in]  flags  0 or SHMEM_PUBLIC.
 *
 * @return     Memory object handle or SHMEM_INVALID and error code in `errno`.
 */
SHMEM shmem_create(size_t size, int flags);

/**
 * Gives access to a shared memory object to another process.
 *
 * The memory object must be previously created with `shmem_create` without
 * SHMEM_PUBLIC flag, or this function will fail with `errno` set to EACCES.
 * Once this function succeeds, the memory object handle passed as an argument
 * to this function should be communicated to the target process using some form
 * of IPC where it can be immediately used for mapping in `shmem_map` calls.
 *
 * Only the given handle will be made available in the target process for use.
 * An attempt to use other handles referring to the same memory object will fail
 * unless they are also made available in it using this function.
 *
 * By default, the target process will inherit access permissions to the memory
 * object from the handle. Setting SHMEM_READONLY in @a flags will effectively
 * make a read-write handle visible as read-only in the target process (if the
 * handle is already read-only, it will simply remain as such). Note that access
 * permissions of the original handle and its mappings in the calling process
 * will remain unchanged in this case.
 *
 * Once not needed in the target process, the handle should be closed with
 * `shmem_close` in that process.
 *
 * If the specified handle is already given to the target process, this function
 * will fail and set `errno` to EPERM.
 *
 * @param[in]  h      Shared memory object handle.
 * @param[in]  pid    PID of a target process to receive access to the handle.
 * @param[in]  flags  0 or SHMEM_READONLY.
 *
 * @return     0 on success, otherwise -1 and error code in `errno`.
 */
int shmem_give(SHMEM h, pid_t pid, int flags);

/**
 * Opens a shared memory object making it accessible in the calling process.
 *
 * The memory object must be previously created with `shmem_create` using
 * SHMEM_PUBLIC flag in another process and its handle passed as an argument to
 * this function should be communicated to this process using some form of IPC.
 * If SHMEM_PUBLIC was not specified at memory object reation, this function
 * will fail with EACCES.
 *
 * By default, the calling process will inherit access permissions to the memory
 * object from the handle. Setting SHMEM_READONLY in @a flags will effectively
 * make a read-write handle visible as read-only in the calling process (if the
 * handle is already read-only, it will simply remain as such). Note that access
 * permissions of the original handle and its mappings in the other processes
 * will remain unchanged in this case.
 *
 * Once not needed, the handle should be closed with `shmem_close`.
 *
 * If the specified handle is already open in the calling process, this function
 * will fail and set `errno` to EPERM.
 *
 * @param[in]  h      Shared memory object handle.
 * @param[in]  flags  0 or SHMEM_READONLY.
 *
 * @return     0 on success, otherwise -1 and error code in `errno`.
 */
int shmem_open(SHMEM h, int flags);

/**
 * Creates a duplicate of a given handle.
 *
 * The duplicate handle refers to the same memory object but is guaranteed to
 * have a distinct numeric value and inherits the source handle's access
 * permissions unless they are modified with @a flags.
 *
 * A typical purpose of a duplicate is to obtain a strong reference to the
 * existing memory object in the calling process preventing it from getting
 * freed until the duplicate is closed with `shmem_close`.
 *
 * Another typical purpose of a duplicate is to restrict access to the
 * underlying memory object from read-write to read-only for all mappings made
 * through the duplicate. This is achieved by by setting SHMEM_READONLY in @a
 * flags when calling this function. If the source handle is already read-only,
 * setting SHMEM_READONLY has no effect. Note that this restriction is primarily
 * intended to control access to the memory object from other processes (by
 * passing them the restricted duplcate and making it accessible there with
 * `shmem_give` or `shmem_open`).
 *
 * Note that if a restricted duplicate is passed to a `shmem_map` call in the
 * process that created it and that call succeeds, the restriction will apply to
 * all existing mappings of the same memory object in this process itself, even
 * those mapped earlier from handles that did not have such a restriction. This
 * behavior is imposed by OS/2 specifics.
 *
 * Once not needed, the handle should be closed with `shmem_close`.
 *
 * @param[in]  h      Shared memory object handle.
 * @param[in]  flags  0 or SHMEM_READONLY.
 *
 * @return     Memory object handle or SHMEM_INVALID and error code in `errno`.
 */
SHMEM shmem_duplicate(SHMEM h, int flags);

/**
 * Closes a given handle.
 *
 * If this function succeeds, the specified handle becomes invalid in the
 * calling process and cannot be used for mapping in `shmem_map` calls. Note
 * that any existing mappings made through this handle will not prevent this
 * function from succeeding and will remain accessible until unmapped with
 * `shmem_unmap`.
 *
 * If the closed handle is the last one referring to the underlying memory
 * object and there are no mappings of it in this or any other process, this
 * function will free the memory object making the underlying memory availalble
 * for new allocations. Hence, it is important to always call `shmem_close` when
 * the handle is no longer needed.
 *
 * @param[in]  h     Shared memory object handle.
 *
 * @return     0 on success, otherwise -1 and error code in `errno`.
 */
int shmem_close(SHMEM h);

/**
 * Maps a shared memory object into the address space of the calling process and
 * returns its vritual address.
 *
 * The mapping is specified with an offset from the beginning of the memory
 * object and a desired length of the mapping. If the requested operation
 * succeeds, it is guaranteed that the whole range of the mapping is backed up
 * by physical memory.
 *
 * The mapping range specified by @a offset and @a length arguments must fit the
 * size of the created memory object specified in `shmem_create`. Otherwise,
 * this function will fail with `errno` set to ERANGE. Also, @a offset must be
 * aligned to a page boundary (4096 bytes), or this function will fail with
 * `errno` set to EINVAL.
 *
 * Specifying 0 as @a length will cause the range to start at @a offset and
 * extend to the end of the memory object. Specifying 0 in both @a offset and @a
 * length will cause the entire memory object to be mapped.
 *
 * The returned mapping will have read-write access permissions unless they were
 * restricted for the specified handle in this process using SHMEM_READONLY flag
 * when calling `shmem_give`,`shmem_open` or if the handle is a read-only
 * duplicate created with `shmem_duplicate`.
 *
 * It is important to note that if the specified handle carries a read-only
 * restriction in the calling process and the calling process has any existing
 * mappings of the same memory object that overlap the requested range, this
 * function will implicitly apply this restriction to the overlapping parts of
 * all of these mappings, even those mapped earlier from handles that did not
 * have such a restriction. Any further attempt to write to any overlapping part
 * of such a mapping will result into an access violation exception. This
 * behavior is imposed by the OS/2 memory manager.
 *
 * When the mapping is no longer necessary, it should be unmapped by calling
 * `shmem_unmap` using exactly the same address value as returned by this
 * function. Multiple mappings of the same memory object are allowed and they
 * may overlap but each mapping should be separately released with a
 * corresponding call to `shmem_unmap` in order to let the system release the
 * memory object and eventally free the underlying physical memory.
 *
 * Note that due to another OS/2 memory manager constraint, this function may
 * return identical virtual address values for multiple `shmem_map` calls and
 * not only in this process but in any other process as well. Applications,
 * however, should never rely on this fact and should treat each returned
 * address as a distinct entity (e.g. call `shmem_unmap` on it as many times as
 * it was returned by `shmem_map`) in order to free all underlying system
 * resources.
 *
 * @param[in]  h       Shared memory object handle.
 * @param[in]  offset  Offset of the mapping from the beginning of the object.
 * @param[in]  length  Length of the mapping.
 *
 * @return     Virtual address of the mapping or NULL and error code in `errno`.
 */
void *shmem_map(SHMEM h, off_t offset, size_t length);

/**
 * Unmaps a mapping of a shared memory object mapped with `shmem_map`.
 *
 * If this function scceeds and there are no other handles and memory mappings
 * that refer to the underlying memory object in this or any other process, this
 * function will free the memory object making the underlying memory availalble
 * for new allocations. Hence, it is important to always call `shmem_unmap` when
 * the mapping is no longer needed.
 *
 * Only addresses returned by `shmem_map` should be passed as arguments to this
 * function, or it will fail with errno set to `EINVAL`.
 *
 * @param      addr  Virtual addresss of the mapping returned by `shmem_map`.
 *
 * @return     0 on success, otherwise -1 and error code in `errno`.
 */
int shmem_unmap(void *addr);

/**
 * Returns the memory object size and flags for a given handle.
 *
 * The memory object size is returned exactly as it was specified in a
 * `shmem_create` call that allocated the underlying memory object. The returned
 * flags correspond to the handle view in the calling process and may not match
 * the original flags specified when the handle was created.
 *
 * If some portion of the information is not needed, NULL may be passed instead
 * of an address of a corresponding variable to indicate that.
 *
 * @param[in]  h         Shared memory object handle.
 * @param      flags     Where to store the handle flags or NULL.
 * @param      size      Where to store the memory object size or NULL.
 * @param      act_size  Where to store the actual memory object size or NULL.
 *
 * @return     0 on success, otherwise -1 and error code in `errno`.
 */
int shmem_get_info(SHMEM h, int *flags, size_t *size, size_t *act_size);

/**
 * Returns the maximum number of distinct shared memory object handles this API
 * can ever return.
 *
 * @return     The maximum number of handles.
 */
size_t shmem_max_handles();

__END_DECLS

#endif /* LIBCX_SHMEM_H */
