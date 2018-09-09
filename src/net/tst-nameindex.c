/*
 * Testcase for if_nameindex API.
 * Copyright (C) 2018 bww bitwise works GmbH.
 * This file is part of the kLIBC Extension Library.
 * Authored by Dmitry Kuminov <coding@dmik.org>, 2018.
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

#include "libcx/net.h"

static int do_test(void);
#define TEST_FUNCTION do_test()
#include "../test-skeleton.c"

/*
 * This is a sligntly extended version of the example from
 * http://man7.org/linux/man-pages/man3/if_nameindex.3.html.
 */

static int do_test(void)
{
  printf("Test 1\n");

  struct if_nameindex *if_ni, *i;
  int n, cnt = 0;

  if_ni = if_nameindex();
  if (if_ni == NULL) {
    perrno_and(return 1, "if_nameindex");
  }

  for (i = if_ni; ! (i->if_index == 0 && i->if_name == NULL); i++, cnt++) {
    printf("%u: %s\n", i->if_index, i->if_name);
    if (i->if_index == 0)
      perr_and(return 1, "if_index = 0, must be greater");
    if (i->if_name == NULL)
      perr_and(return 1, "if_name = NULL, must be non-NULL");
  }

  printf("Test 2\n");

  for (n = 0; n < cnt; n ++) {
    int idx = if_nametoindex(if_ni[n].if_name);
    if (idx == 0)
      perrno_and(return 1, "if_nametoindex");
    if (if_ni[n].if_index != idx)
      perr_and(return 1, "if_nametoindex for [%s] is %d instead of %d", if_ni[n].if_name, idx, if_ni[n].if_index);
    char buf[IF_NAMESIZE];
    char *name = if_indextoname(if_ni[n].if_index, buf);
    if (name == NULL)
      perrno_and(return 1, "if_indextoname");
    if (strcmp(if_ni[n].if_name, name) != 0)
      perr_and(return 1, "if_indextoname for %d is [%s] instead of [%s]", if_ni[n].if_index, name, if_ni[n].if_name);
  }

  if_freenameindex(if_ni);

  return 0;
}
