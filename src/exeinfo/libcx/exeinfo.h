/*
 * Public exeinfo API.
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

#ifndef LIBCX_EXEINFO_H
#define LIBCX_EXEINFO_H

#include <sys/cdefs.h>

/**
 * Format of the executable file.
 */
typedef enum _exeinfo_format
{
  EXEINFO_FORMAT_UNKNOWN = 0, /**< Unknown executable format. */
#if 0 /* TODO later */
  EXEINFO_FORMAT_MZ = 1, /**< DOS 16-bit executable. */
  EXEINFO_FORMAT_NE = 2, /**< OS/2 and Windows 16-bit executable. */
#endif
  EXEINFO_FORMAT_LX = 3, /**< OS/2 LX 32-bit executable. */

  EXEINFO_FORMAT_INVALID = -1, /**< Invalid format (used as error indicator). */
}
EXEINFO_FORMAT;

/**
 * Resource object of the executable file.
 */
typedef struct _exeinfo_resource
{
  int type;
  int id;
  const char *data;
  int size;
}
EXEINFO_RESOURCE;

/**
 * Opaque executable file handle.
 *
 * Used by the exeinfo API functions to refer to an executable file opened with
 * #exeinfo_open().
 */
typedef struct _exeinfo *EXEINFO;

__BEGIN_DECLS

/**
 * Opens an executable file and prepares it for reading the file structure.
 *
 * The file must exist and will be opened in read-only mode. Note that the file
 * will remain open until #execinfo_open() is called.
 *
 * All `execinfo` functions use POSIX error codes to report failures. A special
 * meaning is put into EILSEQ which is returned when an error in the executable
 * file structure is detected that prevents the requrested operation. Note,
 * however, that if the format of the executable file is not recognized by
 * the #execinfo_open() call, it will report a success rather than EILSEQ but
 * #exeinfo_get_format() will return EXEINFO_FORMAT_UNKNOWN in this case. This
 * is because there are `execinfo` functions that don't require a particular
 * format and may work with any existing file.
 *
 * @param fname Executable file name.
 * @return An opaque file handle on success or NULL on failure. Sets @c errno
 * to a POSIX error code if NULL is returned.
 */
EXEINFO exeinfo_open(const char *fname);

/**
 * Returns the format of the executable file.
 *
 * @param info Handle to the executable file returned by #exeinfo_open().
 * @return One of EXEINFO_FORMAT values or EXEINFO_FORMAT_INVALID on failure.
 * Sets @c errno to a POSIX error code if EXEINFO_FORMAT_INVALID is returned.
 */
EXEINFO_FORMAT exeinfo_get_format(EXEINFO info);

/* TODO later
EXEINFO_RESOURCE *exeinfo_find_first_resource(EXEINFO info, int type);
EXEINFO_RESOURCE *exeinfo_find_next_resource(EXEINFO info);
*/

/**
 * Returns a pointer to the requested resource object's data.
 *
 * Valid resource type values are from 1 to 0xFFFE. Values from 1 to 255 are
 * reserved for predefinition. Values greater tthan 255 are user-defined
 * resource types. Well-known predefined resource types are accessible via RT_*
 * constants from <os2.h>. Valid resource identifier values are from 1 to
 * 0xFFFE.
 *
 * The returned data buffer is read-only and its size corresponds to the return
 * value of this function. The pointer to this data buffer is valid until
 * #exeinfo_close() is called on the associated executable file handle. If
 * access to the data buffer is needed after calling #exeinfo_close(), its
 * contents should be copied to a user buffer prior to this call.
 *
 * If @a data is NULL, only the size of the requested resource object's data
 * buffer is returned.
 *
 * If the requested resource is not found in the given executable file, this
 * function returns -1 and sets @c errno to ENOENT.
 *
 * @note See `DosGetResource` in CPREF for more information about predefined
 * resource types.
 *
 * @param info Handle to the executable file returned by #exeinfo_open().
 * @param type Resource type.
 * @param id Resource identifier.
 * @param data Address of the variable to receive a pointer to the resource data.
 * @return Size of the data buffer returned in @a data, in bytes or -1 on
 * failure. Sets @c errno to a POSIX error code if -1 is returned.
 */
int exeinfo_get_resource_data(EXEINFO info, int type, int id, const char **data);

/**
 * Closes the executable file opened with #exeinfo_open() and releases system
 * resources allocated for reading the file structure.
 *
 * @param info Handle to the executable file returned by #exeinfo_open().
 * @return 0 on success and -1 on failure. Sets @c errno to a POSIX error code
 * if -1 is returned.
 */
int exeinfo_close(EXEINFO info);

__END_DECLS

#endif /* LIBCX_EXEINFO_H */
