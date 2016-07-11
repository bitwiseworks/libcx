/*
 * urpo: unlink rename pending operation
 * A library to allow unlink/rename of open files.
 *
 * Copyright (C) 2008 Yuri Dario <mc6530@mclink.it>
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

#include <stdio.h>
#include <string.h>
#include "shared.h"

/**
 * Unlinks a file, directory, symlink, dev, pipe or socket.
 *
 * @returns 0 on success.
 * @returns Negative error code (errno.h) on failure.
 * @param   pszPath         Path to the filesystem file/dir/symlink/whatever to remove.
 */
int unlink(const char *pszPath)
{
    int rc;

    rc = _std_unlink( pszPath);
    if (!rc)
	return rc;
#ifdef DEBUG
    fprintf(stderr, "_std_unlink %s rc=%d errno=%d\n", pszPath, rc, errno);
#endif

    // store pending operation?
    if (errno == EACCES)
    {
	int	rc2, save_errno;
	char szNativePath[PATH_MAX];

	save_errno = errno;

	// Resolve the paths.
	if (!realpath(pszPath, szNativePath))
	{
#ifdef DEBUG
	    fprintf(stderr, "realpath %s errno=%d\n", pszPath, errno);
#endif
	    errno = save_errno;
	    return rc;
	}

	// add to shared list
	rc2 = uropAdd( szNativePath, NULL);
	if (!rc2)
	{
		// done, reset error
		rc = 0;
		errno = 0;
	} else
	{
#ifdef DEBUG
	    fprintf(stderr, "uropAdd %s failed=%d\n", szNativePath, rc2);
#endif
	}
        
    }

    // done
    return rc;
}

/**
 * Removes a directory.
 *
 * @returns 0 on success.
 * @returns Negative error code (errno.h) on failure.
 * @param   pszPath         Path to the filesystem file/dir/symlink/whatever to remove.
 */
int rmdir(const char *pszPath)
{
	int rc;

	rc = _std_rmdir( pszPath);
	if (!rc)
		return rc;

#ifdef DEBUG
	fprintf(stderr, "_std_rmdir %s rc=%d errno=%d\n", pszPath, rc, errno);
#endif

	// store pending operation?
	if (errno == EACCES || errno == ENOTEMPTY)
	{
		int	rc2, save_errno;
		char szNativePath[PATH_MAX];

		save_errno = errno;

		// Resolve the paths.
		if (!realpath(pszPath, szNativePath)) {
#ifdef DEBUG
			fprintf(stderr, "realpath %s errno=%d\n", pszPath, errno);
#endif
			errno = save_errno;
			return rc;
		}

		// add to shared list
		rc2 = uropAdd( szNativePath, NULL);
		if (!rc2) {
			// done, reset error
			rc = 0;
			errno = 0;
		} else {
#ifdef DEBUG
			fprintf(stderr, "uropAdd %s failed=%d\n", szNativePath, rc2);
#endif
		}

	}

	// done
	return rc;
}

/**
 * Remove a file or directory. Mapped to unlink() implementation.
 *
 * @returns 0 on success.
 * @returns Negative error code (errno.h) on failure.
 * @param   pszPath         Path to the filesystem file/dir/symlink/whatever to remove.
 */
int remove(const char *pszPath)
{
    return unlink( pszPath);
}
