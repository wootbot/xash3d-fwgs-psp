/*
gu_studio.c - studio model renderer
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
#include "const.h"
#include "r_studioint.h"
#include "triangleapi.h"
#include "studio.h"
#include "pm_local.h"
#include "gu_extension.h"
#include <stdio.h>
#include <malloc.h>
#include <pspkernel.h>

#define EVENT_CLIENT	5000	// less than this value it's a server-side studio events
#define MAX_LOCALLIGHTS	4

// GoldSrc limit (studiomdl), Xash3D allows 16384: the per-vertex arrays below would take 2 MB
#undef MAXSTUDIOVERTS
#define MAXSTUDIOVERTS	2048

typedef struct
{
	char		name[MAX_OSPATH];
	char		modelname[MAX_OSPATH];
	model_t		*model;
} player_model_t;

// never gonna change, just shut up const warning
CVAR_DEFINE_AUTO( r_shadows, "0", 0, "draw ugly shadows" );

static const vec3_t hullcolor[8] =
{
{ 1.0f, 1.0f, 1.0f },
{ 1.0f, 0.5f, 0.5f },
{ 0.5f, 1.0f, 0.5f },
{ 1.0f, 1.0f, 0.5f },
{ 0.5f, 0.5f, 1.0f },
{ 1.0f, 0.5f, 1.0f },
{ 0.5f, 1.0f, 1.0f },
{ 1.0f, 1.0f, 1.0f },
};

typedef struct sortedmesh_s
{
	mstudiomesh_t	*mesh;
	int		flags;			// face flags
} sortedmesh_t;

typedef struct
{
	double		time;
	double		frametime;
	int		framecount;		// studio framecount
	qboolean		interpolate;
	int		rendermode;
	float		blend;			// blend value

	// bones
	matrix3x4		rotationmatrix;
	matrix3x4		bonestransform[MAXSTUDIOBONES];
	matrix3x4		lighttransform[MAXSTUDIOBONES];

	// boneweighting stuff
	matrix3x4		worldtransform[MAXSTUDIOBONES];

	// cached bones
	matrix3x4		cached_bonestransform[MAXSTUDIOBONES];
	matrix3x4		cached_lighttransform[MAXSTUDIOBONES];
	char		cached_bonenames[MAXSTUDIOBONES][32];
	int		cached_numbones;		// number of bones in cache
#if XASH_PSP
	const mstudiobone_t	*cached_bones;	// R_StudioSaveBones: still to copy from bonestransform
#endif

	sortedmesh_t	meshes[MAXSTUDIOMESHES];	// sorted meshes
	vec3_t		verts[MAXSTUDIOVERTS];
	vec3_t		norms[MAXSTUDIOVERTS];

	// lighting state
	float		ambientlight;
	float		shadelight;
	vec3_t		lightvec;			// averaging light direction
	vec3_t		lightspot;		// shadow spot
	vec3_t		lightcolor;		// averaging lightcolor
	vec3_t		blightvec[MAXSTUDIOBONES];	// bone light vecs
	vec3_t		lightvalues[MAXSTUDIOVERTS];	// precomputed lightvalues per each shared vertex of submodel
	uint		lightcolors[MAXSTUDIOVERTS];	// the same as GU colors without alpha, for models without local lights

	// chrome stuff
	vec3_t		chrome_origin;
	vec2_t		chrome[MAXSTUDIOVERTS];	// texture coords for surface normals
	vec3_t		chromeright[MAXSTUDIOBONES];	// chrome vector "right" in bone reference frames
	vec3_t		chromeup[MAXSTUDIOBONES];	// chrome vector "up" in bone reference frames
	int		chromeage[MAXSTUDIOBONES];	// last time chrome vectors were updated

	// glowshell stuff
	int		normaltable[MAXSTUDIOVERTS];	// glowshell uses this

	// elights cache
	int		numlocallights;
	int		lightage[MAXSTUDIOBONES];
	dlight_t		*locallight[MAX_LOCALLIGHTS];
	int		locallightcolor[MAX_LOCALLIGHTS][3];
	vec4_t		lightpos[MAXSTUDIOVERTS][MAX_LOCALLIGHTS];
	vec3_t		lightbonepos[MAXSTUDIOBONES][MAX_LOCALLIGHTS];
	float		locallightR2[MAX_LOCALLIGHTS];

	// playermodels
	player_model_t  player_models[MAX_CLIENTS];

	// drawelements renderer
	uint			numverts;
	uint			numelems;
} studio_draw_state_t;

// studio-related cvars
CVAR_DEFINE_AUTO( r_studio_sort_textures, "0", FCVAR_GLCONFIG, "change draw order for additive meshes" );
#if XASH_PSP
// the PSP optimizations (VFPU bone math) are in this one, measured faster
CVAR_DEFINE_AUTO( r_studio_builtin_renderer, "1", 0, "use built-in studio model renderer instead of the one provided by client library" );
#else
CVAR_DEFINE_AUTO( r_studio_builtin_renderer, "0", 0, "use built-in studio model renderer instead of the one provided by client library (debugging)" );
#endif
static cvar_t			*cl_righthand = NULL;

static r_studio_interface_t	*pStudioDraw;
static studio_draw_state_t	g_studio;		// global studio state

// global variables
static qboolean m_fDoRemap;
static mstudiomodel_t *m_pSubModel;
static mstudiobodyparts_t *m_pBodyPart;
static player_info_t *m_pPlayerInfo;
static studiohdr_t *m_pStudioHeader;
static float m_flGaitMovement;
static int g_nTopColor, g_nBottomColor;	// remap colors
static int g_nFaceFlags, g_nForceFaceFlags;

/*
====================
R_StudioInit

====================
*/
#if XASH_PSP
static void R_StudioMeEarly( qboolean complete );
static convar_t r_studio_me, r_studio_me_draw, r_studio_me_players, r_studio_precull, r_studio_ge_elights, r_studio_minpixels, r_studio_minpixels_small, r_studio_propcache, r_studio_proplist, r_studio_geplan, r_studio_atlas, r_studio_ge_hwlight;
#endif

void R_StudioInit( void )
{

#if XASH_PSVITA
	// don't do the same array-building work twice since that's what our FFP shim does anyway
	gEngfuncs.Cvar_FullSet( "r_studio_drawelements", "0", FCVAR_READ_ONLY );
#endif

	Matrix3x4_LoadIdentity( g_studio.rotationmatrix );
#if XASH_PSP
	gEngfuncs.Cvar_RegisterVariable( &r_studio_precull );
	gEngfuncs.Cvar_RegisterVariable( &r_studio_ge_hwlight );
	gEngfuncs.Cvar_RegisterVariable( &r_studio_ge_elights );
	gEngfuncs.Cvar_RegisterVariable( &r_studio_atlas );
#if XASH_PSP
	gEngfuncs.Cvar_RegisterVariable( &r_studio_minpixels );
	gEngfuncs.Cvar_RegisterVariable( &r_studio_minpixels_small );
	gEngfuncs.Cvar_RegisterVariable( &r_studio_propcache );
	gEngfuncs.Cvar_RegisterVariable( &r_studio_proplist );
	gEngfuncs.Cvar_RegisterVariable( &r_studio_geplan );
	gEngfuncs.Cvar_RegisterVariable( &r_studio_me );
	gEngfuncs.Cvar_RegisterVariable( &r_studio_me_players );
	psp_entities_hook = R_StudioMeEarly;
	gEngfuncs.Cvar_RegisterVariable( &r_studio_me_draw );
#endif
#endif

	g_studio.interpolate = true;
	g_studio.framecount = 0;
	m_fDoRemap = false;
}

/*
================
R_StudioSetupTimings

init current time for a given model
================
*/
static void R_StudioSetupTimings( void )
{
	if( FBitSet( RI.rvp.flags, RF_DRAW_WORLD ))
	{
		// synchronize with server time
		g_studio.time = gp_cl->time;
		g_studio.frametime = gp_cl->time - gp_cl->oldtime;
	}
	else
	{
		// menu stuff
		g_studio.time = gp_host->realtime;
		g_studio.frametime = gp_host->frametime;
	}
}

/*
================
R_AllowFlipViewModel

should a flip the viewmodel if cl_righthand is set to 1
================
*/
static qboolean R_AllowFlipViewModel( cl_entity_t *e )
{
	if( cl_righthand && cl_righthand->value > 0 )
	{
		if( e == tr.viewent )
			return true;
	}

	return false;
}

/*
================
R_StudioComputeBBox

Compute a full bounding box for current sequence
================
*/
static qboolean R_StudioComputeBBox( vec3_t bbox[8] )
{
	cl_entity_t	*e = RI.currententity;

	if( !m_pStudioHeader )
		return false;

	vec3_t mins, maxs;
	// check if we have valid mins\maxs
	if( !VectorIsNull( RI.currentmodel->mins ) && !VectorIsNull( RI.currentmodel->maxs ))
	{
		// clipping bounding box
		VectorCopy( RI.currentmodel->mins, mins );
		VectorCopy( RI.currentmodel->maxs, maxs );
	}
	else
	{
		ClearBounds( mins, maxs );
	}

	// check sequence range
	if( e->curstate.sequence < 0 || e->curstate.sequence >= m_pStudioHeader->numseq )
		e->curstate.sequence = 0;

	mstudioseqdesc_t *pseqdesc = (mstudioseqdesc_t *)((byte *)m_pStudioHeader + m_pStudioHeader->seqindex) + e->curstate.sequence;

	// add sequence box to the model box
	AddPointToBounds( pseqdesc->bbmin, mins, maxs );
	AddPointToBounds( pseqdesc->bbmax, mins, maxs );
	vec3_t studio_mins, studio_maxs;
	ClearBounds( studio_mins, studio_maxs );

	// compute a full bounding box
	for( int i = 0; i < 8; i++ )
	{
		vec3_t p1, p2;
  		p1[0] = ( i & 1 ) ? mins[0] : maxs[0];
  		p1[1] = ( i & 2 ) ? mins[1] : maxs[1];
  		p1[2] = ( i & 4 ) ? mins[2] : maxs[2];

		Matrix3x4_VectorTransform( g_studio.rotationmatrix, p1, p2 );
		AddPointToBounds( p2, studio_mins, studio_maxs );
		if( bbox ) VectorCopy( p2, bbox[i] );
	}

	if( !bbox && R_CullModel( e, studio_mins, studio_maxs ))
		return false; // model culled
	return true; // visible
}

static void R_StudioComputeSkinMatrix( mstudioboneweight_t *boneweights, matrix3x4 result )
{
	int	numbones = 0;

	for( int i = 0; i < MAXSTUDIOBONEWEIGHTS; i++ )
	{
		if( boneweights->bone[i] != -1 )
			numbones++;
	}

	if( numbones == 4 )
	{
		vec4_t *boneMat0 = (vec4_t *)g_studio.worldtransform[boneweights->bone[0]];
		vec4_t *boneMat1 = (vec4_t *)g_studio.worldtransform[boneweights->bone[1]];
		vec4_t *boneMat2 = (vec4_t *)g_studio.worldtransform[boneweights->bone[2]];
		vec4_t *boneMat3 = (vec4_t *)g_studio.worldtransform[boneweights->bone[3]];
		float flWeight0 = boneweights->weight[0] / 255.0f;
		float flWeight1 = boneweights->weight[1] / 255.0f;
		float flWeight2 = boneweights->weight[2] / 255.0f;
		float flWeight3 = boneweights->weight[3] / 255.0f;
		float flTotal = flWeight0 + flWeight1 + flWeight2 + flWeight3;

		if( flTotal < 1.0f ) flWeight0 += 1.0f - flTotal;	// compensate rounding error

		result[0][0] = boneMat0[0][0] * flWeight0 + boneMat1[0][0] * flWeight1 + boneMat2[0][0] * flWeight2 + boneMat3[0][0] * flWeight3;
		result[0][1] = boneMat0[0][1] * flWeight0 + boneMat1[0][1] * flWeight1 + boneMat2[0][1] * flWeight2 + boneMat3[0][1] * flWeight3;
		result[0][2] = boneMat0[0][2] * flWeight0 + boneMat1[0][2] * flWeight1 + boneMat2[0][2] * flWeight2 + boneMat3[0][2] * flWeight3;
		result[0][3] = boneMat0[0][3] * flWeight0 + boneMat1[0][3] * flWeight1 + boneMat2[0][3] * flWeight2 + boneMat3[0][3] * flWeight3;
		result[1][0] = boneMat0[1][0] * flWeight0 + boneMat1[1][0] * flWeight1 + boneMat2[1][0] * flWeight2 + boneMat3[1][0] * flWeight3;
		result[1][1] = boneMat0[1][1] * flWeight0 + boneMat1[1][1] * flWeight1 + boneMat2[1][1] * flWeight2 + boneMat3[1][1] * flWeight3;
		result[1][2] = boneMat0[1][2] * flWeight0 + boneMat1[1][2] * flWeight1 + boneMat2[1][2] * flWeight2 + boneMat3[1][2] * flWeight3;
		result[1][3] = boneMat0[1][3] * flWeight0 + boneMat1[1][3] * flWeight1 + boneMat2[1][3] * flWeight2 + boneMat3[1][3] * flWeight3;
		result[2][0] = boneMat0[2][0] * flWeight0 + boneMat1[2][0] * flWeight1 + boneMat2[2][0] * flWeight2 + boneMat3[2][0] * flWeight3;
		result[2][1] = boneMat0[2][1] * flWeight0 + boneMat1[2][1] * flWeight1 + boneMat2[2][1] * flWeight2 + boneMat3[2][1] * flWeight3;
		result[2][2] = boneMat0[2][2] * flWeight0 + boneMat1[2][2] * flWeight1 + boneMat2[2][2] * flWeight2 + boneMat3[2][2] * flWeight3;
		result[2][3] = boneMat0[2][3] * flWeight0 + boneMat1[2][3] * flWeight1 + boneMat2[2][3] * flWeight2 + boneMat3[2][3] * flWeight3;
	}
	else if( numbones == 3 )
	{
		vec4_t *boneMat0 = (vec4_t *)g_studio.worldtransform[boneweights->bone[0]];
		vec4_t *boneMat1 = (vec4_t *)g_studio.worldtransform[boneweights->bone[1]];
		vec4_t *boneMat2 = (vec4_t *)g_studio.worldtransform[boneweights->bone[2]];
		float flWeight0 = boneweights->weight[0] / 255.0f;
		float flWeight1 = boneweights->weight[1] / 255.0f;
		float flWeight2 = boneweights->weight[2] / 255.0f;
		float flTotal = flWeight0 + flWeight1 + flWeight2;

		if( flTotal < 1.0f ) flWeight0 += 1.0f - flTotal;	// compensate rounding error

		result[0][0] = boneMat0[0][0] * flWeight0 + boneMat1[0][0] * flWeight1 + boneMat2[0][0] * flWeight2;
		result[0][1] = boneMat0[0][1] * flWeight0 + boneMat1[0][1] * flWeight1 + boneMat2[0][1] * flWeight2;
		result[0][2] = boneMat0[0][2] * flWeight0 + boneMat1[0][2] * flWeight1 + boneMat2[0][2] * flWeight2;
		result[0][3] = boneMat0[0][3] * flWeight0 + boneMat1[0][3] * flWeight1 + boneMat2[0][3] * flWeight2;
		result[1][0] = boneMat0[1][0] * flWeight0 + boneMat1[1][0] * flWeight1 + boneMat2[1][0] * flWeight2;
		result[1][1] = boneMat0[1][1] * flWeight0 + boneMat1[1][1] * flWeight1 + boneMat2[1][1] * flWeight2;
		result[1][2] = boneMat0[1][2] * flWeight0 + boneMat1[1][2] * flWeight1 + boneMat2[1][2] * flWeight2;
		result[1][3] = boneMat0[1][3] * flWeight0 + boneMat1[1][3] * flWeight1 + boneMat2[1][3] * flWeight2;
		result[2][0] = boneMat0[2][0] * flWeight0 + boneMat1[2][0] * flWeight1 + boneMat2[2][0] * flWeight2;
		result[2][1] = boneMat0[2][1] * flWeight0 + boneMat1[2][1] * flWeight1 + boneMat2[2][1] * flWeight2;
		result[2][2] = boneMat0[2][2] * flWeight0 + boneMat1[2][2] * flWeight1 + boneMat2[2][2] * flWeight2;
		result[2][3] = boneMat0[2][3] * flWeight0 + boneMat1[2][3] * flWeight1 + boneMat2[2][3] * flWeight2;
	}
	else if( numbones == 2 )
	{
		vec4_t *boneMat0 = (vec4_t *)g_studio.worldtransform[boneweights->bone[0]];
		vec4_t *boneMat1 = (vec4_t *)g_studio.worldtransform[boneweights->bone[1]];
		float flWeight0 = boneweights->weight[0] / 255.0f;
		float flWeight1 = boneweights->weight[1] / 255.0f;
		float flTotal = flWeight0 + flWeight1;

		if( flTotal < 1.0f ) flWeight0 += 1.0f - flTotal;	// compensate rounding error

		result[0][0] = boneMat0[0][0] * flWeight0 + boneMat1[0][0] * flWeight1;
		result[0][1] = boneMat0[0][1] * flWeight0 + boneMat1[0][1] * flWeight1;
		result[0][2] = boneMat0[0][2] * flWeight0 + boneMat1[0][2] * flWeight1;
		result[0][3] = boneMat0[0][3] * flWeight0 + boneMat1[0][3] * flWeight1;
		result[1][0] = boneMat0[1][0] * flWeight0 + boneMat1[1][0] * flWeight1;
		result[1][1] = boneMat0[1][1] * flWeight0 + boneMat1[1][1] * flWeight1;
		result[1][2] = boneMat0[1][2] * flWeight0 + boneMat1[1][2] * flWeight1;
		result[1][3] = boneMat0[1][3] * flWeight0 + boneMat1[1][3] * flWeight1;
		result[2][0] = boneMat0[2][0] * flWeight0 + boneMat1[2][0] * flWeight1;
		result[2][1] = boneMat0[2][1] * flWeight0 + boneMat1[2][1] * flWeight1;
		result[2][2] = boneMat0[2][2] * flWeight0 + boneMat1[2][2] * flWeight1;
		result[2][3] = boneMat0[2][3] * flWeight0 + boneMat1[2][3] * flWeight1;
	}
	else
	{
		Matrix3x4_Copy( result, g_studio.worldtransform[boneweights->bone[0]] );
	}
}

/*
===============
pfnGetCurrentEntity

===============
*/
static cl_entity_t *pfnGetCurrentEntity( void )
{
	return RI.currententity;
}

/*
===============
pfnPlayerInfo

===============
*/
player_info_t *pfnPlayerInfo( int index )
{
	if( !FBitSet( RI.rvp.flags, RF_DRAW_WORLD ))
		index = -1;

	return gEngfuncs.pfnPlayerInfo( index );
}

/*
===============
pfnGetPlayerState

===============
*/
static entity_state_t *R_StudioGetPlayerState( int index )
{
	if( !FBitSet( RI.rvp.flags, RF_DRAW_WORLD ) && RI.currententity )
		return &RI.currententity->curstate;

	return gEngfuncs.pfnGetPlayerState( index );
}

/*
===============
pfnGetEngineTimes

===============
*/
static void pfnGetEngineTimes( int *framecount, double *current, double *old )
{
	if( framecount ) *framecount = tr.realframecount;
	if( current ) *current = gp_cl->time;
	if( old ) *old =   gp_cl->oldtime;
}

/*
===============
pfnGetViewInfo

===============
*/
static void pfnGetViewInfo( float *origin, float *upv, float *rightv, float *forwardv )
{
	if( origin ) VectorCopy( RI.rvp.vieworigin, origin );
	if( forwardv ) VectorCopy( RI.vforward, forwardv );
	if( rightv ) VectorCopy( RI.vright, rightv );
	if( upv ) VectorCopy( RI.vup, upv );
}

/*
===============
pfnGetModelCounters

===============
*/
static void pfnGetModelCounters( int **s, int **a )
{
	*s = &g_studio.framecount;
	*a = &r_stats.c_studio_models_drawn;
}

/*
===============
pfnStudioGetBoneTransform

===============
*/
static float ****pfnStudioGetBoneTransform( void )
{
	return (float ****)g_studio.bonestransform;
}

/*
===============
pfnStudioGetLightTransform

===============
*/
static float ****pfnStudioGetLightTransform( void )
{
	return (float ****)g_studio.lighttransform;
}

/*
===============
pfnStudioGetRotationMatrix

===============
*/
static float ***pfnStudioGetRotationMatrix( void )
{
	return (float ***)g_studio.rotationmatrix;
}

/*
====================
StudioPlayerBlend

====================
*/
static void R_StudioPlayerBlend( mstudioseqdesc_t *pseqdesc, int *pBlend, float *pPitch )
{
	// calc up/down pointing
	*pBlend = (*pPitch * 3.0f);

	if( *pBlend < pseqdesc->blendstart[0] )
	{
		*pPitch -= pseqdesc->blendstart[0] / 3.0f;
		*pBlend = 0;
	}
	else if( *pBlend > pseqdesc->blendend[0] )
	{
		*pPitch -= pseqdesc->blendend[0] / 3.0f;
		*pBlend = 255;
	}
	else
	{
		if( pseqdesc->blendend[0] - pseqdesc->blendstart[0] < 0.1f ) // catch qc error
			*pBlend = 127;
		else *pBlend = 255 * (*pBlend - pseqdesc->blendstart[0]) / (pseqdesc->blendend[0] - pseqdesc->blendstart[0]);
		*pPitch = 0.0f;
	}
}

/*
====================
R_StudioLerpMovement

====================
*/
void R_StudioLerpMovement( cl_entity_t *e, double time, vec3_t origin, vec3_t angles )
{
	float	f = 1.0f;

	// don't do it if the goalstarttime hasn't updated in a while.
	// NOTE: Because we need to interpolate multiplayer characters, the interpolation time limit
	// was increased to 1.0 s., which is 2x the max lag we are accounting for.
	// float math (no doubles in the PSP FPU), only the time difference needs a double
	if( g_studio.interpolate && ( e->curstate.animtime != e->latched.prevanimtime ))
	{
		const float dt = time - e->curstate.animtime;

		if( dt < 1.0f )
			f = dt / ( e->curstate.animtime - e->latched.prevanimtime );
	}

	// Con_Printf( "%4.2f %.2f %.2f\n", f, e->curstate.animtime, g_studio.time );
	VectorLerp( e->latched.prevorigin, f, e->curstate.origin, origin );

	if( !VectorCompareEpsilon( e->curstate.angles, e->latched.prevangles, ON_EPSILON ))
	{
		vec4_t	q, q1, q2;
		AngleQuaternion( e->curstate.angles, q1, false );
		AngleQuaternion( e->latched.prevangles, q2, false );
		QuaternionSlerp( q2, q1, f, q );
		QuaternionAngle( q, angles );
	}
	else VectorCopy( e->curstate.angles, angles );
}

/*
====================
StudioSetUpTransform

====================
*/
static void R_StudioSetUpTransform( cl_entity_t *e )
{
	vec3_t origin = Vec3( e->origin );
	vec3_t angles = Vec3( e->angles );

	// interpolate monsters position (moved into UpdateEntityFields by user request)
	if( e->curstate.movetype == MOVETYPE_STEP && !FBitSet( gp_host->features, ENGINE_COMPUTE_STUDIO_LERP ))
	{
		R_StudioLerpMovement( e, g_studio.time, origin, angles );
	}

	if( !FBitSet( gp_host->features, ENGINE_COMPENSATE_QUAKE_BUG ))
		angles[PITCH] = -angles[PITCH]; // stupid quake bug

	// don't rotate clients, only aim
	if( e->player ) angles[PITCH] = 0.0f;

	Matrix3x4_CreateFromEntity( g_studio.rotationmatrix, angles, origin, 1.0f );

	if( tr.fFlipViewModel )
	{
		g_studio.rotationmatrix[0][1] = -g_studio.rotationmatrix[0][1];
		g_studio.rotationmatrix[1][1] = -g_studio.rotationmatrix[1][1];
		g_studio.rotationmatrix[2][1] = -g_studio.rotationmatrix[2][1];
	}
}

/*
====================
StudioEstimateFrame

====================
*/
float R_StudioEstimateFrame( cl_entity_t *e, mstudioseqdesc_t *pseqdesc, double time )
{
	// float math (no doubles in the PSP FPU), only the time difference needs a double
	float	dfdt, f;

	if( g_studio.interpolate )
	{
		const float dt = time - e->curstate.animtime;

		if( dt < 0.0f ) dfdt = 0.0f;
		else dfdt = dt * e->curstate.framerate * pseqdesc->fps;
	}
	else dfdt = 0.0f;

	if( pseqdesc->numframes <= 1 ) f = 0.0f;
	else f = (e->curstate.frame * (pseqdesc->numframes - 1)) * ( 1.0f / 256.0f );

	f += dfdt;

	if( pseqdesc->flags & STUDIO_LOOPING )
	{
		if( pseqdesc->numframes > 1 )
			f -= (int)(f / (pseqdesc->numframes - 1)) *  (pseqdesc->numframes - 1);
		if( f < 0 ) f += (pseqdesc->numframes - 1);
	}
	else
	{
		if( f >= pseqdesc->numframes - 1.001f )
			f = pseqdesc->numframes - 1.001f;
		if( f < 0.0f )  f = 0.0f;
	}
	return f;
}

/*
====================
StudioEstimateInterpolant

====================
*/
static float R_StudioEstimateInterpolant( cl_entity_t *e )
{
	float	dadt = 1.0f;

	if( g_studio.interpolate && ( e->curstate.animtime >= e->latched.prevanimtime + 0.01f ))
	{
		dadt = (float)( g_studio.time - e->curstate.animtime ) * ( 1.0f / 0.1f );
		if( dadt > 2.0f ) dadt = 2.0f;
	}

	return dadt;
}

/*
====================
StudioFxTransform

====================
*/
static void R_StudioFxTransform( cl_entity_t *ent, matrix3x4 transform )
{
	switch( ent->curstate.renderfx )
	{
	case kRenderFxDistort:
	case kRenderFxHologram:
		if( !gEngfuncs.COM_RandomLong( 0, 49 ))
		{
			int	axis = gEngfuncs.COM_RandomLong( 0, 1 );

			if( axis == 1 ) axis = 2; // choose between x & z
			VectorScale( transform[axis], gEngfuncs.COM_RandomFloat( 1.0f, 1.484f ), transform[axis] );
		}
		else if( !gEngfuncs.COM_RandomLong( 0, 49 ))
		{
			int	axis = gEngfuncs.COM_RandomLong( 0, 1 );

			if( axis == 1 ) axis = 2; // choose between x & z
			float offset = gEngfuncs.COM_RandomFloat( -10.0f, 10.0f );
			transform[gEngfuncs.COM_RandomLong( 0, 2 )][3] += offset;
		}
		break;
	case kRenderFxExplode:
		{
			float scale = 1.0f + ( g_studio.time - ent->curstate.animtime ) * 10.0f;
			if( scale > 2.0f ) scale = 2.0f; // don't blow up more than 200%

			transform[0][1] *= scale;
			transform[1][1] *= scale;
			transform[2][1] *= scale;
		}
		break;
	}
}

/*
====================
StudioCalcBoneAdj

====================
*/
static void R_StudioBoneAdj( const studiohdr_t *hdr, float dadt, float *adj, const byte *pcontroller1, const byte *pcontroller2, byte mouthopen )
{
	const mstudiobonecontroller_t	*pbonecontroller = (const mstudiobonecontroller_t *)((const byte *)hdr + hdr->bonecontrollerindex);

	for( int j = 0; j < hdr->numbonecontrollers; j++ )
	{
		float value = 0.0f;
		int i = pbonecontroller[j].index;

		if( i == STUDIO_MOUTH )
		{
			// mouth hardcoded at controller 4
			value = (float)mouthopen / 64.0f;
			value = bound( 0.0f, value, 1.0f );
			value = (1.0f - value) * pbonecontroller[j].start + value * pbonecontroller[j].end;
		}
		else if( i < 4 )
		{
			// check for 360% wrapping
			if( FBitSet( pbonecontroller[j].type, STUDIO_RLOOP ))
			{
				if( abs( pcontroller1[i] - pcontroller2[i] ) > 128 )
				{
					int a = (pcontroller1[i] + 128) % 256;
					int b = (pcontroller2[i] + 128) % 256;
					value = (( a * dadt ) + ( b * ( 1.0f - dadt )) - 128) * (360.0f / 256.0f) + pbonecontroller[j].start;
				}
				else
				{
					value = ((pcontroller1[i] * dadt + (pcontroller2[i]) * (1.0f - dadt))) * (360.0f / 256.0f) + pbonecontroller[j].start;
				}
			}
			else
			{
				value = (pcontroller1[i] * dadt + pcontroller2[i] * (1.0f - dadt)) / 255.0f;
				value = bound( 0.0f, value, 1.0f );
				value = (1.0f - value) * pbonecontroller[j].start + value * pbonecontroller[j].end;
			}
		}

		switch( pbonecontroller[j].type & STUDIO_TYPES )
		{
		case STUDIO_XR:
		case STUDIO_YR:
		case STUDIO_ZR:
			adj[j] = DEG2RAD( value );
			break;
		case STUDIO_X:
		case STUDIO_Y:
		case STUDIO_Z:
			adj[j] = value;
			break;
		}
	}
}

static void R_StudioCalcBoneAdj( float dadt, float *adj, const byte *pcontroller1, const byte *pcontroller2, byte mouthopen )
{
	R_StudioBoneAdj( m_pStudioHeader, dadt, adj, pcontroller1, pcontroller2, mouthopen );
}

/*
====================
StudioCalcRotations

====================
*/
static void R_StudioCalcRotations( cl_entity_t *e, float pos[][3], vec4_t *q, mstudioseqdesc_t *pseqdesc, mstudioanim_t *panim, float f )
{
	float adj[MAXSTUDIOCONTROLLERS];

	// bah, fix this bug with changing sequences too fast
	if( f > pseqdesc->numframes - 1 )
	{
		f = 0.0f;
	}
	else if( f < -0.01f )
	{
		// BUG ( somewhere else ) but this code should validate this data.
		// This could cause a crash if the frame # is negative, so we'll go ahead
		// and clamp it here
		f = -0.01f;
	}

	int frame = (int)f;
	float dadt = R_StudioEstimateInterpolant( e );
	float s = (f - frame);

	// add in programtic controllers
	mstudiobone_t *pbone = (mstudiobone_t *)((byte *)m_pStudioHeader + m_pStudioHeader->boneindex);

	R_StudioCalcBoneAdj( dadt, adj, e->curstate.controller, e->latched.prevcontroller, e->mouth.mouthopen );

#if XASH_PSP
	// the rotations as angles, then their quaternions 4 at a time on the VFPU
	static float ang[MAXSTUDIOBONES][4] __attribute__(( aligned( 16 )));
	static vec4_t slowq[MAXSTUDIOBONES];
	static int slowbone[MAXSTUDIOBONES];
	const int numbones = m_pStudioHeader->numbones;
	int numslow = 0;

	for( int i = 0; i < numbones; i++, pbone++, panim++ )
	{
		if( !PSP_StudioCalcBoneAngles( frame, s, pbone, panim, adj, pos[i], ang[i], slowq[numslow] ))
			slowbone[numslow++] = i;
	}

	// q has room for MAXSTUDIOBONES, a multiple of 4
	PSP_StudioAnglesToQuaternions( (const float (*)[4])ang, q, numbones );
	for( int i = 0; i < numslow; i++ )
		Vector4Copy( slowq[i], q[slowbone[i]] );
#else
	for( int i = 0; i < m_pStudioHeader->numbones; i++, pbone++, panim++ )
		R_StudioCalcBones( frame, s, pbone, panim, adj, pos[i], q[i] );
#endif

	if( pseqdesc->motiontype & STUDIO_X ) pos[pseqdesc->motionbone][0] = 0.0f;
	if( pseqdesc->motiontype & STUDIO_Y ) pos[pseqdesc->motionbone][1] = 0.0f;
	if( pseqdesc->motiontype & STUDIO_Z ) pos[pseqdesc->motionbone][2] = 0.0f;
}

/*
====================
StudioMergeBones

====================
*/
#if XASH_PSP
static void R_StudioCopySavedBones( void );
#endif

static void R_StudioMergeBones( cl_entity_t *e, model_t *m_pSubModel )
{
	static vec4_t	q[MAXSTUDIOBONES];
	static float	pos[MAXSTUDIOBONES][3];

#if XASH_PSP
	R_StudioCopySavedBones();
#endif

	if( e->curstate.sequence >=  m_pStudioHeader->numseq )
		e->curstate.sequence = 0;

	mstudioseqdesc_t *pseqdesc = (mstudioseqdesc_t *)((byte *)m_pStudioHeader + m_pStudioHeader->seqindex) + e->curstate.sequence;
	float f = R_StudioEstimateFrame( e, pseqdesc, g_studio.time );

	mstudioanim_t *panim = gEngfuncs.R_StudioGetAnim( m_pStudioHeader, m_pSubModel, pseqdesc );
	R_StudioCalcRotations( e, pos, q, pseqdesc, panim, f );
	mstudiobone_t *pbones = (mstudiobone_t *)((byte *)m_pStudioHeader + m_pStudioHeader->boneindex);

	for( int i = 0; i < m_pStudioHeader->numbones; i++ )
	{
		int j;
		for( j = 0; j < g_studio.cached_numbones; j++ )
		{
			if( !Q_stricmp( pbones[i].name, g_studio.cached_bonenames[j] ))
			{
				Matrix3x4_Copy( g_studio.bonestransform[i], g_studio.cached_bonestransform[j] );
				Matrix3x4_Copy( g_studio.lighttransform[i], g_studio.cached_lighttransform[j] );
				break;
			}
		}

		if( j >= g_studio.cached_numbones )
		{
			matrix3x4 bonematrix;
			Matrix3x4_FromOriginQuat( bonematrix, q[i], pos[i] );
			if( pbones[i].parent == -1 )
			{
				Matrix3x4_ConcatTransforms( g_studio.bonestransform[i], g_studio.rotationmatrix, bonematrix );
				Matrix3x4_Copy( g_studio.lighttransform[i], g_studio.bonestransform[i] );

				// apply client-side effects to the transformation matrix
				R_StudioFxTransform( e, g_studio.bonestransform[i] );
			}
			else
			{
				Matrix3x4_ConcatTransforms( g_studio.bonestransform[i], g_studio.bonestransform[pbones[i].parent], bonematrix );
				Matrix3x4_ConcatTransforms( g_studio.lighttransform[i], g_studio.lighttransform[pbones[i].parent], bonematrix );
			}
		}
	}
}

/*
====================
StudioSetupBones

====================
*/
#if XASH_PSP
/*
====================
R_StudioConcat

Matrix3x4_ConcatTransforms on the VFPU: a scalar product chain is ~2 us a bone
on the PSP's FPU. Rows of the 3x4 matrices load as columns (the transposes,
with 0 0 0 1 as the fourth column), and out^T = in2^T * in1^T. Which operand
order vmmul wants is checked against the C version on first use; if neither
matches, the C version is used. Matrices 1-3 only (ref_gu keeps state in 6, 7).
====================
*/
static int r_vfpu_concat = -1; // -1 untested, 0 C, 1 or 2 the vmmul operand order

static void R_StudioConcatVFPU( int order, matrix3x4 out, const matrix3x4 in1, const matrix3x4 in2 )
{
	if( order == 1 )
	{
		__asm__ volatile(
			"ulv.q   C100, 0(%1)\n"
			"ulv.q   C110, 16(%1)\n"
			"ulv.q   C120, 32(%1)\n"
			"vidt.q  C130\n"
			"ulv.q   C200, 0(%2)\n"
			"ulv.q   C210, 16(%2)\n"
			"ulv.q   C220, 32(%2)\n"
			"vidt.q  C230\n"
			"vmmul.q M300, M200, M100\n"
			"usv.q   C300, 0(%0)\n"
			"usv.q   C310, 16(%0)\n"
			"usv.q   C320, 32(%0)\n"
			:: "r"( out ), "r"( in1 ), "r"( in2 ) : "memory" );
	}
	else
	{
		__asm__ volatile(
			"ulv.q   C100, 0(%1)\n"
			"ulv.q   C110, 16(%1)\n"
			"ulv.q   C120, 32(%1)\n"
			"vidt.q  C130\n"
			"ulv.q   C200, 0(%2)\n"
			"ulv.q   C210, 16(%2)\n"
			"ulv.q   C220, 32(%2)\n"
			"vidt.q  C230\n"
			"vmmul.q M300, M100, M200\n"
			"usv.q   C300, 0(%0)\n"
			"usv.q   C310, 16(%0)\n"
			"usv.q   C320, 32(%0)\n"
			:: "r"( out ), "r"( in1 ), "r"( in2 ) : "memory" );
	}
}

static qboolean R_StudioConcatMatches( int order )
{
	static const matrix3x4 a = { { 0.36f, -0.48f, 0.8f, 10.0f }, { 0.8f, 0.6f, 0.0f, -20.0f }, { -0.48f, 0.64f, 0.6f, 30.0f } };
	static const matrix3x4 b = { { 0.6f, 0.0f, -0.8f, 1.5f }, { 0.0f, 1.0f, 0.0f, 2.5f }, { 0.8f, 0.0f, 0.6f, -3.5f } };
	matrix3x4 ref, out;

	Matrix3x4_ConcatTransforms( ref, a, b );
	R_StudioConcatVFPU( order, out, a, b );

	for( int i = 0; i < 3; i++ )
	{
		for( int j = 0; j < 4; j++ )
		{
			if( fabsf( ref[i][j] - out[i][j] ) > 1e-4f )
				return false;
		}
	}
	return true;
}

/*
Matrix3x4_FromOriginQuat with the quaternion in locals: the generic version
reloads it for every element (out may alias it) and chains the products,
~0.86 us a bone on the PSP's FPU
*/
static inline void R_StudioBoneMatrix( matrix3x4 out, const vec4_t q, const vec3_t origin )
{
	const float x = q[0], y = q[1], z = q[2], w = q[3];
	const float x2 = x + x, y2 = y + y, z2 = z + z;
	const float xx = x * x2, yy = y * y2, zz = z * z2;
	const float xy = x * y2, xz = x * z2, yz = y * z2;
	const float wx = w * x2, wy = w * y2, wz = w * z2;

	out[0][0] = 1.0f - ( yy + zz );
	out[0][1] = xy - wz;
	out[0][2] = xz + wy;
	out[0][3] = origin[0];
	out[1][0] = xy + wz;
	out[1][1] = 1.0f - ( xx + zz );
	out[1][2] = yz - wx;
	out[1][3] = origin[1];
	out[2][0] = xz - wy;
	out[2][1] = yz + wx;
	out[2][2] = 1.0f - ( xx + yy );
	out[2][3] = origin[2];
}

static inline void R_StudioConcat( matrix3x4 out, const matrix3x4 in1, const matrix3x4 in2 )
{
	if( unlikely( r_vfpu_concat < 0 ))
	{
		r_vfpu_concat = R_StudioConcatMatches( 1 ) ? 1 : R_StudioConcatMatches( 2 ) ? 2 : 0;
		gEngfuncs.Con_Printf( "studio: VFPU bone matrices %s\n", r_vfpu_concat ? "on" : "off (didn't match the FPU)" );
	}

	if( r_vfpu_concat )
		R_StudioConcatVFPU( r_vfpu_concat, out, in1, in2 );
	else Matrix3x4_ConcatTransforms( out, in1, in2 );
}

/*
====================
studio models on the Media Engine

Three jobs a frame on the ME (engine/platform/psp/me_psp.c). R_StudioMeEarly
posts the first as soon as the packet entities are in the draw lists: it tells
them apart (mecull_t: what the culling and the sort of the translucent ones
need, and who may draw it), then computes the bones of the models that were
visible in the last frame. The second does the same for the complete lists,
until the world's job is posted (the main CPU waits for that one first). With
the view known R_StudioMePrepare culls with the table and posts the third: the
other bones, and for the models the ME draws (medraw_t) the exact cull, the
attachments, the light and the GE commands. Only the GE waits for those
(r_mebones_gates); R_StudioFinishMeModels does events and attachments at the
end of the frame. R_StudioSetupBones takes the bones of the rest only if what
it sees matches what they were computed from (mebones_key_t), else it computes
them here.

The light: the traces for the floor's and the sky, with caches of the ME's own,
the dynamic lights of the moment the first job was posted, the entity lights of
the third. A light an event makes shows up a frame later than on the main CPU.

Translucent models are drawn the same way, each called where it's due in the
sorted list: blending and alpha are the main CPU's, the light and the rest the ME's.

Players: the gait is processed before the first job (R_StudioMePlayer), the
gait sequence's legs and the weapon model are the ME's.

The ME has no VFPU: its versions of the math are plain float code, with
their own sine and cosine. It reads the entities, the jobs and the models'
data and writes only cache lines the main CPU doesn't write to. Followers,
the view model and the random render effects stay on the main CPU.
====================
*/
static CVAR_DEFINE_AUTO( r_studio_me, "1", FCVAR_ARCHIVE, "cull studio models and compute the bones of animated ones on the Media Engine" );
static CVAR_DEFINE_AUTO( r_studio_me_draw, "1", FCVAR_ARCHIVE, "light them and write their draw commands there too, without the client DLL's renderer" );
static CVAR_DEFINE_AUTO( r_studio_me_players, "1", FCVAR_ARCHIVE, "players too (needs r_studio_me_draw)" );

#define MEBONES_MAXENTS	96
#define MEBONES_POOL	2048	// bones in the output pool
#define MEBONES_SCRATCH	( 2 * MAXSTUDIOBONES )	// and of a model with its weapon after them
#define MEBONES_ATTACHMENTS	4	// cl_entity_t.attachment
#define MEBONES_CMDS	98304	// GE commands of all models, ~1000 each
#define MEBONES_CMDSMIN	16384	// if there's no space for so many
#define MEBONES_MAXPLAYERS	32
#define MEMERGE_CACHE	16
#define MELIGHT_MAXPHYS	64	// brush models the floor light may come from
#define MELIGHT_MAXELIGHTS	16
#define MELIGHT_CACHE	256
#define MELIGHT_CELLS	512

// the light of a model, as alight_t
typedef struct
{
	float		ambient, shade;
	vec3_t		vec;
	float		color[3];
	uint32_t		floor;	// cl_entity_t.cvFloorColor, 8 bits each; bit 31: none
} melight_t;

typedef struct
{
	vec3_t		origin;
	float		radius;
	float		color[3];
} medlight_t;

typedef struct
{
	vec3_t		origin;
	float		r2;
	int		key;
	int		color[3];	// linear
} meelight_t;

// a brush model of the player's physents
typedef struct
{
	model_t		*model;
	qboolean		rotated;
	vec3_t		offset;
	matrix3x4		matrix;	// of a rotated one
} mephys_t;

typedef struct
{
	const studiohdr_t	*hdr;
	double		time;
	int		interpolate, sequence, prevsequence, renderfx;
	float		frame, animtime, framerate, prevanimtime, sequencetime, prevframe;
	byte		controller[4], prevcontroller[4], blending[2], prevblending[2], prevseqblending[2], mouthopen, pad;
} mebones_key_t;

typedef struct
{
	const mstudioanim_t	*panim;	// of the first blend
	const mstudioseqdesc_t	*pseqdesc;
	float		f;
	float		s[2];	// blend weights: [0] blends 1-2 and 3-4, [1] between the pairs
} mebones_seq_t;

// which bone of the player's model a weapon model's takes its transform from, -1: none
typedef struct
{
	const studiohdr_t	*hdr, *phdr;
	uint		frame;	// tr.framecount it was last used in
	signed char	map[MAXSTUDIOBONES];
} memerge_t;

// a player, from the main CPU
typedef struct
{
	int		index;	// in the table
	const model_t	*mod, *wmod;	// of the player and the weapon, or NULL
	const studiohdr_t	*hdr, *whdr;
	const memerge_t	*merge;	// of the weapon
	float		yaw;	// of the legs
	int		gaitsequence;	// 0: none
	float		gaitframe;
	int		number;
} meplayer_t;

// the ME's
typedef struct mebones_ent_s
{
	mebones_key_t	key;	// what the bones were computed from, and the rotation
	matrix3x4		rotation;
	const studiohdr_t	*hdr;
	mebones_seq_t	seq[3];	// the sequence, the previous one to blend from, the gait
	float		prevframe;	// what R_StudioSetupBones stores in latched.prevframe, if blendprev is off
	int		blendprev;
	float		sprev;
	float		adj[MAXSTUDIOCONTROLLERS];
	matrix3x4		*out;
	int		gait;
	const memerge_t	*merge;	// a weapon's
	struct mebones_ent_s *weapon;
	int		done;	// the bones
	int		lit;	// light is known, without the entity lights
	melight_t		light;
} __attribute__(( aligned( 64 ))) mebones_ent_t;

// what both jobs need to know about this frame
typedef struct
{
	double		time;
	int		interpolate;
	int		steplerp;	// the ones that step move between two positions
	int		negpitch;
	cl_entity_t	**solid, **trans;
	int		framecount;
	int		numplayers;
	meplayer_t	players[MEBONES_MAXPLAYERS];

	// if anything is drawn by R_StudioDrawMeModels
	qboolean		draw;
	float		frametime;
	const model_t	*world;
	qboolean		sky;	// there's sky light
	vec3_t		skyvec, skyend;	// direction, and the end of the trace from an origin
	uint		skycolor[3];
	float		direct;
	const uint16_t	*gamma;	// NULL: linear
	int		numdlights, numphys;
	medlight_t	dlights[MAX_DLIGHTS];
	mephys_t		phys[MELIGHT_MAXPHYS];
} mebones_frame_t;

typedef struct
{
	const mebones_frame_t *frame;
	int		firstsolid, numsolid, firsttrans, numtrans;	// new in the table
} mebones_early_t;

// one to draw, or with bones for R_StudioSetupBones
typedef struct
{
	cl_entity_t	*e;
	const struct medraw_s *draw, *wdraw;	// R_StudioDrawMeModels draws it
	int		index;	// in the table
	const meplayer_t	*player;
	int		mode;	// MEMODE
} mebones_item_t;

enum { MEMODE_SOLID = 0, MEMODE_BLEND, MEMODE_ADD };

typedef struct
{
	const mebones_frame_t *frame;
	uint		*cmds, *gates;	// r_mebones_cmds and r_mebones_gates of the frame
	int		count, numsolid;	// the solid ones come first
	int		words, maxwords;	// of their commands, at most
	mebones_item_t	items[MEBONES_MAXENTS];

	// for the ones with a medraw_t
	int		numplanes;
	float		planes[FRUSTUM_PLANES][4];	// normal, -dist
	vec3_t		eye, vright;
	int		numelights;
	meelight_t	elights[MELIGHT_MAXELIGHTS];
} mebones_job_t;

// of an item, two cache lines
typedef struct
{
	uint32_t		visible;	// of the ones with a medraw_t, else 1
	uint32_t		cmds;	// address of its commands, 0: none. The solid ones' are one list, of all
	mebones_ent_t	*ent;	// NULL: not on the ME
	uint32_t		blendprev;
	float		prevframe;
	uint32_t		events;	// R_StudioClientEvents may have something to do
	uint32_t		floor;	// of melight_t
	uint32_t		scratch;	// the bones are gone
	float		attachment[MEBONES_ATTACHMENTS][3];
	matrix3x4		rotation;
} mebones_res_t;

typedef char mebones_res_check[sizeof( mebones_res_t ) == 128 ? 1 : -1];
typedef char mecull_check[sizeof( mecull_t ) == 32 ? 1 : -1];

static struct
{
	mebones_frame_t	frame;
	mebones_early_t	early[2];
	mebones_job_t	job;
	uint32_t		ticket, early_ticket;
	qboolean		begun;	// the first early job is posted
	qboolean		finish;	// R_StudioFinishMeModels has something to do
	double		refused;	// R_StudioMeRefused until then
	qboolean		table;	// the table is this frame's
	qboolean		tableread;	// it's in this CPU's cache
	int		count;	// items valid for this frame's draw
	uint32_t		hits, misses;
	uint32_t		vert_frame;
	void		*alloc;	// what cmds are in, from the first frame on a map
	qboolean		allocfailed;
	uint		*cmds[2];	// by guRender.vert_frame & 1, as the gates
	int		words;	// each
} r_mebones __attribute__(( aligned( 64 )));

// written by the ME only, read by the main CPU after the progress counts say so
static mecull_t r_mecull[MECULL_MAX] __attribute__(( aligned( 64 )));
static mesort_t r_mesort[MECULL_MAXTRANS] __attribute__(( aligned( 64 )));
static matrix3x4 r_mebones_out[MEBONES_POOL + MEBONES_SCRATCH] __attribute__(( aligned( 64 )));
static mebones_res_t r_mebones_res[MEBONES_MAXENTS] __attribute__(( aligned( 64 )));

/*
The GE calls the list of the solid models, and those of the translucent ones,
through gates (only accessed uncached): BASE and a jump to itself, until the ME
is done with the commands and replaces the jump with a NOP, then BASE and a jump
to them. The main CPU doesn't wait for the models
*/
#define MEBONES_GATEWORDS	4
static uint r_mebones_gates[2][( MEBONES_MAXENTS + 4 ) * MEBONES_GATEWORDS] __attribute__(( aligned( 64 ))); // whole cache lines
#define MEBONES_SOLIDGATE	MEBONES_MAXENTS
static mebones_ent_t r_mebones_ents[MEBONES_MAXENTS * 2 + 2] __attribute__(( aligned( 64 )));

// its own, in cache lines of their own (aligned as a type: the size is too)
typedef struct
{
	const studiohdr_t	*hdr;
	int	length, numbones;
	char	name[16];
	byte	bones[MAXSTUDIOBONES];
} megait_t;

// the traces of an entity's light, as floorlight_t and the caches of R_EntitySeesSky
typedef struct
{
	const cl_entity_t	*ent;	// NULL: no floor light
	int	effects;
	float	time;
	vec3_t	origin;
	vec3_t	dir;
	uint	rgb[3];
	const cl_entity_t	*skyent;
	vec3_t	skyorigin;	// the trace's
	int	sky;
} melightcache_t;

typedef struct
{
	int	x, y, z;
	int	sky;	// 0: not known, 1: no, 2: yes
	vec3_t	origin;
} meskycell_t;

typedef struct
{
	int	count;	// of r_mebones_ents
	int	used;	// of r_mebones_out
	qboolean	scratch;	// both are full: the next ones are only for the commands
	uint	*p;	// end of the commands
	const cl_entity_t	*ents[MECULL_MAX];
	byte	slot[MECULL_MAX];	// of r_mebones_ents + 1, by table index
	uint16_t	seen[256];	// when an entity was last not culled, by a hash of its address: the low bits of tr.framecount
	megait_t	gait[4];	// R_MeGaitBones
	int	nextgait;
	const void	*lightkey[6];	// the map and its sky the caches are for
	melightcache_t	light[MELIGHT_CACHE];
	meskycell_t	skycells[MELIGHT_CELLS];
} __attribute__(( aligned( 64 ))) mebones_me_t;

static mebones_me_t r_mebones_me;

// only accessed uncached: items done, the table is complete, the early job should stop
static volatile uint32_t r_mebones_sync[16] __attribute__(( aligned( 64 )));
#define MEBONES_SYNC	((volatile uint32_t *)( 0x40000000 | (uintptr_t)r_mebones_sync ))
enum { MESYNC_ITEMS = 0, MESYNC_TABLE, MESYNC_YIELD };	// the number of the early job

// ME scratch: the rotations of the sequence, its blends, the previous sequence
static struct
{
	vec3_t	pos[5][MAXSTUDIOBONES];
	vec4_t	q[5][MAXSTUDIOBONES];
} __attribute__(( aligned( 64 ))) r_mebones_tmp;

/*
sine and cosine of x in float, ~1e-7 off: x reduced to [-pi/4, pi/4] around
a multiple of pi/2 (in two parts), then the minimax polynomials of Cephes' sinf and cosf
*/
static void R_MeSinCos( float x, float *s, float *c )
{
	const float t = x * ( 2.0f / M_PI_F );
	const int k = (int)( t >= 0.0f ? t + 0.5f : t - 0.5f );
	const float r = ( x - (float)k * 1.5703125f ) - (float)k * 4.8382679489659e-4f;
	const float r2 = r * r;
	const float sn = r + r * r2 * ( -1.6666654611e-1f + r2 * ( 8.3321608736e-3f + r2 * -1.9515295891e-4f ));
	const float cs = 1.0f - 0.5f * r2 + r2 * r2 * ( 4.166664568298827e-2f + r2 * ( -1.388731625493765e-3f + r2 * 2.443315711809948e-5f ));

	switch( k & 3 )
	{
	case 0: *s = sn; *c = cs; break;
	case 1: *s = cs; *c = -sn; break;
	case 2: *s = -sn; *c = -cs; break;
	default: *s = -cs; *c = sn; break;
	}
}

// AngleQuaternion( angles, q, true )
static void R_MeAngleQuaternion( const float *angles, vec4_t q )
{
	float sr, cr, sp, cp, sy, cy;

	R_MeSinCos( angles[0] * 0.5f, &sr, &cr );
	R_MeSinCos( angles[1] * 0.5f, &sp, &cp );
	R_MeSinCos( angles[2] * 0.5f, &sy, &cy );

	q[0] = sr * cp * cy - cr * sp * sy;
	q[1] = cr * sp * cy + sr * cp * sy;
	q[2] = cr * cp * sy - sr * sp * cy;
	q[3] = cr * cp * cy + sr * sp * sy;
}

// QuaternionSlerp: q aligned with p, then a normalized lerp below ~25 degrees apart (off by < 0.1)
static void R_MeQuaternionSlerp( const vec4_t p, const vec4_t q, float t, vec4_t qt )
{
	float q0 = q[0], q1 = q[1], q2 = q[2], q3 = q[3];
	float cosom = p[0] * q0 + p[1] * q1 + p[2] * q2 + p[3] * q3;
	float sclp, sclq;

	if( cosom < 0.0f )
	{
		q0 = -q0; q1 = -q1; q2 = -q2; q3 = -q3;
		cosom = -cosom;
	}

	if( cosom < 0.975f )
	{
		const float omega = acosf( cosom );
		float sinom, c, sa, sb;

		R_MeSinCos( omega, &sinom, &c );
		R_MeSinCos(( 1.0f - t ) * omega, &sa, &c );
		R_MeSinCos( t * omega, &sb, &c );
		sclp = sa / sinom;
		sclq = sb / sinom;
	}
	else
	{
		// the length of the lerp
		const float k = 1.0f / __builtin_sqrtf( 1.0f - 2.0f * t * ( 1.0f - t ) * ( 1.0f - cosom ));

		sclp = ( 1.0f - t ) * k;
		sclq = t * k;
	}

	qt[0] = sclp * p[0] + sclq * q0;
	qt[1] = sclp * p[1] + sclq * q1;
	qt[2] = sclp * p[2] + sclq * q2;
	qt[3] = sclp * p[3] + sclq * q3;
}

// gait: bones not to touch, or NULL
static void R_MeSlerpBones( int numbones, vec4_t q1[], float pos1[][3], const vec4_t q2[], const float pos2[][3], float s, const byte *gait )
{
	s = bound( 0.0f, s, 1.0f );

	for( int i = 0; i < numbones; i++ )
	{
		if( gait && gait[i] )
			continue;
		R_MeQuaternionSlerp( q1[i], q2[i], s, q1[i] );
		VectorLerp( pos1[i], s, pos2[i], pos1[i] );
	}
}

// R_StudioCalcRotations of a bone, s between its keyframes v1 and v2
static void R_MeCalcRotation( const float *v1, const float *v2, float s, vec3_t pos, vec4_t q )
{
	if( !VectorCompare( v1, v2 ))
		VectorLerp( v1, s, v2, pos );
	else VectorCopy( v1, pos );

	// as PSP_StudioCalcBoneAngles: neighbouring keyframes interpolate as angles
	const float d0 = v2[3] - v1[3], d1 = v2[4] - v1[4], d2 = v2[5] - v1[5];
	if( fabsf( d0 ) < 0.25f && fabsf( d1 ) < 0.25f && fabsf( d2 ) < 0.25f )
	{
		const vec3_t a = { v1[3] + s * d0, v1[4] + s * d1, v1[5] + s * d2 };
		R_MeAngleQuaternion( a, q );
	}
	else
	{
		vec4_t qa, qb;
		R_MeAngleQuaternion( &v1[3], qa );
		R_MeAngleQuaternion( &v2[3], qb );
		R_MeQuaternionSlerp( qa, qb, s, q );
	}
}

static int R_MeFrame( const mstudioseqdesc_t *pseqdesc, float f, float *s )
{
	if( f > pseqdesc->numframes - 1 )
		f = 0.0f;
	else if( f < -0.01f )
		f = -0.01f;

	const int frame = (int)f;
	*s = f - frame;
	return frame;
}

static void R_MeMotion( const mstudioseqdesc_t *pseqdesc, float pos[][3] )
{
	if( pseqdesc->motiontype & STUDIO_X ) pos[pseqdesc->motionbone][0] = 0.0f;
	if( pseqdesc->motiontype & STUDIO_Y ) pos[pseqdesc->motionbone][1] = 0.0f;
	if( pseqdesc->motiontype & STUDIO_Z ) pos[pseqdesc->motionbone][2] = 0.0f;
}

// R_StudioCalcRotations with the controller adjustments given, the bones with gait[i] == which
static void R_MeCalcRotations( const studiohdr_t *hdr, float pos[][3], vec4_t *q, const mstudioseqdesc_t *pseqdesc, const mstudioanim_t *panim, float f, const float *adj, const byte *gait, int which )
{
	const mstudiobone_t *pbone = (const mstudiobone_t *)((const byte *)hdr + hdr->boneindex);
	float s;
	const int frame = R_MeFrame( pseqdesc, f, &s );

	for( int i = 0; i < hdr->numbones; i++, pbone++, panim++ )
	{
		float v1[6], v2[6];

		if( gait && gait[i] != which )
			continue;
		PSP_StudioBoneValues( frame, pbone, panim, adj, v1, v2 );
		R_MeCalcRotation( v1, v2, s, pos[i], q[i] );
	}

	R_MeMotion( pseqdesc, pos );
}

/*
two blends at once: most bones of a player are the same in both (only the
spine aims up or down), they need no second rotation and no blend
*/
static void R_MeCalcRotations2( const studiohdr_t *hdr, float pos[][3], vec4_t *q, const mstudioseqdesc_t *pseqdesc, const mstudioanim_t *panim, float f, const float *adj, float blend, const byte *gait )
{
	const mstudiobone_t *pbone = (const mstudiobone_t *)((const byte *)hdr + hdr->boneindex);
	const mstudioanim_t *panim2 = panim + hdr->numbones;
	float s;
	const int frame = R_MeFrame( pseqdesc, f, &s );

	blend = bound( 0.0f, blend, 1.0f );

	for( int i = 0; i < hdr->numbones; i++, pbone++, panim++, panim2++ )
	{
		const uint16_t *o1 = panim->offset, *o2 = panim2->offset;
		float v1[6], v2[6], w1[6], w2[6];
		qboolean same = true;

		if( gait && gait[i] )
			continue;

		PSP_StudioBoneValues( frame, pbone, panim, adj, v1, v2 );

		// animated in one of them
		if( o1[0] | o1[1] | o1[2] | o1[3] | o1[4] | o1[5] | o2[0] | o2[1] | o2[2] | o2[3] | o2[4] | o2[5] )
		{
			PSP_StudioBoneValues( frame, pbone, panim2, adj, w1, w2 );
			for( int k = 0; k < 6 && same; k++ )
				same = v1[k] == w1[k] && v2[k] == w2[k];
		}

		R_MeCalcRotation( v1, v2, s, pos[i], q[i] );

		if( !same )
		{
			vec3_t pos2;
			vec4_t q2;

			R_MeCalcRotation( w1, w2, s, pos2, q2 );
			R_MeQuaternionSlerp( q[i], q2, blend, q[i] );
			VectorLerp( pos[i], blend, pos2, pos[i] );
		}
	}

	R_MeMotion( pseqdesc, pos );
}

// a sequence with its blends, into pos/q[dst], as R_StudioSetupBones; scratch 1-3
static void R_MeSequence( const mebones_ent_t *m, const mebones_seq_t *seq, int dst, const float *adj, const byte *gait )
{
	const studiohdr_t *hdr = m->hdr;
	const int nb = hdr->numbones;
	const mstudioanim_t *panim = seq->panim;
	vec3_t *pos = r_mebones_tmp.pos[dst];
	vec4_t *q = r_mebones_tmp.q[dst];

	if( seq->pseqdesc->numblends <= 1 )
		R_MeCalcRotations( hdr, pos, q, seq->pseqdesc, panim, seq->f, adj, gait, 0 );
	else R_MeCalcRotations2( hdr, pos, q, seq->pseqdesc, panim, seq->f, adj, seq->s[0], gait );

	if( seq->pseqdesc->numblends == 4 )
	{
		R_MeCalcRotations2( hdr, r_mebones_tmp.pos[2], r_mebones_tmp.q[2], seq->pseqdesc, panim + nb * 2, seq->f, adj, seq->s[0], gait );
		R_MeSlerpBones( nb, q, pos, r_mebones_tmp.q[2], r_mebones_tmp.pos[2], seq->s[1], gait );
	}
}

static uint *R_MeModelCommands( const mebones_job_t *job, const mebones_item_t *it, const mebones_ent_t *m, const melight_t *l );

// R_StudioComputeBBox: the box of the model and the sequence, transformed
static qboolean R_MeBonesCulled( const mebones_job_t *job, const mebones_ent_t *m, const model_t *mod )
{
	const mstudioseqdesc_t *pseqdesc = m->seq[0].pseqdesc;
	const qboolean modbox = !VectorIsNull( mod->mins ) && !VectorIsNull( mod->maxs );
	float c[3], e[3], tc[3], te[3];

	for( int i = 0; i < 3; i++ )
	{
		float lo = pseqdesc->bbmin[i], hi = pseqdesc->bbmax[i];

		if( lo > hi )
		{
			const float s = lo;
			lo = hi;
			hi = s;
		}

		if( modbox )
		{
			lo = Q_min( lo, mod->mins[i] );
			hi = Q_max( hi, mod->maxs[i] );
		}
		c[i] = ( lo + hi ) * 0.5f;
		e[i] = ( hi - lo ) * 0.5f;
	}

	for( int i = 0; i < 3; i++ )
	{
		const float *r = m->rotation[i];

		tc[i] = r[0] * c[0] + r[1] * c[1] + r[2] * c[2] + r[3];
		te[i] = fabsf( r[0] ) * e[0] + fabsf( r[1] ) * e[1] + fabsf( r[2] ) * e[2];
	}

	for( int i = 0; i < job->numplanes; i++ )
	{
		const float *p = job->planes[i];

		if( p[0] * tc[0] + p[1] * tc[1] + p[2] * tc[2] + p[3]
			+ fabsf( p[0] ) * te[0] + fabsf( p[1] ) * te[1] + fabsf( p[2] ) * te[2] < 0.0f )
			return true;
	}

	return false;
}

// which bones of a model the gait sequence moves, as R_StudioSetupBones
static const byte *R_MeGaitBones( const studiohdr_t *hdr )
{
	megait_t *const cache = r_mebones_me.gait;
	const mstudiobone_t *pbones = (const mstudiobone_t *)((const byte *)hdr + hdr->boneindex);
	qboolean copy_bones = true;
	int n;

	for( n = 0; n < 4; n++ )
	{
		if( cache[n].hdr == hdr && cache[n].length == hdr->length && cache[n].numbones == hdr->numbones && !memcmp( cache[n].name, hdr->name, sizeof( cache[n].name )))
			return cache[n].bones;
	}

	n = r_mebones_me.nextgait;
	r_mebones_me.nextgait = ( n + 1 ) & 3;
	cache[n].hdr = hdr;
	cache[n].length = hdr->length;
	cache[n].numbones = hdr->numbones;
	memcpy( cache[n].name, hdr->name, sizeof( cache[n].name ));

	for( int i = 0; i < hdr->numbones; i++ )
	{
		if( !Q_strcmp( pbones[i].name, "Bip01 Spine" ))
			copy_bones = false;
		else if( pbones[i].parent >= 0 && !Q_strcmp( pbones[pbones[i].parent].name, "Bip01 Pelvis" ))
			copy_bones = true;
		cache[n].bones[i] = copy_bones;
	}

	return cache[n].bones;
}

// parentbones: the bones a weapon's are merged with
static void R_MeBones( mebones_ent_t *m, matrix3x4 *parentbones )
{
	const studiohdr_t *hdr = m->hdr;
	const mstudiobone_t *pbones = (const mstudiobone_t *)((const byte *)hdr + hdr->boneindex);
	const int nb = hdr->numbones;
	const byte *gait = m->gait ? R_MeGaitBones( hdr ) : NULL;

	if( m->merge )
		R_MeCalcRotations( hdr, r_mebones_tmp.pos[0], r_mebones_tmp.q[0], m->seq[0].pseqdesc, m->seq[0].panim, m->seq[0].f, m->adj, (const byte *)m->merge->map, 0xff );
	else R_MeSequence( m, &m->seq[0], 0, m->adj, gait );

	if( m->blendprev )
	{
		R_MeSequence( m, &m->seq[1], 4, m->adj, gait );
		R_MeSlerpBones( nb, r_mebones_tmp.q[0], r_mebones_tmp.pos[0], r_mebones_tmp.q[4], r_mebones_tmp.pos[4], m->sprev, gait );
	}

	if( gait )
		R_MeCalcRotations( hdr, r_mebones_tmp.pos[0], r_mebones_tmp.q[0], m->seq[2].pseqdesc, m->seq[2].panim, m->seq[2].f, m->adj, gait, 1 );

	for( int i = 0; i < nb; i++ )
	{
		matrix3x4 bonematrix;
		const int parent = pbones[i].parent;

		if( m->merge && m->merge->map[i] >= 0 )
		{
			Matrix3x4_Copy( m->out[i], parentbones[m->merge->map[i]] );
			continue;
		}

		R_StudioBoneMatrix( bonematrix, r_mebones_tmp.q[0][i], r_mebones_tmp.pos[0][i] );
		Matrix3x4_ConcatTransforms( m->out[i], parent == -1 ? m->rotation : m->out[parent], bonematrix );
	}

	PSP_MeWriteback( m->out, nb * sizeof( matrix3x4 ));
	m->done = true;

	if( m->weapon )
		R_MeBones( m->weapon, m->out );
}

// R_StudioGetAnim of what is loaded already
static const mstudioanim_t *R_MeAnim( const studiohdr_t *hdr, const model_t *mod, const mstudioseqdesc_t *pseqdesc )
{
	const cache_user_t *seqs;

	if( pseqdesc->seqgroup == 0 )
		return (const mstudioanim_t *)((const byte *)hdr + pseqdesc->animindex );

	seqs = (const cache_user_t *)mod->submodels;
	if( !seqs || !seqs[pseqdesc->seqgroup].data )
		return NULL;

	return (const mstudioanim_t *)((const byte *)seqs[pseqdesc->seqgroup].data + pseqdesc->animindex );
}

// Matrix3x4_CreateFromEntity from the sines and cosines
static void R_MeAnglesMatrix( matrix3x4 out, const vec3_t origin, float sy, float cy, float sp, float cp, float sr, float cr )
{
	out[0][0] = cp * cy;
	out[0][1] = sr * sp * cy - cr * sy;
	out[0][2] = cr * sp * cy + sr * sy;
	out[0][3] = origin[0];
	out[1][0] = cp * sy;
	out[1][1] = sr * sp * sy + cr * cy;
	out[1][2] = cr * sp * sy - sr * cy;
	out[1][3] = origin[1];
	out[2][0] = -sp;
	out[2][1] = sr * cp;
	out[2][2] = cr * cp;
	out[2][3] = origin[2];
}

// AngleQuaternion( angles, q, false )
static void R_MeEntityQuaternion( const vec3_t angles, vec4_t q )
{
	const vec3_t a = { DEG2RAD( angles[ROLL] ), DEG2RAD( angles[PITCH] ), DEG2RAD( angles[YAW] ) };

	R_MeAngleQuaternion( a, q );
}

// R_StudioSetUpTransform, of a player the legs' yaw
static void R_MeTransform( const mebones_frame_t *frame, const cl_entity_t *e, const meplayer_t *player, matrix3x4 out )
{
	const float sign = frame->negpitch ? -1.0f : 1.0f;
	vec3_t origin, angles;
	float sy, cy, sp, cp, sr = 0.0f, cr = 1.0f;

	if( e->curstate.movetype == MOVETYPE_STEP && frame->steplerp && !player )
	{
		// R_StudioLerpMovement
		float f = 1.0f;

		if( frame->interpolate && e->curstate.animtime != e->latched.prevanimtime )
		{
			const float dt = frame->time - e->curstate.animtime;

			if( dt < 1.0f )
				f = dt / ( e->curstate.animtime - e->latched.prevanimtime );
		}

		VectorLerp( e->latched.prevorigin, f, e->curstate.origin, origin );

		if( !VectorCompareEpsilon( e->curstate.angles, e->latched.prevangles, ON_EPSILON ))
		{
			matrix3x4 m;
			vec4_t q, q1, q2;

			R_MeEntityQuaternion( e->curstate.angles, q1 );
			R_MeEntityQuaternion( e->latched.prevangles, q2 );
			R_MeQuaternionSlerp( q2, q1, f, q );
			R_StudioBoneMatrix( m, q, vec3_origin );

			// Matrix3x4_AnglesFromMatrix: the sines and cosines of its angles
			cp = __builtin_sqrtf( m[0][0] * m[0][0] + m[1][0] * m[1][0] );
			sp = -m[2][0] * sign;

			if( cp > 0.001f )
			{
				const float k = 1.0f / cp;

				cy = m[0][0] * k;
				sy = m[1][0] * k;
				sr = m[2][1] * k;
				cr = m[2][2] * k;
			}
			else
			{
				const float k = 1.0f / __builtin_sqrtf( m[0][1] * m[0][1] + m[1][1] * m[1][1] );

				sy = -m[0][1] * k;
				cy = m[1][1] * k;
			}

			R_MeAnglesMatrix( out, origin, sy, cy, sp, cp, sr, cr );
			return;
		}

		VectorCopy( e->curstate.angles, angles );
	}
	else
	{
		VectorCopy( e->origin, origin );
		VectorCopy( e->angles, angles );
	}

	if( player )
	{
		angles[PITCH] = 0.0f;
		if( player->gaitsequence )
			angles[YAW] = player->yaw;
	}

	R_MeSinCos( DEG2RAD( angles[YAW] ), &sy, &cy );
	R_MeSinCos( DEG2RAD( angles[PITCH] ) * sign, &sp, &cp );
	if( angles[ROLL] )
		R_MeSinCos( DEG2RAD( angles[ROLL] ), &sr, &cr );
	R_MeAnglesMatrix( out, origin, sy, cy, sp, cp, sr, cr );
}

// R_StudioEstimateFrame
static float R_MeEstimateFrame( const mebones_frame_t *frame, const cl_entity_t *e, const mstudioseqdesc_t *pseqdesc )
{
	float dfdt = 0.0f, f;

	if( frame->interpolate )
	{
		const float dt = frame->time - e->curstate.animtime;

		if( dt >= 0.0f )
			dfdt = dt * e->curstate.framerate * pseqdesc->fps;
	}

	if( pseqdesc->numframes <= 1 ) f = 0.0f;
	else f = (e->curstate.frame * (pseqdesc->numframes - 1)) * ( 1.0f / 256.0f );

	f += dfdt;

	if( pseqdesc->flags & STUDIO_LOOPING )
	{
		if( pseqdesc->numframes > 1 )
			f -= (int)(f / (pseqdesc->numframes - 1)) *  (pseqdesc->numframes - 1);
		if( f < 0 ) f += (pseqdesc->numframes - 1);
	}
	else
	{
		if( f >= pseqdesc->numframes - 1.001f )
			f = pseqdesc->numframes - 1.001f;
		if( f < 0.0f )  f = 0.0f;
	}
	return f;
}

// what R_StudioSetupBones depends on, besides the animation data and the rotation
static void R_StudioMeBonesKey( mebones_key_t *k, const cl_entity_t *e, const studiohdr_t *hdr, double time, qboolean interpolate )
{
	memset( k, 0, sizeof( *k ));
	k->hdr = hdr;
	k->time = time;
	k->interpolate = interpolate;
	k->sequence = e->curstate.sequence;
	k->prevsequence = e->latched.prevsequence;
	k->renderfx = e->curstate.renderfx;
	k->frame = e->curstate.frame;
	k->animtime = e->curstate.animtime;
	k->framerate = e->curstate.framerate;
	k->prevanimtime = e->latched.prevanimtime;
	k->sequencetime = e->latched.sequencetime;
	// only the blend from the previous sequence reads it (the first use of the bones sets it)
	if( interpolate && e->latched.sequencetime && ( e->latched.sequencetime + 0.2f > time ) && ( e->latched.prevsequence < hdr->numseq ))
		k->prevframe = e->latched.prevframe;
	memcpy( k->controller, e->curstate.controller, 4 );
	memcpy( k->prevcontroller, e->latched.prevcontroller, 4 );
	memcpy( k->blending, e->curstate.blending, 2 );
	memcpy( k->prevblending, e->latched.prevblending, 2 );
	memcpy( k->prevseqblending, e->latched.prevseqblending, 2 );
	k->mouthopen = e->mouth.mouthopen;
}

// a new entry, not counted yet
static mebones_ent_t *R_MeEntNew( const studiohdr_t *hdr, int ofs, int usedofs )
{
	const int slot = ( r_mebones_me.scratch ? MEBONES_MAXENTS * 2 : r_mebones_me.count ) + ofs;
	const int used = ( r_mebones_me.scratch ? MEBONES_POOL : r_mebones_me.used ) + usedofs;
	mebones_ent_t *m = &r_mebones_ents[slot];

	if( !r_mebones_me.scratch && ( slot >= MEBONES_MAXENTS * 2 || used + (( hdr->numbones + 3 ) & ~3 ) > MEBONES_POOL ))
		return NULL;

	m->hdr = hdr;
	m->out = &r_mebones_out[used];
	m->blendprev = false;
	m->gait = false;
	m->merge = NULL;
	m->weapon = NULL;
	m->done = false;
	m->lit = false;
	return m;
}

// R_StudioSetupBones up to the math
static mebones_ent_t *R_MeGather( const mebones_frame_t *frame, const cl_entity_t *e, const studiohdr_t *hdr, const meplayer_t *player )
{
	const model_t *mod = player ? player->mod : e->model;
	mebones_ent_t *m = R_MeEntNew( hdr, 0, 0 );
	const mstudioseqdesc_t *pseqdescs = (const mstudioseqdesc_t *)((const byte *)hdr + hdr->seqindex );
	// whole cache lines: 4 matrices are 3
	int used = ( hdr->numbones + 3 ) & ~3, count = 1;

	if( !m )
		return NULL;

	R_MeTransform( frame, e, player, m->rotation );
	R_StudioMeBonesKey( &m->key, e, hdr, frame->time, frame->interpolate );

	const mstudioseqdesc_t *pseqdesc = pseqdescs + e->curstate.sequence;
	const float f = R_MeEstimateFrame( frame, e, pseqdesc );
	float dadt = 1.0f;

	// R_StudioEstimateInterpolant
	if( frame->interpolate && ( e->curstate.animtime >= e->latched.prevanimtime + 0.01f ))
	{
		dadt = (float)( frame->time - e->curstate.animtime ) * ( 1.0f / 0.1f );
		if( dadt > 2.0f ) dadt = 2.0f;
	}

	R_StudioBoneAdj( hdr, dadt, m->adj, e->curstate.controller, e->latched.prevcontroller, e->mouth.mouthopen );

	m->seq[0].pseqdesc = pseqdesc;
	m->seq[0].panim = R_MeAnim( hdr, mod, pseqdesc );
	m->seq[0].f = f;
	m->seq[0].s[0] = (e->curstate.blending[0] * dadt + e->latched.prevblending[0] * (1.0f - dadt)) / 255.0f;
	m->seq[0].s[1] = (e->curstate.blending[1] * dadt + e->latched.prevblending[1] * (1.0f - dadt)) / 255.0f;
	if( !m->seq[0].panim )
		return NULL;

	m->blendprev = frame->interpolate && e->latched.sequencetime && ( e->latched.sequencetime + 0.2f > frame->time ) && ( e->latched.prevsequence < hdr->numseq );
	if( m->blendprev )
	{
		pseqdesc = pseqdescs + e->latched.prevsequence;
		m->seq[1].pseqdesc = pseqdesc;
		m->seq[1].panim = R_MeAnim( hdr, mod, pseqdesc );
		m->seq[1].f = e->latched.prevframe;
		m->seq[1].s[0] = (e->latched.prevseqblending[0]) / 255.0f;
		m->seq[1].s[1] = (e->latched.prevseqblending[1]) / 255.0f;
		m->sprev = 1.0f - ( frame->time - e->latched.sequencetime ) / 0.2f;
		if( !m->seq[1].panim )
			return NULL;
	}
	else m->prevframe = f;

	if( player && player->gaitsequence )
	{
		pseqdesc = pseqdescs + player->gaitsequence;
		m->gait = true;
		m->seq[2].pseqdesc = pseqdesc;
		m->seq[2].panim = R_MeAnim( hdr, mod, pseqdesc );
		m->seq[2].f = player->gaitframe;
		if( !m->seq[2].panim )
			return NULL;
	}

	// R_StudioMergeBones
	if( player && player->whdr )
	{
		const studiohdr_t *whdr = player->whdr;
		mebones_ent_t *w = R_MeEntNew( whdr, 1, used );

		if( !w )
			return NULL;

		pseqdesc = (const mstudioseqdesc_t *)((const byte *)whdr + whdr->seqindex );
		if( e->curstate.sequence < whdr->numseq )
			pseqdesc += e->curstate.sequence;

		R_StudioBoneAdj( whdr, dadt, w->adj, e->curstate.controller, e->latched.prevcontroller, e->mouth.mouthopen );
		Matrix3x4_Copy( w->rotation, m->rotation );
		w->merge = player->merge;
		w->seq[0].pseqdesc = pseqdesc;
		w->seq[0].panim = R_MeAnim( whdr, player->wmod, pseqdesc );
		w->seq[0].f = R_MeEstimateFrame( frame, e, pseqdesc );
		if( !w->seq[0].panim )
			return NULL;

		m->weapon = w;
		used += ( whdr->numbones + 3 ) & ~3;
		count++;
	}

	if( !r_mebones_me.scratch )
	{
		r_mebones_me.used += used;
		r_mebones_me.count += count;
	}
	PSP_MeWriteback( m, sizeof( *m ));
	return m;
}

/*
====================
the light of a model on the ME: R_EntityDynamicLight (ref_light.c), from the
main CPU's lights and physents in the frame and caches of its own, then
R_StudioEntityLight and R_StudioGEFoldElights
====================
*/
typedef struct
{
	const model_t	*model;
	float		fraction;
	vec3_t		vec;
	uint		rgb[3];
} melighttrace_t;

static float R_MeNormalize( vec3_t v )
{
	float len = DotProduct( v, v );

	if( len > 0.0f )
	{
		len = __builtin_sqrtf( len );
		const float k = 1.0f / len;
		VectorScale( v, k, v );
	}
	return len;
}

// does the line hit the world's sky first, as PM_RecursiveSurfCheck: 0 nothing hit, 1 something else, 2 sky
static int R_MeSkyTrace( const model_t *mod, const mnode_t *node, const vec3_t p1, const vec3_t p2 )
{
	const float eps = 0.03125f; // FRAC_EPSILON
start:
	if( node->contents < 0 )
		return 0;

	const float t1 = PlaneDiff( p1, node->plane );
	const float t2 = PlaneDiff( p2, node->plane );

	if( t1 >= -eps && t2 >= -eps )
	{
		node = node_child( node, 0, mod );
		goto start;
	}

	if( t1 < eps && t2 < eps )
	{
		node = node_child( node, 1, mod );
		goto start;
	}

	const int side = t1 < 0.0f;
	float frac = t1 / ( t1 - t2 );
	frac = bound( 0.0f, frac, 1.0f );

	vec3_t mid;
	VectorLerp( p1, frac, p2, mid );

	const int hit = R_MeSkyTrace( mod, node_child( node, side, mod ), p1, mid );
	if( hit )
		return hit;

	const int numsurfaces = node_numsurfaces( node, mod );
	const msurface_t *surf = &mod->surfaces[node_firstsurface( node, mod )];

	for( int i = 0; i < numsurfaces; i++, surf++ )
	{
		const mfacebevel_t *fb = surf->info->bevel;
		vec3_t delta;
		int j;

		if( !fb )
			continue;

		VectorSubtract( mid, fb->origin, delta );
		if( DotProduct( delta, delta ) >= fb->radius )
			continue;

		for( j = 0; j < fb->numedges; j++ )
		{
			if( PlaneDiff( mid, &fb->edges[j] ) > eps )
				break;
		}

		if( j == fb->numedges )
			return FBitSet( surf->flags, SURF_DRAWSKY ) ? 2 : 1;
	}

	return R_MeSkyTrace( mod, node_child( node, side ^ 1, mod ), mid, p2 );
}

static int R_MeCell( float f )
{
	const int i = (int)f;
	return i > f ? i - 1 : i;
}

static qboolean R_MeSeesSky( const mebones_frame_t *frame, const cl_entity_t *e, melightcache_t *c, const vec3_t start )
{
	const vec_t *const origin = e->origin;

	if( c->skyent == e && VectorDistance2( c->skyorigin, origin ) < 32.0f * 32.0f )
		return c->sky;

	const int x = R_MeCell( origin[0] * ( 1.0f / 16.0f ));
	const int y = R_MeCell( origin[1] * ( 1.0f / 16.0f ));
	const int z = R_MeCell( origin[2] * ( 1.0f / 16.0f ));
	meskycell_t *cell = &r_mebones_me.skycells[((uint)x * 73856093u ^ (uint)y * 19349663u ^ (uint)z * 83492791u ) & ( MELIGHT_CELLS - 1 )];

	if( !cell->sky || cell->x != x || cell->y != y || cell->z != z )
	{
		vec3_t end;

		VectorSubtract( origin, frame->skyend, end );
		cell->x = x;
		cell->y = y;
		cell->z = z;
		VectorCopy( origin, cell->origin );
		cell->sky = R_MeSkyTrace( frame->world, &frame->world->nodes[frame->world->hulls[0].firstclipnode], start, end ) == 2 ? 2 : 1;
	}

	c->skyent = e;
	VectorCopy( cell->origin, c->skyorigin );
	c->sky = cell->sky == 2;
	return c->sky;
}

// R_RecursiveLightPoint
static qboolean R_MeLightPoint( melighttrace_t *tr, const mnode_t *node, float p1f, float p2f, const vec3_t start, const vec3_t end )
{
	const model_t *const model = tr->model;
restart:
	if( !node || node->contents < 0 )
	{
		VectorClear( tr->rgb );
		return false;
	}

	const float front = PlaneDiff( start, node->plane );
	const float back = PlaneDiff( end, node->plane );
	const int side = front < 0;

	if(( back < 0 ) == side )
	{
		node = node_child( node, side, model );
		goto restart;
	}

	const float frac = front / ( front - back );
	vec3_t mid;
	VectorLerp( start, frac, end, mid );

	const float midf = p1f + ( p2f - p1f ) * frac;

	if( R_MeLightPoint( tr, node_child( node, side, model ), p1f, midf, start, mid ))
		return true;

	const int numsurfaces = node_numsurfaces( node, model );
	const msurface_t *surf = &model->surfaces[node_firstsurface( node, model )];

	for( int i = 0; i < numsurfaces; i++, surf++ )
	{
		const mextrasurf_t *info = surf->info;

		if( FBitSet( surf->flags, SURF_DRAWTILED ))
			continue;

		const float ds = DotProduct( mid, info->lmvecs[0] ) + info->lmvecs[0][3] - info->lightmapmins[0];
		const float dt = DotProduct( mid, info->lmvecs[1] ) + info->lmvecs[1][3] - info->lightmapmins[1];

		if( ds < 0.0f || dt < 0.0f || ds > info->lightextents[0] || dt > info->lightextents[1] )
			continue;

		VectorClear( tr->rgb );

		if( !surf->samples )
			return true;

		const int sample_size = gEngfuncs.Mod_SampleSizeForFace( surf );
		const int smax = info->lightextents[0] / sample_size + 1;
		const int tmax = info->lightextents[1] / sample_size + 1;
		const int size = smax * tmax;
		const int ofs = (int)( dt / sample_size + 0.5f ) * smax + (int)( ds / sample_size + 0.5f );
		const color24 *lm = surf->samples + ofs;
		const color24 *dm = info->deluxemap ? info->deluxemap + ofs : NULL;

		tr->fraction = midf;

		for( int map = 0; map < MAXLIGHTMAPS && surf->styles[map] != 255; map++ )
		{
			const uint scale = g_lightstylevalue[surf->styles[map]];

			tr->rgb[0] += lm->r * scale;
			tr->rgb[1] += lm->g * scale;
			tr->rgb[2] += lm->b * scale;
			lm += size;

			if( dm )
			{
				const float f = 1.0f / 128.0f;
				const float n[3] = { ((float)dm->r - 128.0f) * f, ((float)dm->g - 128.0f) * f, ((float)dm->b - 128.0f) * f };
				vec3_t s = { info->lmvecs[0][0], info->lmvecs[0][1], info->lmvecs[0][2] };
				vec3_t u = { -info->lmvecs[1][0], -info->lmvecs[1][1], -info->lmvecs[1][2] };
				const float sign = FBitSet( surf->flags, SURF_PLANEBACK ) ? -1.0f : 1.0f;

				R_MeNormalize( s );
				R_MeNormalize( u );
				for( int k = 0; k < 3; k++ )
					tr->vec[k] -= ( s[k] * n[0] + u[k] * n[1] + surf->plane->normal[k] * sign * n[2] ) * scale;
				dm += size;
			}
		}

		return true;
	}

	return R_MeLightPoint( tr, node_child( node, !side, model ), midf, p2f, mid, end );
}

// R_LightVecInternal
static void R_MeLightVec( const mebones_frame_t *frame, const vec3_t start, const vec3_t end, uint rgb[3], vec3_t vec )
{
	float last = 1.0f;

	VectorClear( rgb );
	VectorClear( vec );

	if( !frame->world->lightdata )
	{
		VectorSet( rgb, 255, 255, 255 );
		return;
	}

	for( int i = 0; i < frame->numphys; i++ )
	{
		const mephys_t *pe = &frame->phys[i];
		melighttrace_t tr;
		vec3_t p1, p2;

		VectorSubtract( start, pe->offset, p1 );
		VectorSubtract( end, pe->offset, p2 );

		if( pe->rotated )
		{
			const vec3_t s = { p1[0], p1[1], p1[2] }, e = { p2[0], p2[1], p2[2] };

			for( int k = 0; k < 3; k++ )
			{
				p1[k] = s[0] * pe->matrix[0][k] + s[1] * pe->matrix[1][k] + s[2] * pe->matrix[2][k];
				p2[k] = e[0] * pe->matrix[0][k] + e[1] * pe->matrix[1][k] + e[2] * pe->matrix[2][k];
			}
		}

		tr.model = pe->model;
		tr.fraction = 1.0f;
		VectorClear( tr.vec );

		if( !R_MeLightPoint( &tr, &pe->model->nodes[pe->model->hulls[0].firstclipnode], 0.0f, 1.0f, p1, p2 ))
			continue;

		if( tr.fraction < last )
		{
			VectorCopy( tr.vec, vec );
			R_MeNormalize( vec );
			for( int k = 0; k < 3; k++ )
				rgb[k] = Q_min( tr.rgb[k] >> 8, 255 );
			last = tr.fraction;

			if( rgb[0] + rgb[1] + rgb[2] != 0 )
				break;
		}
	}
}

static void R_MeFloorLight( const mebones_frame_t *frame, melightcache_t *c, const vec3_t origin, float dirz )
{
	const vec3_t start = { origin[0], origin[1], origin[2] - dirz * 8.0f };
	const vec3_t end = { start[0], start[1], start[2] + dirz * 2048.0f };
	vec3_t vec;

	R_MeLightVec( frame, start, end, c->rgb, vec );

	// from the ceiling
	if( r_lighting_extended.value && c->rgb[0] + c->rgb[1] + c->rgb[2] == 0 )
		R_MeLightVec( frame, end, start, c->rgb, vec );

	if( VectorIsNull( vec ))
	{
		static const signed char ofs[4][2] = {{ -16, -16 }, { 16, -16 }, { 16, 16 }, { -16, 16 }};
		float grad[4];

		for( int i = 0; i < 4; i++ )
		{
			const vec3_t s = { start[0] + ofs[i][0], start[1] + ofs[i][1], start[2] };
			const vec3_t e = { end[0] + ofs[i][0], end[1] + ofs[i][1], end[2] };
			uint rgb[3];
			vec3_t v;

			R_MeLightVec( frame, s, e, rgb, v );
			grad[i] = ( rgb[0] + rgb[1] + rgb[2] ) * ( 1.0f / 768.0f );
		}

		c->dir[0] = grad[0] - grad[1] - grad[2] + grad[3];
		c->dir[1] = grad[1] + grad[0] - grad[2] - grad[3];
		c->dir[2] = dirz;
		R_MeNormalize( c->dir );
	}
	else VectorCopy( vec, c->dir );
}

// the caches are for the map and the sky they were filled in
static void R_MeLightCheck( const mebones_frame_t *frame )
{
	const void *key[6] = { frame->world, frame->world->nodes, frame->world->surfaces, frame->world->lightdata, (const void *)frame->world->numnodes, NULL };

	memcpy( &key[5], &frame->skyvec[0], sizeof( float ));
	if( !memcmp( key, r_mebones_me.lightkey, sizeof( key )))
		return;

	memcpy( r_mebones_me.lightkey, key, sizeof( key ));
	memset( r_mebones_me.light, 0, sizeof( r_mebones_me.light ));
	memset( r_mebones_me.skycells, 0, sizeof( r_mebones_me.skycells ));
}

// R_EntityDynamicLight
static void R_MeDynamicLight( const mebones_frame_t *frame, const cl_entity_t *e, qboolean small, melight_t *l )
{
	const int flags = e->model->type == mod_studio ? e->model->flags : 0;

	l->floor = BIT( 31 );

	if( FBitSet( e->curstate.effects, EF_FULLBRIGHT ))
	{
		l->shade = 0.0f;
		l->ambient = 192.0f;
		VectorSet( l->vec, 0.0f, 0.0f, -1.0f );
		VectorSet( l->color, 1.0f, 1.0f, 1.0f );
		return;
	}

	melightcache_t *c = &r_mebones_me.light[((uint)(uintptr_t)e * 2654435761u ) >> 24];
	const int effects = e->curstate.effects & EF_INVLIGHT;
	const float dirz = effects ? 1.0f : -1.0f;
	vec3_t dir = { 0.0f, 0.0f, dirz };
	uint rgb[3] = { 0, 0, 0 };

	if( frame->sky )
	{
		const vec3_t start = { e->origin[0], e->origin[1], e->origin[2] - dirz * 8.0f };

		if( FBitSet( flags, STUDIO_FORCE_SKYLIGHT ) || R_MeSeesSky( frame, e, c, start ))
		{
			VectorCopy( frame->skyvec, dir );
			VectorCopy( frame->skycolor, rgb );
		}
	}

	if( rgb[0] + rgb[1] + rgb[2] == 0 )
	{
		const float time = frame->time;
		const float dist = small ? 16.0f : 2.0f; // shells and gibs fly

		if( c->ent != e || c->effects != effects || time < c->time || time >= c->time + 0.25f || VectorDistance2( c->origin, e->origin ) > dist * dist )
		{
			R_MeFloorLight( frame, c, e->origin, dirz );
			c->ent = e;
			c->effects = effects;
			c->time = time;
			VectorCopy( e->origin, c->origin );
		}
		VectorCopy( c->dir, dir );
		VectorCopy( c->rgb, rgb );
	}

	if( e->curstate.renderfx == kRenderFxLightMultiplier && e->curstate.iuser4 != 10 )
	{
		for( int i = 0; i < 3; i++ )
			rgb[i] *= e->curstate.iuser4 * ( 1.0f / 10.0f );
	}

	l->floor = Q_min( rgb[0], 255 ) | Q_min( rgb[1], 255 ) << 8 | Q_min( rgb[2], 255 ) << 16;

	float final[3] = { rgb[0], rgb[1], rgb[2] };
	float total = Q_max( Q_max( final[0], final[1] ), final[2] );
	if( total == 0.0f )
		total = 1.0f;

	VectorScale( dir, total, dir );

	for( int i = 0; i < frame->numdlights; i++ )
	{
		const medlight_t *dl = &frame->dlights[i];
		vec3_t d;

		VectorSubtract( e->origin, dl->origin, d );

		const float d2 = DotProduct( d, d );
		if( d2 >= dl->radius * dl->radius )
			continue;

		const float dist = __builtin_sqrtf( d2 );
		const float add = dl->radius - dist;

		total += add;
		VectorMA( dir, dist > 1.0f ? add / dist : add, d, dir );
		VectorMA( final, add * ( 1.0f / 256.0f ), dl->color, final );
	}

	VectorScale( dir, FBitSet( flags, STUDIO_AMBIENT_LIGHT ) ? 0.6f : frame->direct, dir );

	l->shade = R_MeNormalize( dir );
	l->ambient = total - l->shade;
	VectorCopy( dir, l->vec );

	total = Q_max( Q_max( final[0], final[1] ), final[2] );
	if( total > 0.0f )
		VectorScale( final, 1.0f / total, l->color );
	else VectorSet( l->color, 1.0f, 1.0f, 1.0f );

	if( l->ambient > 128.0f )
		l->ambient = 128.0f;
	if( l->ambient + l->shade > 255.0f )
		l->shade = 255.0f - l->ambient;
}

// R_StudioEntityLight and R_StudioGEFoldElights
static void R_MeEntityLight( const mebones_job_t *job, const mebones_ent_t *m, const cl_entity_t *e, const float attachment[][3], melight_t *l )
{
	const vec3_t origin = { m->rotation[0][3], m->rotation[1][3], m->rotation[2][3] };
	int lights[MAX_LOCALLIGHTS], num = 0;
	float strength[MAX_LOCALLIGHTS], dist2 = 1000000.0f;

	for( int i = 0; i < job->numelights; i++ )
	{
		const meelight_t *el = &job->elights[i];
		vec3_t at = { el->origin[0], el->origin[1], el->origin[2] }, mid;
		int k = num;

		if(( el->key & 0xFFF ) == e->index )
		{
			const int att = ( el->key >> 12 ) & 0xF;

			if( !att ) VectorCopy( e->origin, at );
			else if( att < m->hdr->numattachments ) VectorCopy( attachment[att], at );
			else if( att < MEBONES_ATTACHMENTS ) VectorCopy( e->attachment[att], at );
		}

		VectorSubtract( origin, at, mid );

		const float f = DotProduct( mid, mid );
		const float s = f > el->r2 ? el->r2 / f : 1.0f;

		if( s <= 0.05f )
			continue;

		if( num >= MAX_LOCALLIGHTS )
		{
			k = -1;
			for( int j = 0; j < num; j++ )
			{
				if( strength[j] < dist2 && strength[j] < s )
				{
					dist2 = strength[j];
					k = j;
				}
			}
			if( k < 0 )
				continue;
		}
		else num++;

		lights[k] = i;
		strength[k] = s;
	}

	if( !num )
		return;

	vec3_t vec, color;
	const float base = l->ambient + l->shade;
	float ambient = l->ambient, total = base;

	VectorScale( l->vec, l->shade, vec );
	VectorScale( l->color, base, color );

	for( int i = 0; i < num; i++ )
	{
		const meelight_t *el = &job->elights[lights[i]];
		const float max = Q_max( Q_max( el->color[0], el->color[1] ), el->color[2] );
		vec3_t dir;

		if( max <= 0.0f )
			continue;

		VectorSubtract( origin, el->origin, dir );

		const float d2 = DotProduct( dir, dir );
		const float s = d2 > el->r2 ? __builtin_sqrtf( el->r2 / d2 ) : 1.0f;
		const float strong = max * ( 1.0f / 4.0f ) * s;

		if( d2 > 0.0f )
			VectorMA( vec, ( strong * 0.5f ) / __builtin_sqrtf( d2 ), dir, vec );
		ambient += strong * 0.5f;
		total += strong;

		for( int j = 0; j < 3; j++ )
			color[j] += el->color[j] / max * strong;
	}

	l->shade = R_MeNormalize( vec );
	if( l->shade > 0.0f )
		VectorCopy( vec, l->vec );
	l->ambient = ambient;

	const float cm = Q_max( Q_max( color[0], color[1] ), color[2] );
	if( cm > 0.0f && total > 0.0f )
		VectorScale( color, 1.0f / cm, l->color );
}

// may R_StudioClientEvents have something to do
static qboolean R_MeEvents( const mebones_frame_t *frame, const cl_entity_t *e, const mebones_ent_t *m )
{
	const mstudioseqdesc_t *pseqdesc = m->seq[0].pseqdesc;
	const mstudioevent_t *pevent = (const mstudioevent_t *)((const byte *)m->hdr + pseqdesc->eventindex );
	const float end = m->seq[0].f;
	float start = end - e->curstate.framerate * frame->frametime * pseqdesc->fps;

	if( FBitSet( e->curstate.effects, EF_MUZZLEFLASH ))
		return true;

	if( e->latched.sequencetime == e->curstate.animtime && !FBitSet( pseqdesc->flags, STUDIO_LOOPING ))
		start = Q_min( start, -0.01f );

	// it looks again exactly
	for( int i = 0; i < pseqdesc->numevents; i++ )
	{
		if( pevent[i].event >= EVENT_CLIENT && pevent[i].frame >= start - 0.1f && pevent[i].frame <= end + 0.1f )
			return true;
	}

	return false;
}

static inline uint16_t *R_MeBonesSeen( const cl_entity_t *e )
{
	return &r_mebones_me.seen[((uint)(uintptr_t)e * 2654435761u ) >> 24];
}

// R_StudioPreCull's sphere
static void R_MeSphere( const mebones_frame_t *frame, const cl_entity_t *e, const model_t *mod, const studiohdr_t *hdr, mecull_t *c )
{
	int seq = e->curstate.sequence;

	if( seq < 0 || seq >= hdr->numseq )
		seq = 0;
	const mstudioseqdesc_t *pseqdesc = (const mstudioseqdesc_t *)((const byte *)hdr + hdr->seqindex ) + seq;

	// the farthest corner of the union box from the origin
	const qboolean modbox = !VectorIsNull( mod->mins ) && !VectorIsNull( mod->maxs );
	float r2 = 0.0f;

	for( int i = 0; i < 3; i++ )
	{
		float lo = pseqdesc->bbmin[i], hi = pseqdesc->bbmax[i];

		if( modbox )
		{
			lo = Q_min( lo, mod->mins[i] );
			hi = Q_max( hi, mod->maxs[i] );
		}
		const float m = Q_max( fabsf( lo ), fabsf( hi ));
		r2 += m * m;
	}

	float r = __builtin_sqrtf( r2 );
	if( e->curstate.scale > 1.0f )
		r *= e->curstate.scale;

	c->rpx = r;

	if( e->curstate.movetype == MOVETYPE_STEP )
	{
		const float t = frame->time;
		float f = 0.0f;

		if( t < e->curstate.animtime + 1.0f && e->curstate.animtime != e->latched.prevanimtime )
			f = ( t - e->curstate.animtime ) / ( e->curstate.animtime - e->latched.prevanimtime );

		vec3_t d;
		VectorSubtract( e->origin, e->latched.prevorigin, d );
		r += __builtin_sqrtf( DotProduct( d, d )) * ( fabsf( f ) + 1.0f );
	}

	VectorCopy( e->origin, c->org );
	c->r = r + 16.0f;
	c->hdr = hdr;
	c->body = e->curstate.body;
	c->skin = e->curstate.skin;
}

static void R_MeClassify( const mebones_frame_t *frame, const cl_entity_t *e, mecull_t *c )
{
	const model_t *mod = e->model;

	c->type = MECULL_MAIN;

	if( !mod || e->player )
		return;

	if( mod->type == mod_brush )
	{
		if( R_MeBrushClassify( e, &c->brush ))
			c->type = MECULL_BRUSH;
		return;
	}

	if( mod->type != mod_studio || e->curstate.movetype == MOVETYPE_FOLLOW || e->curstate.renderfx == kRenderFxDeadPlayer )
		return;

	const studiohdr_t *hdr = mod->cache.data;
	if( !hdr || hdr->numseq <= 0 )
		return;

	R_MeSphere( frame, e, mod, hdr, c );
	c->type = MECULL_TEST;
	c->mode = MEMODE_SOLID;
	c->small = hdr->numbones <= 2;

	if( e->curstate.renderfx == kRenderFxDistort || e->curstate.renderfx == kRenderFxHologram || e->curstate.renderfx == kRenderFxExplode )
		return;
	if( hdr->numbones <= 0 || hdr->numbones > MAXSTUDIOBONES || e->curstate.sequence < 0 || e->curstate.sequence >= hdr->numseq )
		return;

	if( !c->small )
		c->type = MECULL_BONES;

	if( e->curstate.rendermode == kRenderTransAdd )
		c->mode = MEMODE_ADD;
	else c->mode = e->curstate.rendermode == kRenderNormal ? MEMODE_SOLID : MEMODE_BLEND;

	if( e->curstate.rendermode >= kRenderNormal && e->curstate.rendermode <= kRenderTransAdd && e->curstate.renderfx != kRenderFxGlowShell
		&& !e->curstate.colormap && e->curstate.body >= 0 && e->curstate.body <= 0xffff
		&& hdr->numattachments <= MEBONES_ATTACHMENTS && !FBitSet( hdr->flags, STUDIO_HAS_BONEWEIGHTS ))
		c->type = c->small ? MECULL_SMALL : MECULL_DRAW;
}

// the view independent part of R_SortTransEntities
static void R_MeSortKey( const cl_entity_t *e, mesort_t *s )
{
	const model_t *mod = e->model;
	int rendermode = e->curstate.rendermode;

	// R_GetEntityRenderMode
	if( !mod || e->player )
	{
		s->rank = MESORT_MAIN;
		return;
	}

	if( mod->type == mod_studio && mod->cache.data )
	{
		if( R_StudioMostlyAdditive( mod->cache.data ))
			rendermode = kRenderTransAdd;
	}
	else if( R_ModelOpaque( rendermode ) && mod->type == mod_brush && FBitSet( mod->flags, MODEL_TRANSPARENT ))
		rendermode = kRenderTransAlpha;

	s->rank = R_RankForRenderMode( rendermode );
	s->far = mod->type == mod_brush && rendermode == kRenderTransAlpha;
	for( int i = 0; i < 3; i++ )
		s->org[i] = ( mod->mins[i] + mod->maxs[i] ) * 0.5f + e->origin[i];
}

static void R_MeEarlyBones( const mebones_early_t *job, int first, int count, uint32_t number )
{
	const mebones_frame_t *frame = job->frame;
	const uint16_t last = frame->framecount;

	for( int i = first; i < first + count; i++ )
	{
		const mecull_t *c = &r_mecull[i];
		const cl_entity_t *e = r_mebones_me.ents[i]; // the main CPU may be sorting the list
		const meplayer_t *player = NULL;

		if( c->type != MECULL_BONES && c->type != MECULL_DRAW && c->type != MECULL_PLAYER && ( c->type != MECULL_SMALL || !frame->draw ))
			continue;
		if( r_mebones_me.slot[i] || *R_MeBonesSeen( e ) != last )
			continue;
		if( MEBONES_SYNC[MESYNC_YIELD] >= number )
			return;

		for( int k = 0; k < frame->numplayers; k++ )
		{
			if( frame->players[k].index == i )
				player = &frame->players[k];
		}

		mebones_ent_t *m = R_MeGather( frame, e, c->hdr, player );
		if( !m )
			continue;

		R_MeBones( m, NULL );
		if( frame->draw && c->type != MECULL_BONES )
		{
			R_MeDynamicLight( frame, e, c->small, &m->light );
			m->lit = true;
		}
		r_mebones_me.slot[i] = m - r_mebones_ents + 1;
	}
}

// before the view is known
static void R_MeEarlyJob( void *arg )
{
	const mebones_early_t *job = arg;
	const mebones_frame_t *frame = job->frame;
	const uint32_t number = job - r_mebones.early + 1;
	const int numsolid = job->firstsolid + job->numsolid, numtrans = job->firsttrans + job->numtrans;

	if( number == 1 )
	{
		r_mebones_me.count = r_mebones_me.used = 0;
		memset( r_mebones_me.slot, 0, sizeof( r_mebones_me.slot ));
		if( frame->draw )
			R_MeLightCheck( frame );
	}

	for( int i = job->firstsolid; i < numsolid; i++ )
	{
		r_mebones_me.ents[i] = frame->solid[i];
		R_MeClassify( frame, frame->solid[i], &r_mecull[i] );
	}

	for( int i = job->firsttrans; i < numtrans; i++ )
	{
		r_mebones_me.ents[MECULL_MAXSOLID + i] = frame->trans[i];
		R_MeClassify( frame, frame->trans[i], &r_mecull[MECULL_MAXSOLID + i] );
		R_MeSortKey( frame->trans[i], &r_mesort[i] );
	}

	for( int i = 0; i < frame->numplayers && number == 1; i++ )
	{
		const meplayer_t *p = &frame->players[i];
		mecull_t *c = &r_mecull[p->index];

		R_MeSphere( frame, frame->solid[p->index], p->mod, p->hdr, c );
		c->type = MECULL_PLAYER;
		c->mode = MEMODE_SOLID;
		c->small = false;
		c->rpx = 1e6f; // never too small
	}

	PSP_MeWriteback( r_mecull + job->firstsolid, job->numsolid * sizeof( r_mecull[0] ));
	PSP_MeWriteback( r_mecull + MECULL_MAXSOLID + job->firsttrans, job->numtrans * sizeof( r_mecull[0] ));
	PSP_MeWriteback( r_mesort + job->firsttrans, job->numtrans * sizeof( r_mesort[0] ));
	MEBONES_SYNC[MESYNC_TABLE] = number;

	R_MeEarlyBones( job, 0, numsolid, number );
	R_MeEarlyBones( job, MECULL_MAXSOLID, numtrans, number );
}

static inline uint *R_MeGate( const mebones_job_t *job, int i )
{
	return &job->gates[i * MEBONES_GATEWORDS];
}

static void R_MeGateClose( const mebones_job_t *job, int i )
{
	volatile uint *const gate = (volatile uint *)( 0x40000000 | (uintptr_t)R_MeGate( job, i ));
	const uintptr_t self = (uintptr_t)( R_MeGate( job, i ) + 1 );

	gate[0] = 0x10000000 | (( self >> 8 ) & 0xf0000 ); // BASE
	gate[1] = 0x08000000 | ( self & 0xffffff ); // JUMP
}

// to the commands, NULL: to nothing
static void R_MeGateOpen( const mebones_job_t *job, int i, uintptr_t cmds )
{
	volatile uint *const gate = (volatile uint *)( 0x40000000 | (uintptr_t)R_MeGate( job, i ));

	if( cmds )
	{
		gate[2] = 0x10000000 | (( cmds >> 8 ) & 0xf0000 );
		gate[3] = 0x08000000 | ( cmds & 0xffffff );
		__asm__ volatile( "sync" ::: "memory" );
	}
	gate[1] = cmds ? 0 : 0x0B000000; // NOP, RET
}

// the GE may have the solid models
static void R_MeSolidDone( const mebones_job_t *job )
{
	uint *p = r_mebones_me.p;

	*p++ = 0x0B000000; // RET
	PSP_MeWriteback( r_mebones_me.p, sizeof( *p ));
	r_mebones_me.p = p;
	R_MeGateOpen( job, MEBONES_SOLIDGATE, (uintptr_t)job->cmds );
}

static void R_MeBonesJob( void *arg )
{
	const mebones_job_t *job = arg;

	r_mebones_me.p = job->cmds;

	for( int i = 0; i < job->count; i++ )
	{
		if( i == job->numsolid )
			R_MeSolidDone( job );

		const mebones_item_t *it = &job->items[i];
		const int slot = r_mebones_me.slot[it->index];
		const mecull_t *c = &r_mecull[it->index];
		mebones_ent_t *m = slot ? &r_mebones_ents[slot - 1] : R_MeGather( job->frame, it->e, c->hdr, it->player );
		mebones_res_t *res = &r_mebones_res[i];

		res->scratch = false;
		if( !m && it->draw )
		{
			// no space: one after the other in the same place
			r_mebones_me.scratch = res->scratch = true;
			m = R_MeGather( job->frame, it->e, c->hdr, it->player );
			r_mebones_me.scratch = false;
		}

		res->cmds = 0;
		res->ent = m;
		res->visible = true;
		res->events = true;

		*R_MeBonesSeen( it->e ) = job->frame->framecount + 1;

		if( m )
		{
			res->visible = !it->draw || !R_MeBonesCulled( job, m, it->player ? it->player->mod : it->e->model );
			res->blendprev = m->blendprev;
			res->prevframe = m->prevframe;
			Matrix3x4_Copy( res->rotation, m->rotation );

			if( res->visible && !m->done )
				R_MeBones( m, NULL );

			if( res->visible && it->draw )
			{
				const mstudioattachment_t *pAtt = (const mstudioattachment_t *)((const byte *)m->hdr + m->hdr->attachmentindex );
				uint *const cmds = r_mebones_me.p;
				melight_t light;

				for( int k = 0; k < m->hdr->numattachments; k++ )
				{
					const float (*b)[4] = (const float (*)[4])m->out[pAtt[k].bone];

					for( int j = 0; j < 3; j++ )
						res->attachment[k][j] = DotProduct( pAtt[k].org, b[j] ) + b[j][3];
				}

				// as R_StudioClientEvents
				for( int k = 0; k < MEBONES_ATTACHMENTS && m->hdr->numattachments <= 0; k++ )
					Matrix3x4_OriginFromMatrix( m->rotation, res->attachment[k] );

				res->events = R_MeEvents( job->frame, it->e, m );

				if( !m->lit )
					R_MeDynamicLight( job->frame, it->e, c->small, &m->light );
				light = m->light;
				res->floor = light.floor;
				if( job->numelights )
					R_MeEntityLight( job, m, it->e, (const float (*)[3])res->attachment, &light );

				uint *const p = R_MeModelCommands( job, it, m, &light );
				if( p )
				{
					res->cmds = (uint32_t)cmds;
					r_mebones_me.p = p;
				}
				PSP_MeWriteback( cmds, (byte *)r_mebones_me.p - (byte *)cmds );
			}
		}

		PSP_MeWriteback( res, sizeof( *res ));
		if( i >= job->numsolid && it->draw )
			R_MeGateOpen( job, i, res->cmds );
		MEBONES_SYNC[MESYNC_ITEMS] = i + 1;
	}

	if( job->numsolid == job->count )
		R_MeSolidDone( job );
}

static const struct medraw_s *R_StudioMeDrawGet( const studiohdr_t *hdr, cl_entity_t *e, int body, int skin, const mstudiotexture_t *remap );
static int R_StudioMeDrawWords( const mebones_item_t *it );
static void R_StudioMeElights( mebones_job_t *job );
static void R_StudioMeAllocCmds( void );
static model_t *R_StudioSetupPlayerModel( int index );
static void R_StudioProcessGait( entity_state_t *pplayer );

static void R_StudioMeReset( void )
{
	r_mebones.finish = false;
	// the last jobs still read the frame and write the pool
	PSP_MeWaitFor( r_mebones.early_ticket );
	PSP_MeWaitFor( r_mebones.ticket );
	r_mebones.ticket = r_mebones.early_ticket = 0;
	r_mebones.table = r_mebones.tableread = false;
	r_mebones.count = 0;
}

// the draw lists aren't the ones the table is about anymore
void R_StudioMeSceneCleared( void )
{
	r_mebones.table = false;
}

// models may be drawn by R_StudioDrawMeModels and R_StudioDrawMeModel: what R_StudioGEAllowed and R_StudioGEDrawPlan check, or draw besides
static qboolean R_StudioMeDrawAllowed( void )
{
	return r_studio_me_draw.value && r_studio_ge.value && r_studio_geplan.value && r_studio_ge_hwlight.value
		&& r_studio_ge_elights.value && !glState.isFogEnabled && !r_lightmap->value && !r_fullbright->value && !r_shadows.value
		&& r_drawentities->value == 1.0f;
}

static const memerge_t *R_StudioMeMergeGet( const studiohdr_t *hdr, const studiohdr_t *phdr )
{
	static memerge_t cache[MEMERGE_CACHE];
	memerge_t *c = &cache[0];

	for( int i = 0; i < MEMERGE_CACHE; i++ )
	{
		if( cache[i].hdr == hdr && cache[i].phdr == phdr )
		{
			cache[i].frame = tr.framecount;
			return &cache[i];
		}
		if( cache[i].frame < c->frame )
			c = &cache[i];
	}

	// the ME may read it
	if( c->hdr && c->frame == tr.framecount )
		return NULL;

	const mstudiobone_t *pbones = (const mstudiobone_t *)((const byte *)hdr + hdr->boneindex );
	const mstudiobone_t *ppbones = (const mstudiobone_t *)((const byte *)phdr + phdr->boneindex );

	c->hdr = hdr;
	c->phdr = phdr;
	c->frame = tr.framecount;

	for( int i = 0; i < hdr->numbones; i++ )
	{
		c->map[i] = -1;
		for( int j = 0; j < phdr->numbones && c->map[i] < 0; j++ )
		{
			if( !Q_stricmp( pbones[i].name, ppbones[j].name ))
				c->map[i] = j;
		}
	}

	return c;
}

// the model of a player and the textures with the player's colors, NULL if it has no such textures
static const mstudiotexture_t *R_StudioMeRemap( cl_entity_t *e, model_t *mod, int index )
{
	const player_info_t *info = gEngfuncs.pfnPlayerInfo( index );
	const int top = bound( 0, info->topcolor, 360 ), bottom = bound( 0, info->bottomcolor, 360 );

	if( !gEngfuncs.CL_EntitySetRemapColors( e, mod, top, bottom ))
		return NULL;

	return gEngfuncs.CL_GetRemapInfoForEntity( e )->ptexture;
}

static qboolean R_StudioMeModelOk( const studiohdr_t *hdr )
{
	return hdr && hdr->numseq > 0 && hdr->numbones > 0 && hdr->numbones <= MAXSTUDIOBONES && !FBitSet( hdr->flags, STUDIO_HAS_BONEWEIGHTS );
}

/*
====================
R_StudioMePlayer

R_StudioDrawPlayer before the bones, for a player R_StudioDrawMeModels will
draw; false if it can't
====================
*/
static qboolean R_StudioMePlayer( cl_entity_t *e, meplayer_t *p )
{
	const int index = e->index - 1;

	if( e->curstate.rendermode != kRenderNormal || e->curstate.renderfx != kRenderFxNone || e->curstate.movetype == MOVETYPE_STEP
		|| e->curstate.movetype == MOVETYPE_FOLLOW || index < 0 || index >= gp_cl->maxclients )
		return false;

	entity_state_t *pplayer = gEngfuncs.pfnGetPlayerState( index );
	if( pplayer->number - 1 != index )
		return false;

	RI.currententity = e;
	model_t *mod = R_StudioSetupPlayerModel( index );
	if( !mod )
		return false;

	studiohdr_t *hdr = gEngfuncs.Mod_Extradata( mod_studio, mod );
	if( !R_StudioMeModelOk( hdr ) || hdr->numattachments > MEBONES_ATTACHMENTS || e->curstate.sequence < 0 || e->curstate.sequence >= hdr->numseq
		|| pplayer->gaitsequence < 0 || pplayer->gaitsequence >= hdr->numseq )
		return false;

	p->mod = mod;
	p->hdr = hdr;
	p->wmod = NULL;
	p->whdr = NULL;

	// as R_StudioDrawPlayer, which leaves them in the entity
	if( cl_himodels->value && mod != e->model )
		e->curstate.body = 255;
	if( !( !gpGlobals->developer && gp_cl->maxclients == 1 ) && mod == e->model )
		e->curstate.body = 1; // force helmet

	if( e->curstate.body < 0 || e->curstate.body > 0xffff )
		return false;

	RI.currentmodel = mod;
	if( !R_StudioMeDrawGet( hdr, e, e->curstate.body, e->curstate.skin, R_StudioMeRemap( e, mod, index )))
		return false;

	if( pplayer->weaponmodel )
	{
		model_t *wmod = CL_ModelHandle( pplayer->weaponmodel );
		const studiohdr_t *whdr = gEngfuncs.Mod_Extradata( mod_studio, wmod );

		if( !R_StudioMeModelOk( whdr ) || !( p->merge = R_StudioMeMergeGet( whdr, hdr )))
			return false;

		RI.currentmodel = wmod;
		if( !R_StudioMeDrawGet( whdr, e, e->curstate.body, e->curstate.skin, NULL ))
			return false;

		p->wmod = wmod;
		p->whdr = whdr;
	}

	player_info_t *info = gEngfuncs.pfnPlayerInfo( index );

	m_pStudioHeader = hdr;
	p->gaitsequence = pplayer->gaitsequence;

	if( pplayer->gaitsequence )
	{
		vec3_t orig_angles = Vec3( e->angles );

		m_pPlayerInfo = info;
		R_StudioProcessGait( pplayer );
		m_pPlayerInfo = NULL;
		p->yaw = e->angles[YAW];
		p->gaitframe = info->gaitframe;
		VectorCopy( orig_angles, e->angles );
	}
	else
	{
		for( int i = 0; i < 4; i++ )
			e->curstate.controller[i] = e->latched.prevcontroller[i] = 127;
	}

	info->gaitsequence = pplayer->gaitsequence;
	info->renderframe = tr.realframecount;
	return true;
}

// what the light of the models comes from
static void R_StudioMeLights( mebones_frame_t *frame )
{
	const movevars_t *mv = gp_movevars;
	const float time = frame->time;

	frame->frametime = gp_host->frametime;
	frame->world = WORLDMODEL;
	frame->sky = mv->skycolor[0] + mv->skycolor[1] + mv->skycolor[2] != 0;
	VectorCopy( mv->skyvec, frame->skyvec );
	VectorScale( mv->skyvec, FBitSet( gp_host->features, ENGINE_WRITE_LARGE_COORD ) ? 65536.0f : 8192.0f, frame->skyend );
	for( int i = 0; i < 3; i++ )
		frame->skycolor[i] = mv->skycolor[i];
	frame->direct = bound( 0.75f, v_direct->value, 1.0f );
	frame->gamma = FBitSet( gp_host->features, ENGINE_LINEAR_GAMMA_SPACE ) ? NULL : tr.lightgammatable;

	frame->numdlights = 0;
	for( int i = 0; i < MAX_DLIGHTS && r_dynamic->value; i++ )
	{
		const dlight_t *dl = &gp_dlights[i];
		medlight_t *l = &frame->dlights[frame->numdlights];

		if( !( dl->radius > 0.0f ) || dl->die < time )
			continue;

		VectorCopy( dl->origin, l->origin );
		l->radius = dl->radius;
		VectorSet( l->color, dl->color.r, dl->color.g, dl->color.b );
		frame->numdlights++;
	}

	frame->numphys = 0;
	for( int i = 0; i < ( r_lighting_extended.value ? MAX_PHYSENTS : 1 ) && frame->numphys < MELIGHT_MAXPHYS; i++ )
	{
		const physent_t *pe = gEngfuncs.EV_GetPhysent( i );
		mephys_t *p = &frame->phys[frame->numphys];

		if( !pe )
			break;
		if( !pe->model || pe->model->type != mod_brush )
			continue;

		p->model = pe->model;
		VectorAdd( pe->model->hulls[0].clip_mins, pe->origin, p->offset );
		p->rotated = !VectorIsNull( pe->angles );
		if( p->rotated )
			Matrix3x4_CreateFromEntity( p->matrix, pe->angles, p->offset, 1.0f );
		frame->numphys++;
	}
}

// the P5 RAM is the map's while it's loading, and free after
static void R_StudioMeAllocCmds( void )
{
	if( r_mebones.alloc || r_mebones.allocfailed )
		return;

	r_mebones.words = MEBONES_CMDS;
	r_mebones.alloc = PSP_VolatileAlloc( r_mebones.words * sizeof( uint ) * 2 + 64 );
	if( !r_mebones.alloc )
	{
		r_mebones.words = MEBONES_CMDSMIN;
		r_mebones.alloc = malloc( r_mebones.words * sizeof( uint ) * 2 + 64 );
	}

	if( !r_mebones.alloc )
	{
		r_mebones.allocfailed = true;
		r_mebones.words = 0;
		return;
	}

	r_mebones.cmds[0] = (uint *)(((uintptr_t)r_mebones.alloc + 63 ) & ~63 );
	r_mebones.cmds[1] = r_mebones.cmds[0] + r_mebones.words;
	sceKernelDcacheWritebackInvalidateRange( r_mebones.cmds[0], r_mebones.words * sizeof( uint ) * 2 );
}

void R_StudioMeFreeCmds( void )
{
	if( PSP_IsVolatile( r_mebones.alloc ))
		PSP_VolatileFree( r_mebones.alloc );
	else free( r_mebones.alloc );
	r_mebones.alloc = NULL;
	r_mebones.allocfailed = false;
	r_mebones.words = 0;
}

// the entities are going to be freed
void R_StudioMeDrop( void )
{
	const int count = r_mebones.count;

	R_StudioMeReset();
	r_mebones.begun = false;

	// the GE may be waiting for them, if the ME was given up on
	for( int i = r_mebones.job.numsolid; i < count; i++ )
		R_MeGateOpen( &r_mebones.job, i, 0 );
	if( count && MEBONES_SYNC[MESYNC_ITEMS] < r_mebones.job.numsolid )
		R_MeGateOpen( &r_mebones.job, MEBONES_SOLIDGATE, 0 );
	r_mebones.job.count = r_mebones.job.numsolid = 0;
}

/*
====================
R_StudioMeEarly

the draw lists have the packet entities in them, or all of them (complete);
the view isn't known yet
====================
*/
static qboolean R_StudioMeBegin( void )
{
	mebones_frame_t *frame = &r_mebones.frame;
	const draw_list_t *list = tr.draw_list;

	R_StudioMeReset();

	if( !r_studio_me.value || !WORLDMODEL || !PSP_MeStart( ))
		return false;

	if( list->num_trans_entities > MECULL_MAXTRANS || list->num_solid_entities > MECULL_MAXSOLID )
		return false;

	cl_entity_t *saveent = RI.currententity;
	model_t *savemodel = RI.currentmodel;
	studiohdr_t *savehdr = m_pStudioHeader;
	const int saveflags = RI.rvp.flags;
	const uint savedtex = glState.currentTexture; // the skin block lookups set it

	SetBits( RI.rvp.flags, RF_DRAW_WORLD );
	R_StudioSetupTimings();

	frame->time = g_studio.time;
	frame->interpolate = g_studio.interpolate;
	frame->steplerp = !FBitSet( gp_host->features, ENGINE_COMPUTE_STUDIO_LERP );
	frame->negpitch = !FBitSet( gp_host->features, ENGINE_COMPENSATE_QUAKE_BUG );
	frame->solid = (cl_entity_t **)list->solid_entities;
	frame->trans = (cl_entity_t **)list->trans_entities;
	frame->framecount = tr.framecount;
	frame->numplayers = 0;
	frame->draw = R_StudioMeDrawAllowed();
	if( frame->draw )
	{
		R_StudioMeAllocCmds();
		R_StudioMeLights( frame );
	}

	if( r_studio_me_players.value && list->num_players > 0 && frame->draw )
	{
		tr.realframecount++; // the one they'll be drawn in

		for( int i = 0; i < list->num_players && frame->numplayers < MEBONES_MAXPLAYERS; i++ )
		{
			meplayer_t *p = &frame->players[frame->numplayers];

			p->index = list->players[i];
			if( R_StudioMePlayer( list->solid_entities[p->index], p ))
				frame->numplayers++;
		}

		tr.realframecount--;
	}

	glState.currentTexture = savedtex;
	RI.rvp.flags = saveflags;
	RI.currententity = saveent;
	RI.currentmodel = savemodel;
	m_pStudioHeader = savehdr;
	return true;
}

static void R_StudioMeEarly( qboolean complete )
{
	mebones_early_t *job = &r_mebones.early[complete ? 1 : 0];
	const draw_list_t *list = tr.draw_list;

	if( !complete )
	{
		r_mebones.begun = R_StudioMeBegin();
		if( !r_mebones.begun )
			return;

		job->firstsolid = job->firsttrans = 0;
		MEBONES_SYNC[MESYNC_TABLE] = MEBONES_SYNC[MESYNC_YIELD] = 0;
	}
	else
	{
		const mebones_early_t *last = &r_mebones.early[0];

		if( !r_mebones.begun )
			return;

		r_mebones.begun = false;
		job->firstsolid = last->numsolid;
		job->firsttrans = last->numtrans;

		// the lists are others now, or too long
		if( r_mebones.frame.solid != (cl_entity_t **)list->solid_entities || list->num_solid_entities < job->firstsolid || list->num_trans_entities < job->firsttrans
			|| list->num_trans_entities > MECULL_MAXTRANS || list->num_solid_entities > MECULL_MAXSOLID )
			return;
		R_StudioMeYield( 1 );
	}

	job->frame = &r_mebones.frame;
	job->numsolid = list->num_solid_entities - job->firstsolid;
	job->numtrans = list->num_trans_entities - job->firsttrans;
	r_mebones.early_ticket = PSP_MeRunKeep( R_MeEarlyJob, job );
	r_mebones.table = complete && r_mebones.early_ticket != 0;
}

// the early job with this number shouldn't keep the next one waiting
void R_StudioMeYield( int number )
{
	MEBONES_SYNC[MESYNC_YIELD] = number;
}

/*
====================
R_StudioMeTable

this frame's table, or NULL: the solid entities, then (from MECULL_MAXSOLID on)
the translucent ones in the order they had before they were sorted, with their
sort keys
====================
*/
const mecull_t *R_StudioMeTable( int *numsolid, int *numtrans, const mesort_t **sort )
{
	const mebones_early_t *early = &r_mebones.early[1];
	const int solid = early->firstsolid + early->numsolid, trans = early->firsttrans + early->numtrans;

	if( !r_mebones.table || !FBitSet( RI.rvp.flags, RF_DRAW_WORLD )
		|| tr.draw_list->num_solid_entities < solid || tr.draw_list->num_trans_entities < trans
		|| r_mebones.frame.solid != (cl_entity_t **)tr.draw_list->solid_entities )
		return NULL;

	if( !r_mebones.tableread )
	{
		for( uint n = 0; MEBONES_SYNC[MESYNC_TABLE] < 2 && !PSP_MeDone( r_mebones.early_ticket ); n++ )
		{
			if( n > 1000000 )
				PSP_MeWaitFor( r_mebones.early_ticket ); // the ME's timeout
		}

		if( MEBONES_SYNC[MESYNC_TABLE] < 2 )
		{
			r_mebones.table = false;
			return NULL;
		}

		sceKernelDcacheInvalidateRange( r_mecull, solid * sizeof( r_mecull[0] ));
		sceKernelDcacheInvalidateRange( r_mecull + MECULL_MAXSOLID, trans * sizeof( r_mecull[0] ));
		sceKernelDcacheInvalidateRange( r_mesort, trans * sizeof( r_mesort[0] ));
		r_mebones.tableread = true;
	}

	*numsolid = solid;
	*numtrans = trans;
	if( sort ) *sort = r_mesort;
	return r_mecull;
}

// models the ME couldn't draw (out of space, a sequence group not loaded) are the main CPU's for half a second
static struct
{
	const cl_entity_t	*e;
	double		time;
} r_merefused[16];

static void R_StudioMeRefuse( const cl_entity_t *e )
{
	static uint next;

	r_merefused[next % 16].e = e;
	r_merefused[next++ % 16].time = gp_cl->time;
	r_mebones.refused = gp_cl->time + 0.5;
}

static qboolean R_StudioMeRefused( const cl_entity_t *e )
{
	if( gp_cl->time >= r_mebones.refused )
		return false;

	for( int i = 0; i < 16; i++ )
	{
		if( r_merefused[i].e == e && gp_cl->time >= r_merefused[i].time && gp_cl->time < r_merefused[i].time + 0.5 )
			return true;
	}

	return false;
}

// true: R_StudioDrawMeModels draws it
static qboolean R_StudioMeAdd( cl_entity_t *e, const mecull_t *c, qboolean draw )
{
	mebones_job_t *job = &r_mebones.job;
	mebones_item_t *it = &job->items[job->count];

	if( job->count >= MEBONES_MAXENTS )
		return false;

	it->e = e;
	it->index = c - r_mecull;
	it->draw = it->wdraw = NULL;
	it->player = NULL;
	it->mode = c->mode;

	// the solid ones are drawn in one go
	if( it->mode != MEMODE_SOLID && it->index < MECULL_MAXSOLID )
		draw = false;
	if( R_StudioMeRefused( e ))
		draw = false;

	switch( c->type )
	{
	case MECULL_PLAYER:
		for( int i = 0; i < r_mebones.frame.numplayers; i++ )
		{
			if( r_mebones.frame.players[i].index == it->index )
				it->player = &r_mebones.frame.players[i];
		}

		if( !draw || !it->player )
			return false;

		RI.currentmodel = (model_t *)it->player->mod;
		it->draw = R_StudioMeDrawGet( c->hdr, e, c->body, c->skin, R_StudioMeRemap( e, RI.currentmodel, e->index - 1 ));
		if( !it->draw )
			return false;

		if( it->player->whdr )
		{
			RI.currentmodel = (model_t *)it->player->wmod;
			it->wdraw = R_StudioMeDrawGet( it->player->whdr, e, c->body, c->skin, NULL );
			if( !it->wdraw )
				return false;
		}

		if( job->words + R_StudioMeDrawWords( it ) > r_mebones.words - 4 )
			return false;
		break;
	case MECULL_SMALL:
		if( !draw )
			return false;
		// fallthrough
	case MECULL_DRAW:
		if( draw )
			it->draw = R_StudioMeDrawGet( c->hdr, e, c->body, c->skin, NULL );
		if( it->draw && job->words + R_StudioMeDrawWords( it ) > r_mebones.words - 4 )
			it->draw = NULL; // no space for its commands
		if( !it->draw && c->type == MECULL_SMALL )
			return false;
		break;
	case MECULL_BONES:
		break;
	default:
		return false;
	}

	job->count++;
	job->words += R_StudioMeDrawWords( it );
	return it->draw != NULL;
}

/*
====================
R_StudioMePost

the second job: the models R_StudioMeAdd took
====================
*/
static void R_StudioMePost( void )
{
	mebones_job_t *job = &r_mebones.job;

	if( !job->count )
		return;

	job->frame = &r_mebones.frame;
	job->numplanes = 0;
	for( int i = 0; i < FRUSTUM_PLANES && !r_nocull.value; i++ )
	{
		float *p = job->planes[job->numplanes];

		if( !FBitSet( RI.frustum.clipFlags, BIT( i )))
			continue;

		VectorCopy( RI.frustum.planes[i].normal, p );
		p[3] = -RI.frustum.planes[i].dist;
		job->numplanes++;
	}
	VectorCopy( RI.rvp.vieworigin, job->eye );
	VectorCopy( RI.vright, job->vright );
	R_StudioMeElights( job );

	job->cmds = r_mebones.cmds[guRender.vert_frame & 1];
	job->maxwords = r_mebones.words;
	job->gates = r_mebones_gates[guRender.vert_frame & 1];
	R_MeGateClose( job, MEBONES_SOLIDGATE );
	for( int i = job->numsolid; i < job->count; i++ )
		R_MeGateClose( job, i );

	MEBONES_SYNC[MESYNC_ITEMS] = 0;
	r_mebones.ticket = PSP_MeRunKeep( R_MeBonesJob, job );
	if( r_mebones.ticket )
		r_mebones.count = job->count;
}

// until the ME is done with item i, NULL if it was given up on
static const mebones_res_t *R_StudioMeWait( int i )
{
	for( uint n = 0; MEBONES_SYNC[MESYNC_ITEMS] <= (uint32_t)i && !PSP_MeDone( r_mebones.ticket ); n++ )
	{
		if( n > 1000000 )
			PSP_MeWaitFor( r_mebones.ticket ); // the ME's timeout
	}

	if( MEBONES_SYNC[MESYNC_ITEMS] <= (uint32_t)i )
		return NULL;

	sceKernelDcacheInvalidateRange( &r_mebones_res[i], sizeof( r_mebones_res[0] ));
	return &r_mebones_res[i];
}

static int R_StudioMeFind( const cl_entity_t *e )
{
	for( int i = 0; i < r_mebones.count; i++ )
	{
		if( r_mebones.job.items[i].e == e )
			return i;
	}

	return -1;
}

// its bones are the ME's
qboolean R_StudioMeBonesPosted( const cl_entity_t *e )
{
	return R_StudioMeFind( e ) >= 0;
}

// the bones of e from the ME, if they were computed from what R_StudioSetupBones sees now
static qboolean R_StudioMeBonesUse( cl_entity_t *e )
{
	const int i = R_StudioMeFind( e );
	const mebones_res_t *res;
	mebones_key_t key;

	if( i < 0 || !( res = R_StudioMeWait( i )))
		return false;

	// not computed if it's off screen, but it's a follower's parent
	const mebones_ent_t *m = res->ent;
	if( !m || !res->visible || res->scratch )
		return false;

	sceKernelDcacheInvalidateRange( m, sizeof( *m ));
	R_StudioMeBonesKey( &key, e, m_pStudioHeader, g_studio.time, g_studio.interpolate );
	if( memcmp( &key, &m->key, sizeof( key )))
	{
		r_mebones.misses++;
		return false;
	}

	for( int r = 0; r < 3; r++ )
	{
		for( int c = 0; c < 4; c++ )
		{
			if( fabsf( res->rotation[r][c] - g_studio.rotationmatrix[r][c] ) > ( c < 3 ? 0.001f : 0.02f ))
			{
				r_mebones.misses++;
				return false;
			}
		}
	}

	const int nb = m_pStudioHeader->numbones;
	const size_t size = nb * sizeof( matrix3x4 );

	sceKernelDcacheInvalidateRange( m->out, (( nb + 3 ) & ~3 ) * sizeof( matrix3x4 ));
	memcpy( g_studio.bonestransform, m->out, size );
	memcpy( g_studio.lighttransform, m->out, size );

	if( !m->blendprev )
		e->latched.prevframe = m->prevframe;

	r_mebones.hits++;
	return true;
}
#endif

static void R_StudioSetupBones( cl_entity_t *e )
{
	static vec3_t	pos[MAXSTUDIOBONES];
	static vec4_t	q[MAXSTUDIOBONES];
	static vec3_t	pos2[MAXSTUDIOBONES];
	static vec4_t	q2[MAXSTUDIOBONES];
	static vec3_t	pos3[MAXSTUDIOBONES];
	static vec4_t	q3[MAXSTUDIOBONES];
	static vec3_t	pos4[MAXSTUDIOBONES];
	static vec4_t	q4[MAXSTUDIOBONES];

	if( e->curstate.sequence >= m_pStudioHeader->numseq )
		e->curstate.sequence = 0;

#if XASH_PSP
	// computed on the ME before the world was drawn
	if( !m_pPlayerInfo && R_StudioMeBonesUse( e ))
		return;
#endif

	mstudioseqdesc_t *pseqdesc = (mstudioseqdesc_t *)((byte *)m_pStudioHeader + m_pStudioHeader->seqindex) + e->curstate.sequence;
	float f = R_StudioEstimateFrame( e, pseqdesc, g_studio.time );

	mstudioanim_t *panim = gEngfuncs.R_StudioGetAnim( m_pStudioHeader, RI.currentmodel, pseqdesc );
	R_StudioCalcRotations( e, pos, q, pseqdesc, panim, f );

	if( pseqdesc->numblends > 1 )
	{
		panim += m_pStudioHeader->numbones;
		R_StudioCalcRotations( e, pos2, q2, pseqdesc, panim, f );

		float dadt = R_StudioEstimateInterpolant( e );
		float s = (e->curstate.blending[0] * dadt + e->latched.prevblending[0] * (1.0f - dadt)) / 255.0f;

		R_StudioSlerpBones( m_pStudioHeader->numbones, q, pos, q2, pos2, s );

		if( pseqdesc->numblends == 4 )
		{
			panim += m_pStudioHeader->numbones;
			R_StudioCalcRotations( e, pos3, q3, pseqdesc, panim, f );

			panim += m_pStudioHeader->numbones;
			R_StudioCalcRotations( e, pos4, q4, pseqdesc, panim, f );

			s = (e->curstate.blending[0] * dadt + e->latched.prevblending[0] * (1.0f - dadt)) / 255.0f;
			R_StudioSlerpBones( m_pStudioHeader->numbones, q3, pos3, q4, pos4, s );

			s = (e->curstate.blending[1] * dadt + e->latched.prevblending[1] * (1.0f - dadt)) / 255.0f;
			R_StudioSlerpBones( m_pStudioHeader->numbones, q, pos, q3, pos3, s );
		}
	}

	if( g_studio.interpolate && e->latched.sequencetime && ( e->latched.sequencetime + 0.2f > g_studio.time ) && ( e->latched.prevsequence < m_pStudioHeader->numseq ))
	{
		// blend from last sequence
		static vec3_t	pos1b[MAXSTUDIOBONES];
		static vec4_t	q1b[MAXSTUDIOBONES];

		pseqdesc = (mstudioseqdesc_t *)((byte *)m_pStudioHeader + m_pStudioHeader->seqindex) + e->latched.prevsequence;
		panim = gEngfuncs.R_StudioGetAnim( m_pStudioHeader, RI.currentmodel, pseqdesc );

		// clip prevframe
		R_StudioCalcRotations( e, pos1b, q1b, pseqdesc, panim, e->latched.prevframe );

		if( pseqdesc->numblends > 1 )
		{
			panim += m_pStudioHeader->numbones;
			R_StudioCalcRotations( e, pos2, q2, pseqdesc, panim, e->latched.prevframe );

			float s = (e->latched.prevseqblending[0]) / 255.0f;
			R_StudioSlerpBones( m_pStudioHeader->numbones, q1b, pos1b, q2, pos2, s );

			if( pseqdesc->numblends == 4 )
			{
				panim += m_pStudioHeader->numbones;
				R_StudioCalcRotations( e, pos3, q3, pseqdesc, panim, e->latched.prevframe );

				panim += m_pStudioHeader->numbones;
				R_StudioCalcRotations( e, pos4, q4, pseqdesc, panim, e->latched.prevframe );

				s = (e->latched.prevseqblending[0]) / 255.0f;
				R_StudioSlerpBones( m_pStudioHeader->numbones, q3, pos3, q4, pos4, s );

				s = (e->latched.prevseqblending[1]) / 255.0f;
				R_StudioSlerpBones( m_pStudioHeader->numbones, q1b, pos1b, q3, pos3, s );
			}
		}

		float s = 1.0f - ( g_studio.time - e->latched.sequencetime ) / 0.2f;
		R_StudioSlerpBones( m_pStudioHeader->numbones, q, pos, q1b, pos1b, s );
	}
	else
	{
		// store prevframe otherwise
		e->latched.prevframe = f;
	}

	mstudiobone_t *pbones = (mstudiobone_t *)((byte *)m_pStudioHeader + m_pStudioHeader->boneindex);

	// calc gait animation
	if( m_pPlayerInfo && m_pPlayerInfo->gaitsequence != 0 )
	{
		qboolean	copy_bones = true;

		if( m_pPlayerInfo->gaitsequence >= m_pStudioHeader->numseq )
			m_pPlayerInfo->gaitsequence = 0;

		pseqdesc = (mstudioseqdesc_t *)((byte *)m_pStudioHeader + m_pStudioHeader->seqindex) + m_pPlayerInfo->gaitsequence;

		panim = gEngfuncs.R_StudioGetAnim( m_pStudioHeader, RI.currentmodel, pseqdesc );
		R_StudioCalcRotations( e, pos2, q2, pseqdesc, panim, m_pPlayerInfo->gaitframe );

		for( int i = 0; i < m_pStudioHeader->numbones; i++ )
		{
			if( !Q_strcmp( pbones[i].name, "Bip01 Spine" ))
				copy_bones = false;
			else if( !Q_strcmp( pbones[pbones[i].parent].name, "Bip01 Pelvis" ))
				copy_bones = true;

			if( !copy_bones ) continue;

			VectorCopy( pos2[i], pos[i] );
			Vector4Copy( q2[i], q[i] );
		}
	}

#if XASH_PSP
	// without these effects the light transforms are the bone transforms:
	// one chain of products and a copy instead of two chains
	if( e->curstate.renderfx != kRenderFxDistort && e->curstate.renderfx != kRenderFxHologram && e->curstate.renderfx != kRenderFxExplode )
	{
		const int numbones = m_pStudioHeader->numbones;

		for( int i = 0; i < numbones; i++ )
		{
			matrix3x4 bonematrix;
			R_StudioBoneMatrix( bonematrix, q[i], pos[i] );

			const int parent = pbones[i].parent;
			R_StudioConcat( g_studio.bonestransform[i], parent == -1 ? g_studio.rotationmatrix : g_studio.bonestransform[parent], bonematrix );
		}
		memcpy( g_studio.lighttransform, g_studio.bonestransform, numbones * sizeof( g_studio.bonestransform[0] ));
		return;
	}
#endif
	for( int i = 0; i < m_pStudioHeader->numbones; i++ )
	{
		matrix3x4 bonematrix;
		Matrix3x4_FromOriginQuat( bonematrix, q[i], pos[i] );

		if( pbones[i].parent == -1 )
		{
			Matrix3x4_ConcatTransforms( g_studio.bonestransform[i], g_studio.rotationmatrix, bonematrix );
			Matrix3x4_Copy( g_studio.lighttransform[i], g_studio.bonestransform[i] );

			// apply client-side effects to the transformation matrix
			R_StudioFxTransform( e, g_studio.bonestransform[i] );
		}
		else
		{
			Matrix3x4_ConcatTransforms( g_studio.bonestransform[i], g_studio.bonestransform[pbones[i].parent], bonematrix );
			Matrix3x4_ConcatTransforms( g_studio.lighttransform[i], g_studio.lighttransform[pbones[i].parent], bonematrix );
		}
	}
}

/*
====================
StudioSaveBones

====================
*/
#if XASH_PSP
/*
the copy waits until a merge needs it: most models have nothing that follows
them, and the names and matrices of ~50 bones are ~25 us a soldier. Nothing
changes the bone transforms between the save and the merge (the parent of a
MOVETYPE_FOLLOW model is set up again right before it, a player's weapon
model right after the player), and every bone setup saves again.
*/
static void R_StudioCopySavedBones( void )
{
	const mstudiobone_t *pbones = g_studio.cached_bones;

	if( !pbones )
		return;
	g_studio.cached_bones = NULL;

	for( int i = 0; i < g_studio.cached_numbones; i++ )
	{
		Matrix3x4_Copy( g_studio.cached_bonestransform[i], g_studio.bonestransform[i] );
		Matrix3x4_Copy( g_studio.cached_lighttransform[i], g_studio.lighttransform[i] );
		Q_strncpy( g_studio.cached_bonenames[i], pbones[i].name, 32 );
	}
}
#endif

static void R_StudioSaveBones( void )
{
	mstudiobone_t *pbones = (mstudiobone_t *)((byte *)m_pStudioHeader + m_pStudioHeader->boneindex);
	g_studio.cached_numbones = m_pStudioHeader->numbones;
#if XASH_PSP
	g_studio.cached_bones = pbones;
	return;
#endif

	for( int i = 0; i < m_pStudioHeader->numbones; i++ )
	{
		Matrix3x4_Copy( g_studio.cached_bonestransform[i], g_studio.bonestransform[i] );
		Matrix3x4_Copy( g_studio.cached_lighttransform[i], g_studio.lighttransform[i] );
		Q_strncpy( g_studio.cached_bonenames[i], pbones[i].name, 32 );
	}
}

/*
====================
StudioBuildNormalTable

NOTE: m_pSubModel must be set
====================
*/
static void R_StudioBuildNormalTable( void )
{
	cl_entity_t	*e = RI.currententity;

	Assert( m_pSubModel != NULL );

	// reset chrome cache
	for( int i = 0; i < m_pStudioHeader->numbones; i++ )
		g_studio.chromeage[i] = 0;

	for( int i = 0; i < m_pSubModel->numverts; i++ )
		g_studio.normaltable[i] = -1;

	for( int j = 0; j < m_pSubModel->nummesh; j++ )
	{
		mstudiomesh_t *pmesh = (mstudiomesh_t *)((byte *)m_pStudioHeader + m_pSubModel->meshindex) + j;
		short *ptricmds = (short *)((byte *)m_pStudioHeader + pmesh->triindex);

		int i;
		while(( i = *( ptricmds++ )))
		{
			if( i < 0 ) i = -i;

			for( ; i > 0; i--, ptricmds += 4 )
			{
				if( g_studio.normaltable[ptricmds[0]] < 0 )
					g_studio.normaltable[ptricmds[0]] = ptricmds[1];
			}
		}
	}

	g_studio.chrome_origin[0] = cos( r_glowshellfreq->value * g_studio.time ) * 4000.0f;
	g_studio.chrome_origin[1] = sin( r_glowshellfreq->value * g_studio.time ) * 4000.0f;
	g_studio.chrome_origin[2] = cos( r_glowshellfreq->value * g_studio.time * 0.33f ) * 4000.0f;

	if( e->curstate.rendercolor.r || e->curstate.rendercolor.g || e->curstate.rendercolor.b )
		TriColor4ub( e->curstate.rendercolor.r, e->curstate.rendercolor.g, e->curstate.rendercolor.b, 255 );
	else TriColor4ub( 255, 255, 255, 255 );
}

/*
====================
StudioGenerateNormals

NOTE: m_pSubModel must be set
g_studio.verts must be computed
====================
*/
static void R_StudioGenerateNormals( void )
{
	Assert( m_pSubModel != NULL );

	for( int i = 0; i < m_pSubModel->numverts; i++ )
		VectorClear( g_studio.norms[i] );

	for( int j = 0; j < m_pSubModel->nummesh; j++ )
	{
		mstudiomesh_t *pmesh = (mstudiomesh_t *)((byte *)m_pStudioHeader + m_pSubModel->meshindex) + j;
		short *ptricmds = (short *)((byte *)m_pStudioHeader + pmesh->triindex);

		int i;
		while(( i = *( ptricmds++ )))
		{
			if( i < 0 )
			{
				i = -i;

				if( i > 2 )
				{
					vec3_t e0, e1, norm;
					int v0 = ptricmds[0]; ptricmds += 4;
					int v1 = ptricmds[0]; ptricmds += 4;

					for( i -= 2; i > 0; i--, ptricmds += 4 )
					{
						int v2 = ptricmds[0];

						VectorSubtract( g_studio.verts[v1], g_studio.verts[v0], e0 );
						VectorSubtract( g_studio.verts[v2], g_studio.verts[v0], e1 );
						CrossProduct( e1, e0, norm );

						VectorAdd( g_studio.norms[v0], norm, g_studio.norms[v0] );
						VectorAdd( g_studio.norms[v1], norm, g_studio.norms[v1] );
						VectorAdd( g_studio.norms[v2], norm, g_studio.norms[v2] );

						v1 = v2;
					}
				}
				else
				{
					ptricmds += i;
				}
			}
			else
			{
				if( i > 2 )
				{
					qboolean	odd = false;
					vec3_t e0, e1, norm;
					int v0 = ptricmds[0]; ptricmds += 4;
					int v1 = ptricmds[0]; ptricmds += 4;

					for( i -= 2; i > 0; i--, ptricmds += 4 )
					{
						int v2 = ptricmds[0];

						VectorSubtract( g_studio.verts[v1], g_studio.verts[v0], e0 );
						VectorSubtract( g_studio.verts[v2], g_studio.verts[v0], e1 );
						CrossProduct( e1, e0, norm );

						VectorAdd( g_studio.norms[v0], norm, g_studio.norms[v0] );
						VectorAdd( g_studio.norms[v1], norm, g_studio.norms[v1] );
						VectorAdd( g_studio.norms[v2], norm, g_studio.norms[v2] );

						if( odd ) v1 = v2;
						else v0 = v2;

						odd = !odd;
					}
				}
				else
				{
					ptricmds += i;
				}
			}
		}
	}

	for( int i = 0; i < m_pSubModel->numverts; i++ )
		VectorNormalize( g_studio.norms[i] );
}

/*
====================
StudioSetupChrome

====================
*/
static void R_StudioSetupChrome( float *pchrome, int bone, vec3_t normal )
{
	if( g_studio.chromeage[bone] != g_studio.framecount )
	{
		// calculate vectors from the viewer to the bone. This roughly adjusts for position
		vec3_t	chromeupvec;	// g_studio.chrome t vector in world reference frame
		vec3_t	chromerightvec;	// g_studio.chrome s vector in world reference frame
		vec3_t	tmp;		// vector pointing at bone in world reference frame

		VectorNegate( g_studio.chrome_origin, tmp );
		tmp[0] += g_studio.lighttransform[bone][0][3];
		tmp[1] += g_studio.lighttransform[bone][1][3];
		tmp[2] += g_studio.lighttransform[bone][2][3];

		VectorNormalize( tmp );
		CrossProduct( tmp, RI.vright, chromeupvec );
		VectorNormalize( chromeupvec );
		CrossProduct( chromeupvec, tmp, chromerightvec );
		VectorNormalize( chromerightvec );

		Matrix3x4_VectorIRotate( g_studio.lighttransform[bone], chromeupvec, g_studio.chromeup[bone] );
		Matrix3x4_VectorIRotate( g_studio.lighttransform[bone], chromerightvec, g_studio.chromeright[bone] );

		g_studio.chromeage[bone] = g_studio.framecount;
	}

	// calc s coord
	float n = DotProduct( normal, g_studio.chromeright[bone] );
	pchrome[0] = (n + 1.0f) * 32.0f;

	// calc t coord
	n = DotProduct( normal, g_studio.chromeup[bone] );
	pchrome[1] = (n + 1.0f) * 32.0f;
}

/*
====================
StudioCalcAttachments

====================
*/
static void R_StudioCalcAttachments( void )
{
	// calculate attachment points
	mstudioattachment_t *pAtt = (mstudioattachment_t *)((byte *)m_pStudioHeader + m_pStudioHeader->attachmentindex);

	for( int i = 0; i < Q_min( MAXSTUDIOATTACHMENTS, m_pStudioHeader->numattachments ); i++ )
	{
		Matrix3x4_VectorTransform( g_studio.lighttransform[pAtt[i].bone], pAtt[i].org, RI.currententity->attachment[i] );
	}
}

/*
===============
pfnStudioSetupModel

===============
*/
static void R_StudioSetupModel( int bodypart, void **ppbodypart, void **ppsubmodel )
{
	if( bodypart > m_pStudioHeader->numbodyparts )
		bodypart = 0;

	m_pBodyPart = (mstudiobodyparts_t *)((byte *)m_pStudioHeader + m_pStudioHeader->bodypartindex) + bodypart;

	int index = RI.currententity->curstate.body / m_pBodyPart->base;
	index = index % m_pBodyPart->nummodels;

	m_pSubModel = (mstudiomodel_t *)((byte *)m_pStudioHeader + m_pBodyPart->modelindex) + index;

	if( ppbodypart ) *ppbodypart = m_pBodyPart;
	if( ppsubmodel ) *ppsubmodel = m_pSubModel;
}

/*
===============
R_StudioCheckBBox

===============
*/
static int R_StudioCheckBBox( void )
{
	if( !RI.currententity || !RI.currentmodel )
		return false;

	return R_StudioComputeBBox( NULL );
}

/*
===============
pfnStudioEntityLight

===============
*/
// the live entity lights, collected once per frame: walking all MAX_ELIGHTS
// for every model costs ~130 us each on the PSP (cache misses, and comparing
// with the double g_studio.time is done in software). One allocated while
// models are drawn (muzzle flash event) is picked up the frame after.
static struct
{
	int	frame;
	double	time;
	float	ftime;	// dlight_t::die is a float anyway
	int	count;
	byte	index[MAX_ELIGHTS];
} r_live_elights = { -1 };

static void R_StudioCollectElights( void )
{
	if( r_live_elights.frame == tr.realframecount && r_live_elights.time == g_studio.time )
		return;

	r_live_elights.frame = tr.realframecount;
	r_live_elights.time = g_studio.time;
	r_live_elights.ftime = g_studio.time;
	r_live_elights.count = 0;

	for( int lnum = 0; lnum < MAX_ELIGHTS; lnum++ )
	{
		const dlight_t *el = &tr.elights[lnum];

		if( el->die >= r_live_elights.ftime && el->radius > 0.0f )
			r_live_elights.index[r_live_elights.count++] = lnum;
	}
}

#if XASH_PSP
// the live ones for the Media Engine's models
static void R_StudioMeElights( mebones_job_t *job )
{
	job->numelights = 0;

	if( !r_dynamic->value )
		return;

	R_StudioCollectElights();

	for( int i = 0; i < r_live_elights.count && job->numelights < MELIGHT_MAXELIGHTS; i++ )
	{
		const dlight_t *el = &tr.elights[r_live_elights.index[i]];
		meelight_t *l = &job->elights[job->numelights++];

		VectorCopy( el->origin, l->origin );
		l->r2 = el->radius * el->radius;
		l->key = el->key;
		l->color[0] = LinearGammaTable( el->color.r << 2 );
		l->color[1] = LinearGammaTable( el->color.g << 2 );
		l->color[2] = LinearGammaTable( el->color.b << 2 );
	}
}

// an entity's own ones move with it
static void R_StudioMoveElights( const cl_entity_t *e )
{
	for( int i = 0; i < r_live_elights.count; i++ )
	{
		dlight_t *el = &tr.elights[r_live_elights.index[i]];
		const int att = ( el->key >> 12 ) & 0xF;

		if(( el->key & 0xFFF ) != e->index )
			continue;

		if( !att ) VectorCopy( e->origin, el->origin );
		else if( att < MEBONES_ATTACHMENTS ) VectorCopy( e->attachment[att], el->origin );
	}
}
#endif

static void R_StudioEntityLight( alight_t *lightinfo )
{
	float		lstrength[MAX_LOCALLIGHTS];
	cl_entity_t	*ent = RI.currententity;

	g_studio.numlocallights = 0;

	if( !ent || !r_dynamic->value )
		return;

	for( int i = 0; i < MAX_LOCALLIGHTS; i++ )
		lstrength[i] = 0;

	vec3_t origin;
	Matrix3x4_OriginFromMatrix( g_studio.rotationmatrix, origin );
	float dist2 = 1000000.0f;
	int k = 0;

	R_StudioCollectElights();

	for( int i = 0; i < r_live_elights.count; i++ )
	{
		dlight_t *el = &tr.elights[r_live_elights.index[i]];

		if( el->die < r_live_elights.ftime || el->radius <= 0.0f )
			continue;

		if(( el->key & 0xFFF ) == ent->index )
		{
			int	att = (el->key >> 12) & 0xF;

			if( att ) VectorCopy( ent->attachment[att], el->origin );
			else VectorCopy( ent->origin, el->origin );
		}

		vec3_t mid;
		VectorSubtract( origin, el->origin, mid );

		float f = DotProduct( mid, mid );
		float r2 = el->radius * el->radius;

		float minstrength;
		if( f > r2 ) minstrength = r2 / f;
		else minstrength = 1.0f;

		if( minstrength > 0.05f )
		{
			if( g_studio.numlocallights >= MAX_LOCALLIGHTS )
			{
				k = -1;
				for( int j = 0; j < g_studio.numlocallights; j++ )
				{
					if( lstrength[j] < dist2 && lstrength[j] < minstrength )
					{
						dist2 = lstrength[j];
						k = j;
					}
				}
			}
			else k = g_studio.numlocallights;

			if( k != -1 )
			{
				g_studio.locallightcolor[k][0] = LinearGammaTable( el->color.r << 2 );
				g_studio.locallightcolor[k][1] = LinearGammaTable( el->color.g << 2 );
				g_studio.locallightcolor[k][2] = LinearGammaTable( el->color.b << 2 );
				g_studio.locallightR2[k] = r2;
				g_studio.locallight[k] = el;
				lstrength[k] = minstrength;

				if( k >= g_studio.numlocallights )
					g_studio.numlocallights = k + 1;
			}
		}
	}
}

/*
===============
R_StudioSetupLighting

===============
*/
static void R_StudioSetupLighting( alight_t *plight )
{
	float	scale = 1.0f;

	if( !m_pStudioHeader || !plight )
		return;

	if( RI.currententity != NULL )
		scale = RI.currententity->curstate.scale;

	g_studio.ambientlight = plight->ambientlight;
	g_studio.shadelight = plight->shadelight;
	VectorCopy( plight->plightvec, g_studio.lightvec );

	for( int i = 0; i < m_pStudioHeader->numbones; i++ )
	{
		Matrix3x4_VectorIRotate( g_studio.lighttransform[i], plight->plightvec, g_studio.blightvec[i] );
		if( scale > 1.0f ) VectorNormalize( g_studio.blightvec[i] ); // in case model may be scaled
	}

	VectorCopy( plight->color, g_studio.lightcolor );
}

/*
===============
R_StudioLighting

===============
*/
static void R_StudioLighting( float *lv, int bone, int flags, vec3_t normal )
{
	if( FBitSet( flags, STUDIO_NF_FULLBRIGHT ))
	{
		*lv = 1.0f;
		return;
	}

	float illum = g_studio.ambientlight;

	if( FBitSet( flags, STUDIO_NF_FLATSHADE ))
	{
		illum += g_studio.shadelight * 0.8f;
	}
	else
	{
		float lightcos;
		if( bone != -1 ) lightcos = DotProduct( normal, g_studio.blightvec[bone] );
		else lightcos = DotProduct( normal, g_studio.lightvec ); // -1 colinear, 1 opposite
		if( lightcos > 1.0f ) lightcos = 1.0f;

		illum += g_studio.shadelight;

		float r = SHADE_LAMBERT;

 		// do modified hemispherical lighting
		if( r <= 1.0f )
		{
			r += 1.0f;
			lightcos = (( r - 1.0f ) - lightcos) / r;
			if( lightcos > 0.0f )
				illum += g_studio.shadelight * lightcos;
		}
		else
		{
			lightcos = (lightcos + ( r - 1.0f )) / r;
			if( lightcos > 0.0f )
				illum -= g_studio.shadelight * lightcos;
		}

		illum = Q_max( illum, 0.0f );
	}

	illum = Q_min( illum, 255.0f );

	*lv = LightToTexGamma( illum * 4 ) / 1023.0f;
}

/*
====================
R_LightLambert

====================
*/
static void R_LightLambert( vec4_t light[MAX_LOCALLIGHTS], const vec3_t normal, const vec3_t color, byte *out )
{
	if( !g_studio.numlocallights )
	{
		VectorScale( color, 255.0f, out );
		return;
	}

	vec3_t finalLight = { 0, 0, 0 };

	for( int i = 0; i < g_studio.numlocallights; i++ )
	{
		float r = DotProduct( normal, light[i] );
		if( likely( !tr.fFlipViewModel ))
			r = -r;

		if( r > 0.0f )
		{
			if( light[i][3] == 0.0f )
			{
				float r2 = DotProduct( light[i], light[i] );

				if( r2 > 0.0f )
					light[i][3] = g_studio.locallightR2[i] / ( r2 * sqrt( r2 ));
				else light[i][3] = 0.0001f;
			}

			float temp = r * light[i][3];

			vec3_t localLight;
			VectorAddScalar( g_studio.locallightcolor[i], temp, localLight );
			VectorAdd( finalLight, localLight, finalLight );
		}
	}

	if( !VectorIsNull( finalLight ))
	{
		for( int i = 0; i < 3; i++ )
		{
			float c = finalLight[i] + LinearGammaTable( color[i] * 1023.0f );

			if( c > 1023.0f )
				out[i] = 255;
			else
				out[i] = ScreenGammaTable( c ) >> 2;
		}
	}
	else
	{
		VectorScale( color, 255.0f, out );
	}
}

// what R_LightLambert gives without local lights, so strip vertices sharing
// a normal don't each convert it again
static inline void R_StudioPackLightColor( int k )
{
	const float *lv = g_studio.lightvalues[k];
	byte c[3];

	VectorScale( lv, 255.0f, c );
	g_studio.lightcolors[k] = GUCOLOR4UB( c[0], c[1], c[2], 0 );
}

/*
===============
R_StudioLightMeshFast

R_StudioLighting + R_StudioPackLightColor for the common case (no local
lights, no bone weights, normal shading): goes straight to the GU colors
===============
*/
static void R_StudioLightMeshFast( uint *out, int count, const vec3_t *norms, const byte *bones )
{
	const float base = g_studio.ambientlight + g_studio.shadelight;
	const float k = g_studio.shadelight * ( 1.0f / SHADE_LAMBERT );
	const float off = SHADE_LAMBERT - 1.0f;
	const uint16_t *gamma = FBitSet( gp_host->features, ENGINE_LINEAR_GAMMA_SPACE ) ? NULL : tr.lightgammatable;
	const float cr = g_studio.lightcolor[0] * ( 255.0f / 1023.0f );
	const float cg = g_studio.lightcolor[1] * ( 255.0f / 1023.0f );
	const float cb = g_studio.lightcolor[2] * ( 255.0f / 1023.0f );

	for( int i = 0; i < count; i++ )
	{
		float d = DotProduct( norms[i], g_studio.blightvec[bones[i]] );
		float illum = base;

		if( d > 1.0f )
			d = 1.0f;
		d += off;
		if( d > 0.0f )
			illum -= k * d;
		if( illum < 0.0f )
			illum = 0.0f;
		else if( illum > 255.0f )
			illum = 255.0f;

		const int b = (int)( illum * 4.0f );
		const float g = gamma ? gamma[b] : b;

		out[i] = GUCOLOR4UB( (byte)( cr * g ), (byte)( cg * g ), (byte)( cb * g ), 0 );
	}
}

static void R_StudioSetColorArray( short *ptricmds, vec3_t *pstudionorms, byte *color )
{
	float	*lv = (float *)g_studio.lightvalues[ptricmds[1]];

	color[3] = tr.blend * 255;
	R_LightLambert( g_studio.lightpos[ptricmds[0]], pstudionorms[ptricmds[1]], lv, color );
}


/*
====================
R_LightStrength

====================
*/
static void R_LightStrength( int bone, vec3_t localpos, vec4_t light[MAX_LOCALLIGHTS] )
{
	if( g_studio.lightage[bone] != g_studio.framecount )
	{
		for( int i = 0; i < g_studio.numlocallights; i++ )
		{
			dlight_t *el = g_studio.locallight[i];
			Matrix3x4_VectorITransform( g_studio.lighttransform[bone], el->origin, g_studio.lightbonepos[bone][i] );
		}

		g_studio.lightage[bone] = g_studio.framecount;
	}

	for( int i = 0; i < g_studio.numlocallights; i++ )
	{
		VectorSubtract( localpos, g_studio.lightbonepos[bone][i], light[i] );
		light[i][3] = 0.0f;
	}
}

/*
===============
R_StudioSetupSkin

===============
*/
static void R_StudioSetupSkin( studiohdr_t *ptexturehdr, int index )
{
	mstudiotexture_t	*ptexture = NULL;

	if( FBitSet( g_nForceFaceFlags, STUDIO_NF_CHROME ))
		return;

	if( ptexturehdr == NULL )
		return;

	// NOTE: user may ignore to call StudioRemapColors and remap_info will be unavailable
	if( m_fDoRemap ) ptexture = gEngfuncs.CL_GetRemapInfoForEntity( RI.currententity )->ptexture;
	if( !ptexture ) ptexture = (mstudiotexture_t *)((byte *)ptexturehdr + ptexturehdr->textureindex); // fallback

	if( r_lightmap->value && !r_fullbright->value )
		GL_Bind( XASH_TEXTURE0, tr.whiteTexture );
	else GL_Bind( XASH_TEXTURE0, ptexture[index].index );
}

/*
===============
R_StudioGetTexture

Doesn't changes studio global state at all
===============
*/
mstudiotexture_t *R_StudioGetTexture( cl_entity_t *e )
{
	studiohdr_t *phdr;

	if(( phdr = gEngfuncs.Mod_Extradata( mod_studio, e->model )) == NULL )
		return NULL;

	studiohdr_t *thdr = m_pStudioHeader;
	if( !thdr ) return NULL;

	mstudiotexture_t *ptexture;
	if( m_fDoRemap ) ptexture = gEngfuncs.CL_GetRemapInfoForEntity( e )->ptexture;
	else ptexture = (mstudiotexture_t *)((byte *)thdr + thdr->textureindex);

	return ptexture;
}

static void R_StudioSetRenderamt( int iRenderamt )
{
	if( !RI.currententity ) return;

	RI.currententity->curstate.renderamt = iRenderamt;
	tr.blend = CL_FxBlend( RI.currententity ) / 255.0f;
}

/*
===============
R_StudioSetCullState

sets true for enable backculling (for left-hand viewmodel)
===============
*/
static void R_StudioSetCullState( int iCull )
{
	// This function intentionally does nothing
}

/*
===============
R_StudioRenderShadow

just a prefab for render shadow
===============
*/
static void R_StudioRenderShadow( int iSprite, float *p1, float *p2, float *p3, float *p4 )
{
	if( !p1 || !p2 || !p3 || !p4 )
		return;

	if( TriSpriteTexture( CL_ModelHandle( iSprite ), 0 ))
	{
		TriRenderMode( kRenderTransAlpha );
		TriColor4f( 0.0f, 0.0f, 0.0f, 1.0f );

		TriBegin( TRI_QUADS );
			TriTexCoord2f( 0.0f, 0.0f );
			TriVertex3fv( p1 );
			TriTexCoord2f( 0.0f, 1.0f );
			TriVertex3fv( p2 );
			TriTexCoord2f( 1.0f, 1.0f );
			TriVertex3fv( p3 );
			TriTexCoord2f( 1.0f, 0.0f );
			TriVertex3fv( p4 );
		TriEnd();

		TriRenderMode( kRenderNormal );
	}
}

/*
===============
R_StudioMeshCompare

Sorting opaque entities by model type
===============
*/
static int R_StudioMeshCompare( const void *a, const void *b )
{
	if( FBitSet( ((const sortedmesh_t*)a)->flags, STUDIO_NF_ADDITIVE ))
		return 1;

	if( FBitSet( ((const sortedmesh_t*)a)->flags, STUDIO_NF_MASKED ))
		return -1;

	return 0;
}

/*
===============
R_StudioCountMeshVerts

the mesh's strips and fans share one block of display list memory: in a
GU_DIRECT list every sceGuGetMemory and sceGuDrawArray makes a syscall
(sceGeListUpdateStallAddr)
===============
*/
static int R_StudioCountMeshVerts( const short *ptricmds )
{
	int i, total = 0;

	while(( i = *( ptricmds++ )))
	{
		if( i < 0 )
			i = -i;
		total += i;
		ptricmds += i * 4;
	}

	return total;
}

/*
===============
R_StudioDrawNormalMesh

generic path
===============
*/
static void R_StudioDrawNormalMesh( short *ptricmds, vec3_t *pstudionorms, float s, float t )
{
	int	i;
	const qboolean fastlight = !g_studio.numlocallights;
	const uint alpha = GUCOLOR4UB( 0, 0, 0, (byte)( tr.blend * 255 ));

	gu_vert_ftcv_t *out = GU_GetVertexMemory( sizeof( gu_vert_ftcv_t ) * R_StudioCountMeshVerts( ptricmds ));

	while(( i = *( ptricmds++ )))
	{
		int prim = GU_TRIANGLE_STRIP;

		if( i < 0 )
		{
			prim = GU_TRIANGLE_FAN;
			i = -i;
		}

		for( int index = 0; index < i; index++, ptricmds += 4 )
		{
			if( fastlight )
				out[index].c = g_studio.lightcolors[ptricmds[1]] | alpha;
			else
			{
				rgba_t color;
				R_StudioSetColorArray( ptricmds, pstudionorms, color );
				out[index].c = GUCOLOR4UBV( color );
			}
			out[index].u = ptricmds[2] * s;
			out[index].v = ptricmds[3] * t;
			out[index].x = g_studio.verts[ptricmds[0]][0];
			out[index].y = g_studio.verts[ptricmds[0]][1];
			out[index].z = g_studio.verts[ptricmds[0]][2];
		}

		extGuDrawArrayNoStall( prim, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF, i, out );
		out += i;
	}

	extGuUpdateStall();
}

/*
===============
R_StudioDrawNormalMesh

generic path
===============
*/
static void R_StudioDrawFloatMesh( short *ptricmds, vec3_t *pstudionorms )
{
	int	i;
	const qboolean fastlight = !g_studio.numlocallights;
	const uint alpha = GUCOLOR4UB( 0, 0, 0, (byte)( tr.blend * 255 ));

	gu_vert_ftcv_t *out = GU_GetVertexMemory( sizeof( gu_vert_ftcv_t ) * R_StudioCountMeshVerts( ptricmds ));

	while(( i = *( ptricmds++ )))
	{
		int prim = GU_TRIANGLE_STRIP;

		if( i < 0 )
		{
			prim = GU_TRIANGLE_FAN;
			i = -i;
		}

		for( int index = 0; index < i; index++, ptricmds += 4 )
		{
			if( fastlight )
				out[index].c = g_studio.lightcolors[ptricmds[1]] | alpha;
			else
			{
				rgba_t color;
				R_StudioSetColorArray( ptricmds, pstudionorms, color );
				out[index].c = GUCOLOR4UBV( color );
			}
			out[index].u = HalfToFloat( ptricmds[2] );
			out[index].v = HalfToFloat( ptricmds[3] );
			out[index].x = g_studio.verts[ptricmds[0]][0];
			out[index].y = g_studio.verts[ptricmds[0]][1];
			out[index].z = g_studio.verts[ptricmds[0]][2];
		}

		extGuDrawArrayNoStall( prim, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF, i, out );
		out += i;
	}

	extGuUpdateStall();
}

/*
===============
R_StudioDrawNormalMesh

generic path
===============
*/
static void R_StudioDrawChromeMesh( short *ptricmds, vec3_t *pstudionorms, float s, float t, float scale )
{
	int	i;
	qboolean	glowShell = (scale > 0.0f) ? true : false;

	gu_vert_ftcv_t *out = GU_GetVertexMemory( sizeof( gu_vert_ftcv_t ) * R_StudioCountMeshVerts( ptricmds ));

	while(( i = *( ptricmds++ )))
	{
		int prim = GU_TRIANGLE_STRIP;

		if( i < 0 )
		{
			prim = GU_TRIANGLE_FAN;
			i = -i;
		}

		for( int index = 0; index < i; index++, ptricmds += 4 )
		{
			int idx;

			if( glowShell )
			{
				color24 *clr = &RI.currententity->curstate.rendercolor;

				idx = g_studio.normaltable[ptricmds[0]];
				float *av = g_studio.verts[ptricmds[0]];
				float *lv = g_studio.norms[ptricmds[0]];
				vec3_t vert;
				VectorMA( av, scale, lv, vert );
				out[index].c = GUCOLOR4UB( clr->r, clr->g, clr->b, 255 );
				out[index].x = vert[0];
				out[index].y = vert[1];
				out[index].z = vert[2];
			}
			else
			{
				rgba_t color;

				idx = ptricmds[1];
				R_StudioSetColorArray( ptricmds, pstudionorms, color );
				out[index].c = GUCOLOR4UBV( color );
				out[index].x = g_studio.verts[ptricmds[0]][0];
				out[index].y = g_studio.verts[ptricmds[0]][1];
				out[index].z = g_studio.verts[ptricmds[0]][2];
			}

			out[index].u = g_studio.chrome[idx][0] * s;
			out[index].v = g_studio.chrome[idx][1] * t;
		}

		extGuDrawArrayNoStall( prim, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF, i, out );
		out += i;
	}

	extGuUpdateStall();
}



/*
===============
R_StudioDrawNormalMesh

generic path
===============
*/

/*
===============
R_StudioDrawNormalMesh

generic path
===============
*/

/*
===============
R_StudioDrawNormalMesh

generic path
===============
*/


/*
=============================================================================

STUDIO TEXTURE ATLAS

The GE path draws each mesh with its own texture: a skin block, a light block
and the bone uploads per mesh, ~13 us of CPU each and ~170 meshes a frame in a
firefight (a grunt has ~10). The plain textures of a model (not masked,
additive, chrome, flat shaded, fullbright or remapped) also go into 8-bit
atlas pages with one palette per page, and the GE meshes of a submodel that
use them are drawn as one (R_StudioGEGetMerge). Each texture keeps its own
copy, for the CPU path and for meshes whose texture coordinates leave the
texture. Fidelity: a page's palette is a median cut of its textures' colors.

=============================================================================
*/
static CVAR_DEFINE_AUTO( r_studio_atlas, "1", 0, "draw the GE meshes of a submodel with plain textures as one, from texture atlases (built for models loaded while it's on)" );

#define ATLAS_MAX_SIZE	512	// the GE's largest texture
#define ATLAS_CELL		16	// TEXTURE_SIZE_MIN: textures are multiples of it
#define ATLAS_GRID		( ATLAS_MAX_SIZE / ATLAS_CELL )
#define ATLAS_MAX_PAGES	4
#define ATLAS_FLAGS		( STUDIO_NF_FLATSHADE|STUDIO_NF_CHROME|STUDIO_NF_FULLBRIGHT|STUDIO_NF_NOMIPS|STUDIO_NF_ADDITIVE|STUDIO_NF_MASKED|STUDIO_NF_NORMALMAP|STUDIO_NF_COLORMAP|STUDIO_NF_UV_COORDS )

// where a texture is in its atlas page: u = ou + s * su for the raw texel coordinates
typedef struct
{
	int		atlas;	// texture number of the page, 0: not in one
	float		ou, ov, su, sv;
} studioatlasslot_t;

static studioatlasslot_t *studio_atlas_slots; // MAX_TEXTURES, by texture number

typedef struct
{
	int		tex;	// index in the model's textures
	int		w, h;	// size as uploaded (powers of two)
	int		x, y;
} atlasitem_t;

typedef struct
{
	byte		rgb[3];
	uint		weight;	// pixels
	ushort		src;	// item * 256 + palette index
} atlascolor_t;

static int atlas_sort_axis;

static int R_StudioAtlasColorCmp( const void *a, const void *b )
{
	return (int)((const atlascolor_t *)a )->rgb[atlas_sort_axis] - (int)((const atlascolor_t *)b )->rgb[atlas_sort_axis];
}

static int R_StudioAtlasItemCmp( const void *a, const void *b )
{
	const atlasitem_t *x = *(const atlasitem_t * const *)a, *y = *(const atlasitem_t * const *)b;

	if( x->h != y->h )
		return y->h - x->h;
	return y->w - x->w;
}

// bottom-left skyline on the 16 pixel grid: the height used, -1 if an item
// doesn't fit (placed: skip those instead and mark the others)
static int R_StudioAtlasSkyline( atlasitem_t **items, int n, int w, int h, qboolean *placed )
{
	const int gw = w / ATLAS_CELL, gh = h / ATLAS_CELL;
	int sky[ATLAS_GRID] = { 0 }, used = 0;

	for( int i = 0; i < n; i++ )
	{
		const int cw = items[i]->w / ATLAS_CELL, ch = items[i]->h / ATLAS_CELL;
		int bestx = -1, besty = gh;

		for( int x = 0; x + cw <= gw; x++ )
		{
			int y = 0;
			for( int k = 0; k < cw; k++ )
				y = Q_max( y, sky[x + k] );
			if( y + ch <= gh && y < besty )
			{
				besty = y;
				bestx = x;
			}
		}

		if( placed )
			placed[i] = bestx >= 0;
		if( bestx < 0 )
		{
			if( !placed )
				return -1;
			continue;
		}

		for( int k = 0; k < cw; k++ )
			sky[bestx + k] = besty + ch;
		items[i]->x = bestx * ATLAS_CELL;
		items[i]->y = besty * ATLAS_CELL;
		used = Q_max( used, ( besty + ch ) * ATLAS_CELL );
	}
	return used;
}

// the smallest page that holds all the items (squarer first), placed
static qboolean R_StudioAtlasFit( atlasitem_t **items, int n, int *outw, int *outh, int *outrows )
{
	int maxw = 0, bestw = 0, besth = 0, bestrows = 0;

	for( int i = 0; i < n; i++ )
		maxw = Q_max( maxw, items[i]->w );

	for( int w = Q_max( maxw, ATLAS_CELL ); w <= ATLAS_MAX_SIZE; w <<= 1 )
	{
		const int used = R_StudioAtlasSkyline( items, n, w, ATLAS_MAX_SIZE, NULL );
		int h = ATLAS_CELL;

		if( used < 0 )
			continue;
		while( h < used )
			h <<= 1;
		// the rows past the used ones aren't stored (GL_CreateAtlasPage)
		if( !bestw || w * used < bestw * bestrows || ( w * used == bestw * bestrows && abs( w - h ) < abs( bestw - besth )))
		{
			bestw = w;
			besth = h;
			bestrows = used;
		}
	}

	if( !bestw )
		return false;
	R_StudioAtlasSkyline( items, n, bestw, besth, NULL );
	*outw = bestw;
	*outh = besth;
	*outrows = bestrows;
	return true;
}

// 256 colors for the page: median cut, weighted by pixels; lut[src] = palette index
static void R_StudioAtlasPalette( atlascolor_t *c, int n, byte *pal, byte *lut )
{
	struct { int start, end, range, axis; } box[256];
	int numboxes = 0;

	memset( pal, 0, 768 );

	for( int b = 0; b < 256; b++ )
	{
		int changed = 0;

		if( b > 0 )
		{
			// split the box with the widest range at its weighted median
			int best = -1;
			for( int i = 0; i < numboxes; i++ )
				if( box[i].end - box[i].start > 1 && box[i].range > 0 && ( best < 0 || box[i].range > box[best].range ))
					best = i;
			if( best < 0 )
				break;

			atlas_sort_axis = box[best].axis;
			qsort( c + box[best].start, box[best].end - box[best].start, sizeof( *c ), R_StudioAtlasColorCmp );

			uint total = 0, sum = 0;
			for( int i = box[best].start; i < box[best].end; i++ )
				total += c[i].weight;
			int m = box[best].start + 1;
			for( int i = box[best].start; i < box[best].end - 1; i++ )
			{
				sum += c[i].weight;
				m = i + 1;
				if( sum * 2 >= total )
					break;
			}
			box[numboxes].start = m;
			box[numboxes].end = box[best].end;
			box[best].end = m;
			changed = best;
			numboxes++;
		}
		else
		{
			box[0].start = 0;
			box[0].end = n;
			numboxes = 1;
		}

		// ranges of the split box and the new one
		for( int k = 0; k < 2; k++ )
		{
			const int i = k ? numboxes - 1 : changed;
			byte lo[3] = { 255, 255, 255 }, hi[3] = { 0, 0, 0 };
			for( int j = box[i].start; j < box[i].end; j++ )
			{
				for( int a = 0; a < 3; a++ )
				{
					lo[a] = Q_min( lo[a], c[j].rgb[a] );
					hi[a] = Q_max( hi[a], c[j].rgb[a] );
				}
			}
			box[i].axis = 0;
			for( int a = 1; a < 3; a++ )
				if( hi[a] - lo[a] > hi[box[i].axis] - lo[box[i].axis] ) box[i].axis = a;
			box[i].range = hi[box[i].axis] - lo[box[i].axis];
		}
	}

	for( int i = 0; i < numboxes; i++ )
	{
		uint sum[3] = { 0, 0, 0 }, total = 0;

		for( int j = box[i].start; j < box[i].end; j++ )
		{
			for( int a = 0; a < 3; a++ )
				sum[a] += c[j].rgb[a] * c[j].weight;
			total += c[j].weight;
		}
		for( int a = 0; a < 3; a++ )
			pal[i * 3 + a] = total ? ( sum[a] + total / 2 ) / total : 0;
		for( int j = box[i].start; j < box[i].end; j++ )
			lut[c[j].src] = i;
	}
}

static int R_StudioAtlasBuildPage( const char *name, const byte *base, const mstudiotexture_t *ptexture, const int *pixofs, atlasitem_t **items, int n, int w, int h, int rows )
{
	byte *pixels = calloc( 1, w * rows );
	uint *count = calloc( n * 256, sizeof( *count ));
	atlascolor_t *colors = malloc( n * 256 * sizeof( *colors ));
	byte *lut = calloc( n, 256 );
	byte pal[768];
	int texnum = 0;

	if( !pixels || !count || !colors || !lut )
	{
		free( pixels );
		free( count );
		free( colors );
		free( lut );
		return 0;
	}

	// the same resampling as GL_ResampleTexture8 for the texture by itself
	for( int k = 0; k < n; k++ )
	{
		const atlasitem_t *it = items[k];
		const mstudiotexture_t *ptex = &ptexture[it->tex];
		const byte *src = base + pixofs[it->tex];
		const uint fracstep = ptex->width * 0x10000 / it->w;

		for( int y = 0; y < it->h; y++ )
		{
			const byte *inrow = src + ptex->width * ( y * ptex->height / it->h );
			byte *out = pixels + ( it->y + y ) * w + it->x;
			uint frac = fracstep >> 1;

			for( int x = 0; x < it->w; x++, frac += fracstep )
			{
				out[x] = inrow[frac >> 16];
				count[k * 256 + out[x]]++;
			}
		}
	}

	int numcolors = 0;
	for( int k = 0; k < n; k++ )
	{
		const mstudiotexture_t *ptex = &ptexture[items[k]->tex];
		const byte *srcpal = base + pixofs[items[k]->tex] + ptex->width * ptex->height;

		for( int i = 0; i < 256; i++ )
		{
			if( !count[k * 256 + i] )
				continue;
			atlascolor_t *c = &colors[numcolors++];
			memcpy( c->rgb, srcpal + i * 3, 3 );
			c->weight = count[k * 256 + i];
			c->src = k * 256 + i;
		}
	}
	R_StudioAtlasPalette( colors, numcolors, pal, lut );

	for( int k = 0; k < n; k++ )
	{
		const atlasitem_t *it = items[k];
		for( int y = 0; y < it->h; y++ )
		{
			byte *out = pixels + ( it->y + y ) * w + it->x;
			for( int x = 0; x < it->w; x++ )
				out[x] = lut[k * 256 + out[x]];
		}
	}

	texnum = GL_CreateAtlasPage( name, w, h, rows, pixels, pal );

	free( pixels );
	free( count );
	free( colors );
	free( lut );
	return texnum;
}

static void R_StudioFreeAtlas( int texnum )
{
	if( !studio_atlas_slots || texnum <= 0 || texnum >= MAX_TEXTURES )
		return;

	const int atlas = studio_atlas_slots[texnum].atlas;
	if( !atlas )
		return;

	for( int i = 0; i < MAX_TEXTURES; i++ )
		if( studio_atlas_slots[i].atlas == atlas ) studio_atlas_slots[i].atlas = 0;
	GL_FreeTexture( atlas );
}

/*
===============
R_StudioBuildAtlas

after the model's textures are loaded, while its pixels (at pixofs from
base) are still in memory
===============
*/
static void R_StudioBuildAtlas( model_t *mod, studiohdr_t *phdr, const int *pixofs )
{
	mstudiotexture_t *ptexture = (mstudiotexture_t *)((byte *)phdr + phdr->textureindex);
	atlasitem_t *items, **order;
	int n = 0, pages = 0, kb = 0, placed = 0, aliased = 0;

	if( !studio_atlas_slots && !( studio_atlas_slots = calloc( MAX_TEXTURES, sizeof( *studio_atlas_slots ))))
		return;

	// stale entries from textures freed in bulk (the numbers are reused)
	for( int i = 0; i < phdr->numtextures; i++ )
		if( ptexture[i].index > 0 && ptexture[i].index < MAX_TEXTURES ) studio_atlas_slots[ptexture[i].index].atlas = 0;

	if( !r_studio_atlas.value || phdr->numtextures < 2 )
		return;

	items = malloc( phdr->numtextures * sizeof( *items ));
	order = malloc( phdr->numtextures * sizeof( *order ));
	if( !items || !order )
	{
		free( items );
		free( order );
		return;
	}

	for( int i = 0; i < phdr->numtextures; i++ )
	{
		const mstudiotexture_t *ptex = &ptexture[i];
		const int texnum = ptex->index;
		int j;

		if( FBitSet( ptex->flags, ATLAS_FLAGS ) || texnum <= 0 || texnum >= MAX_TEXTURES || texnum == tr.defaultTexture || pixofs[i] <= 0 )
			continue;
		if( ptex->width <= 0 || ptex->height <= 0 )
			continue;

		const gl_texture_t *t = R_GetTexture( texnum );
		if( t->name[0] != '#' || t->format != GU_PSM_T8 || t->width % ATLAS_CELL || t->height % ATLAS_CELL )
			continue;

		for( j = 0; j < n; j++ )
			if( ptexture[items[j].tex].index == texnum ) break;
		if( j < n )
			continue; // the same texture twice

		items[n].tex = i;
		items[n].w = t->width;
		items[n].h = t->height;
		order[n] = &items[n];
		n++;
	}
	qsort( order, n, sizeof( *order ), R_StudioAtlasItemCmp );

	char mdlname[64];
	Q_strncpy( mdlname, mod->name, sizeof( mdlname ));
	COM_StripExtension( mdlname );

	while( n >= 2 && pages < ATLAS_MAX_PAGES )
	{
		int count = n, w = 0, h = 0, rows = 0;

		// what doesn't fit in the largest page waits for the next one
		if( !R_StudioAtlasFit( order, count, &w, &h, &rows ))
		{
			qboolean fits[MAXSTUDIOSKINS];
			atlasitem_t *rest[MAXSTUDIOSKINS];
			int nrest = 0;

			if( n > MAXSTUDIOSKINS )
				break;
			R_StudioAtlasSkyline( order, n, ATLAS_MAX_SIZE, ATLAS_MAX_SIZE, fits );
			count = 0;
			for( int i = 0; i < n; i++ )
			{
				if( fits[i] )
					order[count++] = order[i];
				else rest[nrest++] = order[i];
			}
			memcpy( order + count, rest, nrest * sizeof( *rest ));
			if( !R_StudioAtlasFit( order, count, &w, &h, &rows ))
				break;
		}
		if( count < 2 )
			break;

		char name[96];
		Q_snprintf( name, sizeof( name ), "#atlas/%s/%d", mdlname, pages );
		const int atlas = R_StudioAtlasBuildPage( name, (const byte *)phdr, ptexture, pixofs, order, count, w, h, rows );
		if( !atlas )
			break;

		for( int i = 0; i < count; i++ )
		{
			const atlasitem_t *it = order[i];
			const mstudiotexture_t *ptex = &ptexture[it->tex];
			studioatlasslot_t *s = &studio_atlas_slots[ptex->index];

			s->atlas = atlas;
			s->ou = (float)it->x / w;
			s->ov = (float)it->y / h;
			s->su = (float)it->w / ( (float)w * ptex->width );
			s->sv = (float)it->h / ( (float)h * ptex->height );

			// the texture by itself becomes that part of the page
			if( GL_AliasTexture( ptex->index, atlas, it->x, it->y ))
				aliased++;
		}

		pages++;
		placed += count;
		kb += R_GetTexture( atlas )->size / 1024;
		memmove( order, order + count, ( n - count ) * sizeof( *order ));
		n -= count;
	}

	if( pages )
		gEngfuncs.Con_Reportf( "%s: %d textures (%d aliased) in %d atlas pages, %d KB\n", mod->name, placed, aliased, pages, kb );

	free( items );
	free( order );
}

/*
=============================================================================

GE SKINNING

The CPU path transforms every vertex, lights every normal and writes ~24 bytes
per strip vertex into the display list each frame. Here each mesh is stored
once per model with its bone slot and normal, the GE skins it with up to 8
bone matrices per draw (CALLed sub-lists, only changed slots are uploaded),
and the lighting comes from a second pass: the GE generates a texture
coordinate from the skinned normal (u = 0.5 + 0.5 * dot( normal, lightvec ))
into a small ramp texture that holds what R_StudioLighting gives for that dot
product (wrap lambert, light gamma, light color), multiplied into the frame
buffer where the depth matches. Same result as the CPU path up to the ramp
resolution and one more 565 rounding.

Chrome meshes get their texture coordinates from the skinned normals too (a
texture matrix per draw), flat shaded ones a light pass at one fixed point of
the ramp.

Only for the common case: normal render mode, no local lights, no fog, no
glow shell, no bone weights, and meshes without fullbright/additive/float UVs;
everything else takes the CPU path.

=============================================================================
*/
CVAR_DEFINE_AUTO( r_studio_ge, "1", 0, "draw studio models with GE skinning and lighting where possible" );

#define GE_MAXBONES		4
#define GE_RAMP_SIZE	32
#define GE_CACHE_SIZE	512	// submodels

#define GE_CMD( cmd, arg )	((( cmd ) << 24 ) | (( arg ) & 0xffffff ))
#define GE_UNSUPPORTED_FLAGS	( STUDIO_NF_FULLBRIGHT|STUDIO_NF_ADDITIVE|STUDIO_NF_UV_COORDS )

typedef struct
{
	uint		*cmds;		// VTYPE BASE VADDR PRIM... RET, called from the display list
	byte		numbones;
	byte		bones[GE_MAXBONES];
} gebatch_t;

typedef struct
{
	qboolean		ge;	// the GE can draw it, the batches are built on first use (R_StudioGEMeshBatches)
	int		numbatches;
	gebatch_t		*batches;
} gemesh_t;

// the meshes of a submodel that use one atlas page, drawn as one
typedef struct
{
	int		atlas;	// texture number of the page
	int		numbatches;
	gebatch_t		*batches;
} gemergegroup_t;

typedef struct gemerge_s
{
	int		skin;	// skin family
	int		numgroups;
	gemergegroup_t	groups[ATLAS_MAX_PAGES];
	byte		*merged;	// per mesh: drawn by the groups
	struct gemerge_s	*next;
} gemerge_t;

typedef struct
{
	const studiohdr_t	*hdr;
	const mstudiomodel_t *submodel;
	qboolean		cpumeshes;	// some meshes still need the CPU path
	int		nummesh;
	gemesh_t		*meshes;
	gemerge_t		*merges;	// R_StudioGEGetMerge, by skin family
	struct geplan_s	*plans;		// R_StudioGEDrawPlan, by skin family
	void		**blocks;		// batch memory, freed with the model
	int		numblocks;
} gemodel_t;

static gemodel_t	*ge_cache[GE_CACHE_SIZE];
static size_t	ge_cache_bytes;
static uint	ge_rec_gen = 1;	// recorded prop commands (R_StudioPropRecord) from another one are stale

static inline uint GE_Float( float f )
{
	union { float f; uint u; } x = { f };
	return x.u >> 8;
}

static uint R_StudioGEHash( const void *p )
{
	return (((uint)p ) >> 4 ) % GE_CACHE_SIZE;
}

static void R_StudioGEFreeModel( gemodel_t *gm )
{
	for( int i = 0; i < gm->numblocks; i++ )
		free( gm->blocks[i] );
	for( int i = 0; i < gm->nummesh; i++ )
		free( gm->meshes[i].batches );
	while( gm->merges )
	{
		gemerge_t *m = gm->merges;
		gm->merges = m->next;
		for( int i = 0; i < m->numgroups; i++ )
			free( m->groups[i].batches );
		free( m->merged );
		free( m );
	}
	while( gm->plans )
	{
		struct geplan_s *pl = gm->plans;
		gm->plans = *(struct geplan_s **)pl; // next is the first field
		free( pl );
	}
	free( gm->blocks );
	free( gm->meshes );
	free( gm );
}

// Mod_StudioUnloadTextures: the header memory can be reused by another model
static void R_StudioGEUnload( const studiohdr_t *hdr )
{
	ge_rec_gen++; // the batches go
	PSP_MeWaitFor( r_mebones.early_ticket );
	PSP_MeWaitFor( r_mebones.ticket );
	for( int i = 0; i < GE_CACHE_SIZE; i++ )
	{
		if( ge_cache[i] && ge_cache[i]->hdr == hdr )
		{
			R_StudioGEFreeModel( ge_cache[i] );
			ge_cache[i] = NULL;
		}
	}

	// reinsert what came after a freed slot, open addressing
	for( int i = 0; i < GE_CACHE_SIZE; i++ )
	{
		gemodel_t *gm = ge_cache[i];
		if( !gm ) continue;
		ge_cache[i] = NULL;
		uint h = R_StudioGEHash( gm->submodel );
		while( ge_cache[h] ) h = ( h + 1 ) % GE_CACHE_SIZE;
		ge_cache[h] = gm;
	}
}

typedef struct
{
	int		prim;		// GU_TRIANGLE_STRIP, GU_TRIANGLE_FAN or GU_TRIANGLES
	int		count;
	const short	*verts[3];	// GU_TRIANGLES: one triangle, else NULL
	const short	*first;	// strips and fans: first tricmd record
	const studioatlasslot_t *uv;	// atlas page coordinates, NULL: the raw texel shorts
} geunit_t;

static int R_StudioGEUnitBones( const geunit_t *u, const byte *pvertbone, byte *bones )
{
	int n = 0;

	for( int i = 0; i < u->count; i++ )
	{
		const short *t = u->first ? u->first + i * 4 : u->verts[i];
		byte b = pvertbone[t[0]];
		int j;

		for( j = 0; j < n; j++ )
			if( bones[j] == b ) break;
		if( j == n )
		{
			if( n >= MAXSTUDIOBONES ) break;
			bones[n++] = b;
		}
	}
	return n;
}

/*
===============
R_StudioGEBuildBatch

vertices + call list for the units [first, last)
===============
*/
static qboolean R_StudioGEBuildBatch( gemodel_t *gm, gebatch_t *batch, const geunit_t *units, int first, int last, const byte *pvertbone, const vec3_t *pstudioverts, const vec3_t *pstudionorms )
{
	const int nb = batch->numbones;
	const int off_t = ( nb + 1 ) & ~1;
	const int off_n = off_t + 4;
	const int off_p = ( off_n + 3 + 3 ) & ~3;
	const int vsize = off_p + 12;
	int numverts = 0, numprims = 0;

	for( int i = first; i < last; i++ )
	{
		// consecutive triangles go into one GU_TRIANGLES draw
		if( units[i].prim != GU_TRIANGLES || i == first || units[i - 1].prim != GU_TRIANGLES )
			numprims++;
		numverts += units[i].count;
	}

	const size_t vbytes = ( numverts * vsize + 15 ) & ~15;
	const size_t size = vbytes + ( 3 + numprims + 1 ) * 4;
	byte *mem = memalign( 16, size );

	if( !mem )
		return false;

	void **blocks = realloc( gm->blocks, sizeof( *blocks ) * ( gm->numblocks + 1 ));
	if( !blocks )
	{
		free( mem );
		return false;
	}
	gm->blocks = blocks;
	gm->blocks[gm->numblocks++] = mem;
	ge_cache_bytes += size;

	byte *v = mem;
	uint *cmd = (uint *)( mem + vbytes );
	const uint vtype = GU_WEIGHT_8BIT | GU_WEIGHTS( nb ) | GU_TEXTURE_16BIT | GU_NORMAL_8BIT | GU_VERTEX_32BITF | GU_TRANSFORM_3D;

	batch->cmds = cmd;
	*cmd++ = GE_CMD( 0x12, vtype );	// VTYPE
	*cmd++ = GE_CMD( 0x10, ((uint)mem >> 8 ) & 0xf0000 );	// BASE
	*cmd++ = GE_CMD( 0x01, (uint)mem );	// VADDR

	uint *prim = NULL;
	for( int i = first; i < last; i++ )
	{
		const geunit_t *u = &units[i];

		if( u->prim != GU_TRIANGLES || i == first || units[i - 1].prim != GU_TRIANGLES )
		{
			prim = cmd++;
			*prim = GE_CMD( 0x04, ( u->prim << 16 ));
		}
		*prim += u->count;

		for( int k = 0; k < u->count; k++, v += vsize )
		{
			const short *t = u->first ? u->first + k * 4 : u->verts[k];
			const byte bone = pvertbone[t[0]];
			int slot;

			for( slot = 0; slot < nb; slot++ )
				if( batch->bones[slot] == bone ) break;

			memset( v, 0, vsize );
			v[slot] = 128; // 1.0
			if( u->uv )
			{
				// normalized in the page, 1.0 = 32768 (the skin block's texture scale is 1)
				((short *)( v + off_t ))[0] = (short)bound( 0, (int)roundf(( u->uv->ou + t[2] * u->uv->su ) * 32768.0f ), 32767 );
				((short *)( v + off_t ))[1] = (short)bound( 0, (int)roundf(( u->uv->ov + t[3] * u->uv->sv ) * 32768.0f ), 32767 );
			}
			else
			{
				((short *)( v + off_t ))[0] = t[2];
				((short *)( v + off_t ))[1] = t[3];
			}
			for( int c = 0; c < 3; c++ )
				((signed char *)( v + off_n ))[c] = (signed char)bound( -127, (int)roundf( pstudionorms[t[1]][c] * 127.0f ), 127 );
			memcpy( v + off_p, pstudioverts[t[0]], 12 );
		}
	}
	*cmd++ = GE_CMD( 0x0B, 0 ); // RET

	sceKernelDcacheWritebackRange( mem, size );
	return true;
}

/*
===============
R_StudioGEBuildMesh

split the strips into batches of at most 8 bones; strips touching more
bones are drawn as single triangles
===============
*/
// bones of the last batch built: the next one keeps shared bones in the same
// slots, so fewer matrices are uploaded between draws
static byte	ge_build_prev[GE_MAXBONES];
static int	ge_build_prevn;

static void R_StudioGEOrderSlots( gebatch_t *b )
{
	byte order[GE_MAXBONES];
	qboolean used[GE_MAXBONES] = { 0 }, placed[GE_MAXBONES] = { 0 };
	const int n = b->numbones;

	for( int s = 0; s < n && s < ge_build_prevn; s++ )
	{
		for( int k = 0; k < n; k++ )
		{
			if( !used[k] && b->bones[k] == ge_build_prev[s] )
			{
				order[s] = b->bones[k];
				used[k] = placed[s] = true;
				break;
			}
		}
	}

	for( int s = 0, k = 0; s < n; s++ )
	{
		if( placed[s] ) continue;
		while( used[k] ) k++;
		order[s] = b->bones[k];
		used[k] = true;
	}

	memcpy( b->bones, order, n );
	memcpy( ge_build_prev, order, n );
	ge_build_prevn = n;
}

// upper bound of the units of a mesh (one per triangle), -1: a normal on another bone than its vertex
static int R_StudioGECountUnits( const short *ptricmds, const byte *pvertbone, const byte *pnormbone )
{
	const short *p = ptricmds;
	int numunits = 0, i;

	// the GE moves a normal with its vertex's bone
	while(( i = *( p++ )))
	{
		if( i < 0 ) i = -i;
		for( int k = 0; k < i; k++, p += 4 )
			if( pvertbone[p[0]] != pnormbone[p[1]] )
				return -1;
		numunits += i;
	}
	return numunits;
}

// the strips and fans of a mesh, split into triangles where they touch more than 8 bones
static int R_StudioGEAddUnits( geunit_t *units, int nunits, const short *ptricmds, const byte *pvertbone, const studioatlasslot_t *uv )
{
	const short *p = ptricmds;
	byte bones[MAXSTUDIOBONES];
	int i;

	while(( i = *( p++ )))
	{
		geunit_t u = { 0 };

		u.prim = GU_TRIANGLE_STRIP;
		if( i < 0 )
		{
			u.prim = GU_TRIANGLE_FAN;
			i = -i;
		}
		u.count = i;
		u.first = p;
		u.uv = uv;

		if( R_StudioGEUnitBones( &u, pvertbone, bones ) <= GE_MAXBONES )
			units[nunits++] = u;
		else
		{
			for( int k = 0; k + 2 < i; k++ )
			{
				geunit_t *t = &units[nunits++];
				const short *a, *b, *c;

				if( u.prim == GU_TRIANGLE_FAN )
				{
					a = p; b = p + ( k + 1 ) * 4; c = p + ( k + 2 ) * 4;
				}
				else if( k & 1 )
				{
					a = p + ( k + 1 ) * 4; b = p + k * 4; c = p + ( k + 2 ) * 4;
				}
				else
				{
					a = p + k * 4; b = p + ( k + 1 ) * 4; c = p + ( k + 2 ) * 4;
				}

				memset( t, 0, sizeof( *t ));
				t->prim = GU_TRIANGLES;
				t->count = 3;
				t->verts[0] = a; t->verts[1] = b; t->verts[2] = c;
				t->uv = uv;
			}
		}
		p += i * 4;
	}
	return nunits;
}

// greedy batches of at most 8 bones
static qboolean R_StudioGEBatchUnits( gemodel_t *gm, const geunit_t *units, int nunits, const byte *pvertbone, const vec3_t *pstudioverts, const vec3_t *pstudionorms, gebatch_t **outbatches, int *outnum )
{
	gebatch_t *batches = NULL;
	int nbatches = 0;
	int first = 0;
	gebatch_t cur;
	memset( &cur, 0, sizeof( cur ));

	for( int u = 0; u <= nunits; u++ )
	{
		byte ub[MAXSTUDIOBONES];
		int nub = 0, merged = cur.numbones;
		byte mb[MAXSTUDIOBONES];

		if( u < nunits )
		{
			nub = R_StudioGEUnitBones( &units[u], pvertbone, ub );
			memcpy( mb, cur.bones, cur.numbones );
			for( int k = 0; k < nub; k++ )
			{
				int j;
				for( j = 0; j < merged; j++ )
					if( mb[j] == ub[k] ) break;
				if( j == merged )
				{
					if( merged >= GE_MAXBONES ) { merged = GE_MAXBONES + 1; break; }
					mb[merged++] = ub[k];
				}
			}
		}

		if( u == nunits || merged > GE_MAXBONES )
		{
			if( u > first )
			{
				R_StudioGEOrderSlots( &cur );
				gebatch_t *nb = realloc( batches, sizeof( *batches ) * ( nbatches + 1 ));
				if( !nb || !R_StudioGEBuildBatch( gm, &cur, units, first, u, pvertbone, pstudioverts, pstudionorms ))
				{
					free( nb ? nb : batches );
					return false;
				}
				batches = nb;
				batches[nbatches++] = cur;
			}
			if( u == nunits )
				break;

			// start a new batch with this unit
			first = u;
			memset( &cur, 0, sizeof( cur ));
			memcpy( cur.bones, ub, nub );
			cur.numbones = nub;
		}
		else
		{
			memcpy( cur.bones, mb, merged );
			cur.numbones = merged;
		}
	}

	*outbatches = batches;
	*outnum = nbatches;
	ge_cache_bytes += sizeof( *batches ) * nbatches;
	return nbatches > 0;
}

static qboolean R_StudioGEBuildMesh( gemodel_t *gm, gemesh_t *gmesh, const short *ptricmds, const byte *pvertbone, const byte *pnormbone, const vec3_t *pstudioverts, const vec3_t *pstudionorms )
{
	const int numunits = R_StudioGECountUnits( ptricmds, pvertbone, pnormbone );

	if( numunits < 0 )
		return false;

	geunit_t *units = malloc( sizeof( *units ) * ( numunits + 1 ));
	if( !units )
		return false;

	const int nunits = R_StudioGEAddUnits( units, 0, ptricmds, pvertbone, NULL );
	const qboolean ok = R_StudioGEBatchUnits( gm, units, nunits, pvertbone, pstudioverts, pstudionorms, &gmesh->batches, &gmesh->numbatches );

	free( units );
	return ok;
}

/*
===============
R_StudioGEGetModel

GE data for the current submodel, built on first use; NULL: CPU path only
===============
*/
static gemodel_t *R_StudioGEGetModel( void )
{
	uint h = R_StudioGEHash( m_pSubModel );

	for( int n = 0; n < GE_CACHE_SIZE; n++, h = ( h + 1 ) % GE_CACHE_SIZE )
	{
		gemodel_t *gm = ge_cache[h];

		if( !gm )
			break;
		if( gm->submodel == m_pSubModel && gm->hdr == m_pStudioHeader )
			return gm->nummesh ? gm : NULL;
	}

	if( ge_cache[h] ) // full
		return NULL;

	gemodel_t *gm = calloc( 1, sizeof( *gm ));
	if( !gm )
		return NULL;

	gm->hdr = m_pStudioHeader;
	gm->submodel = m_pSubModel;
	ge_cache[h] = gm;

	// bone weighted models: not yet
	if( FBitSet( m_pStudioHeader->flags, STUDIO_HAS_BONEWEIGHTS ) || m_pSubModel->nummesh <= 0 )
		return NULL;

	gm->meshes = calloc( m_pSubModel->nummesh, sizeof( *gm->meshes ));
	if( !gm->meshes )
		return NULL;

	const byte *pvertbone = (const byte *)m_pStudioHeader + m_pSubModel->vertinfoindex;
	const byte *pnormbone = (const byte *)m_pStudioHeader + m_pSubModel->norminfoindex;
	const vec3_t *pstudioverts = (const vec3_t *)((const byte *)m_pStudioHeader + m_pSubModel->vertindex);
	const vec3_t *pstudionorms = (const vec3_t *)((const byte *)m_pStudioHeader + m_pSubModel->normindex);
	const mstudiomesh_t *pmesh = (const mstudiomesh_t *)((const byte *)m_pStudioHeader + m_pSubModel->meshindex);
	int numge = 0;

	// the batches come when a mesh is drawn by itself: the ones in atlas pages
	// may never need them (R_StudioGEGetMerge)
	for( int j = 0; j < m_pSubModel->nummesh; j++ )
	{
		const short *ptricmds = (const short *)((const byte *)m_pStudioHeader + pmesh[j].triindex);

		if( R_StudioGECountUnits( ptricmds, pvertbone, pnormbone ) > 0 )
		{
			gm->meshes[j].ge = true;
			numge++;
		}
		else gm->cpumeshes = true;
	}
	(void)pstudioverts; (void)pstudionorms;

	gm->nummesh = m_pSubModel->nummesh;
	gEngfuncs.Con_DPrintf( "GE studio: %s %d/%d meshes, %u KB cached\n", m_pSubModel->name, numge, gm->nummesh, (uint)( ge_cache_bytes >> 10 ));

	return numge ? gm : NULL;
}

/*
===============
R_StudioGEMeshBatches

the batches of a mesh of the current submodel drawn by itself
===============
*/
static void R_StudioGEMeshBatches( gemodel_t *gm, int j )
{
	gemesh_t *gmesh = &gm->meshes[j];

	if( !gmesh->ge || gmesh->numbatches )
		return;

	const mstudiomesh_t *pmesh = (const mstudiomesh_t *)((const byte *)m_pStudioHeader + m_pSubModel->meshindex);
	const short *ptricmds = (const short *)((const byte *)m_pStudioHeader + pmesh[j].triindex);

	if( !R_StudioGEBuildMesh( gm, gmesh, ptricmds, (const byte *)m_pStudioHeader + m_pSubModel->vertinfoindex,
		(const byte *)m_pStudioHeader + m_pSubModel->norminfoindex,
		(const vec3_t *)((const byte *)m_pStudioHeader + m_pSubModel->vertindex),
		(const vec3_t *)((const byte *)m_pStudioHeader + m_pSubModel->normindex)))
	{
		gmesh->ge = false;
		gmesh->numbatches = 0;
		gm->cpumeshes = true;
	}
}

/*
===============
R_StudioGEGetMerge

the GE meshes of the current submodel that use the atlas pages for this skin
family, built on first use; NULL: none
===============
*/
static const gemerge_t *R_StudioGEGetMerge( gemodel_t *gm, int skin, const mstudiotexture_t *ptexture, const short *pskinref )
{
	for( gemerge_t *m = gm->merges; m; m = m->next )
		if( m->skin == skin ) return m->numgroups ? m : NULL;

	gemerge_t *m = calloc( 1, sizeof( *m ));
	if( !m )
		return NULL;
	m->skin = skin;
	m->next = gm->merges;
	gm->merges = m;

	if( !studio_atlas_slots || !( m->merged = calloc( gm->nummesh, 1 )))
		return NULL;

	const mstudiomesh_t *pmesh = (const mstudiomesh_t *)((const byte *)m_pStudioHeader + m_pSubModel->meshindex);
	const byte *pvertbone = (const byte *)m_pStudioHeader + m_pSubModel->vertinfoindex;
	const byte *pnormbone = (const byte *)m_pStudioHeader + m_pSubModel->norminfoindex;
	const vec3_t *pstudioverts = (const vec3_t *)((const byte *)m_pStudioHeader + m_pSubModel->vertindex);
	const vec3_t *pstudionorms = (const vec3_t *)((const byte *)m_pStudioHeader + m_pSubModel->normindex);
	int count[ATLAS_MAX_PAGES] = { 0 }, units[ATLAS_MAX_PAGES] = { 0 }, numgroups = 0;
	gemergegroup_t groups[ATLAS_MAX_PAGES];

	memset( groups, 0, sizeof( groups ));

	// GE meshes with a texture in a page and texture coordinates inside it
	for( int j = 0; j < gm->nummesh; j++ )
	{
		const mstudiotexture_t *ptex = &ptexture[pskinref[pmesh[j].skinref]];
		const short *ptricmds = (const short *)((const byte *)m_pStudioHeader + pmesh[j].triindex);
		const short *p = ptricmds;
		int i, g;

		if( !gm->meshes[j].ge || FBitSet( ptex->flags, GE_UNSUPPORTED_FLAGS|ATLAS_FLAGS ))
			continue;
		if( ptex->index <= 0 || ptex->index >= MAX_TEXTURES || !studio_atlas_slots[ptex->index].atlas )
			continue;

		qboolean inside = true;
		while( inside && ( i = *( p++ )))
		{
			if( i < 0 ) i = -i;
			for( int k = 0; k < i; k++, p += 4 )
				if( p[2] < 0 || p[2] > ptex->width || p[3] < 0 || p[3] > ptex->height ) inside = false;
		}
		if( !inside )
			continue;

		for( g = 0; g < numgroups; g++ )
			if( groups[g].atlas == studio_atlas_slots[ptex->index].atlas ) break;
		if( g == numgroups )
			groups[numgroups++].atlas = studio_atlas_slots[ptex->index].atlas;

		m->merged[j] = g + 1;
		count[g]++;
		units[g] += R_StudioGECountUnits( ptricmds, pvertbone, pnormbone );
	}

	for( int g = 0; g < numgroups; g++ )
	{
		geunit_t *u = count[g] >= 2 ? malloc( sizeof( *u ) * ( units[g] + 1 )) : NULL;
		int nunits = 0;

		for( int j = 0; u && j < gm->nummesh; j++ )
		{
			if( m->merged[j] != g + 1 )
				continue;
			const mstudiotexture_t *ptex = &ptexture[pskinref[pmesh[j].skinref]];
			nunits = R_StudioGEAddUnits( u, nunits, (const short *)((const byte *)m_pStudioHeader + pmesh[j].triindex), pvertbone, &studio_atlas_slots[ptex->index] );
		}

		ge_build_prevn = 0;
		if( u && R_StudioGEBatchUnits( gm, u, nunits, pvertbone, pstudioverts, pstudionorms, &groups[g].batches, &groups[g].numbatches ))
		{
			m->groups[m->numgroups++] = groups[g];
			for( int j = 0; j < gm->nummesh; j++ )
				if( m->merged[j] == g + 1 ) m->merged[j] = 0xff;
		}
		free( u );
	}

	// 0xff: drawn by a group
	for( int j = 0; j < gm->nummesh; j++ )
		m->merged[j] = m->merged[j] == 0xff;

	return m->numgroups ? m : NULL;
}

/*
===============
R_StudioGERamp

R_StudioLightMeshFast as a function of dot( normal, lightvec ), as a texture
===============
*/
static void R_StudioGERampFill( uint *ramp );

static uint *R_StudioGERamp( void )
{
	static float last[6];
	static uint *lastramp;
	static uint lastframe;
	const float params[6] = { g_studio.ambientlight, g_studio.shadelight, g_studio.lightcolor[0], g_studio.lightcolor[1], g_studio.lightcolor[2], 0.0f };

	if( lastramp && lastframe == guRender.vert_frame && !memcmp( params, last, sizeof( params )) )
		return lastramp;

	byte *mem = GU_GetVertexMemory( GE_RAMP_SIZE * 4 + 16 );
	uint *ramp = (uint *)((((uint)mem ) + 15 ) & ~15u );

	R_StudioGERampFill( ramp );
	memcpy( last, params, sizeof( last ));
	lastramp = ramp;
	lastframe = guRender.vert_frame;
	return ramp;
}

// the light ramp of g_studio's light, GE_RAMP_SIZE colors, 16 byte aligned
static void R_StudioGERampFill( uint *ramp )
{
	const float base = g_studio.ambientlight + g_studio.shadelight;
	const float k = g_studio.shadelight * ( 1.0f / SHADE_LAMBERT );
	const float off = SHADE_LAMBERT - 1.0f;
	const uint16_t *gamma = FBitSet( gp_host->features, ENGINE_LINEAR_GAMMA_SPACE ) ? NULL : tr.lightgammatable;
	const float cr = g_studio.lightcolor[0] * ( 255.0f / 1023.0f );
	const float cg = g_studio.lightcolor[1] * ( 255.0f / 1023.0f );
	const float cb = g_studio.lightcolor[2] * ( 255.0f / 1023.0f );

	for( int i = 0; i < GE_RAMP_SIZE; i++ )
	{
		float d = ( i + 0.5f ) * ( 2.0f / GE_RAMP_SIZE ) - 1.0f;
		float illum = base;

		d += off;
		if( d > 0.0f )
			illum -= k * d;
		if( illum < 0.0f )
			illum = 0.0f;
		else if( illum > 255.0f )
			illum = 255.0f;

		const int b = (int)( illum * 4.0f );
		const float g = gamma ? gamma[b] : b;

		ramp[i] = GUCOLOR4UB( (byte)( cr * g ), (byte)( cg * g ), (byte)( cb * g ), 255 );
	}
}

// bone of each GE matrix slot, valid for the current entity only
static byte	ge_slot_bone[GE_MAXBONES];
static uint	ge_slots_valid;

// ref: the point the translations are relative to, the eye or a prop's origin
static uint *R_StudioGEBones( uint *p, const gebatch_t *b, const float *ref )
{
	int next = -1; // slot the GE writes next without a BOFS

	for( int i = 0; i < b->numbones; i++ )
	{
		if( FBitSet( ge_slots_valid, BIT( i )) && ge_slot_bone[i] == b->bones[i] )
			continue;

		const float (*m)[4] = (const float (*)[4])g_studio.bonestransform[b->bones[i]];

		if( next != i )
			*p++ = GE_CMD( 0x2A, i * 12 ); // BOFS
		*p++ = GE_CMD( 0x2B, GE_Float( m[0][0] ));
		*p++ = GE_CMD( 0x2B, GE_Float( m[1][0] ));
		*p++ = GE_CMD( 0x2B, GE_Float( m[2][0] ));
		*p++ = GE_CMD( 0x2B, GE_Float( m[0][1] ));
		*p++ = GE_CMD( 0x2B, GE_Float( m[1][1] ));
		*p++ = GE_CMD( 0x2B, GE_Float( m[2][1] ));
		*p++ = GE_CMD( 0x2B, GE_Float( m[0][2] ));
		*p++ = GE_CMD( 0x2B, GE_Float( m[1][2] ));
		*p++ = GE_CMD( 0x2B, GE_Float( m[2][2] ));
		// relative to the eye: the GE's 24-bit floats lose too much at map coordinates
		*p++ = GE_CMD( 0x2B, GE_Float( m[0][3] - ref[0] ));
		*p++ = GE_CMD( 0x2B, GE_Float( m[1][3] - ref[1] ));
		*p++ = GE_CMD( 0x2B, GE_Float( m[2][3] - ref[2] ));
		next = i + 1;

		ge_slot_bone[i] = b->bones[i];
		SetBits( ge_slots_valid, BIT( i ));
	}
	return p;
}

static inline uint *R_StudioGECall( uint *p, const uint *addr )
{
	*p++ = GE_CMD( 0x10, ((uint)addr >> 8 ) & 0xf0000 );	// BASE
	*p++ = GE_CMD( 0x0A, (uint)addr );	// CALL
	return p;
}

/*
===============
R_StudioGEViewMatrix

the view matrix without the translation (the bones are relative to the eye)
===============
*/
static void R_StudioGEViewMatrix( qboolean eye_relative )
{
	// both matrices as call blocks, rebuilt when the view changes
	static matrix4x4 view;
	static uint frame = ~0u;
	static const uint *blocks[2];

	if( frame != guRender.vert_frame || memcmp( view, RI.worldviewMatrix, sizeof( view )))
	{
		ScePspFMatrix4 m;

		Matrix4x4_ToFMatrix4( RI.worldviewMatrix, &m );
		for( int i = 0; i < 2; i++ )
		{
			uint *start = extGuBlockBegin();
			if( i ) m.w.x = m.w.y = m.w.z = 0.0f;
			sceGuSetMatrix( GU_VIEW, &m );
			blocks[i] = extGuBlockEnd( start );
		}
		memcpy( view, RI.worldviewMatrix, sizeof( view ));
		frame = guRender.vert_frame;
	}

	uint *p = R_StudioGECall( extGuCmdBegin(), blocks[eye_relative ? 1 : 0] );
	extGuCmdEnd( p );
}

/*
===============
R_StudioGELightBlock

GE state for the lighting pass, called before each batch's second draw:
the ramp texture with u = 0.5 + 0.5 * dot( normal, lightvec ), multiplied
into the frame buffer where the depth matches
===============
*/
static const uint *R_StudioGEBuildLightBlock( const uint *ramp, qboolean flat );

static const uint *R_StudioGELightBlock( const uint *ramp, qboolean flat )
{
	static const uint *last[2], *lastramp[2];
	static vec3_t lastvec;
	static uint frame[2] = { ~0u, ~0u };

	flat = !!flat;
	if( frame[flat] == guRender.vert_frame && ramp == lastramp[flat] && ( flat || VectorCompare( lastvec, g_studio.lightvec )))
		return last[flat];

	last[flat] = R_StudioGEBuildLightBlock( ramp, flat );
	lastramp[flat] = ramp;
	if( !flat )
		VectorCopy( g_studio.lightvec, lastvec );
	frame[flat] = guRender.vert_frame;
	return last[flat];
}

// the light block as a call block in the display list, ends with a RET
static const uint *R_StudioGEBuildLightBlock_( const uint *ramp, qboolean flat );

/*
the light block differs between models only in the ramp address and the
texture matrix (the light vector): build it once with the sceGu calls, then
copy it and patch those words. ~15 sceGu calls per model before.
*/
static const uint *R_StudioGEBuildLightBlock( const uint *ramp, qboolean flat )
{
	static uint *tmpl[2];
	static uint words[2];
	static int tbp[2], tbw[2], tmd[2]; // word indices, -1: not found

	if( !tmpl[flat] )
	{
		const uint *block = R_StudioGEBuildLightBlock_( ramp, flat );
		const uint n = extGuCmdBegin() - block; // up to and including the RET
		uint *t = malloc( n * sizeof( uint ));

		if( !t )
			return block;

		memcpy( t, block, n * sizeof( uint ));
		tbp[flat] = tbw[flat] = tmd[flat] = -1;
		for( uint i = 0; i < n; i++ )
		{
			const uint cmd = t[i] >> 24;
			if( cmd == 0xA0 && tbp[flat] < 0 ) tbp[flat] = i;	// TBP0
			else if( cmd == 0xA8 && tbw[flat] < 0 ) tbw[flat] = i;	// TBW0
			else if( cmd == 0x41 && tmd[flat] < 0 ) tmd[flat] = i;	// TMD, 12 in a row after the TMS
		}
		if( tbp[flat] < 0 || tbw[flat] < 0 || tmd[flat] < 0 || tmd[flat] + 12 > (int)n )
		{
			gEngfuncs.Con_Printf( S_WARN "%s: unexpected light block, built each time\n", __func__ );
			free( t );
			tmpl[flat] = NULL;
			return block;
		}
		words[flat] = n;
		tmpl[flat] = t;
		return block;
	}

	uint *dst = extGuGetMemoryNoStall( words[flat] * sizeof( uint ));
	memcpy( dst, tmpl[flat], words[flat] * sizeof( uint ));
	dst[tbp[flat]] = GE_CMD( 0xA0, (uint)ramp );
	dst[tbw[flat]] = GE_CMD( 0xA8, ((((uint)ramp ) >> 8 ) & 0x0f0000 ) | GE_RAMP_SIZE );

	if( !flat )
	{
		// as below: column x is half the light vector, w = ( 0.5, 0.5, 1 )
		const float m[12] =
		{
			0.5f * g_studio.lightvec[0], 0.0f, 0.0f,
			0.5f * g_studio.lightvec[1], 0.0f, 0.0f,
			0.5f * g_studio.lightvec[2], 0.0f, 0.0f,
			0.5f, 0.5f, 1.0f,
		};
		uint *t = &dst[tmd[flat]];
		for( int i = 0; i < 12; i++ )
			t[i] = GE_CMD( 0x41, GE_Float( m[i] ));
	}

	glState.texIdentityMatrix = false;
	return dst;
}

static const uint *R_StudioGEBuildLightBlock_( const uint *ramp, qboolean flat )
{
	uint *start = extGuBlockBegin();
	ScePspFMatrix4 tm;

	sceGuTexScale( 1.0f, 1.0f );
	sceGuTexMode( GU_PSM_8888, 0, 0, GU_FALSE );
	sceGuTexFilter( GU_LINEAR, GU_LINEAR );
	sceGuTexWrap( GU_CLAMP, GU_CLAMP );
	sceGuTexImage( 0, GE_RAMP_SIZE, 1, GE_RAMP_SIZE, ramp );
	sceGuTexFunc( GU_TFX_REPLACE, GU_TCC_RGB );

	memset( &tm, 0, sizeof( tm ));
	if( flat )
	{
		// STUDIO_NF_FLATSHADE: ambient + shade * 0.8 for every normal, which is
		// the ramp at dot( normal, lightvec ) = 1 - 0.8 * SHADE_LAMBERT
		tm.w.x = 1.0f - 0.4f * SHADE_LAMBERT;
	}
	else
	{
		tm.x.x = 0.5f * g_studio.lightvec[0];
		tm.y.x = 0.5f * g_studio.lightvec[1];
		tm.z.x = 0.5f * g_studio.lightvec[2];
		tm.w.x = 0.5f;
	}
	tm.w.y = 0.5f;
	tm.w.z = 1.0f;
	sceGuSetMatrix( GU_TEXTURE, &tm );
	sceGuTexMapMode( GU_TEXTURE_MATRIX, 0, 0 );
	sceGuTexProjMapMode( GU_NORMALIZED_NORMAL );

	sceGuDepthFunc( GU_EQUAL );
	sceGuDepthMask( GU_TRUE );
	sceGuBlendFunc( GU_ADD, GU_FIX, GU_SRC_COLOR, 0, 0 );
	sceGuEnable( GU_BLEND );

	glState.texIdentityMatrix = false;
	return extGuBlockEnd( start );
}

/*
===============
R_StudioGEDrawMesh

per batch: its bones, the skin pass (writes depth), the lighting pass
===============
*/
static const uint *ge_last_skinblock; // state to restore after GE meshes

/*
the skin pass state of each texture (bind, texture scale, depth and blend
state) as a persistent call block: building it costs ~10 us, and with ~70
textures in a firefight that was 0.7 ms per frame. Checked against what it
was built from on its first use in a frame, rebuilt when a texture or a cvar
changed (~220 meshes a frame share ~40 textures).
*/
typedef struct
{
	uint		*block;
	uint		words, capacity;
	const void	*data, *palette;	// gl_texture_t the block points to
	int		format, width, height, numMips, flags;
	int		skinw, skinh;	// mstudiotexture_t size for the texture scale
	int		chrome;
	float		cvars[5];
	uint		frame;	// guRender.vert_frame it was last checked in
} geskin_t;

static geskin_t	*ge_skins; // MAX_TEXTURES, allocated on first use

static void R_StudioGESkinKey( const gl_texture_t *t, const mstudiotexture_t *ptex, geskin_t *k )
{
	k->data = t->dstTexture;
	k->palette = t->dstPalette;
	k->format = t->format;
	k->width = t->width;
	k->height = t->height;
	k->numMips = t->numMips;
	k->flags = t->flags;
	k->skinw = ptex->width;
	k->skinh = ptex->height;
	k->chrome = FBitSet( ptex->flags, STUDIO_NF_CHROME );
	k->cvars[0] = gl_texture_nearest.value;
	k->cvars[1] = gl_texture_lodfunc.value;
	k->cvars[2] = gl_texture_lodbias.value;
	k->cvars[3] = gl_texture_lodslope.value;
	k->cvars[4] = gl_lightmap_nearest.value;
}

static qboolean R_StudioGESkinMatches( const geskin_t *a, const geskin_t *b )
{
	return a->data == b->data && a->palette == b->palette && a->format == b->format && a->width == b->width
		&& a->height == b->height && a->numMips == b->numMips && a->flags == b->flags && a->skinw == b->skinw
		&& a->skinh == b->skinh && a->chrome == b->chrome && !memcmp( a->cvars, b->cvars, sizeof( a->cvars ));
}

static const uint *R_StudioGEBuildSkinBlock( const mstudiotexture_t *ptex, int skinindex, int bindtex )
{
	uint *start = extGuBlockBegin();

	glState.currentTexture = 0; // force GL_Bind to emit everything
	if( bindtex )
		GL_Bind( XASH_TEXTURE0, bindtex ); // an atlas page
	else R_StudioSetupSkin( m_pStudioHeader, skinindex );
	if( FBitSet( ptex->flags, STUDIO_NF_CHROME ))
	{
		// texcoords from the normals, R_StudioGEChromeMatrix sets the matrix for each batch
		sceGuTexScale( 1.0f, 1.0f );
		sceGuTexMapMode( GU_TEXTURE_MATRIX, 0, 0 );
		sceGuTexProjMapMode( GU_NORMALIZED_NORMAL );
	}
	else
	{
		// texcoords are the raw texel shorts: u = s / 32768 * scale
		sceGuTexScale( 32768.0f / ptex->width, 32768.0f / ptex->height );
		sceGuTexMapMode( GU_TEXTURE_COORDS, 0, 0 );
	}
	sceGuTexFunc( GU_TFX_MODULATE, GU_TCC_RGBA );
	// the batches have no vertex color: the GE modulates with the material
	// color instead, which is whatever sceGuColor was last (blood sprites, shells)
	sceGuColor( 0xffffffff );
	sceGuDepthFunc( GU_LEQUAL );
	sceGuDepthMask( GU_FALSE );
	sceGuDisable( GU_BLEND );
	const uint *block = extGuBlockEnd( start );
	return block;
}

// bindtex: the texture to bind instead of the skin's (atlas pages)
static const uint *R_StudioGESkinBlock( const mstudiotexture_t *ptex, int skinindex, int bindtex )
{
	const int texnum = bindtex ? bindtex : ptex->index;

	// remapped player skins differ per entity, r_lightmap binds the white texture
	if(( m_fDoRemap && !bindtex ) || r_lightmap->value || texnum <= 0 || texnum >= MAX_TEXTURES )
		return R_StudioGEBuildSkinBlock( ptex, skinindex, bindtex );

	if( !ge_skins && !( ge_skins = calloc( MAX_TEXTURES, sizeof( *ge_skins ))))
		return R_StudioGEBuildSkinBlock( ptex, skinindex, bindtex );

	geskin_t *s = &ge_skins[texnum];
	geskin_t key;

	GL_TouchTexture( texnum );
	if( s->block && s->frame == guRender.vert_frame )
	{
		glState.currentTexture = texnum; // what the block binds
		return s->block;
	}

	R_StudioGESkinKey( R_GetTexture( texnum ), ptex, &key );
	if( s->block && R_StudioGESkinMatches( s, &key ))
	{
		glState.currentTexture = texnum; // what the block binds
		s->frame = guRender.vert_frame;
		return s->block;
	}

	const uint *frame_block = R_StudioGEBuildSkinBlock( ptex, skinindex, bindtex );
	const uint words = extGuCmdBegin() - frame_block; // up to and including the RET

	if( words > s->capacity )
	{
		// an old block may still be called from the list the GE is drawing:
		// only happens when a texture or a cvar changed, let it leak
		uint *mem = memalign( 16, words * sizeof( uint ));
		if( !mem )
			return frame_block;
		ge_rec_gen++; // recorded props call the old block
		s->block = mem;
		s->capacity = words;
	}

	memcpy( s->block, frame_block, words * sizeof( uint ));
	sceKernelDcacheWritebackRange( s->block, words * sizeof( uint ));
	s->words = words;
	key.block = s->block;
	key.words = s->words;
	key.capacity = s->capacity;
	*s = key;
	s->frame = guRender.vert_frame;
	return frame_block;
}

/*
===============
R_StudioGEChromeMatrix

the texture matrix of a chrome batch, R_StudioSetupChrome for the normals the GE
skinned: one view direction for the whole batch, to the middle of its bones
===============
*/
static uint *R_StudioGEChromeMatrix( uint *p, const gebatch_t *b, const mstudiotexture_t *ptex )
{
	vec3_t org = { 0.0f, 0.0f, 0.0f }, dir, up, right;

	for( int i = 0; i < b->numbones; i++ )
	{
		const float (*m)[4] = (const float (*)[4])g_studio.lighttransform[b->bones[i]];

		org[0] += m[0][3];
		org[1] += m[1][3];
		org[2] += m[2][3];
	}
	VectorScale( org, 1.0f / b->numbones, org );
	VectorSubtract( org, g_studio.chrome_origin, dir );
	VectorNormalize( dir );
	CrossProduct( dir, RI.vright, up );
	VectorNormalize( up );
	CrossProduct( up, dir, right );
	VectorNormalize( right );

	// u = ( dot( normal, right ) + 1 ) * 32 / width, v the same with up
	const float su = 32.0f / ptex->width, sv = 32.0f / ptex->height;
	const float m[12] =
	{
		right[0] * su, up[0] * sv, 0.0f,
		right[1] * su, up[1] * sv, 0.0f,
		right[2] * su, up[2] * sv, 0.0f,
		su, sv, 1.0f,
	};

	*p++ = GE_CMD( 0x40, 0 ); // TMS
	for( int i = 0; i < 12; i++ )
		*p++ = GE_CMD( 0x41, GE_Float( m[i] )); // TMD
	return p;
}

static void R_StudioGEDrawMesh( const gemesh_t *gmesh, const mstudiotexture_t *ptex, int skinindex, const uint *ramp )
{
	const int flags = ptex->flags | g_nForceFaceFlags;
	const uint *skinblock = R_StudioGESkinBlock( ptex, skinindex, 0 );
	const uint *lightblock = R_StudioGELightBlock( ramp, FBitSet( flags, STUDIO_NF_FLATSHADE ));
	const qboolean chrome = FBitSet( flags, STUDIO_NF_CHROME );

	uint *p = extGuCmdBegin();
	for( int i = 0; i < gmesh->numbatches; i++ )
	{
		const gebatch_t *b = &gmesh->batches[i];

		p = R_StudioGEBones( p, b, RI.rvp.vieworigin );
		if( chrome )
			p = R_StudioGEChromeMatrix( p, b, ptex );
		p = R_StudioGECall( p, skinblock );
		p = R_StudioGECall( p, b->cmds );
		p = R_StudioGECall( p, lightblock );
		p = R_StudioGECall( p, b->cmds );
	}
	extGuCmdEnd( p );
	ge_last_skinblock = skinblock;
}

// the meshes of an atlas page: texture coordinates in the page, texture scale 1
static mstudiotexture_t ge_atlas_ptex = { "atlas", 0, 32768, 32768, 0 };

static void R_StudioGEDrawMerged( const gemergegroup_t *g, const uint *ramp )
{
	const uint *skinblock = R_StudioGESkinBlock( &ge_atlas_ptex, 0, g->atlas );
	const uint *lightblock = R_StudioGELightBlock( ramp, false );

	uint *p = extGuCmdBegin();
	for( int i = 0; i < g->numbatches; i++ )
	{
		const gebatch_t *b = &g->batches[i];

		p = R_StudioGEBones( p, b, RI.rvp.vieworigin );
		p = R_StudioGECall( p, skinblock );
		p = R_StudioGECall( p, b->cmds );
		p = R_StudioGECall( p, lightblock );
		p = R_StudioGECall( p, b->cmds );
	}
	extGuCmdEnd( p );
	ge_last_skinblock = skinblock;
}

/*
===============
R_StudioGERestore

back to the skin pass state (no texture matrix, depth writes, no blending),
which the rest of the renderer expects
===============
*/
static void R_StudioGERestore( void )
{
	if( !ge_last_skinblock )
		return;

	uint *p = R_StudioGECall( extGuCmdBegin(), ge_last_skinblock );
	extGuCmdEnd( p );
	sceGuTexScale( 1.0f, 1.0f );
	sceGuTexMapMode( GU_TEXTURE_COORDS, 0, 0 ); // after a chrome skin block
	glState.currentTexture = 0;
	ge_last_skinblock = NULL;
}

/*
===============
R_StudioGEAllowed

entity state that the GE path can draw
===============
*/
#if XASH_PSP
/*
===============
R_StudioGEFoldElights

entity lights (muzzle flashes, ~20 models a frame in a firefight) need the
per-vertex CPU path, ~130 us more per model. Fold them into the model's one
directional light instead, the way R_EntityDynamicLight adds dlights: the
model brightens toward the flash, tinted by it, instead of per vertex.
===============
*/
static CVAR_DEFINE_AUTO( r_studio_ge_elights, "1", FCVAR_ARCHIVE, "GE studio models: fold entity lights into the model light instead of lighting on the CPU" );

static void R_StudioGEFoldElights( qboolean bones )
{
	if( !g_studio.numlocallights || !r_studio_ge_elights.value )
		return;

	vec3_t origin, dir;
	Matrix3x4_OriginFromMatrix( g_studio.rotationmatrix, origin );

	// the light as a vector: direction of travel scaled by its strength
	vec3_t vec;
	VectorScale( g_studio.lightvec, g_studio.shadelight, vec );
	float ambient = g_studio.ambientlight;
	float base = g_studio.ambientlight + g_studio.shadelight;
	vec3_t color;
	VectorScale( g_studio.lightcolor, base, color );
	float total = base;

	for( int i = 0; i < g_studio.numlocallights; i++ )
	{
		const dlight_t *el = g_studio.locallight[i];
		const int *lc = g_studio.locallightcolor[i]; // linear, 0..1023

		VectorSubtract( origin, el->origin, dir );
		float d2 = DotProduct( dir, dir );
		float s = d2 > g_studio.locallightR2[i] ? sqrtf( g_studio.locallightR2[i] / d2 ) : 1.0f;
		float m = Q_max( Q_max( lc[0], lc[1] ), lc[2] );

		if( m <= 0.0f )
			continue;

		float e = m * ( 1.0f / 4.0f ) * s; // illumination units, like ambient and shade

		if( d2 > 0.0f )
			VectorScale( dir, ( e * 0.5f ) / sqrtf( d2 ), dir );
		else VectorClear( dir );
		VectorAdd( vec, dir, vec );
		ambient += e * 0.5f;
		total += e;

		for( int j = 0; j < 3; j++ )
			color[j] += lc[j] / m * e;
	}

	float shade = VectorLength( vec );
	if( shade > 0.0f )
		VectorScale( vec, 1.0f / shade, g_studio.lightvec );
	g_studio.shadelight = shade;
	g_studio.ambientlight = ambient;

	float cm = Q_max( Q_max( color[0], color[1] ), color[2] );
	if( cm > 0.0f && total > 0.0f )
		VectorScale( color, 1.0f / cm, g_studio.lightcolor );

	for( int i = 0; bones && i < m_pStudioHeader->numbones; i++ )
		Matrix3x4_VectorIRotate( g_studio.lighttransform[i], g_studio.lightvec, g_studio.blightvec[i] );

	g_studio.numlocallights = 0;
}
#endif

static qboolean R_StudioGEAllowed( void )
{
	if( !r_studio_ge.value || g_nForceFaceFlags )
		return false;
	if( g_studio.rendermode != kRenderNormal || RI.currententity->curstate.renderfx == kRenderFxGlowShell )
		return false;
	if( glState.isFogEnabled || r_lightmap->value || r_fullbright->value )
		return false;
	if( FBitSet( m_pStudioHeader->flags, STUDIO_HAS_BONEWEIGHTS ))
		return false;
#if XASH_PSP
	// only once the GE path is otherwise allowed: the CPU path needs the lights as they are
	R_StudioGEFoldElights( true );
#endif
	if( g_studio.numlocallights )
		return false;
	return true;
}

/*
===============
GE draw plans

What R_StudioDrawPoints sends for a submodel the GE draws all of is the same
every frame but for the bones and the light: per batch its bones, the skin
block, the batch, the light block and the batch again. A plan keeps that list
per skin family (the batches and which skin and light block each needs), so
a draw is the view matrix, the ramp and the list, without the mesh loops,
merge and batch lookups, texture flags and sizes. ~150 us of mostly cache
misses per model before, for every body part.
===============
*/
static CVAR_DEFINE_AUTO( r_studio_geplan, "1", FCVAR_ARCHIVE, "draw the GE submodels from a list built once per skin family" );

typedef struct
{
	const gebatch_t		*batches;
	int			numbatches;
	const mstudiotexture_t	*ptex;		// for R_StudioGESkinBlock
	int			skinindex;
	int			bindtex;		// an atlas page, 0: the skin's texture
	qboolean		flat;
	qboolean		chrome;
} geplanitem_t;

typedef struct geplan_s
{
	struct geplan_s	*next;		// first, see R_StudioGEFreeModel
	int		skin;		// skin family
	int		atlas;		// r_studio_atlas it was built with
	qboolean		usable;		// false: some mesh needs R_StudioDrawPoints
	int		polys;
	int		numitems;
	geplanitem_t	items[];
} geplan_t;

// the body parts of a model drawn from plans share the view matrix and the end state
static qboolean ge_plan_open;

/*
GE lighting instead of the light pass: one directional light toward
-lightvec and the global ambient, from the light ramp's colors where the
surface is edge on (ambient) and facing the light (ambient + diffuse). The
lit side is the ramp's; the GE clamps the side away from the light at the
edge on color, where the ramp went on darkening down to the ambient light.
Each batch is drawn once instead of twice, and needs no ramp or light block.
*/
static CVAR_DEFINE_AUTO( r_studio_ge_hwlight, "1", FCVAR_ARCHIVE, "GE studio models: GE lighting, one pass (faster, the dark side is lighter)" );
static qboolean ge_hwlight_on;

// R_StudioGERampFill's color at d = dot( normal, lightvec )
static void R_StudioGELightColor( float d, int rgb[3] )
{
	const float base = g_studio.ambientlight + g_studio.shadelight;
	const float k = g_studio.shadelight * ( 1.0f / SHADE_LAMBERT );
	const uint16_t *gamma = FBitSet( gp_host->features, ENGINE_LINEAR_GAMMA_SPACE ) ? NULL : tr.lightgammatable;
	float illum = base;

	d += SHADE_LAMBERT - 1.0f;
	if( d > 0.0f )
		illum -= k * d;
	illum = bound( 0.0f, illum, 255.0f );

	const int b = (int)( illum * 4.0f );
	const float g = gamma ? gamma[b] : b;

	for( int i = 0; i < 3; i++ )
		rgb[i] = bound( 0, (int)( g_studio.lightcolor[i] * ( 255.0f / 1023.0f ) * g ), 255 );
}

static void R_StudioGEHwLight( qboolean flat )
{
	int lo[3], hi[3];

	if( flat )
	{
		// see R_StudioGEBuildLightBlock_
		R_StudioGELightColor( 1.0f - 0.8f * SHADE_LAMBERT, lo );
		VectorCopy( lo, hi );
	}
	else
	{
		R_StudioGELightColor( 0.0f, lo );
		R_StudioGELightColor( -1.0f, hi );
	}

	sceGuAmbient( GUCOLOR4UB( lo[0], lo[1], lo[2], 255 ));
	sceGuLightColor( 0, GU_DIFFUSE, GUCOLOR4UB( Q_max( hi[0] - lo[0], 0 ), Q_max( hi[1] - lo[1], 0 ), Q_max( hi[2] - lo[2], 0 ), 255 ));
}

static void R_StudioGEPlanClose( void )
{
	if( !ge_plan_open )
		return;
	ge_plan_open = false;
	if( ge_hwlight_on )
	{
		sceGuDisable( GU_LIGHTING );
		sceGuDisable( GU_LIGHT0 );
		ge_hwlight_on = false;
	}
	R_StudioGERestore();
	R_StudioGEViewMatrix( false );
	extGuUpdateStall();
}

static geplan_t *R_StudioGEBuildPlan( gemodel_t *gm, int skin, const mstudiotexture_t *ptexture, const short *pskinref, int atlas )
{
	const mstudiomesh_t *pmesh = (const mstudiomesh_t *)((const byte *)m_pStudioHeader + m_pSubModel->meshindex);
	const gemerge_t *merge = atlas ? R_StudioGEGetMerge( gm, skin, ptexture, pskinref ) : NULL;
	geplan_t *pl = calloc( 1, sizeof( *pl ) + sizeof( geplanitem_t ) * ( ATLAS_MAX_PAGES + gm->nummesh ));

	if( !pl )
		return NULL;

	pl->skin = skin;
	pl->atlas = atlas;
	pl->usable = !gm->cpumeshes;
	pl->next = gm->plans;
	gm->plans = pl;

	// as R_StudioDrawPoints: the atlas pages first, then the other meshes in order
	for( int g = 0; merge && g < merge->numgroups; g++ )
	{
		geplanitem_t *it = &pl->items[pl->numitems++];
		it->batches = merge->groups[g].batches;
		it->numbatches = merge->groups[g].numbatches;
		it->ptex = &ge_atlas_ptex;
		it->bindtex = merge->groups[g].atlas;
	}

	for( int j = 0; j < gm->nummesh; j++ )
	{
		pl->polys += pmesh[j].numtris;
		if( merge && merge->merged[j] )
			continue;

		const mstudiotexture_t *ptex = &ptexture[pskinref[pmesh[j].skinref]];
		R_StudioGEMeshBatches( gm, j );

		// masked and additive meshes change the state around them, sorted by R_StudioMeshCompare
		if( !gm->meshes[j].numbatches || FBitSet( ptex->flags, GE_UNSUPPORTED_FLAGS|STUDIO_NF_MASKED|STUDIO_NF_ADDITIVE ))
		{
			pl->usable = false;
			continue;
		}

		geplanitem_t *it = &pl->items[pl->numitems++];
		it->batches = gm->meshes[j].batches;
		it->numbatches = gm->meshes[j].numbatches;
		it->ptex = ptex;
		it->skinindex = pskinref[pmesh[j].skinref];
		it->flat = FBitSet( ptex->flags, STUDIO_NF_FLATSHADE ) ? true : false;
		it->chrome = FBitSet( ptex->flags, STUDIO_NF_CHROME ) ? true : false;
	}
	return pl;
}

static geplan_t *R_StudioGEGetPlan( gemodel_t *gm, int skin, const mstudiotexture_t *ptexture, const short *pskinref, int atlas )
{
	for( geplan_t *pl = gm->plans; pl; pl = pl->next )
	{
		if( pl->skin == skin && pl->atlas == atlas )
			return pl;
	}

	return R_StudioGEBuildPlan( gm, skin, ptexture, pskinref, atlas );
}

// true when the submodel was drawn from its plan
static qboolean R_StudioGEDrawPlan( gemodel_t *gm, int skin, const mstudiotexture_t *ptexture, const short *pskinref )
{
	const geplan_t *pl = R_StudioGEGetPlan( gm, skin, ptexture, pskinref, r_studio_atlas.value );

	if( !pl || !pl->usable )
		return false;

	if( glState.faceCull != GL_NONE )
	{
		if( R_AllowFlipViewModel( RI.currententity ))
		{
			tr.fFlipViewModel = true;
			GL_Cull( GL_NONE );
		}
		else
		{
			tr.fFlipViewModel = false;
			GL_Cull( GL_FRONT );
		}
	}

	if( !ge_plan_open )
	{
		R_StudioGEViewMatrix( true );
		ge_plan_open = true;
	}
	const uint *skinblock = NULL;
	ge_slots_valid = 0;

	if( r_studio_ge_hwlight.value )
	{
		const ScePspFVector3 dir = { -g_studio.lightvec[0], -g_studio.lightvec[1], -g_studio.lightvec[2] };
		int flat = -1;

		sceGuLight( 0, GU_DIRECTIONAL, GU_DIFFUSE, &dir );
		sceGuEnable( GU_LIGHT0 );
		sceGuEnable( GU_LIGHTING );
		ge_hwlight_on = true;

		for( int i = 0; i < pl->numitems; i++ )
		{
			const geplanitem_t *it = &pl->items[i];

			if( flat != it->flat )
			{
				flat = it->flat;
				R_StudioGEHwLight( flat );
			}

			// may build the block in the display list: before the commands
			skinblock = R_StudioGESkinBlock( it->ptex, it->skinindex, it->bindtex );
			uint *p = R_StudioGECall( extGuCmdBegin(), skinblock );

			for( int k = 0; k < it->numbatches; k++ )
			{
				const gebatch_t *b = &it->batches[k];

				p = R_StudioGEBones( p, b, RI.rvp.vieworigin );
				if( it->chrome )
					p = R_StudioGEChromeMatrix( p, b, it->ptex );
				p = R_StudioGECall( p, b->cmds );
			}
			extGuCmdEnd( p );
		}

		ge_last_skinblock = skinblock; // R_StudioGEPlanClose restores
		r_stats.c_studio_polys += pl->polys;
		return true;
	}

	const uint *ramp = R_StudioGERamp();

	for( int i = 0; i < pl->numitems; i++ )
	{
		const geplanitem_t *it = &pl->items[i];

		// both may build their block in the display list: before the commands
		skinblock = R_StudioGESkinBlock( it->ptex, it->skinindex, it->bindtex );
		const uint *lightblock = R_StudioGELightBlock( ramp, it->flat );
		uint *p = extGuCmdBegin();

		for( int k = 0; k < it->numbatches; k++ )
		{
			const gebatch_t *b = &it->batches[k];

			p = R_StudioGEBones( p, b, RI.rvp.vieworigin );
			if( it->chrome )
				p = R_StudioGEChromeMatrix( p, b, it->ptex );
			p = R_StudioGECall( p, skinblock );
			p = R_StudioGECall( p, b->cmds );
			p = R_StudioGECall( p, lightblock );
			p = R_StudioGECall( p, b->cmds );
		}
		extGuCmdEnd( p );
	}

	ge_last_skinblock = skinblock; // R_StudioGEPlanClose restores

	r_stats.c_studio_polys += pl->polys;
	return true;
}

/*
===============
R_StudioSubmitMesh

===============
*/
static void R_StudioSubmitMesh( short *ptricmds, vec3_t *pstudionorms, float s, float t, float shellscale, int tesslevel )
{
	// strips and fans go straight into the display list, no index arrays
	if( FBitSet( g_nFaceFlags, STUDIO_NF_CHROME ))
		R_StudioDrawChromeMesh( ptricmds, pstudionorms, s, t, shellscale );
	else if( FBitSet( g_nFaceFlags, STUDIO_NF_UV_COORDS ))
		R_StudioDrawFloatMesh( ptricmds, pstudionorms );
	else
		R_StudioDrawNormalMesh( ptricmds, pstudionorms, s, t );
}

/*
===============
draw lists for the Media Engine

The models R_StudioMeAdd takes that are drawn from plans only: the ME
culls them, computes bones, attachments and light and writes the GE commands of
all their body parts, R_StudioFinishMeModels does the rest, without the client
DLL's renderer. A medraw_t has the plan items of a model, body
and skin.
===============
*/
#define MEDRAW_CACHE	64
#define MEDRAW_MAXITEMS	40

typedef struct medraw_s
{
	const studiohdr_t	*hdr;
	const mstudiotexture_t	*remap;	// a player's textures, or NULL
	int		body, skin, atlas;
	uint		gen;		// ge_rec_gen it was built in
	uint		frame;		// guRender.vert_frame the skin blocks were checked in
	qboolean		usable;
	int		polys;
	int		words;		// at most, of the commands
	int		numitems;
	int		firstflat;	// the flat ones come last
	const geplanitem_t	*items[MEDRAW_MAXITEMS];
	const uint	*blocks[MEDRAW_MAXITEMS];	// their skins
} medraw_t;

static medraw_t	r_medraw[MEDRAW_CACHE];

static int R_StudioMeDrawWords( const mebones_item_t *it )
{
	return ( it->draw ? it->draw->words : 0 ) + ( it->wdraw ? it->wdraw->words : 0 );
}

static void R_StudioMeDrawBuild( medraw_t *d, int body, int skinnum, const mstudiotexture_t *remap )
{
	const studiohdr_t *hdr = m_pStudioHeader;
	const mstudiotexture_t *ptexture = (const mstudiotexture_t *)((const byte *)hdr + hdr->textureindex);
	const short *pskinref = (const short *)((const byte *)hdr + hdr->skinindex);
	int skin = 0;

	d->hdr = hdr;
	d->remap = remap;
	d->body = body;
	d->skin = skinnum;
	d->atlas = r_studio_atlas.value;
	d->usable = false;
	d->frame = guRender.vert_frame - 1;
	d->numitems = d->firstflat = d->polys = d->words = 0;
	d->gen = ge_rec_gen;

	if( d->skin > 0 && d->skin < hdr->numskinfamilies )
	{
		skin = d->skin;
		pskinref += skin * hdr->numskinref;
	}

	for( int i = 0; i < hdr->numbodyparts; i++ )
	{
		R_StudioSetupModel( i, NULL, NULL );
		if( m_pSubModel->nummesh <= 0 )
			continue;
		if( m_pSubModel->numverts > MAXSTUDIOVERTS || m_pSubModel->numnorms > MAXSTUDIOVERTS )
			return;

		gemodel_t *gm = R_StudioGEGetModel();
		if( !gm )
			return;

		// the pages of the atlas don't have a player's colors
		geplan_t *pl = R_StudioGEGetPlan( gm, skin, ptexture, pskinref, remap ? 0 : (int)r_studio_atlas.value );
		if( !pl || !pl->usable || d->numitems + pl->numitems > MEDRAW_MAXITEMS )
			return;

		for( int j = 0; j < pl->numitems; j++ )
		{
			const geplanitem_t *it = &pl->items[j];
			int at = d->numitems++;

			if( !it->flat )
			{
				for( ; at > d->firstflat; at-- )
					d->items[at] = d->items[at - 1];
				d->firstflat++;
			}
			d->items[at] = it;
		}
		d->polys += pl->polys;
	}

	// as R_MeDrawCommands, the most it takes
	int slot_bone[GE_MAXBONES];

	for( int i = 0; i < GE_MAXBONES; i++ )
		slot_bone[i] = -1;

	for( int i = 0; i < d->numitems; i++ )
	{
		const geplanitem_t *it = d->items[i];

		d->words += 4; // skin, blending
		for( int k = 0; k < it->numbatches; k++ )
		{
			const gebatch_t *b = &it->batches[k];
			int next = -1;

			for( int n = 0; n < b->numbones; n++ )
			{
				if( slot_bone[n] == b->bones[n] )
					continue;
				d->words += next != n ? 13 : 12;
				next = n + 1;
				slot_bone[n] = b->bones[n];
			}
			d->words += 2 + ( it->chrome ? 13 : 0 );
		}
	}

	d->words += 3 + 2 * 2 + 1; // light, smooth and flat, RET
	d->gen = ge_rec_gen; // it may have built batches
	d->usable = d->numitems > 0;
}

// NULL: not for the ME. body and skin are e's, it sets RI.currententity and m_pStudioHeader
static const medraw_t *R_StudioMeDrawGet( const studiohdr_t *hdr, cl_entity_t *e, int body, int skin, const mstudiotexture_t *remap )
{
	const uint h = ((uint)hdr >> 6 ) + body * 7 + skin * 13 + ((uint)remap >> 5 );
	medraw_t *d = NULL;

	for( int i = 0; i < 2; i++ )
	{
		medraw_t *s = &r_medraw[( h + i ) % MEDRAW_CACHE];

		if( s->hdr == hdr && s->body == body && s->skin == skin && s->remap == remap && s->atlas == (int)r_studio_atlas.value && s->gen == ge_rec_gen )
		{
			d = s;
			break;
		}

		// the ME reads the ones used in this frame
		if( !d && ( !s->hdr || s->frame != guRender.vert_frame ))
			d = s;
	}

	if( !d )
		return NULL;

	RI.currententity = e;
	m_pStudioHeader = (studiohdr_t *)hdr;

	if( d->hdr != hdr || d->body != body || d->skin != skin || d->remap != remap || d->atlas != (int)r_studio_atlas.value || d->gen != ge_rec_gen )
	{
		if( d->hdr && d->frame == guRender.vert_frame )
			return NULL;
		R_StudioMeDrawBuild( d, body, skin, remap );
	}

	if( !d->usable )
		return NULL;

	// the persistent copies of the skin blocks
	if( d->frame != guRender.vert_frame )
	{
		m_fDoRemap = false;

		for( int i = 0; i < d->numitems; i++ )
		{
			const geplanitem_t *it = d->items[i];
			const mstudiotexture_t *ptex = remap && !it->bindtex ? remap + it->skinindex : it->ptex;
			const int texnum = it->bindtex ? it->bindtex : ptex->index;

			R_StudioGESkinBlock( ptex, it->skinindex, remap ? texnum : it->bindtex );
			if( !ge_skins || texnum <= 0 || texnum >= MAX_TEXTURES || !ge_skins[texnum].block || ge_skins[texnum].frame != guRender.vert_frame )
				return NULL;
			d->blocks[i] = ge_skins[texnum].block;
		}
		d->frame = guRender.vert_frame;
	}

	return d;
}

// R_StudioGEChromeMatrix without the VFPU
static uint *R_MeChromeMatrix( const mebones_job_t *job, const mebones_ent_t *m, uint *p, const gebatch_t *b, const mstudiotexture_t *ptex )
{
	vec3_t dir = { 0.0f, 0.0f, 0.0f }, up, right;
	float len;

	for( int i = 0; i < b->numbones; i++ )
	{
		const float (*bm)[4] = (const float (*)[4])m->out[b->bones[i]];

		dir[0] += bm[0][3];
		dir[1] += bm[1][3];
		dir[2] += bm[2][3];
	}

	const float k = 1.0f / b->numbones;
	for( int i = 0; i < 3; i++ )
		dir[i] = dir[i] * k - job->eye[i];

	len = DotProduct( dir, dir );
	if( len > 0.0f )
	{
		len = 1.0f / __builtin_sqrtf( len );
		VectorScale( dir, len, dir );
	}
	CrossProduct( dir, job->vright, up );
	len = DotProduct( up, up );
	if( len > 0.0f )
	{
		len = 1.0f / __builtin_sqrtf( len );
		VectorScale( up, len, up );
	}
	CrossProduct( up, dir, right );
	len = DotProduct( right, right );
	if( len > 0.0f )
	{
		len = 1.0f / __builtin_sqrtf( len );
		VectorScale( right, len, right );
	}

	const float su = 32.0f / ptex->width, sv = 32.0f / ptex->height;
	const float mat[12] =
	{
		right[0] * su, up[0] * sv, 0.0f,
		right[1] * su, up[1] * sv, 0.0f,
		right[2] * su, up[2] * sv, 0.0f,
		su, sv, 1.0f,
	};

	*p++ = GE_CMD( 0x40, 0 ); // TMS
	for( int i = 0; i < 12; i++ )
		*p++ = GE_CMD( 0x41, GE_Float( mat[i] )); // TMD
	return p;
}

// R_StudioGELightColor
static uint R_MeLightColor( const mebones_frame_t *frame, const melight_t *l, float d, int rgb[3] )
{
	float illum = l->ambient + l->shade;

	d += SHADE_LAMBERT - 1.0f;
	if( d > 0.0f )
		illum -= l->shade * ( 1.0f / SHADE_LAMBERT ) * d;
	illum = bound( 0.0f, illum, 255.0f );

	const int b = (int)( illum * 4.0f );
	const float g = frame->gamma ? frame->gamma[b] : b;

	for( int i = 0; i < 3; i++ )
	{
		const int c = (int)( l->color[i] * ( 255.0f / 1023.0f ) * g );
		rgb[i] = bound( 0, c, 255 );
	}

	return rgb[0] | rgb[1] << 8 | rgb[2] << 16;
}

// R_StudioGEHwLight, the alpha is the main CPU's
static uint *R_MeLightCommands( const mebones_frame_t *frame, uint *p, const melight_t *l, qboolean flat )
{
	int lo[3], hi[3];

	if( flat )
	{
		*p++ = GE_CMD( 0x5C, R_MeLightColor( frame, l, 1.0f - 0.8f * SHADE_LAMBERT, lo ));
		*p++ = GE_CMD( 0x90, 0 );
	}
	else
	{
		*p++ = GE_CMD( 0x5C, R_MeLightColor( frame, l, 0.0f, lo )); // global ambient
		R_MeLightColor( frame, l, -1.0f, hi );
		*p++ = GE_CMD( 0x90, Q_max( hi[0] - lo[0], 0 ) | Q_max( hi[1] - lo[1], 0 ) << 8 | Q_max( hi[2] - lo[2], 0 ) << 16 ); // diffuse of light 0
	}
	return p;
}

/*
the draws of a model, as R_StudioGEDrawPlan and R_StudioGEBones write them
*/
static uint *R_MeDrawCommands( const mebones_job_t *job, uint *p, const mebones_ent_t *m, const medraw_t *d, const melight_t *l, int mode )
{
	int slot_bone[GE_MAXBONES];

	for( int i = 0; i < GE_MAXBONES; i++ )
		slot_bone[i] = -1;

	for( int i = 0; i < d->numitems; i++ )
	{
		const geplanitem_t *it = d->items[i];

		if( mode != MEMODE_ADD && ( i == 0 || i == d->firstflat ))
			p = R_MeLightCommands( job->frame, p, l, i >= d->firstflat );

		p = R_StudioGECall( p, d->blocks[i] );
		if( mode != MEMODE_SOLID )
		{
			// over the skin block's
			*p++ = GE_CMD( 0x21, 1 ); // blend
			if( mode == MEMODE_ADD )
				*p++ = GE_CMD( 0xE7, 1 ); // no depth writes
		}

		for( int k = 0; k < it->numbatches; k++ )
		{
			const gebatch_t *b = &it->batches[k];
			int next = -1; // slot the GE writes next without a BOFS

			for( int n = 0; n < b->numbones; n++ )
			{
				if( slot_bone[n] == b->bones[n] )
					continue;

				const float (*bm)[4] = (const float (*)[4])m->out[b->bones[n]];

				if( next != n )
					*p++ = GE_CMD( 0x2A, n * 12 ); // BOFS
				for( int c = 0; c < 3; c++ )
				{
					*p++ = GE_CMD( 0x2B, GE_Float( bm[0][c] ));
					*p++ = GE_CMD( 0x2B, GE_Float( bm[1][c] ));
					*p++ = GE_CMD( 0x2B, GE_Float( bm[2][c] ));
				}
				for( int r = 0; r < 3; r++ )
					*p++ = GE_CMD( 0x2B, GE_Float( bm[r][3] - job->eye[r] ));
				next = n + 1;
				slot_bone[n] = b->bones[n];
			}

			if( it->chrome )
				p = R_MeChromeMatrix( job, m, p, b, it->ptex );
			p = R_StudioGECall( p, b->cmds );
		}
	}

	return p;
}

/*
the light and the draws of an item at r_mebones_me.p, NULL if they don't fit
*/
static uint *R_MeModelCommands( const mebones_job_t *job, const mebones_item_t *it, const mebones_ent_t *m, const melight_t *l )
{
	uint *p = r_mebones_me.p;

	if( p + R_StudioMeDrawWords( it ) >= job->cmds + job->maxwords )
		return NULL;

	// toward the light
	*p++ = GE_CMD( 0x63, GE_Float( -l->vec[0] ));
	*p++ = GE_CMD( 0x64, GE_Float( -l->vec[1] ));
	*p++ = GE_CMD( 0x65, GE_Float( -l->vec[2] ));

	p = R_MeDrawCommands( job, p, m, it->draw, l, it->mode );
	if( it->wdraw && m->weapon )
		p = R_MeDrawCommands( job, p, m->weapon, it->wdraw, l, it->mode );

	if( it->index >= MECULL_MAXSOLID )
		*p++ = GE_CMD( 0x0B, 0 ); // RET
	return p;
}

/*
===============
R_StudioDrawPoints

===============
*/
static void R_StudioDrawPoints( void )
{
	float		shellscale = 0.0f;
	qboolean		need_sort = false;

	if( !m_pStudioHeader ) return;

	if( m_pSubModel->numverts > MAXSTUDIOVERTS || m_pSubModel->numnorms > MAXSTUDIOVERTS )
	{
		gEngfuncs.Con_DPrintf( S_ERROR "%s: %s has too many vertices (%d, %d), max %d\n", __func__,
			m_pSubModel->name, m_pSubModel->numverts, m_pSubModel->numnorms, MAXSTUDIOVERTS );
		return;
	}


	g_studio.numverts = g_studio.numelems = 0;

	int m_skinnum = RI.currententity->curstate.skin;
	mstudiotexture_t *ptexture = (mstudiotexture_t *)((byte *)m_pStudioHeader + m_pStudioHeader->textureindex);
	byte *pvertbone = ((byte *)m_pStudioHeader + m_pSubModel->vertinfoindex);
	byte *pnormbone = ((byte *)m_pStudioHeader + m_pSubModel->norminfoindex);

	mstudiomesh_t *pmesh = (mstudiomesh_t *)((byte *)m_pStudioHeader + m_pSubModel->meshindex);
	vec3_t *pstudioverts = (vec3_t *)((byte *)m_pStudioHeader + m_pSubModel->vertindex);
	vec3_t *pstudionorms = (vec3_t *)((byte *)m_pStudioHeader + m_pSubModel->normindex);

	short *pskinref = (short *)((byte *)m_pStudioHeader + m_pStudioHeader->skinindex);
	if( m_skinnum > 0 && m_skinnum < m_pStudioHeader->numskinfamilies )
		pskinref += (m_skinnum * m_pStudioHeader->numskinref);
	else m_skinnum = 0;

	const int numverts = m_pSubModel->numverts;
	const int numnorms = m_pSubModel->numnorms;

	gemodel_t *gm = R_StudioGEAllowed() ? R_StudioGEGetModel() : NULL;
	if( gm && r_studio_geplan.value && !m_fDoRemap && R_StudioGEDrawPlan( gm, m_skinnum, ptexture, pskinref ))
		return;
	R_StudioGEPlanClose(); // the rest assumes the normal state
	// meshes in atlas pages are drawn as one per page, the others need their own batches
	const gemerge_t *merge = NULL;
	if( gm && r_studio_atlas.value && !m_fDoRemap )
		merge = R_StudioGEGetMerge( gm, m_skinnum, ptexture, pskinref );
	ge_build_prevn = 0; // consecutive meshes keep shared bones in the same slots
	for( int j = 0; gm && j < gm->nummesh; j++ )
		if( !( merge && merge->merged[j] )) R_StudioGEMeshBatches( gm, j );

	qboolean cpumeshes = !gm || gm->cpumeshes;

	// the skin family decides the flags
	for( int j = 0; gm && !cpumeshes && j < m_pSubModel->nummesh; j++ )
	{
		if( FBitSet( ptexture[pskinref[pmesh[j].skinref]].flags, GE_UNSUPPORTED_FLAGS ))
			cpumeshes = true;
	}

	if( !cpumeshes )
	{
		// the GE draws everything
	}
	else if( FBitSet( m_pStudioHeader->flags, STUDIO_HAS_BONEWEIGHTS ) && m_pSubModel->blendvertinfoindex != 0 && m_pSubModel->blendnorminfoindex != 0 )
	{
		mstudioboneweight_t	*pvertweight = (mstudioboneweight_t *)((byte *)m_pStudioHeader + m_pSubModel->blendvertinfoindex);
		mstudioboneweight_t	*pnormweight = (mstudioboneweight_t *)((byte *)m_pStudioHeader + m_pSubModel->blendnorminfoindex);
		matrix3x4		skinMat;

		for( int i = 0; i < numverts; i++ )
		{
			R_StudioComputeSkinMatrix( &pvertweight[i], skinMat );
			Matrix3x4_VectorTransform( skinMat, pstudioverts[i], g_studio.verts[i] );
			R_LightStrength( pvertbone[i], pstudioverts[i], g_studio.lightpos[i] );
		}

		for( int i = 0; i < numnorms; i++ )
		{
			R_StudioComputeSkinMatrix( &pnormweight[i], skinMat );
			Matrix3x4_VectorRotate( skinMat, pstudionorms[i], g_studio.norms[i] );
		}
	}
	else
	{
		for( int i = 0; i < numverts; i++ )
			Matrix3x4_VectorTransform( g_studio.bonestransform[pvertbone[i]], pstudioverts[i], g_studio.verts[i] );

		// only local lights use lightpos
		if( g_studio.numlocallights )
		{
			for( int i = 0; i < numverts; i++ )
				R_LightStrength( pvertbone[i], pstudioverts[i], g_studio.lightpos[i] );
		}
	}

	// generate shared normals for properly scaling glowing shell
	if( RI.currententity->curstate.renderfx == kRenderFxGlowShell )
	{
		float factor = (1.0f / 128.0f);
		shellscale = Q_max( factor, RI.currententity->curstate.renderamt * factor );
		R_StudioBuildNormalTable();
		R_StudioGenerateNormals();
	}

	int k = 0;
	for( int j = 0; j < m_pSubModel->nummesh; j++ )
	{
		g_nFaceFlags = ptexture[pskinref[pmesh[j].skinref]].flags | g_nForceFaceFlags;

		// fill in sortedmesh info
		g_studio.meshes[j].flags = g_nFaceFlags;
		g_studio.meshes[j].mesh = &pmesh[j];

		if( FBitSet( g_nFaceFlags, STUDIO_NF_MASKED|STUDIO_NF_ADDITIVE ))
			need_sort = true;

		if( gm && ( gm->meshes[j].numbatches || ( merge && merge->merged[j] )) && !FBitSet( g_nFaceFlags, GE_UNSUPPORTED_FLAGS ))
		{
			// lit by the GE
			const int n = pmesh[j].numnorms;
			k += n;
			pstudionorms += n;
			pnormbone += n;
		}
		else if( RI.currententity->curstate.rendermode == kRenderTransAdd )
		{
			for( int i = 0; i < pmesh[j].numnorms; i++, k++, pstudionorms++, pnormbone++ )
			{
				if( FBitSet( g_nFaceFlags, STUDIO_NF_CHROME ))
					R_StudioSetupChrome( g_studio.chrome[k], *pnormbone, (float *)pstudionorms );
				VectorSet( g_studio.lightvalues[k], tr.blend, tr.blend, tr.blend );
				R_StudioPackLightColor( k );
			}
		}
		else if( !g_studio.numlocallights && !FBitSet( m_pStudioHeader->flags, STUDIO_HAS_BONEWEIGHTS )
			&& !FBitSet( g_nFaceFlags, STUDIO_NF_FULLBRIGHT|STUDIO_NF_FLATSHADE|STUDIO_NF_CHROME ))
		{
			const int n = pmesh[j].numnorms;

			R_StudioLightMeshFast( &g_studio.lightcolors[k], n, (const vec3_t *)pstudionorms, pnormbone );
			k += n;
			pstudionorms += n;
			pnormbone += n;
		}
		else
		{
			for( int i = 0; i < pmesh[j].numnorms; i++, k++, pstudionorms++, pnormbone++ )
			{
				float lv_tmp;
				if( FBitSet( m_pStudioHeader->flags, STUDIO_HAS_BONEWEIGHTS ))
					R_StudioLighting( &lv_tmp, -1, g_nFaceFlags, g_studio.norms[k] );
				else R_StudioLighting( &lv_tmp, *pnormbone, g_nFaceFlags, (float *)pstudionorms );

				if( FBitSet( g_nFaceFlags, STUDIO_NF_CHROME ))
					R_StudioSetupChrome( g_studio.chrome[k], *pnormbone, (float *)pstudionorms );
				VectorScale( g_studio.lightcolor, lv_tmp, g_studio.lightvalues[k] );
				R_StudioPackLightColor( k );
			}
		}
	}

	if( r_studio_sort_textures.value && need_sort )
	{
		// resort opaque and translucent meshes draw order
		qsort( g_studio.meshes, m_pSubModel->nummesh, sizeof( sortedmesh_t ), R_StudioMeshCompare );
	}

	// NOTE: rewind normals at start
	pstudionorms = (vec3_t *)((byte *)m_pStudioHeader + m_pSubModel->normindex);

	// backface culling for left-handed weapons
	//
	// there is a bug in GoldSrc that sets backface culling to GL_FRONT
	// but doesn't enable OpenGL GL_CULL_FACE. This allows mod developers to disable
	// backface culling through TriAPI call
	//
	// see https://github.com/FWGS/xash3d-fwgs/issues/2517
	if( glState.faceCull != GL_NONE )
	{
		if( R_AllowFlipViewModel( RI.currententity ))
		{
			tr.fFlipViewModel = true;
			GL_Cull( GL_NONE );
		}
		else
		{
			tr.fFlipViewModel = false;
			GL_Cull( GL_FRONT );
		}
	}

	const mstudiomesh_t *pmeshbase = (mstudiomesh_t *)((byte *)m_pStudioHeader + m_pSubModel->meshindex);
	qboolean eye_view = false;
	const uint *ramp = NULL;

	ge_slots_valid = 0;

	// the meshes in atlas pages first, one draw per page
	if( merge )
	{
		eye_view = true;
		R_StudioGEViewMatrix( true );
		ramp = R_StudioGERamp();
		for( int i = 0; i < merge->numgroups; i++ )
			R_StudioGEDrawMerged( &merge->groups[i], ramp );
	}

	for( int j = 0; j < m_pSubModel->nummesh; j++ )
	{
		float	oldblend = tr.blend;

		pmesh = g_studio.meshes[j].mesh;
		if( merge && merge->merged[pmesh - pmeshbase] )
		{
			r_stats.c_studio_polys += pmesh->numtris;
			continue;
		}
		const gemesh_t *gmesh = gm ? &gm->meshes[pmesh - pmeshbase] : NULL;
		if( gmesh && ( !gmesh->numbatches || FBitSet( ptexture[pskinref[pmesh->skinref]].flags | g_nForceFaceFlags, GE_UNSUPPORTED_FLAGS )))
			gmesh = NULL;

		if( !!gmesh != eye_view )
		{
			if( !gmesh )
				R_StudioGERestore();
			eye_view = !!gmesh;
			R_StudioGEViewMatrix( eye_view );
		}
		if( gmesh && !ramp )
			ramp = R_StudioGERamp( );
		short *ptricmds = (short *)((byte *)m_pStudioHeader + pmesh->triindex);

		g_nFaceFlags = ptexture[pskinref[pmesh->skinref]].flags | g_nForceFaceFlags;

		float s = 1.0f / (float)ptexture[pskinref[pmesh->skinref]].width;
		float t = 1.0f / (float)ptexture[pskinref[pmesh->skinref]].height;

		if( FBitSet( g_nFaceFlags, STUDIO_NF_MASKED ))
		{
			sceGuEnable( GU_ALPHA_TEST );
			sceGuAlphaFunc( GU_GREATER, 0x7f, 0xff );
			sceGuDepthMask( GU_FALSE ); // depth writes
			if( R_ModelOpaque( RI.currententity->curstate.rendermode ))
				tr.blend = 1.0f;
		}
		else if( FBitSet( g_nFaceFlags, STUDIO_NF_ADDITIVE ))
		{
			if( R_ModelOpaque( RI.currententity->curstate.rendermode ))
			{
				sceGuBlendFunc( GU_ADD, GU_FIX, GU_FIX, GUBLEND1, GUBLEND1 );
				sceGuDepthMask( GU_TRUE ); // no depth writes
				sceGuEnable( GU_BLEND );
				R_AllowFog( false );
			}
			else sceGuBlendFunc( GU_ADD, GU_SRC_ALPHA, GU_FIX, 0, GUBLEND1 );
		}

		if( gmesh )
			R_StudioGEDrawMesh( gmesh, &ptexture[pskinref[pmesh->skinref]], pskinref[pmesh->skinref], ramp );
		else
		{
			R_StudioSetupSkin( m_pStudioHeader, pskinref[pmesh->skinref] );
			R_StudioSubmitMesh( ptricmds, pstudionorms, s, t, shellscale, 0 );
		}

		if( FBitSet( g_nFaceFlags, STUDIO_NF_MASKED ))
		{
			sceGuAlphaFunc( GU_GREATER, DEFAULT_ALPHATEST, 0xff );
			sceGuDisable( GU_ALPHA_TEST );
		}
		else if( FBitSet( g_nFaceFlags, STUDIO_NF_ADDITIVE ) && R_ModelOpaque( RI.currententity->curstate.rendermode ))
		{
			sceGuDepthMask( GU_FALSE ); // depth writes
			sceGuDisable( GU_BLEND );
			R_AllowFog( true );
		}

		r_stats.c_studio_polys += pmesh->numtris;
		tr.blend = oldblend;
	}

	if( eye_view )
	{
		R_StudioGERestore();
		R_StudioGEViewMatrix( false );
		extGuUpdateStall();
	}
}

/*
===============
R_StudioDrawHulls

===============
*/
static void R_StudioDrawHulls( void )
{
	float alpha = ( r_drawentities->value == 4 ) ? 0.5f : 1.0f;

	GL_Bind( XASH_TEXTURE0, tr.whiteTexture );
	sceGuTexFunc( GU_TFX_MODULATE, GU_TCC_RGBA );

	for( int i = 0; i < m_pStudioHeader->numhitboxes; i++ )
	{
		mstudiobbox_t	*pbbox = (mstudiobbox_t *)((byte *)m_pStudioHeader + m_pStudioHeader->hitboxindex);
		vec3_t		tmp, p[8];

		for( int j = 0; j < 8; j++ )
		{
			tmp[0] = (j & 1) ? pbbox[i].bbmin[0] : pbbox[i].bbmax[0];
			tmp[1] = (j & 2) ? pbbox[i].bbmin[1] : pbbox[i].bbmax[1];
			tmp[2] = (j & 4) ? pbbox[i].bbmin[2] : pbbox[i].bbmax[2];

			Matrix3x4_VectorTransform( g_studio.bonestransform[pbbox[i].bone], tmp, p[j] );
		}

		int j = (pbbox[i].group % 8);

		TriBegin( TRI_QUADS );
		TriColor4f( hullcolor[j][0], hullcolor[j][1], hullcolor[j][2], alpha );

		for( j = 0; j < 6; j++ )
		{
			float lv;
			VectorClear( tmp );
			tmp[j % 3] = (j < 3) ? 1.0f : -1.0f;
			R_StudioLighting( &lv, pbbox[i].bone, 0, tmp );

			TriBrightness( lv );
			TriVertex3fv( p[boxpnt[j][0]] );
			TriVertex3fv( p[boxpnt[j][1]] );
			TriVertex3fv( p[boxpnt[j][2]] );
			TriVertex3fv( p[boxpnt[j][3]] );
		}
		TriEnd();
	}
}

/*
===============
R_StudioDrawAbsBBox

===============
*/
static void R_StudioDrawAbsBBox( void )
{
	vec3_t	p[8], tmp;

	// looks ugly, skip
	if( RI.currententity == tr.viewent )
		return;

	if( !R_StudioComputeBBox( p ))
		return;

	GL_Bind( XASH_TEXTURE0, tr.whiteTexture );
	TriColor4f( 0.5f, 0.5f, 1.0f, 0.5f );
	TriRenderMode( kRenderTransAdd );

	TriBegin( TRI_QUADS );
	for( int i = 0; i < 6; i++ )
	{
		float lv;
		VectorClear( tmp );
		tmp[i % 3] = (i < 3) ? 1.0f : -1.0f;
		R_StudioLighting( &lv, -1, 0, tmp );

		TriBrightness( lv );
		TriVertex3fv( p[boxpnt[i][0]] );
		TriVertex3fv( p[boxpnt[i][1]] );
		TriVertex3fv( p[boxpnt[i][2]] );
		TriVertex3fv( p[boxpnt[i][3]] );
	}
	TriEnd();
	TriRenderMode( kRenderNormal );
}

/*
===============
R_StudioDrawBones

===============
*/
static void R_StudioDrawBones( void )
{
	mstudiobone_t	*pbones = (mstudiobone_t *) ((byte *)m_pStudioHeader + m_pStudioHeader->boneindex);
	vec3_t		point;

	sceGuDisable( GU_TEXTURE_2D );

	for( int i = 0; i < m_pStudioHeader->numbones; i++ )
	{
		gu_vert_fcv_t *const out = extGuGetMemoryNoStall( sizeof( gu_vert_fcv_t ) * 2 );

		Matrix3x4_OriginFromMatrix( g_studio.bonestransform[i], point );
		out[1].c = pbones[i].parent >= 0 ? GU_COLOR( 0.0f, 0.0f, 0.8f, 1.0f ) : GU_COLOR( 0.8f, 0.0f, 0.0f, 1.0f );
		VectorCopy( point, &out[1].x );

		if( pbones[i].parent >= 0 )
		{
			Matrix3x4_OriginFromMatrix( g_studio.bonestransform[pbones[i].parent], point );
			out[0].c = GU_COLOR( 1.0f, 0.7f, 0.0f, 1.0f );
			VectorCopy( point, &out[0].x );
			extGuDrawArrayBatched( GU_LINES, GU_COLOR_8888 | GU_VERTEX_32BITF, 2, out );
		}

		extGuDrawArrayBatched( GU_POINTS, GU_COLOR_8888 | GU_VERTEX_32BITF, 1, &out[1] );
	}

	sceGuEnable( GU_TEXTURE_2D );
}

static void R_StudioDrawAttachments( void )
{
	sceGuDisable( GU_TEXTURE_2D );
	sceGuDisable( GU_DEPTH_TEST );

	for( int i = 0; i < m_pStudioHeader->numattachments; i++ )
	{
		vec3_t		v[4];

		mstudioattachment_t *pattachments = (mstudioattachment_t *)((byte *)m_pStudioHeader + m_pStudioHeader->attachmentindex);
		Matrix3x4_VectorTransform( g_studio.bonestransform[pattachments[i].bone], pattachments[i].org, v[0] );
		Matrix3x4_VectorTransform( g_studio.bonestransform[pattachments[i].bone], pattachments[i].vectors[0], v[1] );
		Matrix3x4_VectorTransform( g_studio.bonestransform[pattachments[i].bone], pattachments[i].vectors[1], v[2] );
		Matrix3x4_VectorTransform( g_studio.bonestransform[pattachments[i].bone], pattachments[i].vectors[2], v[3] );

		gu_vert_fcv_t *const out = extGuGetMemoryNoStall( sizeof( gu_vert_fcv_t ) * 6 );

		for( int j = 0; j < 3; j++ )
		{
			out[j * 2 + 0].c = GU_COLOR( 1.0f, 0.0f, 0.0f, 1.0f );
			VectorCopy( v[0], &out[j * 2 + 0].x );
			out[j * 2 + 1].c = GU_COLOR( 1.0f, 1.0f, 1.0f, 1.0f );
			VectorCopy( v[j + 1], &out[j * 2 + 1].x );
		}

		extGuDrawArrayBatched( GU_LINES, GU_COLOR_8888 | GU_VERTEX_32BITF, 6, out );
	}

	sceGuEnable( GU_TEXTURE_2D );
	sceGuEnable( GU_DEPTH_TEST );
}

/*
===============
R_StudioSetRemapColors

===============
*/
static void R_StudioSetRemapColors( int newTop, int newBottom )
{
	if( gEngfuncs.CL_EntitySetRemapColors( RI.currententity, RI.currentmodel, newTop, newBottom ))
		m_fDoRemap = true;
}

#if XASH_PSP
static void R_StudioPropCacheClear( void );
#endif

void R_StudioResetPlayerModels( void )
{
	memset( g_studio.player_models, 0, sizeof( g_studio.player_models ));
#if XASH_PSP
	R_StudioPropCacheClear(); // called on a new map
#endif
}

/*
===============
R_StudioSetupPlayerModel

===============
*/
static model_t *R_StudioSetupPlayerModel( int index )
{
	player_info_t  *info = gEngfuncs.pfnPlayerInfo( index );

	if( index < 0 || index >= gp_cl->maxclients )
		return NULL;

	player_model_t *state = &g_studio.player_models[index];

	// g-cont: force for "dev-mode", non-local games and menu preview
	if(( gpGlobals->developer || !ENGINE_GET_PARM( PARM_SINGLEPLAYER_GAME ) || !FBitSet( RI.rvp.flags, RF_DRAW_WORLD ) ) && info->model[0] )
	{
		if( Q_strcmp( state->name, info->model ))
		{
			Q_strncpy( state->name, info->model, sizeof( state->name ));
			state->name[sizeof( state->name ) - 1] = 0;

			Q_snprintf( state->modelname, sizeof( state->modelname ), "models/player/%s/%s.mdl", info->model, info->model );

			if( gEngfuncs.fsapi->FileExists( state->modelname, false ) && PSP_PlayerModelFits( state->modelname ))
				state->model = gEngfuncs.Mod_ForName( state->modelname, false, true );
			else
				state->model = NULL;

			if( !state->model )
				state->model = RI.currententity->model;
		}
	}
	else
	{
		if( state->model != RI.currententity->model )
			state->model = RI.currententity->model;
		state->name[0] = 0;
	}

	return state->model;
}

/*
================
R_StudioMostlyAdditive

more additive than opaque textures; also runs on the Media Engine
================
*/
qboolean R_StudioMostlyAdditive( const studiohdr_t *phdr )
{
	const mstudiotexture_t *ptexture = (const mstudiotexture_t *)((const byte *)phdr + phdr->textureindex);
	int opaque = 0, trans = 0;

	for( int i = 0; i < phdr->numtextures; i++, ptexture++ )
	{
		// ignore chrome & additive it's just a specular-like effect
		if( FBitSet( ptexture->flags, STUDIO_NF_ADDITIVE ) && !FBitSet( ptexture->flags, STUDIO_NF_CHROME ))
			trans++;
		else opaque++;
	}

	return trans > opaque;
}

/*
================
R_GetEntityRenderMode

check for texture flags
================
*/
int R_GetEntityRenderMode( cl_entity_t *ent )
{
	model_t *model = NULL;

	cl_entity_t *oldent = RI.currententity;
	RI.currententity = ent;

	if( ent->player ) // check it for real playermodel
		model = R_StudioSetupPlayerModel( ent->curstate.number - 1 );

	if( !model )
		model = ent->model;

	RI.currententity = oldent;

	studiohdr_t *phdr;
	if(( phdr = gEngfuncs.Mod_Extradata( mod_studio, model )) == NULL )
	{
		if( R_ModelOpaque( ent->curstate.rendermode ))
		{
			// forcing to choose right sorting type
			if(( model && model->type == mod_brush ) && FBitSet( model->flags, MODEL_TRANSPARENT ))
				return kRenderTransAlpha;
		}
		return ent->curstate.rendermode;
	}
	// if model is more additive than opaque
	if( R_StudioMostlyAdditive( phdr ))
		return kRenderTransAdd;
	return ent->curstate.rendermode;
}

/*
===============
R_StudioClientEvents

===============
*/
static void R_StudioClientEvents( void )
{
	cl_entity_t	*e = RI.currententity;

	if( g_studio.frametime == 0.0 )
		return; // gamepaused

	// fill attachments with interpolated origin
	if( m_pStudioHeader->numattachments <= 0 )
	{
		Matrix3x4_OriginFromMatrix( g_studio.rotationmatrix, e->attachment[0] );
		Matrix3x4_OriginFromMatrix( g_studio.rotationmatrix, e->attachment[1] );
		Matrix3x4_OriginFromMatrix( g_studio.rotationmatrix, e->attachment[2] );
		Matrix3x4_OriginFromMatrix( g_studio.rotationmatrix, e->attachment[3] );
	}

	if( FBitSet( e->curstate.effects, EF_MUZZLEFLASH ))
	{
		dlight_t	*el = gEngfuncs.CL_AllocElight( 0 );

		ClearBits( e->curstate.effects, EF_MUZZLEFLASH );
		VectorCopy( e->attachment[0], el->origin );
		el->die = gp_cl->time + 0.05f;
		el->color.r = 255;
		el->color.g = 192;
		el->color.b = 64;
		el->decay = 320;
		el->radius = 24;
	}

	int sequence = bound( 0, e->curstate.sequence, m_pStudioHeader->numseq - 1 );
	mstudioseqdesc_t *pseqdesc = (mstudioseqdesc_t *)((byte *)m_pStudioHeader + m_pStudioHeader->seqindex) + sequence;

	// no events for this animation
	if( pseqdesc->numevents == 0 )
		return;

	float end = R_StudioEstimateFrame( e, pseqdesc, g_studio.time );
	float start = end - e->curstate.framerate * gp_host->frametime * pseqdesc->fps;
	mstudioevent_t *pevent = (mstudioevent_t *)((byte *)m_pStudioHeader + pseqdesc->eventindex);

	if( e->latched.sequencetime == e->curstate.animtime )
	{
		if( !FBitSet( pseqdesc->flags, STUDIO_LOOPING ))
			start = -0.01f;
	}

	for( int i = 0; i < pseqdesc->numevents; i++ )
	{
		// ignore all non-client-side events
		if( pevent[i].event < EVENT_CLIENT )
			continue;

		if( (float)pevent[i].frame > start && pevent[i].frame <= end )
			gEngfuncs.pfnStudioEvent( &pevent[i], e );
	}
}

/*
===============
R_StudioGetForceFaceFlags

===============
*/
static int R_StudioGetForceFaceFlags( void )
{
	return g_nForceFaceFlags;
}

/*
===============
R_StudioSetForceFaceFlags

===============
*/
static void R_StudioSetForceFaceFlags( int flags )
{
	g_nForceFaceFlags = flags;
}

/*
===============
pfnStudioSetHeader

===============
*/
static void R_StudioSetHeader( studiohdr_t *pheader )
{
	m_pStudioHeader = pheader;
	m_fDoRemap = false;
}

studiohdr_t *R_StudioGetHeader( void )
{
	return m_pStudioHeader;
}

/*
===============
R_StudioSetRenderModel

===============
*/
static void R_StudioSetRenderModel( model_t *model )
{
	RI.currentmodel = model;
}

/*
===============
R_StudioSetupRenderer

===============
*/
static void R_StudioSetupRenderer( int rendermode )
{
	studiohdr_t	*phdr = m_pStudioHeader;

	if( rendermode > kRenderTransAdd ) rendermode = 0;
	g_studio.rendermode = bound( 0, rendermode, kRenderTransAdd );

	if( g_studio.rendermode == kRenderTransAdd || g_studio.rendermode == kRenderGlow )
		R_AllowFog( false );

	sceGuTexFunc( GU_TFX_MODULATE, GU_TCC_RGBA );
	sceGuDisable( GU_ALPHA_TEST );
	sceGuShadeModel( GU_SMOOTH );

	// a point to setup local to world transform for boneweighted models
	if( phdr && FBitSet( phdr->flags, STUDIO_HAS_BONEINFO ))
	{
		// NOTE: extended boneinfo goes immediately after bones
		mstudioboneinfo_t *boneinfo = (mstudioboneinfo_t *)((byte *)phdr + phdr->boneindex + phdr->numbones * sizeof( mstudiobone_t ));

		for( int i = 0; i < phdr->numbones; i++ )
			Matrix3x4_ConcatTransforms( g_studio.worldtransform[i], g_studio.bonestransform[i], boneinfo[i].poseToBone );
	}
}

/*
===============
R_StudioRestoreRenderer

===============
*/
static void R_StudioRestoreRenderer( void )
{
	if( g_studio.rendermode != kRenderNormal )
		sceGuDisable( GU_BLEND );

	if( g_studio.rendermode == kRenderTransAdd || g_studio.rendermode == kRenderGlow )
		R_AllowFog( true );

	sceGuTexFunc( GU_TFX_REPLACE, GU_TCC_RGBA );
	sceGuShadeModel( GU_FLAT );
	m_fDoRemap = false;
}

/*
===============
R_StudioSetChromeOrigin

===============
*/
static void R_StudioSetChromeOrigin( void )
{
	VectorCopy( RI.rvp.vieworigin, g_studio.chrome_origin );
}

/*
===============
R_StudioDrawPointsShadow

===============
*/
static void R_StudioDrawPointsShadow( void )
{
	if( FBitSet( RI.currententity->curstate.effects, EF_NOSHADOW ))
		return;

	if( glState.stencilEnabled )
		sceGuEnable( GU_STENCIL_TEST );

	float vec_x = -g_studio.lightvec[0] * 8.0f;
	float vec_y = -g_studio.lightvec[1] * 8.0f;

	for( int k = 0; k < m_pSubModel->nummesh; k++ )
	{
		mstudiomesh_t *pmesh = (mstudiomesh_t *)((byte *)m_pStudioHeader + m_pSubModel->meshindex) + k;
		short *ptricmds = (short *)((byte *)m_pStudioHeader + pmesh->triindex);

		r_stats.c_studio_polys += pmesh->numtris;

		int i;
		while(( i = *( ptricmds++ )))
		{
			int prim = GU_TRIANGLE_STRIP;

			if( i < 0 )
			{
				prim = GU_TRIANGLE_FAN;
				i = -i;
			}

			gu_vert_fv_t *const out = extGuGetMemoryNoStall( sizeof( gu_vert_fv_t ) * i );

			for( int index = 0; index < i; index++, ptricmds += 4 )
			{
				float *av = g_studio.verts[ptricmds[0]];
				out[index].x = av[0] - (vec_x * ( av[2] - g_studio.lightspot[2] ));
				out[index].y = av[1] - (vec_y * ( av[2] - g_studio.lightspot[2] ));
				out[index].z = g_studio.lightspot[2] + 1.0f;
			}

			extGuDrawArrayBatched( prim, GU_VERTEX_32BITF, i, out );
		}
	}

	if( glState.stencilEnabled )
		sceGuDisable( GU_STENCIL_TEST );
}

/*
===============
GL_StudioSetRenderMode

set rendermode for studiomodel
===============
*/
static void GL_StudioSetRenderMode( int rendermode )
{
	// NOTE: sceGuDepthMask( GU_TRUE ) disables depth writes
	switch( rendermode )
	{
	case kRenderNormal:
		break;
	case kRenderTransColor:
		sceGuBlendFunc( GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0 );
		sceGuTexFunc( GU_TFX_MODULATE, GU_TCC_RGBA );
		sceGuEnable( GU_BLEND );
		break;
	case kRenderTransAdd:
		sceGuTexFunc( GU_TFX_MODULATE, GU_TCC_RGBA );
		sceGuColor( GUCOLOR4F( tr.blend, tr.blend, tr.blend, 1.0f ));
		sceGuBlendFunc( GU_ADD, GU_FIX, GU_FIX, GUBLEND1, GUBLEND1 );
		sceGuDepthMask( GU_TRUE );
		sceGuEnable( GU_BLEND );
		break;
	default:
		sceGuTexFunc( GU_TFX_MODULATE, GU_TCC_RGBA );
		sceGuBlendFunc( GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0 );
		sceGuColor( GUCOLOR4F( 1.0f, 1.0f, 1.0f, tr.blend ));
		sceGuDepthMask( GU_FALSE );
		sceGuEnable( GU_BLEND );
		break;
	}
}

/*
===============
GL_StudioDrawShadow

g-cont: don't modify this code it's 100% matched with
original GoldSrc code and used in some mods to enable
studio shadows with some asm tricks
===============
*/
static void GL_StudioDrawShadow( void )
{
	sceGuDepthMask( GU_FALSE );

	if( r_shadows.value && g_studio.rendermode != kRenderTransAdd && !FBitSet( RI.currentmodel->flags, STUDIO_AMBIENT_LIGHT ))
	{
		float	color = 1.0f - (tr.blend * 0.5f);

		sceGuDisable( GU_TEXTURE_2D );
		sceGuBlendFunc( GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0 );
		sceGuEnable( GU_BLEND );
		sceGuColor( GUCOLOR4F( 0.0f, 0.0f, 0.0f, 1.0f - color ));

		sceGuDepthFunc( GU_LESS );
		R_StudioDrawPointsShadow();
		sceGuDepthFunc( GU_LEQUAL );

		sceGuEnable( GU_TEXTURE_2D );
		sceGuDisable( GU_BLEND );
		sceGuColor( 0xffffffff );
		sceGuShadeModel( GU_SMOOTH );
	}
}

/*
====================
StudioRenderFinal

====================
*/
static void R_StudioRenderFinal( void )
{
	int rendermode = R_StudioGetForceFaceFlags() ? kRenderTransAdd : RI.currententity->curstate.rendermode;
	R_StudioSetupRenderer( rendermode );

	if( r_drawentities->value == 2 )
	{
		R_StudioDrawBones();
	}
	else if( r_drawentities->value == 3 )
	{
		R_StudioDrawHulls();
	}
	else
	{
		for( int i = 0; i < m_pStudioHeader->numbodyparts; i++ )
		{
			R_StudioSetupModel( i, (void**)&m_pBodyPart, (void**)&m_pSubModel );

			GL_StudioSetRenderMode( rendermode );
			R_StudioDrawPoints();
#if XASH_PSP
			if( r_shadows.value )
				R_StudioGEPlanClose();
#endif
			GL_StudioDrawShadow();
		}
#if XASH_PSP
		R_StudioGEPlanClose();
#endif
	}

	if( r_drawentities->value == 4 )
	{
		TriRenderMode( kRenderTransAdd );
		R_StudioDrawHulls( );
		TriRenderMode( kRenderNormal );
	}

	if( r_drawentities->value == 5 )
	{
		R_StudioDrawAbsBBox( );
	}

	if( r_drawentities->value == 6 )
	{
		R_StudioDrawAttachments();
	}

	if( r_drawentities->value == 7 )
	{
		vec3_t	origin;

		sceGuDisable( GU_TEXTURE_2D );
		sceGuDisable( GU_DEPTH_TEST );

		gu_vert_fcv_t *const out = extGuGetMemoryNoStall( sizeof( gu_vert_fcv_t ) * 4 );

		Matrix3x4_OriginFromMatrix( g_studio.rotationmatrix, origin );
		out[0].c = out[1].c = GU_COLOR( 1.0f, 0.5f, 0.0f, 1.0f );
		VectorCopy( origin, &out[0].x );
		VectorCopy( g_studio.lightspot, &out[1].x );

		VectorMA( g_studio.lightspot, -64.0f, g_studio.lightvec, origin );
		out[2].c = out[3].c = GU_COLOR( 0.0f, 0.5f, 1.0f, 1.0f );
		VectorCopy( g_studio.lightspot, &out[2].x );
		VectorCopy( origin, &out[3].x );

		extGuDrawArrayBatched( GU_LINES, GU_COLOR_8888 | GU_VERTEX_32BITF, 4, out );

		sceGuEnable( GU_DEPTH_TEST );
		sceGuEnable( GU_TEXTURE_2D );
	}

	R_StudioRestoreRenderer();
}

/*
====================
StudioRenderModel

====================
*/
static void R_StudioRenderModel( void )
{
	R_StudioSetChromeOrigin();
	R_StudioSetForceFaceFlags( 0 );

	if( RI.currententity->curstate.renderfx == kRenderFxGlowShell )
	{
		RI.currententity->curstate.renderfx = kRenderFxNone;

		R_StudioRenderFinal( );

		R_StudioSetForceFaceFlags( STUDIO_NF_CHROME );
		TriSpriteTexture( gEngfuncs.GetDefaultSprite( REF_CHROME_SPRITE ), 0 );
		RI.currententity->curstate.renderfx = kRenderFxGlowShell;

		R_StudioRenderFinal( );
	}
	else
	{
		R_StudioRenderFinal( );
	}
}

/*
====================
StudioEstimateGait

====================
*/
static void R_StudioEstimateGait( entity_state_t *pplayer )
{
	vec3_t	est_velocity;
	float dt = bound( 0.0f, g_studio.frametime, 1.0f );

	if( dt == 0.0f || m_pPlayerInfo->renderframe == tr.realframecount )
	{
		m_flGaitMovement = 0;
		return;
	}

	VectorSubtract( RI.currententity->origin, m_pPlayerInfo->prevgaitorigin, est_velocity );
	VectorCopy( RI.currententity->origin, m_pPlayerInfo->prevgaitorigin );
	m_flGaitMovement = VectorLength( est_velocity );

	if( dt <= 0.0f || m_flGaitMovement / dt < 5.0f )
	{
		m_flGaitMovement = 0.0f;
		est_velocity[0] = 0.0f;
		est_velocity[1] = 0.0f;
	}

	if( est_velocity[1] == 0.0f && est_velocity[0] == 0.0f )
	{
		float	flYawDiff = RI.currententity->angles[YAW] - m_pPlayerInfo->gaityaw;

		flYawDiff = flYawDiff - (int)(flYawDiff / 360) * 360;
		if( flYawDiff > 180.0f ) flYawDiff -= 360.0f;
		if( flYawDiff < -180.0f ) flYawDiff += 360.0f;

		if( dt < 0.25f )
			flYawDiff *= dt * 4.0f;
		else flYawDiff *= dt;

		m_pPlayerInfo->gaityaw += flYawDiff;
		m_pPlayerInfo->gaityaw = m_pPlayerInfo->gaityaw - (int)(m_pPlayerInfo->gaityaw / 360) * 360;

		m_flGaitMovement = 0.0f;
	}
	else
	{
		m_pPlayerInfo->gaityaw = ( atan2( est_velocity[1], est_velocity[0] ) * 180 / M_PI_F );
		if( m_pPlayerInfo->gaityaw > 180.0f ) m_pPlayerInfo->gaityaw = 180.0f;
		if( m_pPlayerInfo->gaityaw < -180.0f ) m_pPlayerInfo->gaityaw = -180.0f;
	}

}

/*
====================
StudioProcessGait

====================
*/
static void R_StudioProcessGait( entity_state_t *pplayer )
{
	if( RI.currententity->curstate.sequence >= m_pStudioHeader->numseq )
		RI.currententity->curstate.sequence = 0;

	float dt = bound( 0.0f, g_studio.frametime, 1.0f );

	mstudioseqdesc_t *pseqdesc = (mstudioseqdesc_t *)((byte *)m_pStudioHeader + m_pStudioHeader->seqindex) + RI.currententity->curstate.sequence;

	int iBlend;
	R_StudioPlayerBlend( pseqdesc, &iBlend, &RI.currententity->angles[PITCH] );

	RI.currententity->latched.prevangles[PITCH] = RI.currententity->angles[PITCH];
	RI.currententity->curstate.blending[0] = iBlend;
	RI.currententity->latched.prevblending[0] = RI.currententity->curstate.blending[0];
	RI.currententity->latched.prevseqblending[0] = RI.currententity->curstate.blending[0];
	R_StudioEstimateGait( pplayer );

	// calc side to side turning
	float flYaw = RI.currententity->angles[YAW] - m_pPlayerInfo->gaityaw;
	flYaw = flYaw - (int)(flYaw / 360) * 360;
	if( flYaw < -180.0f ) flYaw = flYaw + 360.0f;
	if( flYaw > 180.0f ) flYaw = flYaw - 360.0f;

	if( flYaw > 120.0f )
	{
		m_pPlayerInfo->gaityaw = m_pPlayerInfo->gaityaw - 180.0f;
		m_flGaitMovement = -m_flGaitMovement;
		flYaw = flYaw - 180.0f;
	}
	else if( flYaw < -120.0f )
	{
		m_pPlayerInfo->gaityaw = m_pPlayerInfo->gaityaw + 180.0f;
		m_flGaitMovement = -m_flGaitMovement;
		flYaw = flYaw + 180.0f;
	}

	// adjust torso
	RI.currententity->curstate.controller[0] = ((flYaw / 4.0f) + 30.0f) / (60.0f / 255.0f);
	RI.currententity->curstate.controller[1] = ((flYaw / 4.0f) + 30.0f) / (60.0f / 255.0f);
	RI.currententity->curstate.controller[2] = ((flYaw / 4.0f) + 30.0f) / (60.0f / 255.0f);
	RI.currententity->curstate.controller[3] = ((flYaw / 4.0f) + 30.0f) / (60.0f / 255.0f);
	RI.currententity->latched.prevcontroller[0] = RI.currententity->curstate.controller[0];
	RI.currententity->latched.prevcontroller[1] = RI.currententity->curstate.controller[1];
	RI.currententity->latched.prevcontroller[2] = RI.currententity->curstate.controller[2];
	RI.currententity->latched.prevcontroller[3] = RI.currententity->curstate.controller[3];

	RI.currententity->angles[YAW] = m_pPlayerInfo->gaityaw;
	if( RI.currententity->angles[YAW] < -0 ) RI.currententity->angles[YAW] += 360.0f;
	RI.currententity->latched.prevangles[YAW] = RI.currententity->angles[YAW];

	if( pplayer->gaitsequence >= m_pStudioHeader->numseq )
		pplayer->gaitsequence = 0;

	pseqdesc = (mstudioseqdesc_t *)((byte *)m_pStudioHeader + m_pStudioHeader->seqindex) + pplayer->gaitsequence;

	// calc gait frame
	if( pseqdesc->linearmovement[0] > 0 )
		m_pPlayerInfo->gaitframe += (m_flGaitMovement / pseqdesc->linearmovement[0]) * pseqdesc->numframes;
	else m_pPlayerInfo->gaitframe += pseqdesc->fps * dt;

	// do modulo
	m_pPlayerInfo->gaitframe = m_pPlayerInfo->gaitframe - (int)(m_pPlayerInfo->gaitframe / pseqdesc->numframes) * pseqdesc->numframes;
	if( m_pPlayerInfo->gaitframe < 0 ) m_pPlayerInfo->gaitframe += pseqdesc->numframes;
}

/*
===============
R_StudioDrawPlayer

===============
*/
static int R_StudioDrawPlayer( int flags, entity_state_t *pplayer )
{
	alight_t	lighting;
	vec3_t	dir;

	int m_nPlayerIndex = pplayer->number - 1;

	if( m_nPlayerIndex < 0 || m_nPlayerIndex >= gp_cl->maxclients )
		return 0;

	RI.currentmodel = R_StudioSetupPlayerModel( m_nPlayerIndex );
	if( RI.currentmodel == NULL )
		return 0;

	R_StudioSetHeader((studiohdr_t *)gEngfuncs.Mod_Extradata( mod_studio, RI.currentmodel ));

	if( pplayer->gaitsequence )
	{
		m_pPlayerInfo = pfnPlayerInfo( m_nPlayerIndex );
		vec3_t orig_angles = Vec3( RI.currententity->angles );

		R_StudioProcessGait( pplayer );

		m_pPlayerInfo->gaitsequence = pplayer->gaitsequence;
		m_pPlayerInfo = NULL;

		R_StudioSetUpTransform( RI.currententity );
		VectorCopy( orig_angles, RI.currententity->angles );
	}
	else
	{
		RI.currententity->curstate.controller[0] = 127;
		RI.currententity->curstate.controller[1] = 127;
		RI.currententity->curstate.controller[2] = 127;
		RI.currententity->curstate.controller[3] = 127;
		RI.currententity->latched.prevcontroller[0] = RI.currententity->curstate.controller[0];
		RI.currententity->latched.prevcontroller[1] = RI.currententity->curstate.controller[1];
		RI.currententity->latched.prevcontroller[2] = RI.currententity->curstate.controller[2];
		RI.currententity->latched.prevcontroller[3] = RI.currententity->curstate.controller[3];

		m_pPlayerInfo = pfnPlayerInfo( m_nPlayerIndex );
		m_pPlayerInfo->gaitsequence = 0;

		R_StudioSetUpTransform( RI.currententity );
	}

	if( flags & STUDIO_RENDER )
	{
		// see if the bounding box lets us trivially reject, also sets
		if( !R_StudioCheckBBox( ))
			return 0;

		r_stats.c_studio_models_drawn++;
		g_studio.framecount++; // render data cache cookie

		if( m_pStudioHeader->numbodyparts == 0 )
			return 1;
	}

	m_pPlayerInfo = pfnPlayerInfo( m_nPlayerIndex );
	R_StudioSetupBones( RI.currententity );
	R_StudioSaveBones( );

	m_pPlayerInfo->renderframe = tr.realframecount;
	m_pPlayerInfo = NULL;

	if( flags & STUDIO_EVENTS )
	{
		R_StudioCalcAttachments( );
		R_StudioClientEvents( );

		// copy attachments into global entity array
		if( RI.currententity->index > 0 )
		{
			cl_entity_t *ent = CL_GetEntityByIndex( RI.currententity->index );
			memcpy( ent->attachment, RI.currententity->attachment, sizeof( vec3_t ) * 4 );
		}
	}

	if( flags & STUDIO_RENDER )
	{
		// change body if it's a menu entity
		if( cl_himodels->value && ( RI.currentmodel != RI.currententity->model || !FBitSet( RI.rvp.flags, RF_DRAW_WORLD )))
		{
			// show highest resolution multiplayer model
			RI.currententity->curstate.body = 255;
		}

		if( !( !gpGlobals->developer && gp_cl->maxclients == 1 ) && ( RI.currentmodel == RI.currententity->model ))
			RI.currententity->curstate.body = 1; // force helmet

		lighting.plightvec = dir;
		R_EntityDynamicLight( RI.currententity, &lighting, FBitSet( RI.rvp.flags, RF_DRAW_WORLD ), g_studio.time, g_studio.lightspot, g_studio.lightvec );

		R_StudioEntityLight( &lighting );

		// model and frame independant
		R_StudioSetupLighting( &lighting );

		m_pPlayerInfo = pfnPlayerInfo( m_nPlayerIndex );

		// get remap colors
		g_nTopColor = m_pPlayerInfo->topcolor;
		g_nBottomColor = m_pPlayerInfo->bottomcolor;

		if( g_nTopColor < 0 ) g_nTopColor = 0;
		if( g_nTopColor > 360 ) g_nTopColor = 360;
		if( g_nBottomColor < 0 ) g_nBottomColor = 0;
		if( g_nBottomColor > 360 ) g_nBottomColor = 360;

		R_StudioSetRemapColors( g_nTopColor, g_nBottomColor );

		R_StudioRenderModel( );
		m_pPlayerInfo = NULL;

		if( pplayer->weaponmodel )
		{
			cl_entity_t	saveent = *RI.currententity;
			model_t		*pweaponmodel = CL_ModelHandle( pplayer->weaponmodel );

			m_pStudioHeader = (studiohdr_t *)gEngfuncs.Mod_Extradata( mod_studio, pweaponmodel );

			R_StudioMergeBones( RI.currententity, pweaponmodel );
			R_StudioSetupLighting( &lighting );
			R_StudioRenderModel( );
			R_StudioCalcAttachments( );

			*RI.currententity = saveent;
		}
	}

	return 1;
}

#if XASH_PSP
/*
===============
R_StudioViewModelBones

the view model's bones are set up twice a frame, for its events
(R_RunViewmodelEvents) and to draw it, from the same state: the second
time they are copied from the first (25 bones, ~100 us)
===============
*/
static struct
{
	qboolean		valid;
	const studiohdr_t	*hdr;
	matrix3x4		rotation;
	double		time;
	float		frame, animtime, framerate, sequencetime, prevframe;
	int		sequence, prevsequence;
	byte		controller[4], blending[2];
	matrix3x4		bones[MAXSTUDIOBONES];
} r_vmbones;

static qboolean R_StudioViewModelBonesMatch( const cl_entity_t *e )
{
	return r_vmbones.valid && r_vmbones.hdr == m_pStudioHeader && r_vmbones.time == g_studio.time
		&& r_vmbones.frame == e->curstate.frame && r_vmbones.animtime == e->curstate.animtime
		&& r_vmbones.framerate == e->curstate.framerate && r_vmbones.sequence == e->curstate.sequence
		&& r_vmbones.prevsequence == e->latched.prevsequence && r_vmbones.sequencetime == e->latched.sequencetime
		&& r_vmbones.prevframe == e->latched.prevframe
		&& !memcmp( r_vmbones.controller, e->curstate.controller, sizeof( r_vmbones.controller ))
		&& !memcmp( r_vmbones.blending, e->curstate.blending, sizeof( r_vmbones.blending ))
		&& !memcmp( r_vmbones.rotation, g_studio.rotationmatrix, sizeof( r_vmbones.rotation ));
}

static void R_StudioViewModelBones( cl_entity_t *e )
{
	const size_t size = m_pStudioHeader->numbones * sizeof( matrix3x4 );

	if( R_StudioViewModelBonesMatch( e ))
	{
		memcpy( g_studio.bonestransform, r_vmbones.bones, size );
		memcpy( g_studio.lighttransform, r_vmbones.bones, size );
		return;
	}

	R_StudioSetupBones( e );

	// with these effects the light transforms differ
	r_vmbones.valid = e->curstate.renderfx != kRenderFxDistort && e->curstate.renderfx != kRenderFxHologram && e->curstate.renderfx != kRenderFxExplode;
	if( !r_vmbones.valid )
		return;

	r_vmbones.hdr = m_pStudioHeader;
	r_vmbones.time = g_studio.time;
	r_vmbones.frame = e->curstate.frame;
	r_vmbones.animtime = e->curstate.animtime;
	r_vmbones.framerate = e->curstate.framerate;
	r_vmbones.sequence = e->curstate.sequence;
	r_vmbones.prevsequence = e->latched.prevsequence;
	r_vmbones.sequencetime = e->latched.sequencetime;
	r_vmbones.prevframe = e->latched.prevframe; // as R_StudioSetupBones left it
	memcpy( r_vmbones.controller, e->curstate.controller, sizeof( r_vmbones.controller ));
	memcpy( r_vmbones.blending, e->curstate.blending, sizeof( r_vmbones.blending ));
	Matrix3x4_Copy( r_vmbones.rotation, g_studio.rotationmatrix );
	memcpy( r_vmbones.bones, g_studio.bonestransform, size );
}
#endif

/*
===============
R_StudioDrawModel

===============
*/
#if XASH_PSP
/*
=================
static props

A prop (a model of at most 2 bones and no controllers in a sequence of one
frame or one that doesn't play: items, weapons on the floor, shells and gibs
at rest) that stays where it is keeps its bones and its
light: the bone setup and the light traces run again only when it moves,
changes, or every 0.25 s for light styles and dynamic lights. fight1: the
props were ~2.5 ms a frame.
=================
*/
static CVAR_DEFINE_AUTO( r_studio_propcache, "1", FCVAR_ARCHIVE, "keep the bones and light of studio models that don't move or animate between frames" );

#define PROP_MAXBONES	2
#define PROP_SLOTS	64
#define PROP_LIGHT_TIME	0.25

typedef struct
{
	const cl_entity_t	*e;
	const model_t	*model;
	int		sequence, body, skin;
	float		frame;
	double		time;		// of the light
	matrix3x4		rotation;
	matrix3x4		bones[PROP_MAXBONES];
	matrix3x4		lights[PROP_MAXBONES];
	vec3_t		blightvec[PROP_MAXBONES];
	float		ambientlight, shadelight;
	vec3_t		lightvec, lightcolor, lightspot;
	qboolean		valid;

	// R_StudioPropRecord: the GE commands of the whole model, bones relative to its origin
	uint		*rec;		// the light ramp, the light blocks, then the commands
	uint		rec_cap;		// words
	const uint	*rec_main;	// what to call
	const uint	*rec_lastskin;	// for R_StudioGERestore
	uint		rec_gen;		// ge_rec_gen it was recorded in, 0: none
	uint		rec_frame;	// guRender.vert_frame it was last called in
	double		rec_time;		// the light it was recorded with
	vec3_t		rec_entorigin, rec_entangles;
	vec3_t		rec_origin;	// the bones are relative to it
	vec3_t		rec_absmin, rec_absmax;
	vec3_t		rec_attachment[4];
	int		rec_polys;
} propcache_t;

static propcache_t r_propcache[PROP_SLOTS];

// the slot of this entity if it may be cached at all; *hit when its state is still good
static propcache_t *R_StudioPropCache( cl_entity_t *e, qboolean *hit )
{
	*hit = false;
	if( !r_studio_propcache.value || e == tr.viewent || m_pStudioHeader->numbones > PROP_MAXBONES || m_pStudioHeader->numbones <= 0 )
		return NULL;
	if( e->curstate.movetype == MOVETYPE_FOLLOW || e->curstate.renderfx != kRenderFxNone || e->curstate.sequence >= m_pStudioHeader->numseq )
		return NULL;

	// one pose: a sequence of one frame, or one that doesn't play (shells, some items)
	const mstudioseqdesc_t *pseqdesc = (mstudioseqdesc_t *)((byte *)m_pStudioHeader + m_pStudioHeader->seqindex) + e->curstate.sequence;
	if(( pseqdesc->numframes > 1 && e->curstate.framerate != 0.0f ) || m_pStudioHeader->numbonecontrollers > 0 )
		return NULL;

	// two slots to choose from: this entity's, else the free or older one
	const uint h = ((uint)(uintptr_t)e * 2654435761u ) >> 26;
	propcache_t *pc = &r_propcache[h], *pc2 = &r_propcache[( h + 1 ) & ( PROP_SLOTS - 1 )];

	if( pc->e != e && ( pc2->e == e || ( pc->valid && ( !pc2->valid || pc2->time < pc->time ))))
		pc = pc2;

	if( pc->valid && pc->e == e && pc->model == RI.currentmodel && pc->sequence == e->curstate.sequence
		&& pc->body == e->curstate.body && pc->skin == e->curstate.skin && pc->frame == e->curstate.frame
		&& g_studio.time >= pc->time && g_studio.time < pc->time + PROP_LIGHT_TIME
		&& !memcmp( pc->rotation, g_studio.rotationmatrix, sizeof( matrix3x4 )))
		*hit = true;
	return pc;
}

static void R_StudioPropRestore( const propcache_t *pc )
{
	const int n = m_pStudioHeader->numbones;

	memcpy( g_studio.bonestransform, pc->bones, n * sizeof( matrix3x4 ));
	memcpy( g_studio.lighttransform, pc->lights, n * sizeof( matrix3x4 ));
	memcpy( g_studio.blightvec, pc->blightvec, n * sizeof( vec3_t ));
	g_studio.ambientlight = pc->ambientlight;
	g_studio.shadelight = pc->shadelight;
	VectorCopy( pc->lightvec, g_studio.lightvec );
	VectorCopy( pc->lightcolor, g_studio.lightcolor );
	VectorCopy( pc->lightspot, g_studio.lightspot );
	g_studio.numlocallights = 0;
}

static void R_StudioPropStore( propcache_t *pc, cl_entity_t *e )
{
	const int n = m_pStudioHeader->numbones;

	// entity lights take the per-vertex path: not worth keeping
	pc->valid = !g_studio.numlocallights;
	if( !pc->valid )
		return;
	pc->e = e;
	pc->model = RI.currentmodel;
	pc->sequence = e->curstate.sequence;
	pc->body = e->curstate.body;
	pc->skin = e->curstate.skin;
	pc->frame = e->curstate.frame;
	pc->time = g_studio.time;
	memcpy( pc->rotation, g_studio.rotationmatrix, sizeof( matrix3x4 ));
	memcpy( pc->bones, g_studio.bonestransform, n * sizeof( matrix3x4 ));
	memcpy( pc->lights, g_studio.lighttransform, n * sizeof( matrix3x4 ));
	memcpy( pc->blightvec, g_studio.blightvec, n * sizeof( vec3_t ));
	pc->ambientlight = g_studio.ambientlight;
	pc->shadelight = g_studio.shadelight;
	VectorCopy( g_studio.lightvec, pc->lightvec );
	VectorCopy( g_studio.lightcolor, pc->lightcolor );
	VectorCopy( g_studio.lightspot, pc->lightspot );
}

static void R_StudioPropCacheClear( void )
{
	for( int i = 0; i < PROP_SLOTS; i++ )
	{
		// the GE may still be drawing a frame that calls them: keep the memory
		uint *rec = r_propcache[i].rec;
		uint cap = r_propcache[i].rec_cap;

		memset( &r_propcache[i], 0, sizeof( r_propcache[i] ));
		r_propcache[i].rec = rec;
		r_propcache[i].rec_cap = cap;
	}
	ge_rec_gen++;
}

/*
=================
recorded props

A cached prop that stays still is drawn from its GE commands, recorded once
from the bones and light the prop cache keeps: the bones relative to its
origin, its own light ramp and light blocks, calls of the persistent skin
blocks and of the batches. Drawing it is then a box test, a view matrix with
its origin in the translation and one call: ~30 us, against ~150 us of
mostly cache misses on the whole studio path. Recorded when the prop cache
hits (the prop stayed still for a frame, so moving ones never are), and so
again after the cached light runs out (PROP_LIGHT_TIME), the prop moved or
changed, or a skin block or GE model changed.
=================
*/
static CVAR_DEFINE_AUTO( r_studio_proplist, "1", FCVAR_ARCHIVE, "draw props that keep their bones and light from GE commands recorded once" );

#define PROP_LIGHTBLOCK_MAX	64	// words, the light block is ~35
#define PROP_MAXITEMS	32

typedef struct
{
	const gebatch_t	*batches;
	int		numbatches;
	const uint	*skin;
	int		flat;
	qboolean		newsubmodel;	// the GE bone slots start over
} propitem_t;

// the skin block R_StudioGESkinBlock keeps, not this frame's copy of it
static const uint *R_StudioPropSkinBlock( const mstudiotexture_t *ptex, int skinindex, int bindtex )
{
	const int texnum = bindtex ? bindtex : ptex->index;
	const uint *block = R_StudioGESkinBlock( ptex, skinindex, bindtex );

	if( !ge_skins || texnum <= 0 || texnum >= MAX_TEXTURES || block != ge_skins[texnum].block )
		return NULL;
	return block;
}

// a copy of a light block at *p, NULL if it doesn't fit before end
static const uint *R_StudioPropLightBlock( uint **p, const uint *end, const uint *ramp, qboolean flat )
{
	const uint *block = R_StudioGEBuildLightBlock( ramp, flat );
	const uint words = extGuCmdBegin() - block; // up to and including the RET

	if( words > PROP_LIGHTBLOCK_MAX || *p + words > end )
		return NULL;

	uint *dst = *p;
	memcpy( dst, block, words * sizeof( uint ));
	*p += words;
	return dst;
}

/*
what R_StudioSetUpTransform builds the transform from: the entity's origin and
angles, for MOVETYPE_STEP the ones it interpolates between, which have to be
the same (it doesn't move). false when it moves.
*/
static qboolean R_StudioPropPose( const cl_entity_t *e, vec3_t origin, vec3_t angles )
{
	if( e->curstate.movetype != MOVETYPE_STEP || FBitSet( gp_host->features, ENGINE_COMPUTE_STUDIO_LERP ))
	{
		VectorCopy( e->origin, origin );
		VectorCopy( e->angles, angles );
		return true;
	}

	if( !VectorCompare( e->latched.prevorigin, e->curstate.origin ) || !VectorCompare( e->latched.prevangles, e->curstate.angles ))
		return false;

	VectorCopy( e->curstate.origin, origin );
	VectorCopy( e->curstate.angles, angles );
	return true;
}

static void R_StudioPropRecordCommands( propcache_t *pc, cl_entity_t *e )
{
	propitem_t items[PROP_MAXITEMS];
	int numitems = 0, polys = 0;

	pc->rec_gen = 0;
	// not while the GE may be reading them
	if( !r_studio_proplist.value || !pc->valid || pc->rec_frame + 1 >= guRender.vert_frame )
		return;
	if( m_fDoRemap || g_studio.rendermode != kRenderNormal || r_drawentities->value != 1 || e->player || !R_StudioPropPose( e, pc->rec_entorigin, pc->rec_entangles ))
		return;
	if( FBitSet( m_pStudioHeader->flags, STUDIO_HAS_BONEINFO ) || !R_StudioGEAllowed( ))
		return;

	int skinnum = e->curstate.skin;

	if( skinnum <= 0 || skinnum >= m_pStudioHeader->numskinfamilies )
		skinnum = 0;

	// what R_StudioDrawPoints draws for each body part, if the GE draws all of it
	for( int i = 0; i < m_pStudioHeader->numbodyparts; i++ )
	{
		R_StudioSetupModel( i, (void **)&m_pBodyPart, (void **)&m_pSubModel );
		if( m_pSubModel->nummesh <= 0 )
			continue;

		gemodel_t *gm = R_StudioGEGetModel();
		if( !gm || gm->cpumeshes )
			return;

		const mstudiotexture_t *ptexture = (const mstudiotexture_t *)((const byte *)m_pStudioHeader + m_pStudioHeader->textureindex);
		const short *pskinref = (const short *)((const byte *)m_pStudioHeader + m_pStudioHeader->skinindex) + skinnum * m_pStudioHeader->numskinref;
		const mstudiomesh_t *pmesh = (const mstudiomesh_t *)((const byte *)m_pStudioHeader + m_pSubModel->meshindex);
		const gemerge_t *merge = r_studio_atlas.value ? R_StudioGEGetMerge( gm, skinnum, ptexture, pskinref ) : NULL;
		qboolean first = true;

		for( int g = 0; merge && g < merge->numgroups; g++ )
		{
			if( numitems >= PROP_MAXITEMS )
				return;
			propitem_t *it = &items[numitems++];
			it->batches = merge->groups[g].batches;
			it->numbatches = merge->groups[g].numbatches;
			it->skin = R_StudioPropSkinBlock( &ge_atlas_ptex, 0, merge->groups[g].atlas );
			it->flat = false;
			it->newsubmodel = first;
			first = false;
			if( !it->skin )
				return;
		}

		for( int j = 0; j < m_pSubModel->nummesh; j++ )
		{
			polys += pmesh[j].numtris;
			if( merge && merge->merged[j] )
				continue;

			const mstudiotexture_t *ptex = &ptexture[pskinref[pmesh[j].skinref]];
			if( FBitSet( ptex->flags, GE_UNSUPPORTED_FLAGS|STUDIO_NF_MASKED|STUDIO_NF_ADDITIVE|STUDIO_NF_CHROME ))
				return;
			if( !gm->meshes[j].numbatches || numitems >= PROP_MAXITEMS )
				return;

			propitem_t *it = &items[numitems++];
			it->batches = gm->meshes[j].batches;
			it->numbatches = gm->meshes[j].numbatches;
			it->skin = R_StudioPropSkinBlock( ptex, pskinref[pmesh[j].skinref], 0 );
			it->flat = FBitSet( ptex->flags, STUDIO_NF_FLATSHADE ) ? 1 : 0;
			it->newsubmodel = first;
			first = false;
			if( !it->skin )
				return;
		}
	}

	// the ramp, two light blocks, then per batch its bones and four calls, and a RET
	uint need = GE_RAMP_SIZE + 2 * PROP_LIGHTBLOCK_MAX + 1;
	for( int i = 0; i < numitems; i++ )
	{
		for( int k = 0; k < items[i].numbatches; k++ )
			need += items[i].batches[k].numbones * 13 + 8;
	}

	if( need > pc->rec_cap )
	{
		// not called this frame (rec_frame), and the GE is done with the last one
		free( pc->rec );
		pc->rec = memalign( 16, need * sizeof( uint ));
		pc->rec_cap = pc->rec ? need : 0;
		if( !pc->rec )
			return;
	}

	uint *ramp = pc->rec, *p = pc->rec + GE_RAMP_SIZE;
	const uint *end = pc->rec + pc->rec_cap;
	const uint *light[2] = { NULL, NULL };

	R_StudioGERampFill( ramp );
	for( int i = 0; i < numitems; i++ )
	{
		const int flat = items[i].flat;
		if( !light[flat] && !( light[flat] = R_StudioPropLightBlock( &p, end, ramp, flat )))
			return;
	}

	vec3_t origin;
	Matrix3x4_OriginFromMatrix( g_studio.rotationmatrix, origin );

	pc->rec_main = p;
	pc->rec_lastskin = NULL;
	for( int i = 0; i < numitems; i++ )
	{
		const propitem_t *it = &items[i];

		if( it->newsubmodel )
			ge_slots_valid = 0;
		for( int k = 0; k < it->numbatches; k++ )
		{
			const gebatch_t *b = &it->batches[k];

			p = R_StudioGEBones( p, b, origin );
			p = R_StudioGECall( p, it->skin );
			p = R_StudioGECall( p, b->cmds );
			p = R_StudioGECall( p, light[it->flat] );
			p = R_StudioGECall( p, b->cmds );
		}
		pc->rec_lastskin = it->skin;
	}
	*p++ = GE_CMD( 0x0B, 0 ); // RET
	sceKernelDcacheWritebackRange( pc->rec, ( p - pc->rec ) * sizeof( uint ));

	vec3_t bbox[8];
	R_StudioComputeBBox( bbox );
	ClearBounds( pc->rec_absmin, pc->rec_absmax );
	for( int i = 0; i < 8; i++ )
		AddPointToBounds( bbox[i], pc->rec_absmin, pc->rec_absmax );

	VectorCopy( origin, pc->rec_origin );
	memcpy( pc->rec_attachment, e->attachment, sizeof( pc->rec_attachment ));
	pc->rec_polys = polys;
	pc->rec_time = pc->time;
	pc->rec_gen = ge_rec_gen;
}

static void R_StudioPropRecord( propcache_t *pc, cl_entity_t *e )
{
	const uint savedtex = glState.currentTexture; // the skin block lookups set it

	pc->rec_gen = 0;
	R_StudioPropRecordCommands( pc, e );
	ge_slots_valid = 0;
	glState.currentTexture = savedtex;
}

// the view matrix with the prop's origin in the translation: its bones are relative to it
static uint *R_StudioPropViewMatrix( uint *p, const vec3_t org )
{
	static ScePspFMatrix4 view;
	static matrix4x4 from;
	static uint frame = ~0u;

	if( frame != guRender.vert_frame || memcmp( from, RI.worldviewMatrix, sizeof( from )))
	{
		Matrix4x4_ToFMatrix4( RI.worldviewMatrix, &view );
		memcpy( from, RI.worldviewMatrix, sizeof( from ));
		frame = guRender.vert_frame;
	}

	const float t[3] =
	{
		view.x.x * org[0] + view.y.x * org[1] + view.z.x * org[2] + view.w.x,
		view.x.y * org[0] + view.y.y * org[1] + view.z.y * org[2] + view.w.y,
		view.x.z * org[0] + view.y.z * org[1] + view.z.z * org[2] + view.w.z,
	};

	// as sceGuSetMatrix( GU_VIEW ): VMS, then the 4 columns of 3
	*p++ = GE_CMD( 0x3C, 0 );
	*p++ = GE_CMD( 0x3D, GE_Float( view.x.x ));
	*p++ = GE_CMD( 0x3D, GE_Float( view.x.y ));
	*p++ = GE_CMD( 0x3D, GE_Float( view.x.z ));
	*p++ = GE_CMD( 0x3D, GE_Float( view.y.x ));
	*p++ = GE_CMD( 0x3D, GE_Float( view.y.y ));
	*p++ = GE_CMD( 0x3D, GE_Float( view.y.z ));
	*p++ = GE_CMD( 0x3D, GE_Float( view.z.x ));
	*p++ = GE_CMD( 0x3D, GE_Float( view.z.y ));
	*p++ = GE_CMD( 0x3D, GE_Float( view.z.z ));
	*p++ = GE_CMD( 0x3D, GE_Float( t[0] ));
	*p++ = GE_CMD( 0x3D, GE_Float( t[1] ));
	*p++ = GE_CMD( 0x3D, GE_Float( t[2] ));
	return p;
}

// true when the entity was handled from its recorded commands (drawn or culled)
// the slot of a prop that can be drawn from its recorded commands
static propcache_t *R_StudioPropRecorded( const cl_entity_t *e )
{
	if( !r_studio_proplist.value || !r_studio_propcache.value || e->player || e == tr.viewent )
		return NULL;

	const uint h = ((uint)(uintptr_t)e * 2654435761u ) >> 26;
	propcache_t *pc = &r_propcache[h];

	if( pc->e != e && ( pc = &r_propcache[( h + 1 ) & ( PROP_SLOTS - 1 )] )->e != e )
		return NULL;

	if( !pc->valid || pc->rec_gen != ge_rec_gen || pc->rec_time != pc->time || pc->rec_frame == guRender.vert_frame )
		return NULL;
	if( pc->model != e->model || pc->sequence != e->curstate.sequence || pc->body != e->curstate.body || pc->skin != e->curstate.skin || pc->frame != e->curstate.frame )
		return NULL;
	if( g_studio.time < pc->time || g_studio.time >= pc->time + PROP_LIGHT_TIME )
		return NULL;

	vec3_t origin, angles;
	if( !R_StudioPropPose( e, origin, angles ) || !VectorCompare( pc->rec_entorigin, origin ) || !VectorCompare( pc->rec_entangles, angles ))
		return NULL;
	if( e->curstate.movetype == MOVETYPE_FOLLOW || e->curstate.renderfx != kRenderFxNone || e->curstate.rendermode != kRenderNormal )
		return NULL;
	if( glState.isFogEnabled || r_lightmap->value || r_fullbright->value || !r_studio_ge.value || r_drawentities->value != 1 || !r_studio_builtin_renderer.value || !FBitSet( RI.rvp.flags, RF_DRAW_WORLD ))
		return NULL;

	return pc;
}

static qboolean R_StudioPropDrawRecorded( cl_entity_t *e )
{
	propcache_t *pc = R_StudioPropRecorded( e );

	if( !pc )
		return false;

	pc->rec_frame = guRender.vert_frame;
	if( R_CullBox( pc->rec_absmin, pc->rec_absmax ))
		return true;

	r_stats.c_studio_models_drawn++;
	r_stats.c_studio_polys += pc->rec_polys;
	g_studio.framecount++;

	memcpy( e->attachment, pc->rec_attachment, sizeof( pc->rec_attachment ));
	if( e->index > 0 )
	{
		cl_entity_t *ent = CL_GetEntityByIndex( e->index );
		if( ent && ent != e )
			memcpy( ent->attachment, pc->rec_attachment, sizeof( pc->rec_attachment ));
	}

	// R_StudioSetupRenderer, R_StudioDrawPoints and R_StudioRestoreRenderer for it
	g_studio.rendermode = kRenderNormal;
	sceGuTexFunc( GU_TFX_MODULATE, GU_TCC_RGBA );
	sceGuDisable( GU_ALPHA_TEST );
	sceGuShadeModel( GU_SMOOTH );
	if( glState.faceCull != GL_NONE )
	{
		tr.fFlipViewModel = false;
		GL_Cull( GL_FRONT );
	}

	uint *p = R_StudioPropViewMatrix( extGuCmdBegin(), pc->rec_origin );
	p = R_StudioGECall( p, pc->rec_main );
	extGuCmdEnd( p );
	glState.texIdentityMatrix = false;

	ge_last_skinblock = pc->rec_lastskin;
	R_StudioGERestore();
	R_StudioGEViewMatrix( false );
	extGuUpdateStall();

	sceGuTexFunc( GU_TFX_REPLACE, GU_TCC_RGBA );
	sceGuShadeModel( GU_FLAT );
	m_fDoRemap = false;
	return true;
}
#endif

static int R_StudioDrawModel( int flags )
{
	alight_t	lighting;
	vec3_t	dir;

	if( RI.currententity->curstate.renderfx == kRenderFxDeadPlayer )
	{
		if( RI.currententity->curstate.renderamt <= 0 ||
			RI.currententity->curstate.renderamt > gp_cl->maxclients )
			return 0;

		// get copy of player
		entity_state_t deadplayer = *R_StudioGetPlayerState( RI.currententity->curstate.renderamt - 1 );

		// clear weapon, movement state
		deadplayer.number = RI.currententity->curstate.renderamt;
		deadplayer.weaponmodel = 0;
		deadplayer.gaitsequence = 0;

		deadplayer.movetype = MOVETYPE_NONE;
		VectorCopy( RI.currententity->curstate.angles, deadplayer.angles );
		VectorCopy( RI.currententity->curstate.origin, deadplayer.origin );

		g_studio.interpolate = false;
		int result = R_StudioDrawPlayer( flags, &deadplayer ); // draw as though it were a player
		g_studio.interpolate = true;

		return result;
	}

	R_StudioSetHeader((studiohdr_t *)gEngfuncs.Mod_Extradata( mod_studio, RI.currentmodel ));

	R_StudioSetUpTransform( RI.currententity );

	if( flags & STUDIO_RENDER )
	{
		// see if the bounding box lets us trivially reject, also sets
		if( !R_StudioCheckBBox( ))
			return 0;

		r_stats.c_studio_models_drawn++;
		g_studio.framecount++; // render data cache cookie

		if( m_pStudioHeader->numbodyparts == 0 )
			return 1;
	}

#if XASH_PSP
	qboolean prop_hit = false;
	propcache_t *prop = FBitSet( flags, STUDIO_RENDER ) ? R_StudioPropCache( RI.currententity, &prop_hit ) : NULL;

	if( prop_hit )
	{
		R_StudioPropRestore( prop );
		R_StudioSaveBones();

		// attachments only: one frame, no events to fire
		if( flags & STUDIO_EVENTS )
		{
			R_StudioCalcAttachments( );
			if( RI.currententity->index > 0 )
			{
				cl_entity_t *ent = CL_GetEntityByIndex( RI.currententity->index );
				memcpy( ent->attachment, RI.currententity->attachment, sizeof( vec3_t ) * 4 );
			}
		}

		g_nTopColor = RI.currententity->curstate.colormap & 0xFF;
		g_nBottomColor = (RI.currententity->curstate.colormap & 0xFF00) >> 8;
		R_StudioSetRemapColors( g_nTopColor, g_nBottomColor );
		R_StudioRenderModel();

		// it stayed still since the last frame: draw it from recorded commands next time
		if( flags & STUDIO_EVENTS )
			R_StudioPropRecord( prop, RI.currententity );
		return 1;
	}
#endif

	if( RI.currententity->curstate.movetype == MOVETYPE_FOLLOW )
		R_StudioMergeBones( RI.currententity, RI.currentmodel );
#if XASH_PSP
	else if( RI.currententity == tr.viewent )
		R_StudioViewModelBones( RI.currententity );
#endif
	else R_StudioSetupBones( RI.currententity );

	R_StudioSaveBones();

	if( flags & STUDIO_EVENTS )
	{
		R_StudioCalcAttachments( );
		R_StudioClientEvents( );

		// copy attachments into global entity array
		if( RI.currententity->index > 0 )
		{
			cl_entity_t *ent = CL_GetEntityByIndex( RI.currententity->index );
			memcpy( ent->attachment, RI.currententity->attachment, sizeof( vec3_t ) * 4 );
		}
	}

	if( flags & STUDIO_RENDER )
	{
		lighting.plightvec = dir;
		R_EntityDynamicLight( RI.currententity, &lighting, FBitSet( RI.rvp.flags, RF_DRAW_WORLD ), g_studio.time, g_studio.lightspot, g_studio.lightvec );

		R_StudioEntityLight( &lighting );

		// model and frame independant
		R_StudioSetupLighting( &lighting );

		// get remap colors
		g_nTopColor = RI.currententity->curstate.colormap & 0xFF;
		g_nBottomColor = (RI.currententity->curstate.colormap & 0xFF00) >> 8;

		R_StudioSetRemapColors( g_nTopColor, g_nBottomColor );

#if XASH_PSP
		if( prop )
			R_StudioPropStore( prop, RI.currententity );
#endif

		R_StudioRenderModel();
	}

	return 1;
}

/*
=================
R_StudioDrawModelInternal
=================
*/
static void R_StudioDrawModelInternal( cl_entity_t *e, int flags )
{
	if( !FBitSet( RI.rvp.flags, RF_DRAW_WORLD ))
	{
		if( e->player )
			R_StudioDrawPlayer( flags, &e->curstate );
		else R_StudioDrawModel( flags );
	}
	else if( unlikely( r_studio_builtin_renderer.value ))
	{
		if( e->player )
			R_StudioDrawPlayer( flags, R_StudioGetPlayerState( e->index - 1 ));
		else R_StudioDrawModel( flags );
	}
	else
	{
		// select the properly method
		if( e->player )
			pStudioDraw->StudioDrawPlayer( flags, R_StudioGetPlayerState( e->index - 1 ));
		else pStudioDraw->StudioDrawModel( flags );
	}
}

static cl_entity_t *R_FindParentEntity( cl_entity_t *e, cl_entity_t **entities, uint num_entities )
{
	for( uint i = 0; i < num_entities; i++ )
	{
		if( entities[i]->index == e->curstate.aiment )
			return entities[i];
	}

	return NULL;
}

#if XASH_PSP
/*
=================
R_StudioPreCull

A frustum test before the client DLL's StudioDrawModel: in a firefight ~60%
of the studio models in the PVS are off screen (gibs, shells, grunts behind
the player), and the client only finds that out after its header, transform
(sines, matrices) and bbox calls. A sphere around everything
R_StudioComputeBBox could see (model box and the current sequence's box, in
any rotation), so it only skips what the client would cull too, which also
skips its events: StudioDrawModel returns before them for culled models.
=================
*/
static CVAR_DEFINE_AUTO( r_studio_precull, "1", FCVAR_ARCHIVE, "cull studio models off screen before the client renderer sees them" );
static CVAR_DEFINE_AUTO( r_studio_minpixels, "2", FCVAR_ARCHIVE, "skip studio models whose bounding radius is less than this many pixels on screen (0 = draw all)" );
static CVAR_DEFINE_AUTO( r_studio_minpixels_small, "6", FCVAR_ARCHIVE, "r_studio_minpixels for models of at most 2 bones: shells, gibs, items, weapons on the floor" );

// per frame values of the test, set by R_StudioPreCullList
static struct
{
	qboolean	active;	// the draw lists were tested: R_DrawStudioModel_ skips its own test
	qboolean	enabled;
	float	minpixels;	// 0 = no screen size test
	float	minpixels_small;	// for models of at most 2 bones
	float	px_scale;	// pixels on screen of one unit at distance 1
	float	time;
} g_precull;

static qboolean R_StudioPreCull( cl_entity_t *e )
{
	const model_t *mod = e->model;

	if( !g_precull.enabled || !mod || mod->type != mod_studio )
		return false;
	if( !FBitSet( RI.rvp.flags, RF_DRAW_WORLD ) || e == tr.viewent || e->curstate.renderfx == kRenderFxDeadPlayer )
		return false;

	const studiohdr_t *hdr = (const studiohdr_t *)gEngfuncs.Mod_Extradata( mod_studio, (model_t *)mod );
	if( !hdr || hdr->numseq <= 0 )
		return false;

	int seq = e->curstate.sequence;
	if( seq < 0 || seq >= hdr->numseq )
		seq = 0;
	const mstudioseqdesc_t *pseqdesc = (const mstudioseqdesc_t *)((const byte *)hdr + hdr->seqindex) + seq;

	// the farthest corner of the union box from the origin
	float r2 = 0.0f;
	for( int i = 0; i < 3; i++ )
	{
		float lo = pseqdesc->bbmin[i], hi = pseqdesc->bbmax[i];

		if( !VectorIsNull( mod->mins ) && !VectorIsNull( mod->maxs ))
		{
			lo = Q_min( lo, mod->mins[i] );
			hi = Q_max( hi, mod->maxs[i] );
		}
		const float m = Q_max( fabsf( lo ), fabsf( hi ));
		r2 += m * m;
	}

	float r = sqrtf( r2 );
	if( e->curstate.scale > 1.0f )
		r *= e->curstate.scale;

	// too small on screen to make out (a shell far away is a pixel or two, and costs as
	// much as a big prop): skip it. Radius r over the distance, in pixels. Small rigid
	// models (~100 us each, shells, gibs and items) go sooner than animated ones
	const float min = hdr->numbones <= 2 ? g_precull.minpixels_small : g_precull.minpixels;
	if( min > 0.0f )
	{
		vec3_t d;
		VectorSubtract( e->origin, RI.rvp.vieworigin, d );

		const float px = r * g_precull.px_scale;

		if( px * px < min * min * DotProduct( d, d ))
			return true;
	}

	// StudioSetUpTransform moves MOVETYPE_STEP models by ( origin - prevorigin ) * ( f - 1 ),
	// f = ( time - animtime ) / ( animtime - prevanimtime ) while animtime is less than 1 s old
	if( e->curstate.movetype == MOVETYPE_STEP )
	{
		const float t = g_precull.time;
		float f = 0.0f;

		if( t < e->curstate.animtime + 1.0f && e->curstate.animtime != e->latched.prevanimtime )
			f = ( t - e->curstate.animtime ) / ( e->curstate.animtime - e->latched.prevanimtime );

		vec3_t d;
		VectorSubtract( e->origin, e->latched.prevorigin, d );
		r += sqrtf( DotProduct( d, d )) * ( fabsf( f ) + 1.0f );
	}
	r += 16.0f;

	const vec3_t mins = { e->origin[0] - r, e->origin[1] - r, e->origin[2] - r };
	const vec3_t maxs = { e->origin[0] + r, e->origin[1] + r, e->origin[2] + r };

	return R_CullBox( mins, maxs );
}

/*
=================
R_StudioMePrepare

R_StudioPreCull for the draw lists before they're drawn: from the Media
Engine's table (an entity, its model and its sequence are ~6 cache misses),
the ones it doesn't know one by one. Then the second job: the bones and the
draws of what's left. Entities stay in the list, so a follower still finds
its culled parent for its bones.
=================
*/
static void R_StudioMePrepareList( cl_entity_t **list, int count, byte *culled, const mecull_t *cull, int numcull, const uint16_t *order, qboolean draw )
{
	float planes[FRUSTUM_PLANES][4];
	int numplanes = 0;

	for( int i = 0; i < FRUSTUM_PLANES; i++ )
	{
		if( !FBitSet( RI.frustum.clipFlags, BIT( i )))
			continue;
		VectorCopy( RI.frustum.planes[i].normal, planes[numplanes] );
		planes[numplanes++][3] = -RI.frustum.planes[i].dist;
	}

	for( int i = 0; i < count; i++ )
	{
		const int index = order ? order[i] : i;

		if( index >= numcull )
		{
			cl_entity_t *e = list[i];

			culled[i] = e->model && e->model->type == mod_studio && !e->player
				&& e->curstate.movetype != MOVETYPE_FOLLOW && R_StudioPreCull( e );
			continue;
		}

		const mecull_t *c = &cull[index];

		culled[i] = false;
		if( c->type == MECULL_MAIN || c->type == MECULL_BRUSH )
			continue;

		if( g_precull.enabled )
		{
			const float min = c->small ? g_precull.minpixels_small : g_precull.minpixels;

			if( min > 0.0f )
			{
				vec3_t d;
				VectorSubtract( c->org, RI.rvp.vieworigin, d );

				const float px = c->rpx * g_precull.px_scale;

				if( px * px < min * min * DotProduct( d, d ))
				{
					culled[i] = true;
					continue;
				}
			}

			for( int k = 0; k < numplanes; k++ )
			{
				if( DotProduct( planes[k], c->org ) + planes[k][3] + c->r < 0.0f )
				{
					culled[i] = true;
					break;
				}
			}

			if( culled[i] )
				continue;
		}

		if( R_StudioMeAdd( list[i], c, draw ))
			culled[i] = R_STUDIO_ME;
	}
}

void R_StudioMePrepare( byte *solid_culled, byte *trans_culled, const uint16_t *trans_order )
{
	int numsolid = 0, numtrans = 0;
	const mecull_t *cull;

	g_precull.enabled = r_studio_precull.value && !r_nocull.value;
	g_precull.minpixels = r_studio_minpixels.value > 0.0f && RI.rvp.fov_x > 0.0f ? r_studio_minpixels.value : 0.0f;
	g_precull.minpixels_small = r_studio_minpixels_small.value > 0.0f && RI.rvp.fov_x > 0.0f ? r_studio_minpixels_small.value : 0.0f;
	g_precull.px_scale = RI.rvp.fov_x > 0.0f ? RI.rvp.viewport[2] * 0.5f / tanf( RI.rvp.fov_x * ( M_PI_F / 360.0f )) : 0.0f;
	g_precull.time = gp_cl->time;

	if( r_mebones.ticket )
	{
		// a second view
		R_StudioFinishMeModels();
		R_StudioMeReset();
	}

	cull = R_StudioMeTable( &numsolid, &numtrans, NULL );
	const qboolean draw = r_mebones.frame.draw && R_StudioMeDrawAllowed();
	r_mebones.job.count = r_mebones.job.words = r_mebones.count = 0;

	cl_entity_t *saveent = RI.currententity;
	model_t *savemodel = RI.currentmodel;
	studiohdr_t *savehdr = m_pStudioHeader;
	const uint savedtex = glState.currentTexture; // the skin block lookups set it

	R_StudioSetupTimings();
	R_StudioMePrepareList( tr.draw_list->solid_entities, tr.draw_list->num_solid_entities, solid_culled, cull, numsolid, NULL, draw );
	r_mebones.job.numsolid = r_mebones.job.count;
	R_StudioMePrepareList( tr.draw_list->trans_entities, tr.draw_list->num_trans_entities, trans_culled, cull ? cull + MECULL_MAXSOLID : NULL, numtrans, trans_order, draw );

	glState.currentTexture = savedtex;
	RI.currententity = saveent;
	RI.currentmodel = savemodel;
	m_pStudioHeader = savehdr;

	if( cull )
		R_StudioMePost();
	g_precull.active = true;
}

void R_StudioPreCullEnd( void )
{
	g_precull.active = false;
}
#endif

/*
=================
R_DrawStudioModel
=================
*/
void R_DrawStudioModel( cl_entity_t *e )
{
	if( FBitSet( RI.rvp.flags, RF_DRAW_CUBEMAP ))
		return;

	// the display list is fixed size and the GE runs it as it's written:
	// overflowing it makes the GE execute garbage and hangs the console
	if( extGuListUsed() > guRender.context_list_size - 128 * 1024 )
	{
		static double last;
		if( gp_cl->time > last + 1.0 || gp_cl->time < last )
		{
			last = gp_cl->time;
			printf( "GULIST: %u KB used, skipping %s\n", extGuListUsed() >> 10, e->model ? e->model->name : "?" );
		}
		return;
	}

	R_StudioSetupTimings();

	if( e->player )
	{
		R_StudioDrawModelInternal( e, STUDIO_RENDER|STUDIO_EVENTS );
	}
	else if( e->curstate.movetype == MOVETYPE_FOLLOW )
	{
		cl_entity_t *parent = CL_GetEntityByIndex( e->curstate.aiment );

		if( !parent || !parent->model || parent->model->type != mod_studio )
			return;

		parent = R_FindParentEntity( e, tr.draw_list->solid_entities, tr.draw_list->num_solid_entities );

		if( !parent )
			parent = R_FindParentEntity( e, tr.draw_list->trans_entities, tr.draw_list->num_trans_entities );

		if( parent )
		{
			RI.currententity = parent;
			R_StudioDrawModelInternal( RI.currententity, 0 );
			VectorCopy( RI.currententity->curstate.origin, e->curstate.origin );
			VectorCopy( RI.currententity->origin, e->origin );
			RI.currententity = e;

			R_StudioDrawModelInternal( e, STUDIO_RENDER|STUDIO_EVENTS );
		}
	}
	else
	{
#if XASH_PSP
		if( !g_precull.active && R_StudioPreCull( e ))
			return;
		if( R_StudioPropDrawRecorded( e ))
			return;
#endif
		R_StudioDrawModelInternal( e, STUDIO_RENDER|STUDIO_EVENTS );
	}
}

#if XASH_PSP
static void R_StudioMeOpen( int mode )
{
	sceGuTexFunc( GU_TFX_MODULATE, GU_TCC_RGBA );
	sceGuDisable( GU_ALPHA_TEST );
	sceGuShadeModel( GU_SMOOTH );
	if( glState.faceCull != GL_NONE )
	{
		tr.fFlipViewModel = false;
		GL_Cull( GL_FRONT );
	}

	// the rest is in the model's commands
	switch( mode )
	{
	case MEMODE_SOLID:
		sceGuAmbient( 0xffffffff );
		break;
	case MEMODE_BLEND:
		sceGuBlendFunc( GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0 );
		sceGuAmbient( GUCOLOR4F( 1.0f, 1.0f, 1.0f, tr.blend ));
		break;
	case MEMODE_ADD:
		sceGuBlendFunc( GU_ADD, GU_FIX, GU_FIX, GUBLEND1, GUBLEND1 );
		sceGuAmbient( GUCOLOR4F( tr.blend, tr.blend, tr.blend, 1.0f ));
		sceGuLightColor( 0, GU_DIFFUSE, 0 );
		break;
	}

	const ScePspFVector3 dir = { 0.0f, 0.0f, 1.0f };

	R_StudioGEViewMatrix( true );
	sceGuLight( 0, GU_DIRECTIONAL, GU_DIFFUSE, &dir );
	sceGuEnable( GU_LIGHT0 );
	sceGuEnable( GU_LIGHTING );
}

static void R_StudioMeClose( void )
{
	sceGuDisable( GU_LIGHTING );
	sceGuDisable( GU_LIGHT0 );
	R_StudioGERestore();
	R_StudioGEViewMatrix( false );
	extGuUpdateStall();
	sceGuTexFunc( GU_TFX_REPLACE, GU_TCC_RGBA );
	sceGuShadeModel( GU_FLAT );
	ge_slots_valid = 0;
}

/*
=================
R_StudioMeItem

what's left to do here for a model the ME draws, NULL if it's not the ME's after all
=================
*/
static const mebones_res_t *R_StudioMeItem( int i )
{
	const mebones_item_t *it = &r_mebones.job.items[i];
	const medraw_t *d = it->draw;
	cl_entity_t *e = it->e;
	const mebones_res_t *res = R_StudioMeWait( i );

	if( !res || !res->ent || ( res->visible && !res->cmds ))
		return NULL;

	if( !res->visible )
		return res;

	r_stats.c_studio_models_drawn++;
	r_stats.c_studio_polys += d->polys + ( it->wdraw ? it->wdraw->polys : 0 );
	g_studio.framecount++; // render data cache cookie

	if( !res->blendprev )
		e->latched.prevframe = res->prevframe;

	if( !FBitSet( res->floor, BIT( 31 )))
	{
		e->cvFloorColor.r = res->floor & 255;
		e->cvFloorColor.g = ( res->floor >> 8 ) & 255;
		e->cvFloorColor.b = ( res->floor >> 16 ) & 255;
	}

	// R_StudioCalcAttachments
	memcpy( e->attachment, res->attachment, sizeof( vec3_t ) * ( d->hdr->numattachments > 0 ? d->hdr->numattachments : MEBONES_ATTACHMENTS ));

	if( res->events )
	{
		RI.currententity = e;
		RI.currentmodel = it->player ? (model_t *)it->player->mod : e->model;
		R_StudioSetHeader( (studiohdr_t *)d->hdr );
		Matrix3x4_Copy( g_studio.rotationmatrix, res->rotation );
		R_StudioClientEvents();
	}

	// copy attachments into global entity array
	if( e->index > 0 )
	{
		cl_entity_t *ent = CL_GetEntityByIndex( e->index );

		if( ent != e )
			memcpy( ent->attachment, e->attachment, sizeof( vec3_t ) * 4 );
		if( r_mebones.job.numelights )
			R_StudioMoveElights( e );
	}

	return res;
}

static const uint *R_StudioMeLastSkin( const mebones_item_t *it )
{
	const medraw_t *d = it->wdraw ? it->wdraw : it->draw;

	return d->blocks[d->numitems - 1];
}

/*
=================
R_StudioDrawMeModels

the solid models of the ME (in the list they're marked R_STUDIO_ME): the GE
waits for their commands, this CPU doesn't
=================
*/
void R_StudioDrawMeModels( void )
{
	const mebones_item_t *last = NULL;

	r_mebones.finish = r_mebones.count > 0;

	for( int i = 0; i < r_mebones.job.numsolid && r_mebones.count; i++ )
	{
		if( r_mebones.job.items[i].draw )
			last = &r_mebones.job.items[i];
	}

	if( !last )
		return;

	R_StudioMeOpen( MEMODE_SOLID );
	extGuCmdEnd( R_StudioGECall( extGuCmdBegin(), R_MeGate( &r_mebones.job, MEBONES_SOLIDGATE )));
	ge_last_skinblock = R_StudioMeLastSkin( last );
	R_StudioMeClose();
}

// a translucent one, marked the same way
void R_StudioDrawMeModel( cl_entity_t *e )
{
	const int i = R_StudioMeFind( e );

	if( i < r_mebones.job.numsolid || !r_mebones.job.items[i].draw )
		return;

	R_StudioMeOpen( r_mebones.job.items[i].mode );
	extGuCmdEnd( R_StudioGECall( extGuCmdBegin(), R_MeGate( &r_mebones.job, i )));
	ge_last_skinblock = R_StudioMeLastSkin( &r_mebones.job.items[i] );
	R_StudioMeClose();
}

/*
=================
R_StudioFinishMeModels

events and attachments of these models, as late in the frame as possible: the
ME may still be at it. In the next frames the main CPU draws the ones the ME
couldn't
=================
*/
void R_StudioFinishMeModels( void )
{
	if( !r_mebones.finish )
		return;

	r_mebones.finish = false;

	cl_entity_t *saveent = RI.currententity;
	model_t *savemodel = RI.currentmodel;

	R_StudioSetupTimings();

	for( int i = 0; i < r_mebones.count; i++ )
	{
		const mebones_item_t *it = &r_mebones.job.items[i];

		if( it->draw && !R_StudioMeItem( i ))
			R_StudioMeRefuse( it->e );
	}

	// it was given up on: the GE skips the others
	if( MEBONES_SYNC[MESYNC_ITEMS] < r_mebones.job.numsolid )
		R_MeGateOpen( &r_mebones.job, MEBONES_SOLIDGATE, 0 );
	for( int i = Q_max( MEBONES_SYNC[MESYNC_ITEMS], r_mebones.job.numsolid ); i < r_mebones.count; i++ )
		R_MeGateOpen( &r_mebones.job, i, 0 );

	RI.currententity = saveent;
	RI.currentmodel = savemodel;
}
#endif

/*
=================
R_RunViewmodelEvents
=================
*/
void R_RunViewmodelEvents( void )
{
	if( r_drawviewmodel->value == 0 )
		return;

	if( ENGINE_GET_PARM( PARM_THIRDPERSON ))
		return;

	// ignore in thirdperson, camera view or client is died
	if( FBitSet( RI.rvp.flags, RF_DRAW_CUBEMAP ) || ENGINE_GET_PARM( PARM_LOCAL_HEALTH ) <= 0 || !CL_IsViewEntityLocalPlayer())
		return;

	RI.currententity = tr.viewent;

	if( !RI.currententity->model || RI.currententity->model->type != mod_studio )
		return;

	R_StudioSetupTimings();

	vec3_t simorg = Vec3( gp_cl->simorg );
	for( int i = 0; i < 4; i++ )
		VectorCopy( simorg, RI.currententity->attachment[i] );
	RI.currentmodel = RI.currententity->model;

	R_StudioDrawModelInternal( RI.currententity, STUDIO_EVENTS );
}

/*
=================
R_ViewModelSolid

nothing of the view model blends with what is behind it: its depth range puts it
in front of everything, so it can be drawn first
=================
*/
qboolean R_ViewModelSolid( void )
{
	const cl_entity_t *view = tr.viewent;

	if( !view->model || view->model->type != mod_studio || view->curstate.rendermode != kRenderNormal || view->curstate.renderfx )
		return false;

	const studiohdr_t *hdr = gEngfuncs.Mod_Extradata( mod_studio, view->model );
	if( !hdr )
		return false;

	const mstudiotexture_t *ptex = (const mstudiotexture_t *)((const byte *)hdr + hdr->textureindex );

	for( int i = 0; i < hdr->numtextures; i++ )
	{
		if( FBitSet( ptex[i].flags, STUDIO_NF_ADDITIVE ))
			return false;
	}

	return true;
}

/*
=================
R_DrawViewModel
=================
*/
void R_DrawViewModel( void )
{
	cl_entity_t	*view = tr.viewent;

	R_GatherPlayerLight( view );

	if( r_drawviewmodel->value == 0 )
		return;

	if( ENGINE_GET_PARM( PARM_THIRDPERSON ))
		return;

	// ignore in thirdperson, camera view or client is died
	if( FBitSet( RI.rvp.flags, RF_DRAW_CUBEMAP ) || ENGINE_GET_PARM( PARM_LOCAL_HEALTH ) <= 0 || !CL_IsViewEntityLocalPlayer())
		return;

	tr.blend = CL_FxBlend( view ) / 255.0f;
	if( !R_ModelOpaque( view->curstate.rendermode ) && tr.blend <= 0.0f )
		return; // invisible ?

	RI.currententity = view;

	if( !RI.currententity->model )
		return;

	// adjust the depth range to prevent view model from poking into walls
	sceGuDepthRange( (int)gldepthmin, (int)( gldepthmin + VIEWMODEL_DEPTH_RANGE * ( gldepthmax - gldepthmin )));
	RI.currentmodel = RI.currententity->model;

	switch( RI.currententity->model->type )
	{
	case mod_alias:
		R_DrawAliasModel( RI.currententity );
		break;
	case mod_studio:
		R_StudioSetupTimings();
		R_StudioDrawModelInternal( RI.currententity, STUDIO_RENDER );
		break;
	}

	// restore depth range
	sceGuDepthRange( (int)gldepthmin, (int)gldepthmax );
}

/*
====================
R_StudioLoadTexture

load model texture with unique name
====================
*/
static void R_StudioLoadTexture( model_t *mod, studiohdr_t *phdr, mstudiotexture_t *ptexture )
{
	int		flags = 0;
	char		texname[128], name[128], mdlname[128];
	texture_t		*tx = NULL;
	qboolean load_external = false;

	if( ptexture->flags & STUDIO_NF_NORMALMAP )
		flags |= (TF_NORMALMAP);

	// store some textures for remapping
	if( !Q_strnicmp( ptexture->name, "DM_Base", 7 ) || !Q_strnicmp( ptexture->name, "remap", 5 ))
	{
		char	val[6];

		int i = mod->numtextures;
		mod->textures = (texture_t **)Mem_Realloc( mod->mempool, mod->textures, ( i + 1 ) * sizeof( texture_t* ));
		int size = ptexture->width * ptexture->height + 768;
		tx = Mem_Calloc( mod->mempool, sizeof( *tx ) + size );
		mod->textures[i] = tx;

		// store ranges into anim_min, anim_max etc
		if( !Q_strnicmp( ptexture->name, "DM_Base", 7 ))
		{
			Q_strncpy( tx->name, "DM_Base", sizeof( tx->name ));
			tx->anim_min = PLATE_HUE_START; // topcolor start
			tx->anim_max = PLATE_HUE_END; // topcolor end
			// bottomcolor start always equal is (topcolor end + 1)
			tx->anim_total = SUIT_HUE_END;// bottomcolor end
		}
		else
		{
			Q_strncpy( tx->name, "DM_User", sizeof( tx->name )); // custom remapped
			Q_strncpy( val, ptexture->name + 7, 4 );
			tx->anim_min = bound( 0, Q_atoi( val ), 255 ); // topcolor start
			Q_strncpy( val, ptexture->name + 11, 4 );
			tx->anim_max = bound( 0, Q_atoi( val ), 255 ); // topcolor end
			// bottomcolor start always equal is (topcolor end + 1)
			Q_strncpy( val, ptexture->name + 15, 4 );
			tx->anim_total = bound( 0, Q_atoi( val ), 255 ); // bottomcolor end
		}

		tx->width = ptexture->width;
		tx->height = ptexture->height;

		// the pixels immediately follow the structures
		byte *pixels = (byte *)phdr + ptexture->index;
		memcpy( tx+1, pixels, size );

		ptexture->flags |= STUDIO_NF_COLORMAP;	// yes, this is colormap image
		flags |= TF_FORCE_COLOR;

		mod->numtextures++;	// done
	}

	Q_strncpy( mdlname, mod->name, sizeof( mdlname ));
	COM_FileBase( ptexture->name, name, sizeof( name ));
	COM_StripExtension( mdlname );

	if( FBitSet( ptexture->flags, STUDIO_NF_NOMIPS ))
		SetBits( flags, TF_NOMIPMAP );

	if( FBitSet( gp_host->features, ENGINE_IMPROVED_LINETRACE ) && FBitSet( ptexture->flags, STUDIO_NF_MASKED ))
		flags |= TF_KEEP_SOURCE; // Paranoia2 texture alpha-tracing

	// NOTE: colormaps must have the palette for properly work. Ignore them
	if( Mod_AllowMaterials( ) && !FBitSet( ptexture->flags, STUDIO_NF_COLORMAP ))
	{
		if( R_SearchForTextureReplacement( texname, sizeof( texname ), mdlname, "materials/%s/%s.tga", mdlname, name ))
		{
			int gl_texturenum = GL_LoadTexture( texname, NULL, 0, flags );
			R_TextureReplacementReport( mdlname, gl_texturenum, texname );
			if(( load_external = gl_texturenum != 0 ))
				ptexture->index = gl_texturenum;
		}
	}

	if( !load_external )
	{
		// NOTE: replace index with pointer to start of imagebuffer, ImageLib expected it
		gEngfuncs.Image_SetMDLPointer((byte *)phdr + ptexture->index);
		size_t size = sizeof( mstudiotexture_t ) + ptexture->width * ptexture->height + 768;

		// build the texname
		Q_snprintf( texname, sizeof( texname ), "#%s/%s.mdl", mdlname, name );
		ptexture->index = GL_LoadTexture( texname, (byte *)ptexture, size, flags );
	}

	if( !ptexture->index )
	{
		ptexture->index = tr.defaultTexture;
	}
	else if( tx )
	{
		// duplicate texnum for easy acess
		tx->gl_texturenum = ptexture->index;
	}
}

// the model whose textures are being loaded and how many are done, see Mod_StudioUnloadTextures
static studiohdr_t *studio_loading_hdr;
static int studio_loaded_textures;

/*
=================
Mod_StudioLoadTextures
=================
*/
void Mod_StudioLoadTextures( model_t *mod, void *data )
{
	R_WaitFrame(); // R_StudioGEUnload frees what the GE may be drawing
	studiohdr_t	*phdr = (studiohdr_t *)data;

	if( !phdr )
		return;

	R_StudioGEUnload( phdr );

	mstudiotexture_t *ptexture = (mstudiotexture_t *)(((byte *)phdr) + phdr->textureindex);
	if( phdr->textureindex > 0 )
	{
		// R_StudioLoadTexture replaces the pixel offsets with texture numbers
		int *pixofs = malloc( Q_max( phdr->numtextures, 1 ) * sizeof( *pixofs ));
		for( int i = 0; pixofs && i < phdr->numtextures; i++ )
			pixofs[i] = ptexture[i].index;

		studio_loading_hdr = phdr;

		for( studio_loaded_textures = 0; studio_loaded_textures < phdr->numtextures; studio_loaded_textures++ )
			R_StudioLoadTexture( mod, phdr, &ptexture[studio_loaded_textures] );

		studio_loading_hdr = NULL;

		if( pixofs )
			R_StudioBuildAtlas( mod, phdr, pixofs );
		free( pixofs );
	}
}

/*
=================
Mod_StudioUnloadTextures
=================
*/
void Mod_StudioUnloadTextures( void *data )
{
	R_WaitFrame(); // R_StudioGEUnload frees what the GE may be drawing
	studiohdr_t	*phdr = (studiohdr_t *)data;

	if( !phdr )
		return;

	R_StudioGEUnload( phdr );

	mstudiotexture_t *ptexture = (mstudiotexture_t *)(((byte *)phdr) + phdr->textureindex);
	int numtextures = phdr->numtextures;

	// a Host_Error (texture out of memory) in the middle of Mod_StudioLoadTextures:
	// the textures after the failed one still hold file offsets, not texture numbers
	if( phdr == studio_loading_hdr )
	{
		numtextures = studio_loaded_textures;
		studio_loading_hdr = NULL;
	}

	// release all textures
	for( int i = 0; i < numtextures; i++ )
	{
		if( ptexture[i].index == tr.defaultTexture )
			continue;
		R_StudioFreeAtlas( ptexture[i].index );
		GL_FreeTexture( ptexture[i].index );
	}
}

static void pfnStudioDynamicLight( cl_entity_t *ent, alight_t *plight )
{
	R_EntityDynamicLight( ent, plight, FBitSet( RI.rvp.flags, RF_DRAW_WORLD ), g_studio.time, g_studio.lightspot, g_studio.lightvec );
}

qboolean R_StudioFillAPI( engine_studio_api_t *api, r_studio_interface_t *pDefaultDraw )
{
	cl_righthand = gEngfuncs.pfnGetCvarPointer( "cl_righthand" );

	api->GetCurrentEntity        = pfnGetCurrentEntity;
	api->PlayerInfo              = pfnPlayerInfo;
	api->GetPlayerState          = R_StudioGetPlayerState;
	api->GetTimes                = pfnGetEngineTimes;
	api->GetViewInfo             = pfnGetViewInfo;
	api->GetModelCounters        = pfnGetModelCounters;
	api->StudioGetBoneTransform  = pfnStudioGetBoneTransform;
	api->StudioGetLightTransform = pfnStudioGetLightTransform;
	api->StudioGetRotationMatrix = pfnStudioGetRotationMatrix;
	api->StudioSetupModel        = R_StudioSetupModel;
	api->StudioCheckBBox         = R_StudioCheckBBox;
	api->StudioDynamicLight      = pfnStudioDynamicLight;
	api->StudioEntityLight       = R_StudioEntityLight;
	api->StudioSetupLighting     = R_StudioSetupLighting;
	api->StudioDrawPoints        = R_StudioDrawPoints;
	api->StudioDrawHulls         = R_StudioDrawHulls;
	api->StudioDrawAbsBBox       = R_StudioDrawAbsBBox;
	api->StudioDrawBones         = R_StudioDrawBones;
	api->StudioSetupSkin         = (void *)R_StudioSetupSkin;
	api->StudioSetRemapColors    = R_StudioSetRemapColors;
	api->SetupPlayerModel        = R_StudioSetupPlayerModel;
	api->StudioClientEvents      = R_StudioClientEvents;
	api->GetForceFaceFlags       = R_StudioGetForceFaceFlags;
	api->SetForceFaceFlags       = R_StudioSetForceFaceFlags;
	api->StudioSetHeader         = (void *)R_StudioSetHeader;
	api->SetRenderModel          = R_StudioSetRenderModel;
	api->SetupRenderer           = R_StudioSetupRenderer;
	api->RestoreRenderer         = R_StudioRestoreRenderer;
	api->SetChromeOrigin         = R_StudioSetChromeOrigin;
	api->GL_StudioDrawShadow     = GL_StudioDrawShadow;
	api->GL_SetRenderMode        = GL_StudioSetRenderMode;
	api->StudioSetRenderamt      = R_StudioSetRenderamt;
	api->StudioSetCullState      = R_StudioSetCullState;
	api->StudioRenderShadow      = R_StudioRenderShadow;

	pDefaultDraw->version         = STUDIO_INTERFACE_VERSION;
	pDefaultDraw->StudioDrawModel  = R_StudioDrawModel;
	pDefaultDraw->StudioDrawPlayer = R_StudioDrawPlayer;

	return true;
}

void R_StudioSetDrawInterface( r_studio_interface_t *pDraw )
{
	pStudioDraw = pDraw;
}
