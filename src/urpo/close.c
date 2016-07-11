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

#include <errno.h>
#include <io.h>
#include <stdio.h>

int close(int fh)
{
    int rc, save_errno;
    rc = _std_close(fh);
    save_errno = errno;
    uropPending();
    errno = save_errno;
    return rc;
}

int fclose(FILE *stream)
{
    int rc, save_errno;
    rc = _std_fclose(stream);
    save_errno = errno;
    uropPending();
    errno = save_errno;
    return rc;
}

