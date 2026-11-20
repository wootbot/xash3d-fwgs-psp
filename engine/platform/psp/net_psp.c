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
// below the main thread (0x20) like in the pspsdk samples, PSP_NetFrame lets them run
// (at 0x18 the whole system locked up)
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
	int      handler;  // apctl handler id
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

		psp_net.handler = sceNetApctlAddHandler( PSP_NetApctlHandler, NULL );
		psp_net.stack = true;
	}

	return true;
}

/*
==================
PSP_NetPickProfile

net_psp_profile if that profile exists, the first existing one otherwise
==================
*/
static int PSP_NetPickProfile( void )
{
	int i, profile = (int)net_psp_profile.value;

	if( profile > 0 && sceUtilityCheckNetParam( profile ) == 0 )
		return profile;

	for( i = 1; i < 32; i++ )
	{
		if( sceUtilityCheckNetParam( i ) != 0 )
			continue;

		Con_Printf( "Wi-Fi: profile %d doesn't exist, using profile %d\n", profile, i );
		return i;
	}

	return -1;
}

// free bytes in the network stack pool, -1 if the stack isn't up
int PSP_NetPoolFree( void )
{
	SceNetMallocStat stat;

	if( !psp_net.stack || sceNetGetMallocStat( &stat ) < 0 )
		return -1;

	return stat.free;
}

/*
==================
PSP_NetFrame

the network stack's threads receive packets and answer ARP, but they have
a lower priority than the main thread, which rarely blocks: without this the
PSP can send but hardly ever receives anything. They run while the main
thread sleeps, ~0.6 ms a frame in a firefight: a short sleep is enough, a
1 ms one left the CPU idle for the rest of it.
==================
*/
void PSP_NetFrame( void )
{
	if( psp_net.stack )
		sceKernelDelayThread( 200 );
}

qboolean PSP_NetIsConnected( void )
{
	return psp_net.connected;
}

// one join attempt, true once there's an IP address
static qboolean PSP_NetTryConnect( int profile )
{
	int ret, state, last_state = -1;
	double start;

	Con_Printf( "Wi-Fi: connecting with profile %d\n", profile );
	psp_net.last_error = 0;

	if(( ret = sceNetApctlConnect( profile )) < 0 )
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
			return true;

		if( Platform_DoubleTime() - start > APCTL_TIMEOUT )
		{
			Con_Printf( S_ERROR "Wi-Fi: timed out\n" );
			break;
		}

		sceKernelDelayThread( 50 * 1000 );
	}

	sceNetApctlDisconnect();
	return false;
}

#define APCTL_ATTEMPTS 10

// disconnect from the access point and wait (up to 2 s) until that's done
static void PSP_NetWaitDisconnected( void )
{
	int i, state;

	if( sceNetApctlGetState( &state ) < 0 || state == PSP_NET_APCTL_STATE_DISCONNECTED )
		return;

	Con_Reportf( "Wi-Fi: disconnecting\n" );
	sceNetApctlDisconnect();

	for( i = 0; i < 40; i++ )
	{
		if( sceNetApctlGetState( &state ) < 0 || state == PSP_NET_APCTL_STATE_DISCONNECTED )
			break;
		sceKernelDelayThread( 50 * 1000 );
	}
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
	int state, profile;

	if( psp_net.connected )
		return true;

	if( !PSP_NetLoadStack( ))
		return false;

	// already connected (e.g. from a previous attempt that timed out late)?
	if( sceNetApctlGetState( &state ) >= 0 && state == PSP_NET_APCTL_STATE_GOT_IP )
		goto got_ip;

	profile = PSP_NetPickProfile();
	if( profile < 0 )
	{
		Con_Printf( S_ERROR "Wi-Fi: no connection profiles, create one in Settings > Network Settings\n" );
		return false;
	}

	// the access point sometimes refuses a join, mostly right after the
	// previous session went away without disconnecting
	for( int i = 0; i < APCTL_ATTEMPTS; i++ )
	{
		if( i > 0 )
		{
			// keep the stack and the modules loaded: unloading and reloading
			// them between attempts hard-froze the whole PSP (PSPLink too)
			Con_Printf( "Wi-Fi: retrying (%d of %d)\n", i + 1, APCTL_ATTEMPTS );
			PSP_NetWaitDisconnected();
			sceKernelDelayThread( 2 * 1000 * 1000 );
		}

		if( PSP_NetTryConnect( profile ))
			goto got_ip;
	}

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
		// wait for the disconnect to finish before tearing the stack down
		PSP_NetWaitDisconnected();

		if( psp_net.handler >= 0 )
			sceNetApctlDelHandler( psp_net.handler );
		psp_net.handler = -1;

		Con_Reportf( "Wi-Fi: apctl term\n" );
		sceNetApctlTerm();
		Con_Reportf( "Wi-Fi: resolver term\n" );
		sceNetResolverTerm();
		Con_Reportf( "Wi-Fi: inet term\n" );
		sceNetInetTerm();
		Con_Reportf( "Wi-Fi: net term\n" );
		sceNetTerm();
		psp_net.stack = false;
	}

	if( psp_net.modules )
	{
		Con_Reportf( "Wi-Fi: unloading modules\n" );
		sceUtilityUnloadNetModule( PSP_NET_MODULE_INET );
		sceUtilityUnloadNetModule( PSP_NET_MODULE_COMMON );
		psp_net.modules = false;
		Con_Reportf( "Wi-Fi: shut down\n" );
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

	SceNetMallocStat stat;

	if( sceNetGetMallocStat( &stat ) >= 0 )
		Con_Printf( "net pool: %d KB free, lowest %d KB free (of %d KB)\n", stat.free >> 10, ( stat.pool - stat.maximum ) >> 10, stat.pool >> 10 );

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
	if( sceNetApctlGetInfo( PSP_NET_APCTL_INFO_POWER_SAVE, &info ) >= 0 )
		Con_Printf( "WLAN power save: %s\n", info.powerSave ? "on" : "off" );
}

void PSP_NetRegisterCommands( void )
{
	Cvar_RegisterVariable( &net_psp_profile );
	Cmd_AddCommand( "net_psp_status", PSP_NetStatus_f, "show the Wi-Fi connection state" );
	Cmd_AddCommand( "net_psp_profiles", PSP_NetProfiles_f, "list the Wi-Fi connection profiles from the system settings" );
}
