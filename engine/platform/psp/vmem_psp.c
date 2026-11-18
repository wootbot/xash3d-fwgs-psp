/*
vmem_psp.c - PSP volatile memory (partition 5) for big short-lived buffers
Copyright (C) 2022 Sergey Galushko
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

// The 4 MB of "volatile" memory are free while a game runs (the system uses them for
// suspend). Pools created with MEM_VOLATILE allocate from here and fall back to the heap
// when it's full: level changes allocate and free a few MB of save/restore and map file
// buffers, which otherwise fragment the heap until a big map allocation no longer fits.
// Every allocation is its own kernel block, which is fine for a handful of big buffers.

#include <pspkernel.h>
#include <pspsuspend.h>
#include "common.h"
#include "platform/platform.h"

#define VMEM_PARTITION 5
#define VMEM_HEADER    16 // keeps the data 16-byte aligned

static void   *vmem_base;
static int    vmem_size;
static qboolean vmem_locked;
static size_t vmem_used;
static int    vmem_blocks;

void PSP_VolatileInit( void )
{
	if( vmem_locked )
		return;

	int ret = sceKernelVolatileMemLock( 0, &vmem_base, &vmem_size );
	if( ret < 0 )
	{
		printf( "PSP_VolatileInit: can't lock volatile memory (0x%08x)\n", ret );
		return;
	}

	vmem_locked = true;
}

void PSP_VolatileShutdown( void )
{
	if( !vmem_locked )
		return;

	// everything should be freed by now, the blocks go away with the process anyway
	sceKernelVolatileMemUnlock( 0 );
	vmem_locked = false;
}

void *PSP_VolatileAlloc( size_t size )
{
	if( !vmem_locked )
		return NULL;

	SceUID uid = sceKernelAllocPartitionMemory( VMEM_PARTITION, "xash vmem", PSP_SMEM_Low, size + VMEM_HEADER, NULL );
	if( uid < 0 )
		return NULL;

	byte *block = sceKernelGetBlockHeadAddr( uid );
	*(SceUID *)block = uid;
	*(size_t *)( block + sizeof( SceUID )) = size;

	vmem_used += size;
	vmem_blocks++;

	return block + VMEM_HEADER;
}

qboolean PSP_IsVolatile( const void *ptr )
{
	return vmem_locked && (const byte *)ptr >= (const byte *)vmem_base && (const byte *)ptr < (const byte *)vmem_base + vmem_size;
}

void PSP_VolatileFree( void *ptr )
{
	byte *block = (byte *)ptr - VMEM_HEADER;

	vmem_used -= *(size_t *)( block + sizeof( SceUID ));
	vmem_blocks--;

	sceKernelFreePartitionMemory( *(SceUID *)block );
}

void PSP_VolatileInfo( size_t *used, int *blocks, size_t *total )
{
	*used = vmem_used;
	*blocks = vmem_blocks;
	*total = vmem_locked ? vmem_size : 0;
}
