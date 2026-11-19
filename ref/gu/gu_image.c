/*
gu_image.c - texture uploading and processing
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
#include <malloc.h>
#include <stdarg.h>
#include "gu_local.h"
#include "crclib.h"

#define TEXTURES_HASH_SIZE	(MAX_TEXTURES >> 2)

static gl_texture_t	gl_textures[MAX_TEXTURES];
static gl_texture_t*	gl_texturesHashTable[TEXTURES_HASH_SIZE];
static uint		gl_numTextures;
static byte*		gl_palette_332 = NULL;

/*
	CLUTs are 16-bit, the format is picked per texture by the alpha the palette needs
	(5650 opaque, 5551 one bit, 4444 gradients), they all cost the same.
	num_colors = PALETTE_BLOCKS x 16
*/
#define PALETTE_SIZE	( 2 * 256 )
#define PALETTE_BLOCKS	16
#define PALETTE_SIZE_T4	( 2 * 16 )

#define TEXTURE_SIZE_MIN	16

#define IsLightMap( tex )	( FBitSet(( tex )->flags, TF_ATLAS_PAGE ))

/*
=================
R_GetTexture

acess to array elem
=================
*/
gl_texture_t *R_GetTexture( unsigned int texnum )
{
	Assert( texnum < MAX_TEXTURES );
	return &gl_textures[texnum];
}

/*
=================
GL_Bind
=================
*/
void GL_Bind( int tmu, unsigned int texnum )
{
	gl_texture_t	*texture;
	int		i, offset, width, height;

	// missed or invalid texture?
	if( texnum <= 0 || texnum >= MAX_TEXTURES )
	{
		if( texnum != 0 )
			gEngfuncs.Con_DPrintf( S_ERROR "GL_Bind: invalid texturenum %d\n", texnum );
		texnum = tr.defaultTexture;
	}

	if( glState.currentTexture == texnum )
		return;

	texture = &gl_textures[texnum];

	// Set palette
	if( texture->format == GU_PSM_T8 )
	{
		sceGuClutMode( texture->clutFormat, 0, 0xff, 0 );
		sceGuClutLoad( PALETTE_BLOCKS, texture->dstPalette );
	}
	else if( texture->format == GU_PSM_T4 )
	{
		sceGuClutMode( texture->clutFormat, 0, 0x0f, 0 );
		sceGuClutLoad( PALETTE_SIZE_T4 / 32, texture->dstPalette );
	}

	// Set texture parameters
	sceGuTexMode( texture->format, texture->numMips - 1, 0, FBitSet( texture->flags, TF_IMG_SWIZZLED ) ? GU_TRUE : GU_FALSE );
	if( texture->numMips > 1 )
	{
		//sceGuTexFilter( GU_LINEAR_MIPMAP_LINEAR, GU_LINEAR_MIPMAP_LINEAR );
		if( FBitSet( texture->flags, TF_NEAREST ) || gl_texture_nearest.value )
			sceGuTexFilter( GU_NEAREST_MIPMAP_NEAREST, GU_NEAREST );
		else
			sceGuTexFilter( GU_LINEAR_MIPMAP_LINEAR, GU_LINEAR );

		sceGuTexLevelMode( gl_texture_lodfunc.value, gl_texture_lodbias.value );
		sceGuTexSlope( gl_texture_lodslope.value );
	}
	else
	{
		if( FBitSet( texture->flags, TF_NEAREST ) || ( IsLightMap( texture ) && gl_lightmap_nearest.value ))
			sceGuTexFilter( GU_NEAREST, GU_NEAREST );
		else
			sceGuTexFilter( GU_LINEAR, GU_LINEAR );
	}

	if( FBitSet( texture->flags, TF_CLAMP ) || FBitSet( texture->flags, TF_BORDER ))
		sceGuTexWrap( GU_CLAMP, GU_CLAMP );
	else
		sceGuTexWrap( GU_REPEAT, GU_REPEAT );

	// Set base texture
	sceGuTexImage( 0, texture->width, texture->height, texture->width, texture->dstTexture );

	// Set mip textures
	if( texture->numMips > 1 )
	{
		offset = texture->width * texture->height * texture->bpp;
		for ( i = 1; i < texture->numMips; i++ )
		{
			width = Q_max( TEXTURE_SIZE_MIN, ( texture->width >> i ));
			height = Q_max( TEXTURE_SIZE_MIN, ( texture->height >> i ));
			sceGuTexImage( i, width, height, width,  &texture->dstTexture[offset] );
			offset += width * height * texture->bpp;
		}
	}
	glState.currentTexture = texnum;
}

/*
==================
GL_CalcImageSize
==================
*/
static size_t GL_CalcImageSize( int format, int width, int height, int depth )
{
	size_t		size = 0;

	// check the depth error
	depth = Q_max( 1, depth );

	switch( format )
	{
	case PF_INDEXED_24:
	case PF_INDEXED_32:
	case PF_LUMINANCE:
	case PF_RGB_332:
		size = width * height * depth;
		break;
	case PF_RGB_5650:
	case PF_RGBA_5551:
	case PF_RGBA_4444:
		size = width * height * depth * 2;
		break;
	case PF_RGB_24:
	case PF_BGR_24:
		size = width * height * depth * 3;
		break;
	case PF_BGRA_32:
	case PF_RGBA_32:
		size = width * height * depth * 4;
		break;
	case PF_DXT1:
		size = (((width + 3) >> 2) * ((height + 3) >> 2) * 8) * depth;
		break;
	case PF_DXT3:
	case PF_DXT5:
	case PF_ATI2:
		size = (((width + 3) >> 2) * ((height + 3) >> 2) * 16) * depth;
		break;
	}

	return size;
}

/*
==================
GL_CalcTextureSize
==================
*/
static size_t GL_CalcTextureSize( int format, int width, int height, byte *outBpp )
{
	size_t		size = 0;
	byte		bpp = 0;

	switch( format )
	{
	case GU_PSM_T4:
	case GU_PSM_DXT1:
		size = (( width * height ) >> 1 );
		break;
	case GU_PSM_T8:
	case GU_PSM_DXT3:
	case GU_PSM_DXT5:
		bpp = 1;
		size = width * height;
		break;
	case GU_PSM_T16:
	case GU_PSM_4444:
	case GU_PSM_5551:
	case GU_PSM_5650:
		bpp = 2;
		size = width * height * 2;
		break;
	case GU_PSM_T32:
	case GU_PSM_8888:
		bpp = 4;
		size = width * height * 4;
		break;
	default:
		gEngfuncs.Host_Error( "GL_CalcTextureSize: bad texture internal format (%u)\n", format );
		break;
	}

	if(outBpp != NULL) *outBpp = bpp;

	return size;
}

static int GL_CalcMipmapCount( gl_texture_t *tex, qboolean haveBuffer, size_t *mipSize )
{
	int		width, height;
	int		mipCount;

	Assert( tex != NULL );

	if( !haveBuffer )
		return 1;


	// generate mip-levels by user request
	if( FBitSet( tex->flags, TF_NOMIPMAP ))
		return 1;

	// 8 levels - 7 + 1 base
	for( mipCount = 1; mipCount < 8; mipCount++ )
	{
		width = Q_max( TEXTURE_SIZE_MIN, ( tex->width >> mipCount ));
		height = Q_max( TEXTURE_SIZE_MIN, ( tex->height >> mipCount ));

		if( width == TEXTURE_SIZE_MIN && height == TEXTURE_SIZE_MIN )
			break;

		// calc without base size
		if( mipSize != NULL )
			*mipSize += GL_CalcTextureSize( tex->format, width, height, NULL );
	}
	return mipCount;
}

/*
================
GL_IsSceneTexture

world, model and world sprite textures: gu_texture_maxsize applies to them
================
*/
static qboolean GL_IsSceneTexture( const gl_texture_t *tex )
{
	if( IsLightMap( tex ))
		return false;

	// "#maps/x.bsp:name.mip" and "x.wad/name.mip" (world), "#models/x.mdl/name.bmp" (studio)
	if( Q_stristr( tex->name, ".mip" ) || Q_stristr( tex->name, ".mdl" ))
		return true;

	// HUD sprites are drawn 1:1
	if( !Q_strnicmp( tex->name, "#HUD/", 5 ))
		return false;

	return Q_stristr( tex->name, ".spr" ) != NULL;
}

/*
================
GL_AllocGEMemory / GL_AllocTextureData

texture data and palettes, VRAM first: texture fetches from main RAM compete
with the CPU for the bus (only ~3% of the frame in a firefight, measured).
Menu images go to RAM, they aren't drawn in game and would take VRAM from the
world and the models.
================
*/
static byte *GL_AllocGEMemory( const gl_texture_t *tex, size_t size, qboolean *inVRAM )
{
	qboolean menuImage = FBitSet( tex->flags, TF_IMAGE ) == TF_IMAGE && !FBitSet( tex->flags, TF_ALLOW_NEAREST )
		&& !IsLightMap( tex ) && Q_strnicmp( tex->name, "#HUD/", 5 ) && !GL_IsSceneTexture( tex );
	byte *data;

	*inVRAM = false;

	if( gu_texture_vram.value && !menuImage )
	{
		data = (byte *)valloc( size );
		if( data )
		{
			*inVRAM = true;
			return data;
		}
	}

	data = (byte *)memalign( 16, size );
	if( !data && gu_texture_vram.value && menuImage )
	{
		data = (byte *)valloc( size );
		*inVRAM = data != NULL;
	}

	return data;
}

static byte *GL_AllocTextureData( gl_texture_t *tex, size_t size )
{
	qboolean inVRAM;
	byte *data = GL_AllocGEMemory( tex, size, &inVRAM );

	if( inVRAM )
		SetBits( tex->flags, TF_IMG_INVRAM );
	else ClearBits( tex->flags, TF_IMG_INVRAM );

	return data;
}

/*
================
GL_SetTextureDimensions
================
*/
static void GL_SetTextureDimensions( gl_texture_t *tex, int width, int height )
{
	int		maxTextureSize = glConfig.max_texture_size;
	int		step = (int)gl_round_down.value;
	int		scaled_width, scaled_height;

	Assert( tex != NULL );

	// store original sizes
	tex->srcWidth = width;
	tex->srcHeight = height;

	for( scaled_width = 1; scaled_width < width; scaled_width <<= 1 );

	if( step > 0 && width < scaled_width && ( step == 1 || ( scaled_width - width ) > ( scaled_width >> step )))
		scaled_width >>= 1;

	for( scaled_height = 1; scaled_height < height; scaled_height <<= 1 );

	if( step > 0 && height < scaled_height && ( step == 1 || ( scaled_height - height ) > ( scaled_height >> step )))
		scaled_height >>= 1;

	width = scaled_width;
	height = scaled_height;

	if( GL_IsSceneTexture( tex ))
		maxTextureSize = bound( TEXTURE_SIZE_MIN, (int)gu_texture_maxsize.value, maxTextureSize );

	if( width > maxTextureSize || height > maxTextureSize )
	{
		while( width > maxTextureSize || height > maxTextureSize )
		{
			width >>= 1;
			height >>= 1;
		}
	}

	// set the texture dimensions
	tex->width  = Q_max( TEXTURE_SIZE_MIN, width );
	tex->height = Q_max( TEXTURE_SIZE_MIN, height );
}

/*
===============
GL_SetTextureFormat
===============
*/
static void GL_SetTextureFormat( gl_texture_t *tex, int format, int channelMask )
{
	qboolean	haveAlpha = ( channelMask & IMAGE_HAS_ALPHA );

	Assert( tex != NULL );

	switch( format )
	{
		case PF_DXT1:	// never use DXT1 with 1-bit alpha
			tex->format = GU_PSM_DXT1;
			break;
		case PF_DXT3:
			tex->format = GU_PSM_DXT3;
			break;
		case PF_DXT5:
			tex->format = GU_PSM_DXT5;
			break;
		case PF_INDEXED_24:
		case PF_INDEXED_32:
		case PF_RGB_332:
			tex->format = GU_PSM_T8;
			break;
		case PF_RGB_5650:
			tex->format = GU_PSM_5650;
			break;
		case PF_RGBA_5551:
			tex->format = GU_PSM_5551;
			break;
		case PF_RGBA_4444:
			tex->format = GU_PSM_4444;
			break;
		case PF_RGBA_32:
		case PF_BGRA_32:
		case PF_RGB_24:
		case PF_BGR_24:
			if ( IsLightMap( tex ))
				tex->format = GU_PSM_8888;
			else if( haveAlpha )
				tex->format = GU_PSM_4444;
			else
				tex->format = GU_PSM_T8; // rgb332
			break;
		default:
			gEngfuncs.Host_Error( "GL_SetTextureFormat: unknown format %i\n", format );
			break;
	}
	/*if( FBitSet( tex->flags, TF_DEPTHMAP ))
	{
		if( FBitSet( tex->flags, TF_ARB_16BIT ))
			tex->format = GL_DEPTH_COMPONENT16;
		else if( FBitSet( tex->flags, TF_ARB_FLOAT ))
			tex->format = GL_DEPTH_COMPONENT32F;
		else tex->format = GL_DEPTH_COMPONENT24;
	}*/
}

/*
=================
GL_ResampleTexture32

Assume input buffer is RGBA
=================
*/
static void GL_ResampleTexture32( const byte *source, int inWidth, int inHeight, byte *dest, int outWidth, int outHeight, qboolean isNormalMap )
{
	uint		frac, fracStep;
	uint		*in = (uint *)source;
	uint		p1[0x1000], p2[0x1000];
	byte		*pix1, *pix2, *pix3, *pix4;
	uint		*out, *inRow1, *inRow2;
	vec3_t		normal;
	int		i, x, y;

	if( !source )
		return;

	fracStep = inWidth * 0x10000 / outWidth;
	out = (uint *)dest;

	frac = fracStep >> 2;
	for( i = 0; i < outWidth; i++ )
	{
		p1[i] = 4 * (frac >> 16);
		frac += fracStep;
	}

	frac = (fracStep >> 2) * 3;
	for( i = 0; i < outWidth; i++ )
	{
		p2[i] = 4 * (frac >> 16);
		frac += fracStep;
	}

	if( isNormalMap )
	{
		for( y = 0; y < outHeight; y++, out += outWidth )
		{
			inRow1 = in + inWidth * (int)(((float)y + 0.25f) * inHeight / outHeight);
			inRow2 = in + inWidth * (int)(((float)y + 0.75f) * inHeight / outHeight);

			for( x = 0; x < outWidth; x++ )
			{
				pix1 = (byte *)inRow1 + p1[x];
				pix2 = (byte *)inRow1 + p2[x];
				pix3 = (byte *)inRow2 + p1[x];
				pix4 = (byte *)inRow2 + p2[x];

				normal[0] = MAKE_SIGNED( pix1[0] ) + MAKE_SIGNED( pix2[0] ) + MAKE_SIGNED( pix3[0] ) + MAKE_SIGNED( pix4[0] );
				normal[1] = MAKE_SIGNED( pix1[1] ) + MAKE_SIGNED( pix2[1] ) + MAKE_SIGNED( pix3[1] ) + MAKE_SIGNED( pix4[1] );
				normal[2] = MAKE_SIGNED( pix1[2] ) + MAKE_SIGNED( pix2[2] ) + MAKE_SIGNED( pix3[2] ) + MAKE_SIGNED( pix4[2] );

				if( !VectorNormalizeLength( normal ))
					VectorSet( normal, 0.5f, 0.5f, 1.0f );

				((byte *)(out+x))[0] = 128 + (byte)(127.0f * normal[0]);
				((byte *)(out+x))[1] = 128 + (byte)(127.0f * normal[1]);
				((byte *)(out+x))[2] = 128 + (byte)(127.0f * normal[2]);
				((byte *)(out+x))[3] = 255;
			}
		}
	}
	else
	{
		for( y = 0; y < outHeight; y++, out += outWidth )
		{
			inRow1 = in + inWidth * (int)(((float)y + 0.25f) * inHeight / outHeight);
			inRow2 = in + inWidth * (int)(((float)y + 0.75f) * inHeight / outHeight);

			for( x = 0; x < outWidth; x++ )
			{
				pix1 = (byte *)inRow1 + p1[x];
				pix2 = (byte *)inRow1 + p2[x];
				pix3 = (byte *)inRow2 + p1[x];
				pix4 = (byte *)inRow2 + p2[x];

				((byte *)(out+x))[0] = (pix1[0] + pix2[0] + pix3[0] + pix4[0]) >> 2;
				((byte *)(out+x))[1] = (pix1[1] + pix2[1] + pix3[1] + pix4[1]) >> 2;
				((byte *)(out+x))[2] = (pix1[2] + pix2[2] + pix3[2] + pix4[2]) >> 2;
				((byte *)(out+x))[3] = (pix1[3] + pix2[3] + pix3[3] + pix4[3]) >> 2;
			}
		}
	}
}

/*
=================
GL_ResampleTexture8

For indexed textures
=================
*/
static void GL_ResampleTexture8(const byte *source, int inWidth, int inHeight, byte *dest, int outWidth, int outHeight)
{
	uint		fracstep;
	byte		*out;
	const byte	*inrow;
	uint		frac;
	int		i, j;

	if( !source )
		return;

	fracstep = inWidth * 0x10000 / outWidth;
	out = dest;

	for (i = 0; i < outHeight ; ++i, out += outWidth)
	{
		inrow	= source + inWidth * (i * inHeight / outHeight);
		frac	= fracstep >> 1;
		for (j = 0; j < outWidth; ++j, frac += fracstep)
		{
			out[j] = inrow[frac >> 16];
		}
	}
}
/*
===============
GL_PixelConverter

32 <-> 16 bit Image converter

Macro for inFormat/outFormat:
PC_HWF - PSP Hardware format mask
PC_SWF - Xash3d Software format mask
===============
*/
void GL_PixelConverter( byte *dst, const byte *src, int size, int inFormat, int outFormat )
{
	int	i;
	byte	color[4];

	color[0] = color[1] = color[2] = color[3] = 0xff;

	for( i = 0; i < size; i++ )
	{
		// unpack
		switch( inFormat )
		{
		case PC_HWF( GU_PSM_5650 ):
			color[0]  = ( *src & 0x1f ) << 3;
			color[1]  = ( *src & 0xe0 ) >> 3; src++;
			color[1] |= ( *src & 0x07 ) << 5;
			color[2]  = ( *src & 0xf8 );      src++;
			color[3]  = 0xff;
			break;
		case PC_HWF( GU_PSM_5551 ):
			color[0]  = ( *src & 0x1f ) << 3;
			color[1]  = ( *src & 0xe0 ) >> 2; src++;
			color[1] |= ( *src & 0x03 ) << 6;
			color[2]  = ( *src & 0x7c ) << 1;
			color[3]  = ( *src & 0x80 ) ? 0xff : 0x00; src++;
			break;
		case PC_HWF( GU_PSM_4444 ):
			color[0]  = ( *src & 0x0f ) << 4;
			color[1]  = ( *src & 0xf0 );      src++;
			color[2]  = ( *src & 0x0f ) << 4;
			color[3]  = ( *src & 0xf0 );      src++;
			break;
		case PC_SWF( PF_INDEXED_24 ): // palette !!!
		case PC_SWF( PF_RGB_24 ):
			color[0] = *src; src++;
			color[1] = *src; src++;
			color[2] = *src; src++;
			color[3] = 0xff;
			break;
		case PC_SWF( PF_BGR_24 ):
			color[2] = *src; src++;
			color[1] = *src; src++;
			color[0] = *src; src++;
			color[3] = 0xff;
			src += 3;
			break;
		case PC_SWF( PF_INDEXED_32 ): // palette !!!
		case PC_SWF( PF_RGBA_32 ):
		case PC_HWF( GU_PSM_8888 ):
			color[0] = *src; src++;
			color[1] = *src; src++;
			color[2] = *src; src++;
			color[3] = *src; src++;
			break;
		case PC_SWF( PF_BGRA_32 ):
			color[2] = *src; src++;
			color[1] = *src; src++;
			color[0] = *src; src++;
			color[3] = *src; src++;
			break;
		default:
			gEngfuncs.Host_Error( "GL_PixelConverter: unknown input format\n");
			break;
		}

		// pack
		switch( outFormat )
		{
		case PC_HWF( GU_PSM_T8 ): // 332, indexing
			*dst  = ( color[0] >> 5 ) & 0x07;
			*dst |= ( color[1] >> 2 ) & 0x38;
			*dst |= ( color[2]      ) & 0xc0; dst++;
			break;
		case PC_HWF( GU_PSM_5650 ):
			*dst  = ( color[0] >> 3 ) & 0x1f;
			*dst |= ( color[1] << 3 ) & 0xe0; dst++;
			*dst  = ( color[1] >> 5 ) & 0x07;
			*dst |= ( color[2]      ) & 0xf8; dst++;
			break;
		case PC_HWF( GU_PSM_5551 ):
			*dst  = ( color[0] >> 3 );
			*dst |= ( color[1] << 2 ) & 0xe0; dst++;
			*dst  = ( color[1] >> 6 ) & 0x03;
			*dst |= ( color[2] >> 1 ) & 0x7c;
			*dst |= ( color[3]      ) & 0x80; dst++;
			break;
		case PC_HWF( GU_PSM_4444 ):
			*dst  = ( color[0] >> 4 ) & 0x0f;
			*dst |= ( color[1]      ) & 0xf0; dst++;
			*dst  = ( color[2] >> 4 ) & 0x0f;
			*dst |= ( color[3]      ) & 0xf0; dst++;
			break;
		case PC_SWF( PF_INDEXED_24 ): // palette !!!
		case PC_SWF( PF_RGB_24 ):
			*dst = color[0]; dst++;
			*dst = color[1]; dst++;
			*dst = color[2]; dst++;
			break;
		case PC_SWF( PF_BGR_24 ):
			*dst = color[2]; dst++;
			*dst = color[1]; dst++;
			*dst = color[0]; dst++;
			break;
		case PC_SWF( PF_INDEXED_32 ): // palette !!!
		case PC_SWF( PF_RGBA_32 ):
		case PC_HWF( GU_PSM_8888 ):
			*dst = color[0]; dst++;
			*dst = color[1]; dst++;
			*dst = color[2]; dst++;
			*dst = color[3]; dst++;
			break;
		case PC_SWF( PF_BGRA_32 ):
			*dst = color[3]; dst++;
			*dst = color[2]; dst++;
			*dst = color[1]; dst++;
			*dst = color[0]; dst++;
			break;
		default:
			gEngfuncs.Host_Error( "GL_PixelConverter: unknown output format\n");
			break;
		}
	}
}

/*
===============
GL_TextureSwizzle

fast swizzling
===============
*/
static void GL_TextureSwizzle( byte *dst, const byte *src, uint width, uint height )
{
	uint		blockx, blocky;
	uint		j;
	uint		width_blocks = ( width / 16 );
	uint		height_blocks = ( height / 8 );
	uint		src_pitch = ( width - 16 ) / 4;
	uint		src_row = width * 8;
	const byte*	ysrc = src;
	uint*		dst32 = ( uint* )dst;

	for ( blocky = 0; blocky < height_blocks; ++blocky )
	{
		const byte* xsrc = ysrc;
		for ( blockx = 0; blockx < width_blocks; ++blockx )
		{
			const int* src32 = ( uint* )xsrc;
			for ( j = 0; j < 8; ++j )
			{
				*( dst32++ ) = *( src32++ );
				*( dst32++ ) = *( src32++ );
				*( dst32++ ) = *( src32++ );
				*( dst32++ ) = *( src32++ );
				src32 += src_pitch;
			}
			xsrc += 16;
		}
		ysrc += src_row;
	}
}

/*
===============
GL_AllocPalette

private CLUT of the texture, VRAM first
===============
*/
static void GL_AllocPalette( gl_texture_t *tex )
{
	if( tex->dstPalette && tex->dstPalette != gl_palette_332 )
		return;

	qboolean inVRAM; // GL_FreePalette checks the address

	tex->dstPalette = GL_AllocGEMemory( tex, PALETTE_SIZE, &inVRAM );
	if( !tex->dstPalette )
		gEngfuncs.Host_Error( "%s: %s out of memory for palette\n", __func__, tex->name );
}

/*
===============
GL_FreePalette / GL_Use332Palette
===============
*/
static void GL_FreePalette( gl_texture_t *tex )
{
	if( tex->dstPalette && tex->dstPalette != gl_palette_332 )
	{
		if( vchkptr( tex->dstPalette ))
			vfree( tex->dstPalette );
		else free( tex->dstPalette );
	}
	tex->dstPalette = NULL;
}

static void GL_Use332Palette( gl_texture_t *tex )
{
	GL_FreePalette( tex );
	tex->dstPalette = gl_palette_332;
	tex->clutFormat = GU_PSM_5650;
}

/*
===============
GL_UploadPalette

store an RGBA32 palette in the 16-bit CLUT format with the most colour bits
that still keeps its alpha: 5650 opaque, 5551 one bit, 4444 gradients
===============
*/
static void GL_UploadPalette( gl_texture_t *tex, const byte *rgba, int numColors )
{
	qboolean opaque = true, onebit = true;
	int i;

	for( i = 0; i < numColors; i++ )
	{
		const byte a = rgba[i * 4 + 3];

		if( a != 0xff )
			opaque = false;
		if( a != 0xff && a != 0x00 )
			onebit = false;
	}

	if( opaque )
		tex->clutFormat = GU_PSM_5650;
	else if( onebit )
		tex->clutFormat = GU_PSM_5551;
	else tex->clutFormat = GU_PSM_4444;

	GL_AllocPalette( tex );
	GL_PixelConverter( tex->dstPalette, rgba, numColors, PC_SWF( PF_RGBA_32 ), PC_HWF( tex->clutFormat ));
	sceKernelDcacheWritebackRange( tex->dstPalette, PALETTE_SIZE );
}

/*
===============
GL_SingleColorAlpha

fonts and similar images are one colour with varying alpha: a 16 entry CLUT of
alpha levels (T4) holds them as well as 4444 does, at a quarter of the size
===============
*/
static qboolean GL_SingleColorAlpha( const rgbdata_t *pic, byte color[4] )
{
	const byte *p = pic->buffer;
	qboolean found = false;
	int i;

	if( pic->type != PF_RGBA_32 && pic->type != PF_BGRA_32 )
		return false;

	if( !FBitSet( pic->flags, IMAGE_HAS_ALPHA ) || FBitSet( pic->flags, IMAGE_PREMULTIPLIED ))
		return false;

	for( i = 0; i < pic->width * pic->height; i++, p += 4 )
	{
		if(( p[3] >> 4 ) == 0 )
			continue; // invisible in 4444 too

		if( !found )
		{
			color[0] = p[0];
			color[1] = p[1];
			color[2] = p[2];
			found = true;
		}
		else if((( p[0] ^ color[0] ) | ( p[1] ^ color[1] ) | ( p[2] ^ color[2] )) & 0xf0 )
			return false; // differs in 4444
	}

	if( pic->type == PF_BGRA_32 )
	{
		const byte t = color[0];
		color[0] = color[2];
		color[2] = t;
	}
	color[3] = 0xff;

	return found;
}

/*
===============
GL_PackAlphaT4

RGBA32 (or BGRA32) -> 4-bit alpha level indices, the first pixel in the low nibble
===============
*/
static void GL_PackAlphaT4( gl_texture_t *tex, const byte *src, byte *dst, const byte color[4] )
{
	byte pal[16 * 4];
	int i;

	for( i = 0; i < 16; i++ )
	{
		pal[i * 4 + 0] = color[0];
		pal[i * 4 + 1] = color[1];
		pal[i * 4 + 2] = color[2];
		pal[i * 4 + 3] = i * 17;
	}
	GL_UploadPalette( tex, pal, 16 );

	for( i = 0; i < tex->width * tex->height; i += 2, src += 8 )
		*dst++ = ( src[3] >> 4 ) | ( src[7] & 0xf0 );
}

/*
===============
GL_QuantizeTexture

RGBA32 (or BGRA32) without alpha -> 8-bit indices with a palette made for the image
(NeuQuant from imagelib), much better than the fixed 332 palette at the same size
===============
*/
static qboolean GL_QuantizeTexture( gl_texture_t *tex, int type, const byte *src, byte *dst )
{
	rgbdata_t in = { 0 }, *out;
	byte pal[256 * 4];
	int i;

	if( type != PF_RGBA_32 && type != PF_BGRA_32 )
		return false;

	in.width = tex->width;
	in.height = tex->height;
	in.type = type;
	in.flags = IMAGE_HAS_COLOR;
	in.size = tex->width * tex->height * 4;
	in.buffer = (byte *)src;

	out = gEngfuncs.FS_CopyImage( &in ); // quantizer works in place on imagelib memory
	if( !out )
		return false;

	gEngfuncs.Image_Process( &out, 0, 0, IMAGE_QUANTIZE, 0.0f );
	if( out->type != PF_INDEXED_24 || !out->palette )
	{
		gEngfuncs.FS_FreeImage( out );
		return false;
	}

	for( i = 0; i < 256; i++ )
	{
		const qboolean bgr = ( type == PF_BGRA_32 );

		pal[i * 4 + 0] = out->palette[i * 3 + ( bgr ? 2 : 0 )];
		pal[i * 4 + 1] = out->palette[i * 3 + 1];
		pal[i * 4 + 2] = out->palette[i * 3 + ( bgr ? 0 : 2 )];
		pal[i * 4 + 3] = 0xff;
	}
	GL_UploadPalette( tex, pal, 256 );
	memcpy( dst, out->buffer, tex->width * tex->height );
	gEngfuncs.FS_FreeImage( out );

	return true;
}

/*
===============
GL_UploadTexture

upload texture into memory
===============
*/
static qboolean GL_UploadTexture( gl_texture_t *tex, rgbdata_t *pic )
{
	size_t		texSize;
	uint		width, height;
	uint		i;
	uint		offset, mipOffset;
	qboolean	normalMap;
	qboolean	swizzle;
	byte		*tempBuff;
	byte		alphaColor[4];

	// dedicated server
	if( !glw_state.initialized )
		return true;

	Assert( pic != NULL );
	Assert( tex != NULL );

	GL_SetTextureDimensions( tex, pic->width, pic->height );
	GL_SetTextureFormat( tex, pic->type, pic->flags );

	// one colour with alpha (fonts): 4-bit alpha levels instead of 4444
	if( tex->format == GU_PSM_4444 && pic->buffer && !IsLightMap( tex ) && GL_SingleColorAlpha( pic, alphaColor ))
		tex->format = GU_PSM_T4;

	tex->fogParams[0] = pic->fogParams[0];
	tex->fogParams[1] = pic->fogParams[1];
	tex->fogParams[2] = pic->fogParams[2];
	tex->fogParams[3] = pic->fogParams[3];

	if(( pic->width * pic->height ) & 3 )
	{
		// will be resampled, just tell me for debug targets
		gEngfuncs.Con_Reportf( "GL_UploadTexture: %s s&3 [%d x %d]\n", tex->name, pic->width, pic->height );
	}

	if( !pic->buffer )
		return true;

	// Prepare sizes
	offset = GL_CalcImageSize( pic->type, pic->width, pic->height, /* pic->depth */ 1 );
	texSize = GL_CalcTextureSize( tex->format, tex->width, tex->height, &tex->bpp );
	normalMap = FBitSet( tex->flags, TF_NORMALMAP ) ? true : false;
	tex->numMips = ImageIND( pic->type ) ? GL_CalcMipmapCount( tex, ( pic->buffer != NULL ), &texSize ) : 1;
	// swizzling works on 16 byte x 8 row blocks
	swizzle = !IsLightMap( tex ) && ( GL_CalcTextureSize( tex->format, tex->width, 1, NULL ) % 16 ) == 0 && ( tex->height % 8 ) == 0;
	mipOffset = 0;

	// Check allocation size
	if( tex->dstTexture && ( tex->size != texSize ) )
	{
		if( FBitSet( tex->flags, TF_IMG_INVRAM ) )
			vfree( tex->dstTexture );
		else
			free( tex->dstTexture );

		tex->dstTexture = NULL;
		ClearBits( tex->flags, TF_IMG_INVRAM );
	}

	// already allocated?
	if( !tex->dstTexture )
	{
		tex->dstTexture = GL_AllocTextureData( tex, texSize );
		if( !tex->dstTexture )
			gEngfuncs.Host_Error( "GL_UploadTexture: %s out of memory for texture ( %u )\n", tex->name, (uint)texSize );
	}

	if( ImageCompressed( pic->type ) )
	{
		memcpy( tex->dstTexture, pic->buffer, texSize );
	}
	else if( ImageIND( pic->type ) || pic->type == PF_RGB_332 )
	{
		if( pic->type != PF_RGB_332 )
		{
			byte rgba[256 * 4];

			GL_PixelConverter( rgba, pic->palette, 256, PC_SWF( pic->type ), PC_SWF( PF_RGBA_32 ));

			// 32-bit palettes carry garbage alpha when the image has none (e.g. BMP reserved
			// bytes are 0); ref_gl uploads such images without an alpha channel, do the same
			if( !FBitSet( pic->flags, IMAGE_HAS_ALPHA ))
			{
				for( i = 0; i < 256; i++ )
					rgba[i * 4 + 3] = 0xff;
			}

			GL_UploadPalette( tex, rgba, 256 );
		}
		else GL_Use332Palette( tex );

		// Volatile memory for temporary buffer
		if( swizzle )
		{
			tempBuff = memalign( 16, texSize );
			if( !tempBuff ) gEngfuncs.Host_Error( "GL_UploadTexture: temporary memory error\n" );
		}

		// Load base + mip texture
		for( i = 0; i < tex->numMips; i++ )
		{
			width = Q_max( TEXTURE_SIZE_MIN, ( tex->width >> i ) );
			height = Q_max( TEXTURE_SIZE_MIN, ( tex->height >> i ) );

			if(( pic->width != width ) || ( pic->height != height ))
				GL_ResampleTexture8( pic->buffer, pic->width, pic->height, swizzle ? tempBuff : ( tex->dstTexture + mipOffset ), width, height );
			else memcpy( swizzle ? tempBuff : ( tex->dstTexture + mipOffset ), pic->buffer, offset );

			if(swizzle) GL_TextureSwizzle( tex->dstTexture + mipOffset, tempBuff, width, height );
			mipOffset += GL_CalcTextureSize( tex->format, width, height, NULL );
		}

		if( swizzle )
		{
			free( tempBuff );
			SetBits( tex->flags, TF_IMG_SWIZZLED );
		}
	}
	else if( pic->type == PF_RGB_5650 || pic->type == PF_RGBA_5551 || pic->type == PF_RGBA_4444 )
	{
		if( swizzle )
		{
			tempBuff = memalign( 16, texSize );
			if( !tempBuff ) gEngfuncs.Host_Error( "GL_UploadTexture: temporary memory error\n" );
		}

		if(( pic->width != tex->width ) || ( pic->height != tex->height ))
		{
			for( i = 0; i < tex->height; i++ )
			{
				memcpy( swizzle ? ( tempBuff + tex->width * tex->bpp * i ) : ( tex->dstTexture + tex->width * tex->bpp * i ),
					( pic->buffer + pic->width * tex->bpp * i ), pic->width * tex->bpp );
			}
		}
		else memcpy( swizzle ? tempBuff : tex->dstTexture, pic->buffer, offset );

		if( swizzle )
		{
			GL_TextureSwizzle( tex->dstTexture, tempBuff, tex->width * tex->bpp, tex->height );
			free( tempBuff );
			SetBits( tex->flags, TF_IMG_SWIZZLED );
		}
	}
	else // RGBA32
	{
		if(( pic->width != tex->width ) || ( pic->height != tex->height ))
			offset = tex->width * tex->height * 4; // src size

		// 32-bit source (offset bytes), then the packed texture to swizzle from
		tempBuff = memalign( 16, swizzle ? ( texSize + offset ) : offset );
		if( !tempBuff ) gEngfuncs.Host_Error( "GL_UploadTexture: temporary memory error\n" );

		if(( pic->width != tex->width ) || ( pic->height != tex->height ))
			GL_ResampleTexture32( pic->buffer, pic->width, pic->height, tempBuff, tex->width, tex->height, normalMap );
		else memcpy( tempBuff, pic->buffer, offset );
/*
		if( !FBitSet( tex->flags, TF_NOMIPMAP ) && FBitSet( pic->flags, IMAGE_ONEBIT_ALPHA ))
			tempBuff = GL_ApplyFilter( tempBuff, tex->width, tex->height );
*/
		if( tex->format == GU_PSM_8888 )
		{
			if ( swizzle )
			{
				GL_TextureSwizzle( tex->dstTexture, tempBuff, tex->width * tex->bpp, tex->height );
				SetBits( tex->flags, TF_IMG_SWIZZLED );
			}
			else memcpy( tex->dstTexture, tempBuff, texSize );
		}
		else
		{
			byte *packed = swizzle ? ( tempBuff + offset ) : tex->dstTexture;

			if( tex->format == GU_PSM_T4 )
				GL_PackAlphaT4( tex, tempBuff, packed, alphaColor );
			else if( tex->format == GU_PSM_T8 && GL_QuantizeTexture( tex, pic->type, tempBuff, packed ))
				; // own palette
			else
			{
				if( tex->format == GU_PSM_T8 )
					GL_Use332Palette( tex );

				GL_PixelConverter( packed, tempBuff, tex->width * tex->height, PC_SWF( pic->type ), PC_HWF( tex->format ) );
			}

			if( swizzle )
			{
				GL_TextureSwizzle( tex->dstTexture, packed, GL_CalcTextureSize( tex->format, tex->width, 1, NULL ), tex->height );
				SetBits( tex->flags, TF_IMG_SWIZZLED );
			}
		}
		free( tempBuff );
	}

	sceKernelDcacheWritebackRange( tex->dstTexture, texSize );
	tex->size = texSize;

	SetBits( tex->flags, TF_IMG_UPLOADED ); // done

	return true;
}

/*
===============
GL_UpdateTexture

===============
*/
qboolean GU_UpdateTextureRect( int texnum, int xoff, int yoff, int width, int height, const void *buffer )
{
	gl_texture_t	*tex;
	byte		*dst, *src;

	// missed or invalid texture?
	if(( texnum <= 0 ) || ( texnum >= MAX_TEXTURES ))
	{
		if( texnum != 0 )
		{
			gEngfuncs.Con_DPrintf( S_ERROR "GL_UpdateTexture: invalid texture num %d\n", texnum );
			return false;
		}
	}
	tex = &gl_textures[texnum];

	if(( tex->width < width + xoff ) || ( tex->height < height + yoff ))
	{
		gEngfuncs.Con_DPrintf( S_ERROR "GL_UpdateTexture: %s invalid update area size XY[%d x %d] WH[%d x %d]\n", tex->name, width, height, xoff, yoff );
		return false;
	}

	if( FBitSet( tex->flags, TF_IMG_SWIZZLED ) || ( FBitSet( tex->flags, TF_IMG_UPLOADED ) && !tex->bpp ))
	{
		// only linear 8, 16 and 32-bit textures (lightmaps) can be patched in place
		gEngfuncs.Con_DPrintf( S_ERROR "GL_UpdateTexture: %s has a packed format\n", tex->name );
		return false;
	}

	if( !FBitSet( tex->flags, TF_IMG_UPLOADED ))
	{
		tex->size	= GL_CalcTextureSize( tex->format, tex->width, tex->height, &tex->bpp );
		tex->numMips	= 1;
		if( tex->format == GU_PSM_T8 )
			GL_Use332Palette( tex ); // default

		tex->dstTexture = GL_AllocTextureData( tex, tex->size );
		if( !tex->dstTexture )
			gEngfuncs.Host_Error( "GL_UpdateTexture: out of memory! ( texture: %i %s )\n", tex->size, tex->name );

		memset( tex->dstTexture, 0x00, tex->size );
		SetBits( tex->flags, TF_IMG_UPLOADED ); // done
	}

	src = ( byte* )buffer;
	dst = tex->dstTexture + ( yoff * tex->width + xoff ) * tex->bpp;
	while( height-- )
	{
		memcpy( dst, src, width * tex->bpp );
		dst += tex->width * tex->bpp;
		src += width * tex->bpp;
	}

	sceKernelDcacheWritebackRange( tex->dstTexture, tex->size );
	return true;
}

/*
===============
GL_ProcessImage

do specified actions on pixels
===============
*/
static void GL_ProcessImage( gl_texture_t *tex, rgbdata_t *pic )
{
	uint		img_flags = 0;

	// force upload texture as RGB or RGBA (detail textures requires this)
	if( tex->flags & TF_FORCE_COLOR ) pic->flags |= IMAGE_HAS_COLOR;
	if( pic->flags & IMAGE_HAS_ALPHA ) tex->flags |= TF_HAS_ALPHA;
	if( FBitSet( pic->flags, IMAGE_PREMULTIPLIED ))
		SetBits( tex->flags, TF_PREMULTIPLIED );

	if( ImageCompressed( pic->type ))
	{
		if( !pic->numMips )
			tex->flags |= TF_NOMIPMAP; // disable mipmapping by user request

		// clear all the unsupported flags
		tex->flags &= ~TF_KEEP_SOURCE;
	}
	else
	{
		// copy flag about luma pixels
		if( pic->flags & IMAGE_HAS_LUMA )
			tex->flags |= TF_HAS_LUMA;

		if( pic->flags & IMAGE_QUAKEPAL )
			tex->flags |= TF_QUAKEPAL;

		// create luma texture from quake texture
		if( tex->flags & TF_MAKELUMA )
		{
			img_flags |= IMAGE_MAKE_LUMA;
			tex->flags &= ~TF_MAKELUMA;
		}

		if( !FBitSet( tex->flags, TF_IMG_UPLOADED ) && FBitSet( tex->flags, TF_KEEP_SOURCE ))
			tex->original = gEngfuncs.FS_CopyImage( pic );

		// indexed images are kept indexed and uploaded as 8-bit CLUT textures

		// processing image before uploading (make luma etc)
		if( pic->buffer ) gEngfuncs.Image_Process( &pic, 0, 0, img_flags, 0 );

		if( FBitSet( tex->flags, TF_LUMINANCE ))
			ClearBits( pic->flags, IMAGE_HAS_COLOR );
	}
}

/*
================
GL_CheckTexName
================
*/
static qboolean GL_CheckTexName( const char *name )
{
	if( !name || COM_StringEmpty( name ))
		return false;

	// because multi-layered textures can exceed name string
	if( Q_strlen( name ) >= sizeof( gl_textures->name ))
	{
		gEngfuncs.Con_Printf( S_ERROR "LoadTexture: too long name %s (%d)\n", name, Q_strlen( name ));
		return false;
	}

	return true;
}

/*
================
GL_TextureForName
================
*/
static gl_texture_t *GL_TextureForName( const char *name )
{
	gl_texture_t	*tex;
	uint		hash;

	// find the texture in array
	hash = COM_HashKey( name, TEXTURES_HASH_SIZE );

	for( tex = gl_texturesHashTable[hash]; tex != NULL; tex = tex->nextHash )
	{
		if( !Q_stricmp( tex->name, name ))
			return tex;
	}

	return NULL;
}

/*
================
GL_AllocTexture
================
*/
static gl_texture_t *GL_AllocTexture( const char *name, texFlags_t flags )
{
	gl_texture_t	*tex;
	uint		i;

	// find a free texture_t slot
	for( i = 0, tex = gl_textures; i < gl_numTextures; i++, tex++ )
		if( !tex->name[0] ) break;

	if( i == gl_numTextures )
	{
		if( gl_numTextures == MAX_TEXTURES )
			gEngfuncs.Host_Error( "GL_AllocTexture: MAX_TEXTURES limit exceeds\n" );
		gl_numTextures++;
	}

	tex = &gl_textures[i];

	// copy initial params
	Q_strncpy( tex->name, name, sizeof( tex->name ));
	tex->flags = flags;
	tex->depth = 1;

	// add to hash table
	tex->hashValue = COM_HashKey( name, TEXTURES_HASH_SIZE );
	tex->nextHash = gl_texturesHashTable[tex->hashValue];
	gl_texturesHashTable[tex->hashValue] = tex;

	return tex;
}

/*
================
GL_DeleteTexture
================
*/
static void GL_DeleteTexture( gl_texture_t *tex )
{
	gl_texture_t	**prev;
	gl_texture_t	*cur;

	Assert( tex != NULL );

	// already freed?
	if( !tex->dstTexture && !tex->name[0] ) return;

	// debug
	if( !tex->name[0] )
	{
		gEngfuncs.Con_Printf( S_ERROR "GL_DeleteTexture: trying to free unnamed texture\n");
		return;
	}

	// remove from hash table
	prev = &gl_texturesHashTable[tex->hashValue];
	while( 1 )
	{
		cur = *prev;
		if( !cur ) break;

		if( cur == tex )
		{
			*prev = cur->nextHash;
			break;
		}
		prev = &cur->nextHash;
	}

	// release source
	if( tex->original )
		gEngfuncs.FS_FreeImage( tex->original );

	GL_FreePalette( tex );

	if( tex->dstTexture )
	{
		if( FBitSet( tex->flags, TF_IMG_INVRAM ) )
			vfree( tex->dstTexture );
		else
			free( tex->dstTexture );
	}
	memset( tex, 0, sizeof( *tex ));
}

/*
================
GL_UpdateTexSize

recalc image room
================
*/
void GL_UpdateTexSize( int texnum, int width, int height, int depth )
{
}

/*
================
GL_LoadTexture
================
*/
int GL_LoadTexture( const char *name, const byte *buf, size_t size, int flags )
{
	gl_texture_t	*tex;
	rgbdata_t	*pic;
	uint		picFlags = 0;

	if( !GL_CheckTexName( name ))
		return 0;

	// see if already loaded
	if(( tex = GL_TextureForName( name )))
		return (tex - gl_textures);

	if( FBitSet( flags, TF_NOFLIP_TGA ))
		SetBits( picFlags, IL_DONTFLIP_TGA );
	SetBits( picFlags, IL_KEEP_8BIT );
	// set some image flags
	gEngfuncs.Image_SetForceFlags( picFlags );

	pic = gEngfuncs.FS_LoadImage( name, buf, size );
	if( !pic ) return 0; // couldn't loading image

	// allocate the new one
	tex = GL_AllocTexture( name, flags );
	GL_ProcessImage( tex, pic );

	if( !GL_UploadTexture( tex, pic ))
	{
		memset( tex, 0, sizeof( gl_texture_t ));
		gEngfuncs.FS_FreeImage( pic ); // release source texture
		return 0;
	}

	gEngfuncs.FS_FreeImage( pic ); // release source texture

	// NOTE: always return texnum as index in array or engine will stop work !!!
	return tex - gl_textures;
}

/*
================
GL_LoadTextureArray
================
*/
int GL_LoadTextureArray( const char **names, int flags )
{
	return 0;
}

/*
================
GL_LoadTextureFromBuffer
================
*/
int GL_LoadTextureFromBuffer( const char *name, rgbdata_t *pic, texFlags_t flags, qboolean update )
{
	gl_texture_t	*tex;

	if( !GL_CheckTexName( name ))
		return 0;

	// see if already loaded
	if(( tex = GL_TextureForName( name )) && !update )
		return (tex - gl_textures);

	// couldn't loading image
	if( !pic ) return 0;

	if( update )
	{
		if( tex == NULL )
			gEngfuncs.Host_Error( "GL_LoadTextureFromBuffer: couldn't find texture %s for update\n", name );
		SetBits( tex->flags, flags );
	}
	else
	{
		// allocate the new one
		tex = GL_AllocTexture( name, flags );
	}

	GL_ProcessImage( tex, pic );

	if( !GL_UploadTexture( tex, pic ))
	{
		memset( tex, 0, sizeof( gl_texture_t ));
		return 0;
	}

	return (tex - gl_textures);
}

/*
================
GL_CreateTexture

creates texture from buffer
================
*/
int GL_CreateTexture( const char *name, int width, int height, const void *buffer, texFlags_t flags )
{
	qboolean	update = FBitSet( flags, TF_UPDATE ) ? true : false;
	int		datasize = 1;
	rgbdata_t	r_empty;

	if( FBitSet( flags, TF_ARB_16BIT ))
		datasize = 2;
	else if( FBitSet( flags, TF_ARB_FLOAT ))
		datasize = 4;

	ClearBits( flags, TF_UPDATE );
	memset( &r_empty, 0, sizeof( r_empty ));
	r_empty.width = width;
	r_empty.height = height;
	r_empty.type = PF_RGBA_32;
	r_empty.size = r_empty.width * r_empty.height * datasize * 4;
	r_empty.buffer = (byte *)buffer;

	// clear invalid combinations
	ClearBits( flags, TF_TEXTURE_3D );

	// if image not luminance and not alphacontrast it will have color
	if( !FBitSet( flags, TF_LUMINANCE ) && !FBitSet( flags, TF_ALPHACONTRAST ))
		SetBits( r_empty.flags, IMAGE_HAS_COLOR );

	if( FBitSet( flags, TF_HAS_ALPHA ))
		SetBits( r_empty.flags, IMAGE_HAS_ALPHA );
	if( FBitSet( flags, TF_CUBEMAP ))
		return 0;

	int texnum = GL_LoadTextureFromBuffer( name, &r_empty, flags, update );

	// builtin textures are created by the engine
	if( !Q_strcmp( name, REF_DEFAULT_TEXTURE ))
		tr.defaultTexture = texnum;
	else if( !Q_strcmp( name, REF_PARTICLE_TEXTURE ))
		tr.particleTexture = texnum;
	else if( !Q_strcmp( name, REF_WHITE_TEXTURE ))
		tr.whiteTexture = texnum;
	else if( !Q_strcmp( name, REF_GRAY_TEXTURE ))
		tr.grayTexture = texnum;
	else if( !Q_strcmp( name, REF_BLACK_TEXTURE ))
		tr.blackTexture = texnum;

	return texnum;
}

/*
================
GL_CreateTextureArray

creates texture array from buffer
================
*/
int GL_CreateTextureArray( const char *name, int width, int height, int depth, const void *buffer, texFlags_t flags )
{
	return 0;
}

/*
================
GL_FindTexture
================
*/
int GL_FindTexture( const char *name )
{
	gl_texture_t	*tex;

	if( !GL_CheckTexName( name ))
		return 0;

	// see if already loaded
	if(( tex = GL_TextureForName( name )))
		return (tex - gl_textures);

	return 0;
}

/*
================
GL_FreeTexture
================
*/
void GL_FreeTexture( unsigned int texnum )
{
	// number 0 it's already freed
	if( texnum <= 0 ) return;

	GL_DeleteTexture( &gl_textures[texnum] );
}

/*
================
GL_ProcessTexture
================
*/
void GL_ProcessTexture( int texnum, float gamma, int topColor, int bottomColor )
{
	gl_texture_t	*image;
	rgbdata_t		*pic;
	int		flags = 0;

	if( texnum <= 0 || texnum >= MAX_TEXTURES )
		return; // missed image
	image = &gl_textures[texnum];

	// select mode
	if( gamma != -1.0f )
	{
		flags = IMAGE_LIGHTGAMMA;
	}
	else if( topColor != -1 && bottomColor != -1 )
	{
		flags = IMAGE_REMAP;
	}
	else
	{
		gEngfuncs.Con_Printf( S_ERROR "GL_ProcessTexture: bad operation for %s\n", image->name );
		return;
	}

	if( !image->original )
	{
		gEngfuncs.Con_Printf( S_ERROR "GL_ProcessTexture: no input data for %s\n", image->name );
		return;
	}

	if( ImageCompressed( image->original->type ))
	{
		gEngfuncs.Con_Printf( S_ERROR "GL_ProcessTexture: can't process compressed texture %s\n", image->name );
		return;
	}

	// all the operations makes over the image copy not an original
	pic = gEngfuncs.FS_CopyImage( image->original );
	gEngfuncs.Image_Process( &pic, topColor, bottomColor, flags, 0.0f );

	GL_UploadTexture( image, pic );

	gEngfuncs.FS_FreeImage( pic );
}

/*
================
GL_TexMemory

return size of all uploaded textures
================
*/
int GL_TexMemory( void )
{
	int	i, total = 0;

	for( i = 0; i < gl_numTextures; i++ )
		total += gl_textures[i].size;

	return total;
}

/*
==============================================================================

INTERNAL TEXTURES

==============================================================================
*/
/*
==================
R_InitDlightTexture
==================
*/
void R_InitDlightTexture( void )
{
	rgbdata_t	r_image;

	if( tr.dlightTexture != 0 )
		return; // already initialized

	memset( &r_image, 0, sizeof( r_image ));
	r_image.width = BLOCK_SIZE;
	r_image.height = BLOCK_SIZE;
	r_image.flags = IMAGE_HAS_COLOR;
	r_image.type = LIGHTMAP_FORMAT;
	r_image.size = r_image.width * r_image.height * LIGHTMAP_BPP;

	tr.dlightTexture = GL_LoadTextureInternal( "*dlight", &r_image, TF_NOMIPMAP|TF_CLAMP|TF_ATLAS_PAGE );
}

/*
==================
GL_Create332Palette
==================
*/
static void GL_Create332Palette( void )
{
	gl_palette_332 = ( byte* )valloc( PALETTE_SIZE );
	if( !gl_palette_332 )
	{
		gl_palette_332 = ( byte* )memalign( 16, PALETTE_SIZE );
		if ( !gl_palette_332 )
			gEngfuncs.Host_Error( "GL_Create332Palette: out of memory!\n" );
	}

	// expand to the full range: 7 (or 3 for blue) must become 255, otherwise lightmaps
	// (multiplied with the textures) come out darker and tinted
	byte rgba[256 * 4];

	for( int i = 0; i < 256; i++ )
	{
		rgba[i * 4 + 0] = (( i >> 0 ) & 7 ) * 255 / 7;
		rgba[i * 4 + 1] = (( i >> 3 ) & 7 ) * 255 / 7;
		rgba[i * 4 + 2] = (( i >> 6 ) & 3 ) * 255 / 3;
		rgba[i * 4 + 3] = 0xff;
	}
	GL_PixelConverter( gl_palette_332, rgba, 256, PC_SWF( PF_RGBA_32 ), PC_HWF( GU_PSM_5650 ));
	sceKernelDcacheWritebackRange( gl_palette_332, PALETTE_SIZE );
}


/*
===============
R_TextureList_f
===============
*/
void R_TextureList_f( void )
{
	gl_texture_t	*image;
	int		i, texCount, ram_bytes = 0, vram_bytes = 0;

	gEngfuncs.Con_Printf( "\n" );
	gEngfuncs.Con_Printf( " -id- -w- -h- -size- -fmt- -s- -wrap- -name--------\n" );

	for( i = texCount = 0, image = gl_textures; i < gl_numTextures; i++, image++ )
	{
		if( !image->dstTexture ) continue;

		if( image->dstPalette && image->dstPalette != gl_palette_332 )
		{
			if( vchkptr( image->dstPalette ))
				vram_bytes += PALETTE_SIZE;
			else ram_bytes += PALETTE_SIZE;
		}

		vram_bytes += ( FBitSet( image->flags, TF_IMG_INVRAM ) ) ? image->size : 0;
		ram_bytes += ( FBitSet( image->flags, TF_IMG_INVRAM ) ) ? 0 : image->size;
		texCount++;

		gEngfuncs.Con_Printf( "%4i: ", i );
		gEngfuncs.Con_Printf( "%3i %3i ", image->width, image->height );
		gEngfuncs.Con_Printf( "%12s ", Q_memprint( image->size ));

		switch( image->format )
		{
		case GU_PSM_T4:
			gEngfuncs.Con_Printf( "T4   " );
			break;
		case GU_PSM_T8:
			gEngfuncs.Con_Printf( "T8   " );
			break;
		case GU_PSM_T16:
			gEngfuncs.Con_Printf( "T16  " );
			break;
		case GU_PSM_T32:
			gEngfuncs.Con_Printf( "T32  " );
			break;
		case GU_PSM_DXT1:
			gEngfuncs.Con_Printf( "DXT1 " );
			break;
		case GU_PSM_DXT3:
			gEngfuncs.Con_Printf( "DXT3 " );
			break;
		case GU_PSM_DXT5:
			gEngfuncs.Con_Printf( "DXT5 " );
			break;
		case GU_PSM_4444:
			gEngfuncs.Con_Printf( "4444 " );
			break;
		case GU_PSM_5551:
			gEngfuncs.Con_Printf( "5551 " );
			break;
		case GU_PSM_5650:
			gEngfuncs.Con_Printf( "5650 " );
			break;
		case GU_PSM_8888:
			gEngfuncs.Con_Printf( "8888 " );
			break;
		default:
			gEngfuncs.Con_Printf( " ^1ERR^7  " );
			break;
		}

		gEngfuncs.Con_Printf( "%s ", FBitSet( image->flags, TF_IMG_SWIZZLED ) ? "Y" : "N" );

		if( image->flags & TF_CLAMP )
			gEngfuncs.Con_Printf( "clamp  " );
		else if( image->flags & TF_BORDER )
			gEngfuncs.Con_Printf( "border " );
		else gEngfuncs.Con_Printf( "repeat " );

		gEngfuncs.Con_Printf( "  %s\n", image->name );
	}

	gEngfuncs.Con_Printf( "---------------------------------------------------------\n" );
	gEngfuncs.Con_Printf( "%i total textures\n", texCount );
	gEngfuncs.Con_Printf( "%i max index\n", gl_numTextures );
	gEngfuncs.Con_Printf( "%s total ram memory used\n", Q_memprint( ram_bytes ));
	gEngfuncs.Con_Printf( "%s total vram memory used\n", Q_memprint( vram_bytes ));
	gEngfuncs.Con_Printf( "\n" );
}


/*
===============
R_DumpTexture_f

debugging aid: decode a texture back from its GU format (unswizzle, CLUT lookup)
and save the base level as scrshots/texdump_<num>.png
===============
*/
static void R_DumpTexture_f( void )
{
	if( gEngfuncs.Cmd_Argc() < 2 )
	{
		gEngfuncs.Con_Printf( S_USAGE "gu_dumptexture <texture number from texturelist>\n" );
		return;
	}

	int texnum = Q_atoi( gEngfuncs.Cmd_Argv( 1 ));
	if( texnum <= 0 || texnum >= gl_numTextures || !gl_textures[texnum].dstTexture )
	{
		gEngfuncs.Con_Printf( S_ERROR "%s: bad texture %d\n", __func__, texnum );
		return;
	}

	const gl_texture_t *tex = &gl_textures[texnum];
	int width = tex->width, height = tex->height;
	int stride = GL_CalcTextureSize( tex->format, width, 1, NULL ); // bytes per row

	if( tex->bpp == 0 && tex->format != GU_PSM_T4 )
	{
		gEngfuncs.Con_Printf( S_ERROR "%s: %s has a compressed format\n", __func__, tex->name );
		return;
	}

	// unswizzle: 16 bytes x 8 rows blocks, row major
	byte *linear = malloc( stride * height );
	if( !linear )
		return;

	if( FBitSet( tex->flags, TF_IMG_SWIZZLED ))
	{
		const byte *src = tex->dstTexture;
		int blocks_x = stride / 16;

		for( int by = 0; by < height / 8; by++ )
		{
			for( int bx = 0; bx < blocks_x; bx++ )
			{
				for( int row = 0; row < 8; row++, src += 16 )
					memcpy( &linear[( by * 8 + row ) * stride + bx * 16], src, 16 );
			}
		}
	}
	else memcpy( linear, tex->dstTexture, stride * height );

	rgbdata_t *pic = Mem_Calloc( r_temppool, sizeof( rgbdata_t ));
	pic->width = width;
	pic->height = height;
	pic->type = PF_RGBA_32;
	pic->flags = IMAGE_HAS_COLOR|IMAGE_HAS_ALPHA;
	pic->size = width * height * 4;
	pic->buffer = Mem_Malloc( r_temppool, pic->size );

	if( tex->format == GU_PSM_T8 || tex->format == GU_PSM_T4 )
	{
		// expand the CLUT first, then look the indices up
		const int numColors = tex->format == GU_PSM_T8 ? 256 : 16;
		uint32_t clut[256];

		GL_PixelConverter( (byte *)clut, tex->dstPalette, numColors, PC_HWF( tex->clutFormat ), PC_SWF( PF_RGBA_32 ));
		for( int i = 0; i < width * height; i++ )
		{
			const int index = tex->format == GU_PSM_T8 ? linear[i] : ( linear[i >> 1] >> (( i & 1 ) * 4 )) & 15;
			((uint32_t *)pic->buffer)[i] = clut[index];
		}
	}
	else GL_PixelConverter( pic->buffer, linear, width * height, PC_HWF( tex->format ), PC_SWF( PF_RGBA_32 ));

	free( linear );

	char name[MAX_QPATH];
	Q_snprintf( name, sizeof( name ), "scrshots/texdump_%d.png", texnum );
	if( gEngfuncs.FS_SaveImage( name, pic ))
		gEngfuncs.Con_Printf( "Write %s (%s, %dx%d, format %d%s)\n", name, tex->name, width, height,
			tex->format, FBitSet( tex->flags, TF_IMG_SWIZZLED ) ? ", swizzled" : "" );
	Mem_Free( pic->buffer );
	Mem_Free( pic );
}
/*
===============
R_InitImages
===============
*/
void R_InitImages( void )
{
	memset( gl_textures, 0, sizeof( gl_textures ));
	memset( gl_texturesHashTable, 0, sizeof( gl_texturesHashTable ));
	gl_numTextures = 0;

	// create unused 0-entry
	Q_strncpy( gl_textures->name, "*unused*", sizeof( gl_textures->name ));
	gl_textures->hashValue = COM_HashKey( gl_textures->name, TEXTURES_HASH_SIZE );
	gl_textures->nextHash = gl_texturesHashTable[gl_textures->hashValue];
	gl_texturesHashTable[gl_textures->hashValue] = gl_textures;
	gl_numTextures = 1;

	// create 332 palette
	GL_Create332Palette();


	gEngfuncs.Cmd_AddCommand( "texturelist", R_TextureList_f, "display loaded textures list" );
	gEngfuncs.Cmd_AddCommand( "gu_dumptexture", R_DumpTexture_f, "save a texture as decoded from GU memory (debugging)" );
}

/*
===============
R_ShutdownImages
===============
*/
void R_ShutdownImages( void )
{
	gl_texture_t	*tex;
	int		i;

	gEngfuncs.Cmd_RemoveCommand( "texturelist" );
	gEngfuncs.Cmd_RemoveCommand( "gu_dumptexture" );
	GL_CleanupAllTextureUnits();

	for( i = 0, tex = gl_textures; i < gl_numTextures; i++, tex++ )
		GL_DeleteTexture( tex );

	if( gl_palette_332 )
	{
		if( vchkptr( gl_palette_332 ))
			vfree( gl_palette_332 );
		else free( gl_palette_332 );
	}

	memset( tr.lightmapTextures, 0, sizeof( tr.lightmapTextures ));
	memset( gl_texturesHashTable, 0, sizeof( gl_texturesHashTable ));
	memset( gl_textures, 0, sizeof( gl_textures ));
	gl_numTextures = 0;
}

/*
===============
R_TextureReplacementReport
===============
*/
void R_TextureReplacementReport( const char *modelname, int gl_texturenum, const char *foundpath )
{
	if( host_allow_materials->value != 2.0f )
		return;

	if( gl_texturenum > 0 )
		gEngfuncs.Con_Printf( "Looking for %s tex replacement..." S_GREEN "OK (%s)\n", modelname, foundpath );
	else if( gl_texturenum < 0 )
		gEngfuncs.Con_Printf( "Looking for %s tex replacement..." S_YELLOW "MISS (%s)\n", modelname, foundpath );
	else
		gEngfuncs.Con_Printf( "Looking for %s tex replacement..." S_RED "FAIL (%s)\n", modelname, foundpath );
}

/*
===============
R_SearchForTextureReplacement
===============
*/
qboolean R_SearchForTextureReplacement( char *out, size_t size, const char *modelname, const char *fmt, ... )
{
	va_list ap;
	va_start( ap, fmt );
	int ret = Q_vsnprintf( out, size, fmt, ap );
	va_end( ap );

	if( ret < 0 )
	{
		R_TextureReplacementReport( modelname, -1, "overflow" );
		return false;
	}

	if( gEngfuncs.fsapi->FileExists( out, false ))
		return true;

	R_TextureReplacementReport( modelname, -1, out );
	return false;
}

/*
===============
R_ShowTextures

Draw all the images to the screen, on top of whatever
was there. This is used to test for texture thrashing.
===============
*/
void R_ShowTextures( void )
{
	static qboolean showHelp = true;

	if( !r_showtextures->value )
		return;

	if( showHelp )
	{
		gEngfuncs.CL_CenterPrint( "use '<-' and '->' keys to change atlas page, ESC to quit", 0.25f );
		showHelp = false;
	}

	sceGuClear( GU_COLOR_BUFFER_BIT );

	int charHeight;
	gEngfuncs.Con_DrawStringLen( NULL, NULL, &charHeight );

	const int base_w = 6, base_h = 3;
	const int per_page = base_w * base_h;
	const float w = gpGlobals->width / base_w;
	const float h = gpGlobals->height / base_h - charHeight;

	// find the first texture of the page
	int start = 1, page = 1;
	for( int i = 1, count = 0; i < gl_numTextures && page < r_showtextures->value; i++ )
	{
		if( !gl_textures[i].dstTexture )
			continue;

		if( ++count == per_page )
		{
			count = 0;
			page++;
			start = i + 1;
		}
	}

	GL_SetRenderMode( kRenderTransTexture );

	const rgba_t color = { 255, 255, 255, 255 };
	for( int i = start, k = 0; i < gl_numTextures && k < per_page; i++ )
	{
		const gl_texture_t *image = &gl_textures[i];

		if( !image->dstTexture )
			continue;

		float x = k % base_w * w;
		float y = k / base_w * ( h + charHeight );

		sceGuColor( 0xffffffff );
		R_DrawStretchPic( x, y, w, h, 0.0f, 0.0f, 1.0f, 1.0f, i );

		string shortname;
		COM_FileBase( image->name, shortname, sizeof( shortname ));
		shortname[12] = '\0'; // cutoff too long names, it looks ugly
		gEngfuncs.Con_DrawString( x + 1, y + h, shortname, color );
		k++;
	}

	gEngfuncs.CL_DrawCenterPrint();
}
