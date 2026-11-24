/*
gu_extension.c - PSP Graphic Unit extensions
Copyright (C) 2020 Sergey Galushko

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

#include <pspkernel.h>
#include <pspge.h>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstrict-prototypes"
#include <pspgu.h>
#pragma GCC diagnostic pop
#include <stdio.h>
#include "gu_extension.h"

/* from guInternal.h */
typedef struct
{
	unsigned int* start;
	unsigned int* current;
	int parent_context;
} GuDisplayList;
extern GuDisplayList* gu_list;
extern int gu_curr_context;
extern int ge_list_executed[];

static unsigned int gu_list_size;


void extGuStart(int cid, void* list, int size)
{
	gu_list_size = size;
	sceGuStart( cid, list );
}

/* bytes used in the current display list */
unsigned int extGuListUsed( void )
{
	return ( unsigned int )gu_list->current - ( unsigned int )gu_list->start;
}

/* GE id of the current display list */
int extGuListId( void )
{
	return ge_list_executed[0];
}

/*
In a GU_DIRECT list sceGuGetMemory and sceGuDrawArray update the GE's stall
address after every call, which is a syscall. These don't: batch them and
call extGuUpdateStall once at the end, the GE stops at the old stall address
until then.
*/
void *extGuGetMemoryNoStall( unsigned int size )
{
	unsigned int *current = gu_list->current;
	unsigned int *next = ( unsigned int * )(( unsigned int )current + (( size + 3 ) & ~3u ) + 8 );

	/* jump over the block */
	current[0] = ( 16 << 24 ) | ((( unsigned int )next >> 8 ) & 0xf0000 );
	current[1] = ( 8 << 24 ) | (( unsigned int )next & 0xffffff );
	gu_list->current = next;

	return current + 2;
}

void extGuDrawArrayNoStall( int prim, int vtype, int count, const void *vertices )
{
	unsigned int *p = gu_list->current;

	/* like sceGuDrawArray: 0 keeps the vertex type, NULL the address */
	if( vtype )
		*p++ = ( 18 << 24 ) | ( vtype & 0xffffff );  /* VTYPE */
	if( vertices )
	{
		*p++ = ( 16 << 24 ) | ((( unsigned int )vertices >> 8 ) & 0xf0000 ); /* BASE */
		*p++ = ( 1 << 24 ) | (( unsigned int )vertices & 0xffffff );  /* VADDR */
	}
	*p++ = ( 4 << 24 ) | ((( prim << 16 ) | count ) & 0xffffff ); /* PRIM */
	gu_list->current = p;
}

/*
extGuDrawArrayNoStall that moves the stall address every 16 draws: for code
that draws many small polygons (world, brush models, sprites, HUD), where a
syscall per draw cost more than the draw itself
*/
static unsigned int gu_batched_draws;

void extGuDrawArrayBatched( int prim, int vtype, int count, const void *vertices )
{
	extGuDrawArrayNoStall( prim, vtype, count, vertices );
	if( ++gu_batched_draws >= 16 )
		extGuUpdateStall();
}

void extGuUpdateStall( void )
{
	gu_batched_draws = 0;
	if( !gu_curr_context )
		sceGeListUpdateStallAddr( ge_list_executed[0], gu_list->current );
}

/* Begin user packet */
void *extGuBeginPacket( unsigned int *maxsize )
{
	unsigned int* current_ptr = gu_list->current;
	if( maxsize != NULL )
	{
		unsigned int size = ( ( unsigned int )gu_list->current ) - ( ( unsigned int )gu_list->start );	
		*maxsize = ( ( unsigned int )gu_list_size ) - ( ( unsigned int )size );
	}
	return current_ptr + 2;
}

/* End user packet */
void extGuEndPacket( void *eaddr )
{
	unsigned int* current_ptr = gu_list->current;
	unsigned int size = ( ( unsigned int ) eaddr ) - ( ( unsigned int )current_ptr );
	
	if( size > 0 )
	{
		size += 3;
		size += ( ( unsigned int )( size >> 31 ) ) >> 30;
		size = ( size >> 2 ) << 2;
		
		unsigned int* new_ptr = ( unsigned int* )( ( ( unsigned int ) current_ptr ) + size + 8 );

		int lo = ( 8 << 24 ) | ( ( ( unsigned int )new_ptr ) & 0xffffff );
		int hi = ( 16 << 24 ) | ( ( ( ( unsigned int )new_ptr ) >> 8 ) & 0xf0000 );

		current_ptr[0] = hi;
		current_ptr[1] = lo;
		
		gu_list->current = new_ptr;
		
		if ( !gu_curr_context )
			sceGeListUpdateStallAddr( ge_list_executed[0], new_ptr );
	}
}

/* raw GE commands: write words at the returned pointer, then pass the end to extGuCmdEnd */
unsigned int *extGuCmdBegin( void )
{
	return gu_list->current;
}

void extGuCmdEnd( unsigned int *p )
{
	gu_list->current = p;
}

/*
a block of commands for CALL: the list jumps over it. sceGu* calls between
extGuBlockBegin and extGuBlockEnd go into the block; returns its address
*/
unsigned int *extGuBlockBegin( void )
{
	unsigned int *start = gu_list->current;

	gu_list->current = start + 2;
	return start;
}

unsigned int *extGuBlockEnd( unsigned int *start )
{
	unsigned int *p = gu_list->current;
	unsigned int next;

	*p++ = 11 << 24; /* RET */
	next = ( unsigned int )p;
	start[0] = ( 16 << 24 ) | (( next >> 8 ) & 0xf0000 );
	start[1] = ( 8 << 24 ) | ( next & 0xffffff );
	gu_list->current = p;
	return start + 2;
}
