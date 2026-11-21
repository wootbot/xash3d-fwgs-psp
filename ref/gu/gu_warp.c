/*
gu_warp.c - sky polygons
Copyright (C) 2010 Uncle Mike
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

#include "gu_local.h"
#include "wadfile.h"

#define SKYCLOUDS_QUALITY	12
#define MAX_CLIP_VERTS	128 // skybox clip vertices

static const vec3_t skyclip[SKYBOX_MAX_SIDES] =
{
{  1,  1,  0 },
{  1, -1,  0 },
{  0, -1,  1 },
{  0,  1,  1 },
{  1,  0,  1 },
{ -1,  0,  1 }
};

// 1 = s, 2 = t, 3 = 2048
static const int st_to_vec[SKYBOX_MAX_SIDES][3] =
{
{  3, -1,  2 },
{ -3,  1,  2 },
{  1,  3,  2 },
{ -1, -3,  2 },
{ -2, -1,  3 },  // 0 degrees yaw, look straight up
{  2, -1, -3 }   // look straight down
};


static void DrawSkyPolygon( int nump, vec3_t vecs )
{
	// decide which face it maps to
	vec3_t v;
	VectorClear( v );

	float *vp = vecs;
	for( int i = 0; i < nump; i++, vp += 3 )
		VectorAdd( vp, v, v );

	const int axis = R_SkyboxAxisFromDir( v );

	// project new texture coords
	for( int i = 0; i < nump; i++, vecs += 3 )
	{
		float s, t;

		if( !R_SkyboxProject( vecs, axis, &s, &t ))
			continue;

		if( s < RI.skyMins[0][axis] ) RI.skyMins[0][axis] = s;
		if( t < RI.skyMins[1][axis] ) RI.skyMins[1][axis] = t;
		if( s > RI.skyMaxs[0][axis] ) RI.skyMaxs[0][axis] = s;
		if( t > RI.skyMaxs[1][axis] ) RI.skyMaxs[1][axis] = t;
	}
}

#if XASH_PSP
static qboolean R_SkyPolygonOnOneFace( int nump, const vec3_t vecs )
{
	const int axis = R_SkyboxAxisFromDir( vecs );

	for( int i = 1; i < nump; i++ )
	{
		if( R_SkyboxAxisFromDir( vecs + i * 3 ) != axis )
			return false;
	}
	return true;
}
#endif

/*
==============
ClipSkyPolygon
==============
*/
static void ClipSkyPolygon( int nump, vec3_t vecs, int stage )
{
	const float	*norm;
	float		*v, d, e;
	qboolean		front, back;
	float		dists[MAX_CLIP_VERTS + 1];
	int		sides[MAX_CLIP_VERTS + 1];
	vec3_t		newv[2][MAX_CLIP_VERTS + 1];
	int		newc[2];
	int		i, j;

	if( nump > MAX_CLIP_VERTS )
		gEngfuncs.Host_Error( "%s: MAX_CLIP_VERTS\n", __func__ );
loc1:
	if( stage == 6 )
	{
		// fully clipped, so draw it
		DrawSkyPolygon( nump, vecs );
		return;
	}

	front = back = false;
	norm = skyclip[stage];
	for( i = 0, v = vecs; i < nump; i++, v += 3 )
	{
		d = DotProduct( v, norm );
		if( d > ON_EPSILON )
		{
			front = true;
			sides[i] = SIDE_FRONT;
		}
		else if( d < -ON_EPSILON )
		{
			back = true;
			sides[i] = SIDE_BACK;
		}
		else
		{
			sides[i] = SIDE_ON;
		}
		dists[i] = d;
	}

	if( !front || !back )
	{
		// not clipped
		stage++;
		goto loc1;
	}

	// clip it
	sides[i] = sides[0];
	dists[i] = dists[0];
	VectorCopy( vecs, ( vecs + ( i * 3 )));
	newc[0] = newc[1] = 0;

	for( i = 0, v = vecs; i < nump; i++, v += 3 )
	{
		switch( sides[i] )
		{
		case SIDE_FRONT:
			VectorCopy( v, newv[0][newc[0]] );
			newc[0]++;
			break;
		case SIDE_BACK:
			VectorCopy( v, newv[1][newc[1]] );
			newc[1]++;
			break;
		case SIDE_ON:
			VectorCopy( v, newv[0][newc[0]] );
			newc[0]++;
			VectorCopy( v, newv[1][newc[1]] );
			newc[1]++;
			break;
		}

		if( sides[i] == SIDE_ON || sides[i+1] == SIDE_ON || sides[i+1] == sides[i] )
			continue;

		d = dists[i] / ( dists[i] - dists[i+1] );
		for( j = 0; j < 3; j++ )
		{
			e = v[j] + d * ( v[j+3] - v[j] );
			newv[0][newc[0]][j] = e;
			newv[1][newc[1]][j] = e;
		}
		newc[0]++;
		newc[1]++;
	}

	// continue
	ClipSkyPolygon( newc[0], newv[0][0], stage + 1 );
	ClipSkyPolygon( newc[1], newv[1][0], stage + 1 );
}


static void MakeSkyVec( float s, float t, int axis, gu_vert_t *out )
{
	int farclip = RI.farClip;

	vec3_t b;
	b[0] = s * (farclip >> 1);
	b[1] = t * (farclip >> 1);
	b[2] = (farclip >> 1);

	for( int j = 0; j < 3; j++ )
	{
		int k = st_to_vec[axis][j];
		out->xyz[j] = (k < 0) ? -b[-k-1] : b[k-1];
		out->xyz[j] += RI.cullorigin[j];
	}

	// avoid bilerp seam
	s = (s + 1.0f) * 0.5f;
	t = (t + 1.0f) * 0.5f;

	s = bound( 1.0f / 512.0f, s, 511.0f / 512.0f );
	t = bound( 1.0f / 512.0f, t, 511.0f / 512.0f );

	t = 1.0f - t;

	out->uv[0] = s;
	out->uv[1] = t;
}

/*
==============
GU_DrawClippedPoly

draws a polygon, clipping it against the frustum if it crosses the guard band
==============
*/
static void GU_DrawClippedPoly( gu_vert_t *verts, int numverts )
{
	if( GU_ClipIsRequired( verts, numverts ))
	{
		gu_vert_t *cv;
		int cvc;

		GU_Clip( verts, numverts, &cv, &cvc );
		if( cvc ) extGuDrawArrayBatched( GU_TRIANGLE_FAN, GU_TEXTURE_32BITF | GU_VERTEX_32BITF, cvc, cv );
	}
	else extGuDrawArrayBatched( GU_TRIANGLE_FAN, GU_TEXTURE_32BITF | GU_VERTEX_32BITF, numverts, verts );
}

/*
==============
R_ClearSkyBox
==============
*/
void R_ClearSkyBox( void )
{
	for( int i = 0; i < SKYBOX_MAX_SIDES; i++ )
	{
		RI.skyMins[0][i] = RI.skyMins[1][i] = 9999999.0f;
		RI.skyMaxs[0][i] = RI.skyMaxs[1][i] = -9999999.0f;
	}
}

/*
=================
R_AddSkyBoxSurface
=================
*/
void R_AddSkyBoxSurface( msurface_t *fa )
{
	if( FBitSet( tr.world->flags, FWORLD_SKYSPHERE ) && fa->polys && !FBitSet( tr.world->flags, FWORLD_CUSTOM_SKYBOX ))
	{
		gu_poly_t *p = GU_POLYS( fa );

		// draw the sky poly
		extGuDrawArrayBatched( GU_TRIANGLE_FAN, GU_TEXTURE_32BITF | GU_VERTEX_32BITF, p->numverts, p->verts );
	}

	// calculate vertex values for sky box
	vec3_t verts[MAX_CLIP_VERTS];
	for( gu_poly_t *p = GU_POLYS( fa ); p; p = p->next )
	{
		for( int i = 0; i < p->numverts; i++ )
			VectorSubtract( p->verts[i].xyz, RI.cullorigin, verts[i] );
#if XASH_PSP
		// a polygon with all its vertices on one face of the box lies on that
		// face (each face's directions are a convex cone): the clipping would
		// only cut it into pieces with the same bounds there (~70% of the sky
		// polygons in a frame, ~12 us each)
		if( R_SkyPolygonOnOneFace( p->numverts, verts[0] ))
		{
			DrawSkyPolygon( p->numverts, verts[0] );
			continue;
		}
#endif
		ClipSkyPolygon( p->numverts, verts[0], 0 );
	}
}

/*
==============
R_UnloadSkybox

Unload previous skybox
==============
*/
void R_UnloadSkybox( void )
{
	// release old skybox
	for( int i = 0; i < SKYBOX_MAX_SIDES; i++ )
	{
		if( !tr.skyboxTextures[i] ) continue;
		GL_FreeTexture( tr.skyboxTextures[i] );
	}

	tr.skyboxbasenum = SKYBOX_BASE_NUM;	// set skybox base (to let some mods load hi-res skyboxes)

	memset( tr.skyboxTextures, 0, sizeof( tr.skyboxTextures ));
	ClearBits( tr.world->flags, FWORLD_CUSTOM_SKYBOX );
}

/*
==============
R_DrawSkybox
==============
*/
void R_DrawSkyBox( void )
{
	// don't fogging skybox (this fix old Half-Life bug)
	if( !RI.fogSkybox ) R_AllowFog( false );

	sceGuDisable( GU_BLEND );
	sceGuDisable( GU_ALPHA_TEST );
	sceGuTexFunc( GU_TFX_REPLACE, GU_TCC_RGBA );

	for( int i = 0; i < SKYBOX_MAX_SIDES; i++ )
	{
		if( RI.skyMins[0][i] >= RI.skyMaxs[0][i] || RI.skyMins[1][i] >= RI.skyMaxs[1][i] )
			continue;

		if( tr.skyboxTextures[r_skyTexOrder[i]] )
			GL_Bind( XASH_TEXTURE0, tr.skyboxTextures[r_skyTexOrder[i]] );
		else GL_Bind( XASH_TEXTURE0, tr.grayTexture ); // stub

		gu_vert_t *const uv = extGuGetMemoryNoStall( sizeof( gu_vert_t ) * 4 );
		MakeSkyVec( RI.skyMins[0][i], RI.skyMins[1][i], i, &uv[0] );
		MakeSkyVec( RI.skyMins[0][i], RI.skyMaxs[1][i], i, &uv[1] );
		MakeSkyVec( RI.skyMaxs[0][i], RI.skyMaxs[1][i], i, &uv[2] );
		MakeSkyVec( RI.skyMaxs[0][i], RI.skyMins[1][i], i, &uv[3] );

		GU_DrawClippedPoly( uv, 4 );
	}

	sceGuTexFunc( GU_TFX_MODULATE, GU_TCC_RGBA );

	if( !RI.fogSkybox )
		R_AllowFog( true );

	R_LoadIdentity();
}

//==============================================================================
//
//  RENDER CLOUDS
//
//==============================================================================
/*
==============
R_CloudVertex
==============
*/
static void R_CloudVertex( float s, float t, int axis, vec3_t v )
{
	int farclip = RI.farClip;

	vec3_t b;
	b[0] = s * (farclip >> 1);
	b[1] = t * (farclip >> 1);
	b[2] = (farclip >> 1);

	for( int j = 0; j < 3; j++ )
	{
		int k = st_to_vec[axis][j];
		v[j] = (k < 0) ? -b[-k-1] : b[k-1];
		v[j] += RI.cullorigin[j];
	}
}

/*
=============
R_CloudTexCoord
=============
*/
static void R_CloudTexCoord( const vec3_t v, float speed, float *s, float *t )
{
	float speedscale = gp_cl->time * speed;
	speedscale -= (int)speedscale & ~127;

	vec3_t dir;
	VectorSubtract( v, RI.rvp.vieworigin, dir );
	dir[2] *= 3.0f; // flatten the sphere

	float length = VectorLength( dir );
	length = 6.0f * 63.0f / length;

	*s = ( speedscale + dir[0] * length ) * (1.0f / 128.0f);
	*t = ( speedscale + dir[1] * length ) * (1.0f / 128.0f);
}

/*
===============
R_CloudDrawPoly
===============
*/
static void R_CloudDrawPoly( vec3_t verts[4] )
{
	GL_SetRenderMode( kRenderNormal );
	GL_Bind( XASH_TEXTURE0, tr.solidskyTexture );

	gu_vert_t *out = extGuGetMemoryNoStall( sizeof( gu_vert_t ) * 4 );
	for( int i = 0; i < 4; i++ )
	{
		R_CloudTexCoord( verts[i], 8.0f, &out[i].uv[0], &out[i].uv[1] );
		VectorCopy( verts[i], out[i].xyz );
	}
	GU_DrawClippedPoly( out, 4 );

	GL_SetRenderMode( kRenderTransTexture );
	GL_Bind( XASH_TEXTURE0, tr.alphaskyTexture );

	out = extGuGetMemoryNoStall( sizeof( gu_vert_t ) * 4 );
	for( int i = 0; i < 4; i++ )
	{
		R_CloudTexCoord( verts[i], 16.0f, &out[i].uv[0], &out[i].uv[1] );
		VectorCopy( verts[i], out[i].xyz );
	}
	GU_DrawClippedPoly( out, 4 );

	sceGuDisable( GU_BLEND );
}

/*
==============
R_CloudRenderSide
==============
*/
static void R_CloudRenderSide( int axis )
{
	vec3_t verts[4];
	R_CloudVertex( -1.0f, -1.0f, axis, verts[0] );
	R_CloudVertex( -1.0f,  1.0f, axis, verts[1] );
	R_CloudVertex(  1.0f,  1.0f, axis, verts[2] );
	R_CloudVertex(  1.0f, -1.0f, axis, verts[3] );

	vec3_t vup, vright;
	VectorSubtract( verts[2], verts[3], vup );
	VectorSubtract( verts[2], verts[1], vright );

	float di = SKYCLOUDS_QUALITY;
	float qi = 1.0f / di;
	float dj = (axis < 4) ? di * 2 : di; //subdivide vertically more than horizontally on skybox sides
	float qj = 1.0f / dj;

	for( int i = 0; i < di; i++ )
	{
		for( int j = 0; j < dj; j++ )
		{
			if( i * qi < RI.skyMins[0][axis] / 2 + 0.5f - qi
			 || i * qi > RI.skyMaxs[0][axis] / 2 + 0.5f
			 || j * qj < RI.skyMins[1][axis] / 2 + 0.5f - qj
			 || j * qj > RI.skyMaxs[1][axis] / 2 + 0.5f )
				continue;

			vec3_t temp, temp2;
			VectorScale( vright, qi * i, temp );
			VectorScale( vup, qj * j, temp2 );
			VectorAdd( temp, temp2, temp );
			vec3_t final_verts[4];
			VectorAdd( verts[0], temp, final_verts[0] );

			VectorScale( vup, qj, temp );
			VectorAdd( final_verts[0], temp, final_verts[1] );

			VectorScale( vright, qi, temp );
			VectorAdd( final_verts[1], temp, final_verts[2] );

			VectorAdd( final_verts[0], temp, final_verts[3] );

			R_CloudDrawPoly( final_verts );
		}
	}
}

/*
==============
R_DrawClouds

Quake-style clouds
==============
*/
void R_DrawClouds( void )
{
	sceGuDepthFunc( GU_GEQUAL );
	sceGuDepthMask( GU_TRUE ); // no depth writes

	for( int i = 0; i < SKYBOX_MAX_SIDES; i++ )
	{
		if( RI.skyMins[0][i] >= RI.skyMaxs[0][i] || RI.skyMins[1][i] >= RI.skyMaxs[1][i] )
			continue;
		R_CloudRenderSide( i );
	}

	sceGuDepthFunc( GU_LEQUAL );
	sceGuDepthMask( GU_FALSE );
}
