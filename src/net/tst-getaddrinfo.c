/*
 * Testcase for getaddrinfo API.
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

#include <sys/socket.h>
#include <netinet/in.h>

#include <os2.h>

#include "libcx/net.h"

static int do_test(void);
#define TEST_FUNCTION do_test()
#include "../test-skeleton.c"

static int do_test(void)
{
  const char *bad_hostname = "nonexistent.comcom";
  const char *hostname = "bitwiseworks.com";
  int addr = (216 << 24) + (239 << 16) + (38 << 8) + 21;

  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = PF_UNSPEC;
  hints.ai_flags = AI_CANONNAME;

  struct addrinfo *res = NULL;
  int rc = getaddrinfo(hostname, 0, &hints, &res);
  if (rc)
    perr_and(return 1, "getaddrinfo returned %d (%s) for [%s]", rc, gai_strerror(rc), hostname);
  if (!res)
    perr_and(return 1, "getaddrinfo returned null list for [%s]", hostname);

  struct sockaddr_in in_addr = *(struct sockaddr_in *)res->ai_addr;

#if 0
  struct sockaddr_in *in = (struct sockaddr_in *)res->ai_addr;
  if (ntohl(in->sin_addr.s_addr) != addr)
    perr_and(return 1, "getaddrinfo returned address %x != %x", ntohl(in->sin_addr.s_addr), addr);
#endif

  if (strcmp(res->ai_canonname, hostname))
    perr_and(return 1, "getaddrinfo returned canonname [%s] != [%s]", res->ai_canonname, hostname);

  while (res)
  {
    struct sockaddr_in *in = (struct sockaddr_in *)res->ai_addr;
    printf("Family %d\nAddresss %0x\nFlags %x\nSocktype %d\nProtocol %d\nCanonical name [%s]\n\n",
           res->ai_family,
           ntohl(in->sin_addr.s_addr),
           res->ai_flags,
           res->ai_socktype,
           res->ai_protocol,
           res->ai_canonname);
    res = res->ai_next;
  }

  freeaddrinfo(res);

  rc = getaddrinfo(bad_hostname, 0, NULL, &res);
  if (!rc)
    perr_and(return 1, "getaddrinfo returned success for [%s]", bad_hostname);

  printf("Result for [%s] is %d (%s)\n", bad_hostname, rc, gai_strerror(rc));

  struct sockaddr_in sa4;
  memset(&sa4, 0, sizeof(sa4));
  sa4.sin_family = AF_INET;
  sa4.sin_addr.s_addr = in_addr.sin_addr.s_addr;
  sa4.sin_port = 80;

  char hostbuf[NI_MAXHOST] = "";
  char servbuf[NI_MAXSERV] = "";
  rc = getnameinfo((struct sockaddr *)&sa4, sizeof(sa4), hostbuf, sizeof(hostbuf), servbuf, sizeof(servbuf), 0);
  if (rc)
    perr_and(return 1, "getnameinfo returned %d (%s) for %x", rc, gai_strerror(rc), ntohl(in_addr.sin_addr.s_addr));

  printf("host [%s] serv [%s]\n", hostbuf, servbuf);

  if (hostbuf[0] == '\0' || servbuf[0] == '\0')
    perr_and(return 1, "getnameinfo returned empty hostbuf or servbuf");

  return 0;
}
