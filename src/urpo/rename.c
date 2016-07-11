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

#include <string.h>
#include "shared.h"
#include "urpo.h"


/**
 * Renames a file or directory.
 *
 * @returns 0 on success.
 * @returns Negative error code (errno.h) on failure.
 * @param   pszPathOld      Old file path.
 * @param   pszPathNew      New file path.
 * @param   force           force adding for all kind of errors
 *
 * @remark OS/2 doesn't preform the deletion of the pszPathNew atomically.
 */
int _rename(const char *pszPathOld, const char *pszPathNew, int force)
{
    int rc = _std_rename(pszPathOld, pszPathNew);
    if (!rc)
        return 0;

    // store pending operation?
	if (errno == EBUSY || errno == ETXTBSY || force)
    {
	int	rc2, save_errno;
	char szNativePathOld[PATH_MAX];
	char szNativePathNew[PATH_MAX];

	save_errno = errno;

	// Resolve the paths.
	rc2 = realpath(pszPathOld, szNativePathOld);
	if (!rc2)
	{
	    errno = save_errno;
	    return rc;
	}

	rc2 = realpath(pszPathNew, szNativePathNew);
	if (rc2 == -1 && errno != ENOENT)
	{
	    errno = save_errno;
	    return rc;
	}

	// add to shared list
	rc2 = uropAdd( szNativePathOld, szNativePathNew);
	if (!rc2)
	{
		// done, reset error
		rc = 0;
		errno = 0;
	}
    }

    // done
    return rc;
}

/**
 * Renames a file or directory, do not force renaming (default)
 *
 * @returns 0 on success.
 * @returns Negative error code (errno.h) on failure.
 * @param   pszPathOld      Old file path.
 * @param   pszPathNew      New file path.
 *
 * @remark OS/2 doesn't preform the deletion of the pszPathNew atomically.
 */
int rename(const char *pszPathOld, const char *pszPathNew)
{
	return _rename( pszPathOld, pszPathNew, 0);
}

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
int renameForce(const char *pszPathOld, const char *pszPathNew)
{
	return _rename( pszPathOld, pszPathNew, 1);
}
