/*
 * network extensions for kLIBC.
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

#ifndef IFADDRS_H
#define IFADDRS_H

#include <stddef.h>

struct ifaddrs {
  struct ifaddrs   *ifa_next;         /* Pointer to next struct */
  char             *ifa_name;         /* Interface name */
  unsigned int     ifa_flags;         /* Interface flags */
  struct sockaddr  *ifa_addr;         /* Interface address */
  struct sockaddr  *ifa_netmask;      /* Interface netmask */
  union {
    struct sockaddr *ifu_broadaddr;   /* Broadcast address of interface */
    struct sockaddr *ifu_dstaddr;     /* Point-to-point destination address */
  } ifa_ifu;
#define              ifa_broadaddr ifa_ifu.ifu_broadaddr
#define              ifa_dstaddr   ifa_ifu.ifu_dstaddr
  void             *ifa_data;         /* Address specific data */
};

__BEGIN_DECLS

int getifaddrs(struct ifaddrs **ifap);
void freeifaddrs(struct ifaddrs *ifa);

__END_DECLS

#endif /* IFADDRS_H */
