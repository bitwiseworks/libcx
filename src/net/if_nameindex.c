/*
 * if_nameindex() family implementation for OS/2 kLIBC
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
 * if_nameindex() family implementaiton for OS/2 kLIBC
 *
 * Copyright (C) 2016 KO Myung-Hun <komh@chollian.net>
 *
 * This program is free software. It comes without any warranty, to
 * the extent permitted by applicable law. You can redistribute it
 * and/or modify it under the terms of the Do What The Fuck You Want
 * To Public License, Version 2, as published by Sam Hocevar. See
 * http://www.wtfpl.net/ for more details.
 */

#include "libcx/net.h"

#include <stdlib.h>
#include <string.h>
#include <io.h>

#include <types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>  /* SIOCGIFCONF */
#include <net/if.h>     /* struct ifconf, struct ifreq */
#include <net/if_dl.h>  /* struct sockaddr_dl */

struct if_nameindex *if_nameindex( void )
{
    int s;
    struct ifconf ifconf;
    struct ifreq iflist[ IFMIB_ENTRIES /* MAX 42 */];
    struct if_nameindex *nis = NULL;
    int ifcount;
    int entries = 0;
    int i;

    s = socket( PF_INET, SOCK_RAW, 0 );
    if( s == -1 )
        goto out;

    /* Get all interfaces */
    ifconf.ifc_len = sizeof( iflist );
    ifconf.ifc_req = iflist;
    if( ioctl( s, SIOCGIFCONF, &ifconf ))
        goto out;

    /* Calculate count of returned interfaces */
    ifcount = ifconf.ifc_len / sizeof( struct ifreq );

    /* Allocate 1 more entry for the last mark */
    nis = calloc( 1, sizeof( *nis ) * ( ifcount + 1 ));
    if( nis == NULL )
        goto out;

    for( i = 0; i < ifcount; i++ )
    {
        /* Find available interfaces */
        if( iflist[ i ].ifr_addr.sa_family == AF_LINK )
        {
            struct sockaddr_dl *sdl =
                ( struct sockaddr_dl * )&iflist[ i ].ifr_addr;

            nis[ entries ].if_index = sdl->sdl_index;
            nis[ entries ].if_name = strdup( iflist[ i ].ifr_name );
            if( nis[ entries ].if_name == NULL )
            {
                if_freenameindex( nis );

                nis = NULL;

                goto out;
            }

            entries++;
        }
    }

    /* Last entry should be 0 and NULL. calloc() did it */

    entries++;

out:
    /* Shrink if needed */
    if( nis != NULL && entries != ifcount )
        nis = realloc( nis, sizeof( *nis ) * entries );

    close( s );

    return nis;
}

void if_freenameindex( struct if_nameindex *ptr )
{
    int i;

    if( ptr == NULL )
        return;

    for( i = 0; ptr[ i ].if_index != 0; i++ )
        free( ptr[ i ].if_name );

    free( ptr );
}

char *if_indextoname( unsigned ifindex, char *ifname )
{
    struct if_nameindex *nis;
    int i;

    nis = if_nameindex();
    if( nis == NULL )
        return NULL;

    for( i = 0; nis[ i ].if_index != 0; i++ )
    {
        if( ifindex == nis[ i ].if_index )
        {
            strcpy( ifname, nis[ i ].if_name );

            break;
        }
    }

    i = nis[ i ].if_index;

    if_freenameindex( nis );

    if( i == 0 )
        return NULL;

    return ifname;
}

unsigned if_nametoindex( const char *ifname )
{
    struct if_nameindex *nis;
    int i;

    nis = if_nameindex();
    if( nis == NULL )
        return 0;

    for( i = 0; nis[ i ].if_index != 0; i++ )
    {
        if( strcmp( ifname, nis[ i ].if_name ) == 0 )
            break;
    }

    i = nis[ i ].if_index;

    if_freenameindex( nis );

    return i;
}
