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

  printf("check mem %d\n", nr);

  if (gpData->size != expected)
  {
    printf("check_mem %d: Committed size is %d, not %d\n",
           nr, gpData->size, expected);
    exit(1);
  }

  rc = _ustats(gpData->heap, &hst);
  if (rc)
  {
    printf("check_mem %d: _ustats failed: %s\n", nr, strerror(errno));
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

  printf("hst._provided %u\n", hst._provided);
  printf("hst._used %u\n", hst._used);
  printf("hst._max_free %u\n", hst._max_free);

  if (hst._provided > gpData->size)
  {
    printf("Total heap size %d is greater than committed size %d\n",
           hst._provided, gpData->size);
    return 1;
  }

  hdr_size = gpData->size - hst._provided;

  printf("hdr_size %u\n", hdr_size);

  check_mem(1, hdr_size, 65536);

  void *data = global_alloc(hst._max_free);

  check_mem(2, hdr_size, 65536);

  void *data2 = global_alloc(5000);

  check_mem(3, hdr_size, 65536 * 2);

  free(data2);
  free(data);

  global_unlock();

  return 0;
}
