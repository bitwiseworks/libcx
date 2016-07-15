/*
 * urpo: unlink rename pending operation
 * A library to allow unlink/rename of open files.
 *
 * Copyright (C) 2008-15 Yuri Dario <yd@os2power.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#ifndef __URPO_H__
#define __URPO_H__

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Unlinks a file, directory, symlink, dev, pipe or socket.
 *
 * If unlinking fails due to EACCESS, the operation is deferred until
 * some files are closed or the process is terminated.
 *
 * @returns 0 on success.
 * @returns Negative error code (errno.h) on failure.
 * @param   pszPath         Path to the filesystem file/dir/symlink/whatever to remove.
 */
int unlink_deferred(const char *pszPath);

/**
 * Removes a directory.
 *
 * If removing fails due to EACCESS or ENOTEMPTY, the operation is deferred until
 * some files are closed or the process is terminated.
 *
 * @returns 0 on success.
 * @returns Negative error code (errno.h) on failure.
 * @param   pszPath         Path to the filesystem directory to remove.
 */
int rmdir_deferred(const char *pszPath);

/**
 * Remove a file or directory. Mapped to unlink() implementation.
 *
 * @returns 0 on success.
 * @returns Negative error code (errno.h) on failure.
 * @param   pszPath         Path to the filesystem file/dir/symlink/whatever to remove.
 */
int remove_deferred(const char *pszPath);

/**
 * Renames a file or directory, force renaming for all errors
 *
 * @returns 0 on success.
 * @returns Negative error code (errno.h) on failure.
 * @param   pszPathOld      Old file path.
 * @param   pszPathNew      New file path.
 *
 * @remark OS/2 doesn't preform the deletion of the pszPathNew atomically.
 */
int renameForce(const char *pszPathOld, const char *pszPathNew);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // __URPO_H__
