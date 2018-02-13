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

#define P_2_NOINHERIT   0x80000000
#define P_2_THREADSAFE  0x40000000
#define P_2_APPENDENV   0x20000000

#define P_2_MODE_MASK 0xFF

/**
 * Starts a child process using the executable specified in @a name.
 *
 * Program arguments are specified in @a argv whose last element must be NULL.
 * The filst element of this array should be the program name, by convention,
 * and must always be non-NULL. The current working directory of the new
 * process is specified with @a cwd. NULL will casuse the new process to
 * inherit the working directory of the current process. The environment of
 * the new process is either given in the @a envp array whose last element must
 * be NULL, or inherited from the current process if @a envp is NULL.
 *
 * If the current process wants to redirect standard I/O streams of the child
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
 * - P_2_APPENDENV causes `spawn2` to append the environment passed in @a envp
 * to a copy of the current process environment and pass the result to the
 * child process instead of initializing the child environment to only what is
 * contained in @a envp (which might be not enoguh to start a program if it
 * contains only a few custom variables).
 *
 * - P_2_NOINHERIT causes `spawn2` to prevent inheritance of all file
 * descriptors of the current process by the child process. Note that due to
 * kLIBC limitations, this flag will cause enumeration of 65k file descriptors
 * and require extra 8k of heap memory. Instead, it's recommended to either
 * open files in O_NOINHERIT mode or use `fcntl(F_SETFD, FD_CLOEXEC)` on
 * specific file descrptors to prevent their inheritance. Note that file
 * descriptors 0, 1 and 2 are always inherited by the child process.
 *
 * - P_2_THREADSAFE causes `spawn2` to use an intermediate process for starting
 * the child process. This allows to avoid race conditions in multithreaded
 * applications when `spawn2` manipulates global process properties such as the
 * current working directory or file descriptors (by changing their inheritance
 * and association with an actual device) because the current process remains
 * unaffected by such manipulations. Although it's not enforced, using this
 * flag makes sense only when at least one of the following is true:
 * P_2_NOINHERIT is used, @a cwd is not NULL, @a stdfds is not NULL and
 * contains at least one non-zero file descriptor. In other cases `spawn2`
 * should be thread-safe without using P_2_THREADSAFE. P_2_THREADSAFE can only
 * be used with P_WAIT or P_NOWAIT and will result in a failure with EINVAL
 * otherwise.
 *
 * Note that if you issue a thread-unsafe `spawn2` call without P_2_THREADSAFE,
 * then you should provide thread safety on your own (by using syncnrhonization
 * primitives etc). Otherwise it may result in unexpected behavior.
 *
 * This function returns the return value of the child process (P_WAIT), or the
 * process ID of the child process (P_NOWAIT, P_DEBUG, P_SESSION and P_PM), or
 * zero (P_UNRELATED) if successful. On error it returns -1 and sets `errno` to
 * the respective error code.
 *
 * Note that if P_2_THREADSAFE is specified, the process ID of the intermediate
 * wrapper process is returned, not the process ID of the target executable
 * (which will be a child of that intermediate process). In most cases it
 * should not matter as the intermediate process redirects the execution result
 * of the final executable (including signals) to the current process in a
 * transparent way. The only known drawback is that it's impossible to use the
 * DosGiveSharedMem (and similar) API that requires an actual PID of the
 * target process to operate. This may be changed in the future.
 *
 * Except for what is described above, the `spawn2` function behaves
 * essentially the same as the standard `spawnvpe` LIBC function. Please
 * consult the `spawnvpe` manual for more information.
 */
int spawn2(int mode, const char *name, const char * const argv[],
           const char *cwd, const char * const envp[], int stdfds[3]);

__END_DECLS

#endif /* LIBCX_SPAWN2_H */
