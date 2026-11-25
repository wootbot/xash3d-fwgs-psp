/*
gu_rmain.c - renderer main loop
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
#include "library.h"
#include "beamdef.h"
#include "entity_types.h"


#define IsLiquidContents( cnt )	( cnt == CONTENTS_WATER || cnt == CONTENTS_SLIME || cnt == CONTENTS_LAVA )
float		gldepthmin, gldepthmax;

ref_instance_t	RI;

int R_RankForRenderMode( int rendermode )
{
	switch( rendermode )
	{
	case kRenderTransTexture:
		return 1;	// draw second
	case kRenderTransAdd:
		return 2;	// draw third
	case kRenderGlow:
		return 3;	// must be last!
	}
	return 0;
}

void R_AllowFog( qboolean allowed )
{
	if( allowed )
	{
		if( glState.isFogEnabled && gl_fog.value )
			sceGuEnable( GU_FOG );
	}
	else
	{
		if( glState.isFogEnabled )
			sceGuDisable( GU_FOG );
	}
}

/*
===============
R_OpaqueEntity

Opaque entity can be brush or studio model but sprite
===============
*/
qboolean R_OpaqueEntity( cl_entity_t *ent )
{
	if( R_GetEntityRenderMode( ent ) == kRenderNormal )
	{
		switch( ent->curstate.renderfx )
		{
		case kRenderFxNone:
		case kRenderFxDeadPlayer:
		case kRenderFxLightMultiplier:
		case kRenderFxExplode:
			return true;
		}
	}
	return false;
}

/*
===============
R_SortTransEntities

sort translucent entities by distance, then by rendermode. The sort keys are
computed once per entity: R_GetEntityRenderMode walks all the textures of a
studio model, and the qsort compare function used to call it twice per
comparison (~2 ms per frame with a couple of hundred temp entities in a fight)
===============
*/
typedef struct
{
	cl_entity_t *ent;
	float dist;
	int rank, order;
} trans_sort_t;

static int R_TransSortCompare( const void *a, const void *b )
{
	const trans_sort_t *s1 = a, *s2 = b;

	if( s1->dist > s2->dist )
		return -1;
	if( s1->dist < s2->dist )
		return 1;
	if( s1->rank > s2->rank )
		return 1;
	if( s1->rank < s2->rank )
		return -1;
	return 0;
}

static uint16_t trans_order[MAX_VISIBLE_PACKET]; // where they were before, 0xffff: not in the table

static void R_SortTransEntities( void )
{
	static trans_sort_t sort[MAX_VISIBLE_PACKET];
	const int count = tr.draw_list->num_trans_entities;
	int numsolid, numkeys = 0;
	const mesort_t *keys = NULL;

#if XASH_PSP
	R_StudioMeTable( &numsolid, &numkeys, &keys );
	for( int i = 0; i < count; i++ )
		trans_order[i] = i < numkeys ? i : 0xffff;
#endif

	if( gl_nosort.value )
		return;

	for( int i = 0; i < count; i++ )
	{
		cl_entity_t *ent = tr.draw_list->trans_entities[i];
		vec3_t vecLen, org;

		sort[i].ent = ent;
		sort[i].order = trans_order[i];

#if XASH_PSP
		if( i < numkeys && keys[i].rank != MESORT_MAIN )
		{
			sort[i].rank = keys[i].rank;
			VectorSubtract( RI.rvp.vieworigin, keys[i].org, vecLen );
			sort[i].dist = keys[i].far ? 1000000000 : DotProduct( vecLen, vecLen );
			continue;
		}
#endif
		int rendermode = R_GetEntityRenderMode( ent );

		sort[i].rank = R_RankForRenderMode( rendermode );

		if( ent->model->type != mod_brush || rendermode != kRenderTransAlpha )
		{
			VectorAverage( ent->model->mins, ent->model->maxs, org );
			VectorAdd( ent->origin, org, org );
			VectorSubtract( RI.rvp.vieworigin, org, vecLen );
			sort[i].dist = DotProduct( vecLen, vecLen );
		}
		else sort[i].dist = 1000000000;
	}

	qsort( sort, count, sizeof( sort[0] ), R_TransSortCompare );

	for( int i = 0; i < count; i++ )
	{
		tr.draw_list->trans_entities[i] = sort[i].ent;
		trans_order[i] = sort[i].order;
	}
}

/*
===============
R_WorldToScreen

Convert a given point from world into screen space
Returns true if we behind to screen
===============
*/
int R_WorldToScreen( const vec3_t point, vec3_t screen )
{
	if( !point || !screen )
		return true;

	matrix4x4 worldToScreen;
	Matrix4x4_Copy( worldToScreen, RI.worldviewProjectionMatrix );
	screen[0] = worldToScreen[0][0] * point[0] + worldToScreen[0][1] * point[1] + worldToScreen[0][2] * point[2] + worldToScreen[0][3];
	screen[1] = worldToScreen[1][0] * point[0] + worldToScreen[1][1] * point[1] + worldToScreen[1][2] * point[2] + worldToScreen[1][3];
	float w = worldToScreen[3][0] * point[0] + worldToScreen[3][1] * point[1] + worldToScreen[3][2] * point[2] + worldToScreen[3][3];
	screen[2] = 0.0f; // just so we have something valid here

	qboolean behind;
	if( w < 0.001f )
	{
		behind = true;
	}
	else
	{
		float invw = 1.0f / w;
		screen[0] *= invw;
		screen[1] *= invw;
		behind = false;
	}

	return behind;
}

/*
===============
R_ScreenToWorld

Convert a given point from screen into world space
===============
*/
void R_ScreenToWorld( const vec3_t screen, vec3_t point )
{
	if( !point || !screen )
		return;

	matrix4x4 screenToWorld;
	Matrix4x4_Invert_Full( screenToWorld, RI.worldviewProjectionMatrix );

	point[0] = screen[0] * screenToWorld[0][0] + screen[1] * screenToWorld[0][1] + screen[2] * screenToWorld[0][2] + screenToWorld[0][3];
	point[1] = screen[0] * screenToWorld[1][0] + screen[1] * screenToWorld[1][1] + screen[2] * screenToWorld[1][2] + screenToWorld[1][3];
	point[2] = screen[0] * screenToWorld[2][0] + screen[1] * screenToWorld[2][1] + screen[2] * screenToWorld[2][2] + screenToWorld[2][3];
	float w = screen[0] * screenToWorld[3][0] + screen[1] * screenToWorld[3][1] + screen[2] * screenToWorld[3][2] + screenToWorld[3][3];
	if( w != 0.0f ) VectorScale( point, ( 1.0f / w ), point );
}

/*
===============
R_PushScene
===============
*/
void R_PushScene( void )
{
	if( ++tr.draw_stack_pos >= MAX_DRAW_STACK )
		gEngfuncs.Host_Error( "draw stack overflow\n" );

	tr.draw_list = &tr.draw_stack[tr.draw_stack_pos];
}

/*
===============
R_PopScene
===============
*/
void R_PopScene( void )
{
	if( --tr.draw_stack_pos < 0 )
		gEngfuncs.Host_Error( "draw stack underflow\n" );
	tr.draw_list = &tr.draw_stack[tr.draw_stack_pos];
}

/*
===============
R_ClearScene
===============
*/
void R_ClearScene( void )
{
	tr.draw_list->num_solid_entities = 0;
	tr.draw_list->num_trans_entities = 0;
	tr.draw_list->num_beam_entities = 0;
#if XASH_PSP
	tr.draw_list->num_players = 0;
	R_StudioMeSceneCleared();
#endif

	// clear the scene befor start new frame
	if( gEngfuncs.drawFuncs->R_ClearScene != NULL )
		gEngfuncs.drawFuncs->R_ClearScene();

}

/*
===============
R_AddEntity
===============
*/
qboolean R_AddEntity( struct cl_entity_s *clent, int type )
{
	if( !r_drawentities->value )
		return false; // not allow to drawing

	if( FBitSet( clent->curstate.effects, EF_NODRAW ))
		return false; // done

	if( !R_ModelOpaque( clent->curstate.rendermode ) && CL_FxBlend( clent ) <= 0 )
		return true; // invisible

	switch( type )
	{
	case ET_FRAGMENTED:
		r_stats.c_client_ents++;
		break;
	case ET_TEMPENTITY:
		r_stats.c_active_tents_count++;
		break;
	}

	if( type == ET_BEAM )
	{
		if( tr.draw_list->num_beam_entities >= MAX_VISIBLE_PACKET )
		{
			gEngfuncs.Con_Printf( S_ERROR "Too many beams %d!\n", tr.draw_list->num_beam_entities );
			return false;
		}

		tr.draw_list->beam_entities[tr.draw_list->num_beam_entities] = clent;
		tr.draw_list->num_beam_entities++;

		return true;
	}
	else if( R_OpaqueEntity( clent ))
	{
		// opaque
		if( tr.draw_list->num_solid_entities >= MAX_VISIBLE_PACKET )
			return false;

#if XASH_PSP
		if( type == ET_PLAYER && tr.draw_list->num_players < MAX_CLIENTS )
			tr.draw_list->players[tr.draw_list->num_players++] = tr.draw_list->num_solid_entities;
#endif

		tr.draw_list->solid_entities[tr.draw_list->num_solid_entities] = clent;
		tr.draw_list->num_solid_entities++;
	}
	else
	{
		// translucent
		if( tr.draw_list->num_trans_entities >= MAX_VISIBLE_PACKET )
			return false;

		tr.draw_list->trans_entities[tr.draw_list->num_trans_entities] = clent;
		tr.draw_list->num_trans_entities++;
	}

	return true;
}

/*
=============
R_Clear
=============
*/
static void R_Clear( int bitMask )
{
	int		bits;

	if( ENGINE_GET_PARM( PARM_DEV_OVERVIEW )) 
		sceGuClearColor( 0xff00ff00 ); // green background (Valve rules)
	else sceGuClearColor( 0xff7f7f7f );

	bits = GU_DEPTH_BUFFER_BIT;

	if( glState.stencilEnabled )
		bits |= GU_STENCIL_BUFFER_BIT;

	bits &= bitMask;

	sceGuClear( bits );

	// change ordering for overview
	if( FBitSet( RI.rvp.flags, RF_DRAW_OVERVIEW ))
	{
		gldepthmin = 65535.0f;
		gldepthmax = 0.0f;
	}
	else
	{
		gldepthmin = 0.0f;
		gldepthmax = 65535.0f;
	}

	sceGuDepthFunc( GU_LEQUAL );
	GU_DepthRange( ( int )gldepthmin, ( int )gldepthmax );
}

//=============================================================================
/*
===============
R_GetFarClip
===============
*/
float R_GetFarClip( void )
{
	if( WORLDMODEL && FBitSet( RI.rvp.flags, RF_DRAW_WORLD ))
		return gp_movevars->zmax * 1.73f;
	return 2048.0f;
}

/*
===============
R_SetupFrustum
===============
*/
void R_SetupFrustum( void )
{
	// build the transformation matrix for the given view angles
	AngleVectors( RI.rvp.viewangles, RI.vforward, RI.vright, RI.vup );

	if( !r_lockfrustum.value )
	{
		VectorCopy( RI.rvp.vieworigin, RI.cullorigin );
		VectorCopy( RI.vforward, RI.cull_vforward );
		VectorCopy( RI.vright, RI.cull_vright );
		VectorCopy( RI.vup, RI.cull_vup );
	}

	if( FBitSet( RI.rvp.flags, RF_DRAW_OVERVIEW ))
	{
		const ref_overview_t	*ov = gEngfuncs.GetOverviewParms();
		GL_FrustumInitOrtho( &RI.frustum, ov->xLeft, ov->xRight, ov->yTop, ov->yBottom, ov->zNear, ov->zFar );
	}
	else
	{
		GL_FrustumInitProj( &RI.frustum, 0.0f, R_GetFarClip(), RI.rvp.fov_x, RI.rvp.fov_y ); // NOTE: we ignore nearplane here (mirrors only)
	}
}

/*
=============
R_SetupProjectionMatrix
=============
*/
static void R_SetupProjectionMatrix( matrix4x4 m )
{
	if( FBitSet( RI.rvp.flags, RF_DRAW_OVERVIEW ))
	{
		const ref_overview_t *ov = gEngfuncs.GetOverviewParms();
		Matrix4x4_CreateOrtho( m, ov->xLeft, ov->xRight, ov->yTop, ov->yBottom, ov->zNear, ov->zFar );
		return;
	}

	RI.farClip = R_GetFarClip();

	GLfloat zNear = 4.0f;
	GLfloat zFar = Q_max( 256.0f, RI.farClip );

	GLfloat yMax = zNear * tan( RI.rvp.fov_y * M_PI_F / 360.0f );
	GLfloat yMin = -yMax;

	GLfloat xMax = zNear * tan( RI.rvp.fov_x * M_PI_F / 360.0f );
	GLfloat xMin = -xMax;

	Matrix4x4_CreateProjection( m, xMax, xMin, yMax, yMin, zNear, zFar );
}

/*
=============
R_SetupModelviewMatrix
=============
*/
static void R_SetupModelviewMatrix( matrix4x4 m )
{
	Matrix4x4_CreateModelview( m );
	Matrix4x4_ConcatRotate( m, -RI.rvp.viewangles[2], 1, 0, 0 );
	Matrix4x4_ConcatRotate( m, -RI.rvp.viewangles[0], 0, 1, 0 );
	Matrix4x4_ConcatRotate( m, -RI.rvp.viewangles[1], 0, 0, 1 );
	Matrix4x4_ConcatTranslate( m, -RI.rvp.vieworigin[0], -RI.rvp.vieworigin[1], -RI.rvp.vieworigin[2] );
}

/*
=============
R_LoadIdentity
=============
*/
void R_LoadIdentity( void )
{
	if( tr.modelviewIdentity )
		return;

	Matrix4x4_LoadIdentity( RI.objectMatrix );
	sceGumMatrixMode( GU_MODEL );
	GL_LoadMatrix( RI.objectMatrix );
	sceGumUpdateMatrix();
	tr.modelviewIdentity = true;
}

/*
=============
R_RotateForEntity
=============
*/
void R_RotateForEntity( cl_entity_t *e )
{
	float	scale = 1.0f;

	if( e == CL_GetEntityByIndex( 0 ))
	{
		R_LoadIdentity();
		return;
	}

	if( e->model->type != mod_brush && e->curstate.scale > 0.0f )
		scale = e->curstate.scale;

	Matrix4x4_CreateFromEntity( RI.objectMatrix, e->angles, e->origin, scale );
	sceGumMatrixMode( GU_MODEL );
	GL_LoadMatrix( RI.objectMatrix );
	sceGumUpdateMatrix();
	tr.modelviewIdentity = false;
}

/*
=============
R_TranslateForEntity
=============
*/
void R_TranslateForEntity( cl_entity_t *e )
{
	float	scale = 1.0f;

	if( e == CL_GetEntityByIndex( 0 ))
	{
		printf("e == gEngfuncs.GetEntityByIndex\n");
		R_LoadIdentity();
		return;
	}

	if( e->model->type != mod_brush && e->curstate.scale > 0.0f )
		scale = e->curstate.scale;

	Matrix4x4_CreateFromEntity( RI.objectMatrix, vec3_origin, e->origin, scale );
	sceGumMatrixMode( GU_MODEL );
	GL_LoadMatrix( RI.objectMatrix );
	sceGumUpdateMatrix();
	tr.modelviewIdentity = false;
}

/*
===============
R_FindViewLeaf
===============
*/
void R_FindViewLeaf( void )
{
	RI.oldviewleaf = RI.viewleaf;
	RI.viewleaf = gEngfuncs.Mod_PointInLeaf( RI.rvp.vieworigin, WORLDMODEL->nodes, WORLDMODEL );
}

/*
===============
R_SetupFrame
===============
*/
static void R_SetupFrame( void )
{
	// setup viewplane dist
	RI.viewplanedist = DotProduct( RI.rvp.vieworigin, RI.vforward );

	// NOTE: this request is the fps-killer on some NVidia drivers
	glState.isFogEnabled = sceGuGetStatus( GU_FOG );

	// current viewleaf
	if( FBitSet( RI.rvp.flags, RF_DRAW_WORLD ))
		R_FindViewLeaf();
}

/*
=============
R_SetupGL
=============
*/
void R_SetupGL( qboolean set_gl_state )
{
	R_SetupModelviewMatrix( RI.worldviewMatrix );
	R_SetupProjectionMatrix( RI.projectionMatrix );

	Matrix4x4_Concat( RI.worldviewProjectionMatrix, RI.projectionMatrix, RI.worldviewMatrix );

	if( !set_gl_state ) return;

	{
		// GL style viewport (origin at the bottom left) to GU, which centers it in the 4096x4096
		// virtual space and offsets the screen with sceGuOffset (see GU_Init)
		const int *vp = RI.rvp.viewport;
		int top = gpGlobals->height - ( vp[1] + vp[3] );

		sceGuViewport( 2048 - ( gpGlobals->width >> 1 ) + vp[0] + ( vp[2] >> 1 ),
			2048 - ( gpGlobals->height >> 1 ) + top + ( vp[3] >> 1 ),
			vp[2], vp[3] );
		sceGuScissor( vp[0], top, vp[0] + vp[2], top + vp[3] );
	}

	GU_LoadProjection( RI.projectionMatrix );

	sceGumMatrixMode( GU_VIEW );
	GL_LoadMatrix( RI.worldviewMatrix );

	sceGumUpdateMatrix();	//Apply a matrix transformation
	sceGumMatrixMode( GU_MODEL );

	// NOTE: user clip planes (RF_CLIPPLANE) aren't supported by the GE
	GL_Cull( GL_FRONT );

	sceGuDisable( GU_BLEND );
	sceGuDisable( GU_ALPHA_TEST );
	sceGuColor( GUCOLOR4F( 1.0f, 1.0f, 1.0f, 1.0f ) );

	GU_ClipSetWorldFrustum( RI.worldviewProjectionMatrix );

}

/*
=============
R_RecursiveFindWaterTexture

using to find source waterleaf with
watertexture to grab fog values from it
=============
*/
static gl_texture_t *R_RecursiveFindWaterTexture( const mnode_t *node, const mnode_t *ignore, qboolean down )
{
	gl_texture_t *tex = NULL;

	// assure the initial node is not null
	// we could check it here, but we would rather check it
	// outside the call to get rid of one additional recursion level
	Assert( node != NULL );

	// ignore solid nodes
	if( node->contents == CONTENTS_SOLID )
		return NULL;

	if( node->contents < 0 )
	{
		// ignore non-liquid leaves
		if( node->contents != CONTENTS_WATER && node->contents != CONTENTS_LAVA && node->contents != CONTENTS_SLIME )
			 return NULL;

		// find texture
		mleaf_t *pleaf = (mleaf_t *)node;
		msurface_t **mark = pleaf->firstmarksurface;
		int c = pleaf->nummarksurfaces;

		for( int i = 0; i < c; i++, mark++ )
		{
			if( (*mark)->flags & SURF_DRAWTURB && (*mark)->texinfo && (*mark)->texinfo->texture )
				return R_GetTexture( (*mark)->texinfo->texture->gl_texturenum );
		}

		// texture not found
		return NULL;
	}

	// this is a regular node
	// traverse children
	mnode_t *child = node_child( node, 0, WORLDMODEL );

	if( child && ( child != ignore ))
	{
		tex = R_RecursiveFindWaterTexture( child, node, true );
		if( tex ) return tex;
	}

	child = node_child( node, 1, WORLDMODEL );

	if( child && ( child != ignore ))
	{
		tex = R_RecursiveFindWaterTexture( child, node, true );
		if( tex ) return tex;
	}

	// for down recursion, return immediately
	if( down ) return NULL;

	// texture not found, step up if any
	if( node->parent )
		return R_RecursiveFindWaterTexture( node->parent, node, false );

	// top-level node, bail out
	return NULL;
}

/*
=============
R_CheckFog

check for underwater fog
Using backward recursion to find waterline leaf
from underwater leaf (idea: XaeroX)
=============
*/
static void R_CheckFog( void )
{
	// quake global fog
	if( FBitSet( gp_host->features, ENGINE_QUAKE_COMPATIBLE ))
	{
		if( !gp_movevars->fog_settings )
		{
			if( sceGuGetStatus( GU_FOG ) )
				sceGuDisable( GU_FOG );
			RI.fogEnabled = false;
			return;
		}

		// quake-style global fog
		RI.fogColor[0] = ((gp_movevars->fog_settings & 0xFF000000) >> 24) / 255.0f;
		RI.fogColor[1] = ((gp_movevars->fog_settings & 0xFF0000) >> 16) / 255.0f;
		RI.fogColor[2] = ((gp_movevars->fog_settings & 0xFF00) >> 8) / 255.0f;
		RI.fogDensity = ((gp_movevars->fog_settings & 0xFF) / 255.0f) * 0.01f;
		RI.fogStart = RI.fogEnd = 0.0f;
		RI.fogColor[3] = 1.0f;
		RI.fogCustom = false;
		RI.fogEnabled = true;
		RI.fogSkybox = true;
		return;
	}

	RI.fogEnabled = false;

	if( FBitSet( RI.rvp.flags, RF_ONLY_CLIENTDRAW ) || ENGINE_GET_PARM( PARM_WATER_LEVEL ) < 3 || !FBitSet( RI.rvp.flags, RF_DRAW_WORLD ) || !RI.viewleaf )
	{
		if( RI.cached_waterlevel == 3 )
		{
			// in some cases waterlevel jumps from 3 to 1. Catch it
			RI.cached_waterlevel = ENGINE_GET_PARM( PARM_WATER_LEVEL );
			RI.cached_contents = CONTENTS_EMPTY;
			if( !RI.fogCustom )
			{
				glState.isFogEnabled = false;
				sceGuDisable( GU_FOG );
			}
		}
		return;
	}

	cl_entity_t *ent = gEngfuncs.CL_GetWaterEntity( RI.rvp.vieworigin );
	int cnt;
	if( ent && ent->model && ent->model->type == mod_brush && ent->curstate.skin < 0 )
		cnt = ent->curstate.skin;
	else cnt = RI.viewleaf->contents;

	RI.cached_waterlevel = ENGINE_GET_PARM( PARM_WATER_LEVEL );

	if( !IsLiquidContents( RI.cached_contents ) && IsLiquidContents( cnt ))
	{
		gl_texture_t *tex = NULL;

		// check for water texture
		if( ent && ent->model && ent->model->type == mod_brush )
		{
			int count = ent->model->nummodelsurfaces;

			msurface_t *surf = &ent->model->surfaces[ent->model->firstmodelsurface];
			for( int i = 0; i < count; i++, surf++ )
			{
				if( surf->flags & SURF_DRAWTURB && surf->texinfo && surf->texinfo->texture )
				{
					tex = R_GetTexture( surf->texinfo->texture->gl_texturenum );
					RI.cached_contents = ent->curstate.skin;
					break;
				}
			}
		}
		else
		{
			tex = R_RecursiveFindWaterTexture( RI.viewleaf->parent, NULL, false );
			if( tex ) RI.cached_contents = RI.viewleaf->contents;
		}

		if( !tex ) return;	// no valid fogs

		// copy fog params
		RI.fogColor[0] = tex->fogParams[0] / 255.0f;
		RI.fogColor[1] = tex->fogParams[1] / 255.0f;
		RI.fogColor[2] = tex->fogParams[2] / 255.0f;
		RI.fogDensity = tex->fogParams[3] * 0.000025f;
		RI.fogStart = RI.fogEnd = 0.0f;
		RI.fogColor[3] = 1.0f;
		RI.fogCustom = false;
		RI.fogEnabled = true;
		RI.fogSkybox = true;
	}
	else
	{
		RI.fogCustom = false;
		RI.fogEnabled = true;
		RI.fogSkybox = true;
	}
}

/*
=============
R_CheckGLFog

special condition for Spirit 1.9
that used direct calls of glFog-functions
=============
*/
static void R_CheckGLFog( void )
{
#ifdef HACKS_RELATED_HLMODS
	if(( !RI.fogEnabled && !RI.fogCustom ) && sceGuGetStatus( GU_FOG ) && VectorIsNull( RI.fogColor ))
	{
		// fill the fog color from GL-state machine
		RI.fogSkybox = true;
	}
#endif
}

/*
=============
R_DrawFog

=============
*/
void R_DrawFog( void )
{
	if( !RI.fogEnabled || !gl_fog.value )
		return;

	// GU fog is linear: exponential fog reaches ~95% at 3 / density
	float start = 0.0f, end = 1000.0f;

	if( RI.fogEnd > RI.fogStart )
	{
		start = RI.fogStart;
		end = RI.fogEnd;
	}
	else if( RI.fogDensity > 0.0f )
	{
		end = ( FBitSet( gp_host->features, ENGINE_QUAKE_COMPATIBLE ) ? 1.75f : 3.0f ) / RI.fogDensity;
	}

	glState.fogDensity = RI.fogDensity;
	glState.fogStart = start;
	glState.fogEnd = end;
	glState.fogColor = GUCOLOR4F( RI.fogColor[0], RI.fogColor[1], RI.fogColor[2], 1.0f );

	sceGuEnable( GU_FOG );
	sceGuFog( start, end, glState.fogColor );
}

/*
=============
R_DrawStaticBrushGroup

draws list[start] and the static brush models after it in the list that share
its render mode (R_StaticBrushModel) in one go, marks them in grouped[]
=============
*/
static void R_DrawStaticBrushGroup( cl_entity_t **list, int count, int start, byte *grouped, qboolean trans )
{
	static cl_entity_t *ents[MAX_VISIBLE_PACKET];
	const int rendermode = list[start]->curstate.rendermode;
	int n = 0, surfs = 0;

	for( int i = start; i < count; i++ )
	{
		cl_entity_t *e = list[i];

		if( grouped[i] || !R_StaticBrushModel( e ) || e->curstate.rendermode != rendermode )
			continue;

		grouped[i] = true;

		// skipped like the translucent loop does
		if( trans && rendermode != kRenderNormal && CL_FxBlend( e ) <= 0 )
			continue;

		if( n && surfs + e->model->nummodelsurfaces > gpGlobals->max_surfaces )
		{
			R_DrawStaticBrushModels( ents, n );
			n = surfs = 0;
		}

		ents[n++] = e;
		surfs += e->model->nummodelsurfaces;
	}

	R_DrawStaticBrushModels( ents, n );
}

static byte solid_culled[MAX_VISIBLE_PACKET], trans_culled[MAX_VISIBLE_PACKET];
static byte solid_grouped[MAX_VISIBLE_PACKET], trans_grouped[MAX_VISIBLE_PACKET];
static qboolean viewmodel_drawn; // before the world, see R_RenderScene
static qboolean viewmodel_events; // to run in R_RenderScene, once the world is under way

/*
=============
R_PrepareEntities

before the world: studio models off screen, all in a row, then the bones of
the others start on the Media Engine while the world is drawn
=============
*/
static void R_PrepareEntities( void )
{
	if( FBitSet( RI.rvp.flags, RF_ONLY_CLIENTDRAW ))
		return;

	memset( solid_grouped, 0, sizeof( solid_grouped ));
	memset( trans_grouped, 0, sizeof( trans_grouped ));
	// the brush models first: this CPU waits for them, only the GE for the studio models
	R_MeBrushesBegin( solid_grouped, trans_grouped, trans_order );
	R_StudioMePrepare( solid_culled, trans_culled, trans_order );
	R_StudioMeSceneCleared(); // a second view in this frame starts over

	// the world walk adds static entities
	memset( solid_culled + tr.draw_list->num_solid_entities, 0, sizeof( solid_culled ) - tr.draw_list->num_solid_entities );
	memset( trans_culled + tr.draw_list->num_trans_entities, 0, sizeof( trans_culled ) - tr.draw_list->num_trans_entities );
}

/*
=============
R_DrawMeBrushes

the static brush models R_MeBrushesBegin took
=============
*/
static void R_DrawMeBrushes( void )
{
	if( R_MeBrushesDraw( ))
		return;

	// the job ran out of space
	for( int i = 0; i < tr.draw_list->num_solid_entities; i++ )
	{
		if( solid_grouped[i] == R_GROUPED_ME )
			solid_grouped[i] = 0;
	}

	for( int i = 0; i < tr.draw_list->num_trans_entities; i++ )
	{
		if( trans_grouped[i] == R_GROUPED_ME )
			trans_grouped[i] = 0; // the loop below gets there
	}

	for( int i = 0; i < tr.draw_list->num_solid_entities; i++ )
	{
		if( !solid_grouped[i] && R_StaticBrushModel( tr.draw_list->solid_entities[i] ))
			R_DrawStaticBrushGroup( tr.draw_list->solid_entities, tr.draw_list->num_solid_entities, i, solid_grouped, false );
	}
}

/*
=============
R_DrawStudioModelsFirst

solid studio models that don't need bones from the Media Engine, before the
world, as long as it isn't done with it
=============
*/
static void R_DrawStudioModelsFirst( void )
{
	for( int i = 0; i < tr.draw_list->num_solid_entities && R_MeWorldBusy(); i++ )
	{
		cl_entity_t *e = tr.draw_list->solid_entities[i];

		// a follower needs its parent's bones
		if( solid_culled[i] || !e->model || e->model->type != mod_studio || e->player
			|| e->curstate.movetype == MOVETYPE_FOLLOW || R_StudioMeBonesPosted( e ))
			continue;

		RI.currententity = e;
		RI.currentmodel = e->model;
		R_DrawStudioModel( e );
		solid_culled[i] = true; // done with
	}
}

/*
=============
R_DrawEntitiesOnList
=============
*/
static void R_DrawEntitiesOnList( void )
{
	tr.blend = 1.0f;

	// first draw solid entities
	for( int i = 0; i < tr.draw_list->num_solid_entities && !FBitSet( RI.rvp.flags, RF_ONLY_CLIENTDRAW ); i++ )
	{
		RI.currententity = tr.draw_list->solid_entities[i];
		RI.currentmodel = RI.currententity->model;

		if( !RI.currentmodel && RI.currententity->player && !FBitSet( RI.rvp.flags, RF_DRAW_WORLD ))
			continue;

		Assert( RI.currententity != NULL );
		Assert( RI.currentmodel != NULL );

		switch( RI.currentmodel->type )
		{
		case mod_brush:
			if( solid_grouped[i] )
				break;
			if( R_StaticBrushModel( RI.currententity ))
				R_DrawStaticBrushGroup( tr.draw_list->solid_entities, tr.draw_list->num_solid_entities, i, solid_grouped, false );
			else R_DrawBrushModel( RI.currententity );
			break;
		case mod_alias:
			R_DrawAliasModel( RI.currententity );
			break;
		case mod_studio:
			if( !solid_culled[i] )
				R_DrawStudioModel( RI.currententity );
			break;
		default:
			break;
		}
	}

#if XASH_PSP
	R_DrawMeBrushes();
	if( !FBitSet( RI.rvp.flags, RF_ONLY_CLIENTDRAW ))
		R_StudioDrawMeModels();
#endif

	// quake-specific feature
	R_DrawAlphaTextureChains();

	// draw sprites seperately, because of alpha blending
	for( int i = 0; i < tr.draw_list->num_solid_entities && !FBitSet( RI.rvp.flags, RF_ONLY_CLIENTDRAW ); i++ )
	{
		RI.currententity = tr.draw_list->solid_entities[i];
		RI.currentmodel = RI.currententity->model;

		if( !RI.currentmodel && RI.currententity->player && !FBitSet( RI.rvp.flags, RF_DRAW_WORLD ))
			continue;

		Assert( RI.currententity != NULL );
		Assert( RI.currentmodel != NULL );

		switch( RI.currentmodel->type )
		{
		case mod_sprite:
			R_DrawSpriteModel( RI.currententity );
			break;
		}
	}


	if( !FBitSet( RI.rvp.flags, RF_ONLY_CLIENTDRAW ))
	{
		gEngfuncs.CL_DrawEFX( tr.frametime, false );
	}


	if( FBitSet( RI.rvp.flags, RF_DRAW_WORLD ))
		gEngfuncs.pfnDrawNormalTriangles();


#ifdef HACKS_RELATED_HLMODS
	// Vampire Slayer leave GL_LINEAR fog with their own start/end after
	// pfnDrawNormalTriangles; restore engine fog for the translucent
	// entities and viewmodel drawn below.
	R_DrawFog();
#endif

	// then draw translucent entities
	for( int i = 0; i < tr.draw_list->num_trans_entities && !FBitSet( RI.rvp.flags, RF_ONLY_CLIENTDRAW ); i++ )
	{
		RI.currententity = tr.draw_list->trans_entities[i];
		RI.currentmodel = RI.currententity->model;

		if( trans_grouped[i] )
			continue;

		if( R_StaticBrushModel( RI.currententity ))
		{
			R_DrawStaticBrushGroup( tr.draw_list->trans_entities, tr.draw_list->num_trans_entities, i, trans_grouped, true );
			continue;
		}

		// handle studiomodels with custom rendermodes on texture
		if( RI.currententity->curstate.rendermode != kRenderNormal )
			tr.blend = CL_FxBlend( RI.currententity ) / 255.0f;
		else tr.blend = 1.0f; // draw as solid but sorted by distance

		if( tr.blend <= 0.0f ) continue;

		if( !RI.currentmodel && RI.currententity->player && !FBitSet( RI.rvp.flags, RF_DRAW_WORLD ))
			continue;

		Assert( RI.currententity != NULL );
		Assert( RI.currentmodel != NULL );

		switch( RI.currentmodel->type )
		{
		case mod_brush:
			R_DrawBrushModel( RI.currententity );
			break;
		case mod_alias:
			R_DrawAliasModel( RI.currententity );
			break;
		case mod_studio:
			if( trans_culled[i] == R_STUDIO_ME )
				R_StudioDrawMeModel( RI.currententity );
			else if( !trans_culled[i] )
				R_DrawStudioModel( RI.currententity );
			break;
		case mod_sprite:
			R_DrawSpriteModel( RI.currententity );
			break;
		default:
			break;
		}
	}


	if( FBitSet( RI.rvp.flags, RF_DRAW_WORLD ))
	{
		sceGuTexFunc( GU_TFX_MODULATE, GU_TCC_RGBA );
		gEngfuncs.pfnDrawTransparentTriangles ();
	}


	if( !FBitSet( RI.rvp.flags, RF_ONLY_CLIENTDRAW ))
	{
		R_AllowFog( false );
		gEngfuncs.CL_DrawEFX( tr.frametime, true );
		R_AllowFog( true );
	}

	sceGuDisable( GU_BLEND );	// Trinity Render issues
	R_StudioPreCullEnd();

	if( !FBitSet( RI.rvp.flags, RF_ONLY_CLIENTDRAW ) && !viewmodel_drawn )
		R_DrawViewModel();
	gEngfuncs.CL_ExtraUpdate();
}

/*
================
R_RenderScene

R_SetupRefParams must be called right before
================
*/
void R_RenderScene( void )
{
	if( !WORLDMODEL && FBitSet( RI.rvp.flags, RF_DRAW_WORLD ))
		gEngfuncs.Host_Error( "%s: NULL worldmodel\n", __func__ );

	// frametime is valid only for normal pass
	if( !FBitSet( RI.rvp.flags, RF_DRAW_CUBEMAP ))
		tr.frametime = gp_cl->time -   gp_cl->oldtime;
	else tr.frametime = 0.0;

	// begin a new frame
	tr.framecount++;

#if XASH_PSP
	GU_WorldSurfsNewFrame();
#endif
	tr.dlightframecount = R_PushDlights( WORLDMODEL, tr.framecount );

	R_SetupFrustum();
	R_SetupFrame();
	R_SetupGL( true );
	R_Clear( ~0 );

	R_MarkLeaves();
	R_DrawFog ();

	R_CheckGLFog();
#if XASH_PSP
	GL_PlaceTextures();
	if( r_drawworld.value )
		R_MeWorldBegin();
#endif
	if( viewmodel_events )
	{
		R_RunViewmodelEvents();
		tr.realframecount++; // right called after viewmodel events
	}
	R_SortTransEntities();
	R_PrepareEntities();
#if XASH_PSP
	viewmodel_drawn = false;
	if( !FBitSet( RI.rvp.flags, RF_ONLY_CLIENTDRAW ))
	{
		// instead of waiting for the ME
		if( R_MeWorldBusy() && R_ViewModelSolid( ))
		{
			R_DrawViewModel();
			viewmodel_drawn = true;
		}

		tr.blend = 1.0f;
		R_DrawStudioModelsFirst();
	}

	if( r_drawworld.value )
#endif
	R_DrawWorld();
	R_CheckFog();

	gEngfuncs.CL_ExtraUpdate ();	// don't let sound get messed up if going slow

	R_DrawEntitiesOnList();

#if XASH_PSP
	if( r_drawworld.value )
#endif
	R_DrawWaterSurfaces();
}

void R_GammaChanged( qboolean do_reset_gamma )
{
	if( do_reset_gamma )
	{
		// paranoia cubemap rendering
		if( gEngfuncs.drawFuncs->GL_BuildLightmaps )
			gEngfuncs.drawFuncs->GL_BuildLightmaps( );
	}
	else
	{
		glConfig.softwareGammaUpdate = true;
		GL_RebuildLightmaps();
		glConfig.softwareGammaUpdate = false;

		ClearBits( vid_brightness->flags, FCVAR_CHANGED );
		ClearBits( vid_gamma->flags, FCVAR_CHANGED );
	}
}

static void R_NextFrame( void );

/*
===============
R_BeginFrame
===============
*/
void R_BeginFrame( qboolean clearScene )
{
	R_NextFrame();
	glConfig.softwareGammaUpdate = false;	// in case of possible fails


	if(( gl_clear->value || ENGINE_GET_PARM( PARM_DEV_OVERVIEW )) &&
		clearScene && ENGINE_GET_PARM( PARM_CONNSTATE ) != ca_cinematic )
	{
		sceGuClear( GU_COLOR_BUFFER_BIT );
	}


	R_Set2DMode( true );
	gEngfuncs.CL_ExtraUpdate ();
}

/*
===============
R_SetupRefParams

set initial params for renderer
===============
*/
void R_SetupRefParams( const ref_viewpass_t *rvp )
{
	RI.rvp = *rvp;

	RI.farClip = 0;
}

/*
===============
R_RenderFrame
===============
*/
void R_RenderFrame( const ref_viewpass_t *rvp )
{
	if( r_norefresh->value )
		return;

	// setup the initial render params
	R_SetupRefParams( rvp );

	// completely override rendering
	if( gEngfuncs.drawFuncs->GL_RenderFrame != NULL )
	{
		tr.fCustomRendering = true;

		if( gEngfuncs.drawFuncs->GL_RenderFrame( rvp ))
		{
			R_GatherPlayerLight( tr.viewent );
			tr.realframecount++;
			tr.fResetVis = true;
			return;
		}
	}

	tr.fCustomRendering = false;
	viewmodel_events = !FBitSet( RI.rvp.flags, RF_ONLY_CLIENTDRAW );
	if( !viewmodel_events )
		tr.realframecount++;
	R_RenderScene();
	viewmodel_events = false;

	return;
}

/*
===============
R_EndFrame
===============
*/
void R_EndFrame( void )
{

	// flush any remaining 2D bits
	R_Set2DMode( false );
	R_StudioFinishMeModels();
	R_MeWorldEarly();

	// finish rendering: the GE draws it while the next frame's game code runs, and shows
	// it when it's done (GU_Present). With vsync R_NextFrame does
	guRender.list_used = extGuListUsed();
	if( guRender.list_used > guRender.context_list_size )
		gEngfuncs.Con_Printf( S_ERROR "GE list overflow: %u of %u KB\n", (uint)guRender.list_used >> 10, (uint)guRender.context_list_size >> 10 );
	guRender.present[guRender.vert_frame & 1] = guRender.draw_buffer;
	sceGuFinishId( gl_vsync->value ? 2 : guRender.vert_frame & 1 );
	guRender.frame_pending = true;
}

static void R_SyncList( int id )
{
	if( id >= 0 )
	{
		R_StudioMeSync( id ); // it may wait for the ME
		sceGeListSync( id, 0 );
	}
}

/*
===============
R_NextFrame

open the next list. The GE may still be drawing the last one (guRender.prev_list), not
the one before: everything it reads from memory that changes every frame is there twice
===============
*/
static void R_NextFrame( void )
{
	const int last = extGuListId();
	qboolean whole = !r_ge_async.value || gl_vsync->value != 0.0f;

	if( !guRender.frame_pending )
		return;
	guRender.frame_pending = false;

	// a long list: the next one may be too
	if( guRender.list_used > PSP_GU_LIST_SIZE / 4 )
		whole = true;

	R_SyncList( guRender.prev_list );
	guRender.prev_list = last;

	if( whole || guRender.list_whole )
	{
		R_SyncList( last );
		guRender.prev_list = -1;
	}

	gEngfuncs.GL_SwapBuffers(); // vsync
	if( gl_vsync->value )
		sceDisplaySetFrameBuf( guRender.draw_buffer, guRender.buffer_width, guRender.buffer_format, PSP_DISPLAY_SETBUF_IMMEDIATE );

	guRender.vert_frame++;

	// swap the buffers.
	void* p_swap = guRender.disp_buffer;
	guRender.disp_buffer = guRender.draw_buffer;
	guRender.draw_buffer = p_swap;

	// start a new render.
	GU_OpenList( whole );
}

/*
===============
R_WaitFrame

wait for the GE to finish the last frame, for what it reads to be changed or freed
===============
*/
void R_WaitFrame( void )
{
	R_NextFrame();
	R_SyncList( guRender.prev_list );
	guRender.prev_list = -1;
}

/*
===============
R_DropFrame

the frame won't be finished (Host_Error): the GE is done with what there is of it and
the Media Engine with its jobs, nothing of it is used anymore
===============
*/
void R_DropFrame( void )
{
	R_StudioMeDrop();
	R_MeWorldWait();
	R_WaitFrame();

	extGuClose( 2 );
	sceGuSync( GU_SYNC_FINISH, GU_SYNC_WAIT );
	R_StudioMeFreeCmds();
	guRender.vert_frame++;
	GU_OpenList( true );
	glState.currentTexture = -1;
}

/*
===============
R_DrawCubemapView
===============
*/
void R_DrawCubemapView( const vec3_t origin, const vec3_t angles, int size )
{
	ref_viewpass_t rvp;

	// basic params
	rvp.flags = rvp.viewentity = 0;
	SetBits( rvp.flags, RF_DRAW_WORLD );
	SetBits( rvp.flags, RF_DRAW_CUBEMAP );

	rvp.viewport[0] = rvp.viewport[1] = 0;
	rvp.viewport[2] = rvp.viewport[3] = size;
	rvp.fov_x = rvp.fov_y = 90.0f; // this is a final fov value

	// setup origin & angles
	VectorCopy( origin, rvp.vieworigin );
	VectorCopy( angles, rvp.viewangles );

	R_RenderFrame( &rvp );

	RI.viewleaf = NULL;		// force markleafs next frame
}

/*
===============
CL_FxBlend
===============
*/
int CL_FxBlend( cl_entity_t *e )
{
	int blend = 0;
	float offset = ((int)e->index ) * 363.0f; // Use ent index to de-sync these fx

	switch( e->curstate.renderfx )
	{
	case kRenderFxPulseSlowWide:
		blend = e->curstate.renderamt + 0x40 * sin( gp_cl->time * 2 + offset );
		break;
	case kRenderFxPulseFastWide:
		blend = e->curstate.renderamt + 0x40 * sin( gp_cl->time * 8 + offset );
		break;
	case kRenderFxPulseSlow:
		blend = e->curstate.renderamt + 0x10 * sin( gp_cl->time * 2 + offset );
		break;
	case kRenderFxPulseFast:
		blend = e->curstate.renderamt + 0x10 * sin( gp_cl->time * 8 + offset );
		break;
	case kRenderFxFadeSlow:
		if( !FBitSet( RI.rvp.flags, RF_DRAW_CUBEMAP ))
		{
			if( e->curstate.renderamt > 0 )
				e->curstate.renderamt -= 1;
			else e->curstate.renderamt = 0;
		}
		blend = e->curstate.renderamt;
		break;
	case kRenderFxFadeFast:
		if( !FBitSet( RI.rvp.flags, RF_DRAW_CUBEMAP ))
		{
			if( e->curstate.renderamt > 3 )
				e->curstate.renderamt -= 4;
			else e->curstate.renderamt = 0;
		}
		blend = e->curstate.renderamt;
		break;
	case kRenderFxSolidSlow:
		if( !FBitSet( RI.rvp.flags, RF_DRAW_CUBEMAP ))
		{
			if( e->curstate.renderamt < 255 )
				e->curstate.renderamt += 1;
			else e->curstate.renderamt = 255;
		}
		blend = e->curstate.renderamt;
		break;
	case kRenderFxSolidFast:
		if( !FBitSet( RI.rvp.flags, RF_DRAW_CUBEMAP ))
		{
			if( e->curstate.renderamt < 252 )
				e->curstate.renderamt += 4;
			else e->curstate.renderamt = 255;
		}
		blend = e->curstate.renderamt;
		break;
	case kRenderFxStrobeSlow:
		blend = 20 * sin( gp_cl->time * 4 + offset );
		if( blend < 0 ) blend = 0;
		else blend = e->curstate.renderamt;
		break;
	case kRenderFxStrobeFast:
		blend = 20 * sin( gp_cl->time * 16 + offset );
		if( blend < 0 ) blend = 0;
		else blend = e->curstate.renderamt;
		break;
	case kRenderFxStrobeFaster:
		blend = 20 * sin( gp_cl->time * 36 + offset );
		if( blend < 0 ) blend = 0;
		else blend = e->curstate.renderamt;
		break;
	case kRenderFxFlickerSlow:
		blend = 20 * (sin( gp_cl->time * 2 ) + sin( gp_cl->time * 17 + offset ));
		if( blend < 0 ) blend = 0;
		else blend = e->curstate.renderamt;
		break;
	case kRenderFxFlickerFast:
		blend = 20 * (sin( gp_cl->time * 16 ) + sin( gp_cl->time * 23 + offset ));
		if( blend < 0 ) blend = 0;
		else blend = e->curstate.renderamt;
		break;
	case kRenderFxHologram:
	case kRenderFxDistort:
	{
		vec3_t tmp = Vec3( e->origin );
		VectorSubtract( tmp, RI.rvp.vieworigin, tmp );
		float dist = DotProduct( tmp, RI.vforward );

		// turn off distance fade
		if( e->curstate.renderfx == kRenderFxDistort )
			dist = 1;

		if( dist <= 0 )
		{
			blend = 0;
		}
		else
		{
			e->curstate.renderamt = 180;
			if( dist <= 100 ) blend = e->curstate.renderamt;
			else blend = (int) ((1.0f - ( dist - 100 ) * ( 1.0f / 400.0f )) * e->curstate.renderamt );
			blend += gEngfuncs.COM_RandomLong( -32, 31 );
		}
		break;
	}
	default:
		blend = e->curstate.renderamt;
		break;
	}

	blend = bound( 0, blend, 255 );

	return blend;
}
