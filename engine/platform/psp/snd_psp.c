/*
snd_psp.c - PSP sound backend
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

/*
A thread copies the engine's DMA buffer into the SRC audio channel, which resamples the engine's
SOUND_DMA_SPEED (11 kHz) to the hardware rate. sceAudioSRCOutputBlocking waits until the
hardware needs the next block, that sets the pace. The semaphore keeps
the mixer and the thread out of the buffer at the same time.
*/

#include "platform/platform.h"
#if XASH_SOUND == SOUND_PSP
#include <malloc.h>
#include <pspaudio.h>
#include <pspkernel.h>
#include "sound.h"

#define PSP_NUM_AUDIO_SAMPLES  256 // per channel, must be multiple of 64 (23 ms at 11 kHz)
#define PSP_OUTPUT_CHANNELS    2
#define PSP_OUTPUT_BUFFER_SIZE ( PSP_NUM_AUDIO_SAMPLES * PSP_OUTPUT_CHANNELS )

static struct
{
	SceUID thread;
	SceUID sema;
	int channel;
	volatile qboolean running;
	volatile qboolean active;
} snd_psp = { -1, -1, -1 };

static short snd_psp_buf[2][PSP_OUTPUT_BUFFER_SIZE] __attribute__(( aligned( 64 )));

static int PSP_SoundThread( SceSize args, void *argp )
{
	int index = 0;

	while( snd_psp.running )
	{
		short *out = snd_psp_buf[index];

		sceKernelWaitSema( snd_psp.sema, 1, NULL );

		if( snd_psp.active && snd.buffer )
		{
			const short *in = (const short *)snd.buffer;
			int len = PSP_OUTPUT_BUFFER_SIZE;
			int pos = snd.samplepos;
			int wrapped = pos + len - snd.samples;

			if( wrapped < 0 )
			{
				memcpy( out, in + pos, len * sizeof( short ));
				snd.samplepos += len;
			}
			else
			{
				int remaining = snd.samples - pos;

				memcpy( out, in + pos, remaining * sizeof( short ));
				if( wrapped > 0 )
					memcpy( out + remaining, in, wrapped * sizeof( short ));
				snd.samplepos = wrapped;
			}
		}
		else memset( out, 0, PSP_OUTPUT_BUFFER_SIZE * sizeof( short ));

		sceKernelSignalSema( snd_psp.sema, 1 );

		sceAudioSRCOutputBlocking( PSP_AUDIO_VOLUME_MAX, out );
		index = !index;
	}

	return 0;
}

qboolean SNDDMA_Init( void )
{
	int samplecount;

	snd.format.speed    = SOUND_DMA_SPEED;
	snd.format.channels = PSP_OUTPUT_CHANNELS;
	snd.format.width    = 2;

	samplecount = s_samplecount.value;
	if( !samplecount )
		samplecount = 0x4000;

	// must be a multiple of the output block
	samplecount = ( samplecount + PSP_NUM_AUDIO_SAMPLES - 1 ) & ~( PSP_NUM_AUDIO_SAMPLES - 1 );

	snd.samples   = samplecount * PSP_OUTPUT_CHANNELS;
	snd.samplepos = 0;
	snd.buffer    = Mem_Calloc( sndpool, snd.samples * 2 );

	snd_psp.channel = sceAudioSRCChReserve( PSP_NUM_AUDIO_SAMPLES, SOUND_DMA_SPEED, PSP_OUTPUT_CHANNELS );
	if( snd_psp.channel < 0 )
	{
		Con_Printf( S_ERROR "Audio: sceAudioSRCChReserve failed: 0x%08x\n", (uint)snd_psp.channel );
		SNDDMA_Shutdown();
		return false;
	}

	snd_psp.sema = sceKernelCreateSema( "sound_sema", 0, 1, 1, NULL );
	if( snd_psp.sema < 0 )
	{
		SNDDMA_Shutdown();
		return false;
	}

	snd_psp.running = true;
	snd_psp.active = true;

	snd_psp.thread = sceKernelCreateThread( "sound_thread", PSP_SoundThread, 0x12, 0x8000, 0, NULL );
	if( snd_psp.thread < 0 || sceKernelStartThread( snd_psp.thread, 0, NULL ) < 0 )
	{
		Con_Printf( S_ERROR "Audio: can't start the output thread\n" );
		SNDDMA_Shutdown();
		return false;
	}

	Con_Printf( "Using PSP audio driver: %d Hz\n", snd.format.speed );

	snd.initialized = true;
	snd.backend_name = "PSP Audio";

	return true;
}

// the mixer nests BeginPainting/Submit pairs (S_UpdateChannels -> S_PaintChannels),
// they're only called from the engine thread, so count the depth instead of a recursive lock
static int snd_psp_lock_depth;

void SNDDMA_BeginPainting( void )
{
	if( snd_psp.sema >= 0 && snd_psp_lock_depth++ == 0 )
		sceKernelWaitSema( snd_psp.sema, 1, NULL );
}

void SNDDMA_Submit( void )
{
	if( snd_psp.sema >= 0 && snd_psp_lock_depth > 0 && --snd_psp_lock_depth == 0 )
		sceKernelSignalSema( snd_psp.sema, 1 );
}

void SNDDMA_Shutdown( void )
{
	Con_Printf( "Shutting down audio.\n" );

	snd.initialized = false;
	snd_psp.running = false;

	if( snd_psp.thread >= 0 )
	{
		sceKernelWaitThreadEnd( snd_psp.thread, NULL );
		sceKernelDeleteThread( snd_psp.thread );
		snd_psp.thread = -1;
	}

	if( snd_psp.sema >= 0 )
	{
		sceKernelDeleteSema( snd_psp.sema );
		snd_psp.sema = -1;
	}

	if( snd_psp.channel >= 0 )
	{
		sceAudioSRCChRelease();
		snd_psp.channel = -1;
	}

	if( snd.buffer )
	{
		Mem_Free( snd.buffer );
		snd.buffer = NULL;
	}
}

void SNDDMA_Activate( qboolean active )
{
	snd_psp.active = active;
}

// no microphone
qboolean VoiceCapture_Init( void )
{
	return false;
}

qboolean VoiceCapture_Activate( qboolean activate )
{
	return false;
}

qboolean VoiceCapture_Lock( qboolean lock )
{
	return false;
}

void VoiceCapture_Shutdown( void )
{
}

#endif // XASH_SOUND == SOUND_PSP
