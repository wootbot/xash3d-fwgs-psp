/*
xash3d_mathlib.c - internal mathlib
Copyright (C) 2010 Uncle Mike

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/
#include "port.h"
#include "xash3d_types.h"
#include "com_model.h"
#include "xash3d_mathlib.h"
#include "studio.h"

static const uint8_t hull_table[] = { 2, 4, 6, 8, 12, 16, 18, 24, 28, 32, 36, 40, 48, 54, 56, 60, 64, 72, 80, 112, 120, 128, 140, 176 };

const int boxpnt[6][4] = {
	{ 0, 4, 6, 2 }, // +X
	{ 0, 1, 5, 4 }, // +Y
	{ 0, 2, 3, 1 }, // +Z
	{ 7, 5, 1, 3 }, // -X
	{ 7, 3, 2, 6 }, // -Y
	{ 7, 6, 4, 5 }, // -Z
};

// pre-quantized table normals from Quake1
const float m_bytenormals[NUMVERTEXNORMALS][3] = {
#include "anorms.h"
};

uint16_t FloatToHalf( float v )
{
	unsigned int i = FloatAsUint( v );
	unsigned int e = ( i >> 23 ) & 0x00ff;
	unsigned int m = i & 0x007fffff;
	unsigned short h;

	if( e <= 127 - 15 )
		h = (( m | 0x00800000 ) >> ( 127 - 14 - e )) >> 13;
	else h = ( i >> 13 ) & 0x3fff;

	h |= ( i >> 16 ) & 0xc000;

	return h;
}

float HalfToFloat( uint16_t h )
{
	unsigned int f = ( h << 16 ) & 0x80000000;
	unsigned int em = h & 0x7fff;

	if( em > 0x03ff )
	{
		f |= ( em << 13 ) + (( 127 - 15 ) << 23 );
	}
	else
	{
		unsigned int m = em & 0x03ff;

		if( m != 0 )
		{
			unsigned int e = ( em >> 10 ) & 0x1f;

			while(( m & 0x0400 ) == 0 )
			{
				m <<= 1;
				e--;
			}

			m &= 0x3ff;
			f |= (( e + ( 127 - 14 )) << 23 ) | ( m << 13 );
		}
	}

	return UintAsFloat( f );
}

/*
=================
RoundHullBound

=================
*/
static float RoundHullBound( float bound, qboolean toward_positive )
{
	qboolean negative = bound < 0.0f;
	float value = fabs( bound );

	if( value == 0.0f )
		return 0.0f;

	if( negative != toward_positive )
	{
		// magnitude grows, snap to the next table value
		value = ceil( value );

		for( size_t i = 0; i < ARRAYSIZE( hull_table ); i++ )
		{
			if( value <= hull_table[i] )
			{
				value = hull_table[i];
				break;
			}
		}
	}
	else
	{
		// magnitude shrinks, snap to the previous table value
		value = floor( value );

		if( value <= hull_table[ARRAYSIZE( hull_table ) - 1] )
		{
			float snapped = 0.0f;

			for( size_t i = 0; i < ARRAYSIZE( hull_table ) && hull_table[i] <= value; i++ )
				snapped = hull_table[i];

			value = snapped;
		}
	}

	return negative ? -value : value;
}

/*
=================
RoundUpHullSize

round the hullsize outwards to nearest 'right' value
=================
*/
void RoundUpHullSize( vec3_t mins, vec3_t maxs )
{
	for( int i = 0; i < 3; i++ )
	{
		mins[i] = RoundHullBound( mins[i], false );
		maxs[i] = RoundHullBound( maxs[i], true );
	}
}

/*
=================
rsqrt
=================
*/
float Q_rsqrt( float number )
{
	if( number == 0.0f )
		return 0.0f;

	float x = number * 0.5f;
	int i = FloatAsInt( number ); // evil floating point bit level hacking
	i = 0x5f3759df - ( i >> 1 ); // what the fuck?
	float y = IntAsFloat( i );
	y = y * ( 1.5f - ( x * y * y )); // first iteration

	return y;
}

void VectorVectors( const vec3_t forward, vec3_t right, vec3_t up )
{
	right[0] = forward[2];
	right[1] = -forward[0];
	right[2] = forward[1];

	float d = DotProduct( forward, right );
	VectorMA( right, -d, forward, right );
	VectorNormalize( right );
	CrossProduct( right, forward, up );
	VectorNormalize( up );
}

/*
=================
VectorAngles

=================
*/
void GAME_EXPORT VectorAngles( const float *forward, float *angles )
{
	if( !forward || !angles )
	{
		if( angles ) VectorClear( angles );
		return;
	}

	float yaw, pitch;

	if( forward[1] == 0 && forward[0] == 0 )
	{
		// fast case
		yaw = 0;
		if( forward[2] > 0 )
			pitch = 90.0f;
		else pitch = 270.0f;
	}
	else
	{
		yaw = ( atan2( forward[1], forward[0] ) * 180 / M_PI_F );
		if( yaw < 0 ) yaw += 360;

		float tmp = sqrt( forward[0] * forward[0] + forward[1] * forward[1] );
		pitch = ( atan2( forward[2], tmp ) * 180 / M_PI_F );
		if( pitch < 0 ) pitch += 360;
	}

	VectorSet( angles, pitch, yaw, 0 );
}

/*
=================
VectorsAngles

=================
*/
void VectorsAngles( const vec3_t forward, const vec3_t right, const vec3_t up, vec3_t angles )
{
	float pitch = -asin( forward[2] );
	float cpitch = cos( pitch );
	float yaw, roll;

	if( fabs( cpitch ) > EQUAL_EPSILON ) // gimball lock?
	{
		cpitch = 1.0f / cpitch;
		pitch = RAD2DEG( pitch );
		yaw = RAD2DEG( atan2( forward[1] * cpitch, forward[0] * cpitch ));
		roll = RAD2DEG( atan2( -right[2] * cpitch, up[2] * cpitch ));
	}
	else
	{
		pitch = forward[2] > 0 ? -90.0f : 90.0f;
		yaw = RAD2DEG( atan2( right[0], -right[1] ));
		roll = 180.0f;
	}

	angles[PITCH] = pitch;
	angles[YAW] = yaw;
	angles[ROLL] = roll;
}

//
// bounds operations
//
/*
=================
SphereIntersect
=================
*/
qboolean SphereIntersect( const vec3_t vSphereCenter, float fSphereRadiusSquared, const vec3_t vLinePt, const vec3_t vLineDir )
{
	// translate sphere to origin.
	vec3_t p;
	VectorSubtract( vLinePt, vSphereCenter, p );

	float a = DotProduct( vLineDir, vLineDir );
	float b = 2.0f * DotProduct( p, vLineDir );
	float c = DotProduct( p, p ) - fSphereRadiusSquared;

	float insideSqr = b * b - 4.0f * a * c;
	if( insideSqr <= 0.000001f )
		return false;
	return true;
}

//
// studio utils
//

/*
====================
QuaternionAlign

make sure quaternions are within 180 degrees of one another,
if not, reverse q
====================
*/
static void QuaternionAlign( const vec4_t p, const vec4_t q, vec4_t qt )
{
	// decide if one of the quaternions is backwards
	float a = 0.0f;
	float b = 0.0f;

	for( int i = 0; i < 4; i++ )
	{
		a += ( p[i] - q[i] ) * ( p[i] - q[i] );
		b += ( p[i] + q[i] ) * ( p[i] + q[i] );
	}

	if( a > b )
	{
		for( int i = 0; i < 4; i++ )
			qt[i] = -q[i];
	}
	else
	{
		for( int i = 0; i < 4; i++ )
			qt[i] = q[i];
	}
}

/*
====================
QuaternionSlerpNoAlign
====================
*/
static void QuaternionSlerpNoAlign( const vec4_t p, const vec4_t q, float t, vec4_t qt )
{
	// 0.0 returns p, 1.0 return q.
	float cosom = p[0] * q[0] + p[1] * q[1] + p[2] * q[2] + p[3] * q[3];
	float sclp, sclq;

	if(( 1.0f + cosom ) > 0.000001f )
	{
		if(( 1.0f - cosom ) > 0.000001f )
		{
			float omega = acos( cosom );
			float sinom = sin( omega );
			sclp = sin(( 1.0f - t ) * omega ) / sinom;
			sclq = sin( t * omega ) / sinom;
		}
		else
		{
			sclp = 1.0f - t;
			sclq = t;
		}

		for( int i = 0; i < 4; i++ )
		{
			qt[i] = sclp * p[i] + sclq * q[i];
		}
	}
	else
	{
		qt[0] = -q[1];
		qt[1] = q[0];
		qt[2] = -q[3];
		qt[3] = q[2];
		sclp = sin(( 1.0f - t ) * ( 0.5f * M_PI_F ));
		sclq = sin( t * ( 0.5f * M_PI_F ));

		for( int i = 0; i < 3; i++ )
		{
			qt[i] = sclp * p[i] + sclq * qt[i];
		}
	}
}

/*
====================
QuaternionSlerp

Quaternion sphereical linear interpolation
====================
*/
#if XASH_PSP
static void PSP_StudioQuaternionSlerp( const vec4_t p, const vec4_t q, float t, vec4_t qt );
#endif

void QuaternionSlerp( const vec4_t p, const vec4_t q, float t, vec4_t qt )
{
#if XASH_PSP
	// rendering only (bone and movement interpolation): the VFPU version below
	PSP_StudioQuaternionSlerp( p, q, t, qt );
	return;
#endif
	vec4_t q2;

	// 0.0 returns p, 1.0 return q.
	// decide if one of the quaternions is backwards
	QuaternionAlign( p, q, q2 );

	QuaternionSlerpNoAlign( p, q2, t, qt );
}

/*
==================
BoxOnPlaneSide

Returns 1, 2, or 1 + 2
==================
*/
int BoxOnPlaneSide( const vec3_t emins, const vec3_t emaxs, const mplane_t *p )
{
	float dist1, dist2;
	int sides = 0;

	// general case
	switch( p->signbits )
	{
	case 0:
		dist1 = p->normal[0] * emaxs[0] + p->normal[1] * emaxs[1] + p->normal[2] * emaxs[2];
		dist2 = p->normal[0] * emins[0] + p->normal[1] * emins[1] + p->normal[2] * emins[2];
		break;
	case 1:
		dist1 = p->normal[0] * emins[0] + p->normal[1] * emaxs[1] + p->normal[2] * emaxs[2];
		dist2 = p->normal[0] * emaxs[0] + p->normal[1] * emins[1] + p->normal[2] * emins[2];
		break;
	case 2:
		dist1 = p->normal[0] * emaxs[0] + p->normal[1] * emins[1] + p->normal[2] * emaxs[2];
		dist2 = p->normal[0] * emins[0] + p->normal[1] * emaxs[1] + p->normal[2] * emins[2];
		break;
	case 3:
		dist1 = p->normal[0] * emins[0] + p->normal[1] * emins[1] + p->normal[2] * emaxs[2];
		dist2 = p->normal[0] * emaxs[0] + p->normal[1] * emaxs[1] + p->normal[2] * emins[2];
		break;
	case 4:
		dist1 = p->normal[0] * emaxs[0] + p->normal[1] * emaxs[1] + p->normal[2] * emins[2];
		dist2 = p->normal[0] * emins[0] + p->normal[1] * emins[1] + p->normal[2] * emaxs[2];
		break;
	case 5:
		dist1 = p->normal[0] * emins[0] + p->normal[1] * emaxs[1] + p->normal[2] * emins[2];
		dist2 = p->normal[0] * emaxs[0] + p->normal[1] * emins[1] + p->normal[2] * emaxs[2];
		break;
	case 6:
		dist1 = p->normal[0] * emaxs[0] + p->normal[1] * emins[1] + p->normal[2] * emins[2];
		dist2 = p->normal[0] * emins[0] + p->normal[1] * emaxs[1] + p->normal[2] * emaxs[2];
		break;
	case 7:
		dist1 = p->normal[0] * emins[0] + p->normal[1] * emins[1] + p->normal[2] * emins[2];
		dist2 = p->normal[0] * emaxs[0] + p->normal[1] * emaxs[1] + p->normal[2] * emaxs[2];
		break;
	default:
		// shut up compiler
		dist1 = dist2 = 0;
		break;
	}

	if( dist1 >= p->dist )
		sides = 1;

	if( dist2 < p->dist )
		sides |= 2;

	return sides;
}

#if XASH_PSP
/*
====================
PSP studio bone math

AngleQuaternion and QuaternionSlerp on the VFPU. The generic versions call the
double sin, cos and acos: the PSP FPU has no doubles, so each is a software
call, ~16 of them per animated bone, ~400 us for one soldier's bone setup.
Uses VFPU matrix 5 only (ref_gu keeps state in matrices 6 and 7).
====================
*/
static void PSP_StudioAngleQuaternion( const float *angles, vec4_t q )
{
	// half angles in quarter turns: ( a / 2 ) / ( pi / 2 )
	const float x[3] = { angles[0] * ( 1.0f / M_PI_F ), angles[1] * ( 1.0f / M_PI_F ), angles[2] * ( 1.0f / M_PI_F ) };
	float sn[3], cs[3];

	__asm__ volatile(
		"lv.s    S500, 0(%2)\n"
		"lv.s    S501, 4(%2)\n"
		"lv.s    S502, 8(%2)\n"
		"vsin.t  C510, C500\n"
		"vcos.t  C520, C500\n"
		"sv.s    S510, 0(%0)\n"
		"sv.s    S511, 4(%0)\n"
		"sv.s    S512, 8(%0)\n"
		"sv.s    S520, 0(%1)\n"
		"sv.s    S521, 4(%1)\n"
		"sv.s    S522, 8(%1)\n"
		:: "r"( sn ), "r"( cs ), "r"( x ) : "memory" );

	// as AngleQuaternion( angles, q, true ): angles[ROLL] gives y, [YAW] p, [PITCH] r
	const float sr = sn[0], cr = cs[0];
	const float sp = sn[1], cp = cs[1];
	const float sy = sn[2], cy = cs[2];

	q[0] = sr * cp * cy - cr * sp * sy; // X
	q[1] = cr * sp * cy + sr * cp * sy; // Y
	q[2] = cr * cp * sy - sr * sp * cy; // Z
	q[3] = cr * cp * cy + sr * sp * sy; // W
}

static void PSP_StudioQuaternionSlerp( const vec4_t p, const vec4_t q, float t, vec4_t qt )
{
	// QuaternionAlign flips q when |p - q| > |p + q|, which is when dot( p, q ) < 0
	const float p0 = p[0], p1 = p[1], p2 = p[2], p3 = p[3];
	float q0 = q[0], q1 = q[1], q2 = q[2], q3 = q[3];
	float cosom = p0 * q0 + p1 * q1 + p2 * q2 + p3 * q3;
	float sclp, sclq;

	if( cosom < 0.0f )
	{
		q0 = -q0; q1 = -q1; q2 = -q2; q3 = -q3;
		cosom = -cosom;
	}

	// q is aligned with p now, so cosom >= 0. Below ~2 degrees the VFPU asin is
	// too coarse for sin( omega ), and a plain lerp is off by less than 2e-4 there
	if(( 1.0f - cosom ) > 0.0005f )
	{
		float in[2] = { cosom, t }, out[2];

		__asm__ volatile(
			"lv.s    S500, 0(%1)\n"		// cosom
			"lv.s    S501, 4(%1)\n"		// t
			"vone.s  S502\n"
			"vasin.s S500, S500\n"
			"vsub.s  S510, S502, S500\n"	// omega = acos( cosom ), in quarter turns
			"vsub.s  S503, S502, S501\n"	// 1 - t
			"vmul.s  S511, S503, S510\n"	// ( 1 - t ) * omega
			"vmul.s  S512, S501, S510\n"	// t * omega
			"vsin.t  C510, C510\n"
			"vrcp.s  S510, S510\n"
			"vmul.s  S511, S511, S510\n"
			"vmul.s  S512, S512, S510\n"
			"sv.s    S511, 0(%0)\n"
			"sv.s    S512, 4(%0)\n"
			:: "r"( out ), "r"( in ) : "memory" );
		sclp = out[0];
		sclq = out[1];
	}
	else
	{
		sclp = 1.0f - t;
		sclq = t;
	}

	qt[0] = sclp * p0 + sclq * q0;
	qt[1] = sclp * p1 + sclq * q1;
	qt[2] = sclp * p2 + sclq * q2;
	qt[3] = sclp * p3 + sclq * q3;
}
#endif

// the values of a bone's channels at frame and the next one (3: position only)
static void R_StudioBoneValues( int frame, const mstudiobone_t *pbone, const mstudioanim_t *panim, const float *adj, float *v1, float *v2, int max )
{
	for( int i = 0; i < max; i++ )
	{
		mstudioanimvalue_t *panimvalue = (mstudioanimvalue_t *)((byte *)panim + panim->offset[i] );
		int j = frame;
		float fadj = 0.0f;

		if( pbone->bonecontroller[i] >= 0 && adj != NULL )
			fadj = adj[pbone->bonecontroller[i]];

		if( panim->offset[i] == 0 )
		{
			v1[i] = v2[i] = pbone->value[i] + fadj;
			continue;
		}

		if( panimvalue->num.total < panimvalue->num.valid )
			j = 0;

		while( panimvalue->num.total <= j )
		{
			j -= panimvalue->num.total;
			panimvalue += panimvalue->num.valid + 1;

			if( panimvalue->num.total < panimvalue->num.valid )
				j = 0;
		}

		if( panimvalue->num.valid > j )
		{
			v1[i] = UnalignedShort( &( panimvalue[j + 1].value ));

			if( panimvalue->num.valid > j + 1 )
				v2[i] = UnalignedShort( &( panimvalue[j + 2].value ));
			else if( panimvalue->num.total > j + 1 )
				v2[i] = v1[i];
			else
				v2[i] = UnalignedShort( &( panimvalue[panimvalue->num.valid + 2].value ));
		}
		else
		{
			v1[i] = UnalignedShort( &( panimvalue[panimvalue->num.valid].value ));

			if( panimvalue->num.total > j + 1 )
				v2[i] = v1[i];
			else
				v2[i] = UnalignedShort( &( panimvalue[panimvalue->num.valid + 2].value ));
		}

		v1[i] = pbone->value[i] + v1[i] * pbone->scale[i] + fadj;
		v2[i] = pbone->value[i] + v2[i] * pbone->scale[i] + fadj;
	}

}

void R_StudioCalcBones( int frame, float s, const mstudiobone_t *pbone, const mstudioanim_t *panim, const float *adj, vec3_t pos, vec4_t q )
{
	float v1[6], v2[6];
	int max = q != NULL ? 6 : 3;

	R_StudioBoneValues( frame, pbone, panim, adj, v1, v2, max );

	if( !VectorCompare( v1, v2 ))
		VectorLerp( v1, s, v2, pos );
	else
		VectorCopy( v1, pos );

	if( q != NULL )
	{
		if( !VectorCompare( &v1[3], &v2[3] ))
		{
			vec4_t q1, q2;

#if XASH_PSP
			// neighbouring keyframes are a few degrees apart: interpolating the
			// angles is within ~0.5 degrees of the slerp below 0.25 rad steps,
			// for one quaternion instead of two and a slerp
			const float d0 = v2[3] - v1[3], d1 = v2[4] - v1[4], d2 = v2[5] - v1[5];
			if( fabsf( d0 ) < 0.25f && fabsf( d1 ) < 0.25f && fabsf( d2 ) < 0.25f )
			{
				const vec3_t a = { v1[3] + s * d0, v1[4] + s * d1, v1[5] + s * d2 };
				PSP_StudioAngleQuaternion( a, q );
				return;
			}
			PSP_StudioAngleQuaternion( &v1[3], q1 );
			PSP_StudioAngleQuaternion( &v2[3], q2 );
			PSP_StudioQuaternionSlerp( q1, q2, s, q );
#else
			AngleQuaternion( &v1[3], q1, true );
			AngleQuaternion( &v2[3], q2, true );
			QuaternionSlerp( q1, q2, s, q );
#endif
		}
		else
		{
#if XASH_PSP
			PSP_StudioAngleQuaternion( &v1[3], q );
#else
			AngleQuaternion( &v1[3], q, true );
#endif
		}
	}
}

#if XASH_PSP
/*
====================
PSP_StudioCalcBoneAngles

R_StudioCalcBones with the rotation left as angles for
PSP_StudioAnglesToQuaternions: in ang the half angles in quarter turns
( a / pi, the VFPU's sin and cos unit ). Keyframes more than 0.25 rad apart
need the slerp: then q is set and it returns false.
====================
*/
/*
R_StudioBoneValues with the values loaded as shorts and kept in locals: the
generic version reads each one bytewise through memcpy (UnalignedShort) and
the stack. The animation data is 2 byte aligned in practice; a bone whose
isn't goes the generic way.
*/
void PSP_StudioBoneValues( int frame, const mstudiobone_t *pbone, const mstudioanim_t *panim, const float *adj, float *v1, float *v2 )
{
	if( (uintptr_t)panim & 1 )
	{
		R_StudioBoneValues( frame, pbone, panim, adj, v1, v2, 6 );
		return;
	}

	for( int i = 0; i < 6; i++ )
	{
		const uint ofs = panim->offset[i];
		const int ctl = pbone->bonecontroller[i];
		const float fadj = ( ctl >= 0 && adj != NULL ) ? adj[ctl] : 0.0f;
		const float value = pbone->value[i];

		if( ofs == 0 )
		{
			v1[i] = v2[i] = value + fadj;
			continue;
		}

		if( ofs & 1 )
		{
			R_StudioBoneValues( frame, pbone, panim, adj, v1, v2, 6 );
			return;
		}

		const mstudioanimvalue_t *pv = (const mstudioanimvalue_t *)((const byte *)panim + ofs );
		int valid = pv->num.valid, total = pv->num.total;
		int j = total < valid ? 0 : frame;

		while( total <= j )
		{
			j -= total;
			pv += valid + 1;
			valid = pv->num.valid;
			total = pv->num.total;
			if( total < valid )
				j = 0;
		}

		int a, b;
		if( valid > j )
		{
			a = pv[j + 1].value;
			if( valid > j + 1 )
				b = pv[j + 2].value;
			else if( total > j + 1 )
				b = a;
			else
				b = pv[valid + 2].value;
		}
		else
		{
			a = pv[valid].value;
			b = total > j + 1 ? a : pv[valid + 2].value;
		}

		const float scale = pbone->scale[i];
		v1[i] = value + (float)a * scale + fadj;
		v2[i] = value + (float)b * scale + fadj;
	}
}

qboolean PSP_StudioCalcBoneAngles( int frame, float s, const mstudiobone_t *pbone, const mstudioanim_t *panim, const float *adj, vec3_t pos, float *ang, vec4_t q )
{
	float v1[6], v2[6];

	PSP_StudioBoneValues( frame, pbone, panim, adj, v1, v2 );

	if( !VectorCompare( v1, v2 ))
		VectorLerp( v1, s, v2, pos );
	else
		VectorCopy( v1, pos );

	const float d0 = v2[3] - v1[3], d1 = v2[4] - v1[4], d2 = v2[5] - v1[5];

	if( fabsf( d0 ) < 0.25f && fabsf( d1 ) < 0.25f && fabsf( d2 ) < 0.25f )
	{
		ang[0] = ( v1[3] + s * d0 ) * ( 1.0f / M_PI_F );
		ang[1] = ( v1[4] + s * d1 ) * ( 1.0f / M_PI_F );
		ang[2] = ( v1[5] + s * d2 ) * ( 1.0f / M_PI_F );
		return true;
	}

	vec4_t q1, q2;
	PSP_StudioAngleQuaternion( &v1[3], q1 );
	PSP_StudioAngleQuaternion( &v2[3], q2 );
	PSP_StudioQuaternionSlerp( q1, q2, s, q );
	return false;
}

/*
====================
PSP_StudioAnglesToQuaternions

PSP_StudioAngleQuaternion for 4 bones at a time on the VFPU: the angles of
4 bones are the columns of a matrix, so each row is one angle of all 4 and
one vsin.q or vcos.q covers it, and the products are quad multiplies. One
bone at a time, the VFPU's latencies and the moves between it and the FPU
were most of the ~0.5 us a bone. ang: 16 byte aligned, n rounded up to 4
rows readable, and q that many writable. Uses VFPU matrices 0-5 (ref_gu
keeps state in 6 and 7).
====================
*/
void PSP_StudioAnglesToQuaternions( const float (*ang)[4], vec4_t *q, int n )
{
	for( int i = 0; i < n; i += 4 )
	{
		__asm__ volatile(
			"lv.q    C500, 0(%1)\n"		// columns: bones, rows: roll, pitch, yaw
			"lv.q    C510, 16(%1)\n"
			"lv.q    C520, 32(%1)\n"
			"lv.q    C530, 48(%1)\n"
			"vsin.q  R400, R500\n"		// sr
			"vsin.q  R401, R501\n"		// sp
			"vsin.q  R402, R502\n"		// sy
			"vcos.q  R300, R500\n"		// cr
			"vcos.q  R301, R501\n"		// cp
			"vcos.q  R302, R502\n"		// cy
			"vmul.q  R200, R301, R302\n"	// cp cy
			"vmul.q  R201, R401, R402\n"	// sp sy
			"vmul.q  R202, R301, R402\n"	// cp sy
			"vmul.q  R203, R401, R302\n"	// sp cy
			"vmul.q  R100, R400, R200\n"	// x = sr cp cy - cr sp sy
			"vmul.q  R000, R300, R201\n"
			"vmul.q  R101, R300, R203\n"	// y = cr sp cy + sr cp sy
			"vmul.q  R001, R400, R202\n"
			"vmul.q  R102, R300, R202\n"	// z = cr cp sy - sr sp cy
			"vmul.q  R002, R400, R203\n"
			"vmul.q  R103, R300, R200\n"	// w = cr cp cy + sr sp sy
			"vmul.q  R003, R400, R201\n"
			"vsub.q  R100, R100, R000\n"
			"vadd.q  R101, R101, R001\n"
			"vsub.q  R102, R102, R002\n"
			"vadd.q  R103, R103, R003\n"
			"usv.q   C100, 0(%0)\n"		// columns: the bones' quaternions
			"usv.q   C110, 16(%0)\n"
			"usv.q   C120, 32(%0)\n"
			"usv.q   C130, 48(%0)\n"
			:: "r"( q[i] ), "r"( ang[i] ) : "memory" );
	}
}
#endif
