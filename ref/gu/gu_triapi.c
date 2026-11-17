/*
gu_triapi.c - TriAPI draw methods
Copyright (C) 2011 Uncle Mike
Copyright (C) 2019 a1batross
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

#include "gu_local.h"
#include "const.h"

#define GL_MODELVIEW_MATRIX  0x0BA6
#define GL_PROJECTION_MATRIX 0x0BA7

static struct
{
	int		renderMode;		// override kRenderMode from TriAPI
	vec4_t		triRGBA;
} ds;

// vertices are written straight into the display list between TriBegin and TriEnd,
// the vertex format is fixed by the attributes set before the first vertex
static struct
{
	byte	*start;
	byte	*end;
	byte	*limit;
	int	count;
	int	prim;
	qboolean quads;	// TRI_QUADS/TRI_QUAD_STRIP are emulated
	uint	flags;
	float	lastTexCord[2];
	uint	lastColor;
} triContext = { 0 };

/*
===============================================================

  TRIAPI IMPLEMENTATION

===============================================================
*/

/*
=============
TriRenderMode

set rendermode
=============
*/
void TriRenderMode( int mode )
{
	ds.renderMode = mode;
	sceGuTexFunc( GU_TFX_MODULATE, GU_TCC_RGBA );

	// NOTE: sceGuDepthMask( GU_TRUE ) disables depth writes
	switch( mode )
	{
	case kRenderNormal:
		R_AllowFog( true );
		sceGuDisable( GU_BLEND );
		sceGuDepthMask( GU_FALSE );
		break;
	case kRenderTransAlpha:
		R_AllowFog( true );
		sceGuEnable( GU_BLEND );
		sceGuBlendFunc( GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0 );
		sceGuDepthMask( GU_TRUE );
		break;
	case kRenderTransColor:
	case kRenderTransTexture:
		R_AllowFog( true );
		sceGuEnable( GU_BLEND );
		sceGuBlendFunc( GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0 );
		break;
	case kRenderGlow:
	case kRenderTransAdd:
		R_AllowFog( false );
		sceGuBlendFunc( GU_ADD, GU_SRC_ALPHA, GU_FIX, 0, GUBLEND1 );
		sceGuEnable( GU_BLEND );
		sceGuDepthMask( GU_TRUE );
		break;
	}
}

/*
=============
TriBegin

begin triangle sequence
=============
*/
void TriBegin( int mode )
{
	uint maxsize;

	triContext.quads = false;

	switch( mode )
	{
	case TRI_POINTS:
		triContext.prim = GU_POINTS;
		break;
	case TRI_TRIANGLES:
		triContext.prim = GU_TRIANGLES;
		break;
	case TRI_TRIANGLE_FAN:
		triContext.prim = GU_TRIANGLE_FAN;
		break;
	case TRI_QUADS:
		// every 4 vertices become two triangles
		triContext.prim = GU_TRIANGLES;
		triContext.quads = true;
		break;
	case TRI_LINES:
		triContext.prim = GU_LINES;
		break;
	case TRI_TRIANGLE_STRIP:
	case TRI_QUAD_STRIP: // same vertex order
		triContext.prim = GU_TRIANGLE_STRIP;
		break;
	case TRI_POLYGON:
	default:
		triContext.prim = GU_TRIANGLE_FAN;
		break;
	}

	triContext.start = extGuBeginPacket( &maxsize );
	triContext.end   = triContext.start;
	// leave room for the draw commands after the vertices
	triContext.limit = triContext.start + ( maxsize > 1024 ? maxsize - 1024 : 0 );
	triContext.count = 0;
	triContext.flags = 0;
}

/*
=============
TriEnd

draw triangle sequence
=============
*/
void TriEnd( void )
{
	if( !triContext.start )
		return;

	if( triContext.count > 0 )
	{
		extGuEndPacket(( void * )triContext.end );
		sceGuDrawArray( triContext.prim, triContext.flags, triContext.count, 0, triContext.start );
	}

	triContext.start = NULL;
	triContext.end   = NULL;
	triContext.count = 0;
}

/*
=============
_TriColor4f

=============
*/
void _TriColor4f( float r, float g, float b, float a )
{
	if( triContext.start )
	{
		if( triContext.count > 0 && !( triContext.flags & GU_COLOR_8888 ))
			return;

		triContext.flags |= GU_COLOR_8888;
		triContext.lastColor = GUCOLOR4F( r, g, b, a );
	}
	else sceGuColor( GUCOLOR4F( r, g, b, a ));
}

/*
=============
_TriColor4ub

=============
*/
void _TriColor4ub( byte r, byte g, byte b, byte a )
{
	if( triContext.start )
	{
		if( triContext.count > 0 && !( triContext.flags & GU_COLOR_8888 ))
			return;

		triContext.flags |= GU_COLOR_8888;
		triContext.lastColor = GUCOLOR4UB( r, g, b, a );
	}
	else sceGuColor( GUCOLOR4UB( r, g, b, a ));
}

/*
=============
TriColor4ub

=============
*/
void TriColor4ub( byte r, byte g, byte b, byte a )
{
	ds.triRGBA[0] = r * (1.0f / 255.0f);
	ds.triRGBA[1] = g * (1.0f / 255.0f);
	ds.triRGBA[2] = b * (1.0f / 255.0f);
	ds.triRGBA[3] = a * (1.0f / 255.0f);

	_TriColor4ub( r, g, b, 255 );
}

/*
=================
TriColor4f
=================
*/
void TriColor4f( float r, float g, float b, float a )
{
	if( ds.renderMode == kRenderTransAlpha )
		TriColor4ub( r * 255.9f, g * 255.9f, b * 255.9f, a * 255.0f );
	else _TriColor4f( r * a, g * a, b * a, 1.0 );

	ds.triRGBA[0] = r;
	ds.triRGBA[1] = g;
	ds.triRGBA[2] = b;
	ds.triRGBA[3] = a;
}

/*
=============
TriTexCoord2f

=============
*/
void TriTexCoord2f( float u, float v )
{
	if( !triContext.start )
		return;

	if( triContext.count > 0 && !( triContext.flags & GU_TEXTURE_32BITF ))
		return;

	triContext.flags |= GU_TEXTURE_32BITF;
	triContext.lastTexCord[0] = u;
	triContext.lastTexCord[1] = v;
}

static size_t TriVertexSize( void )
{
	size_t size = sizeof( float ) * 3;

	if( triContext.flags & GU_TEXTURE_32BITF )
		size += sizeof( float ) * 2;
	if( triContext.flags & GU_COLOR_8888 )
		size += sizeof( uint );

	return size;
}

/*
=============
TriVertex3f

=============
*/
void TriVertex3f( float x, float y, float z )
{
	if( !triContext.start )
		return;

	triContext.flags |= GU_VERTEX_32BITF;

	size_t size = TriVertexSize();

	if( triContext.end + size * 3 > triContext.limit )
		return; // display list is full

	if( triContext.quads && ( triContext.count % 6 ) == 3 )
	{
		// 4th vertex of a quad: abc -> abc acd
		byte *quad = triContext.end - size * 3;

		memcpy( triContext.end, quad, size );
		memcpy( triContext.end + size, quad + size * 2, size );
		triContext.end += size * 2;
		triContext.count += 2;
	}

	if( triContext.flags & GU_TEXTURE_32BITF )
	{
		*( float* )triContext.end = triContext.lastTexCord[0];
		triContext.end += sizeof( float );
		*( float* )triContext.end = triContext.lastTexCord[1];
		triContext.end += sizeof( float );
	}

	if( triContext.flags & GU_COLOR_8888 )
	{
		*( uint* )triContext.end = triContext.lastColor;
		triContext.end += sizeof( uint );
	}

	*( float* )triContext.end = x;
	triContext.end += sizeof( float );
	*( float* )triContext.end = y;
	triContext.end += sizeof( float );
	*( float* )triContext.end = z;
	triContext.end += sizeof( float );

	triContext.count++;
}

/*
=============
TriVertex3fv

=============
*/
void TriVertex3fv( const float *v )
{
	TriVertex3f( v[0], v[1], v[2] );
}

/*
=============
TriWorldToScreen

convert world coordinates (x,y,z) into screen (x, y)
=============
*/
int TriWorldToScreen( const float *world, float *screen )
{
	int retval = R_WorldToScreen( world, screen );

	screen[0] =  0.5f * screen[0] * (float)RI.rvp.viewport[2];
	screen[1] = -0.5f * screen[1] * (float)RI.rvp.viewport[3];
	screen[0] += 0.5f * (float)RI.rvp.viewport[2];
	screen[1] += 0.5f * (float)RI.rvp.viewport[3];

	return retval;
}

/*
=============
TriSpriteTexture

bind current texture
=============
*/
int TriSpriteTexture( model_t *pSpriteModel, int frame )
{
	if( !pSpriteModel || pSpriteModel->type != mod_sprite || !pSpriteModel->cache.data )
		return 0;

	int gl_texturenum = gEngfuncs.R_GetSpriteFrame( pSpriteModel, frame, 0.0f )->gl_texturenum;
	if( gl_texturenum == 0 )
		return 0;

	if( gl_texturenum <= 0 || gl_texturenum >= MAX_TEXTURES )
		gl_texturenum = tr.defaultTexture;

	GL_Bind( XASH_TEXTURE0, gl_texturenum );

	return 1;
}

/*
=============
TriFog

enables global fog on the level
=============
*/
void TriFog( float flFogColor[3], float flStart, float flEnd, int bOn )
{
	// overrided by internal fog
	if( RI.fogEnabled || !gl_fog.value ) return;
	RI.fogCustom = bOn;

	// check for invalid parms
	if( flEnd <= flStart )
	{
		glState.isFogEnabled = RI.fogCustom = false;
		sceGuDisable( GU_FOG );
		return;
	}

	if( RI.fogCustom )
		sceGuEnable( GU_FOG );
	else sceGuDisable( GU_FOG );

	// copy fog params
	RI.fogColor[0] = flFogColor[0] / 255.0f;
	RI.fogColor[1] = flFogColor[1] / 255.0f;
	RI.fogColor[2] = flFogColor[2] / 255.0f;
	RI.fogColor[3] = 1.0f;

	RI.fogStart = flStart;
	RI.fogEnd = flEnd;

	// GU fog is always linear, exponential fog density is ignored
	if( RI.fogDensity <= 0.0f )
		RI.fogSkybox = true;

	glState.fogColor = GUCOLOR4F( RI.fogColor[0], RI.fogColor[1], RI.fogColor[2], 1.0f );
	glState.fogStart = RI.fogStart;
	glState.fogEnd = RI.fogEnd;

	sceGuFog( glState.fogStart, glState.fogEnd, glState.fogColor );
}

/*
=============
TriGetMatrix

very strange export
=============
*/
void TriGetMatrix( const int pname, float *matrix )
{
	switch( pname )
	{
	case GL_MODELVIEW_MATRIX:
		Matrix4x4_ToArrayFloatGL( RI.modelviewMatrix, matrix );
		break;
	case GL_PROJECTION_MATRIX:
		Matrix4x4_ToArrayFloatGL( RI.projectionMatrix, matrix );
		break;
	default:
		gEngfuncs.Con_Reportf( S_WARN "%s: unsupported matrix 0x%x\n", __func__, pname );
		memset( matrix, 0, sizeof( float ) * 16 );
		break;
	}
}

/*
=============
TriForParams

=============
*/
void TriFogParams( float flDensity, int iFogSkybox )
{
	RI.fogDensity = flDensity;
	RI.fogSkybox = iFogSkybox;
}

/*
=============
TriCullFace

=============
*/
void TriCullFace( TRICULLSTYLE mode )
{
	int glMode;

	switch( mode )
	{
	case TRI_FRONT:
		glMode = GL_FRONT;
		break;
	default:
		glMode = GL_NONE;
		break;
	}

	GL_Cull( glMode );
}

/*
=============
TriBrightness
=============
*/
void TriBrightness( float brightness )
{
	float r = ds.triRGBA[0] * ds.triRGBA[3] * brightness;
	float g = ds.triRGBA[1] * ds.triRGBA[3] * brightness;
	float b = ds.triRGBA[2] * ds.triRGBA[3] * brightness;

	_TriColor4f( r, g, b, 1.0f );
}

/*
=============
getTriBrightness

TriBrightness as a GU color, for vertex arrays
=============
*/
uint getTriBrightness( float brightness )
{
	vec3_t color;

	if( brightness < 0.0f ) return 0;

	color[0] = ds.triRGBA[0] * ds.triRGBA[3] * brightness;
	color[1] = ds.triRGBA[1] * ds.triRGBA[3] * brightness;
	color[2] = ds.triRGBA[2] * ds.triRGBA[3] * brightness;

	return GUCOLOR3FV( color );
}
