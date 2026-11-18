/*
net_psp.c - PSP Wi-Fi bring-up: network modules, access point connection
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

#include "platform/platform.h"
#include "platform/psp/net_psp.h" // before the pspnet headers, it silences -Wpacked of their in_addr
#include <pspkernel.h>
#include <pspnet.h>
#include <pspnet_inet.h>
#include <pspnet_apctl.h>
#include <pspnet_resolver.h>
#include <psputility_netmodules.h>
#include <psputility_netparam.h>

// the network stack and its modules cost memory, so nothing is loaded until
// the engine wants real sockets (connect, listen server, server browser)

#define NET_POOL_SIZE       ( 128 * 1024 )
#define NET_THREAD_PRIO     42
#define NET_THREAD_STACK    ( 4 * 1024 )
#define APCTL_THREAD_STACK  ( 12 * 1024 )
#define APCTL_THREAD_PRIO   48
#define APCTL_TIMEOUT       30.0 // seconds

static CVAR_DEFINE_AUTO( net_psp_profile, "1", FCVAR_ARCHIVE | FCVAR_PRIVILEGED, "Wi-Fi connection profile number from the system network settings" );

static struct
{
	qboolean modules;  // utility net modules loaded
	qboolean stack;    // sceNet/Inet/Resolver/Apctl initialized
	qboolean connected; // got an IP from the access point
	char     ip[16];
	volatile int last_error; // from the apctl handler (runs on the apctl thread)
} psp_net;

static void PSP_NetApctlHandler( int old_state, int new_state, int event, int error, void *arg )
{
	if( error != 0 )
		psp_net.last_error = error;
}

static void PSP_NetProfiles_f( void )
{
	netData name, ssid, secure;
	int i, count = 0;

	for( i = 1; i < 32; i++ )
	{
		if( sceUtilityCheckNetParam( i ) != 0 )
			continue;

		if( sceUtilityGetNetParam( i, PSP_NETPARAM_NAME, &name ) < 0 )
			name.asString[0] = 0;
		if( sceUtilityGetNetParam( i, PSP_NETPARAM_SSID, &ssid ) < 0 )
			ssid.asString[0] = 0;
		if( sceUtilityGetNetParam( i, PSP_NETPARAM_SECURE, &secure ) < 0 )
			secure.asUint = 0;

		name.asString[sizeof( name.asString ) - 1] = 0;
		ssid.asString[sizeof( ssid.asString ) - 1] = 0;
		Con_Printf( "%c%2d: \"%s\" SSID \"%s\" security %u\n", i == (int)net_psp_profile.value ? '*' : ' ',
			i, name.asString, ssid.asString, secure.asUint );
		count++;
	}

	Con_Printf( "%d Wi-Fi profiles, net_psp_profile selects one\n", count );
}

static void PSP_NetPrintState( int state )
{
	static const char *names[] =
	{
		"disconnected", "scanning", "joining", "getting IP address", "connected", "EAP authentication", "key exchange"
	};

	if( state >= 0 && state < (int)ARRAYSIZE( names ))
		Con_Printf( "Wi-Fi: %s...\n", names[state] );
	else Con_Printf( "Wi-Fi: state %d...\n", state );
}

static qboolean PSP_NetLoadStack( void )
{
	int ret;

	if( !psp_net.modules )
	{
		if(( ret = sceUtilityLoadNetModule( PSP_NET_MODULE_COMMON )) < 0 )
		{
			Con_Printf( S_ERROR "Wi-Fi: can't load the common net module (0x%08x)\n", ret );
			return false;
		}

		if(( ret = sceUtilityLoadNetModule( PSP_NET_MODULE_INET )) < 0 )
		{
			Con_Printf( S_ERROR "Wi-Fi: can't load the inet module (0x%08x)\n", ret );
			sceUtilityUnloadNetModule( PSP_NET_MODULE_COMMON );
			return false;
		}

		psp_net.modules = true;
	}

	if( !psp_net.stack )
	{
		if(( ret = sceNetInit( NET_POOL_SIZE, NET_THREAD_PRIO, NET_THREAD_STACK, NET_THREAD_PRIO, NET_THREAD_STACK )) < 0 )
		{
			Con_Printf( S_ERROR "Wi-Fi: sceNetInit failed (0x%08x)\n", ret );
			return false;
		}

		if(( ret = sceNetInetInit( )) < 0 )
		{
			Con_Printf( S_ERROR "Wi-Fi: sceNetInetInit failed (0x%08x)\n", ret );
			sceNetTerm();
			return false;
		}

		if(( ret = sceNetResolverInit( )) < 0 )
		{
			Con_Printf( S_ERROR "Wi-Fi: sceNetResolverInit failed (0x%08x)\n", ret );
			sceNetInetTerm();
			sceNetTerm();
			return false;
		}

		if(( ret = sceNetApctlInit( APCTL_THREAD_STACK, APCTL_THREAD_PRIO )) < 0 )
		{
			Con_Printf( S_ERROR "Wi-Fi: sceNetApctlInit failed (0x%08x)\n", ret );
			sceNetResolverTerm();
			sceNetInetTerm();
			sceNetTerm();
			return false;
		}

		sceNetApctlAddHandler( PSP_NetApctlHandler, NULL );
		psp_net.stack = true;
	}

	return true;
}

/*
==================
PSP_NetConnect

brings the network up and connects to the access point from
the selected profile, blocks until there's an IP address
==================
*/
qboolean PSP_NetConnect( void )
{
	union SceNetApctlInfo info;
	int ret, state, last_state = -1;
	double start;

	if( psp_net.connected )
		return true;

	if( !PSP_NetLoadStack( ))
		return false;

	// already connected (e.g. from a previous attempt that timed out late)?
	if( sceNetApctlGetState( &state ) >= 0 && state == PSP_NET_APCTL_STATE_GOT_IP )
		goto got_ip;

	Con_Printf( "Wi-Fi: connecting with profile %d\n", net_psp_profile.value > 0 ? (int)net_psp_profile.value : 1 );
	psp_net.last_error = 0;

	if(( ret = sceNetApctlConnect( net_psp_profile.value > 0 ? (int)net_psp_profile.value : 1 )) < 0 )
	{
		Con_Printf( S_ERROR "Wi-Fi: can't connect (0x%08x), check the profile in the system network settings and the Wi-Fi switch\n", ret );
		return false;
	}

	start = Platform_DoubleTime();

	while( 1 )
	{
		if(( ret = sceNetApctlGetState( &state )) < 0 )
		{
			Con_Printf( S_ERROR "Wi-Fi: can't get the connection state (0x%08x)\n", ret );
			break;
		}

		if( state != last_state )
		{
			// dropped back to disconnected after trying: failed
			if( state == PSP_NET_APCTL_STATE_DISCONNECTED && last_state > PSP_NET_APCTL_STATE_DISCONNECTED )
			{
				Con_Printf( S_ERROR "Wi-Fi: connection failed (error 0x%08x)\n", psp_net.last_error );
				break;
			}

			PSP_NetPrintState( state );
			last_state = state;
		}

		if( state == PSP_NET_APCTL_STATE_GOT_IP )
			goto got_ip;

		if( Platform_DoubleTime() - start > APCTL_TIMEOUT )
		{
			Con_Printf( S_ERROR "Wi-Fi: timed out\n" );
			break;
		}

		sceKernelDelayThread( 50 * 1000 );
	}

	sceNetApctlDisconnect();
	return false;

got_ip:
	if( sceNetApctlGetInfo( PSP_NET_APCTL_INFO_IP, &info ) >= 0 )
		Q_strncpy( psp_net.ip, info.ip, sizeof( psp_net.ip ));
	else Q_strncpy( psp_net.ip, "0.0.0.0", sizeof( psp_net.ip ));

	psp_net.connected = true;

	if( sceNetApctlGetInfo( PSP_NET_APCTL_INFO_SSID, &info ) >= 0 )
		Con_Printf( "Wi-Fi: connected to \"%.32s\", IP address %s\n", info.ssid, psp_net.ip );
	else Con_Printf( "Wi-Fi: connected, IP address %s\n", psp_net.ip );

	return true;
}

/*
==================
PSP_NetGetHostName

there's no gethostname, give the engine our IP address to resolve instead
==================
*/
int PSP_NetGetHostName( char *name, size_t len )
{
	if( !len )
		return -1;

	Q_strncpy( name, psp_net.connected ? psp_net.ip : "localhost", len );
	return 0;
}

void PSP_NetShutdown( void )
{
	if( psp_net.stack )
	{
		if( psp_net.connected )
			sceNetApctlDisconnect();

		sceNetApctlTerm();
		sceNetResolverTerm();
		sceNetInetTerm();
		sceNetTerm();
		psp_net.stack = false;
	}

	if( psp_net.modules )
	{
		sceUtilityUnloadNetModule( PSP_NET_MODULE_INET );
		sceUtilityUnloadNetModule( PSP_NET_MODULE_COMMON );
		psp_net.modules = false;
	}

	psp_net.connected = false;
}

static void PSP_NetStatus_f( void )
{
	union SceNetApctlInfo info;
	int state;

	if( !psp_net.stack )
	{
		Con_Printf( "Wi-Fi: not started (starts on connect or when a multiplayer server is started)\n" );
		return;
	}

	if( sceNetApctlGetState( &state ) >= 0 )
		PSP_NetPrintState( state );

	if( !psp_net.connected )
		return;

	if( sceNetApctlGetInfo( PSP_NET_APCTL_INFO_SSID, &info ) >= 0 )
		Con_Printf( "SSID: %.32s\n", info.ssid );
	Con_Printf( "IP: %s\n", psp_net.ip );
	if( sceNetApctlGetInfo( PSP_NET_APCTL_INFO_GATEWAY, &info ) >= 0 )
		Con_Printf( "gateway: %s\n", info.gateway );
	if( sceNetApctlGetInfo( PSP_NET_APCTL_INFO_PRIMDNS, &info ) >= 0 )
		Con_Printf( "DNS: %s\n", info.primaryDns );
	if( sceNetApctlGetInfo( PSP_NET_APCTL_INFO_STRENGTH, &info ) >= 0 )
		Con_Printf( "signal: %u%%\n", info.strength );
}

void PSP_NetRegisterCommands( void )
{
	Cvar_RegisterVariable( &net_psp_profile );
	Cmd_AddCommand( "net_psp_status", PSP_NetStatus_f, "show the Wi-Fi connection state" );
	Cmd_AddCommand( "net_psp_profiles", PSP_NetProfiles_f, "list the Wi-Fi connection profiles from the system settings" );
}
