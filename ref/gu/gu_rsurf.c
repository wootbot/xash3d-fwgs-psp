/*
gu_rsurf.c - surface-related refresh code
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

/*
Differences from ref_gl's gl_rsurf.c:
- polygons are stored as gu_poly_t: numverts texture vertices followed by numverts lightmap
  vertices, both in the GU_TEXTURE_32BITF | GU_VERTEX_32BITF layout, so they are drawn straight
  from memory (and clipped against the guard band when needed, see gu_clipping.c)
- lightmaps are stored in LIGHTMAP_FORMAT (8-bit RGB 332 by default) and drawn in a separate
  multiplicative pass, there's no overbright
- no VBO, no lightmapped water, no ripples, no wireframe
*/

#include "gu_local.h"
#include "xash3d_mathlib.h"
#include "mod_local.h"
#include "atlas.h"

#define TURBSCALE		( 256.0f / ( M_PI2 ))

// speed up sin calculations
static const float r_turbsin[] =
{
#include "warpsin.h"
};

typedef struct
{
	atlas_t		atlas;
	int		current_lightmap_texture;
	msurface_t	*dynamic_surfaces;
	msurface_t	*lightmap_surfaces[MAX_LIGHTMAPS];
	byte		lightmap_buffer[BLOCK_SIZE_MAX*BLOCK_SIZE_MAX*LIGHTMAP_BPP];
} gllightmapstate_t;

static vec2_t		world_orthocenter;
static vec2_t		world_orthohalf;
static uint		r_blocklights[BLOCK_SIZE_MAX*BLOCK_SIZE_MAX*3];
static mextrasurf_t		*fullbright_surfaces[MAX_TEXTURES];
static mextrasurf_t		*detail_surfaces[MAX_TEXTURES];

typedef struct
{
	int first, last;
} separate_pass_t;

static separate_pass_t draw_wateralpha = { 0, -1 };
static separate_pass_t draw_alpha_surfaces = { 0, -1 };
static separate_pass_t draw_fullbrights = { 0, -1 };
static separate_pass_t draw_details = { 0, -1 };
static msurface_t		*skychain = NULL;
static gllightmapstate_t	gl_lms;

static void LM_UploadBlock( qboolean dynamic );

static inline void R_AddToSeparatePass( separate_pass_t *sp, int num )
{
	if( sp->first > num )
		sp->first = num;

	if( sp->last < num )
		sp->last = num;
}

static inline void R_ResetSeparatePass( separate_pass_t *sp )
{
	sp->last = -1;
}

static inline qboolean R_SeparatePassActive( const separate_pass_t *sp )
{
	return sp->last >= 0 ? true : false;
}

byte *Mod_GetCurrentVis( void )
{
	if( gEngfuncs.drawFuncs->Mod_GetCurrentVis && tr.fCustomRendering )
		return gEngfuncs.drawFuncs->Mod_GetCurrentVis();
	return RI.visbytes;
}

void Mod_SetOrthoBounds( const float *mins, const float *maxs )
{
	if( gEngfuncs.drawFuncs->GL_OrthoBounds )
	{
		gEngfuncs.drawFuncs->GL_OrthoBounds( mins, maxs );
	}

	Vector2Average( maxs, mins, world_orthocenter );
	Vector2Subtract( maxs, world_orthocenter, world_orthohalf );
}

void R_LightmapCoord( const vec3_t v, const msurface_t *surf, const float sample_size, vec2_t coords )
{
	const mextrasurf_t *info = surf->info;

	float s = DotProduct( v, info->lmvecs[0] ) + info->lmvecs[0][3] - info->lightmapmins[0];
	s += surf->light_s * sample_size;
	s += sample_size * 0.5f;
	s /= BLOCK_SIZE * sample_size; //fa->texinfo->texture->width;

	float t = DotProduct( v, info->lmvecs[1] ) + info->lmvecs[1][3] - info->lightmapmins[1];
	t += surf->light_t * sample_size;
	t += sample_size * 0.5f;
	t /= BLOCK_SIZE * sample_size; //fa->texinfo->texture->width;

	Vector2Set( coords, s, t );
}

static void R_TextureCoord( const vec3_t v, const msurface_t *surf, vec2_t coords )
{
	const mtexinfo_t *info = surf->texinfo;

	float s = DotProduct( v, info->vecs[0] );
	float t = DotProduct( v, info->vecs[1] );

	if( !FBitSet( surf->flags, SURF_DRAWTURB ))
	{
		s = ( s + info->vecs[0][3] ) / info->texture->width;
		t = ( t + info->vecs[1][3] ) / info->texture->height;
	}

	Vector2Set( coords, s, t );
}

static void R_GetEdgePosition( const model_t *mod, const msurface_t *fa, int i, vec3_t vec )
{
	const int lindex = mod->surfedges[fa->firstedge + i];

	if( FBitSet( mod->flags, MODEL_QBSP2 ))
	{
		const medge32_t *pedges = mod->edges32;

		if( lindex > 0 )
			VectorCopy( mod->vertexes[pedges[lindex].v[0]].position, vec );
		else
			VectorCopy( mod->vertexes[pedges[-lindex].v[1]].position, vec );
	}
	else
	{
		const medge16_t *pedges = mod->edges16;

		if( lindex > 0 )
			VectorCopy( mod->vertexes[pedges[lindex].v[0]].position, vec );
		else
			VectorCopy( mod->vertexes[pedges[-lindex].v[1]].position, vec );
	}
}

static void BoundPoly( int numverts, float *verts, vec3_t mins, vec3_t maxs )
{
	float	*v;
	int	i;

	ClearBounds( mins, maxs );

	for( i = 0, v = verts; i < numverts; i++ )
	{
		for( int j = 0; j < 3; j++, v++ )
		{
			if( *v < mins[j] ) mins[j] = *v;
			if( *v > maxs[j] ) maxs[j] = *v;
		}
	}
}

static void SubdividePolygon_r( model_t *loadmodel, msurface_t *warpface, int numverts, float *verts )
{
	vec3_t		front[SUBDIVIDE_SIZE], back[SUBDIVIDE_SIZE];
	float		dist[SUBDIVIDE_SIZE];
	float		*v;
	int		j;
	vec3_t		mins, maxs;

	if( numverts > ( SUBDIVIDE_SIZE - 4 ))
		gEngfuncs.Host_Error( "%s: too many vertexes on face ( %i )\n", __func__, numverts );

	BoundPoly( numverts, verts, mins, maxs );

	for( int i = 0; i < 3; i++ )
	{
		float m = ( mins[i] + maxs[i] ) * 0.5f;
		m = SUBDIVIDE_SIZE * floor( m / SUBDIVIDE_SIZE + 0.5f );
		if( maxs[i] - m < 8 ) continue;
		if( m - mins[i] < 8 ) continue;

		// cut it
		v = verts + i;
		for( j = 0; j < numverts; j++, v += 3 )
			dist[j] = *v - m;

		// wrap cases
		dist[j] = dist[0];
		v -= i;
		VectorCopy( verts, v );

		int f = 0, b = 0;
		v = verts;
		for( j = 0; j < numverts; j++, v += 3 )
		{
			if( dist[j] >= 0 )
			{
				VectorCopy( v, front[f] );
				f++;
			}

			if( dist[j] <= 0 )
			{
				VectorCopy (v, back[b]);
				b++;
			}

			if( dist[j] == 0 || dist[j+1] == 0 )
				continue;

			if(( dist[j] > 0 ) != ( dist[j+1] > 0 ))
			{
				// clip point
				float frac = dist[j] / ( dist[j] - dist[j+1] );
				for( int k = 0; k < 3; k++ )
					front[f][k] = back[b][k] = v[k] + frac * (v[3+k] - v[k]);
				f++;
				b++;
			}
		}

		SubdividePolygon_r( loadmodel, warpface, f, front[0] );
		SubdividePolygon_r( loadmodel, warpface, b, back[0] );
		return;
	}

	if( numverts != 4 )
		ClearBits( warpface->flags, SURF_DRAWTURB_QUADS );

	// warped polygons have no lightmap vertices
	gu_poly_t *poly = Mem_Calloc( loadmodel->mempool, sizeof( gu_poly_t ) + numverts * sizeof( gu_vert_t ));
	poly->next = GU_POLYS( warpface );
	poly->flags = warpface->flags;
	warpface->polys = (glpoly2_t *)poly;
	poly->numverts = numverts;

	for( int i = 0; i < numverts; i++, verts += 3 )
	{
		VectorCopy( verts, poly->verts[i].xyz );
		R_TextureCoord( verts, warpface, poly->verts[i].uv );
	}
}

/*
===============================
GL_SetupFogColorForSurfaces

every render pass applies new fog layer, resulting in wrong fog color
recalculate fog color for current pass count
===============================
*/
static void GL_SetupFogColorForSurfacesEx( int passes )
{
	if( !glState.isFogEnabled )
		return;

	if(( passes < 2 ) || (RI.currententity && RI.currententity->curstate.rendermode == kRenderTransTexture ))
	{
		GL_ResetFogColor();
		return;
	}

	float div = passes - 1;
	float factor = passes;
	vec3_t fogColor;
	fogColor[0] = pow( RI.fogColor[0] / div, ( 1.0f / factor ));
	fogColor[1] = pow( RI.fogColor[1] / div, ( 1.0f / factor ));
	fogColor[2] = pow( RI.fogColor[2] / div, ( 1.0f / factor ));

	sceGuFog( glState.fogStart, glState.fogEnd, GUCOLOR3FV( fogColor ));
}

void GL_SetupFogColorForSurfaces( void )
{
	GL_SetupFogColorForSurfacesEx( r_detailtextures.value ? 3 : 2 );
}

void GL_ResetFogColor( void )
{
	// restore fog here
	if( glState.isFogEnabled )
		sceGuFog( glState.fogStart, glState.fogEnd, glState.fogColor );
}

/*
================
GL_SubdivideSurface

Breaks a polygon up along axial 64 unit
boundaries so that turbulent and sky warps
can be done reasonably.
================
*/
void GL_SubdivideSurface( model_t *loadmodel, msurface_t *fa )
{
	vec3_t	verts[SUBDIVIDE_SIZE];

	// convert edges back to a normal polygon
	for( int i = 0; i < fa->numedges; i++ )
		R_GetEdgePosition( loadmodel, fa, i, verts[i] );

	SetBits( fa->flags, SURF_DRAWTURB_QUADS ); // predict state

	// do subdivide
	SubdividePolygon_r( loadmodel, fa, fa->numedges, verts[0] );
}

/*
================
GL_BuildPolygonFromSurface
================
*/
static int GL_BuildPolygonFromSurface( model_t *mod, msurface_t *fa )
{
	int		nColinElim = 0;

	if( !mod || !fa->texinfo || !fa->texinfo->texture )
		return nColinElim; // bad polygon ?

	if( FBitSet( fa->flags, SURF_CONVEYOR ) && fa->texinfo->texture->gl_texturenum != 0 )
	{
		gl_texture_t *glt = R_GetTexture( fa->texinfo->texture->gl_texturenum );
		texture_t *tex = fa->texinfo->texture;
		Assert( glt != NULL && tex != NULL );

		// update conveyor widths for keep properly speed of scrolling
		glt->srcWidth = tex->width;
		glt->srcHeight = tex->height;
	}

	float sample_size = gEngfuncs.Mod_SampleSizeForFace( fa );

	// reconstruct the polygon
	int lnumverts = fa->numedges;

	// detach if already created, reconstruct again
	gu_poly_t *poly = GU_POLYS( fa );
	fa->polys = NULL;

	// quake simple models (healthkits etc) need to be reconstructed their polys because LM coords has changed after the map change
	// PSP GU-friendly format s1t1 xyz, then s2t2 xyz (idea from PSP Quake)
	poly = Mem_Realloc( mod->mempool, poly, GU_POLY_SIZE( lnumverts ));
	poly->next = GU_POLYS( fa );
	poly->flags = fa->flags;
	fa->polys = (glpoly2_t *)poly;
	poly->numverts = lnumverts;

	gu_vert_t *lm = &poly->verts[lnumverts];

	for( int i = 0; i < lnumverts; i++ )
	{
		R_GetEdgePosition( mod, fa, i, poly->verts[i].xyz );
		R_TextureCoord( poly->verts[i].xyz, fa, poly->verts[i].uv );
		VectorCopy( poly->verts[i].xyz, lm[i].xyz );
		R_LightmapCoord( lm[i].xyz, fa, sample_size, lm[i].uv );
	}

	// remove co-linear points - Ed
	if( !gl_keeptjunctions.value && !FBitSet( fa->flags, SURF_UNDERWATER ))
	{
		for( int i = 0; i < lnumverts; i++ )
		{
			vec3_t	v1, v2;

			const float *prev = poly->verts[(i + lnumverts - 1) % lnumverts].xyz;
			const float *next = poly->verts[(i + 1) % lnumverts].xyz;
			const float *this = poly->verts[i].xyz;

			VectorSubtract( this, prev, v1 );
			VectorNormalize( v1 );
			VectorSubtract( next, prev, v2 );
			VectorNormalize( v2 );

			// skip co-linear points
			if(( fabs( v1[0] - v2[0] ) <= 0.001f) && (fabs( v1[1] - v2[1] ) <= 0.001f) && (fabs( v1[2] - v2[2] ) <= 0.001f))
			{
				for( int j = i + 1; j < lnumverts; j++ )
				{
					poly->verts[j - 1] = poly->verts[j];
					lm[j - 1] = lm[j];
				}

				// retry next vertex next time, which is now current vertex
				lnumverts--;
				nColinElim++;
				i--;
			}
		}

		// lightmap vertices must follow the texture vertices
		if( lnumverts != poly->numverts )
			memmove( &poly->verts[lnumverts], lm, lnumverts * sizeof( gu_vert_t ));
	}

	poly->numverts = lnumverts;
	return nColinElim;
}

/*
===============
R_TextureAnimation

Returns the proper texture for a given time and surface
===============
*/
static texture_t *R_TextureAnimation( msurface_t *s )
{
	texture_t	*base = s->texinfo->texture;
	int	reletive;

	if( RI.currententity && RI.currententity->curstate.frame )
	{
		if( base->alternate_anims )
			base = base->alternate_anims;
	}

	if( !base->anim_total )
		return base;

	if( base->name[0] == '-' )
	{
		int	tx = (int)((s->texturemins[0] + (base->width << 16)) / base->width) % MOD_FRAMES;
		int	ty = (int)((s->texturemins[1] + (base->height << 16)) / base->height) % MOD_FRAMES;

		reletive = rtable[tx][ty] % base->anim_total;
	}
	else
	{
		int	speed;

		// Quake1 textures uses 10 frames per second
		if( FBitSet( R_GetTexture( base->gl_texturenum )->flags, TF_QUAKEPAL ))
			speed = 10;
		else speed = 20;

		reletive = (int)(gp_cl->time * speed) % base->anim_total;
	}

	int count = 0;

	while( base->anim_min > reletive || base->anim_max <= reletive )
	{
		base = base->anim_next;

		if( !base || ++count > MOD_FRAMES )
			return s->texinfo->texture;
	}

	return base;
}

/*
===============
R_AddDynamicLights
===============
*/
static void R_AddDynamicLights( const msurface_t *surf, float sample_size, int smax, int tmax )
{
	const mextrasurf_t *info = surf->info;
	int sample_frac = 1.0;

	// no dlighted surfaces here
	if( !surf->dlightbits )
		return;

	mtexinfo_t *tex = surf->texinfo;

	if( FBitSet( tex->flags, TEX_WORLD_LUXELS ))
	{
		if( surf->texinfo->faceinfo )
			sample_frac = surf->texinfo->faceinfo->texture_step;
		else if( FBitSet( surf->texinfo->flags, TEX_EXTRA_LIGHTMAP ))
			sample_frac = LM_SAMPLE_EXTRASIZE;
		else sample_frac = LM_SAMPLE_SIZE;
	}

	for( int lnum = 0; lnum < MAX_DLIGHTS; lnum++ )
	{
		vec3_t impact, origin_l;

		if( !FBitSet( surf->dlightbits, BIT( lnum )))
			continue;	// not lit by this light

		dlight_t *dl = &gp_dlights[lnum];

		// transform light origin to local bmodel space
		if( !tr.modelviewIdentity )
			Matrix4x4_VectorITransform( RI.objectMatrix, dl->origin, origin_l );
		else VectorCopy( dl->origin, origin_l );

		float rad = dl->radius;
		float dist = PlaneDiff( origin_l, surf->plane );
		rad -= fabs( dist );

		// rad is now the highest intensity on the plane
		float minlight = dl->minlight;
		if( rad < minlight )
			continue;

		minlight = rad - minlight;

		if( surf->plane->type < 3 )
		{
			VectorCopy( origin_l, impact );
			impact[surf->plane->type] -= dist;
		}
		else VectorMA( origin_l, -dist, surf->plane->normal, impact );

		float sl = DotProduct( impact, info->lmvecs[0] ) + info->lmvecs[0][3] - info->lightmapmins[0];
		float tl = DotProduct( impact, info->lmvecs[1] ) + info->lmvecs[1][3] - info->lightmapmins[1];

		// dist >= max( sd, td ), so a luxel can only pass the test below when both
		// sd and td are under minlight
		float half = ( minlight + 1.0f ) / ( sample_size * sample_frac );
		int s0 = Q_max( 0, (int)( sl / sample_size - half ));
		int s1 = Q_min( smax - 1, (int)( sl / sample_size + half ));
		int t0 = Q_max( 0, (int)( tl / sample_size - half ));
		int t1 = Q_min( tmax - 1, (int)( tl / sample_size + half ));

		for( int t = t0; t <= t1; t++ )
		{
			int td = (tl - sample_size * t) * sample_frac;

			if( td < 0 )
				td = -td;

			for( int s = s0; s <= s1; s++ )
			{
				int sd = (sl - sample_size * s) * sample_frac;
				float dist;

				if( sd < 0 )
					sd = -sd;

				if( sd > td )
					dist = sd + (td >> 1);
				else
					dist = td + (sd >> 1);

				if( dist < minlight )
				{
					uint *bl = &r_blocklights[(s + (t * smax)) * 3];

					bl[0] += ((int)((rad - dist) * 256) * dl->color.r ) / 256;
					bl[1] += ((int)((rad - dist) * 256) * dl->color.g ) / 256;
					bl[2] += ((int)((rad - dist) * 256) * dl->color.b ) / 256;
				}
			}
		}
	}
}

/*
=============================================================================

  LIGHTMAP ALLOCATION

=============================================================================
*/
static void LM_InitBlock( void )
{
	Atlas_Init( &gl_lms.atlas, BLOCK_SIZE );
}

static qboolean LM_AllocBlock( int w, int h, int *x, int *y )
{
	return Atlas_AllocBlock( &gl_lms.atlas, w, h, x, y );
}

static void LM_UploadBlock( qboolean dynamic )
{
	if( dynamic )
	{
		// NOTE: the GE reads the texture when it executes the list, after this returns
		GU_UpdateTextureRect( tr.dlightTexture, 0, 0, BLOCK_SIZE, gl_lms.atlas.max_height, gl_lms.lightmap_buffer );
		glState.currentTexture = -1; // force rebind
		GL_Bind( XASH_TEXTURE0, tr.dlightTexture );
	}
	else
	{
		rgbdata_t	r_lightmap;
		char	lmName[16];
		int i = gl_lms.current_lightmap_texture;

		// upload static lightmaps only during loading
		memset( &r_lightmap, 0, sizeof( r_lightmap ));
		Q_snprintf( lmName, sizeof( lmName ), "*lightmap%i", i );

		r_lightmap.width = BLOCK_SIZE;
		r_lightmap.height = BLOCK_SIZE;
		r_lightmap.type = LIGHTMAP_FORMAT;
		r_lightmap.size = r_lightmap.width * r_lightmap.height * LIGHTMAP_BPP;
		r_lightmap.flags = IMAGE_HAS_COLOR;
		r_lightmap.buffer = gl_lms.lightmap_buffer;
		tr.lightmapTextures[i] = GL_LoadTextureInternal( lmName, &r_lightmap, TF_NOMIPMAP|TF_ATLAS_PAGE );

		if( ++gl_lms.current_lightmap_texture == MAX_LIGHTMAPS )
			gEngfuncs.Host_Error( "%s: full\n", __func__ );
	}
}

/*
=================
R_BuildLightmap

Combine and scale multiple lightmaps into the floating
format in r_blocklights, then pack into LIGHTMAP_FORMAT
=================
*/
static void R_BuildLightMap( const msurface_t *surf, byte *restrict dest, int stride, qboolean dynamic )
{
	const mextrasurf_t *info = surf->info;
	const qboolean linear_gamma = FBitSet( gp_host->features, ENGINE_LINEAR_GAMMA_SPACE );
	const uint16_t *restrict lightgammatable = tr.lightgammatable;
	const int sample_size = gEngfuncs.Mod_SampleSizeForFace( surf );
	const int smax = ( info->lightextents[0] / sample_size ) + 1;
	const int tmax = ( info->lightextents[1] / sample_size ) + 1;
	const int size = smax * tmax;

	// no overbright: lightmaps are multiplied with the textures in a separate pass
	const int lightscale = ( pow( 2.0f, 1.0f / v_lightgamma->value ) * 256 ) + 0.5;

	int map;
	qboolean init = false;

	// init the lightmap
	for( map = 0; map < MAXLIGHTMAPS && surf->samples; map++ )
	{
		if( surf->styles[map] >= 255 )
			break;

		uint scale = g_lightstylevalue[surf->styles[map]];
		const color24 *lm = &surf->samples[map * size];
		for( int i = 0; i < size; i++ )
		{
			r_blocklights[i * 3 + 0] = lm[i].r * scale;
			r_blocklights[i * 3 + 1] = lm[i].g * scale;
			r_blocklights[i * 3 + 2] = lm[i].b * scale;
		}
		init = true;
		break;
	}

	if( init )
	{
		// add the remaining lightmaps
		for( map++ ; map < MAXLIGHTMAPS; map++ )
		{
			if( surf->styles[map] >= 255 )
				break;

			uint scale = g_lightstylevalue[surf->styles[map]];
			const color24 *lm = &surf->samples[map * size];
			for( int i = 0; i < size; i++ )
			{
				r_blocklights[i * 3 + 0] += lm[i].r * scale;
				r_blocklights[i * 3 + 1] += lm[i].g * scale;
				r_blocklights[i * 3 + 2] += lm[i].b * scale;
			}
		}
	}
	else
		memset( r_blocklights, 0, sizeof( uint ) * size * 3 );

	// add all the dynamic lights
	if( surf->dlightframe == tr.framecount && dynamic )
		R_AddDynamicLights( surf, sample_size, smax, tmax );

	for( int t = 0; t < tmax; t++ )
	{
		for( int s = 0; s < smax; s++ )
		{
			const uint *restrict bl = &r_blocklights[(s + (t * smax)) * 3];
			byte *restrict dst = &dest[(t * stride) + (s * LIGHTMAP_BPP)];
			byte color[3];

			for( int i = 0; i < 3; i++ )
			{
				int c = bl[i] * lightscale >> 14;

				if( c > 1023 )
					c = 1023;

				if( linear_gamma )
					color[i] = c >> 2;
				else
					color[i] = lightgammatable[c] >> 2;
			}

#if LIGHTMAP_BPP == 1
			dst[0]  = ( color[0] >> 5 ) & 0x07;
			dst[0] |= ( color[1] >> 2 ) & 0x38;
			dst[0] |= ( color[2]      ) & 0xc0;
#elif LIGHTMAP_BPP == 2
			dst[0]  = ( color[0] >> 3 ) & 0x1f;
			dst[0] |= ( color[1] << 3 ) & 0xe0;
			dst[1]  = ( color[1] >> 5 ) & 0x07;
			dst[1] |= ( color[2]      ) & 0xf8;
#elif LIGHTMAP_BPP == 3
			dst[0] = color[0];
			dst[1] = color[1];
			dst[2] = color[2];
#else
			dst[0] = color[0];
			dst[1] = color[1];
			dst[2] = color[2];
			dst[3] = 255;
#endif
		}
	}
}

/*
================
GU_DrawPolyVerts

draws a triangle fan straight from memory, clipping it if it crosses the guard band
================
*/
static void GU_DrawPolyVerts( gu_vert_t *verts, int numverts )
{
	if( GU_ClipIsRequired( verts, numverts ))
	{
		gu_vert_t *cv;
		int cvc;

		GU_Clip( verts, numverts, &cv, &cvc );
		if( cvc )
		{
#if CLIPPING_DEBUGGING
			sceGuDisable( GU_TEXTURE_2D );
			sceGuColor( 0xff0000ff );
#endif
			extGuDrawArrayBatched( GU_TRIANGLE_FAN, GU_TEXTURE_32BITF | GU_VERTEX_32BITF, cvc, cv );
#if CLIPPING_DEBUGGING
			sceGuEnable( GU_TEXTURE_2D );
			sceGuColor( 0xffffffff );
#endif
		}
	}
	else extGuDrawArrayBatched( GU_TRIANGLE_FAN, GU_TEXTURE_32BITF | GU_VERTEX_32BITF, numverts, verts );
}

/*
================
GU_DrawPolyVertsCopy

GU_DrawPolyVerts for vertices built in cached memory: the clip test reads
them there, not back from the uncached display list
================
*/
static void GU_DrawPolyVertsCopy( gu_vert_t *verts, int numverts )
{
	if( GU_ClipIsRequired( verts, numverts ))
	{
		GU_DrawPolyVerts( verts, numverts );
		return;
	}

	gu_vert_t *out = extGuGetMemoryNoStall( sizeof( gu_vert_t ) * numverts );
	memcpy( out, verts, sizeof( gu_vert_t ) * numverts );
	extGuDrawArrayBatched( GU_TRIANGLE_FAN, GU_TEXTURE_32BITF | GU_VERTEX_32BITF, numverts, out );
}

/*
================
DrawGLPoly
================
*/
static void DrawGLPoly( gu_poly_t *p, float xScale, float yScale )
{
	float sOffset, tOffset;

	if( !p )
		return;

	if( FBitSet( p->flags, SURF_DRAWTILED ))
		GL_ResetFogColor();

	if( FBitSet( p->flags, SURF_CONVEYOR ))
	{
		const cl_entity_t *e = RI.currententity;
		float flConveyorSpeed;
		float sy, cy;

		if( e == CL_GetEntityByIndex( 0 ) && FBitSet( gp_host->features, ENGINE_QUAKE_COMPATIBLE ))
		{
			// same as doom speed
			flConveyorSpeed = -35.0f;
		}
		else
		{
			flConveyorSpeed = (e->curstate.rendercolor.g<<8|e->curstate.rendercolor.b) / 16.0f;
			if( e->curstate.rendercolor.r ) flConveyorSpeed = -flConveyorSpeed;
		}
		gl_texture_t *texture = R_GetTexture( glState.currentTexture );

		float flRate = fabs( flConveyorSpeed ) / (float)texture->srcWidth;
		float flAngle = ( flConveyorSpeed >= 0 ) ? 180 : 0;

		SinCos( flAngle * ( M_PI_F / 180.0f ), &sy, &cy );
		sOffset = gp_cl->time * cy * flRate;
		tOffset = gp_cl->time * sy * flRate;

		// make sure that we are positive
		if( sOffset < 0.0f ) sOffset += 1.0f + -(int)sOffset;
		if( tOffset < 0.0f ) tOffset += 1.0f + -(int)tOffset;

		// make sure that we are in a [0,1] range
		sOffset = sOffset - (int)sOffset;
		tOffset = tOffset - (int)tOffset;
	}
	else
	{
		sOffset = tOffset = 0.0f;
	}

	const qboolean hasScale = xScale != 0.0f && yScale != 0.0f;
	const qboolean hasOffset = sOffset != 0.0f || tOffset != 0.0f;

	if( !hasScale )
		xScale = yScale = 1.0f;

	// uv' = ( uv + offset ) * scale
	if( hasScale ) sceGuTexScale( xScale, yScale );
	if( hasOffset ) sceGuTexOffset( sOffset * xScale, tOffset * yScale );

	GU_DrawPolyVerts( p->verts, p->numverts );

	if( hasScale ) sceGuTexScale( 1.0f, 1.0f );
	if( hasOffset ) sceGuTexOffset( 0.0f, 0.0f );

	if( FBitSet( p->flags, SURF_DRAWTILED ))
		GL_SetupFogColorForSurfaces();
}

/*
=============
EmitWaterPolys

Does a water warp on the pre-fragmented gu_poly_t chain
=============
*/
static void EmitWaterPolys( msurface_t *warp, qboolean reverse )
{
	float	waveHeight;

	if( !warp->polys ) return;

	// set the current waveheight
	if( GU_POLYS( warp )->verts[0].xyz[2] >= RI.rvp.vieworigin[2] )
		waveHeight = -RI.currententity->curstate.scale;
	else waveHeight = RI.currententity->curstate.scale;

	// reset fog color for nonlightmapped water
	GL_ResetFogColor();

	// the time terms of the table indices below, once per surface and wrapped to
	// the table period: per vertex they were ~16 software double operations on
	// the PSP (and lost precision in float after a long uptime). The 256 * 1024
	// keeps the sums positive, so (int) truncates them the same way as before.
	const float uv_time = fmod( gp_cl->time * TURBSCALE, 256.0 ) + 256.0f * 1024.0f;
	const float wave_time1 = fmod( gp_cl->time * 160.0, 256.0 ) + 256.0f * 1024.0f;
	const float wave_time2 = fmod( gp_cl->time * 171.0, 256.0 ) + 256.0f * 1024.0f;
	const float uv_scale = 0.125f * TURBSCALE; // TURBSCALE is a double

	for( gu_poly_t *p = GU_POLYS( warp ); p; p = p->next )
	{
		const gu_vert_t *v = reverse ? &p->verts[p->numverts - 1] : &p->verts[0];
		gu_vert_t buf[64];
		gu_vert_t *const out = p->numverts <= ARRAYSIZE( buf ) ? buf : extGuGetMemoryNoStall( sizeof( gu_vert_t ) * p->numverts );

		for( int i = 0; i < p->numverts; i++ )
		{
			float os = v->uv[0];
			float ot = v->uv[1];

			out[i].uv[0] = ( os + r_turbsin[(int)( ot * uv_scale + uv_time ) & 255] ) * ( 1.0f / SUBDIVIDE_SIZE );
			out[i].uv[1] = ( ot + r_turbsin[(int)( os * uv_scale + uv_time ) & 255] ) * ( 1.0f / SUBDIVIDE_SIZE );

			out[i].xyz[0] = v->xyz[0];
			out[i].xyz[1] = v->xyz[1];

			if( waveHeight )
			{
				float nv = r_turbsin[(int)( wave_time1 + v->xyz[1] + v->xyz[0] ) & 255] + 8.0f;
				nv = (r_turbsin[(int)( v->xyz[0] * 5.0f + wave_time2 - v->xyz[1] ) & 255] + 8.0f ) * 0.8f + nv;
				out[i].xyz[2] = nv * waveHeight + v->xyz[2];
			}
			else out[i].xyz[2] = v->xyz[2];

			if( reverse ) v--;
			else v++;
		}

		if( out == buf )
			GU_DrawPolyVertsCopy( out, p->numverts );
		else GU_DrawPolyVerts( out, p->numverts );
	}

	GL_SetupFogColorForSurfaces();
}

/*
================
DrawGLPolyChain

Render lightmaps
================
*/
static void DrawGLPolyChain( gu_poly_t *p, float soffset, float toffset )
{
	const qboolean dynamic = soffset != 0.0f || toffset != 0.0f;

	if( dynamic ) sceGuTexOffset( -soffset, -toffset );

	for( ; p != NULL; p = p->chain )
		GU_DrawPolyVerts( &p->verts[p->numverts], p->numverts );

	if( dynamic ) sceGuTexOffset( 0.0f, 0.0f );
}

static qboolean R_HasLightmap( void )
{
	if( r_fullbright->value || !WORLDMODEL->lightdata )
		return false;

	if( RI.currententity )
	{
		if( RI.currententity->curstate.effects & EF_FULLBRIGHT )
			return false;	// disabled by user

		// check for rendermode
		switch( RI.currententity->curstate.rendermode )
		{
		case kRenderTransTexture:
		case kRenderTransColor:
		case kRenderTransAdd:
		case kRenderGlow:
			return false; // no lightmaps
		}
	}

	return true;
}

/*
================
R_BlendLightmaps
================
*/
static void R_BlendLightmaps( void )
{
	msurface_t	*newsurf = NULL;

	if( !R_HasLightmap() )
		return;

	GL_SetupFogColorForSurfaces();

	if( !r_lightmap->value )
		sceGuEnable( GU_BLEND );
	else sceGuDisable( GU_BLEND );

	sceGuDisable( GU_ALPHA_TEST );

	// lightmapped solid surfaces: dst * lightmap
	sceGuDepthMask( GU_TRUE ); // no depth writes
	sceGuDepthFunc( GU_EQUAL );
	sceGuBlendFunc( GU_ADD, GU_FIX, GU_SRC_COLOR, GUBLEND0, 0 );
	sceGuTexFunc( GU_TFX_MODULATE, GU_TCC_RGBA );
	sceGuColor( 0xffffffff );

	// render static lightmaps first
	for( int i = 0; i < MAX_LIGHTMAPS; i++ )
	{
		if( gl_lms.lightmap_surfaces[i] )
		{
			GL_Bind( XASH_TEXTURE0, tr.lightmapTextures[i] );

			for( msurface_t *surf = gl_lms.lightmap_surfaces[i]; surf != NULL; surf = surf->info->lightmapchain )
				DrawGLPolyChain( GU_POLYS( surf ), 0.0f, 0.0f );
		}
	}

	// render dynamic lightmaps
	if( r_dynamic->value )
	{
		LM_InitBlock();

		newsurf = gl_lms.dynamic_surfaces;

		for( msurface_t *surf = gl_lms.dynamic_surfaces; surf != NULL; surf = surf->info->lightmapchain )
		{
			mextrasurf_t	*info = surf->info;
			byte		*base;

			int sample_size = gEngfuncs.Mod_SampleSizeForFace( surf );
			int smax = ( info->lightextents[0] / sample_size ) + 1;
			int tmax = ( info->lightextents[1] / sample_size ) + 1;

			if( LM_AllocBlock( smax, tmax, &surf->info->dlight_s, &surf->info->dlight_t ))
			{
				base = gl_lms.lightmap_buffer;
				base += ( surf->info->dlight_t * BLOCK_SIZE + surf->info->dlight_s ) * LIGHTMAP_BPP;

				R_BuildLightMap( surf, base, BLOCK_SIZE * LIGHTMAP_BPP, true );
			}
			else
			{
				// upload what we have so far
				LM_UploadBlock( true );

				// draw all surfaces that use this lightmap
				msurface_t *drawsurf;
				for( drawsurf = newsurf; drawsurf != surf; drawsurf = drawsurf->info->lightmapchain )
				{
					DrawGLPolyChain( GU_POLYS( drawsurf ),
						( drawsurf->light_s - drawsurf->info->dlight_s ) * ( 1.0f / (float)BLOCK_SIZE ),
						( drawsurf->light_t - drawsurf->info->dlight_t ) * ( 1.0f / (float)BLOCK_SIZE ));
				}

				newsurf = drawsurf;

				// clear the block
				LM_InitBlock();

				// try uploading the block now
				if( !LM_AllocBlock( smax, tmax, &surf->info->dlight_s, &surf->info->dlight_t ))
					gEngfuncs.Host_Error( "AllocBlock: full\n" );

				base = gl_lms.lightmap_buffer;
				base += ( surf->info->dlight_t * BLOCK_SIZE + surf->info->dlight_s ) * LIGHTMAP_BPP;

				R_BuildLightMap( surf, base, BLOCK_SIZE * LIGHTMAP_BPP, true );
			}
		}

		// draw remainder of dynamic lightmaps that haven't been uploaded yet
		if( newsurf ) LM_UploadBlock( true );

		for( msurface_t *surf = newsurf; surf != NULL; surf = surf->info->lightmapchain )
		{
			DrawGLPolyChain( GU_POLYS( surf ),
				( surf->light_s - surf->info->dlight_s ) * ( 1.0f / (float)BLOCK_SIZE ),
				( surf->light_t - surf->info->dlight_t ) * ( 1.0f / (float)BLOCK_SIZE ));
		}
	}

	sceGuDisable( GU_BLEND );
	sceGuDepthMask( GU_FALSE );
	sceGuDepthFunc( GU_LEQUAL );
	sceGuTexFunc( GU_TFX_REPLACE, GU_TCC_RGBA );
	sceGuColor( 0xffffffff );

	// restore fog here
	GL_ResetFogColor();
}

/*
================
R_RenderFullbrights
================
*/
static void R_RenderFullbrights( void )
{
	if( !R_SeparatePassActive( &draw_fullbrights ))
		return;

	R_AllowFog( false );
	sceGuEnable( GU_BLEND );
	sceGuDepthMask( GU_TRUE ); // no depth writes
	sceGuDisable( GU_ALPHA_TEST );
	sceGuBlendFunc( GU_ADD, GU_FIX, GU_FIX, GUBLEND1, GUBLEND1 );
	sceGuTexFunc( GU_TFX_MODULATE, GU_TCC_RGBA );

	for( int i = draw_fullbrights.first; i <= draw_fullbrights.last; i++ )
	{
		mextrasurf_t *es = fullbright_surfaces[i];
		if( !es )
			continue;

		GL_Bind( XASH_TEXTURE0, i );

		for( mextrasurf_t *p = es; p; p = p->lumachain )
			DrawGLPoly( GU_POLYS( p->surf ), 0.0f, 0.0f );

		fullbright_surfaces[i] = NULL;
		es->lumachain = NULL;
	}

	sceGuDisable( GU_BLEND );
	sceGuDepthMask( GU_FALSE );
	sceGuDisable( GU_ALPHA_TEST );
	sceGuTexFunc( GU_TFX_REPLACE, GU_TCC_RGBA );

	R_ResetSeparatePass( &draw_fullbrights );
	R_AllowFog( true );
}

/*
================
R_RenderDetails
================
*/
static void R_RenderDetails( void )
{
	if( !R_SeparatePassActive( &draw_details ))
		return;

	GL_SetupFogColorForSurfacesEx( 3 );

	sceGuEnable( GU_BLEND );
	sceGuBlendFunc( GU_ADD, GU_DST_COLOR, GU_SRC_COLOR, 0, 0 );
	sceGuTexFunc( GU_TFX_DECAL, GU_TCC_RGBA );
	sceGuDepthFunc( GU_EQUAL );

	for( int i = draw_details.first; i <= draw_details.last; i++ )
	{
		mextrasurf_t *es = detail_surfaces[i];
		if( !es )
			continue;

		GL_Bind( XASH_TEXTURE0, i );

		for( mextrasurf_t *p = es; p; p = p->detailchain )
		{
			msurface_t *fa = p->surf;
			gl_texture_t *glt = R_GetTexture( fa->texinfo->texture->gl_texturenum ); // get texture scale

			DrawGLPoly( GU_POLYS( fa ), glt->xscale, glt->yscale );
		}

		detail_surfaces[i] = NULL;
		es->detailchain = NULL;
	}

	sceGuDisable( GU_BLEND );
	sceGuTexFunc( GU_TFX_REPLACE, GU_TCC_RGBA );
	sceGuDepthFunc( GU_LEQUAL );

	R_ResetSeparatePass( &draw_details );

	// restore fog here
	GL_ResetFogColor();
}

static void R_RenderFullbrightForSurface( msurface_t *fa, texture_t *t )
{
	if( !t->fb_texturenum )
		return;

	fa->info->lumachain = fullbright_surfaces[t->fb_texturenum];
	fullbright_surfaces[t->fb_texturenum] = fa->info;
	R_AddToSeparatePass( &draw_fullbrights, t->fb_texturenum );
}

static void R_RenderDetailsForSurface( msurface_t *fa, texture_t *t )
{
	if( !r_detailtextures.value )
		return;

	if( glState.isFogEnabled )
	{
		// don't apply detail textures for windows in the fog
		if( RI.currententity->curstate.rendermode != kRenderTransTexture )
		{
			// draw stub detail texture for underwater surfaces
			int texturenum = t->dt_texturenum ? t->dt_texturenum : tr.grayTexture;

			fa->info->detailchain = detail_surfaces[texturenum];
			detail_surfaces[texturenum] = fa->info;
			R_AddToSeparatePass( &draw_details, texturenum );
		}
	}
	else if( t->dt_texturenum )
	{
		fa->info->detailchain = detail_surfaces[t->dt_texturenum];
		detail_surfaces[t->dt_texturenum] = fa->info;
		R_AddToSeparatePass( &draw_details, t->dt_texturenum );
	}
}

static void R_RenderDecalsForSurface( msurface_t *fa, int cull_type )
{
	if( RI.currententity->curstate.rendermode == kRenderNormal )
	{
		// batch decals to draw later
		if( tr.num_draw_decals < MAX_DECAL_SURFS && fa->pdecals )
			tr.draw_decals[tr.num_draw_decals++] = fa;
	}
	else
	{
		// if rendermode != kRenderNormal draw decals sequentially
		DrawSurfaceDecals( fa, true, (cull_type == CULL_BACKSIDE));
	}
}

static qboolean R_CheckLightMap( msurface_t *fa )
{
	if( unlikely( !r_dynamic->value ))
		return false;

	if( fa->dlightframe == tr.framecount )
		return true; // dlighted surfaces are always dynamic

	// check for light styles
	for( int maps = 0; maps < MAXLIGHTMAPS && fa->styles[maps] != 255; maps++ )
	{
		const int style = fa->styles[maps];

		if( g_lightstylevalue[style] == fa->cached_light[maps] )
			continue;

		static byte temp[132*132*LIGHTMAP_BPP];
		mextrasurf_t *info = fa->info;
		int sample_size = gEngfuncs.Mod_SampleSizeForFace( fa );
		int smax = ( info->lightextents[0] / sample_size ) + 1;
		int tmax = ( info->lightextents[1] / sample_size ) + 1;

		if( smax < 132 && tmax < 132 )
			R_BuildLightMap( fa, temp, smax * LIGHTMAP_BPP, true );
		else
		{
			smax = Q_min( smax, 132 );
			tmax = Q_min( tmax, 132 );
			memset( temp, 255, sizeof( temp ));
		}

		R_UpdateSurfaceCachedLight( fa );

		// update the static lightmap in place
		GU_UpdateTextureRect( tr.lightmapTextures[fa->lightmaptexturenum], fa->light_s, fa->light_t, smax, tmax, temp );
		return false;
	}

	return false; // no change
}

static void R_RenderLightmapForSurface( msurface_t *fa )
{
	if( !fa->polys || FBitSet( fa->flags, SURF_DRAWTILED|SURF_DRAWTURB ))
		return;

	if( R_CheckLightMap( fa ))
	{
		fa->info->lightmapchain = gl_lms.dynamic_surfaces;
		gl_lms.dynamic_surfaces = fa;
	}
	else
	{
		fa->info->lightmapchain = gl_lms.lightmap_surfaces[fa->lightmaptexturenum];
		gl_lms.lightmap_surfaces[fa->lightmaptexturenum] = fa;
	}
}

/*
================
R_RenderBrushPoly
================
*/
static void R_RenderBrushPoly( msurface_t *fa, int cull_type )
{
	r_stats.c_world_polys++;

	if( fa->flags & SURF_DRAWSKY )
		return; // already handled

	texture_t *t = R_TextureAnimation( fa );

	GL_Bind( XASH_TEXTURE0, t->gl_texturenum );

	if( FBitSet( fa->flags, SURF_DRAWTURB ))
	{
		// warp texture, no lightmaps
		EmitWaterPolys( fa, cull_type == CULL_BACKSIDE );
		return;
	}

	R_RenderFullbrightForSurface( fa, t );
	R_RenderDetailsForSurface( fa, t );
	DrawGLPoly( GU_POLYS( fa ), 0.0f, 0.0f );
	R_RenderDecalsForSurface( fa, cull_type );
	R_RenderLightmapForSurface( fa );
}

/*
================
R_DrawTextureChains
================
*/
static void R_DrawTextureChains( void )
{
	// make sure what color is reset
	sceGuColor( 0xffffffff );
	R_LoadIdentity();	// set identity matrix

	GL_SetupFogColorForSurfaces();

	// restore worldmodel
	RI.currententity = CL_GetEntityByIndex( 0 );
	RI.currentmodel = RI.currententity->model;

	if( FBitSet( tr.world->flags, FWORLD_SKYSPHERE ) && !FBitSet( tr.world->flags, FWORLD_CUSTOM_SKYBOX ))
	{
		sceGuDisable( GU_TEXTURE_2D );
		sceGuColor( 0xffffffff );
	}

	// clip skybox surfaces
	for( msurface_t *s = skychain; s != NULL; s = s->texturechain )
		R_AddSkyBoxSurface( s );

	if( FBitSet( tr.world->flags, FWORLD_SKYSPHERE ) && !FBitSet( tr.world->flags, FWORLD_CUSTOM_SKYBOX ))
	{
		sceGuEnable( GU_TEXTURE_2D );
		if( skychain )
			R_DrawClouds();
		skychain = NULL;
	}

	for( int i = 0; i < WORLDMODEL->numtextures; i++ )
	{
		texture_t *t = WORLDMODEL->textures[i];
		if( !t ) continue;

		msurface_t *s = t->texturechain;

		if( !s || ( i == tr.skytexturenum ))
			continue;

		if(( s->flags & SURF_DRAWTURB ) && gp_movevars->wateralpha < 1.0f )
		{
			R_AddToSeparatePass( &draw_wateralpha, i );
			continue;	// draw translucent water later
		}

		if( FBitSet( gp_host->features, ENGINE_QUAKE_COMPATIBLE ) && FBitSet( s->flags, SURF_TRANSPARENT ))
		{
			R_AddToSeparatePass( &draw_alpha_surfaces, i );
			continue;	// draw transparent surfaces later
		}

		for( ; s != NULL; s = s->texturechain )
			R_RenderBrushPoly( s, CULL_VISIBLE );
		t->texturechain = NULL;
	}
}

/*
================
R_DrawAlphaTextureChains
================
*/
void R_DrawAlphaTextureChains( void )
{
	if( !R_SeparatePassActive( &draw_alpha_surfaces ))
		return;

	memset( gl_lms.lightmap_surfaces, 0, sizeof( gl_lms.lightmap_surfaces ));
	gl_lms.dynamic_surfaces = NULL;

	// make sure what color is reset
	sceGuColor( 0xffffffff );
	R_LoadIdentity(); // set identity matrix

	sceGuDisable( GU_BLEND );
	sceGuEnable( GU_ALPHA_TEST );
	sceGuAlphaFunc( GU_GREATER, 0x40, 0xff );

	// restore worldmodel
	RI.currententity = CL_GetEntityByIndex( 0 );
	RI.currentmodel = RI.currententity->model;
	RI.currententity->curstate.rendermode = kRenderTransAlpha;

	GL_SetupFogColorForSurfaces();

	for( int i = draw_alpha_surfaces.first; i <= draw_alpha_surfaces.last; i++ )
	{
		texture_t *t = WORLDMODEL->textures[i];
		if( !t )
			continue;

		msurface_t *s = t->texturechain;

		if( !s || !FBitSet( s->flags, SURF_TRANSPARENT ))
			continue;

		for( ; s != NULL; s = s->texturechain )
			R_RenderBrushPoly( s, CULL_VISIBLE );
		t->texturechain = NULL;
	}

	R_ResetSeparatePass( &draw_alpha_surfaces );

	GL_ResetFogColor();
	R_BlendLightmaps();
	RI.currententity->curstate.rendermode = kRenderNormal; // restore world rendermode
	sceGuAlphaFunc( GU_GREATER, DEFAULT_ALPHATEST, 0xff );
}

/*
================
R_DrawWaterSurfaces
================
*/
void R_DrawWaterSurfaces( void )
{
	if( !FBitSet( RI.rvp.flags, RF_DRAW_WORLD ) || FBitSet( RI.rvp.flags, RF_ONLY_CLIENTDRAW ))
		return;

	// non-transparent water is already drawed
	if( !R_SeparatePassActive( &draw_wateralpha ))
		return;

	// restore worldmodel
	RI.currententity = CL_GetEntityByIndex( 0 );
	RI.currentmodel = RI.currententity->model;

	// go back to the world matrix
	R_LoadIdentity();

	sceGuEnable( GU_BLEND );
	sceGuDepthMask( GU_TRUE ); // no depth writes
	sceGuDisable( GU_ALPHA_TEST );
	sceGuBlendFunc( GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0 );
	sceGuTexFunc( GU_TFX_MODULATE, GU_TCC_RGBA );
	sceGuColor( GUCOLOR4F( 1.0f, 1.0f, 1.0f, gp_movevars->wateralpha ));

	for( int i = draw_wateralpha.first; i <= draw_wateralpha.last; i++ )
	{
		texture_t *t = WORLDMODEL->textures[i];
		if( !t ) continue;

		msurface_t *s = t->texturechain;
		if( !s ) continue;

		if( !FBitSet( s->flags, SURF_DRAWTURB ))
			continue;

		GL_Bind( XASH_TEXTURE0, t->gl_texturenum );

		for( ; s; s = s->texturechain )
			EmitWaterPolys( s, false );

		t->texturechain = NULL;
	}

	GL_ResetFogColor();

	R_ResetSeparatePass( &draw_wateralpha );

	sceGuDisable( GU_BLEND );
	sceGuDepthMask( GU_FALSE );
	sceGuDisable( GU_ALPHA_TEST );
	sceGuTexFunc( GU_TFX_REPLACE, GU_TCC_RGBA );
	sceGuColor( 0xffffffff );
}

/*
=================
R_SurfaceCompare

compare translucent surfaces
=================
*/
static int R_SurfaceCompare( const void *a, const void *b )
{
	vec3_t		org1, org2;

	msurface_t *surf1 = (msurface_t *)((sortedface_t *)a)->surf;
	msurface_t *surf2 = (msurface_t *)((sortedface_t *)b)->surf;

	VectorAdd( RI.currententity->origin, surf1->info->origin, org1 );
	VectorAdd( RI.currententity->origin, surf2->info->origin, org2 );

	// compare by plane dists
	float len1 = DotProduct( org1, RI.vforward ) - RI.viewplanedist;
	float len2 = DotProduct( org2, RI.vforward ) - RI.viewplanedist;

	if( len1 > len2 )
		return -1;
	if( len1 < len2 )
		return 1;

	return 0;
}

static void R_SetRenderMode( cl_entity_t *e )
{
	// NOTE: sceGuDepthMask( GU_TRUE ) disables depth writes
	switch( e->curstate.rendermode )
	{
	case kRenderNormal:
		sceGuColor( 0xffffffff );
		break;
	case kRenderTransColor:
		sceGuBlendFunc( GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0 );
		sceGuColor( GUCOLOR4UB( e->curstate.rendercolor.r, e->curstate.rendercolor.g, e->curstate.rendercolor.b, e->curstate.renderamt ));
		sceGuTexFunc( GU_TFX_MODULATE, GU_TCC_RGBA );
		sceGuDisable( GU_TEXTURE_2D );
		sceGuEnable( GU_BLEND );
		break;
	case kRenderTransAdd:
		sceGuTexFunc( GU_TFX_MODULATE, GU_TCC_RGBA );
		sceGuColor( GUCOLOR4F( tr.blend, tr.blend, tr.blend, 1.0f ));
		sceGuBlendFunc( GU_ADD, GU_FIX, GU_FIX, GUBLEND1, GUBLEND1 );
		sceGuDepthMask( GU_TRUE );
		sceGuEnable( GU_BLEND );
		break;
	case kRenderTransAlpha:
		sceGuEnable( GU_ALPHA_TEST );
		sceGuTexFunc( GU_TFX_MODULATE, GU_TCC_RGBA );
		if( FBitSet( gp_host->features, ENGINE_QUAKE_COMPATIBLE ))
		{
			sceGuBlendFunc( GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0 );
			sceGuColor( GUCOLOR4F( 1.0f, 1.0f, 1.0f, tr.blend ));
			sceGuEnable( GU_BLEND );
		}
		else
		{
			sceGuColor( 0xffffffff );
			sceGuDisable( GU_BLEND );
		}
		sceGuAlphaFunc( GU_GREATER, 0x40, 0xff );
		break;
	default:
		sceGuTexFunc( GU_TFX_MODULATE, GU_TCC_RGBA );
		sceGuBlendFunc( GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0 );
		sceGuColor( GUCOLOR4F( 1.0f, 1.0f, 1.0f, tr.blend ));
		sceGuDepthMask( GU_TRUE );
		sceGuEnable( GU_BLEND );
		break;
	}
}

static int R_SortBrushModelSurfaces( cl_entity_t *e, model_t *clmodel, vec3_t mins )
{
	qboolean quake_compatible = FBitSet( gp_host->features, ENGINE_QUAKE_COMPATIBLE ) ? true : false;
	int num_sorted = 0;

	for( int i = 0; i < clmodel->nummodelsurfaces; i++ )
	{
		msurface_t *psurf = &clmodel->surfaces[clmodel->firstmodelsurface + i];

		if( FBitSet( psurf->flags, SURF_DRAWTURB ) && !quake_compatible )
		{
			if( psurf->plane->type != PLANE_Z && !FBitSet( e->curstate.effects, EF_WATERSIDES ))
				continue;

			if( mins[2] + 1.0f >= psurf->plane->dist )
				continue;
		}

		int cull_type = R_CullSurface( psurf, &RI.frustum, RI.frustum.clipFlags );

		if( cull_type >= CULL_FRUSTUM )
			continue;

		if( cull_type == CULL_BACKSIDE )
		{
			if( !FBitSet( psurf->flags, SURF_DRAWTURB ))
			{
				if( !( psurf->pdecals && e->curstate.rendermode == kRenderTransTexture ))
					continue;
			}
		}

		if( num_sorted < gpGlobals->max_surfaces )
		{
			gpGlobals->draw_surfaces[num_sorted].surf = psurf;
			gpGlobals->draw_surfaces[num_sorted].cull = cull_type;
			num_sorted++;
		}
	}

	// sort faces if needs
	if( !FBitSet( clmodel->flags, MODEL_LIQUID ) && e->curstate.rendermode == kRenderTransTexture && !gl_nosort.value )
		qsort( gpGlobals->draw_surfaces, num_sorted, sizeof( sortedface_t ), R_SurfaceCompare );

	return num_sorted;
}

/*
=================
R_DrawBrushModel
=================
*/
void R_DrawBrushModel( cl_entity_t *e )
{
	vec3_t mins, maxs;
	qboolean rotated;

	if( !FBitSet( RI.rvp.flags, RF_DRAW_WORLD ))
		return;

	model_t *clmodel = e->model;

	if( !VectorIsNull( e->angles ))
	{
		for( int i = 0; i < 3; i++ )
		{
			mins[i] = e->origin[i] - clmodel->radius;
			maxs[i] = e->origin[i] + clmodel->radius;
		}
		rotated = true;
	}
	else
	{
		VectorAdd( e->origin, clmodel->mins, mins );
		VectorAdd( e->origin, clmodel->maxs, maxs );
		rotated = false;
	}

	if( R_CullBox( mins, maxs ))
		return;

	memset( gl_lms.lightmap_surfaces, 0, sizeof( gl_lms.lightmap_surfaces ));
	int old_rendermode = e->curstate.rendermode;
	gl_lms.dynamic_surfaces = NULL;

	if( rotated )
	{
		R_RotateForEntity( e );

		Matrix4x4_VectorITransform( RI.objectMatrix, RI.cullorigin, tr.modelorg );
	}
	else
	{
		R_TranslateForEntity( e );

		VectorSubtract( RI.cullorigin, e->origin, tr.modelorg );
	}

	// the guard band clipper works in model space
	GU_ClipSetModelFrustum( RI.objectMatrix );

	if( FBitSet( gp_host->features, ENGINE_QUAKE_COMPATIBLE ) && FBitSet( clmodel->flags, MODEL_TRANSPARENT ))
		e->curstate.rendermode = kRenderTransAlpha;

	e->visframe = tr.realframecount; // visible

	// calculate dynamic lighting for bmodel
	R_PushDlightsForBmodel( clmodel, tr.dlightframecount, RI.objectMatrix );

	// setup the rendermode
	R_SetRenderMode( e );
	if( e->curstate.rendermode == kRenderTransAdd )
		R_AllowFog( false );

	GL_SetupFogColorForSurfaces ();

	int num_sorted = R_SortBrushModelSurfaces( e, clmodel, mins );

	// draw bmodels with a polyoffset to avoid flickering when they are too close to world
	if( gl_polyoffset_bmodels.value )
		GL_PushPolygonOffset( -0.5f, -gl_polyoffset_bmodels.value );

	// draw sorted translucent surfaces
	for( int i = 0; i < num_sorted; i++ )
		R_RenderBrushPoly( gpGlobals->draw_surfaces[i].surf, gpGlobals->draw_surfaces[i].cull );

	if( e->curstate.rendermode == kRenderTransColor )
		sceGuEnable( GU_TEXTURE_2D );

	DrawDecalsBatch();

	GL_ResetFogColor();
	R_BlendLightmaps();
	R_RenderFullbrights();
	R_RenderDetails();

	if( gl_polyoffset_bmodels.value )
		GL_PopPolygonOffset();

	// restore fog here
	if( e->curstate.rendermode == kRenderTransAdd )
		R_AllowFog( true );

	e->curstate.rendermode = old_rendermode;
	sceGuDisable( GU_ALPHA_TEST );
	sceGuAlphaFunc( GU_GREATER, DEFAULT_ALPHATEST, 0xff );
	sceGuDisable( GU_BLEND );
	sceGuDepthMask( GU_FALSE );

	if( r_showhull->value > 0.0f )
	{
		GL_PushPolygonOffset( 1.0f, 2.0f );
		gEngfuncs.R_DrawModelHull( clmodel );	// draw before restore
		GL_PopPolygonOffset();
	}

	R_LoadIdentity();	// restore worldmatrix
	GU_ClipRestoreWorldFrustum();
}

/*
=============================================================

	WORLD MODEL

=============================================================
*/
/*
================
R_RecursiveWorldNode
================
*/
static void R_RecursiveWorldNode( mnode_t *node, uint clipflags )
{
loc0:
	if( node->contents == CONTENTS_SOLID )
		return; // hit a solid leaf

	if( node->visframe != tr.visframecount )
		return;

	if( clipflags && !r_nocull.value )
	{
		for( int i = 0; i < 6; i++ )
		{
			const mplane_t	*p = &RI.frustum.planes[i];

			if( !FBitSet( clipflags, BIT( i )))
				continue;

			int clipped = BOX_ON_PLANE_SIDE( node->minmaxs, node->minmaxs + 3, p );
			if( clipped == 2 ) return;
			if( clipped == 1 ) ClearBits( clipflags, BIT( i ));
		}
	}

	// if a leaf node, draw stuff
	if( node->contents < 0 )
	{
		mleaf_t *pleaf = (mleaf_t *)node;
		msurface_t **mark = pleaf->firstmarksurface;

		for( int i = 0; i < pleaf->nummarksurfaces; i++ )
			mark[i]->visframe = tr.framecount;

		// deal with model fragments in this leaf
		if( pleaf->efrags )
			gEngfuncs.R_StoreEfrags( &pleaf->efrags, tr.realframecount );

		r_stats.c_world_leafs++;
		return;
	}

	// node is just a decision point, so go down the apropriate sides

	// find which side of the node we are on
	float dot = PlaneDiff( tr.modelorg, node->plane );
	int side = (dot >= 0.0f) ? 0 : 1;

	// recurse down the children, front side first
	R_RecursiveWorldNode( node_child( node, side, WORLDMODEL ), clipflags );

	int firstsurface = node_firstsurface( node, WORLDMODEL );
	int numsurfaces = node_numsurfaces( node, WORLDMODEL );

	// draw stuff
	for( int i = firstsurface; i < firstsurface + numsurfaces; i++ )
	{
		msurface_t *surf = &WORLDMODEL->surfaces[i];

		if( R_CullSurface( surf, &RI.frustum, clipflags ))
			continue;

		if( surf->flags & SURF_DRAWSKY )
		{
			// make sky chain to right clip the skybox
			surf->texturechain = skychain;
			skychain = surf;
		}
		else
		{
			surf->texturechain = surf->texinfo->texture->texturechain;
			surf->texinfo->texture->texturechain = surf;
		}
	}

	// recurse down the back side
	node = node_child( node, !side, WORLDMODEL );
	goto loc0;
}

/*
================
R_CullNodeTopView

cull node by user rectangle (simple scissor)
================
*/
static qboolean R_CullNodeTopView( mnode_t *node )
{
	vec2_t	delta, size;
	vec3_t	center, half;

	// build the node center and half-diagonal
	VectorAverage( node->minmaxs, node->minmaxs + 3, center );
	VectorSubtract( node->minmaxs + 3, center, half );

	// cull against the screen frustum or the appropriate area's frustum.
	Vector2Subtract( center, world_orthocenter, delta );
	Vector2Add( half, world_orthohalf, size );

	return ( fabs( delta[0] ) > size[0] ) || ( fabs( delta[1] ) > size[1] );
}

/*
================
R_DrawTopViewLeaf
================
*/
static void R_DrawTopViewLeaf( mleaf_t *pleaf, uint clipflags )
{
	msurface_t	**mark;
	int		i;

	for( i = 0, mark = pleaf->firstmarksurface; i < pleaf->nummarksurfaces; i++, mark++ )
	{
		msurface_t *surf = *mark;

		// don't process the same surface twice
		if( surf->visframe == tr.framecount )
			continue;

		surf->visframe = tr.framecount;

		if( R_CullSurface( surf, &RI.frustum, clipflags ))
			continue;

		if(!( surf->flags & SURF_DRAWSKY ))
		{
			surf->texturechain = surf->texinfo->texture->texturechain;
			surf->texinfo->texture->texturechain = surf;
		}
	}

	// deal with model fragments in this leaf
	if( pleaf->efrags )
		gEngfuncs.R_StoreEfrags( &pleaf->efrags, tr.realframecount );

	r_stats.c_world_leafs++;
}

/*
================
R_DrawWorldTopView
================
*/
static void R_DrawWorldTopView( mnode_t *node, uint clipflags )
{
	msurface_t	*surf;
	int		c;

	do
	{
		if( node->contents == CONTENTS_SOLID )
			return;	// hit a solid leaf

		if( node->visframe != tr.visframecount )
			return;

		if( clipflags && !r_nocull.value )
		{
			for( int i = 0; i < 6; i++ )
			{
				const mplane_t	*p = &RI.frustum.planes[i];

				if( !FBitSet( clipflags, BIT( i )))
					continue;

				int clipped = BOX_ON_PLANE_SIDE( node->minmaxs, node->minmaxs + 3, p );
				if( clipped == 2 ) return;
				if( clipped == 1 ) ClearBits( clipflags, BIT( i ));
			}
		}

		// cull against the screen frustum or the appropriate area's frustum.
		if( R_CullNodeTopView( node ))
			return;

		// if a leaf node, draw stuff
		if( node->contents < 0 )
		{
			R_DrawTopViewLeaf( (mleaf_t *)node, clipflags );
			return;
		}

		// draw stuff
		int numsurfaces = node_numsurfaces( node, WORLDMODEL );
		int firstsurface = node_firstsurface( node, WORLDMODEL );

		for( c = numsurfaces, surf = WORLDMODEL->surfaces + firstsurface; c; c--, surf++ )
		{
			// don't process the same surface twice
			if( surf->visframe == tr.framecount )
				continue;

			surf->visframe = tr.framecount;

			if( R_CullSurface( surf, &RI.frustum, clipflags ))
				continue;

			if(!( surf->flags & SURF_DRAWSKY ))
			{
				surf->texturechain = surf->texinfo->texture->texturechain;
				surf->texinfo->texture->texturechain = surf;
			}
		}

		// recurse down both children, we don't care the order...
		R_DrawWorldTopView( node_child( node, 0, WORLDMODEL ), clipflags );
		node = node_child( node, 1, WORLDMODEL );
	} while( node );
}

/*
=============
R_DrawWorld
=============
*/
void R_DrawWorld( void )
{
	// paranoia issues: when gl_renderer is "0" we need have something valid for currententity
	// to prevent crashing until HeadShield drawing.
	RI.currententity = CL_GetEntityByIndex( 0 );
	if( !RI.currententity )
		return;

	RI.currentmodel = RI.currententity->model;
	if( !FBitSet( RI.rvp.flags, RF_DRAW_WORLD ) || FBitSet( RI.rvp.flags, RF_ONLY_CLIENTDRAW ))
		return;

	VectorCopy( RI.cullorigin, tr.modelorg );
	memset( gl_lms.lightmap_surfaces, 0, sizeof( gl_lms.lightmap_surfaces ));
	memset( fullbright_surfaces, 0, sizeof( fullbright_surfaces ));
	memset( detail_surfaces, 0, sizeof( detail_surfaces ));

	gl_lms.dynamic_surfaces = NULL;
	sceGuDisable( GU_ALPHA_TEST );
	sceGuDisable( GU_BLEND );
	tr.blend = 1.0f;

	R_ClearSkyBox ();

	double start = gEngfuncs.pfnTime();
	if( FBitSet( RI.rvp.flags, RF_DRAW_OVERVIEW ))
		R_DrawWorldTopView( WORLDMODEL->nodes, RI.frustum.clipFlags );
	else R_RecursiveWorldNode( WORLDMODEL->nodes, RI.frustum.clipFlags );
	double end = gEngfuncs.pfnTime();

	r_stats.t_world_node = end - start;

	start = gEngfuncs.pfnTime();

	R_DrawTextureChains();

	if( !ENGINE_GET_PARM( PARM_DEV_OVERVIEW ))
	{
		DrawDecalsBatch();
		GL_ResetFogColor();
		R_BlendLightmaps();
		R_RenderFullbrights();
		R_RenderDetails();

		if( skychain )
			R_DrawSkyBox();
	}

	end = gEngfuncs.pfnTime();

	r_stats.t_world_draw = end - start;
	tr.num_draw_decals = 0;
	skychain = NULL;

	gEngfuncs.R_DrawWorldHull();
}

/*
===============
R_MarkLeaves

Mark the leaves and nodes that are in the PVS for the current leaf
===============
*/
void R_MarkLeaves( void )
{
	qboolean	novis = false;
	qboolean	force = false;
	mleaf_t	*leaf = NULL;

	if( !FBitSet( RI.rvp.flags, RF_DRAW_WORLD ))
		return;

	if( FBitSet( r_novis.flags, FCVAR_CHANGED ) || tr.fResetVis )
	{
		// force recalc viewleaf
		ClearBits( r_novis.flags, FCVAR_CHANGED );
		tr.fResetVis = false;
		RI.viewleaf = NULL;
	}

	vec3_t test = Vec3( RI.rvp.vieworigin );

	if( RI.viewleaf != NULL )
	{
		// merge two leafs that can be a crossed-line contents
		if( RI.viewleaf->contents == CONTENTS_EMPTY )
			VectorSet( test, RI.rvp.vieworigin[0], RI.rvp.vieworigin[1], RI.rvp.vieworigin[2] - 16.0f );
		else
			VectorSet( test, RI.rvp.vieworigin[0], RI.rvp.vieworigin[1], RI.rvp.vieworigin[2] + 16.0f );

		leaf = gEngfuncs.Mod_PointInLeaf( test, WORLDMODEL->nodes, WORLDMODEL );

		if(( leaf->contents != CONTENTS_SOLID ) && ( RI.viewleaf != leaf ))
			force = true;
	}

	if( RI.viewleaf == RI.oldviewleaf && RI.viewleaf != NULL && !force )
		return;

	// development aid to let you run around
	// and see exactly where the pvs ends
	if( r_lockpvs.value ) return;

	RI.oldviewleaf = RI.viewleaf;
	tr.visframecount++;

	if( r_novis.value || FBitSet( RI.rvp.flags, RF_DRAW_OVERVIEW ) || !RI.viewleaf || !WORLDMODEL->visdata )
		novis = true;

	gEngfuncs.R_FatPVS( RI.rvp.vieworigin, r_pvs_radius->value, RI.visbytes, false, novis );
	if( force && !novis )
		gEngfuncs.R_FatPVS( test, r_pvs_radius->value, RI.visbytes, true, novis );

	for( int i = 0; i < WORLDMODEL->numleafs; i++ )
	{
		if( CHECKVISBIT( RI.visbytes, i ))
		{
			mnode_t *node = (mnode_t *)&WORLDMODEL->leafs[i+1];
			do
			{
				if( node->visframe == tr.visframecount )
					break;
				node->visframe = tr.visframecount;
				node = node->parent;
			} while( node );
		}
	}
}

/*
========================
GL_CreateSurfaceLightmap
========================
*/
static void GL_CreateSurfaceLightmap( msurface_t *surf, model_t *loadmodel )
{
	mextrasurf_t	*info = surf->info;

	if( !loadmodel->lightdata )
		return;

	if( FBitSet( surf->flags, SURF_DRAWTILED|SURF_DRAWTURB ))
		return;

	int sample_size = gEngfuncs.Mod_SampleSizeForFace( surf );
	int smax = ( info->lightextents[0] / sample_size ) + 1;
	int tmax = ( info->lightextents[1] / sample_size ) + 1;

	if( !LM_AllocBlock( smax, tmax, &surf->light_s, &surf->light_t ))
	{
		LM_UploadBlock( false );
		LM_InitBlock();

		if( !LM_AllocBlock( smax, tmax, &surf->light_s, &surf->light_t ))
			gEngfuncs.Host_Error( "%s: full\n", __func__ );
	}

	surf->lightmaptexturenum = gl_lms.current_lightmap_texture;

	byte *base = gl_lms.lightmap_buffer;
	base += ( surf->light_t * BLOCK_SIZE + surf->light_s ) * LIGHTMAP_BPP;

	R_UpdateSurfaceCachedLight( surf );
	R_BuildLightMap( surf, base, BLOCK_SIZE * LIGHTMAP_BPP, false );
}

/*
==================
GL_RebuildLightmaps

Rebuilds the lightmap texture
when gamma is changed
==================
*/
void GL_RebuildLightmaps( void )
{
	if( !ENGINE_GET_PARM( PARM_CLIENT_ACTIVE ) )
		return; // wait for worldmodel

	// release old lightmaps
	for( int i = 0; i < MAX_LIGHTMAPS; i++ )
	{
		if( !tr.lightmapTextures[i] ) break;
		GL_FreeTexture( tr.lightmapTextures[i] );
	}

	memset( tr.lightmapTextures, 0, sizeof( tr.lightmapTextures ));
	gl_lms.current_lightmap_texture = 0;

	// setup all the lightstyles
	CL_RunLightStyles((lightstyle_t *)ENGINE_GET_PARM( PARM_GET_LIGHTSTYLES_PTR ));

	LM_InitBlock();

	for( int i = 0; i < gp_cl->nummodels; i++ )
	{
		model_t *m = CL_ModelHandle( i + 1 );
		if( m == NULL )
			continue;

		if( m->name[0] == '*' || m->type != mod_brush )
			continue;

		for( int j = 0; j < m->numsurfaces; j++ )
			GL_CreateSurfaceLightmap( m->surfaces + j, m );
	}
	LM_UploadBlock( false );

	if( gEngfuncs.drawFuncs->GL_BuildLightmaps )
	{
		// build lightmaps on the client-side
		gEngfuncs.drawFuncs->GL_BuildLightmaps( );
	}
}

/*
==================
GL_BuildLightmaps

Builds the lightmap texture
with all the surfaces from all brush models
==================
*/
void GL_BuildLightmaps( void )
{
	int	nColinElim = 0;

	// release old lightmaps
	for( int i = 0; i < MAX_LIGHTMAPS; i++ )
	{
		if( !tr.lightmapTextures[i] ) break;
		GL_FreeTexture( tr.lightmapTextures[i] );
	}

	memset( tr.lightmapTextures, 0, sizeof( tr.lightmapTextures ));
	memset( &RI, 0, sizeof( RI ));

	// GU lightmaps are always 128x128 (BLOCK_SIZE_MAX)
	tr.block_size = BLOCK_SIZE_DEFAULT;

	skychain = NULL;

	tr.framecount = tr.visframecount = 1;	// no dlight cache
	gl_lms.current_lightmap_texture = 0;
	tr.modelviewIdentity = false;
	tr.realframecount = 1;

	// setup the texture for dlights
	R_InitDlightTexture();

	// setup all the lightstyles
	CL_RunLightStyles((lightstyle_t *)ENGINE_GET_PARM( PARM_GET_LIGHTSTYLES_PTR ));

	LM_InitBlock();

	for( int i = 0; i < gp_cl->nummodels; i++ )
	{
		model_t *m = CL_ModelHandle( i + 1 );
		if( m == NULL )
			continue;

		if( m->name[0] == '*' || m->type != mod_brush )
			continue;

		for( int j = 0; j < m->numsurfaces; j++ )
		{
			// clearing all decal chains
			m->surfaces[j].pdecals = NULL;
			m->surfaces[j].visframe = 0;

			GL_CreateSurfaceLightmap( m->surfaces + j, m );

			if( m->surfaces[j].flags & SURF_DRAWTURB )
				continue;

			nColinElim += GL_BuildPolygonFromSurface( m, m->surfaces + j );
		}

		// clearing visframe
		for( int j = 0; j < m->numleafs; j++ )
			m->leafs[j+1].visframe = 0;
		for( int j = 0; j < m->numnodes; j++ )
			m->nodes[j].visframe = 0;
	}

	LM_UploadBlock( false );

	if( gEngfuncs.drawFuncs->GL_BuildLightmaps )
	{
		// build lightmaps on the client-side
		gEngfuncs.drawFuncs->GL_BuildLightmaps( );
	}
}
