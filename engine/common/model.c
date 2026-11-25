/*
model.c - modelloader
Copyright (C) 2007 Uncle Mike

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/
#include "common.h"
#include "mod_local.h"
#include "sprite.h"
#include "xash3d_mathlib.h"
#include "alias.h"
#include "studio.h"
#include "wadfile.h"
#include "world.h"
#include "enginefeatures.h"
#include "client.h"
#if XASH_PSP
#include "sound.h"
#endif
#if XASH_PSP
#include "platform/platform.h"
#endif
#include "server.h"

static model_info_t	mod_crcinfo[MAX_MODELS];
// in chunks, they stay where they are. Most games have a few hundred
#define MOD_CHUNK_BITS	6
#define MOD_CHUNK_SIZE	( 1 << MOD_CHUNK_BITS )
static model_t	mod_first[MOD_CHUNK_SIZE];
static model_t	*mod_chunks[MAX_MODELS >> MOD_CHUNK_BITS] = { mod_first };
static int	mod_numknown = 0;
poolhandle_t      com_studiocache;		// cache for submodels
CVAR_DEFINE( mod_studiocache, "r_studiocache", "1", FCVAR_ARCHIVE, "enables studio cache for speedup tracing hitboxes" );
CVAR_DEFINE_AUTO( r_wadtextures, "0", FCVAR_LATCH, "completely ignore textures in the bsp-file if enabled" );
CVAR_DEFINE_AUTO( r_showhull, "0", 0, "draw collision hulls 1-3" );
CVAR_DEFINE_AUTO( r_allow_wad3_luma, "0", FCVAR_LATCH|FCVAR_ARCHIVE, "allow usage of luma textures in wad3 (tilde textures)" );

/*
===============================================================================

			MOD COMMON UTILS

===============================================================================
*/
static inline model_t *Mod_Known( int i )
{
	return &mod_chunks[i >> MOD_CHUNK_BITS][i & ( MOD_CHUNK_SIZE - 1 )];
}

static model_info_t *Mod_CRCInfo( const model_t *mod )
{
	for( int i = 0; i < ARRAYSIZE( mod_chunks ) && mod_chunks[i]; i++ )
	{
		if( mod >= mod_chunks[i] && mod < mod_chunks[i] + MOD_CHUNK_SIZE )
			return &mod_crcinfo[( i << MOD_CHUNK_BITS ) + ( mod - mod_chunks[i] )];
	}

	Host_Error( "%s: unknown model %s\n", __func__, mod->name );
	return mod_crcinfo;
}

/*
================
Mod_Modellist_f
================
*/
static void Mod_Modellist_f( void )
{
	int	i, nummodels;

	Con_Printf( "\n" );
	Con_Printf( "-----------------------------------\n" );

	for( i = nummodels = 0; i < mod_numknown; i++ )
	{
		const model_t *mod = Mod_Known( i );
		const char *color_str;

		if( mod->needload == NL_UNREFERENCED )
			continue; // free slot

		switch( mod->type )
		{
		case mod_alias:
			color_str = S_YELLOW;
			break;
		case mod_studio:
			color_str = S_GREEN;
			break;
		case mod_sprite:
			color_str = S_MAGENTA;
			break;
		case mod_brush:
			color_str = mod->name[0] == '*' ? S_CYAN : S_BLUE;
			break;
		default:
			color_str = S_RED;
			break;
		}

		Con_Printf( "%3d:\t%s%s\n" S_DEFAULT, i, color_str, mod->name );
		nummodels++;
	}

	Con_Printf( "-----------------------------------\n" );
	Con_Printf( "%i total models, %i total allocated slots\n", nummodels, mod_numknown );
	Con_Printf( "\n" );
}

static void Mod_UnloadRenderData( model_t *mod )
{
#if !XASH_DEDICATED
	// loader failed before renderer got a chance to process this model
	if( mod->type == mod_bad )
		return;

	switch( mod->type )
	{
	case mod_sprite:
		Mod_SpriteUnloadTextures( mod->cache.data );
		break;
	default:
		break;
	}

	ref.dllFuncs.Mod_ProcessRenderData( mod, false, NULL, 0 );
#endif
}

/*
================
Mod_FreeUserData
================
*/
static void Mod_FreeUserData( model_t *mod )
{
	// ignore submodels and freed models
	if( mod->needload == NL_UNREFERENCED || mod->name[0] == '*' )
		return;

	if( Host_IsDedicated() )
	{
		if( svgame.physFuncs.Mod_ProcessUserData != NULL )
		{
			// let the server.dll free custom data
			svgame.physFuncs.Mod_ProcessUserData( mod, false, NULL );
		}
	}
	else
	{
		Mod_UnloadRenderData( mod );
	}
}

/*
================
Mod_FreeModel
================
*/
void Mod_FreeModel( model_t *mod )
{
	// already freed?
	if( !mod || mod->needload == NL_UNREFERENCED )
		return;

	if( mod->type != mod_brush || mod->name[0] != '*' )
	{
#if XASH_PSP
		PSP_MeWait();
#endif
		Mod_FreeUserData( mod );
		Mem_FreePool( &mod->mempool );
	}

	if( mod->type == mod_brush && FBitSet( mod->flags, MODEL_WORLD ) )
	{
		world.version = 0;
		world.shadowdata = NULL;
		world.deluxedata = NULL;

		// data already freed by Mem_FreePool above
		world.hull_models = NULL;
		world.compressed_phs = NULL;
		world.phsofs = NULL;
	}

	memset( mod, 0, sizeof( *mod ));
}

/*
===============================================================================

			MODEL INITIALIZE\SHUTDOWN

===============================================================================
*/
/*
================
Mod_Init
================
*/
void Mod_Init( void )
{
	com_studiocache = Mem_AllocPool( "Studio Cache" );
	Cvar_RegisterVariable( &mod_studiocache );
	Cvar_RegisterVariable( &r_wadtextures );
	Cvar_RegisterVariable( &r_showhull );
	Cvar_RegisterVariable( &r_allow_wad3_luma );

	Cmd_AddCommand( "mapstats", Mod_PrintWorldStats_f, "show stats for currently loaded map" );
	Cmd_AddCommand( "modellist", Mod_Modellist_f, "display loaded models list" );

	Mod_ResetStudioAPI ();
	Mod_InitStudioHull ();
}

/*
================
Mod_FreeAll
================
*/
void Mod_FreeAll( void )
{
#if !XASH_DEDICATED
	Mod_ReleaseHullPolygons();
#endif
	for( int i = 0; i < mod_numknown; i++ )
		Mod_FreeModel( Mod_Known( i ));
	mod_numknown = 0;
}

/*
================
Mod_ClearUserData
================
*/
void Mod_ClearUserData( void )
{
	for( int i = 0; i < mod_numknown; i++ )
		Mod_FreeUserData( Mod_Known( i ));
}

/*
================
Mod_Shutdown
================
*/
void Mod_Shutdown( void )
{
	Mod_FreeAll();
	Mem_FreePool( &com_studiocache );
}

/*
===============================================================================

			MODELS MANAGEMENT

===============================================================================
*/
/*
==================
Mod_FindName

never return NULL
==================
*/
model_t *Mod_FindName( const char *filename, qboolean trackCRC )
{
	char	modname[MAX_QPATH];
	model_t	*mod = NULL;
	int	i;

	Q_strncpy( modname, filename, sizeof( modname ));

	// search the currently loaded models
	for( i = 0; i < mod_numknown; i++ )
	{
		mod = Mod_Known( i );
		if( !Q_stricmp( mod->name, modname ))
		{
			if( mod->mempool || mod->name[0] == '*' )
				mod->needload = NL_PRESENT;
			else
				mod->needload = NL_NEEDS_LOADED;

			return mod;
		}
	}

	// find a free model slot spot
	for( i = 0; i < mod_numknown; i++ )
	{
		mod = Mod_Known( i );
		if( mod->needload == NL_UNREFERENCED )
			break; // this is a valid spot
	}

	if( i == mod_numknown )
	{
		if( mod_numknown == MAX_MODELS )
			Host_Error( "MAX_MODELS limit exceeded (%d)\n", MAX_MODELS );
		if( !mod_chunks[i >> MOD_CHUNK_BITS] )
			mod_chunks[i >> MOD_CHUNK_BITS] = Mem_Calloc( host.mempool, sizeof( model_t ) * MOD_CHUNK_SIZE );
		mod = Mod_Known( i );
		mod_numknown++;
	}

	// copy name, so model loader can find model file
	Q_strncpy( mod->name, modname, sizeof( mod->name ));
	if( trackCRC ) mod_crcinfo[i].flags = FCRC_SHOULD_CHECKSUM;
	else mod_crcinfo[i].flags = 0;
	mod->needload = NL_NEEDS_LOADED;
	mod_crcinfo[i].initialCRC = 0;

	return mod;
}

/*
==================
Mod_LoadModel

Loads a model into the cache
==================
*/
#if XASH_PSP
// a map or a model too big for the PSP's memory: drop it and go back to the
// menu (Host_Error), not out of the game. Called by Mem_OutOfMemory.
static model_t *mod_loading;
static byte *mod_loading_buf;

void Mod_LoadOutOfMemory( const char *func, size_t size, const char *filename, int fileline )
{
	model_t *mod = mod_loading;
	char name[MAX_QPATH];

	if( !mod )
		return;

	mod_loading = NULL;
	Q_strncpy( name, mod->name, sizeof( name ));
	Con_Printf( S_ERROR "%s: out of memory (alloc size %s at %s:%i) loading %s\n", func, Q_memprint( size ), filename, fileline, name );

	Mod_AbortBmodelLoad();
	Mod_FreeModel( mod );
	if( mod_loading_buf )
		Mem_Free( mod_loading_buf );
	Host_Error( "Not enough memory to load %s\n", name );
}
#endif

#if MOD_STREAM_BSP
// NULL if the file isn't a map
static file_t *Mod_OpenMap( const char *name, fs_offset_t *length )
{
	file_t *f = FS_Open( name, "rb", false );
	uint ident;

	if( !f )
		return NULL;

	if( FS_Read( f, &ident, sizeof( ident )) == sizeof( ident ))
	{
		switch( ident )
		{
		case LittleLong( Q1BSP_VERSION ):
		case LittleLong( HLBSP_VERSION ):
		case LittleLong( QBSP2_VERSION ):
			*length = FS_FileLength( f );
			return f;
		}
	}

	FS_Close( f );
	return NULL;
}

static uint32_t Mod_FileCRC( const char *name )
{
	uint32_t crc;
	byte buf[1024];
	file_t *f = FS_Open( name, "rb", false );

	CRC32_Init( &crc );
	for( int len; f && ( len = FS_Read( f, buf, sizeof( buf ))) > 0; )
		CRC32_ProcessBuffer( &crc, buf, len );
	if( f ) FS_Close( f );

	return CRC32_Final( crc );
}

/*
==================
Mod_LoadMap

Mod_LoadModel for brush models, the file isn't loaded as a whole
==================
*/
static model_t *Mod_LoadMap( model_t *mod, file_t *f, fs_offset_t length, qboolean crash )
{
	qboolean loaded, loaded2 = false;
	char name[MAX_QPATH];

	Q_strncpy( name, mod->name, sizeof( name ));
	COM_FixSlashes( name );
	Con_Reportf( "loading %s\n", mod->name );
	mod->needload = NL_PRESENT;
	mod->type = mod_bad;

#if XASH_PSP
	mod_loading = mod;
	mod_loading_buf = NULL;
#endif

	Mod_LoadBrushModelFile( mod, f, length, &loaded );

	if( loaded )
	{
		if( world.loading )
			SetBits( mod->flags, MODEL_WORLD ); // mark worldmodel

		if( Host_IsDedicated() )
			loaded2 = true;
#if !XASH_DEDICATED
		else loaded2 = ref.dllFuncs.Mod_ProcessRenderData( mod, true, NULL, 0 );
#endif
	}

#if XASH_PSP
	mod_loading = NULL;
#endif

	if( !loaded || !loaded2 )
	{
		Mod_FreeModel( mod );

		if( crash ) Host_Error( "Could not load model %s\n", name );
		else Con_Printf( S_ERROR "Could not load model %s\n", name );

		return NULL;
	}

	model_info_t *p = Mod_CRCInfo( mod );
	mod->needload = NL_PRESENT;

	if( FBitSet( p->flags, FCRC_SHOULD_CHECKSUM ))
	{
		const uint32_t currentCRC = Mod_FileCRC( name );

		if( FBitSet( p->flags, FCRC_CHECKSUM_DONE ))
		{
			if( currentCRC != p->initialCRC )
				Host_Error( "%s has a bad checksum\n", name );
		}
		else
		{
			SetBits( p->flags, FCRC_CHECKSUM_DONE );
			p->initialCRC = currentCRC;
		}
	}

	return mod;
}
#endif

#if XASH_PSP
/*
==============================================================================

STUDIO MODELS ON DEMAND

A map's models may be more than fit. While a map is loading, studio models are only
loaded as long as enough memory is left for the rest (Mod_KeepCold). The others are
cold: no more than their bounds is known, Mod_Use loads them when they are needed and
frees the ones that weren't used for the longest time.

Cold models have no extradata: Mod_Use keeps them away from the renderer and the
client library. Loading and freeing is only done between two frames (Mod_LoadCold).

==============================================================================
*/
#define MOD_REQUESTS	16
#define MOD_IDLE_TIME	2.0f	// not used for that long: may be freed
#define MOD_RETRY_TIME	1.0f
#define MOD_MARGIN		( 2 * 1024 * 1024 )	// left for everything else
#define MOD_WARM_BLOCK	( 384 * 1024 )	// in one piece for Mod_Warm

// studio models don't use numleafs (here: when it was last used) and numplanes
// (here: not loaded before this time, -1 never)

static model_t *mod_requests[MOD_REQUESTS];
static int mod_numrequests;
static size_t mod_reserve;	// != 0: a map is loading, this much is needed besides the models

static qboolean Mod_IsCold( const model_t *mod )
{
	return mod->type == mod_studio && !mod->cache.data && mod->mempool;
}

// memory the model takes: what is left of the file, textures
static size_t Mod_StudioCost( const studiohdr_t *hdr, size_t filesize )
{
	const size_t data = hdr->numtextures > 0 && hdr->texturedataindex > 0 && (size_t)hdr->texturedataindex < filesize ? hdr->texturedataindex : filesize;
	const size_t pixels = filesize - data;

	// the renderer's copies of the meshes, and textures of at most ~128 KB (R_StudioLimitTextures)
	return data + data / 2 + ( hdr->numtextures > 0 ? Q_min( pixels, 192 * 1024 ) : 192 * 1024 );
}

static qboolean Mod_StudioHeader( const char *name, studiohdr_t *hdr, fs_offset_t *filesize, int *bodies )
{
	file_t *f = FS_Open( name, "rb", false );
	qboolean ok;

	if( !f )
		return false;

	*filesize = FS_FileLength( f );
	ok = FS_Read( f, hdr, sizeof( *hdr )) == sizeof( *hdr ) && hdr->ident == IDSTUDIOHEADER && hdr->version == STUDIO_VERSION;

	if( ok )
	{
		// as Mod_StudioBodyVariations
		int count = 1;

		if( hdr->numbodyparts < 0 || hdr->numbodyparts > 64 || FS_Seek( f, hdr->bodypartindex, SEEK_SET ) < 0 )
			ok = false;

		for( int i = 0; ok && i < hdr->numbodyparts; i++ )
		{
			mstudiobodyparts_t part;

			if( FS_Read( f, &part, sizeof( part )) != sizeof( part ))
				ok = false;
			else count *= part.nummodels;
		}
		*bodies = count;
	}

	FS_Close( f );
	return ok;
}

/*
==================
Mod_BeginPrecache

the map is loaded and its models are next
==================
*/
void Mod_BeginPrecache( void )
{
	const model_t *w = mod_first;

	mod_numrequests = 0;
	mod_reserve = 0;

	if( !w || w->type != mod_brush || SV_Active( ))
		return;

	// the renderer's copy of the world: surfaces, nodes, lightmaps, polygons
	size_t polys = w->numsurfedges * 40 + w->numsurfaces * 32;

	mod_reserve = w->numsurfaces * 34 + ( w->numnodes + w->numleafs ) * 36 + w->nummarksurfaces * 2;
	mod_reserve += ( w->numsurfaces - w->nummodelsurfaces ) * 32;
	mod_reserve += polys > 2 * 1024 * 1024 ? 1536 * 1024 : polys;

	if( w->lightdata )
	{
		size_t luxels = 0;

		for( int i = 0; i < w->numsurfaces; i++ )
		{
			const mextrasurf_t *info = w->surfaces[i].info;

			if( w->surfaces[i].samples && info )
				luxels += ( info->lightextents[0] / 16 + 1 ) * ( info->lightextents[1] / 16 + 1 );
		}
		mod_reserve += luxels + luxels / 4;
	}

	// entities, sprites, sounds, sky, decals, temp entities, and some to spare
	mod_reserve += 9 * 1024 * 1024;
	Con_Reportf( "%s: %s free, %s needed besides the models\n", __func__, Q_memprint( PSP_HeapFree( )), Q_memprint( mod_reserve ));
}

void Mod_EndPrecache( void )
{
	mod_reserve = 0;
}

// leave it on the stick for now?
static qboolean Mod_KeepCold( model_t *mod, const char *name )
{
	studiohdr_t hdr;
	fs_offset_t filesize;
	int bodies;

	// players' own models are drawn with whatever there is
	if( !mod_reserve || PSP_HeapFree() >= mod_reserve + 2 * 1024 * 1024 || !Q_strnicmp( name, "models/player", 13 ) || Q_stricmp( COM_FileExtension( name ), "mdl" ))
		return false;

	if( !Mod_StudioHeader( name, &hdr, &filesize, &bodies ))
		return false;

	if( PSP_HeapFree() >= mod_reserve + Mod_StudioCost( &hdr, filesize ))
		return false;

	mod->type = mod_studio;
	mod->mempool = Mem_AllocPool( va( "^2%s^7", mod->name ));
	mod->cache.data = NULL;
	if( !VectorCompare( vec3_origin, hdr.bbmin ))
	{
		VectorCopy( hdr.bbmin, mod->mins );
		VectorCopy( hdr.bbmax, mod->maxs );
	}
	else if( !VectorCompare( vec3_origin, hdr.min ))
	{
		VectorCopy( hdr.min, mod->mins );
		VectorCopy( hdr.max, mod->maxs );
	}
	else
	{
		// computed from the vertices
		byte *buf = FS_LoadFileToPool( host.volatilepool, name, NULL, false );

		if( buf )
		{
			Mod_StudioHullBounds( buf, mod->mins, mod->maxs );
			Mem_Free( buf );
		}
	}
	mod->numframes = bodies;
	mod->radius = RadiusFromBounds( mod->mins, mod->maxs );
	mod->flags = hdr.flags;
	mod->needload = NL_PRESENT;
	mod->numleafs = 0;
	Con_Reportf( "%s stays cold\n", mod->name );

	return true;
}

static void Mod_Chill( model_t *mod )
{
	PSP_MeWait();
	Con_Reportf( "%s is cold\n", mod->name );
	ref.dllFuncs.Mod_ProcessRenderData( mod, false, NULL, 0 );
	Mem_FreePool( &mod->mempool );
	mod->mempool = Mem_AllocPool( va( "^2%s^7", mod->name ));
	mod->cache.data = NULL;
	mod->textures = NULL;
	mod->numtextures = 0;
}

// frees models until there is enough memory, and a block of that size in one piece
static qboolean Mod_MakeRoom( size_t size, size_t block )
{
	const int idle = host.realtime * 10 - MOD_IDLE_TIME * 10;

	while( PSP_HeapFree() < size || !PSP_HeapHasBlock( block ))
	{
		model_t *oldest = NULL;

		for( int i = 1; i < mod_numknown; i++ )
		{
			model_t *mod = Mod_Known( i );

			// the ones with remapped textures are players
			if( mod->type != mod_studio || !mod->cache.data || mod->numtextures || mod->numleafs > idle || !Q_strnicmp( mod->name, "models/player", 13 ))
				continue;
			if( !oldest || mod->numleafs < oldest->numleafs )
				oldest = mod;
		}

		if( oldest )
			Mod_Chill( oldest );
		else if( !S_FreeIdleSound( ))
			return false;
	}

	return true;
}

/*
==================
Mod_MakeRoomFor

an allocation failed in a game: can it be tried again?
==================
*/
qboolean Mod_MakeRoomFor( size_t size )
{
	static qboolean active;
	qboolean ok;

	if( active || mod_loading || cls.state != ca_active || SV_Active( ))
		return false;

	active = true;
	ok = Mod_MakeRoom( size, size );
	active = false;

	return ok;
}

static void Mod_Warm( model_t *mod )
{
	studiohdr_t hdr;
	fs_offset_t filesize, length = 0;
	char name[MAX_QPATH];
	qboolean loaded = false;
	int bodies;

	Q_strncpy( name, mod->name, sizeof( name ));
	COM_FixSlashes( name );

	// failed: not again
	mod->numplanes = -1;
	if( !Mod_StudioHeader( name, &hdr, &filesize, &bodies ))
		return;

	// what stays of the file in one piece, the file itself goes into P5 RAM
	const size_t data = hdr.texturedataindex > 0 && hdr.texturedataindex < filesize ? hdr.texturedataindex : filesize;

	// a block for the texture conversions too (up to 256 KB): when one fails here it's a
	// Host_Error (Mod_LoadOutOfMemory), with 2.4 MB free in pieces of < 300 KB
	if( !Mod_MakeRoom( Mod_StudioCost( &hdr, filesize ) + MOD_MARGIN, Q_max( data + 64 * 1024, MOD_WARM_BLOCK )))
	{
		// try again later
		mod->numplanes = host.realtime + MOD_RETRY_TIME + 1.0f;
		return;
	}

	Mem_SetSoftFail( true );
	byte *buf = FS_LoadFileToPool( host.volatilepool, name, &length, false );
	if( Mem_SetSoftFail( false ) || !buf )
	{
		mod->numplanes = host.realtime + MOD_RETRY_TIME + 1.0f;
		return;
	}

	Con_Reportf( "loading %s, %s free\n", mod->name, Q_memprint( PSP_HeapFree( )));
	Mem_FreePool( &mod->mempool );
	mod->textures = NULL;
	mod->numtextures = 0;
	mod_loading = mod;
	mod_loading_buf = buf;

	Mod_LoadStudioModel( mod, buf, length, &loaded );
	if( loaded )
		loaded = ref.dllFuncs.Mod_ProcessRenderData( mod, true, buf, length );
	else mod->cache.data = NULL;

	mod_loading = NULL;
	Mem_Free( buf );
	if( loaded )
		mod->numplanes = 0;
}

/*
==================
Mod_LoadCold

the models that were asked for, between two frames
==================
*/
void Mod_LoadCold( void )
{
	// one a frame
	if( mod_numrequests > 0 )
	{
		model_t *mod = mod_requests[0];

		mod_numrequests--;
		memmove( mod_requests, mod_requests + 1, sizeof( mod_requests[0] ) * mod_numrequests );

		if( Mod_IsCold( mod ))
			Mod_Warm( mod );
	}
}

/*
==================
Mod_Use

the model is about to be drawn: false if it's cold
==================
*/
qboolean Mod_Use( model_t *mod )
{
	if( !mod )
		return false;

	if( mod->type != mod_studio )
		return true;

	// 1/10 s
	mod->numleafs = host.realtime * 10;

	if( !Mod_IsCold( mod ))
		return true;

	if( mod->numplanes < 0 || mod->numplanes > host.realtime || mod_numrequests == MOD_REQUESTS )
		return false;

	for( int i = 0; i < mod_numrequests; i++ )
	{
		if( mod_requests[i] == mod )
			return false;
	}

	mod_requests[mod_numrequests++] = mod;
	return false;
}

/*
==================
Mod_FreeUnlisted

studio models of the last map that the next one doesn't have, before it's loaded
==================
*/
void Mod_FreeUnlisted( qboolean (*listed)( const char *name ))
{
	mod_numrequests = 0;

	for( int i = 1; i < mod_numknown; i++ )
	{
		model_t *mod = Mod_Known( i );

		if( mod->type == mod_studio && mod->mempool && !listed( mod->name ))
			Mod_FreeModel( mod );
	}
}
#endif

static model_t *Mod_LoadModel( model_t *mod, qboolean crash )
{
	char		tempname[MAX_QPATH];
	fs_offset_t		length = 0;
	qboolean		loaded, loaded2 = false;

	if( !mod )
	{
		Host_Error( "%s: mod == NULL\n", __func__ );
		return NULL;
	}

	// check if already loaded (or inline bmodel)
	if( mod->mempool || mod->name[0] == '*' )
	{
		mod->needload = NL_PRESENT;
		return mod;
	}

	if( mod->needload != NL_NEEDS_LOADED )
	{
		Host_Error( "%s: trying to load model not marked for loading (%d)\n", __func__, mod->needload );
		return NULL;
	}

	// store modelname to show error
	Q_strncpy( tempname, mod->name, sizeof( tempname ));
	COM_FixSlashes( tempname );

#if MOD_STREAM_BSP
	file_t *bsp = Mod_OpenMap( tempname, &length );
	if( bsp )
		return Mod_LoadMap( mod, bsp, length, crash );
#endif

#if XASH_PSP
	if( Mod_KeepCold( mod, tempname ))
		return mod;

	// the file is only needed while loading, keep it out of the heap (P5 RAM)
	byte *buf = FS_LoadFileToPool( host.volatilepool, tempname, &length, false );
#else
	byte *buf = FS_LoadFile( tempname, &length, false );
#endif

	if( !buf || length < sizeof( uint ))
	{
		memset( mod, 0, sizeof( model_t ));

		if( crash ) Host_Error( "Could not load model %s from disk\n", tempname );
		else Con_Printf( S_ERROR "Could not load model %s from disk\n", tempname );

		return NULL;
	}

	Con_Reportf( "loading %s\n", mod->name );
	mod->needload = NL_PRESENT;
	mod->type = mod_bad;

#if XASH_PSP
	mod_loading = mod;
	mod_loading_buf = buf;
#endif

	// call the apropriate loader
	switch( *(uint *)buf )
	{
	case LittleLong( IDSTUDIOHEADER ):
		Mod_LoadStudioModel( mod, buf, length, &loaded );
		break;
	case LittleLong( IDSPRITEHEADER ):
		Mod_LoadSpriteModel( mod, buf, length, &loaded );
		break;
	case LittleLong( IDALIASHEADER ):
		Mod_LoadAliasModel( mod, buf, &loaded );
		break;
	case LittleLong( Q1BSP_VERSION ):
	case LittleLong( HLBSP_VERSION ):
	case LittleLong( QBSP2_VERSION ):
		Mod_LoadBrushModel( mod, buf, length, &loaded );
		break;
	default:
#if XASH_PSP
		mod_loading = NULL;
#endif
		Mem_Free( buf );
		if( crash ) Host_Error( "%s has unknown format\n", tempname );
		else Con_Printf( S_ERROR "%s has unknown format\n", tempname );
		return NULL;
	}

	if( loaded )
	{
		if( world.loading )
			SetBits( mod->flags, MODEL_WORLD ); // mark worldmodel

		if( Host_IsDedicated() )
		{
			if( svgame.physFuncs.Mod_ProcessUserData != NULL )
			{
				// let the server.dll load custom data
				svgame.physFuncs.Mod_ProcessUserData( mod, true, buf );
			}
			loaded2 = true;
		}
#if !XASH_DEDICATED
		else
		{
			loaded2 = ref.dllFuncs.Mod_ProcessRenderData( mod, true, buf, length );
		}
#endif
	}

#if XASH_PSP
	mod_loading = NULL;
#endif

	if( mod->type == mod_alias )
	{
		aliashdr_t *hdr = mod->cache.data;
		if( hdr ) // clean up temporary pointer after passing the alias model to the renderer
			hdr->pposeverts = NULL;
	}

	if( !loaded || !loaded2 )
	{
		Mod_FreeModel( mod );
		Mem_Free( buf );

		if( crash ) Host_Error( "Could not load model %s\n", tempname );
		else Con_Printf( S_ERROR "Could not load model %s\n", tempname );

		return NULL;
	}

	model_info_t *p = Mod_CRCInfo( mod );
	mod->needload = NL_PRESENT;

	if( FBitSet( p->flags, FCRC_SHOULD_CHECKSUM ))
	{
		uint32_t currentCRC;

		CRC32_Init( &currentCRC );
		CRC32_ProcessBuffer( &currentCRC, buf, length );
		currentCRC = CRC32_Final( currentCRC );

		if( FBitSet( p->flags, FCRC_CHECKSUM_DONE ))
		{
			if( currentCRC != p->initialCRC )
				Host_Error( "%s has a bad checksum\n", tempname );
		}
		else
		{
			SetBits( p->flags, FCRC_CHECKSUM_DONE );
			p->initialCRC = currentCRC;
		}
	}
	Mem_Free( buf );

	return mod;
}

/*
==================
Mod_ForName

Loads in a model for the given name
==================
*/
model_t *Mod_ForName( const char *name, qboolean crash, qboolean trackCRC )
{
	if( COM_StringEmptyOrNULL( name ))
		return NULL;

	model_t *mod = Mod_FindName( name, trackCRC );
	return Mod_LoadModel( mod, crash );
}

/*
==================
Mod_PurgeStudioCache

free studio cache on change level
==================
*/
static void Mod_PurgeStudioCache( void )
{
	// refresh hull data
	SetBits( r_showhull.flags, FCVAR_CHANGED );
#if !XASH_DEDICATED
	Mod_ReleaseHullPolygons();
#endif
	// release previois map
	Mod_FreeModel( mod_first );	// world is stuck on slot #0 always

	// we should release all the world submodels
	// and clear studio sequences
	for( int i = 1; i < mod_numknown; i++ )
	{
		model_t *mod = Mod_Known( i );

		if( mod->needload == NL_UNREFERENCED )
			continue;

		if( mod->type == mod_studio )
			mod->submodels = NULL;

		if( mod->name[0] == '*' )
			Mod_FreeModel( mod );
		else
			mod->needload = NL_FREE_UNUSED;
	}

	Mem_EmptyPool( com_studiocache );
	Mod_ClearStudioCache();
}

/*
==================
Mod_LoadWorld

Loads in the map and all submodels
==================
*/
model_t *Mod_LoadWorld( const char *name, qboolean preload )
{
	// already loaded?
	if( !Q_stricmp( mod_first->name, name ))
		return mod_first;

	// free sequence files on studiomodels
	Mod_PurgeStudioCache();

	// load the newmap
	world.loading = true;
	model_t *pworld = Mod_FindName( name, false );
	if( preload ) Mod_LoadModel( pworld, true );
	world.loading = false;

	ASSERT( pworld == mod_first );

	return pworld;
}

/*
==================
Mod_FreeUnused

Purge all unused models
==================
*/
void Mod_FreeUnused( void )
{
	// never tries to release worldmodel
	for( int i = 1; i < mod_numknown; i++ )
	{
		model_t *mod = Mod_Known( i );

		if( mod->needload == NL_FREE_UNUSED )
			Mod_FreeModel( mod );
	}
}

/*
===============================================================================

			MODEL ROUTINES

===============================================================================
*/
/*
===============
Mod_Calloc

===============
*/
void *GAME_EXPORT Mod_Calloc( int number, size_t size )
{
	if( number <= 0 || size <= 0 )
		return NULL;

	cache_user_t *cu = (cache_user_t *)Mem_Calloc( com_studiocache, sizeof( cache_user_t ) + number * size );
	cu->data = (void *)cu; // make sure that cu->data is not NULL

	return cu;
}

/*
===============
Mod_CacheCheck

===============
*/
void *GAME_EXPORT Mod_CacheCheck( cache_user_t *c )
{
	if( !c->data )
		return NULL;

	if( !Mem_IsAllocatedExt( com_studiocache, c->data ))
		return NULL;

	return c->data;
}

/*
===============
Mod_LoadCacheFile

===============
*/
void GAME_EXPORT Mod_LoadCacheFile( const char *filename, cache_user_t *cu )
{
	char	modname[MAX_QPATH];
	fs_offset_t	size;

	Assert( cu != NULL );

	if( COM_StringEmptyOrNULL( filename ))
		return;

	Q_strncpy( modname, filename, sizeof( modname ));
	COM_FixSlashes( modname );

	byte *buf = FS_LoadFile( modname, &size, false );
	if( !buf || !size ) Host_Error( "LoadCacheFile: ^1can't load %s^7\n", filename );
	cu->data = Mem_Malloc( com_studiocache, size );
	memcpy( cu->data, buf, size );
	Mem_Free( buf );

	// this handles when studio model renderer tries to load sequence files on it's own
	// which is what they always do in HLSDK
#if XASH_BIG_ENDIAN
	if( size >= sizeof( int ) && LittleLong( IDSEQGRPHEADER ) == *(uint *)cu->data )
	{
		studiohdr_t *phdr = (studiohdr_t *)REF_GET_PARM( PARM_GET_STUDIO_HDR, 0 );
		if( !phdr )
			return;

		mstudioseqdesc_t *pseq = (mstudioseqdesc_t *)((byte *)phdr + phdr->seqindex );

		for( int i = 0; i < phdr->numseq; i++ )
		{
			if( pseq[i].seqgroup == 0 )
				continue;

			mstudioseqgroup_t *pgrp = (mstudioseqgroup_t *)((byte *)phdr + phdr->seqgroupindex ) + pseq[i].seqgroup;

			// assuming filename passes seqgroup's name
			if( !Q_stricmp( pgrp->name, filename ))
				Mod_SwapStudioSeqGroupAnims( phdr, &pseq[i], (byte *)cu->data );
		}
	}
#endif
}

/*
==================
Mod_ValidateCRC

==================
*/
qboolean Mod_ValidateCRC( const char *name, uint32_t crc )
{
	model_t *mod = Mod_FindName( name, true );
	model_info_t *p = Mod_CRCInfo( mod );

	if( !FBitSet( p->flags, FCRC_CHECKSUM_DONE ))
		return true;
	if( p->initialCRC == crc )
		return true;
	return false;
}

/*
==================
Mod_NeedCRC

==================
*/
void Mod_NeedCRC( const char *name, qboolean needCRC )
{
	model_t *mod = Mod_FindName( name, true );
	model_info_t *p = Mod_CRCInfo( mod );

	if( needCRC ) SetBits( p->flags, FCRC_SHOULD_CHECKSUM );
	else ClearBits( p->flags, FCRC_SHOULD_CHECKSUM );
}

#if XASH_LLVM_LIBFUZZER

static const uint8_t *fuzz_data;
static size_t fuzz_size;

static byte *Fuzz_LoadFile( const char *path, fs_offset_t *filesizeptr, qboolean gamedironly )
{
	byte *buf = Mem_Malloc( host.mempool, fuzz_size );
	memcpy( buf, fuzz_data, fuzz_size );
	*filesizeptr = fuzz_size;
	return buf;
}

int EXPORT Fuzz_Mod_LoadModel( const uint8_t *Data, size_t Size );
int EXPORT Fuzz_Mod_LoadModel( const uint8_t *Data, size_t Size )
{
	model_t mod = { .name = "test", .needload = NL_NEEDS_LOADED };

	Memory_Init();

	host.type = HOST_DEDICATED;
	host.mempool = Mem_AllocPool( "fuzzing pool" );
	fuzz_data = Data;
	fuzz_size = Size;
	refState.draw_surfaces = NULL;

	g_fsapi.LoadFile = Fuzz_LoadFile;

	if( Mod_LoadModel( &mod, false ) && mod.mempool )
		Mem_FreePool( &mod.mempool );

	Mem_FreePool( &host.mempool );

	return 0;
}

#endif // XASH_LLVM_LIBFUZZER
