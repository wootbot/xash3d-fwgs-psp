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
#if XASH_PSP
#include <malloc.h>
#endif

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
#if XASH_PSP
static const model_t	*lm_prebuilt; // see R_PrebuildLightmaps
#endif

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

#if XASH_PSP
/*
=============================================================

POLYGONS ON DEMAND

A polygon takes 40 bytes a vertex: 6 MB in a map of 30000 surfaces, but a place
in it only sees 1000 - 5000 of them. The world's polygons of such a map are built
when they get into the PVS (R_PolysForPVS), in chunks that are all dropped when
there are too many of them.

=============================================================
*/
#define LP_CHUNK		( 256 * 1024 )
#define LP_MAXCHUNKS	14		// what doesn't fit in them isn't drawn (out of the map everything is visible)
#define LP_KEEP		6		// dropped at the next change of the PVS with more than this
#define LP_MINSIZE		( 2 * 1024 * 1024 )	// maps with less keep all of them

static struct
{
	const model_t	*world;		// NULL: all polygons are there
	byte		*chunks[LP_MAXCHUNKS];
	int		numchunks;		// allocated
	int		chunk, used;	// the one being filled
	int		numsurfs;		// of the world itself
} lp;

static qboolean R_PolyIsLazy( const model_t *mod, const msurface_t *surf )
{
	return lp.world == mod && mod && surf - mod->surfaces < lp.numsurfs && !FBitSet( surf->flags, SURF_DRAWTURB );
}

static void *R_PolyAlloc( size_t size )
{
	if( size > LP_CHUNK )
		return NULL;

	if( lp.numchunks && lp.used + size <= LP_CHUNK )
	{
		lp.used += size;
		return lp.chunks[lp.chunk] + lp.used - size;
	}

	if( lp.numchunks && lp.chunk + 1 < lp.numchunks )
		lp.chunk++;
	else
	{
		byte *chunk = lp.numchunks < LP_MAXCHUNKS ? memalign( 64, LP_CHUNK ) : NULL;

		if( !chunk )
			return NULL;

		lp.chunk = lp.numchunks;
		lp.chunks[lp.numchunks++] = chunk;
	}

	lp.used = size;
	return lp.chunks[lp.chunk];
}

/*
=================
R_PolysBegin

before the polygons of a new map are built
=================
*/
static void R_PolysFree( void )
{
	for( int i = 0; i < lp.numchunks; i++ )
		free( lp.chunks[i] );
	memset( &lp, 0, sizeof( lp ));
}

static void R_PolysBegin( const model_t *world )
{
	size_t size = 0;

	R_PolysFree();

	if( !world || world->type != mod_brush )
		return;

	const int numsurfs = world->firstmodelsurface + world->nummodelsurfaces;

	for( int i = 0; i < numsurfs; i++ )
		size += GU_POLY_SIZE( world->surfaces[i].numedges );

	if( size < LP_MINSIZE )
		return;

	lp.world = world;
	lp.numsurfs = numsurfs;
	gEngfuncs.Con_Reportf( "%s: %d polygons on demand, %d KB\n", __func__, numsurfs, (int)( size >> 10 ));
}

/*
================
GL_BuildPolygonFromSurface
================
*/
#endif
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
#if XASH_PSP
	if( R_PolyIsLazy( mod, fa ))
	{
		if( !poly && !( poly = R_PolyAlloc( GU_POLY_SIZE( lnumverts ))))
			return nColinElim;
	}
	else
#endif
	poly = Mem_Realloc( mod->mempool, poly, GU_POLY_SIZE( lnumverts ));
	poly->next = GU_POLYS( fa );
	poly->chain = NULL; // R_PolyAlloc doesn't clear it
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

	// the texture repeats: whole repeats away from the polygon's middle, the GE's 24 bit
	// floats get less accurate with the size of the coordinates. Detail textures don't
	// repeat with it, they get seams if their scale isn't an integer.
	for( int j = 0; j < 2; j++ )
	{
		float lo = poly->verts[0].uv[j], hi = lo;

		for( int i = 1; i < lnumverts; i++ )
		{
			lo = Q_min( lo, poly->verts[i].uv[j] );
			hi = Q_max( hi, poly->verts[i].uv[j] );
		}

		const float shift = floorf(( lo + hi ) * 0.5f );

		for( int i = 0; i < lnumverts; i++ )
			poly->verts[i].uv[j] -= shift;
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

static qboolean LM_AllocDynamic( int w, int h, mextrasurf_t *info )
{
	int s, t;

	if( !LM_AllocBlock( w, h, &s, &t ))
		return false;

	info->dlight_s = s;
	info->dlight_t = t;
	return true;
}

static void LM_UploadBlock( qboolean dynamic )
{
	if( dynamic )
	{
		// NOTE: the GE reads the texture when it executes the list, after this returns
		tr.dlightTexture = tr.dlightTextures[guRender.vert_frame & 1];
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

// the GE list has a fixed size, past its end the GE runs whatever is there: what
// doesn't fit isn't drawn. The rest is for what is drawn after the world
static qboolean GU_WorldListFull( void )
{
	static uint last;

	if( extGuListUsed() <= guRender.context_list_size - 192 * 1024 )
		return false;

	if( guRender.vert_frame - last > 300 || !last )
	{
		last = guRender.vert_frame;
		gEngfuncs.Con_Reportf( "%s: %u KB used\n", __func__, extGuListUsed() >> 10 );
	}

	return true;
}

/*
================
GU_DrawPolyVerts

draws a triangle fan straight from memory, clipping it if it crosses the guard band
================
*/
static void GU_DrawPolyVerts( gu_vert_t *verts, int numverts )
{
	if( GU_WorldListFull( ))
		return;

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
GU_DrawPoly

draws one of a surface's vertex sets, without the clip test when
R_RenderBrushPoly found the surface's box inside the clip planes
================
*/
static void GU_DrawPoly( gu_poly_t *p, gu_vert_t *verts )
{
	if( FBitSet( p->flags, GU_POLY_NOCLIP ) && !GU_WorldListFull( ))
		extGuDrawArrayBatched( GU_TRIANGLE_FAN, GU_TEXTURE_32BITF | GU_VERTEX_32BITF, p->numverts, verts );
	else GU_DrawPolyVerts( verts, p->numverts );
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
// the texture offset of a conveyor surface of RI.currententity with the bound texture
static void R_ConveyorOffset( float *s, float *t )
{
	const cl_entity_t *e = RI.currententity;
	float sOffset, tOffset;
#if XASH_PSP
	// the same for all surfaces of an entity and texture in a frame: double math and a
	// sine, ~10 us a surface (deluge_beta_v3: 94 of them in view)
	static struct { const cl_entity_t *e; int texture; double time; float s, t; } conveyor;

	if( conveyor.e == e && e && conveyor.texture == glState.currentTexture && conveyor.time == gp_cl->time )
	{
		*s = conveyor.s;
		*t = conveyor.t;
		return;
	}
#endif
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
	*s = sOffset - (int)sOffset;
	*t = tOffset - (int)tOffset;
#if XASH_PSP
	conveyor.e = e;
	conveyor.texture = glState.currentTexture;
	conveyor.time = gp_cl->time;
	conveyor.s = *s;
	conveyor.t = *t;
#endif
}

static void DrawGLPoly( gu_poly_t *p, float xScale, float yScale )
{
	float sOffset, tOffset;

	if( !p )
		return;

	if( FBitSet( p->flags, SURF_DRAWTILED ))
		GL_ResetFogColor();

	if( FBitSet( p->flags, SURF_CONVEYOR ))
		R_ConveyorOffset( &sOffset, &tOffset );
	else sOffset = tOffset = 0.0f;

	const qboolean hasScale = xScale != 0.0f && yScale != 0.0f;
	const qboolean hasOffset = sOffset != 0.0f || tOffset != 0.0f;

	if( !hasScale )
		xScale = yScale = 1.0f;

	// uv' = ( uv + offset ) * scale
	if( hasScale ) sceGuTexScale( xScale, yScale );
	if( hasOffset ) sceGuTexOffset( sOffset * xScale, tOffset * yScale );

	GU_DrawPoly( p, p->verts );

	if( hasScale ) sceGuTexScale( 1.0f, 1.0f );
	if( hasOffset ) sceGuTexOffset( 0.0f, 0.0f );

	if( FBitSet( p->flags, SURF_DRAWTILED ))
		GL_SetupFogColorForSurfaces();
}

#if XASH_PSP
/*
=============================================================

COMPACT WORLD SURFACES

The world walk and the two passes of a plain surface (one polygon, static lightmap
with the normal style, no decals, extra passes or animation) read a 32 byte copy of
it instead of msurface_t, its mextrasurf_t, texinfo, texture and polygon: those are
in different places and cost a cache miss each on the PSP, ~4 us a surface for
~350 surfaces a frame in a firefight. The other surfaces still go through
R_RenderBrushPoly. Leaves mark the copies, and the visible ones are chained per
texture and per lightmap page through them.

=============================================================
*/
#define WS_TEXTURE	0x07ff		// WORLDMODEL->textures index
#define WS_STALE	BIT( 11 )		// the lightmap has an old light style 0
#define WS_SLOW		BIT( 12 )		// drawn by R_RenderBrushPoly
#define WS_NOCLIP	BIT( 13 )		// this frame: the box is inside the clip planes
#define WS_DLIT		BIT( 14 )		// dlights reach it this frame
#define WS_DECALS	BIT( 15 )		// has decals
#define WS_SKY		( WS_SLOW|( WS_TEXTURE - 1 ))	// a sky surface
#define WS_NONE		0xffff		// end of a chain

typedef struct
{
	short		mins[3], maxs[3];	// rounded outwards
	uint16_t	pad0;
	uint16_t	texflags;
	gu_vert_t	*verts;		// the texture pass vertices, then the lightmap ones
	byte		numverts;
	byte		lightmap;
	uint16_t	texnext;		// the next visible one with this texture
	uint16_t	lmnext;		// the next visible one on this lightmap page
	uint16_t	pad[3];
} gu_wsurf_t;

static void R_MeWorldFree( void );
static qboolean R_HasLightmap( void );
static void R_SetRenderMode( cl_entity_t *e );
static void R_RenderBrushPoly( msurface_t *fa, int cull_type );
static void R_BlendLightmaps( void );
static void R_RenderFullbrights( void );
static void R_RenderDetails( void );

typedef char ws_size_check[sizeof( gu_wsurf_t ) == 32 ? 1 : -1];

static void		*ws_alloc;
static gu_wsurf_t	*ws_surfs;		// WORLDMODEL->numsurfaces of them, NULL without
static int		ws_numsurfs;

// the low bits of tr.framecount when a leaf of the surface was visited: one
// cache line for 32 surfaces, not 2 (the leaves mark ~700 a frame, the nodes
// test ~850, ~400 pass)
static uint16_t		*ws_visframe;
static int		ws_visfill;		// tr.framecount >> 15 of the last fill
static int		ws_style0;		// light style 0 of the lightmaps that aren't WS_STALE
static qboolean		ws_allslow;		// this frame: draw all through R_RenderBrushPoly
static qboolean		ws_skybox;		// this frame: the sky is a box, not sky polygons
static qboolean		ws_skyseen;		// this frame: a sky surface is visible
static uint16_t		ws_texhead[WS_TEXTURE + 1];
static uint16_t		ws_textures[WS_TEXTURE + 1];	// the textures with a chain
static int		ws_numtextures;
static uint16_t		ws_lmhead[MAX_LIGHTMAPS];

// the textures R_WorldSurfSlow chained surfaces to, for R_DrawTextureChains
#define WS_MAX_SLOWTEXTURES	32
static texture_t		*ws_slowtextures[WS_MAX_SLOWTEXTURES];
static int		ws_numslowtextures = -1;	// -1: look at all of them

#define WS_MAX_DLIT	1024
static uint16_t		ws_dlit[WS_MAX_DLIT];
static int		ws_numdlit;
static qboolean		ws_dlit_overflow;

/*
=================
R_WorldTextureIsSlow

R_TextureAnimation picks a frame by time for the world (it has frame 0, no
alternate frames), the same for all of its surfaces, or by the position of
the surface for random tiling ones, which doesn't change. Extra passes are
per frame.
=================
*/
static qboolean R_WorldTextureIsSlow( const texture_t *base )
{
	const texture_t *t = base;
	for( int i = 0; t && i <= MOD_FRAMES; i++ )
	{
		if( t->fb_texturenum || t->dt_texturenum )
			return true;

		t = t->anim_next;
		if( t == base )
			break;
	}

	return false;
}

/*
=================
R_WorldTextureFrame

R_TextureAnimation for a world texture that R_WorldTextureIsSlow passed,
random tiling ones are picked in R_WorldSurfsBuild
=================
*/
static const texture_t *R_WorldTextureFrame( const texture_t *base )
{
	if( !base->anim_total || base->name[0] == '-' )
		return base;

	// Quake1 textures uses 10 frames per second
	const int speed = FBitSet( R_GetTexture( base->gl_texturenum )->flags, TF_QUAKEPAL ) ? 10 : 20;
	const int reletive = (int)( gp_cl->time * speed ) % base->anim_total;
	const texture_t *t = base;
	int count = 0;

	while( t->anim_min > reletive || t->anim_max <= reletive )
	{
		t = t->anim_next;

		if( !t || ++count > MOD_FRAMES )
			return base;
	}

	return t;
}

static qboolean R_WorldSurfIsSlow( const model_t *world, const msurface_t *surf )
{
	const texture_t *t = surf->texinfo ? surf->texinfo->texture : NULL;
	const gu_poly_t *p = GU_POLYS( surf );

	if( R_PolyIsLazy( world, surf ))
	{
		// not more vertices than that when it's built
		if( !t || surf->numedges < 3 || surf->numedges > 255 )
			return true;
	}
	else if( !t || !p || p->next || p->numverts < 3 || p->numverts > 255 )
		return true;

	if( FBitSet( surf->flags, SURF_DRAWSKY|SURF_DRAWTURB|SURF_DRAWTILED|SURF_CONVEYOR ))
		return true;

	// R_DrawTextureChains draws these later
	if( FBitSet( surf->flags, SURF_TRANSPARENT ) && FBitSet( gp_host->features, ENGINE_QUAKE_COMPATIBLE ))
		return true;

	if( R_WorldTextureIsSlow( t ))
		return true;

	// R_CheckLightMap: styles other than the normal one change
	if( !world->lightdata || surf->lightmaptexturenum < 0 || surf->lightmaptexturenum >= MAX_LIGHTMAPS
		|| ( world != lm_prebuilt && ( surf->styles[0] != 0 || ( MAXLIGHTMAPS > 1 && surf->styles[1] != 255 ))))
		return true;

	return false;
}

/*
=================
the BSP tree of the world in DFS order, nodes and leaves in 32 bytes each (a
cache line holds 2): R_RecursiveWorldNode reads mnode_t (56 bytes), its plane
and a box of floats, ~1 us a node and ~600 nodes a frame, mostly cache misses
=================
*/
#define WN_LEAF		0xfe		// type
#define WN_SOLID	0xff		// type, a CONTENTS_SOLID leaf

typedef struct
{
	short		mins[3], maxs[3];	// rounded outwards
	byte		type;		// the plane type, WN_LEAF or WN_SOLID
	int8_t		qnormal[3];	// the plane's normal * WN_QNORMAL, rounded (R_MeNodesWalk)
	union
	{
		float		dist;		// of the plane
		uint32_t	firstmark;	// leaves: in wn_marks
	};
	uint16_t	child[2];		// leaves: the number of marks, the leaf number
	uint16_t	firstsurf, numsurf;
	uint16_t	plane;		// WORLDMODEL->planes index, for non axial planes
	uint16_t	parent;		// WS_NONE for the root
} gu_wnode_t;

typedef char wn_size_check[sizeof( gu_wnode_t ) == 32 ? 1 : -1];

#define WN_QNORMAL	127.0f

static void		*wn_alloc;
static gu_wnode_t	*wn_nodes;		// NULL without
static int		wn_numnodes, wn_maxnodes;
static uint16_t		*wn_marks;		// ws_surfs indexes
static int		wn_nummarks, wn_maxmarks;
static uint16_t		*wn_leafnode;		// WORLDMODEL->leafs index to wn_nodes index
static uint16_t		*wn_visframe;		// the low bits of tr.visframecount in the PVS, 0 never

// the frustum side planes ( normal, -dist ), then | normal |, for R_WorldBoxTest
static float		wn_planes[8][4] __attribute__(( aligned( 16 )));

static int R_WorldNodeAdd( const model_t *world, const mnode_t *node, int parent )
{
	if( node->contents < 0 )
	{
		const int l = (const mleaf_t *)node - world->leafs;

		// the solid children are all leaf 0, never in the PVS
		if( l >= 0 && l <= world->numleafs && wn_leafnode[l] != WS_NONE )
		{
			if( node->contents == CONTENTS_SOLID )
				return wn_leafnode[l];
			return -1;
		}
	}

	if( wn_numnodes >= wn_maxnodes )
		return -1;

	const int n = wn_numnodes++;
	gu_wnode_t *w = &wn_nodes[n];

	for( int j = 0; j < 3; j++ )
	{
		const float mins = floorf( node->minmaxs[j] ), maxs = ceilf( node->minmaxs[j + 3] );

		if( mins < -32768.0f || maxs > 32767.0f )
			return -1;

		w->mins[j] = mins;
		w->maxs[j] = maxs;
	}

	w->parent = parent < 0 ? WS_NONE : parent;

	if( node->contents < 0 )
	{
		const mleaf_t *leaf = (const mleaf_t *)node;
		const int l = leaf - world->leafs;

		if( l < 0 || l > world->numleafs || leaf->nummarksurfaces < 0 || leaf->nummarksurfaces > wn_maxmarks - wn_nummarks )
			return -1;

		w->type = node->contents == CONTENTS_SOLID ? WN_SOLID : WN_LEAF;
		w->firstmark = wn_nummarks;
		w->child[0] = leaf->nummarksurfaces;
		w->child[1] = l;

		for( int i = 0; i < leaf->nummarksurfaces; i++ )
		{
			const int surf = leaf->firstmarksurface[i] - world->surfaces;

			if( surf < 0 || surf >= ws_numsurfs )
				return -1;

			wn_marks[wn_nummarks++] = surf;
		}

		wn_leafnode[l] = n;
		return n;
	}

	const int first = node_firstsurface( node, world ), num = node_numsurfaces( node, world );

	if( node->plane->type >= WN_LEAF || first < 0 || num < 0 || first + num > ws_numsurfs )
		return -1;

	w->type = node->plane->type;
	w->dist = node->plane->dist;
	w->plane = node->plane - world->planes;
	for( int j = 0; j < 3; j++ )
		w->qnormal[j] = (int)roundf( node->plane->normal[j] * WN_QNORMAL );
	w->firstsurf = first;
	w->numsurf = num;

	for( int side = 0; side < 2; side++ )
	{
		const int c = R_WorldNodeAdd( world, node_child( node, side, world ), n );

		if( c < 0 )
			return -1;

		w = &wn_nodes[n];
		w->child[side] = c;

		// the clip flags a node leaves hold for its subtree
		const gu_wnode_t *cw = &wn_nodes[c];
		if( cw->type != WN_SOLID )
		{
			for( int j = 0; j < 3; j++ )
			{
				w->mins[j] = Q_min( w->mins[j], cw->mins[j] );
				w->maxs[j] = Q_max( w->maxs[j], cw->maxs[j] );
			}
		}
	}

	for( int i = first; i < first + num; i++ )
	{
		if( !world->surfaces[i].info )
			continue;

		for( int j = 0; j < 3; j++ )
		{
			w->mins[j] = Q_min( w->mins[j], ws_surfs[i].mins[j] );
			w->maxs[j] = Q_max( w->maxs[j], ws_surfs[i].maxs[j] );
		}
	}

	return n;
}

/*
=================
R_WorldNodesBuild

after R_WorldSurfsBuild
=================
*/
static void R_WorldNodesBuild( void )
{
	const model_t *world = WORLDMODEL;

	if( wn_alloc )
		Mem_Free( wn_alloc );
	wn_alloc = NULL;
	wn_nodes = NULL;
	wn_numnodes = wn_nummarks = 0;

	if( !ws_surfs || !world->nodes || world->numplanes > WS_NONE )
		return;

	wn_maxnodes = world->numnodes + world->numleafs + 1;
	wn_maxmarks = world->nummarksurfaces;

	if( wn_maxnodes >= WS_NONE )
		return;

	// on cache lines
	wn_alloc = Mem_Malloc( r_temppool, sizeof( gu_wnode_t ) * wn_maxnodes + sizeof( uint16_t ) * ( wn_maxmarks + world->numleafs + 1 + wn_maxnodes ) + 63 );
	wn_nodes = (gu_wnode_t *)((( uintptr_t )wn_alloc + 63 ) & ~(uintptr_t)63 );
	wn_marks = (uint16_t *)( wn_nodes + wn_maxnodes );
	wn_leafnode = wn_marks + wn_maxmarks;
	wn_visframe = wn_leafnode + world->numleafs + 1;
	memset( wn_visframe, 0, sizeof( uint16_t ) * wn_maxnodes );
	memset( wn_nodes, 0, sizeof( gu_wnode_t ) * wn_maxnodes );
	memset( wn_leafnode, 0xff, sizeof( uint16_t ) * ( world->numleafs + 1 ));

	if( R_WorldNodeAdd( world, world->nodes, -1 ) < 0 )
	{
		gEngfuncs.Con_Reportf( "%s: doesn't fit, R_RecursiveWorldNode walks it\n", __func__ );
		Mem_Free( wn_alloc );
		wn_alloc = NULL;
		wn_nodes = NULL;
		return;
	}

	// mark them in R_MarkLeaves
	tr.fResetVis = true;

	gEngfuncs.Con_Reportf( "%s: %d nodes and leaves, %d marks\n", __func__, wn_numnodes, wn_nummarks );
}

/*
=================
R_WorldBoxTest

for a box of shorts (mins, maxs) on 16 bytes: the frustum side planes it is
behind of (bits 0-3), the ones it is in front of (bits 4-7) and the clip planes
(GU_ClipBoxInside) it is in front of (bits 8-11)
=================
*/
#define WB_GUARD	( 15 << 8 )	// clip flags: the clip planes

static inline int R_WorldBoxTest( const short *box )
{
	// the distances as words: their sign bits are the answers (the farthest
	// corners are never -0, the nearest ones can be: that keeps a plane)
	uint32_t d[12] __attribute__(( aligned( 16 )));

	__asm__ volatile(
		".set		push\n"
		".set		noreorder\n"
		"lv.q		C000, 0(%1)\n"			// mins, maxs, 2 shorts a word
		"vs2i.p		C200, C000\n"			// C200 = mins, maxs[0] ( << 16 )
		"vs2i.p		C210, C002\n"			// C210 = maxs[1], maxs[2], ...
		"vi2f.q		C200, C200, 16\n"
		"vi2f.p		C210, C210, 16\n"
		"vmov.s		S220, S203\n"			// C220 = maxs
		"vmov.s		S221, S210\n"
		"vmov.s		S222, S211\n"
		"vfim.s		S230, 0.5\n"
		"vadd.t		C020, C200, C220\n"		// C020 = center * 2
		"vsub.t		C030, C220, C200\n"		// C030 = extents * 2
		"vscl.t		C020, C020, S230\n"		// C020 = center
		"vscl.t		C030, C030, S230\n"		// C030 = extents
		"vzero.s	S033\n"
		"lv.q		C300,  0(%2)\n"			// M300 = the side planes
		"lv.q		C310, 16(%2)\n"
		"lv.q		C320, 32(%2)\n"
		"lv.q		C330, 48(%2)\n"
		"lv.q		C100, 64(%2)\n"			// M100 = | normals |
		"lv.q		C110, 80(%2)\n"
		"lv.q		C120, 96(%2)\n"
		"lv.q		C130, 112(%2)\n"
		"vhtfm4.q	C200, M300, C020\n"		// C200 = plane distances of the center
		"vtfm4.q	C210, M100, C030\n"		// C210 = the box's reach along each plane normal
		"vadd.q		C220, C200, C210\n"		// C220 = distances of the farthest corners
		"vsub.q		C230, C200, C210\n"		// C230 = distances of the nearest corners
		"vabs.q		C100, C700\n"			// M100 = | clip planes | (see GU_ClipBoxInside)
		"vabs.q		C110, C710\n"
		"vabs.q		C120, C720\n"
		"vabs.q		C130, C730\n"
		"vhtfm4.q	C300, M700, C020\n"
		"vtfm4.q	C310, M100, C030\n"
		"vsub.q		C320, C300, C310\n"		// distances of the nearest corners
		"sv.q		C220,  0(%0)\n"
		"sv.q		C230, 16(%0)\n"
		"sv.q		C320, 32(%0)\n"
		".set		pop\n"
		:
		: "r"( d ), "r"( box ), "r"( wn_planes )
		: "memory"
	);

	uint32_t test = 0;

	for( int i = 0; i < 4; i++ )
	{
		test |= ( d[i] >> 31 ) << i;
		test |= ( ~d[i + 4] >> 31 ) << ( i + 4 );
		test |= ( ~d[i + 8] >> 31 ) << ( i + 8 );
	}

	return test;
}

/*
=================
R_WorldBoxClip

the clip flags left for the box, -1 when it is culled
=================
*/
static int R_WorldBoxClip( const short *box, uint clipflags )
{
	const int test = R_WorldBoxTest( box );

	if( test & clipflags & 15 )
		return -1;

	clipflags &= ~((( test >> 4 ) & 15 ) | ( test & WB_GUARD ));

	// far and near
	if( clipflags & ( BIT( 4 )|BIT( 5 )))
	{
		const vec3_t mins = { box[0], box[1], box[2] };
		const vec3_t maxs = { box[3], box[4], box[5] };

		for( int i = 4; i < FRUSTUM_PLANES; i++ )
		{
			if( !FBitSet( clipflags, BIT( i )))
				continue;

			const int clipped = BOX_ON_PLANE_SIDE( mins, maxs, &RI.frustum.planes[i] );
			if( clipped == 2 ) return -1;
			if( clipped == 1 ) ClearBits( clipflags, BIT( i ));
		}
	}

	return clipflags;
}

/*
=================
the surfaces of the brush models in the world BSP ( *1, *2, ... ) for
R_SortBrushModelSurfaces, in 32 bytes each: R_CullSurface reads the surface,
its texinfo, texture, plane and extents, ~2.5 us a surface and ~180 a frame in
a firefight, mostly cache misses. The plane is flipped for SURF_PLANEBACK.
=================
*/
#define BS_SLOW		BIT( 0 )		// R_CullSurface and the water checks decide
#define BS_TURB		BIT( 1 )		// SURF_DRAWTURB
#define BS_PLANEZ	BIT( 2 )		// the plane is PLANE_Z
#define BS_PLANEBACK	BIT( 3 )		// SURF_PLANEBACK: the plane's dist is -dist

typedef struct
{
	vec3_t		normal;
	float		dist;
	short		mins[3], maxs[3];	// rounded outwards
	uint16_t	flags;
	uint16_t	pad;
} gu_bsurf_t;

typedef char bs_size_check[sizeof( gu_bsurf_t ) == 32 ? 1 : -1];

static void		*bs_alloc;
static gu_bsurf_t	*bs_surfs;		// for the surfaces from bs_first on, NULL without
static int		bs_first, bs_numsurfs;

static void R_BrushSurfsBuild( void )
{
	const model_t *world = WORLDMODEL;

	if( bs_alloc )
		Mem_Free( bs_alloc );
	bs_alloc = NULL;
	bs_surfs = NULL;
	bs_first = bs_numsurfs = 0;

	if( !world || !world->surfaces )
		return;

	// the world's own surfaces come first
	bs_first = world->firstmodelsurface + world->nummodelsurfaces;
	bs_numsurfs = world->numsurfaces - bs_first;
	if( bs_first < 0 || bs_numsurfs <= 0 )
	{
		bs_first = bs_numsurfs = 0;
		return;
	}

	bs_alloc = Mem_Malloc( r_temppool, sizeof( gu_bsurf_t ) * bs_numsurfs + 63 );
	bs_surfs = (gu_bsurf_t *)((( uintptr_t )bs_alloc + 63 ) & ~(uintptr_t)63 );
	memset( bs_surfs, 0, sizeof( gu_bsurf_t ) * bs_numsurfs );

	for( int i = 0; i < bs_numsurfs; i++ )
	{
		const msurface_t *surf = &world->surfaces[bs_first + i];
		gu_bsurf_t *b = &bs_surfs[i];

		if( !surf->info || !surf->plane || !surf->texinfo || !surf->texinfo->texture || VectorIsNull( surf->plane->normal ))
		{
			b->flags = BS_SLOW;
			continue;
		}

		if( FBitSet( surf->flags, SURF_DRAWTURB ))
			SetBits( b->flags, BS_TURB );
		if( surf->plane->type == PLANE_Z )
			SetBits( b->flags, BS_PLANEZ );

		if( FBitSet( surf->flags, SURF_PLANEBACK ))
		{
			SetBits( b->flags, BS_PLANEBACK );
			VectorNegate( surf->plane->normal, b->normal );
			b->dist = -surf->plane->dist;
		}
		else
		{
			VectorCopy( surf->plane->normal, b->normal );
			b->dist = surf->plane->dist;
		}

		for( int j = 0; j < 3; j++ )
		{
			b->mins[j] = bound( -32768, (int)floorf( surf->info->mins[j] ) - 1, 32767 );
			b->maxs[j] = bound( -32768, (int)ceilf( surf->info->maxs[j] ) + 1, 32767 );
		}
	}
}

/*
=================
R_WorldSurfsBuild

after the polygons and lightmaps of the world are built
=================
*/
static void R_WorldSurfsBuild( void )
{
	const model_t *world = WORLDMODEL;

	R_BrushSurfsBuild();
	R_MeWorldFree();

	if( ws_alloc )
		Mem_Free( ws_alloc );
	ws_alloc = NULL;
	ws_surfs = NULL;
	ws_numsurfs = 0;
	ws_numdlit = 0;
	ws_dlit_overflow = false;
	ws_numtextures = 0;
	memset( ws_texhead, 0xff, sizeof( ws_texhead ));
	memset( ws_lmhead, 0xff, sizeof( ws_lmhead ));

	if( !world || world->numsurfaces <= 0 || world->numsurfaces >= WS_NONE || world->numtextures >= WS_TEXTURE - 1 )
	{
		R_WorldNodesBuild();
		return;
	}

	// on cache lines
	ws_alloc = Mem_Malloc( r_temppool, ( sizeof( gu_wsurf_t ) + sizeof( uint16_t )) * world->numsurfaces + 63 );
	ws_surfs = (gu_wsurf_t *)((( uintptr_t )ws_alloc + 63 ) & ~(uintptr_t)63 );
	memset( ws_surfs, 0, sizeof( gu_wsurf_t ) * world->numsurfaces );
	ws_visframe = (uint16_t *)( ws_surfs + world->numsurfaces );
	ws_visfill = -1;
	ws_numsurfs = world->numsurfaces;
	ws_style0 = g_lightstylevalue[0];

	// no alternate frames in R_TextureAnimation
	cl_entity_t *ent = RI.currententity;
	RI.currententity = NULL;

	int slow = 0;
	for( int i = 0; i < world->numsurfaces; i++ )
	{
		const msurface_t *surf = &world->surfaces[i];
		gu_wsurf_t *w = &ws_surfs[i];
		int tex = 0;

		w->texnext = w->lmnext = WS_NONE;

		if( surf->info )
		{
			for( int j = 0; j < 3; j++ )
			{
				w->mins[j] = bound( -32768, (int)floorf( surf->info->mins[j] ) - 1, 32767 );
				w->maxs[j] = bound( -32768, (int)ceilf( surf->info->maxs[j] ) + 1, 32767 );
			}
		}

		if( surf->texinfo && surf->texinfo->texture )
		{
			const texture_t *t = surf->texinfo->texture;

			// the frame of a random tiling texture
			if( t->anim_total && t->name[0] == '-' )
				t = R_TextureAnimation( (msurface_t *)surf );

			for( tex = 0; tex < world->numtextures; tex++ )
			{
				if( world->textures[tex] == t )
					break;
			}
		}

		if( FBitSet( surf->flags, SURF_DRAWSKY ))
		{
			// see R_WorldSurfsCull
			w->texflags = WS_SKY;
			slow++;
			continue;
		}

		if( !surf->info || tex >= world->numtextures || R_WorldSurfIsSlow( world, surf ))
		{
			w->texflags = WS_SLOW|WS_TEXTURE;
			slow++;
			continue;
		}

		const gu_poly_t *p = GU_POLYS( surf );
		w->texflags = tex;
		if( p ) // on demand
		{
			w->verts = (gu_vert_t *)p->verts;
			w->numverts = p->numverts;
		}
		w->lightmap = surf->lightmaptexturenum;
		if( surf->pdecals )
			SetBits( w->texflags, WS_DECALS );
	}

	RI.currententity = ent;

	gEngfuncs.Con_Reportf( "%s: %d surfaces, %d through R_RenderBrushPoly\n", __func__, ws_numsurfs, slow );

	R_WorldNodesBuild();
}

static gu_wsurf_t *R_WorldSurf( const msurface_t *surf )
{
	if( !ws_surfs || surf < WORLDMODEL->surfaces || surf >= WORLDMODEL->surfaces + ws_numsurfs )
		return NULL;
	return &ws_surfs[surf - WORLDMODEL->surfaces];
}

/*
=================
R_SurfPoly

the polygon of a surface, built now if it's on demand. NULL without memory
=================
*/
gu_poly_t *R_SurfPoly( msurface_t *surf )
{
	if( surf->polys || !lp.world || lp.world != WORLDMODEL || !R_PolyIsLazy( lp.world, surf ))
		return GU_POLYS( surf );

	GL_BuildPolygonFromSurface( (model_t *)lp.world, surf );

	gu_poly_t *p = GU_POLYS( surf );
	gu_wsurf_t *w = R_WorldSurf( surf );

	if( p && w && !FBitSet( w->texflags, WS_SLOW ) && p->numverts >= 3 && p->numverts <= 255 )
	{
		w->verts = p->verts;
		w->numverts = p->numverts;
	}

	return p;
}

static void R_PolysDrop( void )
{
	// the GE and the ME read them
	R_WaitFrame();
	R_MeWorldWait();

	for( int i = 0; i < lp.numsurfs; i++ )
	{
		msurface_t *surf = &lp.world->surfaces[i];

		if( !surf->polys || FBitSet( surf->flags, SURF_DRAWTURB ))
			continue;

		gu_wsurf_t *w = R_WorldSurf( surf );

		surf->polys = NULL;
		if( w )
			w->verts = NULL;
	}

	while( lp.numchunks > LP_KEEP )
		free( lp.chunks[--lp.numchunks] );
	lp.chunk = lp.used = 0;
}

/*
=================
R_PolysForPVS

the PVS changed (R_MarkLeaves)
=================
*/
static void R_PolysForPVS( void )
{
	qboolean dropped = lp.numchunks <= LP_KEEP;

	// the first frame has no view leaf, everything is in its PVS
	if( !lp.world || lp.world != WORLDMODEL || !RI.viewleaf )
		return;

restart:
	for( int i = 0; i < WORLDMODEL->numleafs; i++ )
	{
		if( !CHECKVISBIT( RI.visbytes, i ))
			continue;

		const mleaf_t *leaf = &WORLDMODEL->leafs[i + 1];

		for( int j = 0; j < leaf->nummarksurfaces; j++ )
		{
			msurface_t *surf = leaf->firstmarksurface[j];

			if( surf->polys )
				continue;

			if( !dropped )
			{
				R_PolysDrop();
				dropped = true;
				goto restart;
			}

			if( !R_SurfPoly( surf ) && R_PolyIsLazy( lp.world, surf ))
				return; // out of memory
		}
	}
}

/*
=================
GU_WorldSurfDecals

the decals of a surface changed
=================
*/
void GU_WorldSurfDecals( const msurface_t *surf )
{
	gu_wsurf_t *w = R_WorldSurf( surf );

	if( !w )
		return;

	if( surf->pdecals )
		SetBits( w->texflags, WS_DECALS );
	else ClearBits( w->texflags, WS_DECALS );
}

/*
=================
GU_WorldSurfDlit

R_MarkLights marked the surface for this frame. Prebuilt lightmaps don't
change (R_CheckLightMap): it stays on the fast path then
=================
*/
void GU_WorldSurfDlit( const msurface_t *surf )
{
	if( lm_prebuilt && lm_prebuilt == WORLDMODEL )
		return;

	gu_wsurf_t *w = R_WorldSurf( surf );

	if( !w || FBitSet( w->texflags, WS_DLIT ))
		return;

	if( ws_numdlit >= WS_MAX_DLIT )
	{
		ws_dlit_overflow = true;
		return;
	}

	SetBits( w->texflags, WS_DLIT );
	ws_dlit[ws_numdlit++] = w - ws_surfs;
}

/*
=================
GU_WorldSurfsNewFrame

before R_PushDlights
=================
*/
void GU_WorldSurfsNewFrame( void )
{
	for( int i = 0; i < ws_numdlit; i++ )
		ClearBits( ws_surfs[ws_dlit[i]].texflags, WS_DLIT );

	ws_numdlit = 0;
	ws_dlit_overflow = false;
}

/*
=================
R_WorldSurfsBeginWalk
=================
*/
static void R_WorldSurfsBeginWalk( void )
{
	// light style 0 changed (it does once after loading, the lightstyles come
	// from the server): R_CheckLightMap updates each lightmap when its surface
	// is drawn by R_RenderBrushPoly
	if( g_lightstylevalue[0] != ws_style0 && WORLDMODEL != lm_prebuilt )
	{
		for( int i = 0; i < ws_numsurfs; i++ )
		{
			if( !FBitSet( ws_surfs[i].texflags, WS_SLOW ))
				SetBits( ws_surfs[i].texflags, WS_STALE );
		}
		ws_style0 = g_lightstylevalue[0];
	}

	// dlights the list missed, details on every surface in fog
	// (R_RenderDetailsForSurface), alternate texture frames
	ws_allslow = ws_dlit_overflow || ( glState.isFogEnabled && r_detailtextures.value ) || RI.currententity->curstate.frame;
	ws_skybox = !FBitSet( tr.world->flags, FWORLD_SKYSPHERE ) || FBitSet( tr.world->flags, FWORLD_CUSTOM_SKYBOX );
	ws_skyseen = false;

	memset( ws_lmhead, 0xff, sizeof( ws_lmhead ));

	// a value no frame matches before the next fill
	if(( tr.framecount >> 15 ) != ws_visfill )
	{
		ws_visfill = tr.framecount >> 15;
		const uint16_t fill = ( tr.framecount & 0x8000 ) ^ 0x8000;
		for( int i = 0; i < ws_numsurfs; i++ )
			ws_visframe[i] = fill;
	}

	for( int i = 0; i < 4; i++ )
	{
		const mplane_t *p = &RI.frustum.planes[i];

		for( int j = 0; j < 3; j++ )
		{
			wn_planes[i][j] = p->normal[j];
			wn_planes[i + 4][j] = fabsf( p->normal[j] );
		}
		wn_planes[i][3] = -p->dist;
		wn_planes[i + 4][3] = 0.0f;
	}
}

// a visible surface for R_RenderBrushPoly
static void R_WorldSurfSlow( int i )
{
	gu_wsurf_t *w = &ws_surfs[i];
	msurface_t *surf = &WORLDMODEL->surfaces[i];

	// R_RenderBrushPoly updates it this frame, unless dlit
	if( !FBitSet( w->texflags, WS_DLIT ))
		ClearBits( w->texflags, WS_STALE );

	if( !surf->texinfo || !surf->texinfo->texture )
		return;

	if( surf->flags & SURF_DRAWSKY )
	{
		// make sky chain to right clip the skybox
		surf->texturechain = skychain;
		skychain = surf;
	}
	else
	{
		texture_t *t = surf->texinfo->texture;
		int k = 0;

		while( k < ws_numslowtextures && ws_slowtextures[k] != t )
			k++;

		if( k == ws_numslowtextures )
		{
			if( k < WS_MAX_SLOWTEXTURES )
				ws_slowtextures[ws_numslowtextures++] = t;
			else ws_numslowtextures = -1;
		}

		surf->texturechain = t->texturechain;
		t->texturechain = surf;
	}
}

// a visible plain one into the chains of the two passes
static void R_WorldSurfChain( int i )
{
	gu_wsurf_t *w = &ws_surfs[i];
	const int tex = w->texflags & WS_TEXTURE;

	if( ws_texhead[tex] == WS_NONE )
		ws_textures[ws_numtextures++] = tex;
	w->texnext = ws_texhead[tex];
	ws_texhead[tex] = i;
	w->lmnext = ws_lmhead[w->lightmap];
	ws_lmhead[w->lightmap] = i;
}

/*
=================
R_WorldSurfsCull

the surfaces of a node in R_RecursiveWorldNode
=================
*/
static void R_WorldSurfsCull( int first, int count, uint clipflags )
{
	const uint16_t visframe = tr.framecount;
	gu_wsurf_t *w = &ws_surfs[first];

	for( int i = first; i < first + count; i++, w++ )
	{
		if( ws_visframe[i] != visframe )
			continue;

		const qboolean slow = FBitSet( w->texflags, WS_SLOW|WS_DLIT|WS_DECALS|WS_STALE ) || ws_allslow;
		int left = 0;

		if( clipflags )
		{
			left = R_WorldBoxClip( w->mins, clipflags );

			if( left < 0 )
				continue;
		}

		if( !slow && !w->verts )
			continue;

		if( slow )
		{
			// R_AddSkyBoxSurface cuts each sky polygon at the box edges to find the
			// part of the box it shows, ~10 us a polygon, ~30 a frame: the box is
			// drawn behind the world, its visible faces whole show the same
			if( ws_skybox && w->texflags == WS_SKY )
				ws_skyseen = true;
			else R_WorldSurfSlow( i );
			continue;
		}

		if( !FBitSet( left, WB_GUARD ))
			SetBits( w->texflags, WS_NOCLIP );
		else ClearBits( w->texflags, WS_NOCLIP );

		R_WorldSurfChain( i );
	}
}

static void R_WorldSurfDraw( const gu_wsurf_t *w, gu_vert_t *verts )
{
	if( GU_WorldListFull( ))
		return;

	if( FBitSet( w->texflags, WS_NOCLIP ))
		extGuDrawArrayBatched( GU_TRIANGLE_FAN, GU_TEXTURE_32BITF | GU_VERTEX_32BITF, w->numverts, verts );
	else GU_DrawPolyVerts( verts, w->numverts );
}

/*
=================
R_WorldSurfsDrawTextures

the texture pass, in R_DrawTextureChains
=================
*/
static void R_WorldSurfsDrawTextures( void )
{
	for( int i = 0; i < ws_numtextures; i++ )
	{
		const int tex = ws_textures[i];

		GL_Bind( XASH_TEXTURE0, R_WorldTextureFrame( WORLDMODEL->textures[tex] )->gl_texturenum );

		for( int s = ws_texhead[tex]; s != WS_NONE; s = ws_surfs[s].texnext )
		{
			R_WorldSurfDraw( &ws_surfs[s], ws_surfs[s].verts );
			r_stats.c_world_polys++;
		}

		ws_texhead[tex] = WS_NONE;
	}

	ws_numtextures = 0;
}

/*
=================
R_WorldSurfsDrawLightmaps

the lightmap pass of a page, in R_BlendLightmaps
=================
*/
static void R_WorldSurfsDrawLightmaps( int page )
{
	for( int s = ws_lmhead[page]; s != WS_NONE; s = ws_surfs[s].lmnext )
	{
		const gu_wsurf_t *w = &ws_surfs[s];

		// the page again before a polygon that may need clipping: the GE drew some
		// of those with another palette than the page's (whole floor polygons red in
		// deluge_beta_v3 after a while of play), although the commands between the
		// page's bind and these draws are only clipped vertices and draws
		if( !FBitSet( w->texflags, WS_NOCLIP ))
		{
			glState.currentTexture = -1;
			GL_Bind( XASH_TEXTURE0, tr.lightmapTextures[page] );
		}
		R_WorldSurfDraw( w, w->verts + w->numverts );
	}

	ws_lmhead[page] = WS_NONE;
}

/*
=============================================================

THE WORLD AND THE STATIC BRUSH MODELS ON THE MEDIA ENGINE

The walk of the world and the draws of the plain surfaces, from the view on:
R_MeWorldBegin posts a job for the second CPU (engine/platform/psp/me_psp.c)
while the main CPU prepares the entities. The job marks and culls like
R_WorldNodesWalk and R_WorldSurfsCull and writes the GE commands of the
texture pass and the lightmap pass as two lists, which R_DrawTextureChains and
R_BlendLightmaps call. What it can't draw (the slow surfaces, polygons that
need clipping, leaves with efrags) it lists for the main CPU (R_MeWorldEnd).

R_MeBrushesBegin posts a second one for the static brush models in the draw
lists (R_StudioMeTable), after the bones (R_StudioMePrepare): culled like R_SortBrushModelSurfaces,
two lists for the opaque ones and two for the alpha tested ones
(R_MeBrushesDraw).

If a job runs out of space the main CPU does the work as before.

No VFPU, no syscalls, and only its output and the job are written (cache lines
of their own): see me_psp.c.

=============================================================
*/
static CVAR_DEFINE_AUTO( r_world_me, "1", FCVAR_ARCHIVE, "walk the world and write its draw commands on the Media Engine" );
static CVAR_DEFINE_AUTO( r_brushes_me, "1", FCVAR_ARCHIVE, "the same for the static brush models (needs r_world_me)" );

#define MEWORLD_MAXVIS	4096	// visible plain surfaces
#define MEWORLD_MAXLIST	1024	// surfaces for the main CPU, of each kind
#define MEWORLD_MAXLEAFS	256	// leaves with efrags
#define MEWORLD_CMDS	16384	// GE commands of all lists of a frame (deluge_beta_v3: up to ~13600)
#define MEWORLD_BUFFERS	3	// the GE may still read the lists of two frames (R_NextFrame), R_MeWorldEarly writes the next one's
#define MEWORLD_MAXBIND	48	// GE commands of one bind
#define MEWORLD_MAXBRUSHES	128	// static brush models
#define MEWORLD_BACKSIDE	BIT( 15 )	// meworld_res_t.who: CULL_BACKSIDE

enum
{
	MEWORLD_WORLD = 0,
	MEWORLD_BRUSHES,		// kRenderNormal
	MEWORLD_ABRUSHES,		// kRenderTransAlpha
	MEWORLD_RESULTS
};

typedef struct
{
	uint32_t		framecount;	// the job's, if it didn't run out of space
	uint32_t		numslow, numclip;
	uint32_t		numpolys, numleafs, leafs, skyseen;
	uint32_t		texlist, lmlist;	// offsets in cmds, 0: nothing to draw
	uint32_t		pad[7];

	uint16_t		slow[MEWORLD_MAXLIST];
	uint16_t		clip[MEWORLD_MAXLIST];
	uint16_t		who[MEWORLD_MAXLIST];	// of a slow one: the brush model in the job, MEWORLD_BACKSIDE
} meworld_res_t;

typedef struct
{
	meworld_res_t	res[MEWORLD_RESULTS];
	uint16_t		leaf[MEWORLD_MAXLEAFS];

	// the ME's own
	struct
	{
		const gu_vert_t	*verts;	// not read from the surface again: they are far apart
		uint16_t		texnext, lmnext;
		byte		numverts, pad[3];
	} vis[MEWORLD_MAXVIS];
	uint16_t		texhead[WS_TEXTURE + 1], textail[WS_TEXTURE + 1], textures[WS_TEXTURE + 1];
	uint16_t		lmhead[MAX_LIGHTMAPS];
	uint32_t		texused[( MAX_TEXTURES + 31 ) >> 5];
	uint32_t		cmds[MEWORLD_BUFFERS][MEWORLD_CMDS] __attribute__(( aligned( 64 )));
} meworld_out_t;

typedef struct
{
	meworld_out_t	*out;
	uint32_t		*cmds;		// the frame's in out
	uint16_t		*visframe;	// ws_visframe for the ME, after out in the same block
	int		visfill;		// fill it with this value first, -1 don't
	uint32_t		framecount;
	uint16_t		visframecount;
	uint16_t		stamp;		// marks the surfaces of this walk in visframe
	byte		allslow, skybox;
	uint		clipflags;
	vec3_t		org;
	int		time10, time20;	// animated textures: frames at 10 and 20 fps
	float		planes[12][4];	// normal, -dist; the clip planes from 8 on, as in the clip flags
	float		absplanes[12][4];
	float		qerror;		// how far R_MeNodesWalk's dot product with a rounded normal can be off

	// the ME's own
	uint		*p;		// end of the commands
	uint		base;		// the last BASE in this list, ~0 none yet
	int		full;
	int		numvis, numtextures;
	meworld_res_t	*res;
	int		who;
} meworld_job_t;

// posted while the world's job may still run: in cache lines of its own
typedef struct
{
	meworld_job_t	*world;
	float		front;		// backface culling: 1 or -1, 0 off
	int		numbrushes;
	meworld_brush_t	brushes[MEWORLD_MAXBRUSHES];
} meworld_brushjob_t;

static struct
{
	meworld_job_t	job __attribute__(( aligned( 64 )));
	meworld_brushjob_t	brushjob __attribute__(( aligned( 64 )));
	void		*alloc __attribute__(( aligned( 64 )));
	uint32_t		ticket, brush_ticket;	// not waited for yet
	int		visfill;
	uint		vert_frame;	// of the last job: one per GE list
	qboolean		begun;		// this frame: the world's job was posted
	qboolean		brushes;		// and the brush models', not drawn yet
	uint		texlist, lmlist;	// addresses, for the next R_DrawTextureChains and R_BlendLightmaps
	cl_entity_t	*ents[MEWORLD_MAXBRUSHES];
	uint16_t		stamp;		// the last job's

	// R_MeWorldEarly: the next frame's walk, from this one's view with margins
	struct
	{
		qboolean		ready;		// this frame's view is in here
		uint32_t		ticket;
		uint		vert_frame;	// the frame it's for, and what it must find there
		int		framecount, visframecount;
		const model_t	*world;
		vec3_t		org, forward, right, up;
		vec3_t		lastorg;		// the view of the last capture
		const model_t	*lastworld;
		float		fov_x, fov_y, farclip;
		int		flags;
		byte		allslow, skybox;
		uint		clipflags;
		float		qerror;
		float		planes[12][4];
		int		time10, time20;
	} early;
} r_meworld __attribute__(( aligned( 64 )));

static CVAR_DEFINE_AUTO( r_world_me_early, "1", FCVAR_ARCHIVE, "walk the world for the next frame on the Media Engine at the end of a frame, used if the view stays close" );

// how far the next view may be from this one, for R_MeWorldEarly
#define MEWORLD_EARLY_MOVE	8.0f	// units
#define MEWORLD_EARLY_TURN	8.0f	// degrees the planes are widened by
#define MEWORLD_EARLY_COS	0.99619f	// cos( 5 degrees ): what is accepted of each axis

// uncached view
#define MEWORLD_OUT	((volatile meworld_out_t *)( 0x40000000 | (uintptr_t)r_meworld.job.out ))

// R_WorldBoxClip without the VFPU
static int R_MeBoxClip( const meworld_job_t *job, const short *box, uint clipflags )
{
	const float x0 = box[0], y0 = box[1], z0 = box[2];
	const float x1 = box[3], y1 = box[4], z1 = box[5];
	const float cx = ( x0 + x1 ) * 0.5f, cy = ( y0 + y1 ) * 0.5f, cz = ( z0 + z1 ) * 0.5f;
	const float ex = ( x1 - x0 ) * 0.5f, ey = ( y1 - y0 ) * 0.5f, ez = ( z1 - z0 ) * 0.5f;
	uint flags = clipflags;

	while( flags )
	{
		const int i = __builtin_ctz( flags );
		const float *p = job->planes[i];
		const float *a = job->absplanes[i];
		const float dist = p[0] * cx + p[1] * cy + p[2] * cz + p[3];
		const float reach = a[0] * ex + a[1] * ey + a[2] * ez;

		flags &= flags - 1;

		if( dist >= reach )
			clipflags &= ~BIT( i );
		else if( i < 8 && dist + reach < 0.0f )
			return -1;
	}

	return clipflags;
}

// GU_ClipIsRequired without the VFPU
static qboolean R_MeClipIsRequired( const meworld_job_t *job, const gu_vert_t *v, int numverts )
{
	for( ; numverts > 0; numverts--, v++ )
	{
		for( int i = 8; i < 12; i++ )
		{
			const float *p = job->planes[i];

			if( p[0] * v->xyz[0] + p[1] * v->xyz[1] + p[2] * v->xyz[2] + p[3] < 0.0f )
				return true;
		}
	}

	return false;
}

// a surface in the view: into the chains, or for the main CPU
static void R_MeSurf( meworld_job_t *job, int i, const gu_wsurf_t *w, uint left )
{
	meworld_out_t *out = job->out;
	meworld_res_t *res = job->res;

	if( FBitSet( w->texflags, WS_SLOW|WS_DLIT|WS_DECALS|WS_STALE ) || job->allslow )
	{
		if( res->numslow < MEWORLD_MAXLIST )
		{
			res->who[res->numslow] = job->who;
			res->slow[res->numslow++] = i;
		}
		else job->full = true;
		return;
	}

	if( !w->verts )
		return;

	if( FBitSet( left, WB_GUARD ) && R_MeClipIsRequired( job, w->verts, w->numverts ))
	{
		if( res->numclip < MEWORLD_MAXLIST )
			res->clip[res->numclip++] = i;
		else job->full = true;
		return;
	}

	if( job->numvis >= MEWORLD_MAXVIS )
	{
		job->full = true;
		return;
	}

	const int n = job->numvis++;
	const int tex = w->texflags & WS_TEXTURE;

	out->vis[n].verts = w->verts;
	out->vis[n].numverts = w->numverts;
	// the nearest first: what they hide fails the depth test
	if( out->texhead[tex] == WS_NONE )
	{
		out->textures[job->numtextures++] = tex;
		out->texhead[tex] = n;
	}
	else out->vis[out->textail[tex]].texnext = n;
	out->vis[n].texnext = WS_NONE;
	out->textail[tex] = n;
	out->vis[n].lmnext = out->lmhead[w->lightmap];
	out->lmhead[w->lightmap] = n;
}

// R_WorldSurfsCull
static void R_MeSurfsCull( meworld_job_t *job, int first, int count, uint clipflags )
{
	const uint16_t visframe = job->stamp;
	const gu_wsurf_t *w = &ws_surfs[first];

	for( int i = first; i < first + count; i++, w++ )
	{
		if( job->visframe[i] != visframe )
			continue;

		int left = 0;

		if( clipflags )
		{
			left = R_MeBoxClip( job, w->mins, clipflags );

			if( left < 0 )
				continue;
		}

		if( job->skybox && w->texflags == WS_SKY )
			job->res->skyseen = true;
		else R_MeSurf( job, i, w, left );
	}
}

// R_WorldNodesWalk
static void R_MeNodesWalk( meworld_job_t *job, int n, uint clipflags )
{
	const uint16_t visframe = job->visframecount;

	for( ;; )
	{
		const gu_wnode_t *w = &wn_nodes[n];

		if( w->type == WN_SOLID )
			return;

		if( clipflags )
		{
			const int left = R_MeBoxClip( job, w->mins, clipflags );

			if( left < 0 )
				return;
			clipflags = left;
		}

		if( w->type == WN_LEAF )
		{
			const uint16_t *mark = &wn_marks[w->firstmark];
			const uint16_t framecount = job->stamp;

			for( int i = 0; i < w->child[0]; i++ )
				job->visframe[mark[i]] = framecount;

			if( WORLDMODEL->leafs[w->child[1]].efrags )
			{
				if( job->res->numleafs < MEWORLD_MAXLEAFS )
					job->out->leaf[job->res->numleafs++] = w->child[1];
				else job->full = true;
			}

			job->res->leafs++;
			return;
		}

		// The leaves on the view's side are marked before the node's surfaces: the ones
		// that face away aren't drawn. The plane is a cache miss, the node has its normal
		// rounded: when that could pick the wrong side, both sides are marked first
		// (some surfaces that face away are drawn).
		int side;

		if( w->type < 3 )
			side = job->org[w->type] - w->dist >= 0.0f ? 0 : 1;
		else
		{
			const float dot = ( job->org[0] * w->qnormal[0] + job->org[1] * w->qnormal[1] + job->org[2] * w->qnormal[2] ) * ( 1.0f / WN_QNORMAL ) - w->dist;

			if( dot >= job->qerror )
				side = 0;
			else if( dot <= -job->qerror )
				side = 1;
			else
			{
				for( int c = 0; c < 2; c++ )
				{
					if( wn_visframe[w->child[c]] == visframe )
						R_MeNodesWalk( job, w->child[c], clipflags );
				}
				if( w->numsurf )
					R_MeSurfsCull( job, w->firstsurf, w->numsurf, clipflags );
				return;
			}
		}

		if( wn_visframe[w->child[side]] == visframe )
			R_MeNodesWalk( job, w->child[side], clipflags );

		if( w->numsurf )
			R_MeSurfsCull( job, w->firstsurf, w->numsurf, clipflags );

		n = w->child[!side];
		if( wn_visframe[n] != visframe )
			return;
	}
}

// R_CullBox and R_SortBrushModelSurfaces for a model that doesn't blend
static void R_MeBrush( meworld_job_t *job, const meworld_brush_t *m, float front, int who )
{
	const int left = R_MeBoxClip( job, m->mins, job->clipflags );

	if( left < 0 )
		return;

	const uint clipflags = m->cullsurfs ? left : left & WB_GUARD;
	const gu_bsurf_t *b = &bs_surfs[m->firstsurf - bs_first];

	for( int i = m->firstsurf; i < m->firstsurf + m->numsurfs; i++, b++ )
	{
		const gu_wsurf_t *w = &ws_surfs[i];
		int back = 0, surfleft = 0;

		if( FBitSet( b->flags, BS_SLOW ))
		{
			job->full = true;
			return;
		}

		if( FBitSet( b->flags, BS_TURB ))
		{
			if( !FBitSet( b->flags, BS_PLANEZ ) && !m->watersides )
				continue;

			if( m->waterz >= ( FBitSet( b->flags, BS_PLANEBACK ) ? -b->dist : b->dist ))
				continue;
		}

		if( front != 0.0f && front * ( DotProduct( job->org, b->normal ) - b->dist ) <= BACKFACE_EPSILON )
		{
			if( !FBitSet( b->flags, BS_TURB ))
				continue;
			back = MEWORLD_BACKSIDE;
		}
		else if( clipflags )
		{
			surfleft = R_MeBoxClip( job, b->mins, clipflags );

			if( surfleft < 0 )
				continue;
		}

		job->who = who | back;
		R_MeSurf( job, i, w, surfleft );
	}
}

static qboolean R_MeSpace( meworld_job_t *job, int words )
{
	if( job->p + words <= job->cmds + MEWORLD_CMDS )
		return true;

	job->full = true;
	return false;
}

static qboolean R_MeBind( meworld_job_t *job, int texnum )
{
	const gl_texture_t *t = texnum > 0 && texnum < MAX_TEXTURES ? R_GetTexture( texnum ) : NULL;

	// GL_Bind would use the default texture: leave that to the main CPU
	if( !t || !t->dstTexture || (( t->format == GU_PSM_T8 || t->format == GU_PSM_T4 ) && !t->dstPalette ))
		job->full = true;

	if( !R_MeSpace( job, MEWORLD_MAXBIND ) || job->full )
		return false;

	job->p = GL_BindCommands( t, job->p );
	SetBits( job->out->texused[texnum >> 5], BIT( texnum & 31 ));
	return true;
}

// extGuDrawArrayNoStall with the vertex type set before
static qboolean R_MeDrawVerts( meworld_job_t *job, const gu_vert_t *verts, int numverts )
{
	const uint base = ( 16 << 24 ) | (((uint)verts >> 8 ) & 0xf0000 );
	uint *p = job->p;

	if( !R_MeSpace( job, 3 ))
		return false;

	// the polygons are mostly in one 16 MB block
	if( base != job->base )
		*p++ = job->base = base; // BASE
	*p++ = ( 1 << 24 ) | ((uint)verts & 0xffffff ); // VADDR
	*p++ = ( 4 << 24 ) | ( GU_TRIANGLE_FAN << 16 ) | numverts; // PRIM
	job->p = p;
	return true;
}

static uint32_t R_MeListBegin( meworld_job_t *job )
{
	if( !R_MeSpace( job, 2 ))
		return 0;

	*job->p++ = 0; // NOP, so that no list starts at 0
	*job->p++ = ( 18 << 24 ) | GU_TEXTURE_32BITF | GU_VERTEX_32BITF; // VTYPE
	job->base = ~0u;
	return job->p - 1 - job->cmds;
}

static void R_MeListEnd( meworld_job_t *job )
{
	if( R_MeSpace( job, 1 ))
		*job->p++ = 11 << 24; // RET
}

// R_WorldTextureFrame
static const texture_t *R_MeTextureFrame( const meworld_job_t *job, const texture_t *base )
{
	if( !base->anim_total || base->name[0] == '-' )
		return base;

	const int reletive = ( FBitSet( R_GetTexture( base->gl_texturenum )->flags, TF_QUAKEPAL ) ? job->time10 : job->time20 ) % base->anim_total;
	const texture_t *t = base;
	int count = 0;

	while( t->anim_min > reletive || t->anim_max <= reletive )
	{
		t = t->anim_next;

		if( !t || ++count > MOD_FRAMES )
			return base;
	}

	return t;
}

// the chains as GE commands
static void R_MeCommands( meworld_job_t *job )
{
	meworld_out_t *out = job->out;

	job->res->texlist = R_MeListBegin( job );

	for( int i = 0; i < job->numtextures; i++ )
	{
		const int tex = out->textures[i];

		if( !R_MeBind( job, R_MeTextureFrame( job, WORLDMODEL->textures[tex] )->gl_texturenum ))
			return;

		for( int n = out->texhead[tex]; n != WS_NONE; n = out->vis[n].texnext )
		{
			if( !R_MeDrawVerts( job, out->vis[n].verts, out->vis[n].numverts ))
				return;
		}
	}

	R_MeListEnd( job );
	job->res->lmlist = R_MeListBegin( job );

	for( int i = 0; i < MAX_LIGHTMAPS; i++ )
	{
		if( out->lmhead[i] == WS_NONE )
			continue;

		if( !R_MeBind( job, tr.lightmapTextures[i] ))
			return;

		for( int n = out->lmhead[i]; n != WS_NONE; n = out->vis[n].lmnext )
		{
			if( !R_MeDrawVerts( job, out->vis[n].verts + out->vis[n].numverts, out->vis[n].numverts ))
				return;
		}
	}

	R_MeListEnd( job );
}

static void R_MeResultBegin( meworld_job_t *job, int result )
{
	meworld_out_t *out = job->out;
	meworld_res_t *res = &out->res[result];

	job->res = res;
	job->numvis = job->numtextures = 0;
	res->framecount = 0;
	res->numslow = res->numclip = res->numleafs = 0;
	res->leafs = res->skyseen = 0;
	res->texlist = res->lmlist = 0;
	memset( out->texhead, 0xff, WORLDMODEL->numtextures * sizeof( out->texhead[0] ));
	memset( out->lmhead, 0xff, sizeof( out->lmhead ));
}

static void R_MeResultEnd( meworld_job_t *job )
{
	if( job->numvis && !job->full )
		R_MeCommands( job );

	job->res->numpolys = job->numvis;
	if( !job->full )
		job->res->framecount = job->framecount;
}

static void R_MeWorldJob( void *arg )
{
	meworld_job_t *job = arg;

	if( job->visfill >= 0 )
	{
		for( int i = 0; i < ws_numsurfs; i++ )
			job->visframe[i] = job->visfill;
	}

	memset( job->out->texused, 0, sizeof( job->out->texused ));
	job->full = false;
	job->p = job->cmds;
	job->who = 0;

	R_MeResultBegin( job, MEWORLD_WORLD );
	if( wn_visframe[0] == job->visframecount )
		R_MeNodesWalk( job, 0, job->clipflags );
	R_MeResultEnd( job );
}

// after the world's, its commands too
static void R_MeBrushesJob( void *arg )
{
	const meworld_brushjob_t *bjob = arg;
	meworld_job_t *job = bjob->world;

	for( int r = MEWORLD_BRUSHES; r <= MEWORLD_ABRUSHES; r++ )
	{
		R_MeResultBegin( job, r );

		for( int i = 0; i < bjob->numbrushes; i++ )
		{
			if( bjob->brushes[i].result == r )
				R_MeBrush( job, &bjob->brushes[i], bjob->front, i );
		}

		R_MeResultEnd( job );
	}
}

void R_MeWorldWait( void )
{
	PSP_MeWaitFor( r_meworld.early.ticket );
	PSP_MeWaitFor( r_meworld.ticket );
	PSP_MeWaitFor( r_meworld.brush_ticket );
	r_meworld.early.ticket = r_meworld.ticket = r_meworld.brush_ticket = 0;
	r_meworld.brushes = false;
	r_meworld.texlist = r_meworld.lmlist = 0;
	r_meworld.begun = false;
}

static void R_MeWorldFree( void )
{
	R_MeWorldWait();
	if( r_meworld.alloc )
		free( r_meworld.alloc );
	r_meworld.alloc = NULL;
}

void R_MeWorldRegister( void )
{
	gEngfuncs.Cvar_RegisterVariable( &r_world_me );
	gEngfuncs.Cvar_RegisterVariable( &r_brushes_me );
	gEngfuncs.Cvar_RegisterVariable( &r_world_me_early );
}

// the stamp of the next walk, and whether its visframe needs a fill first
static void R_MeWorldStamp( meworld_job_t *job )
{
	job->stamp = ++r_meworld.stamp;
	job->visfill = -1;

	// a value no walk stamps before the next fill
	if(( job->stamp >> 15 ) != r_meworld.visfill )
	{
		r_meworld.visfill = job->stamp >> 15;
		job->visfill = ( job->stamp & 0x8000 ) ^ 0x8000;
	}
}

// the view of the walk: RI's
static void R_MeWorldView( meworld_job_t *job )
{
	// each component of the rounded normal is off by half a step at most, and the float math a little
	job->qerror = ( fabsf( RI.cullorigin[0] ) + fabsf( RI.cullorigin[1] ) + fabsf( RI.cullorigin[2] )) * ( 0.5f / WN_QNORMAL ) + 1.0f;
	job->clipflags = ( r_nocull.value ? 0 : RI.frustum.clipFlags )|WB_GUARD;
	VectorCopy( RI.cullorigin, job->org );

	for( int i = 0; i < FRUSTUM_PLANES; i++ )
	{
		VectorCopy( RI.frustum.planes[i].normal, job->planes[i] );
		job->planes[i][3] = -RI.frustum.planes[i].dist;
	}
	GU_ClipGetWorldPlanes( job->planes + 8 );

	for( int i = 0; i < 12; i++ )
	{
		for( int j = 0; j < 3; j++ )
			job->absplanes[i][j] = fabsf( job->planes[i][j] );
	}
}

/*
=================
R_MeWorldCapture

this frame's view, for a walk of the next one that covers any view up to
MEWORLD_EARLY_MOVE away and turned by up to MEWORLD_EARLY_TURN: the frustum
wider and further out, the guard band narrower and further in (a polygon it
lets through may need clipping in the next view), the side of a plane can be
off by the move
=================
*/
static void R_MeWorldCapture( void )
{
	const float turn = DEG2RAD( MEWORLD_EARLY_TURN );
	gl_frustum_t frustum;

	r_meworld.early.ready = false;

	// the dlights of the next frame are marked while the walk may run
	if( !r_world_me_early.value || ( ws_numdlit && WORLDMODEL != lm_prebuilt ) || r_nocull.value
		|| RI.rvp.fov_x + 2.0f * MEWORLD_EARLY_TURN >= 170.0f || RI.rvp.fov_y + 2.0f * MEWORLD_EARLY_TURN >= 170.0f )
		return;

	// where the view goes if it keeps moving as it did since the last frame
	vec3_t move = { 0.0f, 0.0f, 0.0f };

	if( r_meworld.early.lastworld == WORLDMODEL )
	{
		VectorSubtract( RI.cullorigin, r_meworld.early.lastorg, move );
		if( DotProduct( move, move ) > 32.0f * 32.0f )
			VectorClear( move ); // teleported
	}
	VectorCopy( RI.cullorigin, r_meworld.early.lastorg );
	r_meworld.early.lastworld = WORLDMODEL;

	r_meworld.early.world = WORLDMODEL;
	r_meworld.early.framecount = tr.framecount + 1;
	r_meworld.early.visframecount = tr.visframecount;
	VectorAdd( RI.cullorigin, move, r_meworld.early.org );
	VectorCopy( RI.cull_vforward, r_meworld.early.forward );
	VectorCopy( RI.cull_vright, r_meworld.early.right );
	VectorCopy( RI.cull_vup, r_meworld.early.up );
	r_meworld.early.fov_x = RI.rvp.fov_x;
	r_meworld.early.fov_y = RI.rvp.fov_y;
	r_meworld.early.farclip = R_GetFarClip();
	r_meworld.early.flags = RI.rvp.flags;
	r_meworld.early.allslow = ws_allslow;
	r_meworld.early.skybox = ws_skybox;
	r_meworld.early.clipflags = RI.frustum.clipFlags|WB_GUARD;
	r_meworld.early.qerror = ( fabsf( RI.cullorigin[0] ) + fabsf( RI.cullorigin[1] ) + fabsf( RI.cullorigin[2] )) * ( 0.5f / WN_QNORMAL ) + 1.0f + MEWORLD_EARLY_MOVE;
	r_meworld.early.time10 = (int)(( 2.0 * gp_cl->time - gp_cl->oldtime ) * 10 );
	r_meworld.early.time20 = (int)(( 2.0 * gp_cl->time - gp_cl->oldtime ) * 20 );

	GL_FrustumInitProj( &frustum, 0.0f, r_meworld.early.farclip, RI.rvp.fov_x + 2.0f * MEWORLD_EARLY_TURN, RI.rvp.fov_y + 2.0f * MEWORLD_EARLY_TURN );
	// the planes are through RI.cullorigin: moved with the view
	for( int i = 0; i < FRUSTUM_PLANES; i++ )
	{
		VectorCopy( frustum.planes[i].normal, r_meworld.early.planes[i] );
		r_meworld.early.planes[i][3] = -frustum.planes[i].dist - DotProduct( frustum.planes[i].normal, move ) + MEWORLD_EARLY_MOVE;
	}

	// the guard band's edges at the angle from the view's axis less the turn
	const float tx = tanf( DEG2RAD( RI.rvp.fov_x ) * 0.5f ), ty = tanf( DEG2RAD( RI.rvp.fov_y ) * 0.5f );
	const float gx = tanf( atanf( GU_GUARD_X * tx ) - turn ) / tx;
	const float gy = tanf( atanf( GU_GUARD_Y * ty ) - turn ) / ty;

	GU_ClipGetWorldPlanesGuard( r_meworld.early.planes + 8, gx, gy );
	for( int i = 8; i < 12; i++ )
		r_meworld.early.planes[i][3] -= DotProduct( r_meworld.early.planes[i], move ) + MEWORLD_EARLY_MOVE;

	r_meworld.early.ready = true;
}

// whether the walk R_MeWorldEarly posted covers this frame's view
static qboolean R_MeWorldEarlyMatches( void )
{
	int why = 0;
	vec3_t d;

	VectorSubtract( RI.cullorigin, r_meworld.early.org, d );

	if( r_meworld.early.vert_frame != guRender.vert_frame || r_meworld.early.framecount != tr.framecount || r_meworld.early.world != WORLDMODEL )
		why = 1;
	else if( r_meworld.early.visframecount != tr.visframecount )
		why = 2; // another leaf: R_MarkLeaves marked others
	else if( DotProduct( d, d ) > MEWORLD_EARLY_MOVE * MEWORLD_EARLY_MOVE )
		why = 3;
	else if( DotProduct( RI.cull_vforward, r_meworld.early.forward ) < MEWORLD_EARLY_COS || DotProduct( RI.cull_vright, r_meworld.early.right ) < MEWORLD_EARLY_COS
		|| DotProduct( RI.cull_vup, r_meworld.early.up ) < MEWORLD_EARLY_COS )
		why = 4;
	else if( RI.rvp.fov_x != r_meworld.early.fov_x || RI.rvp.fov_y != r_meworld.early.fov_y || R_GetFarClip() != r_meworld.early.farclip
		|| RI.rvp.flags != r_meworld.early.flags || ( RI.frustum.clipFlags|WB_GUARD ) != r_meworld.early.clipflags || r_nocull.value )
		why = 5;
	else if( ws_allslow != r_meworld.early.allslow || ws_skybox != r_meworld.early.skybox || ( ws_numdlit && WORLDMODEL != lm_prebuilt ))
		why = 6;

	return why == 0;
}

/*
=================
R_MeWorldEarly

at the end of a frame: the walk of the next one starts on the ME, from this
frame's view with margins (R_MeWorldCapture). R_MeWorldBegin uses it if the next
view is that close, the main CPU doesn't wait for the walk then (~2 ms a frame
on deluge_beta_v3). Its lists go to the third buffer: the GE may be drawing this
frame and the last one.
=================
*/
void R_MeWorldEarly( void )
{
	meworld_job_t *job = &r_meworld.job;

	if( !r_meworld.early.ready )
		return;
	r_meworld.early.ready = false;

	if( !r_world_me_early.value || !r_meworld.alloc || r_meworld.early.world != WORLDMODEL || !PSP_MeStart( ))
		return;

	// this frame's jobs are done with the job
	R_MeWorldWait();

	r_meworld.early.vert_frame = guRender.vert_frame + 1;
	job->cmds = job->out->cmds[r_meworld.early.vert_frame % MEWORLD_BUFFERS];
	R_MeWorldStamp( job );
	job->framecount = r_meworld.early.framecount;
	job->visframecount = r_meworld.early.visframecount;
	job->allslow = r_meworld.early.allslow;
	job->skybox = r_meworld.early.skybox;
	job->clipflags = r_meworld.early.clipflags;
	job->qerror = r_meworld.early.qerror;
	VectorCopy( r_meworld.early.org, job->org );
	job->time10 = r_meworld.early.time10;
	job->time20 = r_meworld.early.time20;
	memcpy( job->planes, r_meworld.early.planes, sizeof( job->planes ));

	for( int i = 0; i < 12; i++ )
	{
		for( int j = 0; j < 3; j++ )
			job->absplanes[i][j] = fabsf( job->planes[i][j] );
	}

	r_meworld.early.ticket = PSP_MeRunKeep( R_MeWorldJob, job );
}

/*
=================
R_MeWorldBegin

the view, the dlights, the PVS and the fog are set up: the walk starts on the ME
=================
*/
void R_MeWorldBegin( void )
{
	meworld_job_t *job = &r_meworld.job;
	const qboolean early = r_meworld.early.ticket != 0;

	r_meworld.early.ready = false;
	R_MeWorldWait();

	// what it drew in the last frame
	if( r_meworld.alloc && ( guRender.vert_frame & 7 ) == 0 )
	{
		for( int i = 0; i < ARRAYSIZE( MEWORLD_OUT->texused ); i++ )
		{
			for( uint bits = MEWORLD_OUT->texused[i]; bits; bits &= bits - 1 )
				GL_TouchTexture( i * 32 + __builtin_ctz( bits ));
		}
	}

	if( !r_world_me.value || !wn_nodes || !ws_surfs || !FBitSet( RI.rvp.flags, RF_DRAW_WORLD )
		|| FBitSet( RI.rvp.flags, RF_ONLY_CLIENTDRAW|RF_DRAW_OVERVIEW ) || !PSP_MeStart( ))
		return;

	// the GE reads the commands: one view a frame
	if( r_meworld.vert_frame == guRender.vert_frame )
		return;

	// R_BlendLightmaps has nothing to do, or binds the white texture
	RI.currententity = CL_GetEntityByIndex( 0 );
	if( !RI.currententity || !R_HasLightmap() || r_lightmap->value )
		return;

	if( !r_meworld.alloc )
	{
		// tried on this map already (not in r_world_me: it's archived, the next map may fit)
		static const model_t *nomem;
		const size_t outsize = ( sizeof( meworld_out_t ) + 63 ) & ~63;
		const size_t size = outsize + (( ws_numsurfs * sizeof( uint16_t ) + 63 ) & ~63 );

		if( nomem == WORLDMODEL )
			return;

		r_meworld.alloc = memalign( 64, size );
		if( !r_meworld.alloc )
		{
			gEngfuncs.Con_Printf( S_WARN "%s: out of memory, the main CPU walks the world on this map\n", __func__ );
			nomem = WORLDMODEL;
			return;
		}
		nomem = NULL;

		memset( r_meworld.alloc, 0, size );
		job->out = r_meworld.alloc;
		job->visframe = (uint16_t *)((byte *)r_meworld.alloc + outsize );
		r_meworld.visfill = -1;
	}

	r_meworld.vert_frame = guRender.vert_frame;

	R_WorldSurfsBeginWalk();

	// the walk R_MeWorldEarly started (and R_MeWorldWait finished) is good for this view:
	// the brush models' job takes the view from the job
	if( early && R_MeWorldEarlyMatches( ))
	{
		R_MeWorldView( job );
		R_StudioMeYield( 2 );
		r_meworld.begun = true;
		R_MeWorldCapture();
		return;
	}

	job->cmds = job->out->cmds[guRender.vert_frame % MEWORLD_BUFFERS];
	R_MeWorldStamp( job );
	job->framecount = tr.framecount;
	job->visframecount = tr.visframecount;
	job->allslow = ws_allslow;
	job->skybox = ws_skybox;
	job->time10 = (int)( gp_cl->time * 10 );
	job->time20 = (int)( gp_cl->time * 20 );
	R_MeWorldView( job );

	R_StudioMeYield( 2 );
	r_meworld.ticket = PSP_MeRunKeep( R_MeWorldJob, job );
	r_meworld.begun = r_meworld.ticket != 0;
	if( r_meworld.begun )
		R_MeWorldCapture();
}

// the job isn't done yet (R_MeWorldEnd)
qboolean R_MeWorldBusy( void )
{
	return r_meworld.ticket && !PSP_MeDone( r_meworld.ticket );
}

/*
=================
R_MeBrushClassify

runs on the ME (R_StudioMeEarly): whether it can draw the entity, R_DrawStaticBrushModels
with the chains and bs_surfs
=================
*/
qboolean R_MeBrushClassify( const cl_entity_t *e, meworld_brush_t *m )
{
	const model_t *mod = e->model;

	if( mod->surfaces != WORLDMODEL->surfaces || !bs_surfs || mod->firstmodelsurface < bs_first
		|| mod->firstmodelsurface + mod->nummodelsurfaces > bs_first + bs_numsurfs )
		return false;

	// R_StaticBrushModel
	if( !VectorIsNull( e->origin ) || !VectorIsNull( e->angles ) || FBitSet( e->curstate.effects, EF_FULLBRIGHT ) || e->curstate.frame )
		return false;

	// with an effect it may be invisible: CL_FxBlend
	if( e->curstate.rendermode != kRenderNormal && ( e->curstate.rendermode != kRenderTransAlpha || e->curstate.renderfx != kRenderFxNone || e->curstate.renderamt <= 0 ))
		return false;

	for( int j = 0; j < 3; j++ )
	{
		m->mins[j] = bound( -32768, (int)( mod->mins[j] - 1.0f ), 32767 );
		m->maxs[j] = bound( -32768, (int)( mod->maxs[j] + 1.0f ), 32767 );
	}

	// R_MeBrush can't do these: R_DrawStaticBrushModels draws this one
	for( int i = mod->firstmodelsurface - bs_first; i < mod->firstmodelsurface - bs_first + mod->nummodelsurfaces; i++ )
	{
		if( FBitSet( bs_surfs[i].flags, BS_SLOW ))
			return false;
	}

	m->firstsurf = mod->firstmodelsurface;
	m->numsurfs = mod->nummodelsurfaces;
	m->result = e->curstate.rendermode == kRenderNormal ? MEWORLD_BRUSHES : MEWORLD_ABRUSHES;
	m->cullsurfs = true;
	m->watersides = FBitSet( e->curstate.effects, EF_WATERSIDES ) ? true : false;
	m->waterz = mod->mins[2] + 1.0f;
	return true;
}

static void R_MeBrushesAdd( cl_entity_t **list, int count, const mecull_t *cull, int numcull, const uint16_t *order, byte *grouped, qboolean dlights )
{
	meworld_brushjob_t *bjob = &r_meworld.brushjob;

	for( int i = 0; i < count && bjob->numbrushes < MEWORLD_MAXBRUSHES; i++ )
	{
		const int index = order ? order[i] : i;
		const mecull_t *c = &cull[index];

		// the table doesn't have the ones added since
		if( index >= numcull || c->type != MECULL_BRUSH )
			continue;

		grouped[i] = R_GROUPED_ME;
		r_meworld.ents[bjob->numbrushes] = list[i];
		bjob->brushes[bjob->numbrushes++] = c->brush;

		if( dlights )
			R_PushDlightsForBmodel( list[i]->model, tr.dlightframecount, RI.objectMatrix );
	}
}

/*
=================
R_MeBrushesBegin

the static brush models of the draw lists, marked R_GROUPED_ME in grouped, for R_MeBrushesDraw
=================
*/
void R_MeBrushesBegin( byte *solid_grouped, byte *trans_grouped, const uint16_t *trans_order )
{
	meworld_brushjob_t *bjob = &r_meworld.brushjob;
	const mecull_t *cull;
	int numsolid, numtrans;
	qboolean dlights = false;

	if( !r_brushes_me.value || !r_meworld.begun || r_nocull.value || FBitSet( gp_host->features, ENGINE_QUAKE_COMPATIBLE ) || r_showhull->value > 0.0f )
		return;

	if( !( cull = R_StudioMeTable( &numsolid, &numtrans, NULL )))
		return;

	R_LoadIdentity();
	VectorCopy( RI.cullorigin, tr.modelorg );

	for( int i = 0; i < MAX_DLIGHTS && !dlights; i++ )
		dlights = gp_dlights[i].radius != 0.0f;

	bjob->world = &r_meworld.job;
	bjob->front = glState.faceCull == GL_NONE ? 0.0f : glState.faceCull == GL_FRONT ? 1.0f : -1.0f;
	bjob->numbrushes = 0;
	R_MeBrushesAdd( tr.draw_list->solid_entities, numsolid, cull, numsolid, NULL, solid_grouped, dlights );
	R_MeBrushesAdd( tr.draw_list->trans_entities, tr.draw_list->num_trans_entities, cull + MECULL_MAXSOLID, numtrans, trans_order, trans_grouped, dlights );

	if( bjob->numbrushes > 0 )
	{
		r_meworld.brush_ticket = PSP_MeRunKeep( R_MeBrushesJob, bjob );
		r_meworld.brushes = true;
	}
}

static uint R_MeListAddress( uint offset )
{
	return offset ? (uint)( r_meworld.job.cmds + offset ) : 0;
}

static void R_MeCallList( uint *list )
{
	if( *list )
	{
		uint *p = extGuCmdBegin();

		*p++ = ( 16 << 24 ) | (( *list >> 8 ) & 0xf0000 ); // BASE
		*p++ = ( 10 << 24 ) | ( *list & 0xffffff ); // CALL
		extGuCmdEnd( p );
		glState.currentTexture = -1;
	}

	*list = 0;
}

// polygons that need clipping, and the lists
static void R_MeResultUse( const volatile meworld_res_t *res )
{
	const int numclip = res->numclip;

	for( int i = 0; i < numclip; i++ )
	{
		const int surf = res->clip[i];

		ClearBits( ws_surfs[surf].texflags, WS_NOCLIP );
		R_WorldSurfChain( surf );
	}

	r_stats.c_world_polys += res->numpolys;
	r_meworld.texlist = R_MeListAddress( res->texlist );
	r_meworld.lmlist = R_MeListAddress( res->lmlist );
}

/*
=================
R_MeWorldEnd

the rest of the walk for the main CPU; false if it has to walk the world itself
=================
*/
static qboolean R_MeWorldEnd( void )
{
	// R_MeWorldBegin may have taken a finished walk (R_MeWorldEarly)
	if( !r_meworld.begun )
		return false;

	if( R_MeWorldBusy( ))
		gEngfuncs.CL_ExtraUpdate(); // the sound, instead of waiting

	PSP_MeWaitFor( r_meworld.ticket );
	r_meworld.ticket = 0;

	const volatile meworld_out_t *out = MEWORLD_OUT;
	const volatile meworld_res_t *res = &out->res[MEWORLD_WORLD];

	if( res->framecount != (uint32_t)tr.framecount )
		return false;

	const int numslow = res->numslow, numleafs = res->numleafs;

	for( int i = 0; i < numleafs; i++ )
	{
		mleaf_t *pleaf = &WORLDMODEL->leafs[out->leaf[i]];
		gEngfuncs.R_StoreEfrags( &pleaf->efrags, tr.realframecount );
	}

	ws_numslowtextures = 0;
	for( int i = 0; i < numslow; i++ )
		R_WorldSurfSlow( res->slow[i] );

	R_MeResultUse( res );
	ws_skyseen = res->skyseen;
	r_stats.c_world_leafs += res->leafs;
	return true;
}

/*
=================
R_MeBrushesDraw

as R_DrawStaticBrushModels; false if it has to draw them
=================
*/
qboolean R_MeBrushesDraw( void )
{
	if( !r_meworld.brushes )
		return true;

	PSP_MeWaitFor( r_meworld.brush_ticket );
	r_meworld.brush_ticket = 0;
	r_meworld.brushes = false;

	const volatile meworld_out_t *out = MEWORLD_OUT;

	for( int r = MEWORLD_BRUSHES; r <= MEWORLD_ABRUSHES; r++ )
	{
		if( out->res[r].framecount != (uint32_t)tr.framecount )
			return false;
	}

	R_LoadIdentity();
	VectorCopy( RI.cullorigin, tr.modelorg );

	for( int r = MEWORLD_BRUSHES; r <= MEWORLD_ABRUSHES; r++ )
	{
		const volatile meworld_res_t *res = &out->res[r];
		const meworld_brush_t *m = r_meworld.brushjob.brushes;
		const int numslow = res->numslow;
		int first = 0;

		if( !res->numpolys && !numslow && !res->numclip )
			continue;

		while( m[first].result != r )
			first++;

		memset( gl_lms.lightmap_surfaces, 0, sizeof( gl_lms.lightmap_surfaces ));
		gl_lms.dynamic_surfaces = NULL;

		RI.currententity = r_meworld.ents[first];
		RI.currentmodel = RI.currententity->model;
		R_SetRenderMode( RI.currententity );
		GL_SetupFogColorForSurfaces();

		if( gl_polyoffset_bmodels.value )
			GL_PushPolygonOffset( -0.5f, -gl_polyoffset_bmodels.value );

		// not after a frame without a lightmap pass
		memset( ws_lmhead, 0xff, sizeof( ws_lmhead ));

		for( int i = 0; i < numslow; i++ )
		{
			const int surf = res->slow[i], who = res->who[i];

			// R_RenderBrushPoly updates it now, unless dlit
			if( !FBitSet( ws_surfs[surf].texflags, WS_DLIT ))
				ClearBits( ws_surfs[surf].texflags, WS_STALE );

			RI.currententity = r_meworld.ents[who & ~MEWORLD_BACKSIDE];
			RI.currentmodel = RI.currententity->model;
			R_RenderBrushPoly( &WORLDMODEL->surfaces[surf], FBitSet( who, MEWORLD_BACKSIDE ) ? CULL_BACKSIDE : CULL_VISIBLE );
		}

		R_MeResultUse( res );
		R_MeCallList( &r_meworld.texlist );
		R_WorldSurfsDrawTextures();

		DrawDecalsBatch();

		GL_ResetFogColor();
		R_BlendLightmaps();
		R_RenderFullbrights();
		R_RenderDetails();
		r_meworld.lmlist = 0;

		if( gl_polyoffset_bmodels.value )
			GL_PopPolygonOffset();

		sceGuDisable( GU_ALPHA_TEST );
		sceGuAlphaFunc( GU_GREATER, DEFAULT_ALPHATEST, 0xff );
		sceGuDisable( GU_BLEND );
		sceGuDepthMask( GU_FALSE );
	}

	return true;
}

/*
=================
R_WorldSurfChainBrush

a visible surface of a static brush model (R_DrawStaticBrushModels) into the
chains of R_WorldSurfsDrawTextures and R_WorldSurfsDrawLightmaps; false when
it needs R_RenderBrushPoly. R_RenderBrushPoly was ~8 us a surface, ~60 a frame
in a firefight: texture binds, the clip test from the surface's extra data,
the decal and lightmap checks.
=================
*/
static qboolean R_WorldSurfChainBrush( const msurface_t *surf )
{
	gu_wsurf_t *w = R_WorldSurf( surf );

	if( !w || ws_allslow )
		return false;

	if( FBitSet( w->texflags, WS_SLOW|WS_DLIT|WS_DECALS|WS_STALE ))
	{
		// R_RenderBrushPoly updates it now, unless dlit
		if( !FBitSet( w->texflags, WS_DLIT ))
			ClearBits( w->texflags, WS_STALE );
		return false;
	}

	const vec3_t mins = { w->mins[0], w->mins[1], w->mins[2] };
	const vec3_t maxs = { w->maxs[0], w->maxs[1], w->maxs[2] };

	if( GU_ClipBoxInside( mins, maxs ))
		SetBits( w->texflags, WS_NOCLIP );
	else ClearBits( w->texflags, WS_NOCLIP );

	const int i = w - ws_surfs;
	const int tex = w->texflags & WS_TEXTURE;
	if( ws_texhead[tex] == WS_NONE )
		ws_textures[ws_numtextures++] = tex;
	w->texnext = ws_texhead[tex];
	ws_texhead[tex] = i;
	w->lmnext = ws_lmhead[w->lightmap];
	ws_lmhead[w->lightmap] = i;
	return true;
}
#endif // XASH_PSP

/*
=============
EmitWaterPolys

Does a water warp on the pre-fragmented gu_poly_t chain
=============
*/
#if XASH_PSP
/*
=============
static water

Warping water is ~4.5 us a polygon on the CPU (fight1: 328 polygons, 1.5 ms a
frame). With r_water_static the polygons of a water surface are built once
into one triangle list that the GE draws as is, with the texture scrolling
instead of the warp: no ripples, but still clearly water. Polygons that need
clipping this frame (near or behind the eye) and surfaces seen from below are
warped as before. fight1: ~310 of ~328 polygons a frame from the lists,
26.3 -> 26.65 fps.
=============
*/
#include <pspkernel.h>
#include <malloc.h>
static CVAR_DEFINE_AUTO( r_water_static, "1", FCVAR_ARCHIVE, "draw water without the warp, from prebuilt vertex lists (faster)" );

typedef struct
{
	const msurface_t	*surf;
	const model_t	*world;
	gu_vert_t		*verts;
	int		numverts;
	int		numpolys;
	int		*first;		// per poly: its first vertex in verts
	byte		*drawn;		// per poly, this call: drawn from verts
} staticwater_t;

#define STATICWATER_SIZE 256
static staticwater_t r_staticwater[STATICWATER_SIZE];

// called on a new map
void R_StaticWaterClear( void )
{
	static qboolean registered;

	if( !registered )
	{
		gEngfuncs.Cvar_RegisterVariable( &r_water_static );
		registered = true;
	}

	for( int i = 0; i < STATICWATER_SIZE; i++ )
	{
		free( r_staticwater[i].verts );
		free( r_staticwater[i].first );
	}
	memset( r_staticwater, 0, sizeof( r_staticwater ));
}

static staticwater_t *R_StaticWaterGet( const msurface_t *warp )
{
	uint h = ((uint)(uintptr_t)warp * 2654435761u ) >> 24;

	for( int n = 0; n < 8; n++, h = ( h + 1 ) & ( STATICWATER_SIZE - 1 ))
	{
		staticwater_t *w = &r_staticwater[h];

		if( w->surf == warp && w->world == WORLDMODEL )
			return w->verts ? w : NULL;
		if( w->surf && w->world == WORLDMODEL )
			continue;

		// free slot (or one of another map): build
		free( w->verts );
		free( w->first );
		memset( w, 0, sizeof( *w ));
		w->surf = warp;
		w->world = WORLDMODEL;

		int count = 0, polys = 0;
		for( const gu_poly_t *p = GU_POLYS( warp ); p; p = p->next, polys++ )
		{
			if( p->numverts >= 3 )
				count += ( p->numverts - 2 ) * 3;
		}
		if( !count )
			return NULL;
		w->verts = memalign( 16, count * sizeof( gu_vert_t ));
		w->first = malloc(( polys + 1 ) * sizeof( int ) + polys );
		if( !w->verts || !w->first )
		{
			free( w->verts );
			free( w->first );
			w->verts = NULL;
			w->first = NULL;
			return NULL;
		}
		w->numpolys = polys;
		w->drawn = (byte *)( w->first + polys + 1 );

		int pi = 0;
		for( const gu_poly_t *p = GU_POLYS( warp ); p; p = p->next, pi++ )
		{
			w->first[pi] = w->numverts;
			for( int i = 2; i < p->numverts; i++ )
			{
				const gu_vert_t *fan[3] = { &p->verts[0], &p->verts[i - 1], &p->verts[i] };
				for( int k = 0; k < 3; k++ )
				{
					gu_vert_t *o = &w->verts[w->numverts++];
					o->uv[0] = fan[k]->uv[0] * ( 1.0f / SUBDIVIDE_SIZE );
					o->uv[1] = fan[k]->uv[1] * ( 1.0f / SUBDIVIDE_SIZE );
					VectorCopy( fan[k]->xyz, o->xyz );
				}
			}
		}
		w->first[pi] = w->numverts;
		sceKernelDcacheWritebackRange( w->verts, w->numverts * sizeof( gu_vert_t ));
		return w;
	}
	return NULL; // table full around this slot
}

// draws the polys of the surface that need no clipping; true when that was all of them,
// else *drawn flags (per poly) the ones done
static qboolean R_StaticWaterDraw( msurface_t *warp, qboolean reverse, const byte **drawn )
{
	*drawn = NULL;
	if( reverse || !r_water_static.value )
		return false;

	staticwater_t *w = R_StaticWaterGet( warp );
	if( !w )
		return false;

	// the scroll stands in for the warp
	const float t = (float)fmod( gp_cl->time * 0.05, 1.0 );
	int run = -1, left = 0, pi = 0;

	GL_ResetFogColor();
	sceGuTexOffset( t, t * 0.5f );

	// the whole surface inside the clip planes: one draw, no test per polygon
	// (the polygons of a big water surface are ~1 us each to test)
	if( warp->info && GU_ClipBoxInside( warp->info->mins, warp->info->maxs ))
	{
		extGuDrawArrayBatched( GU_TRIANGLES, GU_TEXTURE_32BITF | GU_VERTEX_32BITF, w->numverts, w->verts );
		sceGuTexOffset( 0.0f, 0.0f );
		GL_SetupFogColorForSurfaces();
		return true;
	}

	for( gu_poly_t *p = GU_POLYS( warp ); p; p = p->next, pi++ )
	{
		const qboolean ok = p->numverts < 3 || !GU_ClipIsRequired( p->verts, p->numverts );

		w->drawn[pi] = ok;
		if( ok )
		{
			if( run < 0 )
				run = pi;
			continue;
		}
		left++;
		if( run >= 0 && w->first[pi] > w->first[run] )
			extGuDrawArrayBatched( GU_TRIANGLES, GU_TEXTURE_32BITF | GU_VERTEX_32BITF, w->first[pi] - w->first[run], w->verts + w->first[run] );
		run = -1;
	}
	if( run >= 0 && w->first[pi] > w->first[run] )
		extGuDrawArrayBatched( GU_TRIANGLES, GU_TEXTURE_32BITF | GU_VERTEX_32BITF, w->first[pi] - w->first[run], w->verts + w->first[run] );
	sceGuTexOffset( 0.0f, 0.0f );
	GL_SetupFogColorForSurfaces();

	if( !left )
		return true;
	*drawn = w->drawn;
	return false;
}
#endif

static void EmitWaterPolys( msurface_t *warp, qboolean reverse )
{
	float	waveHeight;

	if( !warp->polys || GU_WorldListFull( )) return;

#if XASH_PSP
	const byte *drawn;
	int pi = 0;

	if( R_StaticWaterDraw( warp, reverse, &drawn ))
		return;
#endif

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
#if XASH_PSP
		if( drawn && drawn[pi++] )
			continue;
#endif
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
		GU_DrawPoly( p, &p->verts[p->numverts] );

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
#if XASH_PSP
		if( gl_lms.lightmap_surfaces[i] || ws_lmhead[i] != WS_NONE )
#else
		if( gl_lms.lightmap_surfaces[i] )
#endif
		{
			GL_Bind( XASH_TEXTURE0, tr.lightmapTextures[i] );

			for( msurface_t *surf = gl_lms.lightmap_surfaces[i]; surf != NULL; surf = surf->info->lightmapchain )
				DrawGLPolyChain( GU_POLYS( surf ), 0.0f, 0.0f );
#if XASH_PSP
			R_WorldSurfsDrawLightmaps( i );
#endif
		}
	}

#if XASH_PSP
	R_MeCallList( &r_meworld.lmlist );
#endif

	// render dynamic lightmaps
	if( r_dynamic->value && gl_lms.dynamic_surfaces )
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

			if( LM_AllocDynamic( smax, tmax, surf->info ))
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
				if( !LM_AllocDynamic( smax, tmax, surf->info ))
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

#if XASH_PSP
	if( lm_prebuilt && lm_prebuilt == WORLDMODEL )
		return false; // static, see R_PrebuildLightmaps
#endif

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

	// the clip test result for this surface's other passes in this frame too
	// (fullbrights, details, lightmaps): all of them come through here first
	gu_poly_t *p = GU_POLYS( fa );
	if( p )
	{
		if( GU_ClipBoxInside( fa->info->mins, fa->info->maxs ))
			SetBits( p->flags, GU_POLY_NOCLIP );
		else ClearBits( p->flags, GU_POLY_NOCLIP );
	}

	R_RenderFullbrightForSurface( fa, t );
	R_RenderDetailsForSurface( fa, t );
	DrawGLPoly( p, 0.0f, 0.0f );
	R_RenderDecalsForSurface( fa, cull_type );
	R_RenderLightmapForSurface( fa );
}

#if XASH_PSP
/*
================
R_DrawConveyors

R_RenderBrushPoly for plain conveyor surfaces of RI.currententity, in their order:
the offset is set once per texture, the lightmaps are only chained when
R_BlendLightmaps draws them (~17 us a surface before, deluge_beta_v3 has 94 in view)
================
*/
static void R_DrawConveyors( msurface_t **surfs, int count )
{
	const qboolean lightmap = R_HasLightmap();
	int texture = -1;

	for( int i = 0; i < count; i++ )
	{
		msurface_t *fa = surfs[i];
		texture_t *t = R_TextureAnimation( fa );
		gu_poly_t *p = GU_POLYS( fa );

		r_stats.c_world_polys++;
		if( t->gl_texturenum != texture )
		{
			float s, tc;

			texture = t->gl_texturenum;
			GL_Bind( XASH_TEXTURE0, texture );
			R_ConveyorOffset( &s, &tc );
			sceGuTexOffset( s, tc );
		}

		if( GU_ClipBoxInside( fa->info->mins, fa->info->maxs ))
			SetBits( p->flags, GU_POLY_NOCLIP );
		else ClearBits( p->flags, GU_POLY_NOCLIP );

		R_RenderFullbrightForSurface( fa, t );
		R_RenderDetailsForSurface( fa, t );
		GU_DrawPoly( p, p->verts );
		if( lightmap )
			R_RenderLightmapForSurface( fa );
	}

	sceGuTexOffset( 0.0f, 0.0f );
}
#endif

static void R_DrawTextureChain( int i )
{
	texture_t *t = WORLDMODEL->textures[i];
	if( !t )
		return;

	msurface_t *s = t->texturechain;

	if( !s || ( i == tr.skytexturenum ))
		return;

	if(( s->flags & SURF_DRAWTURB ) && gp_movevars->wateralpha < 1.0f )
	{
		R_AddToSeparatePass( &draw_wateralpha, i );
		return;	// draw translucent water later
	}

	if( FBitSet( gp_host->features, ENGINE_QUAKE_COMPATIBLE ) && FBitSet( s->flags, SURF_TRANSPARENT ))
	{
		R_AddToSeparatePass( &draw_alpha_surfaces, i );
		return;	// draw transparent surfaces later
	}

	for( ; s != NULL; s = s->texturechain )
		R_RenderBrushPoly( s, CULL_VISIBLE );
	t->texturechain = NULL;
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

#if XASH_PSP
	if( ws_numslowtextures >= 0 )
	{
		// each texture is a cache miss, ~100 of them a frame
		for( int k = 0; k < ws_numslowtextures; k++ )
		{
			for( int i = 0; i < WORLDMODEL->numtextures; i++ )
			{
				if( WORLDMODEL->textures[i] == ws_slowtextures[k] )
				{
					R_DrawTextureChain( i );
					break;
				}
			}
		}
		ws_numslowtextures = -1;
	}
	else
#endif
	for( int i = 0; i < WORLDMODEL->numtextures; i++ )
		R_DrawTextureChain( i );

#if XASH_PSP
	R_MeCallList( &r_meworld.texlist );
	R_WorldSurfsDrawTextures();
#endif
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

	VectorAverage( surf1->info->mins, surf1->info->maxs, org1 );
	VectorAverage( surf2->info->mins, surf2->info->maxs, org2 );
	VectorAdd( RI.currententity->origin, org1, org1 );
	VectorAdd( RI.currententity->origin, org2, org2 );

	// compare by plane dists
	float len1 = DotProduct( org1, RI.vforward ) - RI.viewplanedist;
	float len2 = DotProduct( org2, RI.vforward ) - RI.viewplanedist;

	if( len1 > len2 )
		return -1;
	if( len1 < len2 )
		return 1;

	return 0;
}

#if XASH_PSP
typedef struct
{
	float		dist;
	sortedface_t	face;
} sortkey_t;

static int R_SortKeyCompare( const void *a, const void *b )
{
	const float d1 = ((const sortkey_t *)a)->dist, d2 = ((const sortkey_t *)b)->dist;

	return d1 > d2 ? -1 : d1 < d2 ? 1 : 0;
}

// R_SurfaceCompare's order with each distance computed once: qsort read the
// surfaces' boxes ~8 times each (deluge_beta_v3, 8 translucent brush models,
// 0.56 -> 0.46 ms with their culling)
static void R_SortSurfacesBackToFront( sortedface_t *faces, int count )
{
	static sortkey_t keys[512];

	if( count > ARRAYSIZE( keys ))
	{
		qsort( faces, count, sizeof( *faces ), R_SurfaceCompare );
		return;
	}

	for( int i = 0; i < count; i++ )
	{
		const mextrasurf_t *info = faces[i].surf->info;
		vec3_t org;

		VectorAverage( info->mins, info->maxs, org );
		VectorAdd( RI.currententity->origin, org, org );
		keys[i].dist = DotProduct( org, RI.vforward ) - RI.viewplanedist;
		keys[i].face = faces[i];
	}

	qsort( keys, count, sizeof( *keys ), R_SortKeyCompare );

	for( int i = 0; i < count; i++ )
		faces[i] = keys[i].face;
}
#endif

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

static int R_SortBrushModelSurfaces( cl_entity_t *e, model_t *clmodel, vec3_t mins, int first )
{
	qboolean quake_compatible = FBitSet( gp_host->features, ENGINE_QUAKE_COMPATIBLE ) ? true : false;
	sortedface_t *draw_surfaces = gpGlobals->draw_surfaces + first;
	const int max_surfaces = gpGlobals->max_surfaces - first;
	int num_sorted = 0;
	const gu_bsurf_t *bs = NULL;

	// R_CullSurface through bs_surfs: for a brush model of the world BSP that
	// isn't the world, with culling on and no overview
	if( bs_surfs && clmodel->surfaces == WORLDMODEL->surfaces && e != CL_GetEntityByIndex( 0 )
		&& clmodel->firstmodelsurface >= bs_first && clmodel->firstmodelsurface + clmodel->nummodelsurfaces <= bs_first + bs_numsurfs
		&& !r_nocull.value && !FBitSet( RI.rvp.flags, RF_DRAW_OVERVIEW ))
		bs = &bs_surfs[clmodel->firstmodelsurface - bs_first];

	// only static ents can be culled by frustum
	uint clipflags = R_StaticEntity( e ) ? RI.frustum.clipFlags : 0;

	// the surfaces of a model inside a plane are too
	if( bs && clipflags )
	{
		for( int i = 0; i < FRUSTUM_PLANES; i++ )
		{
			const mplane_t *p = &RI.frustum.planes[i];
			vec3_t near;

			if( !FBitSet( clipflags, BIT( i )))
				continue;

			for( int j = 0; j < 3; j++ )
				near[j] = p->normal[j] >= 0.0f ? clmodel->mins[j] : clmodel->maxs[j];

			if( DotProduct( p->normal, near ) >= p->dist )
				ClearBits( clipflags, BIT( i ));
		}
	}
	const float front = glState.faceCull == GL_FRONT ? 1.0f : -1.0f;

	for( int i = 0; i < clmodel->nummodelsurfaces; i++ )
	{
		msurface_t *psurf = &clmodel->surfaces[clmodel->firstmodelsurface + i];

		if( bs && !FBitSet( bs[i].flags, BS_SLOW ))
		{
			const gu_bsurf_t *b = &bs[i];
			const qboolean turb = FBitSet( b->flags, BS_TURB ) ? true : false;
			int cull_type = CULL_VISIBLE;

			if( turb && !quake_compatible )
			{
				if( !FBitSet( b->flags, BS_PLANEZ ) && !FBitSet( e->curstate.effects, EF_WATERSIDES ))
					continue;

				if( mins[2] + 1.0f >= ( FBitSet( b->flags, BS_PLANEBACK ) ? -b->dist : b->dist ))
					continue;
			}

			if( glState.faceCull != GL_NONE && front * ( DotProduct( tr.modelorg, b->normal ) - b->dist ) <= BACKFACE_EPSILON )
			{
				// kept for water and the decals of kRenderTransTexture
				if( !turb && !( psurf->pdecals && e->curstate.rendermode == kRenderTransTexture ))
					continue;
				cull_type = CULL_BACKSIDE;
			}
			else if( clipflags )
			{
				const vec3_t mins = { b->mins[0], b->mins[1], b->mins[2] };
				const vec3_t maxs = { b->maxs[0], b->maxs[1], b->maxs[2] };

				if( GL_FrustumCullBox( &RI.frustum, mins, maxs, clipflags ))
					continue;
			}

			if( num_sorted < max_surfaces )
			{
				draw_surfaces[num_sorted].surf = psurf;
				draw_surfaces[num_sorted].cull = cull_type;
				num_sorted++;
			}
			continue;
		}

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

		if( num_sorted < max_surfaces )
		{
			draw_surfaces[num_sorted].surf = psurf;
			draw_surfaces[num_sorted].cull = cull_type;
			num_sorted++;
		}
	}

	// sort faces if needs
	if( !FBitSet( clmodel->flags, MODEL_LIQUID ) && e->curstate.rendermode == kRenderTransTexture && !gl_nosort.value )
#if XASH_PSP
		R_SortSurfacesBackToFront( draw_surfaces, num_sorted );
#else
		qsort( draw_surfaces, num_sorted, sizeof( sortedface_t ), R_SurfaceCompare );
#endif

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

	int num_sorted = R_SortBrushModelSurfaces( e, clmodel, mins, 0 );

	// draw bmodels with a polyoffset to avoid flickering when they are too close to world
	if( gl_polyoffset_bmodels.value )
		GL_PushPolygonOffset( -0.5f, -gl_polyoffset_bmodels.value );

#if XASH_PSP
	// the plain surfaces through the world's chains, as R_DrawStaticBrushModels: ~10 us
	// a surface less. A translucent model's own surfaces aren't drawn back to front then.
	const qboolean chains = ws_surfs && !r_fullbright->value && WORLDMODEL->lightdata && clmodel->surfaces == WORLDMODEL->surfaces
		&& !FBitSet( e->curstate.effects, EF_FULLBRIGHT ) && !e->curstate.frame;

	// the world's lightmap pass emptied them; not after a frame without one
	if( chains )
		memset( ws_lmhead, 0xff, sizeof( ws_lmhead ));

	static msurface_t *conveyors[512];
	int numconveyors = 0;
#endif

	// draw sorted translucent surfaces
	for( int i = 0; i < num_sorted; i++ )
	{
		msurface_t *ps = gpGlobals->draw_surfaces[i].surf;
#if XASH_PSP
		if( chains && gpGlobals->draw_surfaces[i].cull == CULL_VISIBLE && !FBitSet( ps->flags, SURF_DRAWTURB ))
		{
			if( FBitSet( ps->flags, SURF_CONVEYOR ) && !FBitSet( ps->flags, SURF_DRAWTILED|SURF_DRAWSKY ) && !ps->pdecals
				&& GU_POLYS( ps ) && numconveyors < ARRAYSIZE( conveyors ))
			{
				conveyors[numconveyors++] = ps;
				continue;
			}
			if( R_WorldSurfChainBrush( ps ))
				continue;
		}
#endif
		R_RenderBrushPoly( ps, gpGlobals->draw_surfaces[i].cull );
	}
#if XASH_PSP
	if( numconveyors )
		R_DrawConveyors( conveyors, numconveyors );
	if( chains )
		R_WorldSurfsDrawTextures();
#endif

	if( e->curstate.rendermode == kRenderTransColor )
		sceGuEnable( GU_TEXTURE_2D );

	DrawDecalsBatch();

	GL_ResetFogColor();
	R_BlendLightmaps();
	R_RenderFullbrights();
	R_RenderDetails();
#if XASH_PSP
	// without a lightmap pass (R_HasLightmap) they're still chained
	if( chains )
		memset( ws_lmhead, 0xff, sizeof( ws_lmhead ));
#endif

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
=================
R_StaticBrushModel

brush models that sit at the world origin unrotated (func_wall and friends) and
draw without blending: R_DrawStaticBrushModels draws a row of them in one go
=================
*/
qboolean R_StaticBrushModel( const cl_entity_t *e )
{
	if( !e->model || e->model->type != mod_brush || !FBitSet( RI.rvp.flags, RF_DRAW_WORLD ))
		return false;

	if( !VectorIsNull( e->origin ) || !VectorIsNull( e->angles ))
		return false;

	if( FBitSet( gp_host->features, ENGINE_QUAKE_COMPATIBLE ) || r_showhull->value > 0.0f )
		return false;

	return e->curstate.rendermode == kRenderNormal || e->curstate.rendermode == kRenderTransAlpha;
}

/*
=================
R_DrawStaticBrushModels

R_DrawBrushModel for models that pass R_StaticBrushModel and share a render mode:
they need no model matrix or frustum, and the render state, decal, lightmap,
fullbright and detail passes are set up once for all of them. They write depth
and don't blend, so drawing them before the translucent entities sorted
between them gives the same picture.
=================
*/
void R_DrawStaticBrushModels( cl_entity_t **ents, int count )
{
	static struct { cl_entity_t *e; int first; } runs[MAX_VISIBLE_PACKET];
	int num_runs = 0, num_sorted = 0;

	if( count <= 0 )
		return;

	cl_entity_t *e0 = ents[0];

	memset( gl_lms.lightmap_surfaces, 0, sizeof( gl_lms.lightmap_surfaces ));
	gl_lms.dynamic_surfaces = NULL;

	R_LoadIdentity();
	VectorCopy( RI.cullorigin, tr.modelorg );

	for( int i = 0; i < count && num_runs < MAX_VISIBLE_PACKET; i++ )
	{
		cl_entity_t *e = ents[i];
		model_t *clmodel = e->model;

		// R_DrawStaticBrushGroup keeps the sum of nummodelsurfaces within max_surfaces
		if( num_sorted + clmodel->nummodelsurfaces > gpGlobals->max_surfaces )
			break;

		if( R_CullBox( clmodel->mins, clmodel->maxs ))
			continue;

		e->visframe = tr.realframecount; // visible
		R_PushDlightsForBmodel( clmodel, tr.dlightframecount, RI.objectMatrix );

		RI.currententity = e;
		RI.currentmodel = clmodel;
		const int n = R_SortBrushModelSurfaces( e, clmodel, clmodel->mins, num_sorted );
		if( !n )
			continue;

		runs[num_runs].e = e;
		runs[num_runs].first = num_sorted;
		num_runs++;
		num_sorted += n;
	}

	if( !num_runs )
		return;

	R_SetRenderMode( e0 );
	GL_SetupFogColorForSurfaces();

	if( gl_polyoffset_bmodels.value )
		GL_PushPolygonOffset( -0.5f, -gl_polyoffset_bmodels.value );

	// the surfaces that allow it through the world's chains: lightmapped (R_BlendLightmaps
	// below checks the last entity only), the first texture frames. R_StaticBrushModel
	// lets kRenderNormal and kRenderTransAlpha through, R_SetRenderMode set the alpha test.
	qboolean chains = ws_surfs && !r_fullbright->value && WORLDMODEL->lightdata;

	for( int r = 0; r < num_runs && chains; r++ )
	{
		if( FBitSet( runs[r].e->curstate.effects, EF_FULLBRIGHT ) || runs[r].e->curstate.frame )
			chains = false;
	}

	// the world's lightmap pass emptied them; not after a frame without one
	if( chains )
		memset( ws_lmhead, 0xff, sizeof( ws_lmhead ));

	for( int r = 0; r < num_runs; r++ )
	{
		const int end = r + 1 < num_runs ? runs[r + 1].first : num_sorted;

		// texture animation reads the entity's frame
		RI.currententity = runs[r].e;
		RI.currentmodel = RI.currententity->model;

		for( int i = runs[r].first; i < end; i++ )
		{
			msurface_t *surf = gpGlobals->draw_surfaces[i].surf;
			const int cull = gpGlobals->draw_surfaces[i].cull;

			if( !chains || cull != CULL_VISIBLE || !R_WorldSurfChainBrush( surf ))
				R_RenderBrushPoly( surf, cull );
		}
	}

	if( chains )
		R_WorldSurfsDrawTextures();

	DrawDecalsBatch();

	GL_ResetFogColor();
	R_BlendLightmaps();
	R_RenderFullbrights();
	R_RenderDetails();

	if( gl_polyoffset_bmodels.value )
		GL_PopPolygonOffset();

	sceGuDisable( GU_ALPHA_TEST );
	sceGuAlphaFunc( GU_GREATER, DEFAULT_ALPHATEST, 0xff );
	sceGuDisable( GU_BLEND );
	sceGuDepthMask( GU_FALSE );
}

/*
=============================================================

	WORLD MODEL

=============================================================
*/
#if XASH_PSP
/*
================
R_WorldNodesWalk

R_RecursiveWorldNode through wn_nodes, for a node in the PVS
================
*/
static void R_WorldNodesWalk( int n, uint clipflags )
{
	const uint16_t visframe = tr.visframecount;

	for( ;; )
	{
		const gu_wnode_t *w = &wn_nodes[n];

		if( w->type == WN_SOLID )
			return;

		if( clipflags )
		{
			const int left = R_WorldBoxClip( w->mins, clipflags );

			if( left < 0 )
				return;
			clipflags = left;
		}

		if( w->type == WN_LEAF )
		{
			const uint16_t *mark = &wn_marks[w->firstmark];
			const uint16_t framecount = tr.framecount;

			for( int i = 0; i < w->child[0]; i++ )
				ws_visframe[mark[i]] = framecount;

			// deal with model fragments in this leaf
			mleaf_t *pleaf = &WORLDMODEL->leafs[w->child[1]];
			if( pleaf->efrags )
				gEngfuncs.R_StoreEfrags( &pleaf->efrags, tr.realframecount );

			r_stats.c_world_leafs++;
			return;
		}

		// find which side of the node we are on
		const float dot = ( w->type < 3 ? tr.modelorg[w->type] : DotProduct( tr.modelorg, WORLDMODEL->planes[w->plane].normal )) - w->dist;
		const int side = ( dot >= 0.0f ) ? 0 : 1;

		// recurse down the children, front side first
		if( wn_visframe[w->child[side]] == visframe )
			R_WorldNodesWalk( w->child[side], clipflags );

		if( w->numsurf )
			R_WorldSurfsCull( w->firstsurf, w->numsurf, clipflags );

		// recurse down the back side
		n = w->child[!side];
		if( wn_visframe[n] != visframe )
			return;
	}
}
#endif

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

#if XASH_PSP
		if( ws_surfs )
		{
			const msurface_t *surfaces = WORLDMODEL->surfaces;
			const uint16_t visframe = tr.framecount;

			for( int i = 0; i < pleaf->nummarksurfaces; i++ )
				ws_visframe[mark[i] - surfaces] = visframe;
		}
		else
#endif
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
#if XASH_PSP
	if( ws_surfs )
		R_WorldSurfsCull( firstsurface, numsurfaces, clipflags|WB_GUARD );
	else
#endif
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
	// their passes empty them (they're 12 KB, most of the data cache)
	if( R_SeparatePassActive( &draw_fullbrights ))
		memset( fullbright_surfaces, 0, sizeof( fullbright_surfaces ));
	if( R_SeparatePassActive( &draw_details ))
		memset( detail_surfaces, 0, sizeof( detail_surfaces ));

	gl_lms.dynamic_surfaces = NULL;
	sceGuDisable( GU_ALPHA_TEST );
	sceGuDisable( GU_BLEND );
	tr.blend = 1.0f;

	R_ClearSkyBox ();
#if XASH_PSP
	const qboolean walked = R_MeWorldEnd();

	if( !walked && ws_surfs )
		R_WorldSurfsBeginWalk();
#endif

	double start = gEngfuncs.pfnTime();
#if XASH_PSP
	if( walked )
		; // on the Media Engine
	else
#endif
	if( FBitSet( RI.rvp.flags, RF_DRAW_OVERVIEW ))
		R_DrawWorldTopView( WORLDMODEL->nodes, RI.frustum.clipFlags );
#if XASH_PSP
	else if( wn_nodes )
	{
		if( wn_visframe[0] == (uint16_t)tr.visframecount )
			R_WorldNodesWalk( 0, ( r_nocull.value ? 0 : RI.frustum.clipFlags )|WB_GUARD );
	}
#endif
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

		qboolean sky = skychain != NULL;
#if XASH_PSP
		if( ws_skyseen )
		{
			// whole faces, see R_WorldSurfsCull
			for( int i = 0; i < SKYBOX_MAX_SIDES; i++ )
			{
				RI.skyMins[0][i] = RI.skyMins[1][i] = -1.0f;
				RI.skyMaxs[0][i] = RI.skyMaxs[1][i] = 1.0f;
			}
			ws_skyseen = false;
			sky = true;
		}
#endif
		if( sky )
			R_DrawSkyBox();
	}

	end = gEngfuncs.pfnTime();

	r_stats.t_world_draw = end - start;
	tr.num_draw_decals = 0;
	skychain = NULL;
#if XASH_PSP
	r_meworld.lmlist = 0;

	// not drawn when the world has no lightmap pass, not for brush models either
	memset( ws_lmhead, 0xff, sizeof( ws_lmhead ));
#endif

	gEngfuncs.R_DrawWorldHull();
}

#if XASH_PSP
static byte *mark_lastvis; // the PVS R_MarkLeaves marked last, of mark_world
static const model_t *mark_world;

// true if RI.visbytes is what was marked last
static qboolean R_MarkLeavesSame( qboolean novis )
{
	const size_t size = ( WORLDMODEL->numleafs + 7 ) >> 3;

	if( novis || !wn_nodes )
	{
		mark_world = NULL;
		return false;
	}

	if( mark_world == WORLDMODEL && mark_lastvis && !memcmp( mark_lastvis, RI.visbytes, size ))
		return true;

	if( mark_world != WORLDMODEL )
	{
		free( mark_lastvis );
		mark_lastvis = malloc( size );
		mark_world = mark_lastvis ? WORLDMODEL : NULL;
	}

	if( mark_lastvis )
		memcpy( mark_lastvis, RI.visbytes, size );
	return false;
}
#endif

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

	if( r_novis.value || FBitSet( RI.rvp.flags, RF_DRAW_OVERVIEW ) || !RI.viewleaf || !WORLDMODEL->visdata )
		novis = true;

	gEngfuncs.R_FatPVS( RI.rvp.vieworigin, r_pvs_radius->value, RI.visbytes, false, novis );
	if( force && !novis )
		gEngfuncs.R_FatPVS( test, r_pvs_radius->value, RI.visbytes, true, novis );

#if XASH_PSP
	// the same leaves are visible from the new one (often, in a room): the marks
	// hold, and so does the walk R_MeWorldEarly started for this frame
	if( R_MarkLeavesSame( novis ))
	{
		RI.oldviewleaf = RI.viewleaf;
		return;
	}
#endif

	RI.oldviewleaf = RI.viewleaf;
	tr.visframecount++;

#if XASH_PSP
	if( wn_nodes && ( tr.visframecount & 0xffff ) == 0 )
	{
		// wn_nodes keep the low bits: none matches a new count
		for( int i = 0; i < wn_numnodes; i++ )
			wn_visframe[i] = 0;
		tr.visframecount++;
	}
#endif

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

#if XASH_PSP
			if( wn_nodes )
			{
				const uint16_t visframe = tr.visframecount;

				for( int n = wn_leafnode[i + 1]; n != WS_NONE && wn_visframe[n] != visframe; n = wn_nodes[n].parent )
					wn_visframe[n] = visframe;
			}
#endif
		}
	}

#if XASH_PSP
	R_PolysForPVS();
#endif
}

#if XASH_PSP
static int lm_prebuiltpages;
#endif

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

#if XASH_PSP
	if( loadmodel == lm_prebuilt )
		return;
#endif

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

#if XASH_PSP
/*
==================
R_PrebuildLightmaps

the light of a big map takes megabytes. Its lightmaps are built as soon as it's
loaded, after that only the light of the floors is kept, for the models on them. The
light styles and dynamic lights don't change its lightmaps then (as with r_dynamic 0).
==================
*/
#define LM_PREBUILD_SIZE	( 1024 * 1024 )

static int R_SurfaceLightSize( const msurface_t *surf )
{
	const int sample_size = gEngfuncs.Mod_SampleSizeForFace( surf );
	int styles = 0;

	while( styles < MAXLIGHTMAPS && surf->styles[styles] != 255 )
		styles++;

	return ( surf->info->lightextents[0] / sample_size + 1 ) * ( surf->info->lightextents[1] / sample_size + 1 ) * styles;
}

static qboolean R_SurfaceIsFloor( const msurface_t *surf )
{
	if( !surf->plane )
		return false;

	const float z = FBitSet( surf->flags, SURF_PLANEBACK ) ? -surf->plane->normal[2] : surf->plane->normal[2];

	return z > 0.0f && !FBitSet( surf->flags, SURF_DRAWTILED|SURF_DRAWTURB );
}

// the switchable lights (styles 32 and up) that the map starts dark: off
static void R_PrebuildDarkStyles( const model_t *mod )
{
	char *p = mod->entities;
	char token[256], key[64];

	if( !p )
		return;

	while(( p = COM_ParseFile( p, token, sizeof( token ))) && token[0] == '{' )
	{
		qboolean light = false;
		int style = 0, flags = 0;

		while(( p = COM_ParseFile( p, key, sizeof( key ))) && key[0] != '}' )
		{
			if( !( p = COM_ParseFile( p, token, sizeof( token ))))
				return;

			if( !Q_strcmp( key, "classname" ))
				light = !Q_strncmp( token, "light", 5 );
			else if( !Q_strcmp( key, "style" ))
				style = Q_atoi( token );
			else if( !Q_strcmp( key, "spawnflags" ))
				flags = Q_atoi( token );
		}

		if( !p )
			return;

		// SF_LIGHT_START_OFF
		if( light && style >= 32 && style < MAX_LIGHTSTYLES && FBitSet( flags, 1 ))
			g_lightstylevalue[style] = 0;
	}
}

void R_PrebuildLightmaps( model_t *mod )
{
	int total = 0, kept = 0;

	lm_prebuilt = NULL;

	if( !mod->lightdata )
		return;

	for( int i = 0; i < mod->numsurfaces; i++ )
	{
		const msurface_t *surf = &mod->surfaces[i];

		if( !surf->samples )
			continue;

		total += R_SurfaceLightSize( surf );
		if( R_SurfaceIsFloor( surf ))
			kept += R_SurfaceLightSize( surf );
	}

	if( total * sizeof( color24 ) < LM_PREBUILD_SIZE || !kept )
		return;

	tr.block_size = BLOCK_SIZE_DEFAULT;
	gl_lms.current_lightmap_texture = 0;

	// the server's styles aren't here yet: all of them normal ('m'), but the
	// switchable lights as the map starts them (deluge_beta_v3's coloured
	// lights of locked doors, off at first, lit their rooms)
	for( int i = 0; i < MAX_LIGHTSTYLES; i++ )
		g_lightstylevalue[i] = ( 'm' - 'a' ) * 22;
	R_PrebuildDarkStyles( mod );

	LM_InitBlock();
	for( int i = 0; i < mod->numsurfaces; i++ )
		GL_CreateSurfaceLightmap( &mod->surfaces[i], mod );
	LM_UploadBlock( false );

	color24 *out = Mem_Malloc( mod->mempool, kept * sizeof( *out ));
	int ofs = 0;

	for( int i = 0; i < mod->numsurfaces; i++ )
	{
		msurface_t *surf = &mod->surfaces[i];

		if( !surf->samples )
			continue;

		if( R_SurfaceIsFloor( surf ))
		{
			const int size = R_SurfaceLightSize( surf );

			memcpy( out + ofs, surf->samples, size * sizeof( *out ));
			surf->samples = out + ofs;
			ofs += size;
		}
		else surf->samples = NULL;
	}

	Mem_Free( mod->lightdata );
	mod->lightdata = out;
	lm_prebuilt = mod;
	lm_prebuiltpages = gl_lms.current_lightmap_texture;

	gEngfuncs.Con_Reportf( "%s: %d KB of %d KB light data kept\n", __func__, (int)( kept * sizeof( *out )) >> 10, (int)( total * sizeof( *out )) >> 10 );
}

/*
==================
R_WorldFree

the world is unloaded: what was made of it goes too, the next one needs the memory
==================
*/
void R_WorldFree( void )
{
	R_MeWorldFree();
	R_PolysFree();
	R_StaticWaterClear();

	free( mark_lastvis );
	mark_lastvis = NULL;
	mark_world = NULL;

	if( ws_alloc )
		Mem_Free( ws_alloc );
	ws_alloc = NULL;
	ws_surfs = NULL;
	ws_numsurfs = 0;

	if( wn_alloc )
		Mem_Free( wn_alloc );
	wn_alloc = NULL;
	wn_nodes = NULL;
	wn_numnodes = wn_nummarks = 0;

	if( bs_alloc )
		Mem_Free( bs_alloc );
	bs_alloc = NULL;
	bs_surfs = NULL;
	bs_first = bs_numsurfs = 0;

	for( int i = 0; i < MAX_LIGHTMAPS && tr.lightmapTextures[i]; i++ )
		GL_FreeTexture( tr.lightmapTextures[i] );
	memset( tr.lightmapTextures, 0, sizeof( tr.lightmapTextures ));
	lm_prebuilt = NULL;
}
#endif

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

#if XASH_PSP
	if( lm_prebuilt && lm_prebuilt == WORLDMODEL )
		return; // the light is gone
#endif

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
#if XASH_PSP
	R_WorldSurfsBuild();
#endif

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

#if XASH_PSP
	// the first ones are the world's
	const int prebuilt = lm_prebuilt && lm_prebuilt == WORLDMODEL ? lm_prebuiltpages : 0;
#else
	const int prebuilt = 0;
#endif

	// release old lightmaps
	for( int i = prebuilt; i < MAX_LIGHTMAPS; i++ )
	{
		if( !tr.lightmapTextures[i] ) break;
		GL_FreeTexture( tr.lightmapTextures[i] );
		tr.lightmapTextures[i] = 0;
	}

	memset( &RI, 0, sizeof( RI ));

	if( guRender.lightmap_vram )
	{
		vfree( guRender.lightmap_vram );
		guRender.lightmap_vram = NULL;
	}

	// GU lightmaps are always 128x128 (BLOCK_SIZE_MAX)
	tr.block_size = BLOCK_SIZE_DEFAULT;

	skychain = NULL;

	tr.framecount = tr.visframecount = 1;	// no dlight cache
	gl_lms.current_lightmap_texture = prebuilt;
	tr.modelviewIdentity = false;
	tr.realframecount = 1;

	// setup the texture for dlights
	R_InitDlightTexture();

	// setup all the lightstyles
	CL_RunLightStyles((lightstyle_t *)ENGINE_GET_PARM( PARM_GET_LIGHTSTYLES_PTR ));

	LM_InitBlock();

#if XASH_PSP
	R_PolysBegin( WORLDMODEL );
#endif

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

#if XASH_PSP
			if( R_PolyIsLazy( m, m->surfaces + j ))
			{
				m->surfaces[j].polys = NULL;
				continue;
			}
#endif

			nColinElim += GL_BuildPolygonFromSurface( m, m->surfaces + j );
		}

		// clearing visframe
		for( int j = 0; j < m->numleafs; j++ )
			m->leafs[j+1].visframe = 0;
		for( int j = 0; j < m->numnodes; j++ )
			m->nodes[j].visframe = 0;
	}

	LM_UploadBlock( false );
#if XASH_PSP
	R_WorldSurfsBuild();
#endif

	if( gEngfuncs.drawFuncs->GL_BuildLightmaps )
	{
		// build lightmaps on the client-side
		gEngfuncs.drawFuncs->GL_BuildLightmaps( );
	}
}
