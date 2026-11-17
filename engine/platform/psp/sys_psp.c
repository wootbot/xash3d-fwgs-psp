/*
sys_psp.c - PSP system utils
Copyright (C) 2021 Sergey Galushko

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
#include "platform/psp/ka/ka.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstrict-prototypes"
#include <pspsdk.h>
#pragma GCC diagnostic pop
#include <pspkernel.h>
#include <psppower.h>
#include <pspctrl.h>
#include <pspdebug.h>
#include <ctype.h>
#include <unistd.h>

PSP_MODULE_INFO( "Xash3D", PSP_MODULE_USER, 1, 0 );
PSP_MAIN_THREAD_ATTR( PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU );
PSP_MAIN_THREAD_STACK_SIZE_KB( 512 );
// current pspsdk: any negative heap size means "all free memory minus the threshold",
// the threshold is left for the game PRX modules
PSP_HEAP_SIZE_KB( -1 );
PSP_HEAP_THRESHOLD_SIZE_KB( 3 * 1024 );

#define PSP_MAX_ARGV     32
#define PSP_CMDLINE_FILE "start.cmd"

static int PSP_PowerCallback( int count, int arg, void *common )
{
	// TODO: suspend/resume, see P5 RAM in the old port
	return 0;
}

static int PSP_CallbackThread( SceSize args, void *argp )
{
	int cbid = sceKernelCreateCallback( "Power Callback", PSP_PowerCallback, NULL );
	scePowerRegisterCallback( 0, cbid );

	sceKernelSleepThreadCB();

	return 0;
}

static void PSP_SetupCallbacks( void )
{
	int thid = sceKernelCreateThread( "update_thread", PSP_CallbackThread, 0x11, 0xFA0, 0, NULL );

	if( thid >= 0 )
		sceKernelStartThread( thid, 0, NULL );
}

#if XASH_TIMER == TIMER_PSP
double Platform_DoubleTime( void )
{
	return (double)sceKernelGetSystemTimeWide() * 0.000001; // microseconds to seconds
}

void Platform_Sleep( int msec )
{
	sceKernelDelayThreadCB( msec * 1000 );
}
#endif // XASH_TIMER == TIMER_PSP

void Platform_ShellExecute( const char *path, const char *parms )
{
}

#if XASH_MESSAGEBOX == MSGBOX_PSP
void Platform_MessageBox( const char *title, const char *message, qboolean parentMainWindow )
{
	static qboolean init = false;
	SceCtrlData pad;

	printf( "%s: %s\n", title, message );

	if( !init )
	{
		pspDebugScreenInit();
		init = true;
	}

	pspDebugScreenSetTextColor( 0x0080ff );
	pspDebugScreenPrintf( "\n\n\n\n%s:\n", title );
	pspDebugScreenSetTextColor( 0x0000ff );
	pspDebugScreenPrintData( message, Q_strlen( message ));
	pspDebugScreenSetTextColor( 0xffffff );
	pspDebugScreenPrintf( "\n\nPress X to continue.\n" );

	// under PSPLink nobody may be looking at the screen, don't block the engine forever
	char cwd[16] = "";
	const qboolean psplink = getcwd( cwd, sizeof( cwd )) && !Q_strnicmp( cwd, "host", 4 );
	const SceUInt64 deadline = sceKernelGetSystemTimeWide() + 10 * 1000 * 1000;

	// wait for the button to be released first, then pressed
	do
	{
		sceCtrlReadBufferPositive( &pad, 1 );
	} while( pad.Buttons & PSP_CTRL_CROSS );

	do
	{
		sceCtrlReadBufferPositive( &pad, 1 );

		if( psplink && sceKernelGetSystemTimeWide() > deadline )
			break;
	} while( !( pad.Buttons & PSP_CTRL_CROSS ));

	pspDebugScreenClear();
}
#endif // XASH_MESSAGEBOX == MSGBOX_PSP

/*
===========
PSP_GetArgv

The XMB starts us with only argv[0]. Additional arguments are read
from start.cmd next to the EBOOT, whitespace separated.
===========
*/
int PSP_GetArgv( int in_argc, char **in_argv, char ***out_argv )
{
	static char *argv[PSP_MAX_ARGV];
	int argc = 0, fd, size;
	char *buf, *p;

	*out_argv = in_argv;

	if( in_argc > 1 ) // started by PSPLink with arguments
		return in_argc;

	fd = sceIoOpen( PSP_CMDLINE_FILE, PSP_O_RDONLY, 0777 );
	if( fd < 0 )
		return in_argc;

	size = sceIoLseek( fd, 0, PSP_SEEK_END );
	sceIoLseek( fd, 0, PSP_SEEK_SET );

	buf = size > 0 ? malloc( size + 1 ) : NULL;
	if( !buf || sceIoRead( fd, buf, size ) != size )
	{
		free( buf );
		sceIoClose( fd );
		return in_argc;
	}
	sceIoClose( fd );
	buf[size] = 0;

	argv[argc++] = in_argc > 0 ? in_argv[0] : (char *)"xash";

	for( p = buf; *p && argc < PSP_MAX_ARGV - 1; )
	{
		while( *p && isspace( (unsigned char)*p ))
			*p++ = 0;

		if( !*p )
			break;

		argv[argc++] = p;

		while( *p && !isspace( (unsigned char)*p ))
			p++;
	}

	argv[argc] = NULL;
	*out_argv = argv;

	printf( "%s: %d arguments\n", PSP_CMDLINE_FILE, argc - 1 );

	return argc; // buf is never freed, argv points into it
}

/*
===========
PSP_LoadModule / PSP_UnloadModule

Load and start a PRX module, used by the library loader too
===========
*/
int PSP_LoadModule( const char *filename, int mpid, size_t argsize, void *argp )
{
	SceKernelLMOption option = { 0 };
	SceUID modid;
	int ret, status;

	option.size = sizeof( option );
	option.mpidtext = mpid;
	option.mpiddata = mpid;
	option.position = 0;
	option.access = 1;

	modid = sceKernelLoadModule( filename, 0, mpid ? &option : NULL );
	if( modid < 0 )
		return modid;

	ret = sceKernelStartModule( modid, argsize, argp, &status, NULL );
	if( ret < 0 )
	{
		sceKernelUnloadModule( modid );
		return ret;
	}

	return modid;
}

int PSP_UnloadModule( int modid, int *sce_code )
{
	int status = 0;

	*sce_code = sceKernelStopModule( modid, 0, NULL, &status, NULL );

	if( *sce_code < 0 )
		return -2;
	else if( status == SCE_KERNEL_ERROR_NOT_STOPPED )
		return -1;

	*sce_code = sceKernelUnloadModule( modid );

	return *sce_code < 0 ? -2 : 0;
}

/*
===========
PSP_UnlockVRAM

PSP-2000 and later have 4 MB of VRAM, only 2 MB are mapped by default.
ka.prx is a small kernel module that changes that, optional.
===========
*/
static void PSP_UnlockVRAM( void )
{
	int ret, code;
	int mod = PSP_LoadModule( "ka.prx", 1, 0, NULL );

	if( mod < 0 )
		return;

	ret = kaGeEdramGetHwSize();
	if( ret > 0 )
		kaGeEdramSetSize( ret );

	PSP_UnloadModule( mod, &code );
}

void PSP_Init( void )
{
	// disable FPU exceptions (division by zero and etc...)
	pspSdkDisableFPUExceptions();

	PSP_SetupCallbacks();

	// set max CPU/GPU frequency
	scePowerSetClockFrequency( 333, 333, 166 );

	PSP_UnlockVRAM();
}

void PSP_Shutdown( void )
{
}
