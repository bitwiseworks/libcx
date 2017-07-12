/*
 * Testcase for exeinfo API.
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

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <os2.h>

#include "libcx/exeinfo.h"

#include "tst-exeinfo-rc.h"

static int do_test(void);
#define TEST_FUNCTION do_test()
#include "../test-skeleton.c"

static int do_test(void)
{
  char fname[PATH_MAX + 16 /* "packed-N */];
  if (_execname(fname, PATH_MAX) == -1)
    perrno_and(return 1, "_execname");

  char *fext = _getext(fname);
  if (!fext)
    perrno_and(return 1, "_getext");

  EXEINFO info;
  EXEINFO_FORMAT fmt;

  int i;
  int len;
  const char *data;

  for (i = 1; i <= 3; ++i)
  {
    if (i == 2)
      strcpy(fext, "-packed-1.exe");
    else if (i == 3)
      strcpy(fext, "-packed-2.exe");

    printf("Test %d.open\n", i);

    info = exeinfo_open(fname);
    if (!info)
      perrno_and(return 1, "exeinfo_open");

    fmt = exeinfo_get_format(info);
    if (fmt != EXEINFO_FORMAT_LX)
      perr_and(return 1, "exeinfo_get_format is %d and not EXEINFO_FORMAT_LX (%d)",
               fmt, EXEINFO_FORMAT_LX);

    printf("Test %d.1\n", i);

    len = exeinfo_get_resource_data (info, RT_RCDATA, ID_NON_EXISTENT, &data);
    if (len != -1 || errno != ENOENT)
      perr_and(return 1,
               "exeinfo_get_resource_data(RT_RCDATA, %d) shoud fail with -1 and ENOENT (%d) "
               "but it returns %d and errno %d",
               ID_NON_EXISTENT, ENOENT, len, errno);

    printf("Test %d.2\n", i);

    len = exeinfo_get_resource_data (info, RT_RCDATA, ID_1, &data);
    if (len == -1)
      perrno_and(return 1, "exeinfo_get_resource_data");
    if (len != sizeof(ID_1_DATA))
      perr_and(return 1, "ID_1_DATA size should be %d, got %d", sizeof(ID_1_DATA), len);
    if (strncmp(data, ID_1_DATA, len) != 0)
      perr_and(return 1, "ID_1 data should be [%s], got [%s]", ID_1_DATA, data);

    printf("Test %d.3\n", i);

    len = exeinfo_get_resource_data (info, RT_RCDATA, ID_2, &data);
    if (len == -1)
      perrno_and(return 1, "exeinfo_get_resource_data");
    if (len != sizeof(ID_2_DATA))
      perr_and(return 1, "ID_2_DATA size should be %d, got %d", sizeof(ID_2_DATA), len);
    if (strncmp(data, ID_2_DATA, len) != 0)
      perr_and(return 1, "ID_2 data should be [%s], got [%s]", ID_2_DATA, data);

    printf("Test %d.close\n", i);

    if (exeinfo_close(info) == -1)
      perrno_and(return 1, "exeinfo_close");
  }

  return 0;
}
