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

#ifndef LIBCX_NET_H
#define LIBCX_NET_H

/*
 * Definitions that originally belong to <netdb.h>
 */

#include <stddef.h>

/* GAI error codes */
#define EAI_BADFLAGS    -1
#define EAI_NONAME      -2
#define EAI_AGAIN       -3
#define EAI_FAIL        -4
#define EAI_NODATA      -5
#define EAI_FAMILY      -6
#define EAI_SOCKTYPE    -7
#define EAI_SERVICE     -8
#define EAI_ADDRFAMILY  -9
#define EAI_MEMORY      -10
#define EAI_OVERFLOW    -11
#define EAI_SYSTEM      -12

#define NI_NUMERICHOST 0x01
#define NI_NUMERICSERV 0x02
#define NI_NOFQDN      0x04
#define NI_NAMEREQD    0x08
#define NI_DGRAM       0x10

#define AI_PASSIVE     0x0001
#define AI_CANONNAME   0x0002
#define AI_NUMERICHOST 0x0004
#define AI_NUMERICSERV 0x0400

typedef int socklen_t;

struct addrinfo
{
    int ai_flags;
    int ai_family;
    int ai_socktype;
    int ai_protocol;
    size_t ai_addrlen;
    struct sockaddr *ai_addr;
    char *ai_canonname;
    struct addrinfo *ai_next;
};

__BEGIN_DECLS

const char *gai_strerror (int errnum);
int getaddrinfo (const char *node, const char *service,
                 const struct addrinfo *hints, struct addrinfo **res);
void freeaddrinfo (struct addrinfo *res);
int getnameinfo (const struct sockaddr *sa, socklen_t salen, char *host,
                 int hostlen, char *serv, int servlen, int flags);

__END_DECLS

#endif // LIBCX_NET_H
