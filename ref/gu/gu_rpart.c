/*
gu_rpart.c - particles and tracers
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
#include "r_efx.h"
#include "event_flags.h"
#include "entity_types.h"
#include "triangleapi.h"
#include "pm_local.h"
#include "studio.h"

CVAR_DEFINE_AUTO( r_fast_particles, "1", FCVAR_ARCHIVE, "draw particles as untextured points" );

static float gTracerSize[11] = { 1.5f, 0.5f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
static color24 gTracerColors[] =
{
{ 255, 255, 255 },		// White
{ 255, 0, 0 },		// Red
{ 0, 255, 0 },		// Green
{ 0, 0, 255 },		// Blue
{ 0, 0, 0 },		// Tracer default, filled in from cvars, etc.
{ 255, 167, 17 },		// Yellow-orange sparks
{ 255, 130, 90 },		// Yellowish streaks (garg)
{ 55, 60, 144 },		// Blue egon streak
{ 255, 130, 90 },		// More Yellowish streaks (garg)
{ 255, 140, 90 },		// More Yellowish streaks (garg)
{ 200, 130, 90 },		// More red streaks (garg)
{ 255, 120, 70 },		// Darker red streaks (garg)
};

static uint CL_ParticleColor( particle_t *p, float time )
{
	p->color = bound( 0, p->color, 255 );
	color24 color = tr.palette[p->color];

	int alpha = 255 * (p->die - time) * 16.0f;
	if( alpha > 255 || p->type == pt_static )
		alpha = 255;

	return GUCOLOR4UB( color.r, color.g, color.b, alpha );
}

/*
================
CL_DrawParticles

update particle color, position, free expired and draw it
================
*/
void CL_DrawParticles( double frametime, particle_t *cl_active_particles, float partsize )
{
	uint vert_count = 0, max_verts, max_bytes;
	const float time = gp_cl->time; // particle_t::die is a float, no software doubles per particle

	if( !cl_active_particles )
		return;	// nothing to draw?

	sceGuEnable( GU_BLEND );
	sceGuDisable( GU_ALPHA_TEST );
	sceGuBlendFunc( GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0 );
	sceGuDepthMask( GU_TRUE ); // no depth writes

	// vertices go straight into the display list, keep some room for the commands after them
	void *packet = extGuBeginPacket( &max_bytes );
	max_bytes = max_bytes > 1024 ? max_bytes - 1024 : 0;

	if( r_fast_particles.value )
	{
		// one point per particle
		gu_vert_fcv_t *const out = packet;

		max_verts = max_bytes / sizeof( *out );
		sceGuDisable( GU_TEXTURE_2D );

		for( particle_t *p = cl_active_particles; p; p = p->next )
		{
			if(( p->type != pt_blob ) || ( p->unused == 255 ))
			{
				if( vert_count < max_verts )
				{
					out[vert_count].c = CL_ParticleColor( p, time );
					out[vert_count].x = p->org[0];
					out[vert_count].y = p->org[1];
					out[vert_count].z = p->org[2];
					vert_count++;
				}

				r_stats.c_particle_count++;
			}

			gEngfuncs.CL_ThinkParticle( frametime, p );
		}

		if( vert_count )
		{
			extGuEndPacket( out + vert_count );
			sceGuDrawArray( GU_POINTS, GU_COLOR_8888 | GU_VERTEX_32BITF, vert_count, 0, out );
		}

		sceGuEnable( GU_TEXTURE_2D );
	}
	else
	{
		// screen aligned sprite between two opposite corners
		gu_vert_ftcv_t *const out = packet;

		max_verts = max_bytes / sizeof( *out );
		GL_Bind( XASH_TEXTURE0, tr.particleTexture );
		sceGuTexFunc( GU_TFX_MODULATE, GU_TCC_RGBA );

		for( particle_t *p = cl_active_particles; p; p = p->next )
		{
			if(( p->type != pt_blob ) || ( p->unused == 255 ))
			{
				if( vert_count + 2 <= max_verts )
				{
					float size = partsize; // get initial size of particle

					// scale up to keep particles from disappearing
					size += (p->org[0] - RI.rvp.vieworigin[0]) * RI.cull_vforward[0];
					size += (p->org[1] - RI.rvp.vieworigin[1]) * RI.cull_vforward[1];
					size += (p->org[2] - RI.rvp.vieworigin[2]) * RI.cull_vforward[2];

					if( size < 20.0f ) size = partsize;
					else size = partsize + size * 0.002f;

					// scale the axes by radius
					vec3_t right, up;
					VectorScale( RI.cull_vright, size, right );
					VectorScale( RI.cull_vup, size, up );

					uint color = CL_ParticleColor( p, time );

					out[vert_count].u = 0.0f;
					out[vert_count].v = 0.0f;
					out[vert_count].c = color;
					out[vert_count].x = p->org[0] + right[0] + up[0];
					out[vert_count].y = p->org[1] + right[1] + up[1];
					out[vert_count].z = p->org[2] + right[2] + up[2];
					vert_count++;
					out[vert_count].u = 1.0f;
					out[vert_count].v = 1.0f;
					out[vert_count].c = color;
					out[vert_count].x = p->org[0] - right[0] - up[0];
					out[vert_count].y = p->org[1] - right[1] - up[1];
					out[vert_count].z = p->org[2] - right[2] - up[2];
					vert_count++;
				}

				r_stats.c_particle_count++;
			}

			gEngfuncs.CL_ThinkParticle( frametime, p );
		}

		if( vert_count )
		{
			extGuEndPacket( out + vert_count );
			sceGuDrawArray( GU_SPRITES, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF, vert_count, 0, out );
		}
	}

	sceGuDepthMask( GU_FALSE );
}

/*
================
CL_CullTracer

check tracer bbox
================
*/
static qboolean CL_CullTracer( particle_t *p, const vec3_t start, const vec3_t end )
{
	vec3_t mins, maxs;

	// compute the bounding box
	for( int i = 0; i < 3; i++ )
	{
		if( start[i] < end[i] )
		{
			mins[i] = start[i];
			maxs[i] = end[i];
		}
		else
		{
			mins[i] = end[i];
			maxs[i] = start[i];
		}

		// don't let it be zero sized
		if( mins[i] == maxs[i] )
		{
			maxs[i] += gTracerSize[p->type] * 2.0f;
		}
	}

	// check bbox
	return R_CullBox( mins, maxs );
}

/*
================
CL_DrawTracers

update tracer color, position, free expired and draw it
================
*/
void CL_DrawTracers( double frametime, particle_t *cl_active_tracers )
{
	static const float st[4][2] = { { 0.0f, 0.8f }, { 1.0f, 0.8f }, { 1.0f, 0.0f }, { 0.0f, 0.0f } };
	static const int order[4] = { 2, 3, 1, 0 };

	// update tracer color if this is changed
	if( FBitSet( tracerred->flags|tracergreen->flags|tracerblue->flags|traceralpha->flags, FCVAR_CHANGED ))
	{
		color24 *customColors = &gTracerColors[4];
		customColors->r = (byte)(tracerred->value * traceralpha->value * 255);
		customColors->g = (byte)(tracergreen->value * traceralpha->value * 255);
		customColors->b = (byte)(tracerblue->value * traceralpha->value * 255);
		ClearBits( tracerred->flags, FCVAR_CHANGED );
		ClearBits( tracergreen->flags, FCVAR_CHANGED );
		ClearBits( tracerblue->flags, FCVAR_CHANGED );
		ClearBits( traceralpha->flags, FCVAR_CHANGED );
	}

	if( !cl_active_tracers )
		return;	// nothing to draw?

	if( !TriSpriteTexture( gEngfuncs.GetDefaultSprite( REF_DOT_SPRITE ), 0 ))
		return;

	R_AllowFog( false );
	sceGuEnable( GU_BLEND );
	sceGuBlendFunc( GU_ADD, GU_SRC_ALPHA, GU_FIX, 0, GUBLEND1 );
	sceGuDisable( GU_ALPHA_TEST );
	sceGuDepthMask( GU_TRUE ); // no depth writes
	sceGuTexFunc( GU_TFX_MODULATE, GU_TCC_RGBA );

	const float ft = frametime, time = gp_cl->time; // no software doubles per tracer
	float gravity = ft * gp_movevars->gravity;
	float scale = 1.0f - (ft * 0.9f);
	if( scale < 0.0f ) scale = 0.0f;

	for( particle_t *p = cl_active_tracers; p; p = p->next )
	{
		float atten = (p->die - time);
		if( atten > 0.1f ) atten = 0.1f;

		vec3_t start, end, delta;
		VectorScale( p->vel, ( p->ramp * atten ), delta );
		VectorAdd( p->org, delta, end );
		VectorCopy( p->org, start );

		if( !CL_CullTracer( p, start, end ))
		{
			vec3_t	verts[4], tmp2;
			vec3_t	tmp, normal;
			vec3_t	screen, screenLast;

			// Transform point into screen space
			TriWorldToScreen( start, screen );
			TriWorldToScreen( end, screenLast );

			// build world-space normal to screen-space direction vector
			VectorSubtract( screen, screenLast, tmp );

			// we don't need Z, we're in screen space
			tmp[2] = 0;
			VectorNormalize( tmp );

			// build point along normal line (normal is -y, x)
			VectorScale( RI.cull_vup, tmp[0] * gTracerSize[p->type], normal );
			VectorScale( RI.cull_vright, -tmp[1] * gTracerSize[p->type], tmp2 );
			VectorSubtract( normal, tmp2, normal );

			// compute four vertexes
			VectorSubtract( start, normal, verts[0] );
			VectorAdd( start, normal, verts[1] );
			VectorAdd( verts[0], delta, verts[2] );
			VectorAdd( verts[1], delta, verts[3] );

			if( p->color < 0 || p->color >= sizeof( gTracerColors ) / sizeof( gTracerColors[0] ))
			{
				p->color = TRACER_COLORINDEX_DEFAULT;
			}

			color24 color = gTracerColors[p->color];
			sceGuColor( GUCOLOR4UB( color.r, color.g, color.b, p->unused ));

			gu_vert_ftv_t *const out = sceGuGetMemory( sizeof( gu_vert_ftv_t ) * 4 );

			for( int i = 0; i < 4; i++ )
			{
				out[i].u = st[i][0];
				out[i].v = st[i][1];
				out[i].x = verts[order[i]][0];
				out[i].y = verts[order[i]][1];
				out[i].z = verts[order[i]][2];
			}
			sceGuDrawArray( GU_TRIANGLE_FAN, GU_TEXTURE_32BITF | GU_VERTEX_32BITF, 4, 0, out );
		}

		// evaluate position
		VectorMA( p->org, ft, p->vel, p->org );

		if( p->type == pt_grav )
		{
			p->vel[0] *= scale;
			p->vel[1] *= scale;
			p->vel[2] -= gravity;

			p->unused = 255 * (p->die - time) * 2;
			if( p->unused > 255 ) p->unused = 255;
		}
		else if( p->type == pt_slowgrav )
		{
			p->vel[2] = gravity * 0.05f;
		}
	}

	sceGuDepthMask( GU_FALSE );
	R_AllowFog( true );
}

/*
===============
CL_DrawParticlesExternal

allow to draw effects from custom renderer
===============
*/
void CL_DrawParticlesExternal( const ref_viewpass_t *rvp, qboolean trans_pass, float frametime )
{
	ref_instance_t	oldRI = RI;

	R_SetupRefParams( rvp );
	R_SetupFrustum();
	R_SetupGL( false );	// don't touch GL-states
	tr.frametime = frametime;

	gEngfuncs.CL_DrawEFX( frametime, trans_pass );

	// restore internal state
	RI = oldRI;
}
