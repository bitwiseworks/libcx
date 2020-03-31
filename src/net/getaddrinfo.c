/*
 * getaddrinfo() implementation for OS/2 kLIBC
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

/*-------------------------------------------------------------------------
 *
 * getaddrinfo.c
 *	  Support getaddrinfo() on platforms that don't have it.
 *
 * We also supply getnameinfo() here, assuming that the platform will have
 * it if and only if it has getaddrinfo().	If this proves false on some
 * platform, we'll need to split this file and provide a separate configure
 * test for getnameinfo().
 *
 * Copyright (c) 2003-2007, PostgreSQL Global Development Group
 *
 * Copyright (C) 2007 Jeremy Allison.
 * Modified to return multiple IPv4 addresses for Samba.
 *
 *-------------------------------------------------------------------------
 */

#include "libcx/net.h"

#include "../shared.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>

#define HOST_NAME_MAX NI_MAXHOST

#define rep_getaddrinfo getaddrinfo
#define rep_freeaddrinfo freeaddrinfo
#define rep_getnameinfo getnameinfo
#define rep_gai_strerror gai_strerror

#define size_t int

#ifndef SMB_MALLOC
#define SMB_MALLOC(s) malloc(s)
#endif

#ifndef SMB_STRDUP
#define SMB_STRDUP(s) strdup(s)
#endif


static int check_hostent_err(struct hostent *hp)
{
	if (!hp) {
		switch (h_errno) {
			case HOST_NOT_FOUND:
			case NO_DATA:
				return EAI_NONAME;
			case TRY_AGAIN:
				return EAI_AGAIN;
			case NO_RECOVERY:
			default:
				return EAI_FAIL;
		}
	}
	if (!hp->h_name || hp->h_addrtype != AF_INET) {
		return EAI_FAIL;
	}
	return 0;
}

static char *canon_name_from_hostent(struct hostent *hp,
				int *perr)
{
	char *ret = NULL;

	*perr = check_hostent_err(hp);
	if (*perr) {
		return NULL;
	}
	ret = SMB_STRDUP(hp->h_name);
	if (!ret) {
		*perr = EAI_MEMORY;
	}
	return ret;
}

static char *get_my_canon_name(int *perr)
{
	char name[HOST_NAME_MAX+1];

	_fmutex *fsem = global_tcpip_sem();
	ASSERT(_fmutex_request(fsem, _FMR_IGNINT) == 0);

	if (gethostname(name, HOST_NAME_MAX) == -1) {
		_fmutex_release(fsem);
		*perr = EAI_FAIL;
		return NULL;
	}
	/* Ensure null termination. */
	name[HOST_NAME_MAX] = '\0';
	char *ret = canon_name_from_hostent(gethostbyname(name), perr);

	_fmutex_release(fsem);

	return ret;
}

static char *get_canon_name_from_addr(struct in_addr ip,
				int *perr)
{
	_fmutex *fsem = global_tcpip_sem();
	ASSERT(_fmutex_request(fsem, _FMR_IGNINT) == 0);

	char *ret = canon_name_from_hostent(
			gethostbyaddr((char *)&ip, sizeof(ip), AF_INET),
			perr);

	_fmutex_release(fsem);

	return ret;
}

static int get_port_by_name(const char *service)
{
	struct servent	*port;		/* Port number for service */
	int		portnum;	/* Port number */

	if (!service)
		portnum = 0;
	else if (isdigit(*service & 255))
		portnum = atoi(service);
	else if ((port = getservbyname(service, NULL)) != NULL)
		portnum = ntohs(port->s_port);
	else if (!strcmp(service, "http"))
		portnum = 80;
	else if (!strcmp(service, "https"))
		portnum = 443;
	else if (!strcmp(service, "ipp") || !strcmp(service, "ipps"))
		portnum = 631;
	else if (!strcmp(service, "lpd"))
		portnum = 515;
	else if (!strcmp(service, "socket"))
		portnum = 9100;
	else
		portnum = 0;

	return portnum;
}

static struct addrinfo *alloc_entry(const struct addrinfo *hints,
				struct in_addr ip,
				unsigned short port)
{
	struct sockaddr_in *psin = NULL;
	struct addrinfo *ai = SMB_MALLOC(sizeof(*ai));

	if (!ai) {
		return NULL;
	}
	memset(ai, '\0', sizeof(*ai));

	psin = SMB_MALLOC(sizeof(*psin));
	if (!psin) {
		free(ai);
		return NULL;
	}

	memset(psin, '\0', sizeof(*psin));

	psin->sin_family = AF_INET;
	psin->sin_port = htons(port);
	psin->sin_addr = ip;

	ai->ai_flags = 0;
	ai->ai_family = AF_INET;
	ai->ai_socktype = hints->ai_socktype;
	ai->ai_protocol = hints->ai_protocol;
	ai->ai_addrlen = sizeof(*psin);
	ai->ai_addr = (struct sockaddr *) psin;
	ai->ai_canonname = NULL;
	ai->ai_next = NULL;

	return ai;
}

/*
 * get address info for a single ipv4 address.
 *
 */

static int getaddr_info_single_addr(const char *service,
				uint32_t addr,
				const struct addrinfo *hints,
				struct addrinfo **res)
{

	struct addrinfo *ai = NULL;
	struct in_addr ip;
	unsigned short port = 0;

	port = get_port_by_name(service);
	ip.s_addr = htonl(addr);

	ai = alloc_entry(hints, ip, port);
	if (!ai) {
		return EAI_MEMORY;
	}

	/* If we're asked for the canonical name,
	 * make sure it returns correctly. */
	if (!(hints->ai_flags & AI_NUMERICSERV) &&
			hints->ai_flags & AI_CANONNAME) {
		int err;
		if (addr == INADDR_LOOPBACK || addr == INADDR_ANY) {
			ai->ai_canonname = get_my_canon_name(&err);
		} else {
			ai->ai_canonname =
			get_canon_name_from_addr(ip,&err);
		}
		if (ai->ai_canonname == NULL) {
			freeaddrinfo(ai);
			return err;
		}
	}

	*res = ai;
	return 0;
}

/*
 * get address info for multiple ipv4 addresses.
 *
 */

static int getaddr_info_name(const char *node,
				const char *service,
				const struct addrinfo *hints,
				struct addrinfo **res)
{
	struct addrinfo *listp = NULL, *prevp = NULL;
	char **pptr = NULL;
	int err;
	struct hostent *hp = NULL;
	unsigned short port = 0;

	port = get_port_by_name(service);

	_fmutex *fsem = global_tcpip_sem();
	ASSERT(_fmutex_request(fsem, _FMR_IGNINT) == 0);

	hp = gethostbyname(node);

	_fmutex_release(fsem);

	err = check_hostent_err(hp);
	if (err) {
		return err;
	}

	for(pptr = hp->h_addr_list; *pptr; pptr++) {
		struct in_addr ip = *(struct in_addr *)*pptr;
		struct addrinfo *ai = alloc_entry(hints, ip, port);

		if (!ai) {
			freeaddrinfo(listp);
			return EAI_MEMORY;
		}

		if (!listp) {
			listp = ai;
			prevp = ai;
			ai->ai_canonname = SMB_STRDUP(hp->h_name);
			if (!ai->ai_canonname) {
				freeaddrinfo(listp);
				return EAI_MEMORY;
			}
		} else {
			prevp->ai_next = ai;
			prevp = ai;
		}
	}
	*res = listp;
	return 0;
}

/*
 * get address info for ipv4 sockets.
 *
 *	Bugs:	- servname can only be a number, not text.
 */

int rep_getaddrinfo(const char *node,
		const char *service,
		const struct addrinfo * hintp,
		struct addrinfo ** res)
{
	struct addrinfo hints;

	/* Setup the hints struct. */
	if (hintp == NULL) {
		memset(&hints, 0, sizeof(hints));
		hints.ai_family = AF_INET;
		hints.ai_socktype = SOCK_STREAM;
	} else {
		memcpy(&hints, hintp, sizeof(hints));
	}

	if (hints.ai_family != AF_INET && hints.ai_family != AF_UNSPEC) {
		return EAI_FAMILY;
	}

	if (hints.ai_socktype == 0) {
		hints.ai_socktype = SOCK_STREAM;
	}

	if (!node && !service) {
		return EAI_NONAME;
	}

	if (node) {
		if (node[0] == '\0') {
			return getaddr_info_single_addr(service,
					INADDR_ANY,
					&hints,
					res);
		} else if (hints.ai_flags & AI_NUMERICHOST) {
			struct in_addr ip;
			if (!inet_aton(node, &ip)) {
				return EAI_FAIL;
			}
			return getaddr_info_single_addr(service,
					ntohl(ip.s_addr),
					&hints,
					res);
		} else {
			return getaddr_info_name(node,
						service,
						&hints,
						res);
		}
	} else if (hints.ai_flags & AI_PASSIVE) {
		return getaddr_info_single_addr(service,
					INADDR_ANY,
					&hints,
					res);
	}
	return getaddr_info_single_addr(service,
					INADDR_LOOPBACK,
					&hints,
					res);
}


void rep_freeaddrinfo(struct addrinfo *res)
{
	struct addrinfo *next = NULL;

	for (;res; res = next) {
		next = res->ai_next;
		free(res->ai_canonname);
		free(res->ai_addr);
		free(res);
	}
}


const char *rep_gai_strerror(int errcode)
{
	/* This is a mix of Samba and VLC implementations */

	switch (errcode)
	{
		case EAI_BADFLAGS:
			return "Invalid argument";
		case EAI_NONAME:
			return "Unknown host";
		case EAI_AGAIN:
			return "Temporary name service failure";
		case EAI_FAIL:
			return "Non-recoverable name service failure";
		case EAI_NODATA:
			return "No host data of that type was found";
		case EAI_FAMILY:
			return "Address family not supported";
		case EAI_SOCKTYPE:
			return "Socket type not supported";
		case EAI_SERVICE:
			return "Class type not found";
		case EAI_ADDRFAMILY:
			return "Unavailable address family for host name";
		case EAI_MEMORY:
			return "Not enough memory";
		case EAI_OVERFLOW:
			return "Buffer overflow";
		case EAI_SYSTEM:
			return "System error";
		default:
			return "Unknown server error";
	}
}

static int gethostnameinfo(const struct sockaddr *sa,
			char *node,
			size_t nodelen,
			int flags)
{
	int ret = -1;
	char *p = NULL;

	if (!(flags & NI_NUMERICHOST)) {
		_fmutex *fsem = global_tcpip_sem();
		ASSERT(_fmutex_request(fsem, _FMR_IGNINT) == 0);

		struct hostent *hp = gethostbyaddr(
				(char *)&((struct sockaddr_in *)sa)->sin_addr,
				sizeof(struct in_addr),
				sa->sa_family);

		_fmutex_release(fsem);

		ret = check_hostent_err(hp);
		if (ret == 0) {
			/* Name looked up successfully. */
			ret = snprintf(node, nodelen, "%s", hp->h_name);
			if (ret < 0 || (size_t)ret >= nodelen) {
				return EAI_MEMORY;
			}
			if (flags & NI_NOFQDN) {
				p = strchr(node,'.');
				if (p) {
					*p = '\0';
				}
			}
			return 0;
		}

		if (flags & NI_NAMEREQD) {
			/* If we require a name and didn't get one,
			 * automatically fail. */
			return ret;
		}
		/* Otherwise just fall into the numeric host code... */
	}
	p = inet_ntoa(((struct sockaddr_in *)sa)->sin_addr);
	ret = snprintf(node, nodelen, "%s", p);
	if (ret < 0 || (size_t)ret >= nodelen) {
		return EAI_MEMORY;
	}
	return 0;
}

static int getservicenameinfo(const struct sockaddr *sa,
			char *service,
			size_t servicelen,
			int flags)
{
	int ret = -1;
	int port = ntohs(((struct sockaddr_in *)sa)->sin_port);

	if (!(flags & NI_NUMERICSERV)) {
		_fmutex *fsem = global_tcpip_sem();
		ASSERT(_fmutex_request(fsem, _FMR_IGNINT) == 0);

		struct servent *se = getservbyport(
				port,
				(flags & NI_DGRAM) ? "udp" : "tcp");

		_fmutex_release(fsem);

		if (se && se->s_name) {
			/* Service name looked up successfully. */
			ret = snprintf(service, servicelen, "%s", se->s_name);
			if (ret < 0 || (size_t)ret >= servicelen) {
				return EAI_MEMORY;
			}
			return 0;
		}
		/* Otherwise just fall into the numeric service code... */
	}
	ret = snprintf(service, servicelen, "%d", port);
	if (ret < 0 || (size_t)ret >= servicelen) {
		return EAI_MEMORY;
	}
	return 0;
}

/*
 * Convert an ipv4 address to a hostname.
 *
 * Bugs:	- No IPv6 support.
 */
int rep_getnameinfo(const struct sockaddr *sa, socklen_t salen,
			char *node, size_t nodelen,
			char *service, size_t servicelen, int flags)
{

	/* Invalid arguments. */
	if (sa == NULL || (node == NULL && service == NULL)) {
		return EAI_FAIL;
	}

	if (sa->sa_family != AF_INET) {
		return EAI_FAIL;
	}

	if (salen < sizeof(struct sockaddr_in)) {
		return EAI_FAIL;
	}

	int rc = 0;

	if (node) {
		rc = gethostnameinfo(sa, node, nodelen, flags);
	}

	if (!rc && service) {
		rc = getservicenameinfo(sa, service, servicelen, flags);
	}

	return rc;
}
