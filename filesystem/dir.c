/*
dir.c - caseinsensitive directory operations
Copyright (C) 2022 Alibek Omarov, Velaron
Copyright (C) 2023 Xash3D FWGS contributors

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

#include "build.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <stddef.h>
#if XASH_POSIX
#include <unistd.h>
#if !XASH_PSVITA
#include <sys/ioctl.h>
#endif
#endif
#if XASH_LINUX
#include <linux/fs.h>
#ifndef FS_CASEFOLD_FL // for compatibility with older distros
#define FS_CASEFOLD_FL 0x40000000
#endif // FS_CASEFOLD_FL
#endif // XASH_LINUX

#include "port.h"
#include "filesystem_internal.h"
#include "crtlib.h"
#include "xash3d_mathlib.h"
#include "common/com_strings.h"

enum
{
	DIRENTRY_EMPTY_DIRECTORY = 0, // don't care if it's not directory or it's empty
	DIRENTRY_NOT_SCANNED = -1,
	DIRENTRY_CASEINSENSITIVE = -2, // directory is already caseinsensitive, just copy whatever is left
};

typedef struct dir_s
{
	string name;
	int numentries;
	struct dir_s *entries; // sorted
} dir_t;

static qboolean Platform_GetDirectoryCaseSensitivity( const char *dir )
{
#if XASH_WIN32 || XASH_PSVITA || XASH_NSWITCH || XASH_PSP
	return false;
#elif XASH_ANDROID
	// on Android, doing code below causes crash in MediaProviderGoogle.apk!libfuse_jni.so
	// which in turn makes vold (Android's Volume Daemon) to umount /storage/emulated/0
	// and because you can't unmount a filesystem when there is file descriptors open
	// it has no other choice but to terminate and then kill our program
	return true;
#elif XASH_LINUX && defined( FS_IOC_GETFLAGS )
	int flags = 0;
	int fd = open( dir, O_RDONLY | O_NONBLOCK );

	if( fd < 0 )
		return true;

	if( ioctl( fd, FS_IOC_GETFLAGS, &flags ) < 0 )
	{
		close( fd );
		return true;
	}

	close( fd );

	return !FBitSet( flags, FS_CASEFOLD_FL );
#else
	return true;
#endif
}

static int FS_SortDirEntries( const void *_a, const void *_b )
{
	const dir_t *a = _a;
	const dir_t *b = _b;
	return Q_stricmp( a->name, b->name );
}

static void FS_FreeDirEntries( dir_t *dir )
{
	if( dir->entries )
	{
		for( int i = 0; i < dir->numentries; i++ )
			FS_FreeDirEntries( &dir->entries[i] );

		Mem_Free( dir->entries );
		dir->entries = NULL;
	}

	dir->numentries = DIRENTRY_NOT_SCANNED;
}

static void FS_InitDirEntries( dir_t *dir, const stringlist_t *list )
{
	dir->numentries = list->numstrings;
	dir->entries = Mem_Malloc( fs_mempool, sizeof( dir_t ) * dir->numentries );

	for( int i = 0; i < list->numstrings; i++ )
	{
		dir_t *entry = &dir->entries[i];

		Q_strncpy( entry->name, list->strings[i], sizeof( entry->name ));
		entry->numentries = DIRENTRY_NOT_SCANNED;
		entry->entries = NULL;
	}

	qsort( dir->entries, dir->numentries, sizeof( dir->entries[0] ), FS_SortDirEntries );
}

static void FS_PopulateDirEntries( dir_t *dir, const char *path )
{
	stringlist_t list;

	// check this first: a missing directory on a case insensitive filesystem
	// would otherwise be rescanned with listdirectory on every lookup
	if( !Platform_GetDirectoryCaseSensitivity( path ))
	{
		dir->numentries = DIRENTRY_CASEINSENSITIVE;
		dir->entries = NULL;
		return;
	}

	if( !FS_SysFolderExists( path ))
	{
		dir->numentries = DIRENTRY_EMPTY_DIRECTORY;
		dir->entries = NULL;
		return;
	}

	stringlistinit( &list );
	listdirectory( &list, path, false );
	if( !list.numstrings )
	{
		dir->numentries = DIRENTRY_EMPTY_DIRECTORY;
		dir->entries = NULL;
	}
	else
	{
		FS_InitDirEntries( dir, &list );
	}
	stringlistfreecontents( &list );
}

static int FS_FindDirEntry( const dir_t *dir, const char *name, qboolean exact )
{
	// look for the file (binary search)
	int left = 0;
	int right = dir->numentries - 1;

	while( left <= right )
	{
		int middle = (left + right) / 2;
		int diff = Q_stricmp( dir->entries[middle].name, name );

		// found it
		if( !diff )
		{
			int i = middle;

			if( !Q_strcmp( dir->entries[middle].name, name ))
				return middle;

			// names that only differ by case are next to each other, prefer the exact one
			while( i > 0 && !Q_stricmp( dir->entries[i - 1].name, name ))
				i--;

			for( ; i < dir->numentries && !Q_stricmp( dir->entries[i].name, name ); i++ )
			{
				if( !Q_strcmp( dir->entries[i].name, name ))
					return i;
			}

			return exact ? -1 : middle;
		}

		// if we're too far in the list
		if( diff > 0 )
			right = middle - 1;
		else left = middle + 1;
	}
	return -1;
}

static void FS_MergeDirEntries( dir_t *dir, const stringlist_t *list )
{
	dir_t temp;

	// glorified realloc for sorted dir entries
	// make new array and copy old entries with same name and subentries
	// everything else get freed

	FS_InitDirEntries( &temp, list );

	for( int i = 0; i < dir->numentries; i++ )
	{
		dir_t *oldentry = &dir->entries[i];
		dir_t *newentry;
		int j;

		// don't care about directories without subentries
		if( oldentry->entries == NULL )
			continue;

		// try to find this directory in new tree, by exact name, or directories
		// that only differ by case would end up in the same entry and leak
		j = FS_FindDirEntry( &temp, oldentry->name, true );

		// not found, free memory
		if( j < 0 )
		{
			FS_FreeDirEntries( oldentry );
			continue;
		}

		// found directory, move all entries
		newentry = &temp.entries[j];

		newentry->numentries = oldentry->numentries;
		newentry->entries = oldentry->entries;
	}

	// now we can free old tree and replace it with temporary
	// do not add null check there! If we hit it, it's probably a logic error!
	Mem_Free( dir->entries );
	dir->numentries = temp.numentries;
	dir->entries = temp.entries;
}

static int FS_MaybeUpdateDirEntries( dir_t *dir, const char *path, const char *entryname )
{
	stringlist_t list;
	int ret;

	stringlistinit( &list );
	listdirectory( &list, path, false );

	if( list.numstrings == 0 ) // empty directory
	{
		FS_FreeDirEntries( dir );
		dir->numentries = DIRENTRY_EMPTY_DIRECTORY;
		ret = -1;
	}
	else if( dir->numentries <= DIRENTRY_EMPTY_DIRECTORY ) // not initialized or was empty
	{
		FS_InitDirEntries( dir, &list );
		ret = FS_FindDirEntry( dir, entryname, false );
	}
	else if( list.numstrings != dir->numentries ) // quick update
	{
		FS_MergeDirEntries( dir, &list );
		ret = FS_FindDirEntry( dir, entryname, false );
	}
	else
	{
		// do heavy compare if directory now have an entry we need
		// don't merge if we already have it, an entry that is listed
		// but doesn't exist on disk would rebuild the tree on every lookup
		qboolean found = false, changed = false;

		for( int i = 0; i < list.numstrings; i++ )
		{
			if( Q_stricmp( list.strings[i], entryname ))
				continue;

			found = true;

			if( FS_FindDirEntry( dir, list.strings[i], true ) < 0 )
			{
				changed = true;
				break;
			}
		}

		if( changed )
			FS_MergeDirEntries( dir, &list );

		ret = found ? FS_FindDirEntry( dir, entryname, false ) : -1;
	}

	stringlistfreecontents( &list );
	return ret;
}

static inline qboolean FS_AppendToPath( char *dst, size_t *pi, const size_t len, const char *src, const char *path, const char *err )
{
	size_t i = *pi;

	i += Q_strncpy( &dst[i], src, len - i );
	*pi = i;

	if( i >= len )
	{
		Con_Printf( S_ERROR "%s: overflow while appending %s (%s)\n", __func__, path, err );
		return false;
	}
	return true;
}

qboolean FS_FixFileCase( dir_t *dir, const char *path, char *dst, const size_t len, qboolean createpath )
{
	size_t i = 0;

	if( !FS_AppendToPath( dst, &i, len, dir->name, path, "init" ))
		return false;

	// nothing to fix
	if( COM_StringEmpty( path ))
		return true;

	// we can't and shouldn't track parent directories to not track the whole filesystem
	// exit early for this case
	// FIXME: track the path to catch other cases
	if( !Q_strncmp( path, "..", 2 ) && ( path[2] == '\0' || path[2] == '/' ))
	{
		if( !FS_AppendToPath( dst, &i, len, path, path, "escape to parent directory" ))
			return false;

		// check file existense
		return createpath ? true : FS_SysFileOrFolderExists( dst );
	}

	for( const char *prev = path, *next = Q_strchrnul( prev, '/' );
		  ;
		  prev = next + 1, next = Q_strchrnul( prev, '/' ))
	{
		qboolean uptodate = false; // do not run second scan if we're just updated our directory list
		dir_t *parent;
		size_t temp;
		char entryname[MAX_SYSPATH];
		int ret;

		if( dir->numentries == DIRENTRY_NOT_SCANNED )
		{
			// read directory first time
			FS_PopulateDirEntries( dir, dst );
			uptodate = true;
		}

		// this subdirectory is case insensitive, just slam everything that's left
		if( dir->numentries == DIRENTRY_CASEINSENSITIVE )
		{
			if( !FS_AppendToPath( dst, &i, len, prev, path, "caseinsensitive entry" ))
				return false;

			// check file existense
			return createpath ? true : FS_SysFileOrFolderExists( dst );
		}

		// get our entry name
		Q_strncpy( entryname, prev, next - prev + 1 );

		// didn't found, but does it exists in FS?
		if(( ret = FS_FindDirEntry( dir, entryname, false )) < 0 )
		{
			// if we're creating files or folders, we don't care if path doesn't exist
			// so copy everything that's left and exit without an error
			if( uptodate || ( ret = FS_MaybeUpdateDirEntries( dir, dst, entryname )) < 0 )
				return createpath ? FS_AppendToPath( dst, &i, len, prev, path, "create path" ) : false;

			uptodate = true;
		}

		parent = dir;
		dir = &parent->entries[ret];
		temp = i;
		if( !FS_AppendToPath( dst, &temp, len, dir->name, path, "case fix" ))
			return false;

		if( !uptodate && !FS_SysFileOrFolderExists( dst )) // file not found, rescan...
		{
			dst[i] = 0; // strip failed part

			// rescan the directory that holds the entry, not the entry itself,
			// and append the found name right after the stripped part
			// if we're creating files or folders, we don't care if path doesn't exist
			// so copy everything that's left and exit without an error
			if(( ret = FS_MaybeUpdateDirEntries( parent, dst, entryname )) < 0 )
				return createpath ? FS_AppendToPath( dst, &i, len, prev, path, "create path rescan" ) : false;

			dir = &parent->entries[ret];
			temp = i;
			if( !FS_AppendToPath( dst, &temp, len, dir->name, path, "case fix rescan" ))
				return false;
		}
		i = temp;

		// end of string, found file, return
		if( next[0] == '\0' || ( next[0] == '/' && next[1] == '\0' ))
			break;

		if( !FS_AppendToPath( dst, &i, len, "/", path, "path separator" ))
			return false;
	}

	return true;
}

static void FS_Close_DIR( searchpath_t *search )
{
	FS_FreeDirEntries( search->dir );
	Mem_Free( search->dir );
}

static void FS_PrintInfo_DIR( searchpath_t *search, char *dst, size_t size )
{
	Q_strncpy( dst, search->filename, size );
}

#if XASH_PSP
/*
=============================================================================

PSP DIRECTORY LISTING CACHE

Looking a file up on the memory stick costs ~15 ms per stat (more in big
directories), and finding one file takes a few of them across the search
paths: ~45 ms per file found, ~65 ms per file missing, tens of seconds per
connect. Each directory is listed once instead and lookups are answered from
memory, missing directories included. A write through the filesystem
(FS_SysOpen for writing, FS_CreatePath, FS_Rename, FS_Delete) drops the
listings of directories with that name, files written outside it in the search
paths aren't seen.

=============================================================================
*/
#include <pspiofilemgr.h>

#define DIRCACHE_BUCKETS 256

typedef struct fs_dircache_s
{
	struct fs_dircache_s *next;
	uint32_t	pathhash;
	size_t	pathlen;
	int	count;
	uint32_t	*hashes;	// lowercased names
	int	*offsets;	// into names
	int	*sizes;	// -1 for directories
	char	*names;
	char	path[1];	// the directory with its trailing slash, allocated with the struct
} fs_dircache_t;

static fs_dircache_t *fs_dircache[DIRCACHE_BUCKETS];

static uint32_t FS_DirCacheHash( const char *s, size_t len )
{
	uint32_t h = 2166136261u;

	for( size_t i = 0; i < len; i++ )
	{
		byte c = Q_tolower( s[i] );

		if( c == '\\' )
			c = '/';
		h = ( h ^ c ) * 16777619u;
	}

	return h;
}

static void FS_DirCacheFree( fs_dircache_t *d );

void FS_DirCacheClear( void )
{
	for( int i = 0; i < DIRCACHE_BUCKETS; i++ )
	{
		fs_dircache_t *d, *next;

		for( d = fs_dircache[i]; d; d = next )
		{
			next = d->next;
			FS_DirCacheFree( d );
		}
		fs_dircache[i] = NULL;
	}
}

static void FS_DirCacheFree( fs_dircache_t *d )
{
	if( d->hashes ) Mem_Free( d->hashes );
	if( d->offsets ) Mem_Free( d->offsets );
	if( d->sizes ) Mem_Free( d->sizes );
	if( d->names ) Mem_Free( d->names );
	Mem_Free( d );
}

/*
path is going to be written, or made if it's a directory. Which of the ways to
spell its directory the listings have isn't known: all directories of that name
are listed again
*/
void FS_DirCacheDrop( const char *path, qboolean isdir )
{
	const char *end = path + Q_strlen( path );

	if( end > path && ( end[-1] == '/' || end[-1] == '\\' ))
		end--;

	for( int pass = isdir ? 0 : 1; pass < 2; pass++ )
	{
		const char *start;

		// the last directory of it: a directory itself, then what it's in
		if( pass == 1 )
		{
			while( end > path && end[-1] != '/' && end[-1] != '\\' )
				end--;
			if( end > path )
				end--;
		}

		for( start = end; start > path && start[-1] != '/' && start[-1] != '\\' && start[-1] != ':'; start-- );

		const size_t len = end - start;

		if( len > 0 && *start == '.' )
		{
			FS_DirCacheClear();
			return;
		}

		for( int i = 0; i < DIRCACHE_BUCKETS; i++ )
		{
			for( fs_dircache_t **prev = &fs_dircache[i], *d; ( d = *prev ); )
			{
				const char *name = d->path + Q_max( 0, (int)( d->pathlen - 1 - len )); // d->path ends with a slash

				if( len == 0 ? d->pathlen <= 2 : d->pathlen > len && !Q_strnicmp( name, start, len ) && ( name == d->path || name[-1] == '/' || name[-1] == '\\' || name[-1] == ':' ))
				{
					*prev = d->next;
					FS_DirCacheFree( d );
				}
				else prev = &d->next;
			}
		}
	}
}

static void FS_DirCacheList( fs_dircache_t *d )
{
	char path[MAX_SYSPATH], abspath[MAX_SYSPATH];
	int maxcount = 0, nameslen = 0, namessize = 0;
	SceIoDirent ent;
	SceUID dir;

	Q_strncpy( path, d->pathlen ? d->path : "./", sizeof( path ));
	if( d->pathlen > 1 && d->pathlen < sizeof( path ))
		path[d->pathlen - 1] = '\0'; // no trailing slash

	// a failed opendir costs as much as a listing: when the parent's listing
	// doesn't have this directory, don't try (not for . and .., which
	// listings leave out)
	if( d->pathlen > 1 )
	{
		const char *last = Q_strrchr( path, '/' );

		last = last ? last + 1 : path;
		if( Q_strcmp( last, "." ) && Q_strcmp( last, ".." ) && FS_DirCacheLookup( path ) < 0 )
			return;
	}

	// sceIoDopen, not opendir: the listing has the sizes too (FS_FileSize).
	// Relative paths are resolved like libc does, against its cwd
	if( !Q_strchr( path, ':' ) && getcwd( abspath, sizeof( abspath )))
	{
		Q_strncat( abspath, "/", sizeof( abspath ));
		Q_strncat( abspath, path, sizeof( abspath ));
	}
	else Q_strncpy( abspath, path, sizeof( abspath ));

	if(( dir = sceIoDopen( abspath )) < 0 )
		return; // doesn't exist: nothing in it

	while( 1 )
	{
		size_t len;

		memset( &ent, 0, sizeof( ent )); // d_private must be NULL
		if( sceIoDread( dir, &ent ) <= 0 )
			break;

		len = Q_strlen( ent.d_name );
		if( ent.d_name[0] == '.' && ( len == 1 || ( len == 2 && ent.d_name[1] == '.' )))
			continue;

		if( d->count == maxcount )
		{
			maxcount = maxcount ? maxcount * 2 : 64;
			d->hashes = Mem_Realloc( fs_mempool, d->hashes, maxcount * sizeof( *d->hashes ));
			d->offsets = Mem_Realloc( fs_mempool, d->offsets, maxcount * sizeof( *d->offsets ));
			d->sizes = Mem_Realloc( fs_mempool, d->sizes, maxcount * sizeof( *d->sizes ));
		}

		if( nameslen + len + 1 > namessize )
		{
			namessize = Q_max( namessize * 2, nameslen + len + 1 + 1024 );
			d->names = Mem_Realloc( fs_mempool, d->names, namessize );
		}

		d->hashes[d->count] = FS_DirCacheHash( ent.d_name, len );
		d->offsets[d->count] = nameslen;
		d->sizes[d->count] = FIO_S_ISDIR( ent.d_stat.st_mode ) ? -1 : (int)ent.d_stat.st_size;
		memcpy( d->names + nameslen, ent.d_name, len + 1 );
		nameslen += len + 1;
		d->count++;
	}

	sceIoDclose( dir );
}

// the size of a file, -1 for a directory, -2 nothing there
static int FS_DirCacheEntry( const char *path )
{
	const char *name = Q_strrchr( path, '/' );
	size_t dirlen = name ? name - path + 1 : 0;
	uint32_t pathhash, namehash;
	fs_dircache_t *d;

	name = path + dirlen;
	if( !name[0] )
		return -2;

	pathhash = FS_DirCacheHash( path, dirlen );

	for( d = fs_dircache[pathhash % DIRCACHE_BUCKETS]; d; d = d->next )
	{
		if( d->pathhash == pathhash && d->pathlen == dirlen && !Q_strnicmp( d->path, path, dirlen ))
			break;
	}

	if( !d )
	{
		d = Mem_Calloc( fs_mempool, sizeof( *d ) + dirlen );
		memcpy( d->path, path, dirlen );
		d->pathlen = dirlen;
		d->pathhash = pathhash;
		FS_DirCacheList( d );
		d->next = fs_dircache[pathhash % DIRCACHE_BUCKETS];
		fs_dircache[pathhash % DIRCACHE_BUCKETS] = d;
	}

	namehash = FS_DirCacheHash( name, Q_strlen( name ));

	for( int i = 0; i < d->count; i++ )
	{
		if( d->hashes[i] == namehash && !Q_stricmp( d->names + d->offsets[i], name ))
			return d->sizes[i];
	}

	return -2;
}

// 1 a file, 0 a directory, -1 nothing there
int FS_DirCacheLookup( const char *path )
{
	int size = FS_DirCacheEntry( path );

	return size >= 0 ? 1 : size == -1 ? 0 : -1;
}

// -1 when it's not a file
fs_offset_t FS_DirCacheFileSize( const char *path )
{
	int size = FS_DirCacheEntry( path );

	return size >= 0 ? size : -1;
}
#endif // XASH_PSP

static int FS_FindFile_DIR( searchpath_t *search, const char *path, char *fixedname, size_t len )
{
	char netpath[MAX_SYSPATH];

#if XASH_PSP
	// the PSP is case insensitive, no case fix needed
	Q_snprintf( netpath, sizeof( netpath ), "%s%s", search->filename, path );
	if( FS_DirCacheLookup( netpath ) != 1 )
		return -1;

	if( fixedname )
		Q_strncpy( fixedname, path, len );
	return 0;
#else
	if( !FS_FixFileCase( search->dir, path, netpath, sizeof( netpath ), false ))
		return -1;

	if( FS_SysFileExists( netpath ))
	{
		// return fixed case file name only local for that searchpath
		if( fixedname )
			Q_strncpy( fixedname, netpath + Q_strlen( search->filename ), len );
		return 0;
	}

	return -1;
#endif
}

static void FS_Search_DIR( searchpath_t *search, stringlist_t *list, const char *pattern, int caseinsensitive )
{
	string netpath, temp;
	stringlist_t dirlist;
	const char *slash = Q_strrchr( pattern, '/' );
	const char *backslash = Q_strrchr( pattern, '\\' );
	const char *colon = Q_strrchr( pattern, ':' );
	const char *separator = Q_max( slash, backslash );
	int basepathlength, dirlistindex;
	char *basepath;

	separator = Q_max( separator, colon );

	basepathlength = separator ? (separator + 1 - pattern) : 0;
	basepath = Mem_Calloc( fs_mempool, basepathlength + 1 );
	if( basepathlength ) memcpy( basepath, pattern, basepathlength );
	basepath[basepathlength] = '\0';

	if( !FS_FixFileCase( search->dir, basepath, netpath, sizeof( netpath ), false ))
	{
		Mem_Free( basepath );
		return;
	}

	stringlistinit( &dirlist );
	listdirectory( &dirlist, netpath, false );

	Q_strncpy( temp, basepath, sizeof( temp ));

	for( dirlistindex = 0; dirlistindex < dirlist.numstrings; dirlistindex++ )
	{
		Q_strncpy( &temp[basepathlength], dirlist.strings[dirlistindex], sizeof( temp ) - basepathlength );

		if( matchpattern( temp, (char *)pattern, true ) )
		{
			int resultlistindex;

			for( resultlistindex = 0; resultlistindex < list->numstrings; resultlistindex++ )
			{
				if( !Q_strcmp( list->strings[resultlistindex], temp ) )
					break;
			}

			if( resultlistindex == list->numstrings )
				stringlistappend( list, temp );
		}
	}

	stringlistfreecontents( &dirlist );

	Mem_Free( basepath );
}

static int FS_FileTime_DIR( searchpath_t *search, const char *filename )
{
	char path[MAX_SYSPATH];

	Q_snprintf( path, sizeof( path ), "%s%s", search->filename, filename );
	return FS_SysFileTime( path );
}

static file_t *FS_OpenFile_DIR( searchpath_t *search, const char *filename, const char *mode, int pack_ind )
{
	char path[MAX_SYSPATH];
	file_t *f;

	Q_snprintf( path, sizeof( path ), "%s%s", search->filename, filename );
	f = FS_SysOpen( path, mode );
	if( !f )
		return NULL;

	f->searchpath = search;

	return f;
}

void FS_InitDirectorySearchpath( searchpath_t *search, const char *path, int flags )
{
	memset( search, 0, sizeof( searchpath_t ));

	Q_strncpy( search->filename, path, sizeof( search->filename ) - 1 );
	COM_PathSlashFix( search->filename );

	if( !Q_stricmp( COM_FileExtension( path ), "pk3dir" ))
		search->type = SEARCHPATH_PK3DIR;
	else search->type = SEARCHPATH_PLAIN;
	search->flags = flags;
	search->pfnPrintInfo = FS_PrintInfo_DIR;
	search->pfnClose = FS_Close_DIR;
	search->pfnOpenFile = FS_OpenFile_DIR;
	search->pfnFileTime = FS_FileTime_DIR;
	search->pfnFindFile = FS_FindFile_DIR;
	search->pfnSearch = FS_Search_DIR;

	// create cache root
	search->dir = Mem_Malloc( fs_mempool, sizeof( dir_t ));
	Q_strncpy( search->dir->name, search->filename, sizeof( search->dir->name ));
	FS_PopulateDirEntries( search->dir, path );
}

searchpath_t *FS_AddDir_Fullpath( const char *path, int flags )
{
	searchpath_t *search = (searchpath_t *)Mem_Calloc( fs_mempool, sizeof( searchpath_t ));

	FS_InitDirectorySearchpath( search, path, flags );
	Con_Printf( "Adding directory: %s\n", path );

	return search;
}
