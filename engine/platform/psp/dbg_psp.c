/*
dbg_psp.c - debug console over Wi-Fi, for development without PSPLink
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

/*
PSPLink's USB link dies when the Wi-Fi stack is up for a while, and anything
waiting on host0: then hangs. With "-dbgcon" the game joins Wi-Fi at startup
and talks to scripts/psp/dbgcon.py on the PC instead:

- the PC sends to UDP port 27999: packets starting with 0xff are keepalives,
  anything else is a console command
- console output and watchdog reports go back to whoever sent the last packet
  (so the PC's firewall sees replies, not unsolicited traffic)
- "psp_update" receives a new EBOOT.PBP from the PC over TCP and restarts with it
- "psp_restart" restarts the game, also when the main thread is stuck: the
  watchdog thread then reads the commands (see PSP_DbgConPollStalled)
*/

#include "common.h"
#include "platform/platform.h"
#include "platform/psp/net_psp.h"
#include <pspkernel.h>
#include <psploadexec_kernel.h>

// SystemCtrlForUser (PRO/ARK custom firmware): user mode sceKernelLoadExec is refused
int sctrlKernelLoadExecVSHMs2( const char *file, struct SceKernelLoadExecVSHParam *param );
#include <fcntl.h>
#include <unistd.h>

#define DBGCON_PORT     27999
#define DBGCON_BUF      16384
#define DBGCON_PACKET   1200

static struct
{
	qboolean enabled;
	int sock;
	struct sockaddr_in pc;
	volatile qboolean have_pc;
	double next_join;

	// output collected between frames, sent by the main thread
	char buf[DBGCON_BUF];
	volatile int len;
	int dropped;
} dbg = { .sock = -1 };

static qboolean PSP_DbgConActive( void )
{
	return dbg.enabled && dbg.sock >= 0 && dbg.have_pc;
}

/*
==================
PSP_DbgConPrint

any thread; buffered until the next PSP_DbgConFrame
==================
*/
void PSP_DbgConPrint( const char *msg )
{
	int n, intr;

	if( !dbg.enabled )
		return;

	n = Q_strlen( msg );

	intr = sceKernelCpuSuspendIntr();
	if( dbg.len + n <= DBGCON_BUF )
	{
		memcpy( dbg.buf + dbg.len, msg, n );
		dbg.len += n;
	}
	else dbg.dropped += n;
	sceKernelCpuResumeIntr( intr );
}

/*
==================
PSP_DbgConSendNow

for the watchdog thread: straight to the PC, the main thread may be stuck
==================
*/
qboolean PSP_DbgConSendNow( const char *msg )
{
	if( !PSP_DbgConActive( ))
		return false;

	sendto( dbg.sock, msg, Q_min( Q_strlen( msg ), DBGCON_PACKET ), 0, (struct sockaddr *)&dbg.pc, sizeof( dbg.pc ));
	return true;
}

static void PSP_DbgConFlush( void )
{
	static char out[DBGCON_BUF];
	int len, intr, dropped;

	if( !PSP_DbgConActive( ) || !dbg.len )
		return;

	intr = sceKernelCpuSuspendIntr();
	len = dbg.len;
	memcpy( out, dbg.buf, len );
	dbg.len = 0;
	dropped = dbg.dropped;
	dbg.dropped = 0;
	sceKernelCpuResumeIntr( intr );

	for( int i = 0; i < len; i += DBGCON_PACKET )
		sendto( dbg.sock, out + i, Q_min( len - i, DBGCON_PACKET ), 0, (struct sockaddr *)&dbg.pc, sizeof( dbg.pc ));

	if( dropped )
	{
		char msg[64];
		Q_snprintf( msg, sizeof( msg ), "\n[dbgcon: %d bytes of output dropped]\n", dropped );
		sendto( dbg.sock, msg, Q_strlen( msg ), 0, (struct sockaddr *)&dbg.pc, sizeof( dbg.pc ));
	}
}

static void PSP_DbgConOpen( void )
{
	struct sockaddr_in addr;

	dbg.sock = socket( AF_INET, SOCK_DGRAM, IPPROTO_UDP );
	if( dbg.sock < 0 )
	{
		Con_Printf( S_ERROR "dbgcon: socket failed (%d)\n", errno );
		return;
	}

	memset( &addr, 0, sizeof( addr ));
	addr.sin_family = AF_INET;
	addr.sin_port = htons( DBGCON_PORT );
	addr.sin_addr.s_addr = INADDR_ANY;

	if( bind( dbg.sock, (struct sockaddr *)&addr, sizeof( addr )) < 0 || !NET_MakeSocketNonBlocking( dbg.sock ))
	{
		Con_Printf( S_ERROR "dbgcon: can't open UDP port %d (%d)\n", DBGCON_PORT, errno );
		closesocket( dbg.sock );
		dbg.sock = -1;
		return;
	}

	Con_Printf( "dbgcon: listening on UDP port %d\n", DBGCON_PORT );
}

/*
==================
PSP_DbgConFrame

main thread, every frame
==================
*/
void PSP_DbgConFrame( void )
{
	char packet[1024];
	struct sockaddr_in from;
	socklen_t fromlen;
	int n;

	if( !dbg.enabled )
		return;

	if( dbg.sock < 0 )
	{
		// the access point often refuses joins, keep trying
		if( host.realtime < dbg.next_join )
			return;

		dbg.next_join = host.realtime + 20.0;

		if( !PSP_NetIsConnected( ) && !PSP_NetConnect( ))
			return;

		PSP_DbgConOpen();
		if( dbg.sock < 0 )
			return;
	}

	while( 1 )
	{
		fromlen = sizeof( from );
		n = recvfrom( dbg.sock, packet, sizeof( packet ) - 2, 0, (struct sockaddr *)&from, &fromlen );
		if( n <= 0 )
			break;

		if( !dbg.have_pc || dbg.pc.sin_addr.s_addr != from.sin_addr.s_addr || dbg.pc.sin_port != from.sin_port )
		{
			dbg.pc = from;
			dbg.have_pc = true;
			Con_Printf( "dbgcon: talking to %s:%d\n", inet_ntoa( from.sin_addr ), ntohs( from.sin_port ));
		}

		if( (byte)packet[0] == 0xff ) // keepalive
			continue;

		packet[n] = '\n';
		packet[n + 1] = 0;
		Cbuf_AddText( packet );
	}

	PSP_DbgConFlush();
}

/*
==================
PSP_DbgConRestart

start <cwd>/EBOOT.PBP again, through SystemCtrlForUser (PRO/ARK custom
firmware): user mode sceKernelLoadExec is refused
==================
*/
static void PSP_DbgConRestart( void )
{
	struct SceKernelLoadExecVSHParam param;
	char cwd[256], eboot[300], msg[128];
	int ret;

	if( !getcwd( cwd, sizeof( cwd )))
		Q_strncpy( cwd, "ms0:/PSP/GAME/Xash3D", sizeof( cwd ));
	Q_snprintf( eboot, sizeof( eboot ), "%s/EBOOT.PBP", cwd );

	memset( &param, 0, sizeof( param ));
	param.size = sizeof( param );
	param.args = Q_strlen( eboot ) + 1;
	param.argp = eboot;
	param.key = "game";

	PSP_MeShutdown();
	ret = sctrlKernelLoadExecVSHMs2( eboot, &param );

	Q_snprintf( msg, sizeof( msg ), "dbgcon: sctrlKernelLoadExecVSHMs2 failed (0x%08x), start the game again from the XMB\n", ret );
	PSP_DbgConSendNow( msg );
}

static void PSP_DbgConRestart_f( void )
{
	Con_Printf( "restarting\n" );
	PSP_DbgConFlush();
	Host_WriteConfig();
	// leave the access point first: restarted with the connection still up,
	// the new instance often came back at the menu and never joined Wi-Fi
	PSP_NetShutdown();
	PSP_DbgConRestart();
}

/*
==================
PSP_DbgConPollStalled

watchdog thread, while the main thread is stuck (so it doesn't read the socket
itself): the only command that works then is psp_restart
==================
*/
void PSP_DbgConPollStalled( void )
{
	char packet[256];
	int n;

	if( !PSP_DbgConActive( ))
		return;

	// what the main thread printed before it got stuck (an error dialog...)
	PSP_DbgConFlush();

	while(( n = recvfrom( dbg.sock, packet, sizeof( packet ) - 1, 0, NULL, NULL )) > 0 )
	{
		if( (byte)packet[0] == 0xff ) // keepalive
			continue;

		packet[n] = 0;
		if( !Q_strncmp( packet, "psp_restart", 11 ))
		{
			PSP_DbgConSendNow( "dbgcon: the main thread is stuck, restarting from the watchdog\n" );
			PSP_DbgConRestart();
		}
		else PSP_DbgConSendNow( "dbgcon: the main thread is stuck, only psp_restart works now\n" );
	}
}

/*
==================
PSP_DbgConUpdate_f

psp_update [file]: wait (30 s) for the PC to connect to TCP port 27999 and send
"<size>\n" followed by a new EBOOT.PBP (or the given file, relative to the game
directory, e.g. valve/cl_dlls/client_psp_mipsel.prx), then restart with it.
The PC connects to us so its firewall doesn't get in the way. Blocking, it's a
debugging tool.
==================
*/
static void PSP_DbgConUpdate_f( void )
{
	char cwd[256], path[300], eboot[300];
	const char *dest = Cmd_Argc() > 1 ? Cmd_Argv( 1 ) : "EBOOT.PBP";
	static char buf[16384];
	int lsock, sock = -1, fd, n, total = 0, size = 0, have = 0;
	struct sockaddr_in addr;
	double deadline;

	if( Q_strstr( dest, ".." ) || dest[0] == '/' || Q_strchr( dest, ':' ))
	{
		Con_Printf( S_ERROR "psp_update: bad file name %s\n", dest );
		return;
	}

	memset( &addr, 0, sizeof( addr ));
	addr.sin_family = AF_INET;
	addr.sin_port = htons( DBGCON_PORT );
	addr.sin_addr.s_addr = INADDR_ANY;

	if(( lsock = socket( AF_INET, SOCK_STREAM, IPPROTO_TCP )) < 0 )
	{
		Con_Printf( S_ERROR "psp_update: socket failed (%d)\n", errno );
		return;
	}

	if( bind( lsock, (struct sockaddr *)&addr, sizeof( addr )) < 0 || listen( lsock, 1 ) < 0 || !NET_MakeSocketNonBlocking( lsock ))
	{
		Con_Printf( S_ERROR "psp_update: can't listen on TCP port %d (%d)\n", DBGCON_PORT, errno );
		closesocket( lsock );
		return;
	}

	Con_Printf( "psp_update: waiting for %s on TCP port %d\n", dest, DBGCON_PORT );
	PSP_DbgConFlush();

	for( deadline = Sys_DoubleTime() + 30.0; Sys_DoubleTime() < deadline; )
	{
		if(( sock = accept( lsock, NULL, NULL )) >= 0 )
			break;
		sceKernelDelayThread( 100 * 1000 );
	}
	closesocket( lsock );

	if( sock < 0 )
	{
		Con_Printf( S_ERROR "psp_update: nobody connected\n" );
		return;
	}

	// the accepted socket may inherit non-blocking mode
	{
		uint mode = 0;
		ioctlsocket( sock, FIONBIO, (void *)&mode );
	}

	// "<size>\n"
	while( have < 32 && ( n = recv( sock, buf + have, 1, 0 )) == 1 )
	{
		if( buf[have] == '\n' )
			break;
		have++;
	}
	buf[have] = 0;
	size = Q_atoi( buf );

	if( size < 1024 )
	{
		Con_Printf( S_ERROR "psp_update: bad size \"%s\"\n", buf );
		closesocket( sock );
		return;
	}

	if( !getcwd( cwd, sizeof( cwd )))
		Q_strncpy( cwd, "ms0:/PSP/GAME/Xash3D", sizeof( cwd ));

	Q_snprintf( eboot, sizeof( eboot ), "%s/%s", cwd, dest );
	Q_snprintf( path, sizeof( path ), "%s.new", eboot );
	if(( fd = open( path, O_WRONLY | O_CREAT | O_TRUNC, 0777 )) < 0 )
	{
		Con_Printf( S_ERROR "psp_update: can't write %s\n", path );
		closesocket( sock );
		return;
	}

	while( total < size && ( n = recv( sock, buf, Q_min( (int)sizeof( buf ), size - total ), 0 )) > 0 )
	{
		write( fd, buf, n );
		total += n;
	}

	close( fd );
	send( sock, total == size ? "OK\n" : "SHORT\n", total == size ? 3 : 6, 0 );
	closesocket( sock );

	if( total != size )
	{
		Con_Printf( S_ERROR "psp_update: got %d of %d bytes\n", total, size );
		return;
	}

	remove( eboot );
	if( rename( path, eboot ) < 0 )
	{
		Con_Printf( S_ERROR "psp_update: can't rename %s (%d)\n", path, errno );
		return;
	}

	Con_Printf( "psp_update: got %d bytes\n", total );
	PSP_DbgConRestart_f();
}

void PSP_DbgConInit( void )
{
	dbg.enabled = Sys_CheckParm( "-dbgcon" );
	Cmd_AddCommand( "psp_restart", PSP_DbgConRestart_f, "restart the game (debugging)" );
	Cmd_AddCommand( "psp_update", PSP_DbgConUpdate_f, "receive a new EBOOT.PBP (or the given file) from scripts/psp/dbgcon.py and restart with it (debugging)" );
}
