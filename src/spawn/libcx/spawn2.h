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
#define P_2_XREDIR      0x10000000
#define P_2_XREDIR2     0x18000000

#define P_2_XFLAG_MASK  0xFF000000

#define P_2_MODE_MASK 0x0FF
#define P_2_TYPE_MASK 0xF00

/**
 * Starts a child process using the executable specified in @a name.
 *
 * Program arguments are specified in @a argv whose last element must be NULL.
 * The first element of this array should be the program name, by convention,
 * and must always be non-NULL. The current working directory of the new process
 * is specified with @a cwd. NULL will cause the new process to inherit the
 * working directory of the current process. The environment of the new process
 * is either given in the @a envp array whose last element must be NULL, or
 * inherited from the current process if @a envp is NULL.
 *
 * If @a envp is not NULL, three special pseudo-environment variables are
 * recognized and processed accordingly using the DosSetExtLIBPATH API:
 * BEGINLIBPATH, ENDLIBPATH and LIBPATHSTRICT. The previous values of these
 * variables are saved and restored after starting the child process. Note that
 * the length limit for BEGINLIBPATH and ENDLIBPATH values is 1023 characters.
 * If this limit is exceeded (or if DosQueryExtLIBPATH or DosSetExtLIBPATH
 * fails), this function returns -1 and sets errno to EOVERFLOW. Note that all
 * pseudo-environment variables are omitted from the child environment as they
 * could confuse started programs if passed on.
 *
 * Note that it's a responsibility of the caller to make sure that @a envp does
 * not contain duplicate variables. If it does, it's undefined which one will be
 * seen by the child process. Also note that all variables should be given as
 * `VAR=VAL` strings where both `VAL` and `=` are optional. Variables without
 * `=` will be silently ignored unless mode contains P_2_APPENDENV in which case
 * special processing will take place (see below).
 *
 * The @a stdfds argument, when not NULL, allows the current process to set up
 * file handle redirection and inheritance for the child process. It has two
 * modes: simple mode (the default one) and extended mode (activated by
 * P_2_XREDIR @a mode flag). Interpretation of the @a stdfds array differs
 * depending on the mode. If @a stdfds is NULL, no redirection takes place
 * regardless of the mode and the child process will simply inherit standard I/O
 * handles of its parent.
 *
 * If the current process only wants to redirect standard I/O streams of the
 * child process, it should use simple mode and specify the new handles in the
 * @a stdfds array. This array, if not NULL, must contain exactly 3 elements
 * that represent LIBC file descriptors to be used by the child's stdin, stdout
 * and stderr streams, respectively. Some integer values have a special meaning
 * when used as certain array elements' values:
 *
 * - 0 in any element indicates that the respective stream should not be
 *   redirected but instead inherited from the parent.
 *
 * - 1 in stdfds[2] indicates that the stderr stream should be redirected to the
 *   stdout stream (or to where stdout is redirected by stdfds[1] if it's not
 *   0). Putting 1 in stdfds[1] is equivalent to putting 0 there (see above).
 *   Putting 1 in stdfds[0] will result in EINVAL.
 *
 * - 2 in stdfds[1] indicates that the stdout stream should be redirected to the
 *   stderr stream (or to where stderr is redirected by stdfds[2] if it's not
 *   0). Putting 2 in stdfds[2] is equivalent to putting 0 there (see above).
 *   Putting 2 in stdfds[0] will result in EINVAL.
 *
 * If the current process needs a more complex redirection and inheritance setup
 * for the child, it should specify P_2_XREDIR in @a mode and @a stdfds will be
 * interpreted as sequence of integer pairs ending with a special value of -1
 * that indicates the end of the sequence. Integers in each pair represent LIBC
 * file descriptors where the first one is an open file descriptor of the
 * current process to be inherited by the child process and the second one is a
 * file descriptor the inherited one should appear as in the child process. This
 * scheme allows for both selective inheritance and complex redirection.
 *
 * If both values in the pair match, the specified file descriptor will be
 * simply inherited by the child process and will appear there under the same
 * number. If the second value is different, it specifies a new file descriptor
 * that will represent the file descriptor from the first value in the child
 * process, effectively enabling I/O stream redirection. Note that as opposed to
 * simple mode where values of 0, 1 and 2 are given a special meaning, in
 * P_2_XREDIR mode the first value in the pair always represents an open file
 * descriptor of the current process while the second value always represents a
 * new file descriptor in the child process that will be opened to represent the
 * specified file descriptor of its parent.
 *
 * All file descriptors specified in the second value of each pair (i.e. the
 * ones to be opened in the child process) must be unique. Having a duplicate
 * there will result in EINVAL from `spawn2`. File descriptors in the first
 * value of each pair (i.e. the current process ones) must represent open files
 * and may occur more than once: this allows to redirect the same parent file to
 * more than one child descriptors (e.g. combine child stdout and stderr into
 * one stream). Specifying a non-existent file descriptor as the first value in
 * the pair will result in EBADF.
 *
 * Here is an example of redirecting both child stdout and stderr streams to the
 * current process' file descriptor no. 5 using simple mode:
 * ```
 * int stdfds[] = { 0, 5, 1 };
 * ```
 * and using extended (P_2_XREDIR) mode:
 * ```
 * int stdfds[] = { 5, 1, 5, 2, -1 };
 * ```
 *
 * Note that the second case will also cause the current process' stdin to be
 * not inherited by the child, as opposed to the first one. In order to inherit
 * it, a pair of 0, 0 should be added somewhere in the array.
 *
 * Note that in P_2_XREDIR mode P_2_NOINHERIT is always implied. I.e. only file
 * descriptors explicitly specified in @a stdfds will be inherited by the child
 * process from the current process and nothing else. This differs from simple
 * I/O redirection mode which forces inheritance of standard I/O file
 * descriptors with values 0, 1, 2 but does not touch other descriptors (so that
 * they will be inherited depending on their individual settings via opening
 * files in O_NOINHERIT mode or using `fcntl(F_SETFD, FD_CLOEXEC)`).
 *
 * In addition to the standard `P_*` constants for the @a mode argument, the
 * following new values are recognized:
 *
 * - P_2_APPENDENV causes `spawn2` to append the environment passed in @a envp
 *   to a copy of the current process environment and pass the result to the
 *   child process instead of initializing the child environment to only what is
 *   contained in @a envp (which might be not enough to start a program if it
 *   contains only a few custom variables). If a variable in @a envp does not
 *   contain an `=` character, it will be removed from the copy of the current
 *   process environment instead of being appended to it. This allows to
 *   effectively hide specific parent variables from the child process. Note
 *   that if P_2_APPENDENV is not given, such variables in @a envp will be
 *   silently ignored (not passed to the child in any form).
 *
 * - P_2_NOINHERIT causes `spawn2` to prevent inheritance of all file
 *   descriptors of the current process by the child process. Note that due to
 *   kLIBC limitations, this flag will cause enumeration of 65k file descriptors
 *   and requires extra 8k of heap memory. Instead, it's recommended to either
 *   open files in O_NOINHERIT mode or use `fcntl(F_SETFD, FD_CLOEXEC)` on
 *   specific file descriptors to prevent their inheritance. Note that file
 *   descriptors 0, 1 and 2 are always inherited by the child process.
 *
 * - P_2_THREADSAFE causes `spawn2` to use an intermediate process for starting
 *   the child process. This allows to avoid race conditions in multithreaded
 *   applications when `spawn2` manipulates global process properties such as
 *   the current working directory or file descriptors (by changing their
 *   inheritance and association with an actual device) because the current
 *   process remains unaffected by such manipulations. Although it's not
 *   enforced, using this flag makes sense only when at least one of the
 *   following is true: P_2_NOINHERIT is used, @a cwd is not NULL, @a stdfds is
 *   not NULL and contains at least one non-zero file descriptor. In other cases
 *   `spawn2` should be thread-safe without using P_2_THREADSAFE. P_2_THREADSAFE
 *   can only be used with P_WAIT, P_NOWAIT, P_SESSION or P_PM and will result
 *   in a failure with EINVAL otherwise.
 *
 * - P_2_XREDIR (added in version 0.6.7) enables extended redirection mode that
 *   changes interpretation of the @a stdfds argument (which must be not NULL in
 *   this case). The extended redirection mode is described in detail above.
 *
 * - P_2_XREDIR2 (added in version 0.7.5) behaves exactly like P_2_XREDIR but it
 *   does not imply P_2_NOINHERIT. This flag should be rarely needed because
 *   extended redirection implies full control over inheritance.
 *
 * Note that if you issue a thread-unsafe `spawn2` call without P_2_THREADSAFE,
 * then you should provide thread safety on your own (by using synchronization
 * primitives etc). Otherwise it may result in unexpected behavior.
 *
 * This function returns the return value of the child process (P_WAIT), or the
 * process ID of the child process (P_NOWAIT, P_SESSION and P_PM), or zero
 * (P_UNRELATED) if successful. On error it returns -1 and sets `errno` to the
 * respective error code.
 *
 * Note that since version 0.6.4, `spawn2` will return a process ID of a final
 * process (which will be its grand-child), not the intermediate wrapper (its
 * direct child) in P_2_THREADSAFE mode. This allows to use APIs like
 * `DosGiveSharedMem` to communicate with the final process. Note that the
 * `waitpid` family (and even `DosWaitChild`) provided by LIBCx is able to
 * handle this situation and will work correctly despite the fact that waiting
 * on non-immediate children is not "officially" possible on OS/2.
 *
 * ALso note that when P_2_THREADSAFE is used together with P_SESSION and
 * P_UNRELATED, `spawn2` will return a PID of a started process instead of zero
 * even though this process is completely unrelated to the caller in terms of
 * the parent-child relationship. This is an unique LIBCx feature not available
 * in EMX or in `DosStartSession`. If P_2_THREADSAFE is not used, zero will
 * still be returned.
 *
 * Except for what is described above, the `spawn2` function behaves essentially
 * the same as the standard `spawnvpe` function in the EMX library. Please
 * consult the `spawnvpe` manual for more information.
 */
int spawn2(int mode, const char *name, const char * const argv[],
           const char *cwd, const char * const envp[], const int stdfds[]);

__END_DECLS

#endif /* LIBCX_SPAWN2_H */
