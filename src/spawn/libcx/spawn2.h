/*
 * Public spawn2 API.
 * Copyright (C) 2017 bww bitwise works GmbH.
 * This file is part of the kLIBC Extension Library.
 * Authored by Dmitry Kuminov <coding@dmik.org>, 2017.
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

#ifndef LIBCX_SPAWN2_H
#define LIBCX_SPAWN2_H

#include <sys/cdefs.h>

#include <sys/process.h>

__BEGIN_DECLS

#define P_2_NOINHERIT 0xF0000000

/**
 * Starts a child process using the executable specified in @a name.
 *
 * Program arguments are specified in @a argv whose last element must be NULL.
 * The filst element of this array should be the program name, by convention,
 * and must always be non-NULL. The current working directory of the new
 * process is specified with @a cwd. NULL will casuse the new process to
 * inherit the working directory of the starting process. The environment of
 * the new process is either given in the @a envp array whose last element must
 * be NULL, or inherited from the starting process if @a envp is NULL.

 * If the starting process wants to redirect standard I/O streams of the child
 * process, it should specify the new handles in the @a stdfds array (which
 * otherwise should be NULL to indicate that the child process should inherit
 * standard I/O handles of its parent). This array, if not NULL, must contain 3
 * elements that represent LIBC file descriptors to be used by the child's
 * stdin, stdout and stderr streams, respectively. Some integer values have a
 * special meaning when used in this array and therefore these values are never
 * interpreted as file descriptors:
 *
 * - 0 indicates that the respective stream should not be redirected but
 * instead inherited from the parent.
 *
 * - 1 indicates that the stderr stream should be redirected to the stdout
 * stream. A value of 1 can only be put in the last element of @a stdfds (with
 * an index of 2) that represents the stderr stream, and is invalid otherwise.
 *
 * - 2 is reserved for future use and is currently interpreted as an invalid
 * value.
 *
 * In addition to the standard `P_*` constanst for the @a mode argument, the
 * following new values are recognized:
 *
 * - P_2_NOINHERIT causes `spawn2` to prevent inheritance of all file
 * descriptors of the starting process by the child process. Note that due to
 * kLIBC limitations, this flag will cause enumeration of 65k file descriptors
 * and require extra 8k of heap memory. Instead, it's recommended to either
 * open files in O_NOINHERIT mode or use `fcntl(F_SETFD, FD_CLOEXEC)` on
 * specific file descrptors to prevent their inheritance. Note that file
 * descriptors 0, 1 and 2 are always inherited by the child process.
 *
 * Note that to guarantee full thread safety, only one thread may execute
 * `spawn2` at a time, all other threads of the calling process will be blocked
 * for the duration of the call. For this reason using the P_WAIT, P_OVERLAY or
 * P_DETACH modes is not allowed with `spawn2` and will immediately result in
 * an error with `errno` set to E_INVAL.
 *
 * The function returns the process ID of the child process (P_NOWAIT, P_DEBUG,
 * P_SESSION and P_PM), or zero (P_UNRELATED) if successful. On error it
 * returns -1 and sets `errno` to the respective error code.
 *
 * Except for what is described above, the `spawn2` function behaves
 * essentially the same as the standard `spawnvpe` LIBC function. Please
 * consult the `spawnvpe` manual for more information
 */
int spawn2(int mode, const char *name, const char * const argv[],
           const char *cwd, const char * const envp[], int stdfds[3]);

__END_DECLS

#endif /* LIBCX_SPAWN2_H */
