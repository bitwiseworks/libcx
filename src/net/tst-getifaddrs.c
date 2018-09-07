/*
 * Testcase for getifaddrs API.
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

#include <sys/socket.h>
#include <netinet/in.h>
#include <net/if.h>
#include <net/if_dl.h>

#include <os2.h>

#include "libcx/net.h"

static int do_test(void);
#define TEST_FUNCTION do_test()
#include "../test-skeleton.c"

/*
 * This is a simplified (AF_INET only) and sligntly extended version of the
 * example from  http://man7.org/linux/man-pages/man3/getifaddrs.3.html.
 */

static int do_test(void)
{
  struct ifaddrs *ifaddr, *ifa;
  int family, s, n;
  char host[NI_MAXHOST];

  if (getifaddrs(&ifaddr) == -1) {
      perrno_and(return 1, "getifaddrs");
  }

  /* Walk through linked list, maintaining head pointer so we
     can free list later */

  for (ifa = ifaddr, n = 0; ifa != NULL; ifa = ifa->ifa_next, n++) {
      if (ifa->ifa_addr == NULL)
          continue;

      family = ifa->ifa_addr->sa_family;

      /* Display interface name and family (including symbolic
         form of the latter for the common families) */

      printf("%-8s %s (%d)\n",
             ifa->ifa_name,
             (family == AF_LINK) ? "AF_LINK" :
             (family == AF_INET) ? "AF_INET" : "???",
             family);

      /* For an AF_INET* interface address, display the address */

      if (family == AF_INET) {
          s = getnameinfo(ifa->ifa_addr, sizeof(struct sockaddr_in),
                  host, NI_MAXHOST,
                  NULL, 0, NI_NUMERICHOST);
          if (s != 0) {
              printf("getnameinfo() failed: %s\n", gai_strerror(s));
              exit(EXIT_FAILURE);
          }
          printf("\t\taddress: <%s>\n", host);
          s = getnameinfo(ifa->ifa_netmask, sizeof(struct sockaddr_in),
                  host, NI_MAXHOST,
                  NULL, 0, NI_NUMERICHOST);
          if (s != 0) {
              printf("getnameinfo() failed: %s\n", gai_strerror(s));
              exit(EXIT_FAILURE);
          }
          printf("\t\tnetmask: <%s>\n", host);
          if (ifa->ifa_flags & IFF_BROADCAST) {
            s = getnameinfo(ifa->ifa_broadaddr, sizeof(struct sockaddr_in),
                    host, NI_MAXHOST,
                    NULL, 0, NI_NUMERICHOST);
            if (s != 0) {
                printf("getnameinfo() failed: %s\n", gai_strerror(s));
                exit(EXIT_FAILURE);
            }
            printf("\t\tbroadcast address: <%s>\n", host);
          } else if (ifa->ifa_flags & IFF_POINTOPOINT) {
            s = getnameinfo(ifa->ifa_dstaddr, sizeof(struct sockaddr_in),
                    host, NI_MAXHOST,
                    NULL, 0, NI_NUMERICHOST);
            if (s != 0) {
                printf("getnameinfo() failed: %s\n", gai_strerror(s));
                exit(EXIT_FAILURE);
            }
            printf("\t\tP2P address: <%s>\n", host);
          }
      } else if (family == AF_LINK) {
          struct sockaddr_dl * dl = (struct sockaddr_dl *)ifa->ifa_addr;
          printf("\t\tinterface index: <%d>\n", dl->sdl_index);
          printf("\t\tinterface type: <0x%x>\n", dl->sdl_type);
          printf("\t\tinterface name: <%.*s>\n", dl->sdl_nlen, dl->sdl_data);
          if (dl->sdl_alen) {
              printf("\t\tlink address: <");
              int n;
              for (n = 0; n < dl->sdl_alen; ++n)
                  printf("%02X%s", (unsigned char) *(dl->sdl_data + dl->sdl_nlen + n),
                         n == dl->sdl_alen - 1 ? ">\n" : ":");
          }
      }
      printf("\t\tflags: <0x%x>\n", ifa->ifa_flags);
  }

  freeifaddrs(ifaddr);

  return 0;
}
