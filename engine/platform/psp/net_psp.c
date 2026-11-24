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
#include <fcntl.h>
#include <unistd.h>

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

	// PSP_NetConnectFrame
	qboolean joining;
	int      join_profile, join_state, join_attempts;
	double   join_start, join_next, join_first;
	double   link_check;
} psp_net;

/*
==================
PSP_NetLog

what happens to the connection with -dbgcon, also into wifi.log in the game
directory: while the PSP is offline nothing gets to the PC. Kept over restarts
==================
*/
static void PSP_NetLog( const char *fmt, ... )
{
	char buf[256];
	va_list args;
	int len;

	len = Q_snprintf( buf, sizeof( buf ), "%7.1f s: ", Platform_DoubleTime( ));
	va_start( args, fmt );
	Q_vsnprintf( buf + len, sizeof( buf ) - len, fmt, args );
	va_end( args );

	Con_Printf( "Wi-Fi: %s", buf );

	int fd = open( "wifi.log", O_WRONLY | O_CREAT | O_APPEND, 0777 );
	if( fd < 0 )
		return;

	if( lseek( fd, 0, SEEK_END ) > 64 * 1024 )
	{
		close( fd );
		if(( fd = open( "wifi.log", O_WRONLY | O_CREAT | O_TRUNC, 0777 )) < 0 )
			return;
	}
	write( fd, buf, Q_strlen( buf ));
	close( fd );
}

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
1 ms one left the CPU idle for the rest of it. In long frames it's 4% of the
time: 200 us 25 times a second isn't enough for them, what the server sent
came up to a second late.
==================
*/
void PSP_NetFrame( void )
{
	static uint32_t last;
	const uint32_t now = sceKernelGetSystemTimeLow();
	const uint32_t elapsed = now - last;

	last = now;

	if( psp_net.joining )
		sceKernelDelayThread( 8 * 1000 ); // the key exchange takes time
	else if( psp_net.stack )
		sceKernelDelayThread( bound( 200, elapsed / 25, 2000 ));
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

static void PSP_NetGotIP( void )
{
	union SceNetApctlInfo info;

	if( sceNetApctlGetInfo( PSP_NET_APCTL_INFO_IP, &info ) >= 0 )
		Q_strncpy( psp_net.ip, info.ip, sizeof( psp_net.ip ));
	else Q_strncpy( psp_net.ip, "0.0.0.0", sizeof( psp_net.ip ));

	psp_net.connected = true;
	psp_net.joining = false;

	if( sceNetApctlGetInfo( PSP_NET_APCTL_INFO_SSID, &info ) >= 0 )
		Con_Printf( "Wi-Fi: connected to \"%.32s\", IP address %s\n", info.ssid, psp_net.ip );
	else Con_Printf( "Wi-Fi: connected, IP address %s\n", psp_net.ip );
}

/*
==================
PSP_NetRejoin

leave the access point, PSP_NetConnectFrame joins it again
==================
*/
void PSP_NetRejoin( const char *why )
{
	PSP_NetLog( "%s, joining again\n", why );
	sceNetApctlDisconnect();
	psp_net.connected = psp_net.joining = false;
	psp_net.join_attempts = 0;
	psp_net.join_next = Platform_DoubleTime() + 2.0;
}

static void PSP_NetJoinFailed( const char *why, int error )
{
	// the access point refuses most joins right after a restart, a few of them is normal
	if( psp_net.join_attempts <= 3 || psp_net.join_attempts % 10 == 0 )
		PSP_NetLog( "join %d: %s (0x%08x)\n", psp_net.join_attempts, why, error );

	sceNetApctlDisconnect();
	psp_net.joining = false;
	psp_net.join_next = Platform_DoubleTime() + 2.0;
}

/*
==================
PSP_NetConnectFrame

for -dbgcon: join the access point without blocking, as often as it takes, and
again if the connection is lost. True while there's an IP address
==================
*/
qboolean PSP_NetConnectFrame( void )
{
	const double now = Platform_DoubleTime();
	int ret, state;

	if( psp_net.connected )
	{
		if( now < psp_net.link_check )
			return true;

		psp_net.link_check = now + 1.0;

		if( sceNetApctlGetState( &state ) >= 0 && state == PSP_NET_APCTL_STATE_GOT_IP )
			return true;

		PSP_NetRejoin( "connection lost" );
		return false;
	}

	if( !psp_net.joining )
	{
		if( now < psp_net.join_next )
			return false;

		if( !PSP_NetLoadStack( ))
		{
			PSP_NetLog( "can't load the network stack\n" );
			psp_net.join_next = now + 20.0;
			return false;
		}

		if( sceNetApctlGetState( &state ) >= 0 && state != PSP_NET_APCTL_STATE_DISCONNECTED )
		{
			// the last attempt isn't over yet
			sceNetApctlDisconnect();
			psp_net.join_next = now + 1.0;
			return false;
		}

		if( psp_net.join_profile <= 0 && ( psp_net.join_profile = PSP_NetPickProfile( )) < 0 )
		{
			PSP_NetLog( "no connection profiles\n" );
			psp_net.join_next = now + 60.0;
			return false;
		}

		if( !psp_net.join_attempts++ )
			psp_net.join_first = now;

		psp_net.last_error = 0;
		if(( ret = sceNetApctlConnect( psp_net.join_profile )) < 0 )
		{
			PSP_NetJoinFailed( "can't connect", ret );
			return false;
		}

		psp_net.joining = true;
		psp_net.join_start = now;
		psp_net.join_state = PSP_NET_APCTL_STATE_DISCONNECTED;
		return false;
	}

	if(( ret = sceNetApctlGetState( &state )) < 0 )
	{
		PSP_NetJoinFailed( "no connection state", ret );
	}
	else if( state == PSP_NET_APCTL_STATE_GOT_IP )
	{
		PSP_NetGotIP();
		PSP_NetLog( "joined after %d attempts, %.0f s\n", psp_net.join_attempts, now - psp_net.join_first );
		psp_net.join_attempts = 0;
		return true;
	}
	else if( state == PSP_NET_APCTL_STATE_DISCONNECTED && psp_net.join_state != PSP_NET_APCTL_STATE_DISCONNECTED )
	{
		PSP_NetJoinFailed( "refused", psp_net.last_error );
	}
	else if( now - psp_net.join_start > APCTL_TIMEOUT )
	{
		PSP_NetJoinFailed( "timed out", state );
	}
	else psp_net.join_state = state;

	return false;
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
	int state, profile;

	if( psp_net.connected )
		return true;

	if( !PSP_NetLoadStack( ))
		return false;

	if( psp_net.joining )
	{
		// take over from the background
		psp_net.joining = false;
		if( sceNetApctlGetState( &state ) >= 0 && state != PSP_NET_APCTL_STATE_GOT_IP )
			PSP_NetWaitDisconnected();
	}

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
	PSP_NetGotIP();
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

	psp_net.connected = psp_net.joining = false;
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
