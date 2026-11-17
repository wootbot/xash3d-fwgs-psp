/*
lib_psp.c - library loader for Sony PSP
Copyright (C) 2018 Flying With Gauss
Copyright (C) 2022 Sergey Galushko

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
There is no dynamic linker on the PSP. Engine libraries (filesystem, menu, renderer)
are linked into the engine as relocatable objects, see scripts/waifulib/xshlib.py,
and found by name in the generated table.

Game libraries are PRX modules. The engine passes a pointer to receive the module's
export table and the root directory to module_start, see hlsdk-portable psp/server_psp.c:
	argp[0] - mod_export_t ** that receives the export table
	argp[1] - root directory with a trailing slash, for the module's libc
*/

#include "platform/platform.h"

#if XASH_LIB == LIB_PSP
#include <pspkernel.h>
#include <pspmodulemgr.h>
#include "library.h"
#include "server.h"

typedef struct mod_export_s
{
	const char *name;
	void *addr;
} mod_export_t;

typedef struct table_s
{
	const char *name;
	void *pointer;
} table_t;

#include "generated_library_tables.h"

typedef struct mod_handle_s
{
	SceUID       uid;      // -1 for static libraries
	string       name;
	uint         seg_addr; // first segment, for debugging
	uint         seg_size;
	mod_export_t *exports;
	struct mod_handle_s *next;
} mod_handle_t;

static mod_handle_t *mod_list;

static void Mod_LibError( const char *fmt, ... ) FORMAT_CHECK( 1 );
static void Mod_LibError( const char *fmt, ... )
{
	char buf[MAX_VA_STRING];
	va_list args;

	va_start( args, fmt );
	Q_vsnprintf( buf, sizeof( buf ), fmt, args );
	va_end( args );

	COM_PushLibraryError( buf );
}

static qboolean Mod_IsLoaded( const mod_handle_t *handle )
{
	const mod_handle_t *h;

	for( h = mod_list; h; h = h->next )
	{
		if( h == handle )
			return true;
	}

	return false;
}

// "path/libname.prx" -> "name"
static void Mod_StaticName( const char *path, char *out, size_t size )
{
	string base;

	COM_FileBase( path, base, sizeof( base ));

	if( !Q_strncmp( base, "lib", 3 ))
		Q_strncpy( out, base + 3, size );
	else Q_strncpy( out, base, size );
}

static mod_export_t *Mod_FindStatic( const char *path )
{
	const table_t *lib;
	string name;

	Mod_StaticName( path, name, sizeof( name ));

	for( lib = (const table_t *)libs; lib->name; lib++ )
	{
		if( !Q_strcmp( lib->name, name ))
			return lib->pointer;
	}

	return NULL;
}

static mod_handle_t *Mod_Load( const char *path, qboolean allow_static )
{
	mod_export_t *exports = NULL;
	SceKernelModuleInfo info = { 0 };
	SceUID uid = -1;
	mod_handle_t *h;
	string rootdir;
	void *args[2];

	for( h = mod_list; h; h = h->next )
	{
		if( !Q_strcmp( h->name, path ))
			return h;
	}

	h = calloc( 1, sizeof( *h ));
	if( !h )
	{
		Mod_LibError( "%s: out of memory", __func__ );
		return NULL;
	}

	if( allow_static && ( exports = Mod_FindStatic( path )))
	{
		Con_Reportf( "%s: %s (static)\n", __func__, path );
	}
	else
	{
		// the filesystem has changed the directory to the root directory
		if( !getcwd( rootdir, sizeof( rootdir ) - 1 ))
			rootdir[0] = 0;
		Q_strncat( rootdir, "/", sizeof( rootdir ));
		args[0] = &exports;
		args[1] = rootdir;

		uid = PSP_LoadModule( path, 0, sizeof( args ), args );
		if( uid < 0 )
		{
			Mod_LibError( "%s: can't load %s (0x%08x)", __func__, path, (uint)uid );
			free( h );
			return NULL;
		}

		if( !exports )
		{
			int code;

			Mod_LibError( "%s: %s didn't provide an export table", __func__, path );
			PSP_UnloadModule( uid, &code );
			free( h );
			return NULL;
		}

		info.size = sizeof( info );
		if( sceKernelQueryModuleInfo( uid, &info ) >= 0 && info.nsegment > 0 )
		{
			h->seg_addr = info.segmentaddr[0];
			h->seg_size = info.segmentsize[0];
		}

		Con_Reportf( "%s: %s (PRX at 0x%08x, %u bytes)\n", __func__, path, h->seg_addr, h->seg_size );
	}

	h->uid = uid;
	h->exports = exports;
	Q_strncpy( h->name, path, sizeof( h->name ));
	h->next = mod_list;
	mod_list = h;

	return h;
}

static void Mod_Unload( mod_handle_t *h )
{
	mod_handle_t **prev;
	int ret, code;

	if( !Mod_IsLoaded( h ))
		return;

	if( h->uid >= 0 )
	{
		ret = PSP_UnloadModule( h->uid, &code );
		if( ret == -1 )
			Con_Printf( S_WARN "%s: %s doesn't want to stop\n", __func__, h->name );
		else if( ret < 0 )
			Con_Printf( S_WARN "%s: %s: error 0x%08x\n", __func__, h->name, (uint)code );
	}

	for( prev = &mod_list; *prev; prev = &( *prev )->next )
	{
		if( *prev == h )
		{
			*prev = h->next;
			break;
		}
	}

	free( h );
}

void *COM_LoadLibrary( const char *dllname, int build_ordinals_table, qboolean directpath )
{
	dll_user_t *hInst;
	mod_handle_t *h;

	COM_ResetLibraryError();

	// engine libraries are linked in
	if( Mod_FindStatic( dllname ))
		return Mod_Load( dllname, true );

	hInst = FS_FindLibrary( dllname, directpath );
	if( !hInst )
	{
		h = Mod_Load( dllname, false );
		if( !h )
			Mod_LibError( "Failed to find library %s", dllname );
		return h;
	}

	if( hInst->custom_loader )
	{
		Mod_LibError( "Custom library loader is not available. Extract library %s and fix gameinfo.txt!", hInst->fullPath );
		Mem_Free( hInst );
		return NULL;
	}

	h = Mod_Load( hInst->fullPath, false );
	Mem_Free( hInst );

	return h;
}

void COM_FreeLibrary( void *hInstance )
{
	Mod_Unload( hInstance );
}

void *COM_GetProcAddress( void *hInstance, const char *name )
{
	mod_handle_t *h = hInstance;
	mod_export_t *e;

	if( !h || !name || !Mod_IsLoaded( h ))
		return NULL;

	for( e = h->exports; e->name && e->addr; e++ )
	{
		if( !Q_strcmp( e->name, name ))
			return e->addr;
	}

	return NULL;
}

void *COM_GetProcAddressFromDependency( void *hInstance, const char *depname, const char *name )
{
	return NULL;
}

void *COM_FunctionFromName( void *hInstance, const char *pName )
{
	return COM_GetProcAddress( hInstance, pName );
}

const char *COM_NameForFunction( void *hInstance, void *function )
{
	mod_handle_t *h = hInstance;
	mod_export_t *e;

	if( h && Mod_IsLoaded( h ))
	{
		for( e = h->exports; e->name && e->addr; e++ )
		{
			if( e->addr == function )
				return e->name;
		}
	}

	// not exported, save its offset from pfnGameInit, see COM_FunctionFromName_SR
	return COM_OffsetNameForFunction( function );
}

#endif // XASH_LIB == LIB_PSP
