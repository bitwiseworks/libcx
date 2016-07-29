/*
 * Testcase for general functionality.
 * Copyright (C) 2016 bww bitwise works GmbH.
 * This file is part of the kLIBC Extension Library.
 * Authored by Dmitry Kuminov <coding@dmik.org>, 2016.
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

#include "shared.h"

static int do_test(void);
#define TEST_FUNCTION do_test()
#include "test-skeleton.c"

static void check_mem(int nr, size_t hdr_size, size_t expected)
{
  int rc;
  _HEAPSTATS hst;

  if (gpData->size != expected)
  {
    printf("check_mem %d: Committed size is %d, not %d",
           nr, gpData->size, expected);
    exit(1);
  }

  rc = _ustats(gpData->heap, &hst);
  if (rc)
  {
    printf("check_mem %d: _ustats failed: %s", nr, strerror(errno));
    exit(1);
  }

  if (gpData->size - hdr_size != hst._provided)
  {
    printf("check_mem %d: Total heap size %d mismatches commiited "
           "size %d (must be %d)\n",
           nr, hst._provided, gpData->size, gpData->size - hdr_size);
    exit(1);
  }
}

static int do_test(void)
{
  int rc;
  _HEAPSTATS hst;
  size_t hdr_size;

  global_lock();

  rc = _ustats(gpData->heap, &hst);
  if (rc)
  {
    perror("_ustats failed");
    return 1;
  }

  if (hst._provided > gpData->size)
  {
    printf("Total heap size %d is greater than commiited size %d\n",
           hst._provided, gpData->size);
    return 1;
  }

  hdr_size = gpData->size - hst._provided;

  check_mem(1, hdr_size, 65536);

  void *data = global_alloc(55000);

  check_mem(2, hdr_size, 65536);

  void *data2 = global_alloc(5000);

  check_mem(3, hdr_size, 65536 * 2);

  void *data3 = global_alloc(5000);

  check_mem(4, hdr_size, 65536 * 2);

  void *data4 = global_alloc(5000);

  check_mem(5, hdr_size, 65536 * 2);

  global_free(data4);
  global_free(data3);
  global_free(data2);
  global_free(data);

  global_unlock();

  return 0;
}
