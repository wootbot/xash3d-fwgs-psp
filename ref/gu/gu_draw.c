/*
gu_draw.c - orthogonal drawing stuff
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

/*
=============
R_GetImageParms
=============
*/
void R_GetTextureParms( int *w, int *h, int texnum )
{
	gl_texture_t *glt = R_GetTexture( texnum );

	if( w ) *w = glt->srcWidth;
	if( h ) *h = glt->srcHeight;
}

/*
=============
R_DrawStretchPic

drawn in through mode (screen coordinates, no transform)
=============
*/
void R_DrawStretchPic( float x, float y, float w, float h, float s1, float t1, float s2, float t2, int texnum )
{
	GL_Bind( XASH_TEXTURE0, texnum );

	gl_texture_t *glt = R_GetTexture( texnum );

	x += glState.offset2D[0];
	y += glState.offset2D[1];

	gu_vert_htv_t *const out = extGuGetMemoryNoStall( sizeof( gu_vert_htv_t ) * 2 );
	out[0].u = s1 * glt->width;
	out[0].v = t1 * glt->height;
	out[0].x = x;
	out[0].y = y;
	out[0].z = 0;
	out[1].u = s2 * glt->width;
	out[1].v = t2 * glt->height;
	out[1].x = x + w;
	out[1].y = y + h;
	out[1].z = 0;
	extGuDrawArrayBatched( GU_SPRITES, GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, out );
}

/*
=============
CL_FillRGBA

drawn in through mode (screen coordinates, no transform)
=============
*/
void CL_FillRGBA( int rendermode, float x, float y, float w, float h, byte r, byte g, byte b, byte a )
{
	sceGuDisable( GU_TEXTURE_2D );
	sceGuEnable( GU_BLEND );
	sceGuTexFunc( GU_TFX_MODULATE, GU_TCC_RGBA );
	if( rendermode == kRenderTransAdd )
		sceGuBlendFunc( GU_ADD, GU_SRC_ALPHA, GU_FIX, 0, GUBLEND1 );
	else
		sceGuBlendFunc( GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0 );
	sceGuColor( GUCOLOR4UB( r, g, b, a ));

	x += glState.offset2D[0];
	y += glState.offset2D[1];

	gu_vert_hv_t *const out = extGuGetMemoryNoStall( sizeof( gu_vert_hv_t ) * 2 );
	out[0].x = x;
	out[0].y = y;
	out[0].z = 0;
	out[1].x = x + w;
	out[1].y = y + h;
	out[1].z = 0;
	extGuDrawArrayBatched( GU_SPRITES, GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, out );

	sceGuColor( 0xffffffff );
	sceGuEnable( GU_TEXTURE_2D );
	sceGuDisable( GU_BLEND );
}

/*
=============
GL_UpdateTexture

cinematic frames (BGRA), not supported yet
=============
*/
void GL_UpdateTexture( int texnum, int cols, int rows, int width, int height, const byte *buffer, pixformat_t fmt )
{
	static qboolean warned;

	if( !warned )
	{
		gEngfuncs.Con_Reportf( S_WARN "%s: not implemented\n", __func__ );
		warned = true;
	}
}

static void R_Load2DModelMatrix( void )
{
	sceGumMatrixMode( GU_MODEL );
	sceGumLoadIdentity();
	if( glState.offset2D[0] != 0.0f || glState.offset2D[1] != 0.0f )
	{
		const ScePspFVector3 offset = { glState.offset2D[0], glState.offset2D[1], 0.0f };
		sceGumTranslate( &offset );
	}
	sceGumUpdateMatrix();
	tr.modelviewIdentity = false;
}

/*
===============
R_Set2DMode
===============
*/
void R_Set2DMode( qboolean enable )
{
	if( enable )
	{
		matrix4x4 projection_matrix;

		if( glState.in2DMode )
			return;

		// set 2D virtual screen size, TriAPI goes through the matrices, sprites use through mode
		sceGuViewport( 2048, 2048, gpGlobals->width, gpGlobals->height );
		sceGuScissor( 0, 0, gpGlobals->width, gpGlobals->height );

		Matrix4x4_CreateOrtho( projection_matrix, 0, gpGlobals->width, gpGlobals->height, 0, -99999, 99999 );
		GU_LoadProjection( projection_matrix );

		sceGumMatrixMode( GU_VIEW );
		sceGumLoadIdentity();

		R_Load2DModelMatrix();

		GL_Cull( GL_NONE );

		sceGuDepthMask( GU_TRUE ); // no depth writes
		sceGuDisable( GU_DEPTH_TEST );
		sceGuEnable( GU_ALPHA_TEST );
		// HUD and console must not be affected by fog
		sceGuDisable( GU_FOG );
		sceGuColor( 0xffffffff );

		glState.in2DMode = true;
		RI.currententity = NULL;
		RI.currentmodel = NULL;
	}
	else
	{
		sceGuDepthMask( GU_FALSE );
		sceGuEnable( GU_DEPTH_TEST );
		// restore the scene fog that was disabled in 2D mode
		if( gl_fog.value && ( RI.fogEnabled || RI.fogCustom ))
			sceGuEnable( GU_FOG );
		glState.in2DMode = false;

		GU_LoadProjection( RI.projectionMatrix );

		sceGumMatrixMode( GU_VIEW );
		GL_LoadMatrix( RI.worldviewMatrix );

		sceGumMatrixMode( GU_MODEL );
		sceGumLoadIdentity();
		sceGumUpdateMatrix();
		Matrix4x4_LoadIdentity( RI.objectMatrix );
		tr.modelviewIdentity = true;

		GL_Cull( GL_FRONT );
	}
}

/*
===============
R_Set2DOffset

===============
*/
void R_Set2DOffset( float x, float y )
{
	Vector2Set( glState.offset2D, x, y );

	if( !glState.in2DMode )
		return;

	R_Load2DModelMatrix();
}
