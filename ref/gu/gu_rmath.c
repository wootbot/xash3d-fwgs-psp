/*
gu_rmath.c - renderer mathlib
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

// matrix4x4 is row-major (in[row][column]), ScePspFMatrix4 is column-major like OpenGL
void Matrix4x4_ToFMatrix4( const matrix4x4 in, ScePspFMatrix4 *out )
{
	float *o = (float *)out;
	int i, j;

	for( i = 0; i < 4; i++ )
	{
		for( j = 0; j < 4; j++ )
			o[i * 4 + j] = in[j][i];
	}
}

void Matrix4x4_FromFMatrix4( matrix4x4 out, const ScePspFMatrix4 *in )
{
	const float *f = (const float *)in;
	int i, j;

	for( i = 0; i < 4; i++ )
	{
		for( j = 0; j < 4; j++ )
			out[j][i] = f[i * 4 + j];
	}
}
