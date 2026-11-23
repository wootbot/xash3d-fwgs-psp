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
#include <pspge.h>
#include <pspctrl.h>
#include <pspdebug.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <malloc.h>

PSP_MODULE_INFO( "Xash3D", PSP_MODULE_USER, 1, 0 );
PSP_MAIN_THREAD_ATTR( PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU );
PSP_MAIN_THREAD_STACK_SIZE_KB( 512 );
// current pspsdk: any negative heap size means "all free memory minus the threshold",
// the threshold is left for the game PRX modules (server and client, ~2.6 MB of files) and the
// Wi-Fi modules, loaded later: with 3 MB only ~32 KB were left once Wi-Fi was up, and a
// client PRX 24 KB bigger made the inet module fail to load (0x800208d9)
PSP_HEAP_SIZE_KB( -1 );
PSP_HEAP_THRESHOLD_SIZE_KB( 3 * 1024 + 256 );

#define PSP_MAX_ARGV     32
#define PSP_CMDLINE_FILE "start.cmd"

static int PSP_PowerCallback( int count, int arg, void *common )
{
	// TODO: suspend/resume, see P5 RAM in the old port
	return 0;
}

/*
===========
PSP_ExitCallback

HOME, then Yes in the system's dialog. The main thread quits on its next frame
the normal way (config saved, Wi-Fi left); if it doesn't within 5 s (stuck,
or in a long load), the game is ended from here
===========
*/
static volatile qboolean psp_exit_requested;

static int PSP_ExitCallback( int count, int arg, void *common )
{
	psp_exit_requested = true;
	sceKernelDelayThread( 5 * 1000 * 1000 );
	PSP_MeShutdown();
	sceKernelExitGame();
	return 0;
}

qboolean PSP_ExitRequested( void )
{
	return psp_exit_requested;
}

static int PSP_CallbackThread( SceSize args, void *argp )
{
	int cbid = sceKernelCreateCallback( "Exit Callback", PSP_ExitCallback, NULL );
	sceKernelRegisterExitCallback( cbid );

	cbid = sceKernelCreateCallback( "Power Callback", PSP_PowerCallback, NULL );
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

/*
===========
PSP_WatchdogThread

a main thread spinning without ever blocking starves every lower priority
thread, PSPLink's shell included, and looks like a hard freeze. If frames stop
for a while, drop the main thread to the lowest priority so PSPLink can
answer again (thctx <main uid> shows where it is stuck); restore it once
frames come back (long map loads trigger this too, harmlessly)
===========
*/
#define PSP_WATCHDOG_SECONDS 15

static SceUID psp_main_thread;

// -nostdout: console and watchdog output only go to the log files, so a PSPLink
// run from ms0: leaves the USB link idle
qboolean psp_nostdout;
static size_t PSP_LargestFreeBlock( void );

// to stdout and to watchdog.log next to the game, for runs without PSPLink
void PSP_WatchdogLog( qboolean truncate, const char *fmt, ... )
{
	char buf[1100];
	va_list args;
	int len;

	va_start( args, fmt );
	len = vsnprintf( buf, sizeof( buf ), fmt, args );
	va_end( args );

	if( len < 0 )
		return;
	if( len >= sizeof( buf ))
		len = sizeof( buf ) - 1;

	if( !psp_nostdout )
		printf( "%s", buf );
	PSP_DbgConSendNow( buf ); // straight out, the main thread may be stuck

	// libc open: it resolves relative paths against the libc cwd, sceIoOpen
	// in a new thread doesn't
	int fd = open( "watchdog.log", O_WRONLY | O_CREAT | ( truncate ? O_TRUNC : O_APPEND ), 0777 );
	if( fd >= 0 )
	{
		write( fd, buf, len );
		close( fd );
	}
}

// main thread stack high-water mark, the stack starts out filled with 0xff
static void PSP_CheckMainStack( void )
{
	static SceKernelThreadInfo info;
	static size_t reported;

	if( !info.size )
	{
		info.size = sizeof( info );
		if( sceKernelReferThreadStatus( psp_main_thread, &info ) < 0 || !info.stack )
			return;
	}

	const uint32_t *p = (const uint32_t *)info.stack + 4; // skip the thread id at the bottom
	const uint32_t *end = (const uint32_t *)((byte *)info.stack + info.stackSize );

	while( p < end && *p == 0xffffffff )
		p++;

	size_t used = (byte *)end - (byte *)p;

	if( used >= reported + 8 * 1024 )
	{
		reported = used;
		PSP_WatchdogLog( false, "STACK: main thread used %u of %u KB\n", (uint)( used >> 10 ), (uint)( info.stackSize >> 10 ));
	}
}

const char * volatile psp_stage[PSP_STAGE_MAX] = { "-", "-" };
volatile int psp_stage_depth = 2; // [0] host stage and [1] client stage are always there

// what the stuck main thread is doing: the engine stages it entered last and its kernel
// state (waiting on what, or running: then the run clocks keep growing between reports)
static void PSP_WatchdogReportStall( void )
{
	static SceKernelThreadInfo info;
	char stages[512];
	int len = 0, depth = psp_stage_depth;

	stages[0] = 0;
	for( int i = 0; i < depth && i < PSP_STAGE_MAX && len < (int)sizeof( stages ); i++ )
		len += snprintf( stages + len, sizeof( stages ) - len, "%s%s", i ? " > " : "", psp_stage[i] ? psp_stage[i] : "?" );

	memset( &info, 0, sizeof( info ));
	info.size = sizeof( info );
	if( sceKernelReferThreadStatus( psp_main_thread, &info ) < 0 )
	{
		PSP_WatchdogLog( false, "STALL: stage %s (depth %d), no thread status\n", stages, depth );
		return;
	}

	PSP_WatchdogLog( false, "STALL: stage %s (depth %d) | status 0x%x wait type %d id 0x%x prio %d run clocks %u ms, preempted intr %u thread %u, release %u\n",
		stages, depth, info.status, info.waitType, (uint)info.waitId, info.currentPriority,
		(uint)( info.runClocks.low / 1000 ), info.intrPreemptCount, info.threadPreemptCount, info.releaseCount );
}

static int PSP_WatchdogThread( SceSize args, void *argp )
{
	int last = -1, still = 0, seconds = 0;
	qboolean demoted = false;

	while( 1 )
	{
		sceKernelDelayThread( 1000 * 1000 );
		PSP_CheckMainStack();

		// heartbeat, the last one tells when a hard freeze happened. On the
		// stick only without the PC console, once a minute: each append
		// holds up the game's own file reads for ~130 ms
		if( ++seconds % 5 == 0 )
		{
			char msg[128];

			Q_snprintf( msg, sizeof( msg ), "WATCHDOG: %d s, frame %d, net pool %d bytes free\n", seconds, host.framecount, PSP_NetPoolFree( ));
			if( !PSP_DbgConSendNow( msg ) && seconds % 60 == 0 )
				PSP_WatchdogLog( false, "%s", msg );
			else if( !psp_nostdout )
				printf( "%s", msg );
		}

		if( host.framecount != last )
		{
			last = host.framecount;
			still = 0;

			if( demoted )
			{
				sceKernelChangeThreadPriority( psp_main_thread, 0x20 );
				PSP_WatchdogLog( false, "WATCHDOG: frames again, main thread priority restored\n" );
				demoted = false;
			}
			continue;
		}

		if( ++still == PSP_WATCHDOG_SECONDS && !demoted )
		{
			// demote first: printf can block on the stdio lock or the USB link
			sceKernelChangeThreadPriority( psp_main_thread, 0x7e );
			demoted = true;
			PSP_WatchdogLog( false, "WATCHDOG: no frame for %d s (frame %d), main thread 0x%08x demoted\n",
				PSP_WATCHDOG_SECONDS, last, (uint)psp_main_thread );
		}

		if( still >= PSP_WATCHDOG_SECONDS && ( still - PSP_WATCHDOG_SECONDS ) % 10 == 0 )
			PSP_WatchdogReportStall();

		if( still >= PSP_WATCHDOG_SECONDS )
			PSP_DbgConPollStalled(); // psp_restart from the PC
	}

	return 0;
}

static void PSP_StartWatchdog( void )
{
	psp_main_thread = sceKernelGetThreadId();

	int thid = sceKernelCreateThread( "xash_watchdog", PSP_WatchdogThread, 0x12, 0x8000, 0, NULL ); // vsnprintf + open() need a few KB each

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

void PSP_DelayMicroseconds( int usec )
{
	sceKernelDelayThreadCB( usec > 0 ? usec : 1 );
}

void Platform_ShellExecute( const char *path, const char *parms )
{
}

#if XASH_MESSAGEBOX == MSGBOX_PSP
void Platform_MessageBox( const char *title, const char *message, qboolean parentMainWindow )
{
	static qboolean init = false;
	SceCtrlData pad;

	// also to watchdog.log and the Wi-Fi debug console: the dialog blocks the main thread
	PSP_WatchdogLog( false, "%s: %s\n", title, message );

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
sceGeEdramSetSize changes that, it's a kernel function: through ka.prx (a small
kernel module, optional), the user stub or the custom firmware's kernel call
bridge, whichever works. A PSP-1000 refuses 4 MB.
===========
*/
#define PSP_VRAM_FULL_SIZE	0x400000
#define PSP_NID_GE_EDRAM_SET_SIZE	0x5BAA5439

// SystemCtrlForUser and KUBridge from PRO/ARK, declared here: their headers clash with pspsdk ones
typedef struct
{
	uint32_t arg[12];
	uint32_t ret1, ret2;
} psp_kcall_args_t;
unsigned int sctrlHENFindFunction( const char *mod_name, const char *library, unsigned int nid );
int kuKernelCall( void *func_addr, psp_kcall_args_t *args );

static char psp_vram_status[128] = "not tried";

static void PSP_UnlockVRAM( void )
{
	psp_kcall_args_t args = { 0 };
	int ret, code = 0, mod;
	unsigned int func;

	if( sceGeEdramGetSize() >= PSP_VRAM_FULL_SIZE )
	{
		Q_strncpy( psp_vram_status, "4 MB already mapped", sizeof( psp_vram_status ));
		return;
	}

	mod = PSP_LoadModule( "ka.prx", 1, 0, NULL );
	if( mod >= 0 )
	{
		ret = kaGeEdramGetHwSize();
		if( ret > 0 )
			kaGeEdramSetSize( ret );

		PSP_UnloadModule( mod, &code );

		if( sceGeEdramGetSize() >= PSP_VRAM_FULL_SIZE )
		{
			Q_strncpy( psp_vram_status, "4 MB through ka.prx", sizeof( psp_vram_status ));
			return;
		}
	}

	ret = sceGeEdramSetSize( PSP_VRAM_FULL_SIZE );
	if( sceGeEdramGetSize() >= PSP_VRAM_FULL_SIZE )
	{
		Q_strncpy( psp_vram_status, "4 MB through sceGe_user", sizeof( psp_vram_status ));
		return;
	}

	// kernel address of sceGeEdramSetSize, error codes are below 0x88000000
	func = sctrlHENFindFunction( "sceGE_Manager", "sceGe_driver", PSP_NID_GE_EDRAM_SET_SIZE );
	if( func >= 0x88000000 && func < 0x8C000000 )
	{
		args.arg[0] = PSP_VRAM_FULL_SIZE;
		code = kuKernelCall( (void *)func, &args );

		if( sceGeEdramGetSize() >= PSP_VRAM_FULL_SIZE )
		{
			Q_strncpy( psp_vram_status, "4 MB through the kernel call bridge", sizeof( psp_vram_status ));
			return;
		}
	}

	Q_snprintf( psp_vram_status, sizeof( psp_vram_status ),
		"2 MB (ka.prx 0x%08x, sceGe_user 0x%08x, sceGe_driver 0x%08x, kernel call 0x%08x ret 0x%08x)",
		mod, ret, func, code, (uint)args.ret1 );
}


/*
===========
PSP_MemInfo_f

the engine heap is one newlib arena sized at startup (all free memory minus
PSP_HEAP_THRESHOLD_SIZE_KB), memlist only shows what the engine pools hold
===========
*/
static size_t PSP_LargestFreeBlock( void )
{
	size_t lo = 0, hi = 64 << 20;

	// binary search for the biggest malloc that still succeeds
	while( hi - lo > 4096 )
	{
		size_t mid = lo + ( hi - lo ) / 2;
		// volatile, or gcc drops the malloc/free pair and assumes success
		void *volatile p = malloc( mid );

		if( p )
		{
			free( p );
			lo = mid;
		}
		else hi = mid;
	}

	return lo;
}

void PSP_PrintMemInfo( void )
{
	struct mallinfo mi = mallinfo();

	Con_Printf( "heap: %u KB in use, %u KB free inside the arena (arena %u KB)\n",
		(uint)( mi.uordblks >> 10 ), (uint)( mi.fordblks >> 10 ), (uint)( mi.arena >> 10 ));
	Con_Printf( "heap: largest malloc %u KB\n", (uint)( PSP_LargestFreeBlock() >> 10 ));
	Con_Printf( "kernel: %u KB free, largest block %u KB (game modules come from here)\n",
		(uint)( sceKernelTotalFreeMemSize() >> 10 ), (uint)( sceKernelMaxFreeMemSize() >> 10 ));

	size_t vused, vtotal;
	int vblocks;

	PSP_VolatileInfo( &vused, &vblocks, &vtotal );
	Con_Printf( "P5 RAM: %u KB in use in %d blocks (of %u KB)\n", (uint)( vused >> 10 ), vblocks, (uint)( vtotal >> 10 ));
	Con_Printf( "VRAM: %s\n", psp_vram_status );
}

void PSP_RegisterCommands( void )
{
	Cmd_AddCommand( "psp_meminfo", PSP_PrintMemInfo, "print free heap and kernel memory" );
	PSP_NetRegisterCommands();
	PSP_MeRegisterCommands();
	PSP_DbgConInit();
}

void PSP_Init( void )
{
	// disable FPU exceptions (division by zero and etc...)
	pspSdkDisableFPUExceptions();

	psp_nostdout = Sys_CheckParm( "-nostdout" );
	if( psp_nostdout ) // stray printf()s too
		freopen( "stdout.log", "w", stdout );

	PSP_SetupCallbacks();

	// compare XMB and PSPLink launches, they don't get the same memory
	PSP_WatchdogLog( true, "start: heap %u KB free (largest malloc), kernel %u KB free (largest block %u KB)\n",
		(uint)( PSP_LargestFreeBlock() >> 10 ), (uint)( sceKernelTotalFreeMemSize() >> 10 ),
		(uint)( sceKernelMaxFreeMemSize() >> 10 ));

	PSP_StartWatchdog();

	// set max CPU/GPU frequency
	scePowerSetClockFrequency( 333, 333, 166 );

	PSP_UnlockVRAM();
	PSP_WatchdogLog( false, "VRAM: %s\n", psp_vram_status );

	// P5 RAM for MEM_VOLATILE pools
	PSP_VolatileInit();
}

void PSP_Shutdown( void )
{
	PSP_MeShutdown();
	PSP_NetShutdown();
	PSP_VolatileShutdown();
}
