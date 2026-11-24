/*
s_load.c - sounds managment
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
#include "client.h"
#include "sound.h"

// during registration it is possible to have more sounds
// than could actually be referenced during gameplay,
// because we don't want to free anything until we are
// sure we won't need it.
#define MAX_SFX      8192
#define MAX_SFX_HASH (MAX_SFX/4)

static int      s_numSfx = 0;

// in chunks, they stay where they are. Most maps have a few hundred
#define SFX_CHUNK_BITS 7
#define SFX_CHUNK_SIZE ( 1 << SFX_CHUNK_BITS )
static sfx_t    s_firstSfx[SFX_CHUNK_SIZE];
static sfx_t    *s_sfxChunks[MAX_SFX >> SFX_CHUNK_BITS] = { s_firstSfx };

static sfx_t    *s_sfxHashList[MAX_SFX_HASH];
static qboolean s_registering = false;

#define SENTENCE_INDEX -99999 // unique sentence index
static string   s_sentenceImmediateName;	// keep dummy sentence name

static inline sfx_t *S_KnownSfx( int i )
{
	return &s_sfxChunks[i >> SFX_CHUNK_BITS][i & ( SFX_CHUNK_SIZE - 1 )];
}

static int S_SfxIndex( const sfx_t *sfx )
{
	for( int i = 0; i < ARRAYSIZE( s_sfxChunks ) && s_sfxChunks[i]; i++ )
	{
		if( sfx >= s_sfxChunks[i] && sfx < s_sfxChunks[i] + SFX_CHUNK_SIZE )
			return ( i << SFX_CHUNK_BITS ) + ( sfx - s_sfxChunks[i] );
	}

	return -1;
}

/*
=================
S_SoundList_f
=================
*/
void S_SoundList_f( void )
{
	sfx_t	*sfx;
	int	i, totalSfx = 0;
	int	totalSize = 0;

	for( i = 0; i < s_numSfx; i++ )
	{
		sfx = S_KnownSfx( i );
		if( !sfx->name[0] )
			continue;

		wavdata_t *sc = sfx->cache;
		if( sc )
		{
			totalSize += sc->size;

			if( FBitSet( sc->flags, SOUND_LOOPED ))
				Con_Printf( "L" );
			else
				Con_Printf( " " );

			if( sfx->name[0] == '*' || !Q_strncmp( sfx->name, DEFAULT_SOUNDPATH, sizeof( DEFAULT_SOUNDPATH ) - 1 ))
				Con_Printf( " (%2db) %s : %s\n", sc->width * 8, Q_memprint( sc->size ), sfx->name );
			else Con_Printf( " (%2db) %s : " DEFAULT_SOUNDPATH "%s\n", sc->width * 8, Q_memprint( sc->size ), sfx->name );
			totalSfx++;
		}
	}

	Con_Printf( "-------------------------------------------\n" );
	Con_Printf( "%i total sounds\n", totalSfx );
	Con_Printf( "%s total memory\n", Q_memprint( totalSize ));
	Con_Printf( "\n" );
}

/*
=================
S_CreateDefaultSound
=================
*/
static wavdata_t *S_CreateDefaultSound( void )
{
	uint samples = SOUND_DMA_SPEED;
	uint channels = 1;
	uint width = 2;
	size_t size = samples * width * channels;

	wavdata_t *sc = Mem_Calloc( sndpool, sizeof( wavdata_t ) + size );

	sc->width = width;
	sc->channels = channels;
	sc->rate = SOUND_DMA_SPEED;
	sc->samples = samples;
	sc->size = size;

	return sc;
}

static wavdata_t *S_ProcessSound( wavdata_t *sc )
{
	if( sc->rate < SOUND_11k ) // some bad sounds
		Sound_Process( &sc, SOUND_11k, sc->width, sc->channels, SOUND_RESAMPLE );
	else if( sc->rate > SOUND_11k && sc->rate < SOUND_22k ) // some bad sounds
		Sound_Process( &sc, SOUND_22k, sc->width, sc->channels, SOUND_RESAMPLE );
	else if( sc->rate > SOUND_22k && sc->rate != SOUND_44k ) // some bad sounds
		Sound_Process( &sc, SOUND_44k, sc->width, sc->channels, SOUND_RESAMPLE );

	return sc;
}

/*
=================
S_LoadSound
=================
*/
wavdata_t *S_LoadSound( sfx_t *sfx )
{
	wavdata_t	*sc = NULL;

	if( !sfx ) return NULL;

	// see if still in memory
	if( sfx->cache )
		return sfx->cache;

	if( COM_StringEmptyOrNULL( sfx->name ))
		return NULL;

	// load it from disk
	if( Q_stricmp( sfx->name, "*default" ))
	{
		// load it from disk
		if( s_warn_late_precache.value > 0 && cls.state == ca_active )
			Con_Printf( S_WARN "%s: late precache of %s\n", __func__, sfx->name );

#if XASH_PSP
		// sounds load on first use (see S_EndRegistration), when the heap may
		// be full: skip the sound then instead of stopping the game, it's
		// tried again next time it plays
		Mem_SetSoftFail( true );
#endif
		if( sfx->name[0] == '*' )
			sc = FS_LoadSound( sfx->name + 1, NULL, 0 );
		else
			sc = FS_LoadSound( sfx->name, NULL, 0 );
#if XASH_PSP
		if( Mem_SetSoftFail( false ) && !sc )
			return NULL;
#endif
	}

	if( !sc ) sc = S_CreateDefaultSound();

	sfx->cache = S_ProcessSound( sc );

	return sfx->cache;
}

#if XASH_PSP
/*
=============================================================================

Sounds load when they're first played (S_EndRegistration), and most of the
~40 ms that takes is the memory stick looking for the file. So in a game they're
requested: a thread reads the file, their channels wait for it.

=============================================================================
*/
#define S_LOAD_QUEUE	64	// a power of 2

// cache lines of its own: the stick's driver drops them from the cache while the main thread
// runs, what it wrote next to the buffer would be lost
#define S_LOAD_ALIGNED( p )	((byte *)(((uintptr_t)( p ) + 63 ) & ~63 ))

CVAR_DEFINE_AUTO( s_async, "1", FCVAR_ARCHIVE, "load sounds in the background" );

static struct
{
	struct
	{
		sfx_t	*sfx;
		char	name[MAX_QPATH];	// it may be another sound's by then
	} queue[S_LOAD_QUEUE];
	uint	head, tail;
	byte	*buffer;
	string	path;	// of the one being read, as FS_LoadSound wants it
} s_loadq;

qboolean S_SoundPending( const sfx_t *sfx )
{
	for( uint i = s_loadq.head; i != s_loadq.tail; i++ )
	{
		if( s_loadq.queue[i % S_LOAD_QUEUE].sfx == sfx )
			return true;
	}

	return false;
}

// true: it's on its way. False: it's there, or S_LoadSound it
qboolean S_RequestSound( sfx_t *sfx )
{
	if( !sfx || sfx->cache )
		return false;

	if( S_SoundPending( sfx ))
		return true;

	if( !s_async.value || cls.state != ca_active || s_registering
		|| s_loadq.tail - s_loadq.head >= S_LOAD_QUEUE || COM_StringEmpty( sfx->name ) || sfx->name[0] == '!' || !Q_stricmp( sfx->name, "*default" ))
		return false;

	s_loadq.queue[s_loadq.tail % S_LOAD_QUEUE].sfx = sfx;
	Q_strncpy( s_loadq.queue[s_loadq.tail % S_LOAD_QUEUE].name, sfx->name, sizeof( s_loadq.queue[0].name ));
	s_loadq.tail++;
	return true;
}

// the sounds they're for go, or the files
static void S_LoadCancel( void )
{
	PSP_AioReset();
	if( s_loadq.buffer )
		Mem_Free( s_loadq.buffer );
	s_loadq.buffer = NULL;
	s_loadq.head = s_loadq.tail = 0;
}

// a file of the sound on the stick
static qboolean S_LoadPath( const char *name, string diskpath )
{
	static const char *const exts[] = { "wav", "mp3" };
	const char *ext = COM_FileExtension( name );

	if( name[0] == '*' )
		name++;

	for( int i = 0; i < ARRAYSIZE( exts ); i++ )
	{
		const qboolean has = !Q_stricmp( ext, exts[i] );

		// another extension is part of the name
		if( !has && ( !Q_stricmp( ext, "wav" ) || !Q_stricmp( ext, "mp3" )))
			continue;

		if( Q_snprintf( s_loadq.path, sizeof( s_loadq.path ), has ? "%s%s" : "%s%s.%s", DEFAULT_SOUNDPATH, name, exts[i] ) < 0 )
			return false;

		if( g_fsapi.GetFullDiskPath( diskpath, sizeof( string ), s_loadq.path, false ))
			return true;
	}

	return false;
}

// once a frame
void S_LoadThink( void )
{
	string diskpath;

	if( s_loadq.head == s_loadq.tail )
		return;

	sfx_t *sfx = s_loadq.queue[s_loadq.head % S_LOAD_QUEUE].sfx;
	const qboolean wanted = !sfx->cache && !Q_strcmp( sfx->name, s_loadq.queue[s_loadq.head % S_LOAD_QUEUE].name );

	switch( PSP_AioState( ))
	{
	case PSP_AIO_IDLE:
		if( !wanted )
		{
			s_loadq.head++;
			return;
		}

		// in an archive, or not there
		if( !S_LoadPath( sfx->name, diskpath ) || !PSP_AioOpen( diskpath ))
		{
			s_loadq.head++;
			S_LoadSound( sfx );
		}
		return;
	case PSP_AIO_OPEN:
		Mem_SetSoftFail( true );
		if( wanted && PSP_AioSize() > 0 )
			s_loadq.buffer = Mem_Malloc( sndpool, PSP_AioSize() + 128 );
		Mem_SetSoftFail( false );

		if( s_loadq.buffer )
		{
			PSP_AioRead( S_LOAD_ALIGNED( s_loadq.buffer ));
			return;
		}
		break; // no memory, no sound this time
	case PSP_AIO_DONE:
		if( wanted )
		{
			string name;
			wavdata_t *sc;

			Q_snprintf( name, sizeof( name ), "#%s", s_loadq.path );
			Mem_SetSoftFail( true );
			sc = FS_LoadSound( name, S_LOAD_ALIGNED( s_loadq.buffer ), PSP_AioSize( ));
			if( !Mem_SetSoftFail( false ) || sc )
				sfx->cache = S_ProcessSound( sc ? sc : S_CreateDefaultSound( ));
		}
		break;
	case PSP_AIO_FAILED:
		if( wanted )
			sfx->cache = S_ProcessSound( S_CreateDefaultSound( ));
		break;
	default:
		return;
	}

	PSP_AioReset();
	if( s_loadq.buffer )
		Mem_Free( s_loadq.buffer );
	s_loadq.buffer = NULL;
	s_loadq.head++;
}
#endif

/*
==================
S_FindName

==================
*/
sfx_t *S_FindName( const char *pname, qboolean *pfInCache )
{
	sfx_t	*sfx;
	uint	i;
	string	name;

	if( COM_StringEmptyOrNULL( pname ) || !snd.initialized )
		return NULL;

	if( Q_strlen( pname ) >= sizeof( sfx->name ))
		return NULL;

	Q_strncpy( name, pname, sizeof( name ));
	COM_FixSlashes( name );

	// see if already loaded
	uint hash = COM_HashKey( name, MAX_SFX_HASH );
	for( sfx = s_sfxHashList[hash]; sfx; sfx = sfx->hashNext )
	{
		if( !Q_strcmp( sfx->name, name ))
		{
			if( pfInCache )
			{
				// indicate whether or not sound is currently in the cache.
				*pfInCache = ( sfx->cache != NULL ) ? true : false;
			}
			// prolonge registration
			sfx->servercount = cl.servercount;
			return sfx;
		}
	}

	// find a free sfx slot spot
	for( i = 0; i < s_numSfx; i++ )
		if( !S_KnownSfx( i )->name[0] ) break; // free spot

	if( i == s_numSfx )
	{
		if( s_numSfx == MAX_SFX )
			return NULL;
		if( !s_sfxChunks[i >> SFX_CHUNK_BITS] )
			s_sfxChunks[i >> SFX_CHUNK_BITS] = Mem_Calloc( host.mempool, sizeof( sfx_t ) * SFX_CHUNK_SIZE );
		s_numSfx++;
	}

	sfx = S_KnownSfx( i );
	memset( sfx, 0, sizeof( *sfx ));
	if( pfInCache ) *pfInCache = false;
	Q_strncpy( sfx->name, name, sizeof( sfx->name ));
	sfx->servercount = cl.servercount;
	sfx->hashValue = COM_HashKey( sfx->name, MAX_SFX_HASH );

	// link it in
	sfx->hashNext = s_sfxHashList[sfx->hashValue];
	s_sfxHashList[sfx->hashValue] = sfx;

	return sfx;
}

/*
==================
S_FreeSound
==================
*/
void S_FreeSound( sfx_t *sfx )
{
	if( !sfx || !sfx->name[0] )
		return;

	// de-link it from the hash tree
	sfx_t **prev = &s_sfxHashList[sfx->hashValue];
	while( 1 )
	{
		sfx_t *hashSfx = *prev;
		if( !hashSfx )
			break;

		if( hashSfx == sfx )
		{
			*prev = hashSfx->hashNext;
			break;
		}
		prev = &hashSfx->hashNext;
	}

	if( clgame.soundFuncs.pfnS_FreeSound )
	{
		clgame.soundFuncs.pfnS_FreeSound( sfx, S_SfxIndex( sfx ));
		return;
	}

#if XASH_PSP
	S_PaintSync(); // the Media Engine may be mixing it
#endif
	if( sfx->cache )
		FS_FreeSound( sfx->cache );
	memset( sfx, 0, sizeof( *sfx ));
}

/*
=====================
S_BeginRegistration

=====================
*/
void S_BeginRegistration( void )
{
	snd.have_ambient_sfx = false;

	// check for automatic ambient sounds
	for( int i = 0; i < NUM_AMBIENTS; i++ )
	{
		if( !GI->ambientsound[i][0] )
			continue;	// empty slot

		snd.ambient_sfx[i] = S_RegisterSound( GI->ambientsound[i] );
		if( snd.ambient_sfx[i] )
			snd.have_ambient_sfx = true; // allow auto-ambients
	}

	s_registering = true;
}

/*
=====================
S_EndRegistration

=====================
*/
void S_EndRegistration( void )
{
	sfx_t	*sfx;
	int	i;

	if( !s_registering || !snd.initialized )
		return;

#if XASH_PSP
	S_LoadCancel();
#endif

	// free any sounds not from this registration sequence
	for( i = 0; i < s_numSfx; i++ )
	{
		sfx = S_KnownSfx( i );
		if( !sfx->name[0] || !Q_stricmp( sfx->name, "*default" ))
			continue; // don't release default sound

		if( sfx->servercount != cl.servercount )
			S_FreeSound( sfx ); // don't need this sound
	}

#if XASH_PSP
	// list their directories now (dir.c): the first lookup in one takes ~40 ms
	for( i = 0; i < s_numSfx; i++ )
	{
		sfx = S_KnownSfx( i );
		string path;

		if( sfx->name[0] && sfx->name[0] != '!' && Q_snprintf( path, sizeof( path ), DEFAULT_SOUNDPATH "%s", sfx->name[0] == '*' ? sfx->name + 1 : sfx->name ) > 0 )
			g_fsapi.FileExists( path, false );
	}
	g_fsapi.FileExists( DEFAULT_SOUNDPATH "fvox/bell.wav", false );

	vox_kept_bytes = 0; // the sentence words kept by VOX_FreeWord went with the old map's sounds
#endif

#if !XASH_PSP
	// load everything in
	// (not on the PSP: big co-op maps precache hundreds of sounds, many never play,
	// they're loaded on first use instead)
	for( i = 0; i < s_numSfx; i++ )
	{
		sfx = S_KnownSfx( i );
		if( !sfx->name[0] )
			continue;
		S_LoadSound( sfx );
	}
#endif
	s_registering = false;
}

/*
==================
S_RegisterSound

==================
*/
sound_t S_RegisterSound( const char *name )
{
	sfx_t	*sfx;

	if( COM_StringEmptyOrNULL( name ) || !snd.initialized )
		return -1;

	if( S_TestSoundChar( name, '!' ))
	{
		Q_strncpy( s_sentenceImmediateName, name, sizeof( s_sentenceImmediateName ));
		return SENTENCE_INDEX;
	}

	// some stupid mappers used leading '/' or '\' in path to models or sounds
	if( name[0] == '/' || name[0] == '\\' ) name++;
	if( name[0] == '/' || name[0] == '\\' ) name++;

	sfx = S_FindName( name, NULL );
	if( !sfx ) return -1;

	sfx->servercount = cl.servercount;
	if( !s_registering && !S_RequestSound( sfx )) S_LoadSound( sfx );

	return S_SfxIndex( sfx );
}

sfx_t *S_GetSfxByHandle( sound_t handle )
{
	if( !snd.initialized )
		return NULL;

	// create new sfx
	if( handle == SENTENCE_INDEX )
		return S_FindName( s_sentenceImmediateName, NULL );

	if( handle < 0 || handle >= s_numSfx )
		return NULL;

	return S_KnownSfx( handle );
}

/*
=================
S_InitSounds
=================
*/
void S_InitSounds( void )
{
	// create unused 0-entry
	Q_strncpy( s_firstSfx->name, "*default", sizeof( s_firstSfx->name ));
	s_firstSfx->hashValue = COM_HashKey( s_firstSfx->name, MAX_SFX_HASH );
	s_firstSfx->hashNext = s_sfxHashList[s_firstSfx->hashValue];
	s_sfxHashList[s_firstSfx->hashValue] = s_firstSfx;
	s_firstSfx->cache = S_CreateDefaultSound();
	s_numSfx = 1;
}

/*
=================
S_FreeSounds
=================
*/
void S_FreeSounds( void )
{
	if( !snd.initialized )
		return;

	// stop all sounds
	S_StopAllSounds( true );

#if XASH_PSP
	S_LoadCancel();
#endif

	// free all sounds
	for( int i = 0; i < s_numSfx; i++ )
		S_FreeSound( S_KnownSfx( i ));


	for( int i = 0; i < ARRAYSIZE( s_sfxChunks ) && s_sfxChunks[i]; i++ )
		memset( s_sfxChunks[i], 0, sizeof( sfx_t ) * SFX_CHUNK_SIZE );
	memset( s_sfxHashList, 0, sizeof( s_sfxHashList ));

	s_numSfx = 0;
}
