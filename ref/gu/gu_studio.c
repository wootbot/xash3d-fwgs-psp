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
CVAR_DEFINE_AUTO( r_studio_builtin_renderer, "0", 0, "use built-in studio model renderer instead of the one provided by client library (debugging)" );
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
void R_StudioInit( void )
{

#if XASH_PSVITA
	// don't do the same array-building work twice since that's what our FFP shim does anyway
	gEngfuncs.Cvar_FullSet( "r_studio_drawelements", "0", FCVAR_READ_ONLY );
#endif

	Matrix3x4_LoadIdentity( g_studio.rotationmatrix );

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
static void R_StudioCalcBoneAdj( float dadt, float *adj, const byte *pcontroller1, const byte *pcontroller2, byte mouthopen )
{
	mstudiobonecontroller_t	*pbonecontroller = (mstudiobonecontroller_t *)((byte *)m_pStudioHeader + m_pStudioHeader->bonecontrollerindex);

	for( int j = 0; j < m_pStudioHeader->numbonecontrollers; j++ )
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

	for( int i = 0; i < m_pStudioHeader->numbones; i++, pbone++, panim++ )
		R_StudioCalcBones( frame, s, pbone, panim, adj, pos[i], q[i] );

	if( pseqdesc->motiontype & STUDIO_X ) pos[pseqdesc->motionbone][0] = 0.0f;
	if( pseqdesc->motiontype & STUDIO_Y ) pos[pseqdesc->motionbone][1] = 0.0f;
	if( pseqdesc->motiontype & STUDIO_Z ) pos[pseqdesc->motionbone][2] = 0.0f;
}

/*
====================
StudioMergeBones

====================
*/
static void R_StudioMergeBones( cl_entity_t *e, model_t *m_pSubModel )
{
	static vec4_t	q[MAXSTUDIOBONES];
	static float	pos[MAXSTUDIOBONES][3];

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
static void R_StudioSaveBones( void )
{
	mstudiobone_t *pbones = (mstudiobone_t *)((byte *)m_pStudioHeader + m_pStudioHeader->boneindex);
	g_studio.cached_numbones = m_pStudioHeader->numbones;

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

Only for the common case: normal render mode, no local lights, no fog, no
glow shell, no bone weights, and meshes without chrome/fullbright/flatshade/
additive/float UVs; everything else takes the CPU path.

=============================================================================
*/
CVAR_DEFINE_AUTO( r_studio_ge, "1", 0, "draw studio models with GE skinning and lighting where possible" );

#define GE_MAXBONES		8
#define GE_RAMP_SIZE	32
#define GE_CACHE_SIZE	512	// submodels

#define GE_CMD( cmd, arg )	((( cmd ) << 24 ) | (( arg ) & 0xffffff ))
#define GE_UNSUPPORTED_FLAGS	( STUDIO_NF_CHROME|STUDIO_NF_FULLBRIGHT|STUDIO_NF_FLATSHADE|STUDIO_NF_ADDITIVE|STUDIO_NF_UV_COORDS )

typedef struct
{
	uint		*cmds;		// VTYPE BASE VADDR PRIM... RET, called from the display list
	byte		numbones;
	byte		bones[GE_MAXBONES];
} gebatch_t;

typedef struct
{
	int		numbatches;	// 0: drawn by the CPU path
	gebatch_t		*batches;
} gemesh_t;

typedef struct
{
	const studiohdr_t	*hdr;
	const mstudiomodel_t *submodel;
	qboolean		cpumeshes;	// some meshes still need the CPU path
	int		nummesh;
	gemesh_t		*meshes;
	void		**blocks;		// batch memory, freed with the model
	int		numblocks;
} gemodel_t;

static gemodel_t	*ge_cache[GE_CACHE_SIZE];
static size_t	ge_cache_bytes;

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
	free( gm->blocks );
	free( gm->meshes );
	free( gm );
}

// Mod_StudioUnloadTextures: the header memory can be reused by another model
static void R_StudioGEUnload( const studiohdr_t *hdr )
{
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
			((short *)( v + off_t ))[0] = t[2];
			((short *)( v + off_t ))[1] = t[3];
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

static qboolean R_StudioGEBuildMesh( gemodel_t *gm, gemesh_t *gmesh, const short *ptricmds, const byte *pvertbone, const byte *pnormbone, const vec3_t *pstudioverts, const vec3_t *pstudionorms )
{
	int numunits = 0, i;
	const short *p = ptricmds;

	// the GE moves a normal with its vertex's bone
	while(( i = *( p++ )))
	{
		if( i < 0 ) i = -i;
		for( int k = 0; k < i; k++, p += 4 )
			if( pvertbone[p[0]] != pnormbone[p[1]] )
				return false;
		numunits += i; // upper bound: one unit per triangle
	}

	geunit_t *units = malloc( sizeof( *units ) * ( numunits + 1 ));
	gebatch_t *batches = NULL;
	int nunits = 0, nbatches = 0;
	byte bones[MAXSTUDIOBONES];

	if( !units )
		return false;

	p = ptricmds;
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
			}
		}
		p += i * 4;
	}

	// greedy batches
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
					free( units );
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

	free( units );
	gmesh->batches = batches;
	gmesh->numbatches = nbatches;
	ge_cache_bytes += sizeof( *batches ) * nbatches;
	return nbatches > 0;
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

	ge_build_prevn = 0;
	for( int j = 0; j < m_pSubModel->nummesh; j++ )
	{
		const short *ptricmds = (const short *)((const byte *)m_pStudioHeader + pmesh[j].triindex);

		if( R_StudioGEBuildMesh( gm, &gm->meshes[j], ptricmds, pvertbone, pnormbone, pstudioverts, pstudionorms ))
			numge++;
		else
		{
			gm->meshes[j].numbatches = 0;
			gm->cpumeshes = true;
		}
	}

	gm->nummesh = m_pSubModel->nummesh;
	gEngfuncs.Con_DPrintf( "GE studio: %s %d/%d meshes, %u KB cached\n", m_pSubModel->name, numge, gm->nummesh, (uint)( ge_cache_bytes >> 10 ));

	return numge ? gm : NULL;
}

/*
===============
R_StudioGERamp

R_StudioLightMeshFast as a function of dot( normal, lightvec ), as a texture
===============
*/
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

	memcpy( last, params, sizeof( last ));
	lastramp = ramp;
	lastframe = guRender.vert_frame;
	return ramp;
}

// bone of each GE matrix slot, valid for the current entity only
static byte	ge_slot_bone[GE_MAXBONES];
static uint	ge_slots_valid;

static uint *R_StudioGEBones( uint *p, const gebatch_t *b )
{
	const float *eye = RI.rvp.vieworigin;
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
		*p++ = GE_CMD( 0x2B, GE_Float( m[0][3] - eye[0] ));
		*p++ = GE_CMD( 0x2B, GE_Float( m[1][3] - eye[1] ));
		*p++ = GE_CMD( 0x2B, GE_Float( m[2][3] - eye[2] ));
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
static const uint *R_StudioGELightBlock( const uint *ramp )
{
	static const uint *last, *lastramp;
	static vec3_t lastvec;
	static uint frame = ~0u;

	if( frame == guRender.vert_frame && ramp == lastramp && VectorCompare( lastvec, g_studio.lightvec ))
		return last;

	uint *start = extGuBlockBegin();
	ScePspFMatrix4 tm;

	sceGuTexScale( 1.0f, 1.0f );
	sceGuTexMode( GU_PSM_8888, 0, 0, GU_FALSE );
	sceGuTexFilter( GU_LINEAR, GU_LINEAR );
	sceGuTexWrap( GU_CLAMP, GU_CLAMP );
	sceGuTexImage( 0, GE_RAMP_SIZE, 1, GE_RAMP_SIZE, ramp );
	sceGuTexFunc( GU_TFX_REPLACE, GU_TCC_RGB );

	memset( &tm, 0, sizeof( tm ));
	tm.x.x = 0.5f * g_studio.lightvec[0];
	tm.y.x = 0.5f * g_studio.lightvec[1];
	tm.z.x = 0.5f * g_studio.lightvec[2];
	tm.w.x = 0.5f;
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
	last = extGuBlockEnd( start );
	lastramp = ramp;
	VectorCopy( g_studio.lightvec, lastvec );
	frame = guRender.vert_frame;
	return last;
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
was built from on every use, rebuilt when a texture or a cvar changed.
*/
typedef struct
{
	uint		*block;
	uint		words, capacity;
	const void	*data, *palette;	// gl_texture_t the block points to
	int		format, width, height, numMips, flags;
	int		skinw, skinh;	// mstudiotexture_t size for the texture scale
	float		cvars[5];
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
		&& a->skinh == b->skinh && !memcmp( a->cvars, b->cvars, sizeof( a->cvars ));
}

static const uint *R_StudioGEBuildSkinBlock( const mstudiotexture_t *ptex, int skinindex )
{
	uint *start = extGuBlockBegin();

	glState.currentTexture = 0; // force GL_Bind to emit everything
	R_StudioSetupSkin( m_pStudioHeader, skinindex );
	// texcoords are the raw texel shorts: u = s / 32768 * scale
	sceGuTexScale( 32768.0f / ptex->width, 32768.0f / ptex->height );
	sceGuTexFunc( GU_TFX_MODULATE, GU_TCC_RGBA );
	// the batches have no vertex color: the GE modulates with the material
	// color instead, which is whatever sceGuColor was last (blood sprites, shells)
	sceGuColor( 0xffffffff );
	sceGuTexMapMode( GU_TEXTURE_COORDS, 0, 0 );
	sceGuDepthFunc( GU_LEQUAL );
	sceGuDepthMask( GU_FALSE );
	sceGuDisable( GU_BLEND );
	const uint *block = extGuBlockEnd( start );
	return block;
}

static const uint *R_StudioGESkinBlock( const mstudiotexture_t *ptex, int skinindex )
{
	const int texnum = ptex->index;

	// remapped player skins differ per entity, r_lightmap binds the white texture
	if( m_fDoRemap || r_lightmap->value || texnum <= 0 || texnum >= MAX_TEXTURES )
		return R_StudioGEBuildSkinBlock( ptex, skinindex );

	if( !ge_skins && !( ge_skins = calloc( MAX_TEXTURES, sizeof( *ge_skins ))))
		return R_StudioGEBuildSkinBlock( ptex, skinindex );

	geskin_t *s = &ge_skins[texnum];
	geskin_t key;

	R_StudioGESkinKey( R_GetTexture( texnum ), ptex, &key );
	if( s->block && R_StudioGESkinMatches( s, &key ))
	{
		glState.currentTexture = texnum; // what the block binds
		return s->block;
	}

	const uint *frame_block = R_StudioGEBuildSkinBlock( ptex, skinindex );
	const uint words = extGuCmdBegin() - frame_block; // up to and including the RET

	if( words > s->capacity )
	{
		// an old block may still be called from the list the GE is drawing:
		// only happens when a texture or a cvar changed, let it leak
		uint *mem = memalign( 16, words * sizeof( uint ));
		if( !mem )
			return frame_block;
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
	return frame_block;
}

static void R_StudioGEDrawMesh( const gemesh_t *gmesh, const mstudiotexture_t *ptex, int skinindex, const uint *lightblock )
{
	const uint *skinblock = R_StudioGESkinBlock( ptex, skinindex );

	uint *p = extGuCmdBegin();
	for( int i = 0; i < gmesh->numbatches; i++ )
	{
		const gebatch_t *b = &gmesh->batches[i];

		p = R_StudioGEBones( p, b );
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
	glState.currentTexture = 0;
	ge_last_skinblock = NULL;
}

/*
===============
R_StudioGEAllowed

entity state that the GE path can draw
===============
*/
static qboolean R_StudioGEAllowed( void )
{
	if( !r_studio_ge.value || g_studio.numlocallights || g_nForceFaceFlags )
		return false;
	if( g_studio.rendermode != kRenderNormal || RI.currententity->curstate.renderfx == kRenderFxGlowShell )
		return false;
	if( glState.isFogEnabled || r_lightmap->value || r_fullbright->value )
		return false;
	if( FBitSet( m_pStudioHeader->flags, STUDIO_HAS_BONEWEIGHTS ))
		return false;
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

	const int numverts = m_pSubModel->numverts;
	const int numnorms = m_pSubModel->numnorms;

	gemodel_t *gm = R_StudioGEAllowed() ? R_StudioGEGetModel() : NULL;
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

		if( gm && gm->meshes[j].numbatches && !FBitSet( g_nFaceFlags, GE_UNSUPPORTED_FLAGS ))
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
	const uint *lightblock = NULL;

	ge_slots_valid = 0;

	for( int j = 0; j < m_pSubModel->nummesh; j++ )
	{
		float	oldblend = tr.blend;

		pmesh = g_studio.meshes[j].mesh;
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
		if( gmesh && !lightblock )
			lightblock = R_StudioGELightBlock( R_StudioGERamp( ));
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
			R_StudioGEDrawMesh( gmesh, &ptexture[pskinref[pmesh->skinref]], pskinref[pmesh->skinref], lightblock );
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

void R_StudioResetPlayerModels( void )
{
	memset( g_studio.player_models, 0, sizeof( g_studio.player_models ));
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

			if( gEngfuncs.fsapi->FileExists( state->modelname, false ))
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
	mstudiotexture_t *ptexture = (mstudiotexture_t *)((byte *)phdr + phdr->textureindex);

	int opaque = 0, trans = 0;
	for( int i = 0; i < phdr->numtextures; i++, ptexture++ )
	{
		// ignore chrome & additive it's just a specular-like effect
		if( FBitSet( ptexture->flags, STUDIO_NF_ADDITIVE ) && !FBitSet( ptexture->flags, STUDIO_NF_CHROME ))
			trans++;
		else opaque++;
	}

	// if model is more additive than opaque
	if( trans > opaque )
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
			GL_StudioDrawShadow();
		}
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

/*
===============
R_StudioDrawModel

===============
*/
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

	if( RI.currententity->curstate.movetype == MOVETYPE_FOLLOW )
		R_StudioMergeBones( RI.currententity, RI.currentmodel );
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

/*
=================
R_DrawStudioModel
=================
*/
void R_DrawStudioModel( cl_entity_t *e )
{
	if( FBitSet( RI.rvp.flags, RF_DRAW_CUBEMAP ))
		return;

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
		R_StudioDrawModelInternal( e, STUDIO_RENDER|STUDIO_EVENTS );
	}
}

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

/*
=================
Mod_StudioLoadTextures
=================
*/
void Mod_StudioLoadTextures( model_t *mod, void *data )
{
	studiohdr_t	*phdr = (studiohdr_t *)data;

	if( !phdr )
		return;

	mstudiotexture_t *ptexture = (mstudiotexture_t *)(((byte *)phdr) + phdr->textureindex);
	if( phdr->textureindex > 0 )
	{
		for( int i = 0; i < phdr->numtextures; i++ )
			R_StudioLoadTexture( mod, phdr, &ptexture[i] );
	}
}

/*
=================
Mod_StudioUnloadTextures
=================
*/
void Mod_StudioUnloadTextures( void *data )
{
	studiohdr_t	*phdr = (studiohdr_t *)data;

	if( !phdr )
		return;

	R_StudioGEUnload( phdr );

	mstudiotexture_t *ptexture = (mstudiotexture_t *)(((byte *)phdr) + phdr->textureindex);

	// release all textures
	for( int i = 0; i < phdr->numtextures; i++ )
	{
		if( ptexture[i].index == tr.defaultTexture )
			continue;
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
