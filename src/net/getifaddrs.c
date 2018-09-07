/*
 * getifaddrs() implementation for OS/2 kLIBC
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

/*
   Unix SMB/CIFS implementation.
   Samba utility functions
   Copyright (C) Andrew Tridgell 1998
   Copyright (C) Jeremy Allison 2007
   Copyright (C) Jelmer Vernooij <jelmer@samba.org> 2007

     ** NOTE! The following LGPL license applies to the replace
     ** library. This does NOT imply that all of Samba is released
     ** under the LGPL

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 3 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Library General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library; if not, see <http://www.gnu.org/licenses/>.
*/

#include "libcx/net.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <net/if.h>

#include <stdlib.h>
#include <string.h>

#include <unistd.h>
#include <stdio.h>
#include <sys/types.h>

#define HAVE_SOCKADDR_SA_LEN
#define rep_getifaddrs getifaddrs
#define rep_freeifaddrs freeifaddrs

#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif

#ifndef SIOCGIFCONF
#ifdef HAVE_SYS_SOCKIO_H
#include <sys/sockio.h>
#endif
#endif

void rep_freeifaddrs(struct ifaddrs *ifp)
{
	if (ifp != NULL) {
		free(ifp->ifa_name);
		free(ifp->ifa_addr);
		free(ifp->ifa_netmask);
		free(ifp->ifa_dstaddr);
		freeifaddrs(ifp->ifa_next);
		free(ifp);
	}
}

static struct sockaddr *sockaddr_dup(struct sockaddr *sa)
{
	struct sockaddr *ret;
	socklen_t socklen;
#ifdef HAVE_SOCKADDR_SA_LEN
	socklen = sa->sa_len;
#else
	socklen = sizeof(struct sockaddr_storage);
#endif
	ret = calloc(1, socklen);
	if (ret == NULL)
		return NULL;
	memcpy(ret, sa, socklen);
	return ret;
}

/****************************************************************************
this one is for AIX (tested on 4.2)
****************************************************************************/
int rep_getifaddrs(struct ifaddrs **ifap)
{
	char buff[8192];
	int fd, i;
	struct ifconf ifc;
	struct ifreq *ifr=NULL;
	struct ifaddrs *curif;
	struct ifaddrs *lastif = NULL;

	*ifap = NULL;

	if ((fd = socket(AF_INET, SOCK_DGRAM, 0)) == -1) {
		return -1;
	}

	ifc.ifc_len = sizeof(buff);
	ifc.ifc_buf = buff;

	if (ioctl(fd, SIOCGIFCONF, &ifc) != 0) {
		close(fd);
		return -1;
	}

	ifr = ifc.ifc_req;

	/* Loop through interfaces */
	i = ifc.ifc_len;

	while (i > 0) {
		unsigned int inc;

		inc = ifr->ifr_addr.sa_len;

		curif = calloc(1, sizeof(struct ifaddrs));
		if (lastif == NULL) {
			*ifap = curif;
		} else {
			lastif->ifa_next = curif;
		}

		curif->ifa_name = strdup(ifr->ifr_name);
		curif->ifa_addr = sockaddr_dup(&ifr->ifr_addr);
		curif->ifa_dstaddr = NULL;
		curif->ifa_data = NULL;
		curif->ifa_netmask = NULL;
		curif->ifa_next = NULL;

		if (ioctl(fd, SIOCGIFFLAGS, ifr) != 0) {
			goto fail;
		}

		curif->ifa_flags = (unsigned short)ifr->ifr_flags;

		/*
		 * The rest is requested only for AF_INET, for portability and
		 * compatiblilty (see e.g. https://linux.die.net/man/7/netdevice)
		 */

		if (curif->ifa_addr->sa_family == AF_INET) {
			if (ioctl(fd, SIOCGIFNETMASK, ifr) != 0) {
				goto fail;
			}

			curif->ifa_netmask = sockaddr_dup(&ifr->ifr_addr);
		}

		lastif = curif;

		/*
		 * Patch from Archie Cobbs (archie@whistle.com).  The
		 * addresses in the SIOCGIFCONF interface list have a
		 * minimum size. Usually this doesn't matter, but if
		 * your machine has tunnel interfaces, etc. that have
		 * a zero length "link address", this does matter.  */

		if (inc < sizeof(ifr->ifr_addr))
			inc = sizeof(ifr->ifr_addr);
		inc += IFNAMSIZ;

		ifr = (struct ifreq*) (((char*) ifr) + inc);
		i -= inc;
	}

	close(fd);
	return 0;

fail:
	close(fd);
	freeifaddrs(*ifap);
	return -1;
}
