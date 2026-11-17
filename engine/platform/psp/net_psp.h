/*
net_psp.h - BSD sockets for PSP, through libcglue and pspnet_inet
Copyright (C) 2026 Xash3D FWGS contributors

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

#pragma once
#ifndef NET_PSP_H
#define NET_PSP_H

// pspsdk's struct in_addr is packed
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpacked"
#include "platform/posix/net.h"
#pragma GCC diagnostic pop

// IPv4 only stack, define enough IPv6 for net_ws.c to compile
#define XASH_NO_IPV6_RESOLVE 1

struct in6_addr
{
	uint8_t s6_addr[16];
};

struct sockaddr_in6
{
	uint8_t         sin6_len;
	sa_family_t     sin6_family;
	in_port_t       sin6_port;
	uint32_t        sin6_flowinfo;
	struct in6_addr sin6_addr;
	uint32_t        sin6_scope_id;
};

static const struct in6_addr in6addr_any;

#ifndef IN6_IS_ADDR_V4MAPPED
#define IN6_IS_ADDR_V4MAPPED( p ) ( 0 )
#endif

#ifndef IPPROTO_IPV6
#define IPPROTO_IPV6 41
#endif

#ifndef IPV6_MULTICAST_LOOP
#define IPV6_MULTICAST_LOOP 11
#endif

#ifndef IPV6_V6ONLY
#define IPV6_V6ONLY 27
#endif

// no getaddrinfo in pspsdk, emulate the IPv4 part with gethostbyname
struct addrinfo
{
	int              ai_flags;
	int              ai_family;
	int              ai_socktype;
	int              ai_protocol;
	socklen_t        ai_addrlen;
	struct sockaddr  *ai_addr;
	char             *ai_canonname;
	struct addrinfo  *ai_next;
};

static inline int getaddrinfo_psp( const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res )
{
	struct hostent *h;
	struct addrinfo *ai;
	struct sockaddr_in *sin;

	*res = NULL;

	if( hints && hints->ai_family != AF_UNSPEC && hints->ai_family != AF_INET )
		return -1;

	h = gethostbyname( node );
	if( !h || h->h_addrtype != AF_INET || !h->h_addr_list[0] )
		return -1;

	ai = calloc( 1, sizeof( *ai ) + sizeof( *sin ));
	if( !ai )
		return -1;

	sin = (struct sockaddr_in *)( ai + 1 );
	sin->sin_len = sizeof( *sin );
	sin->sin_family = AF_INET;
	memcpy( &sin->sin_addr, h->h_addr_list[0], sizeof( sin->sin_addr ));

	ai->ai_family = AF_INET;
	ai->ai_addrlen = sizeof( *sin );
	ai->ai_addr = (struct sockaddr *)sin;
	*res = ai;

	return 0;
}

static inline void freeaddrinfo_psp( struct addrinfo *ai )
{
	free( ai );
}

#define getaddrinfo  getaddrinfo_psp
#define freeaddrinfo freeaddrinfo_psp

#ifndef FIONBIO
#define FIONBIO SO_NONBLOCK
#endif

// ioctlsocket() is only used to set non-blocking mode on sockets
static inline int ioctl_psp( int fd, int req, unsigned int *arg )
{
	if( req == FIONBIO )
		return setsockopt( fd, SOL_SOCKET, SO_NONBLOCK, arg, sizeof( *arg ));
	return -1;
}

#define ioctlsocket ioctl_psp

#endif // NET_PSP_H
