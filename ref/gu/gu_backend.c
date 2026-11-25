/*
gu_backend.c - rendering backend
Copyright (C) 2010 Uncle Mike
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
#include "xash3d_mathlib.h"

static char r_speeds_msg[MAX_SYSPATH];
ref_speeds_t r_stats; // r_speeds counters

/*
===============
R_SpeedsMessage
===============
*/
qboolean R_SpeedsMessage( char *out, size_t size )
{
	if( gEngfuncs.drawFuncs->R_SpeedsMessage != NULL )
	{
		if( gEngfuncs.drawFuncs->R_SpeedsMessage( out, size ))
			return true;
		// otherwise pass to default handler
	}

	if( r_speeds->value <= 0 ) return false;
	if( !out || !size ) return false;

	Q_strncpy( out, r_speeds_msg, size );

	return true;
}

/*
==============
GL_BackendStartFrame
==============
*/
void GL_BackendStartFrame( void )
{
	r_speeds_msg[0] = '\0';
}

/*
==============
GL_BackendEndFrame
==============
*/
void GL_BackendEndFrame( void )
{
	if( r_speeds->value <= 0 || !FBitSet( RI.rvp.flags, RF_DRAW_WORLD ))
		return;

	mleaf_t *curleaf = RI.viewleaf ? RI.viewleaf : WORLDMODEL->leafs;

	switch( (int)r_speeds->value )
	{
	case 1:
		Q_snprintf( r_speeds_msg, sizeof( r_speeds_msg ), "%3i wpoly, %3i apoly\n%3i epoly, %3i spoly",
			r_stats.c_world_polys, r_stats.c_alias_polys, r_stats.c_studio_polys, r_stats.c_sprite_polys );
		break;
	case 2:
		Q_snprintf( r_speeds_msg, sizeof( r_speeds_msg ),
			"Renderer: ^1Engine^7\n\n"
			"visible leafs:\n%3i leafs\ncurrent leaf %3i\n"
			"ReciusiveWorldNode: %3lf secs\nDrawTextureChains %lf",
			r_stats.c_world_leafs, (int)( curleaf - WORLDMODEL->leafs ), r_stats.t_world_node, r_stats.t_world_draw );
		break;
	case 3:
		Q_snprintf( r_speeds_msg, sizeof( r_speeds_msg ), "%3i alias models drawn\n%3i studio models drawn\n%3i sprites drawn",
			r_stats.c_alias_models_drawn, r_stats.c_studio_models_drawn, r_stats.c_sprite_models_drawn );
		break;
	case 4:
		Q_snprintf( r_speeds_msg, sizeof( r_speeds_msg ), "%3i static entities\n%3i normal entities\n%3i server entities",
			r_numStatics, r_numEntities - r_numStatics, (int)ENGINE_GET_PARM( PARM_NUMENTITIES ));
		break;
	case 5:
		Q_snprintf( r_speeds_msg, sizeof( r_speeds_msg ), "%3i tempents\n%3i viewbeams\n%3i particles",
			r_stats.c_active_tents_count, r_stats.c_view_beams_count, r_stats.c_particle_count );
		break;
	}

	memset( &r_stats, 0, sizeof( r_stats ));
}

/*
=================
GL_LoadTexMatrixExt
=================
*/
void GL_LoadTexMatrixExt( const float *glmatrix )
{
	ScePspFMatrix4 m;

	Assert( glmatrix != NULL );

	memcpy( &m, glmatrix, sizeof( m ));
	sceGumMatrixMode( GU_TEXTURE );
	sceGumLoadMatrix( &m );
	sceGumUpdateMatrix();

	glState.texIdentityMatrix = false;
}

/*
=================
GL_LoadMatrix

loads into the current sceGum matrix mode
=================
*/
void GL_LoadMatrix( const matrix4x4 source )
{
	ScePspFMatrix4	dest;

	Matrix4x4_ToFMatrix4( source, &dest );
	sceGumLoadMatrix( &dest );
}

/*
=================
GU_DepthRange, GU_DepthOffset, GU_LoadProjection

The GE's divide isn't exact to the 16 bits of the depth buffer and nearly all of a scene
is at 0.9 or more of the usual -1 (near) to 1 (far): vertices came out tens of steps wrong, so decals and entities on walls disappeared in them. With -1 (near) to
0 (far) the same depth values are computed from small numbers.
=================
*/
static int gu_depth_min = 0, gu_depth_max = 65535, gu_depth_offset;

static void GU_UpdateDepthRange( void )
{
	const int min = gu_depth_min, max = gu_depth_max;

	sceGuSendCommandf( 68, max - min ); // viewport Z scale
	sceGuSendCommandf( 71, max + gu_depth_offset ); // viewport Z center
	sceGuSendCommandi( 214, Q_min( min, max )); // Z clamp
	sceGuSendCommandi( 215, Q_max( min, max ));
}

void GU_DepthRange( int min, int max )
{
	gu_depth_min = min;
	gu_depth_max = max;
	GU_UpdateDepthRange();
}

void GU_DepthOffset( int offset )
{
	gu_depth_offset = offset;
	GU_UpdateDepthRange();
}

void GU_LoadProjection( const matrix4x4 source )
{
	matrix4x4 m;

	Matrix4x4_Copy( m, source );

	for( int i = 0; i < 4; i++ )
		m[2][i] = ( m[2][i] - m[3][i] ) * 0.5f;

	sceGumMatrixMode( GU_PROJECTION );
	GL_LoadMatrix( m );
}

/*
=================
GL_LoadIdentityTexMatrix
=================
*/
void GL_LoadIdentityTexMatrix( void )
{
	if( glState.texIdentityMatrix )
		return;

	sceGumMatrixMode( GU_TEXTURE );
	sceGumLoadIdentity();
	sceGumUpdateMatrix();

	glState.texIdentityMatrix = true;
}

/*
=================
RenderAPI texture unit and texgen functions

GU has a single texture unit and no texgen, these are only here for RenderAPI
=================
*/
void GL_SelectTexture( int tmu )
{
}

void GL_CleanUpTextureUnits( int last )
{
}

void GL_CleanupAllTextureUnits( void )
{
}

void GL_TextureTarget( uint target )
{
}

void GL_TexGen( GLenum coord, GLenum mode )
{
}

void GL_SetTexCoordArrayMode( GLenum mode )
{
}

/*
=================
GL_Cull
=================
*/
void GL_Cull( GLenum cull )
{
	if( glState.faceCull == cull )
		return;

	if( !cull )
	{
		sceGuDisable( GU_CULL_FACE );
		glState.faceCull = 0;
		return;
	}

	sceGuEnable( GU_CULL_FACE );
	sceGuFrontFace( cull - 1 );
	glState.faceCull = cull;
}

/*
=================
GL_SetRenderMode
=================
*/
void GL_SetRenderMode( int mode )
{
	sceGuTexFunc( GU_TFX_MODULATE, GU_TCC_RGBA );

	switch( mode )
	{
	case kRenderNormal:
	default:
		R_AllowFog( true );
		sceGuDisable( GU_BLEND );
		sceGuDisable( GU_ALPHA_TEST );
		break;
	case kRenderTransColor:
	case kRenderTransTexture:
		R_AllowFog( true );
		sceGuEnable( GU_BLEND );
		sceGuDisable( GU_ALPHA_TEST );
		sceGuBlendFunc( GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0 );
		break;
	case kRenderTransAlpha:
		R_AllowFog( true );
		sceGuDisable( GU_BLEND );
		sceGuEnable( GU_ALPHA_TEST );
		break;
	case kRenderGlow:
	case kRenderTransAdd:
		R_AllowFog( false );
		sceGuEnable( GU_BLEND );
		sceGuDisable( GU_ALPHA_TEST );
		sceGuBlendFunc( GU_ADD, GU_SRC_ALPHA, GU_FIX, 0, GUBLEND1 );
		break;
	case kRenderScreenFadeModulate:
		// dst * src color
		R_AllowFog( true );
		sceGuEnable( GU_BLEND );
		sceGuDisable( GU_ALPHA_TEST );
		sceGuBlendFunc( GU_ADD, GU_FIX, GU_SRC_COLOR, GUBLEND0, 0 );
		break;
	}
}

/*
==============
GL_PushPolygonOffset

enable polygon offset and push it onto the stack
GU only has a constant depth offset, factor is ignored. The units are steps of the 16-bit
depth buffer: n of them show a surface through anything less than about
n * z * z / 262140 units in front of it (zNear is 4)
==============
*/
static int gu_polyoffsets[2];
static int gu_num_polyoffsets;

void GL_PushPolygonOffset( float factor, float units )
{
	if( gu_num_polyoffsets >= ARRAYSIZE( gu_polyoffsets ))
	{
		gEngfuncs.Con_Reportf( "%s: overflow\n", __func__ );
		return;
	}

	gu_polyoffsets[gu_num_polyoffsets] = (int)units;
	GU_DepthOffset( gu_polyoffsets[gu_num_polyoffsets] );
	gu_num_polyoffsets++;
}

/*
==============
GL_PopPolygonOffset

pop polygon offset from the stack and restore previous state
==============
*/
void GL_PopPolygonOffset( void )
{
	if( gu_num_polyoffsets <= 0 )
	{
		gEngfuncs.Con_Reportf( "%s: underflow\n", __func__ );
		return;
	}

	gu_num_polyoffsets--;

	if( gu_num_polyoffsets == 0 )
		GU_DepthOffset( 0 );
	else GU_DepthOffset( gu_polyoffsets[gu_num_polyoffsets - 1] );
}

/*
==============================================================================

SCREEN SHOTS

==============================================================================
*/
qboolean VID_ScreenShot( const char *filename, int shot_type )
{
	R_WaitFrame();
	uint flags = 0;
	int width = 0, height = 0;

	rgbdata_t *r_shot = Mem_Calloc( r_temppool, sizeof( rgbdata_t ));
	r_shot->width = (gpGlobals->width + 3) & ~3;
	r_shot->height = (gpGlobals->height + 3) & ~3;
	r_shot->flags = IMAGE_HAS_COLOR;
	r_shot->type = PF_RGB_24;
	int bpp = gEngfuncs.Image_GetPFDesc( r_shot->type )->bpp;
	r_shot->size = r_shot->width * r_shot->height * bpp;
	r_shot->palette = NULL;
	r_shot->buffer = Mem_Malloc( r_temppool, r_shot->size );

	// get the last displayed frame, the framebuffer is top-down already
	// read it through the uncached VRAM mirror, the GE doesn't go through the CPU cache
	const byte *src = (const byte *)((uintptr_t)guRender.disp_buffer | 0x40000000 );
	byte *dst = r_shot->buffer;

	for( int y = 0; y < r_shot->height; y++ )
	{
		GL_PixelConverter( dst, src, r_shot->width, PC_HWF( guRender.buffer_format ), PC_SWF( r_shot->type ));
		dst += r_shot->width * bpp;
		src += guRender.buffer_width * guRender.buffer_bpp;
	}

	switch( shot_type )
	{
	case VID_SCREENSHOT:
		break;
	case VID_SNAPSHOT:
		gEngfuncs.fsapi->AllowDirectPaths( true );
		break;
	case VID_LEVELSHOT:
	case VID_MINISHOT:
		flags |= IMAGE_RESAMPLE;
		height = shot_type == VID_MINISHOT ? 200 : 480;
		width = Q_rint( height * ((double)r_shot->width / r_shot->height ));
		break;
	case VID_MAPSHOT:
		flags |= IMAGE_RESAMPLE|IMAGE_QUANTIZE;	// GoldSrc request overviews in 8-bit format
		height = 768;
		width = 1024;
		break;
	}

	gEngfuncs.Image_Process( &r_shot, width, height, flags, 0.0f );

	// write image
	qboolean result = gEngfuncs.FS_SaveImage( filename, r_shot );
	gEngfuncs.fsapi->AllowDirectPaths( false );			// always reset after store screenshot
	gEngfuncs.FS_FreeImage( r_shot );

	return result;
}

/*
=================
VID_CubemapShot

not supported
=================
*/
qboolean VID_CubemapShot( const char *base, uint size, const float *vieworg, qboolean skyshot )
{
	return false;
}

/*
================
SCR_TimeRefresh_f

timerefresh [noflip]
================
*/
void SCR_TimeRefresh_f( void )
{
	if( ENGINE_GET_PARM( PARM_CONNSTATE ) != ca_active )
		return;

	double start = gEngfuncs.pfnTime();

	// run without page flipping like GoldSrc
	if( gEngfuncs.Cmd_Argc() == 1 )
	{
		for( int i = 0; i < 128; i++ )
		{
			gpGlobals->viewangles[1] = i / 128.0f * 360.0f;
			R_RenderScene();
		}
		R_EndFrame();
	}
	else
	{
		for( int i = 0; i < 128; i++ )
		{
			R_BeginFrame( true );
			gpGlobals->viewangles[1] = i / 128.0f * 360.0f;
			R_RenderScene();
			R_EndFrame();
		}
	}

	double stop = gEngfuncs.pfnTime ();
	double time = (stop - start);
	gEngfuncs.Con_Printf( "%f seconds (%f fps)\n", time, 128 / time );
}
