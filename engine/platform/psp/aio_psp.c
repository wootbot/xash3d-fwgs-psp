/*
aio_psp.c - PSP: reads a file in the background
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
Opening a file on the memory stick takes ~30 ms, reading it a few more. One
file at a time: PSP_AioOpen, then PSP_AioRead once its size is known, or
PSP_AioReset. The thread only waits in the kernel, it uses no engine or libc
functions.
*/

#include "common.h"
#include "platform/platform.h"
#include <pspkernel.h>
#include <unistd.h>

static struct
{
	SceUID		thread, sema;	// the semaphore: a command
	volatile int	state;		// PSP_AIO
	char		path[MAX_SYSPATH * 2];
	SceUID		fd;
	int		size;
	byte		*buffer;
} aio = { -1, -1 };

static int PSP_AioThread( SceSize args, void *argp )
{
	while( sceKernelWaitSema( aio.sema, 1, NULL ) >= 0 )
	{
		if( aio.state == PSP_AIO_OPENING )
		{
			aio.fd = sceIoOpen( aio.path, PSP_O_RDONLY, 0 );
			aio.size = 0;
			if( aio.fd < 0 )
			{
				aio.state = PSP_AIO_FAILED;
				continue;
			}

			aio.size = sceIoLseek32( aio.fd, 0, PSP_SEEK_END );
			sceIoLseek32( aio.fd, 0, PSP_SEEK_SET );
			aio.state = PSP_AIO_OPEN;
		}
		else if( aio.state == PSP_AIO_READING )
		{
			int done = 0;

			while( done < aio.size )
			{
				const int n = sceIoRead( aio.fd, aio.buffer + done, aio.size - done );

				if( n <= 0 )
					break;
				done += n;
			}

			sceIoClose( aio.fd );
			aio.fd = -1;
			aio.state = done == aio.size ? PSP_AIO_DONE : PSP_AIO_FAILED;
		}
	}

	return 0;
}

int PSP_AioState( void )
{
	return aio.state;
}

int PSP_AioSize( void )
{
	return aio.size;
}

// only when idle
qboolean PSP_AioOpen( const char *path )
{
	char cwd[MAX_SYSPATH];

	if( aio.state != PSP_AIO_IDLE )
		return false;

	if( aio.thread < 0 )
	{
		if( aio.sema < 0 )
			aio.sema = sceKernelCreateSema( "xash_aio", 0, 0, 1, NULL );
		if( aio.sema < 0 )
			return false;

		// it hardly runs, but must not wait for the main thread to sleep
		aio.thread = sceKernelCreateThread( "xash_aio", PSP_AioThread, 0x1c, 0x1000, PSP_THREAD_ATTR_USER, NULL );
		if( aio.thread < 0 )
			return false;

		if( sceKernelStartThread( aio.thread, 0, NULL ) < 0 )
		{
			sceKernelDeleteThread( aio.thread );
			aio.thread = -1;
			return false;
		}
	}

	// the working directory is a thread's own
	if( !Q_strchr( path, ':' ) && getcwd( cwd, sizeof( cwd )))
	{
		if( Q_snprintf( aio.path, sizeof( aio.path ), "%s/%s", cwd, path ) < 0 )
			return false;
	}
	else Q_strncpy( aio.path, path, sizeof( aio.path ));

	aio.fd = -1;
	aio.state = PSP_AIO_OPENING;
	sceKernelSignalSema( aio.sema, 1 );
	return true;
}

// PSP_AIO_OPEN: all of it into buffer, that stays until PSP_AIO_DONE or PSP_AIO_FAILED
// and has whole cache lines of its own
void PSP_AioRead( void *buffer )
{
	if( aio.state != PSP_AIO_OPEN )
		return;

	aio.buffer = buffer;
	aio.state = PSP_AIO_READING;
	sceKernelSignalSema( aio.sema, 1 );
}

// idle again, waits for what's being done
void PSP_AioReset( void )
{
	while( aio.state == PSP_AIO_OPENING || aio.state == PSP_AIO_READING )
		sceKernelDelayThread( 1000 );

	if( aio.state == PSP_AIO_OPEN && aio.fd >= 0 )
		sceIoClose( aio.fd );

	aio.fd = -1;
	aio.state = PSP_AIO_IDLE;
}
