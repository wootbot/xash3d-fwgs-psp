/*
gu_clipping.c - software clipping implimentation
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

Based on clipping system from PSP Quake by Peter Mackay and Chris Swindle.

*/
/*
	VFPU REGS:

	M700 - current frustum
	M600 - world frustum
	C010 - current plane ( GU_Clip2Plane )
*/

#include "gu_local.h"

#define MAX_CLIPPED_VERTICES	32

// Cache
static ScePspFMatrix4	projection_view_matrix;

// The temporary working buffers.
static gu_vert_t	work_buffer[2][MAX_CLIPPED_VERTICES] __attribute__(( aligned( 16 )));

/*
=================
GU_ClipGetAndStoreFrustum

The GE culls a whole primitive when a vertex lands outside its 4096 pixel
coordinate range (the guard band, +-2048 around the view center); anything
inside it is cut to the screen by the scissor. So only primitives that reach
past the guard band need clipping on the CPU, against planes inside it.
Clipping against the view frustum instead clipped ~65 world polygons a
frame in a firefight on the CPU, of which ~2 reach past the guard band.
The margin covers views not centered at 2048 (a viewport offset by up to
its own width or height).
=================
*/
#define GU_GUARD_X	6.0f	// |x| <= 6 w: 1440 pixels from the center of a 480 pixel wide view
#define GU_GUARD_Y	12.0f	// |y| <= 12 w: 1632 pixels for 272 lines

// the planes in C as well, for GU_ClipBoxInside: distances in world units
static float clip_planes[4][4] __attribute__(( aligned( 16 )));
static float clip_planes_world[4][4] __attribute__(( aligned( 16 )));

static void GU_ClipGetAndStoreFrustum( const ScePspFMatrix4 *matrix )
{
	// ScePspFMatrix4 holds columns: row r of column c is m[c * 4 + r]
	const float *m = (const float *)matrix;
	// bottom ( w + y ), left ( w + x ), right ( w - x ), top ( w - y ), w scaled to the guard band
	static const int axis[4] = { 1, 0, 0, 1 };
	static const float sign[4] = { 1.0f, 1.0f, -1.0f, -1.0f };
	static const float guard[4] = { GU_GUARD_Y, GU_GUARD_X, GU_GUARD_X, GU_GUARD_Y };


	for( int i = 0; i < 4; i++ )
	{
		float p[4];

		for( int c = 0; c < 4; c++ )
			p[c] = guard[i] * m[c * 4 + 3] + sign[i] * m[c * 4 + axis[i]];

		const float len = sqrtf( p[0] * p[0] + p[1] * p[1] + p[2] * p[2] );
		const float scale = len > 0.0f ? 1.0f / len : 0.0f;

		for( int c = 0; c < 4; c++ )
			clip_planes[i][c] = p[c] * scale;
	}

	// planes into the columns of M700
	__asm__ volatile(
		".set		push\n"				// save assembler option
		".set		noreorder\n"			// suppress reordering
		"lv.q		C700,  0(%0)\n"			// BOTTOM
		"lv.q		C710, 16(%0)\n"			// LEFT
		"lv.q		C720, 32(%0)\n"			// RIGHT
		"lv.q		C730, 48(%0)\n"			// TOP
		".set		pop\n"				// Restore assembler option
		::	"r"( clip_planes )
		:	"memory"
	);
}

/*
=================
GU_ClipBoxInside

true if the box is inside all clip planes: none of its polygons need the
per vertex test (one box test against ~5 vertex tests reading vertex data
that is mostly not in the cache)
=================
*/
qboolean GU_ClipBoxInside( const vec3_t mins, const vec3_t maxs )
{
	// the four planes at once from M700 (see GU_ClipIsRequired), for every
	// world and brush surface: the scalar floats were ~0.1 ms a frame more
	int result = 0;

	__asm__ (
		".set		push\n"
		".set		noreorder\n"
		"lv.s		S000, 0(%1)\n"			// C000 = mins
		"lv.s		S001, 4(%1)\n"
		"lv.s		S002, 8(%1)\n"
		"lv.s		S010, 0(%2)\n"			// C010 = maxs
		"lv.s		S011, 4(%2)\n"
		"lv.s		S012, 8(%2)\n"
		"vabs.q		C100, C700\n"			// M100 = | planes |
		"vabs.q		C110, C710\n"
		"vabs.q		C120, C720\n"
		"vabs.q		C130, C730\n"
		"vfim.s		S003, 0.5\n"
		"vadd.t		C020, C000, C010\n"		// C020 = center * 2
		"vsub.t		C030, C010, C000\n"		// C030 = extents * 2
		"vscl.t		C020, C020, S003\n"		// C020 = center
		"vscl.t		C030, C030, S003\n"		// C030 = extents
		"vzero.s	S033\n"
		"vhtfm4.q	C200, M700, C020\n"		// C200 = plane distances of the center
		"vtfm4.q	C210, M100, C030\n"		// C210 = the box's reach along each plane normal
		"vsub.q		C220, C200, C210\n"		// C220 = distances of the nearest corners
		"vzero.q	C230\n"
		"vcmp.q		LT, C220, C230\n"		// any corner behind a plane?
		"bvt		4, 1f\n"
		"nop\n"
		"li		%0, 1\n"
	"1:\n"
		".set		pop\n"
		: "+r"( result )
		: "r"( mins ), "r"( maxs )
	);

	return result;
}

/*
=================
GU_ClipBeginFrame

Calculate the clipping frustum for static objects
=================
*/
void GU_ClipSetWorldFrustum( const matrix4x4 in )
{
	// Get matrix.
	Matrix4x4_ToFMatrix4( in, &projection_view_matrix );

	// Calculate and cache the clipping frustum.
	GU_ClipGetAndStoreFrustum( &projection_view_matrix );

	// Save the clipping frustum.
	memcpy( clip_planes_world, clip_planes, sizeof( clip_planes ));
	__asm__ volatile(
		".set		push\n"				// save assembler option
		".set		noreorder\n"			// suppress reordering
		"vmmov.q	M600, M700\n"			// Save frustum
		".set		pop\n"				// Restore assembler option
	);
}

/*
=================
GU_ClipRestoreWorldFrustum

Restore the clipping frustum
=================
*/
void GU_ClipRestoreWorldFrustum( void )
{
	// Restore the clipping frustum.
	memcpy( clip_planes, clip_planes_world, sizeof( clip_planes ));
	__asm__ volatile(
		".set		push\n"				// save assembler option
		".set		noreorder\n"			// suppress reordering
		"vmmov.q	M700, M600\n"			// Restore frustum
		".set		pop\n"				// Restore assembler option
	);
}

/*
=================
GU_ClipSetModelFrustum

Calculate the clipping frustum for dynamic objects
=================
*/
void GU_ClipSetModelFrustum( const matrix4x4 in )
{
	ScePspFMatrix4	model_matrix;
	ScePspFMatrix4	projection_view_model_matrix;

	// Get matrix.
	Matrix4x4_ToFMatrix4( in, &model_matrix );

	// Combine the matrices (multiply projection-view by model).
	gumMultMatrix( &projection_view_model_matrix, &projection_view_matrix, &model_matrix );

	// Calculate and cache the clipping frustum.
	GU_ClipGetAndStoreFrustum( &projection_view_model_matrix );
}

/*
=================
GU_ClipLoadFrustum

Load the clipping frustum ( native )
=================
*/
#if 0
void GU_ClipLoadFrustum( const mplane_t *plane )
{
	__asm__ volatile(
		".set		push\n"				// save assembler option
		".set		noreorder\n"			// suppress reordering
		"ulv.q		C700, %0\n"			// Load plane into register
		"ulv.q		C710, %1\n"			// Load plane into register
		"ulv.q		C720, %2\n"			// Load plane into register
		"ulv.q		C730, %3\n"			// Load plane into register
		"vneg.q		R703, R703\n"			// R703 = -R703 = -dist
		".set		pop\n"				// Restore assembler option
		::	"m"( plane[FRUSTUM_BOTTOM] ),
			"m"( plane[FRUSTUM_LEFT] ),
			"m"( plane[FRUSTUM_RIGHT] ),
			"m"( plane[FRUSTUM_TOP] )

	);
}
#endif

/*
=================
GU_ClipIsRequired

Is clipping required?
=================
*/
int GU_ClipIsRequired( gu_vert_t* uv, int uvc )
{
	int		result = 1;
	gu_vert_t	*uv_end = uv + ( uvc - 1 );

	__asm__ (
		".set		push\n"				// save assembler option
		".set		noreorder\n"			// suppress reordering
		"vzero.q	C000\n"				// C000 = [0.0f, 0.0f, 0.0f. 0.0f]
	"0:\n"							// loop
		"lv.s		S010,  8(%1)\n"			// S010 = v[i].xyz[0]
		"lv.s		S011,  12(%1)\n"		// S011 = v[i].xyz[1]
		"lv.s		S012,  16(%1)\n"		// S012 = v[i].xyz[2]
		"vhtfm4.q	C020, M700, C010\n"		// C020 = frustrum * v[i].xyz
		"vcmp.q		LT,   C020, C000\n"		// S020 < 0.0f || S021 < 0.0f || S022 < 0.0f || S023 < 0.0f
		"bvt		4,    1f\n"			// if ( CC[4] == 1 ) jump to exit
		"nop\n"						// 								( delay slot )
		"bne		%1,   %2,   0b\n"		// if ( $10 != $8 ) jump to loop
		"addiu		%1,   %1,   %3\n"		// $8 = $8 + sizeof( gu_vert_t )				( delay slot )
		"move		%0,   $0\n"			// res = 0
	"1:\n"							// exit
		".set		pop\n"				// Restore assembler option
		:	"=r"( result ), "+r"( uv )
		:	"r"( uv_end ),
			"n"(sizeof( gu_vert_t ))
		:	"$8"
	);
	return result;
}

/*
=================
GU_Clip2Plane

Clips a polygon against a plane.
=================
*/
static inline void GU_Clip2Plane( gu_vert_t *uv, int uvc, gu_vert_t *cv, int *cvc )
{
	gu_vert_t	*uv_end = uv + ( uvc - 1 );
	gu_vert_t	*cv_start = cv;

	__asm__ (
		".set		push\n"				// save assembler option
		".set		noreorder\n"			// suppress reordering
		"vzero.q	C000\n"				// set zero vector
		"lv.s		S200,  8(%2)\n"			// Load vertex P XYZ(4b) X into register
		"lv.s		S201, 12(%2)\n"			// Load vertex P XYZ(4b) Y into register
		"lv.s		S202, 16(%2)\n"			// Load vertex P XYZ(4b) Z into register
		"lv.s		S210,  0(%2)\n"			// Load vertex P TEX(4b) U into register
		"lv.s		S211,  4(%2)\n"			// Load vertex P TEX(4b) V into register
		"vhdp.q		S212, C200, C010\n"		// distance P  -> dP
	"0:\n"
		"lv.s		S220,  8(%1)\n"			// Load vertex S XYZ(4b) X into register
		"lv.s		S221, 12(%1)\n"			// Load vertex S XYZ(4b) Y into register
		"lv.s		S222, 16(%1)\n"			// Load vertex S XYZ(4b) Z into register
		"lv.s		S230,  0(%1)\n"			// Load vertex S TEX(4b) U into register
		"lv.s		S231,  4(%1)\n"			// Load vertex S TEX(4b) V into register
		"vhdp.q		S232, C220, C010\n"		// distance S -> dS
		"vcmp.s		GT, S212, S000\n"		// if (dP <= 0)
		"bvf		0, 1f\n"			// goto 1:
		"nop\n"
		"sv.s		S210,  0(%0)\n"			// cv->uv[0]  = C020 U
		"sv.s		S211,  4(%0)\n"			// cv->uv[1]  = C021 V
		"sv.s		S200,  8(%0)\n"			// cv->xyz[0] = C030 X
		"sv.s		S201, 12(%0)\n"			// cv->xyz[1] = C031 Y
		"sv.s		S202, 16(%0)\n"			// cv->xyz[2] = C032 Z
		"addiu		%0, %0, %3\n"			// cv + sizeof(gu_vert_t)
	"1:\n"
		"vmul.s		S020, S232, S212\n"		// (dS * dP)
		"vcmp.s		LT, S020, S000\n"		// if (dS * dP < 0)
		"bvf		0, 2f\n"			// goto 2:
		"vsub.s		S021, S232, S212\n"		// (dS - dP)
		"vrcp.s		S021, S021\n"
		"vmul.s		S021, S021, S232\n"		// R = dS / ( dS - dP )
#if 0
		"vsub.t		C200, C220, C200\n"		// (S - P) XYZ
		"vsub.p		C210, C230, C210\n"		// (S - P) UV
		"vscl.t		C200, C200, S021\n"		// ((S - P) * R) XYZ
		"vscl.p		C210, C210, S021\n"		// ((S - P) * R) UV
		"vsub.t		C200, C220, C200\n"		// (S - (S - P) * R) XYZ
		"vsub.p		C210, C230, C210\n"		// (S - (S - P) * R) UV
#else
		"vsub.t		C200, C200, C220\n"		// (P - S) XYZ
		"vsub.p		C210, C210, C230\n"		// (P - S) UV
		"vscl.t		C200, C200, S021\n"		// ((P - S) * R) XYZ
		"vscl.p		C210, C210, S021\n"		// ((P - S) * R) UV
		"vadd.t		C200, C220, C200\n"		// (S + (P - S) * R) XYZ
		"vadd.p		C210, C230, C210\n"		// (S + (P - S) * R) UV
#endif
		"sv.s		S210,  0(%0)\n"			// cv->uv[0]  = S210 U
		"sv.s		S211,  4(%0)\n"			// cv->uv[1]  = S211 V
		"sv.s		S200,  8(%0)\n"			// cv->xyz[0] = S200 X
		"sv.s		S201, 12(%0)\n"			// cv->xyz[1] = S201 Y
		"sv.s		S202, 16(%0)\n"			// cv->xyz[2] = S202 Z
		"addiu		%0, %0, %3\n"			// cv + sizeof(gu_vert_t)
	"2:\n"
		"vmov.t		C200, C220\n"			// P = S XYZ
		"vmov.t		C210, C230\n"			// P = S UV and dS
		"bne		%1, %2, 0b\n"			// if (uv != uv_end) goto 0:
		"addiu		%1, %1, %3\n"			// uv + sizeof(gu_vert_t)						( delay slot )
		".set		pop\n"				// suppress reordering
		:	"+r"( cv ), "+r"( uv )
		:	"r"( uv_end ),
			"n"( sizeof( gu_vert_t ))
		:	"memory"
	) ;
	*cvc = ( cv - cv_start );
}

/*
=================
GU_Clip

Clips a polygon against the frustum
=================
*/
void GU_Clip( gu_vert_t *uv, int uvc, gu_vert_t **cv, int* cvc )
{
	size_t		vc;

	if ( !uvc )	// no vertices to clip?
		gEngfuncs.Host_Error( "GU_Clip: calling clip with zero vertices!" );

	vc = uvc;
	*cvc = 0;

	__asm__ volatile(
		".set		push\n"				// save assembler option
		".set		noreorder\n"			// suppress reordering
		"vmov.q		C010, C700\n"			// CPLANE_BOTTOM
		".set		pop\n"				// suppress reordering
	);
	GU_Clip2Plane( uv,             vc, work_buffer[0], &vc );
	if ( !vc ) return;

	__asm__ volatile(
		".set		push\n"				// save assembler option
		".set		noreorder\n"			// suppress reordering
		"vmov.q		C010, C710\n"			// CPLANE_LEFT
		".set		pop\n"				// suppress reordering
	);
	GU_Clip2Plane( work_buffer[0], vc, work_buffer[1], &vc );
	if ( !vc ) return;

	__asm__ volatile(
		".set		push\n"				// save assembler option
		".set		noreorder\n"			// suppress reordering
		"vmov.q		C010, C720\n"			// CPLANE_RIGHT
		".set		pop\n"				// suppress reordering
	);
	GU_Clip2Plane( work_buffer[1], vc, work_buffer[0], &vc );
	if ( !vc ) return;

	__asm__ volatile(
		".set		push\n"				// save assembler option
		".set		noreorder\n"			// suppress reordering
		"vmov.q		C010, C730\n"			// CPLANE_TOP
		".set		pop\n"				// suppress reordering
	);
	*cv = extGuBeginPacket( NULL ); // uncached
	GU_Clip2Plane( work_buffer[0], vc, *cv, cvc );
	if (!( *cvc )) return;
	extGuEndPacket(( void * )( *cv + *cvc ));
}
