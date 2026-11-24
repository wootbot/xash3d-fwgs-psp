/*
gu_local.h - renderer local declarations
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

#ifndef GU_LOCAL_H
#define GU_LOCAL_H

// pspsdk headers have "()" declarations
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstrict-prototypes"
#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspgu.h>
#include <pspgum.h>
#pragma GCC diagnostic pop

#include "ref_common.h"
#include "port.h"
#include "xash3d_types.h"
#include "cvardef.h"
#include "protocol.h"
#include "gu_frustum.h"
#include "ref_params.h"
#include "enginefeatures.h"
#include "com_strings.h"
#include "gu_helper.h"
#include "gu_extension.h"
#include "gu_vram.h"
#include "wadfile.h"
#include "common/mod_local.h"
#include "pmove.h"

#ifndef offsetof
#define offsetof(s,m) __builtin_offsetof(s,m)
#endif

#include <stdio.h>

// make mod_ref.h?
#define LM_SAMPLE_SIZE             16

// lightmaps are stored in a GU pixel format, 8-bit RGB 332 by default to save VRAM
#define LIGHTMAP_BPP	1 // 1 2 3 4

#if LIGHTMAP_BPP == 1
#define LIGHTMAP_FORMAT	PF_RGB_332
#elif LIGHTMAP_BPP == 2
#define LIGHTMAP_FORMAT	PF_RGB_5650
#elif LIGHTMAP_BPP == 3
#define LIGHTMAP_FORMAT	PF_RGB_24
#elif LIGHTMAP_BPP == 4
#define LIGHTMAP_FORMAT	PF_RGBA_32
#else
#error (1 > LIGHTMAP_BPP > 4)
#endif

#define BLOCK_SIZE		tr.block_size	// lightmap blocksize
#define BLOCK_SIZE_DEFAULT	128		// for keep backward compatibility
#define BLOCK_SIZE_MAX	128

#define MAX_TEXTURES	1536
#define MAX_DETAIL_TEXTURES	64
#define MAX_LIGHTMAPS	64
#define SUBDIVIDE_SIZE	64
#define MAX_DECAL_SURFS	256
#define MAX_DRAW_STACK	2		// normal view and menu view

#define SHADEDOT_QUANT 	16		// precalculated dot products for quantized angles
#define SHADE_LAMBERT	1.4953241f
#define DEFAULT_ALPHATEST	0x00	// GU alpha reference is a byte

#define R_ModelOpaque( rm )	( rm == kRenderNormal )
#define R_StaticEntity( ent )	( VectorIsNull( ent->origin ) && VectorIsNull( ent->angles ))
#define RP_LOCALCLIENT( e )	((e) != NULL && (e)->index == ( gp_cl->playernum + 1 ) && e->player )

#define CL_IsViewEntityLocalPlayer() ( gp_cl->viewentity == ( gp_cl->playernum + 1 ))

#define CULL_VISIBLE	0		// not culled
#define CULL_BACKSIDE	1		// backside of transparent wall
#define CULL_FRUSTUM	2		// culled by frustum
#define CULL_VISFRAME	3		// culled by PVS
#define CULL_OTHER		4		// culled by other reason

#define HACKS_RELATED_HLMODS		// some HL-mods works differently under Xash and can't be fixed without some hacks at least at current time

#define SKYBOX_BASE_NUM 5800 // set skybox base (to let some mods load hi-res skyboxes)

// viewmodel is drawn into this fraction of the depth range so it never clips into walls
#define VIEWMODEL_DEPTH_RANGE 0.3f

typedef struct gltexture_s
{
	char		name[128];	// game path, including extension (can be store image programs)
	word		srcWidth;		// keep unscaled sizes
	word		srcHeight;
	word		width;		// upload width\height
	word		height;
	word		depth;		// always 1, GU has no texture arrays
	byte		numMips;		// mipmap count
	byte		bpp;
	byte		*dstTexture;	// texture data, in VRAM when TF_IMG_INVRAM, swizzled when TF_IMG_SWIZZLED
	byte		*dstPalette;	// CLUT for 4/8-bit textures
	byte		clutFormat;	// GU_PSM_5650, 5551 or 4444, depending on the alpha the palette needs
	word		stride;		// row length in pixels when it's a part of an atlas page, see GL_AliasTexture
	word		page;		// and the page
	word		rows;		// atlas pages: rows stored at the base level (0: all), GL_TextureRows
	int		format;		// uploaded format (pixformat_t)
	texFlags_t	flags;

	rgba_t		fogParams;	// some water textures
					// contain info about underwater fog
	rgbdata_t		*original;	// keep original image

	// debug info
	size_t		size;		// upload size for debug targets

	// detail textures stuff
	float		xscale;
	float		yscale;

	uint		hashValue;
	struct gltexture_s	*nextHash;
} gl_texture_t;

// texture flags private to ref_gu, in the bits engine doesn't use
#define TF_IMG_SWIZZLED	BIT( 29 )
#define TF_IMG_INVRAM	BIT( 30 )

// pixel formats private to ref_gu, after the engine's PF_TOTALCOUNT
#define PF_RGB_332	( PF_TOTALCOUNT + 0 )	// 8-bit  R3 G3 B2
#define PF_RGB_5650	( PF_TOTALCOUNT + 1 )	// 16-bit R5 G6 B5
#define PF_RGBA_5551	( PF_TOTALCOUNT + 2 )	// 16-bit R5 G5 B5 A1
#define PF_RGBA_4444	( PF_TOTALCOUNT + 3 )	// 16-bit R4 G4 B4 A4

// GU vertex formats
typedef struct
{
	float x, y, z;
} gu_vert_fv_t;

typedef struct
{
	unsigned int c;
	float x, y, z;
} gu_vert_fcv_t;

typedef struct
{
	float u, v;
	float x, y, z;
} gu_vert_ftv_t;

typedef struct
{
	float u, v;
	unsigned int c;
	float x, y, z;
} gu_vert_ftcv_t;

typedef struct
{
	float u, v;
	unsigned int c;
	float nx, ny, nz;
	float x, y, z;
} gu_vert_ftcnv_t;

typedef struct
{
	short x, y, z;
} gu_vert_hv_t;

typedef struct
{
	short u, v;
	short x, y, z;
} gu_vert_htv_t;

typedef struct
{
	short u, v;
	unsigned int c;
	short x, y, z;
} gu_vert_htcv_t;

// same layout as GU_TEXTURE_32BITF | GU_VERTEX_32BITF, can be drawn directly
typedef struct
{
	float uv[2];
	float xyz[3];
} gu_vert_t;

// ref_gu stores surface and decal polygons in its own layout instead of glpoly2_t:
// numverts texture vertices followed by numverts lightmap vertices (same xyz, lightmap st)
typedef struct gu_poly_s
{
	struct gu_poly_s	*next;
	struct gu_poly_s	*chain;
	int		numverts;
	int		flags;		// for SURF_UNDERWATER
	gu_vert_t	verts[];	// variable sized (xyz s1t1 + lm(xyz s2t2))
} gu_poly_t;

#define GU_POLYS( x ) ((gu_poly_t *)( x )->polys )
#define GU_POLY_NOCLIP BIT( 30 ) // set while drawing a surface whose box is inside the clip planes
#define GU_POLY_SIZE( numverts ) ( sizeof( gu_poly_t ) + ( numverts ) * 2 * sizeof( gu_vert_t ))

typedef struct
{
	ref_viewpass_t rvp;

	cl_entity_t	*currententity;
	model_t		*currentmodel;
	cl_entity_t	*currentbeam;	// same as above but for beams

	gl_frustum_t	frustum;

	mleaf_t		*viewleaf;
	mleaf_t		*oldviewleaf;
	vec3_t		vforward;
	vec3_t		vright;
	vec3_t		vup;

	vec3_t		cullorigin;
	vec3_t		cull_vforward;
	vec3_t		cull_vright;
	vec3_t		cull_vup;

	float		farClip;

	qboolean		fogCustom;
	qboolean		fogEnabled;
	qboolean		fogSkybox;
	vec4_t		fogColor;
	float		fogDensity;
	float		fogStart;
	float		fogEnd;
	int		cached_contents;	// in water
	int		cached_waterlevel;	// was in water

	float		skyMins[2][SKYBOX_MAX_SIDES];
	float		skyMaxs[2][SKYBOX_MAX_SIDES];

	matrix4x4		objectMatrix;		// currententity matrix
	matrix4x4		worldviewMatrix;		// modelview for world
	matrix4x4		modelviewMatrix;		// worldviewMatrix * objectMatrix

	matrix4x4		projectionMatrix;
	matrix4x4		worldviewProjectionMatrix;	// worldviewMatrix * projectionMatrix
	byte		visbytes[(MAX_MAP_LEAFS+7)/8];// actual PVS for current frame

	float		viewplanedist;
	mplane_t		clipPlane;
} ref_instance_t;

typedef struct
{
	cl_entity_t	*solid_entities[MAX_VISIBLE_PACKET];	// opaque moving or alpha brushes
	cl_entity_t	*trans_entities[MAX_VISIBLE_PACKET];	// translucent brushes
	cl_entity_t	*beam_entities[MAX_VISIBLE_PACKET];
	uint		num_solid_entities;
	uint		num_trans_entities;
	uint		num_beam_entities;
#if XASH_PSP
	// of the solid ones, for the Media Engine
	uint16_t		players[MAX_CLIENTS];
	uint		num_players;
#endif
} draw_list_t;

typedef struct
{
	int		defaultTexture;   	// use for bad textures
	int		particleTexture;
	int		whiteTexture;
	int		grayTexture;
	int		blackTexture;
	int		solidskyTexture;	// quake1 solid-sky layer
	int		alphaskyTexture;	// quake1 alpha-sky layer
	int		lightmapTextures[MAX_LIGHTMAPS];
	int		dlightTexture;	// custom dlight texture
	int		dlightTextures[2];	// one of these, by guRender.vert_frame & 1
	int		skyboxTextures[SKYBOX_MAX_SIDES];	// skybox sides
	int		skytexturenum;	// this not a gl_texturenum!
	int		skyboxbasenum;	// start with 5800

	// entity lists
	draw_list_t	draw_stack[MAX_DRAW_STACK];
	int		draw_stack_pos;
	draw_list_t	*draw_list;

	msurface_t	*draw_decals[MAX_DECAL_SURFS];
	int		num_draw_decals;

	// matrix states
	qboolean		modelviewIdentity;

	int		visframecount;	// PVS frame
	int		dlightframecount;	// dynamic light frame
	int		realframecount;	// not including viewpasses
	int		framecount;

	qboolean		fCustomRendering;
	qboolean		fResetVis;
	qboolean		fFlipViewModel;

	byte		visbytes[(MAX_MAP_LEAFS+7)/8];	// member custom PVS
	int		block_size;			// lightmap blocksize

	double		frametime;	// special frametime for multipass rendering (will set to 0 on a nextview)
	float		blend;		// global blend value

	// cull info
	vec3_t		modelorg;		// relative to viewpoint

	// get from engine
	model_t *worldmodel;
	world_static_t *world;
	cl_entity_t **entchunks;
	color24 *palette;
	cl_entity_t *viewent;
	dlight_t *elights;
	byte *texgammatable;
	uint16_t *lightgammatable;
	uint16_t *lineargammatable;
	uint16_t *screengammatable;

	uint max_entities;
} gl_globals_t;

typedef struct
{
	uint		c_world_polys;
	uint		c_studio_polys;
	uint		c_sprite_polys;
	uint		c_alias_polys;
	uint		c_world_leafs;

	uint		c_view_beams_count;
	uint		c_active_tents_count;
	uint		c_alias_models_drawn;
	uint		c_studio_models_drawn;
	uint		c_sprite_models_drawn;
	uint		c_particle_count;

	uint		c_client_ents;	// entities that moved to client
	double		t_world_node;
	double		t_world_draw;
} ref_speeds_t;

extern ref_speeds_t		r_stats;
extern ref_instance_t	RI;
extern gl_globals_t	tr;

extern float		gldepthmin, gldepthmax;
#define r_numEntities	(tr.draw_list->num_solid_entities + tr.draw_list->num_trans_entities)
#define r_numStatics	(r_stats.c_client_ents)
#define Mod_AllowMaterials() (host_allow_materials->value && !FBitSet( gp_host->features, ENGINE_DISABLE_HDTEXTURES ))

//
// gu_backend.c
//
void GL_BackendStartFrame( void );
void GL_BackendEndFrame( void );
void GL_CleanUpTextureUnits( int last );
void GL_Bind( int tmu, unsigned int texnum );
uint *GL_BindCommands( const gl_texture_t *texture, uint *p );
void GL_LoadTexMatrixExt( const float *glmatrix );
void GL_LoadMatrix( const matrix4x4 source );
void GL_CleanupAllTextureUnits( void );
void GL_LoadIdentityTexMatrix( void );
void GL_SetRenderMode( int mode );
void GL_Cull( GLenum cull );
void GL_PushPolygonOffset( float factor, float units );
void GL_PopPolygonOffset( void );
void SCR_TimeRefresh_f( void );
// RenderAPI stubs, GU has a single texture unit and no texgen
void GL_SelectTexture( int tmu );
void GL_TextureTarget( uint target );
void GL_TexGen( GLenum coord, GLenum mode );
void GL_SetTexCoordArrayMode( GLenum mode );

//
// gu_beams.c
//
void CL_DrawBeams( int fTrans, BEAM *active_beams );

//
// gu_cull.c
//
qboolean R_CullModel( const cl_entity_t *e, const vec3_t absmin, const vec3_t absmax );
qboolean R_CullBox( const vec3_t mins, const vec3_t maxs );
int R_CullSurface( const msurface_t *surf, const gl_frustum_t *frustum, uint clipflags );

//
// gu_decals.c
//
void DrawSurfaceDecals( msurface_t *fa, qboolean single, qboolean reverse );
float *R_DecalSetupVerts( decal_t *pDecal, msurface_t *surf, int texture, int *outCount );
void DrawSingleDecal( decal_t *pDecal, msurface_t *fa );
void R_EntityRemoveDecals( model_t *mod );
void DrawDecalsBatch( void );
void R_ClearDecals( void );

//
// gu_draw.c
//
void R_Set2DMode( qboolean enable );
void R_Set2DOffset( float x, float y );
void GL_UpdateTexture( int texnum, int cols, int rows, int width, int height, const byte *buffer, pixformat_t fmt );
void CL_FillRGBA( int rendermode, float x, float y, float w, float h, byte r, byte g, byte b, byte a );

//
// gu_image.c
//
gl_texture_t *R_GetTexture( unsigned int texnum );
#define GL_LoadTextureInternal( name, pic, flags ) GL_LoadTextureFromBuffer( name, pic, flags, false )
#define GL_UpdateTextureInternal( name, pic, flags ) GL_LoadTextureFromBuffer( name, pic, flags, true )
int GL_LoadTexture( const char *name, const byte *buf, size_t size, int flags );
int GL_LoadTextureArray( const char **names, int flags );
int GL_LoadTextureFromBuffer( const char *name, rgbdata_t *pic, texFlags_t flags, qboolean update );
qboolean GL_AliasTexture( int texnum, int atlas, int x, int y );
int GL_CreateAtlasPage( const char *name, int width, int height, int rows, const byte *pixels, const byte *palette );
#define PC_SWF( X ) ( ( X ) | ( 1 << 15 ) )
#define PC_HWF( X ) ( X )
#define ImageIND( type ) ( type == PF_INDEXED_24 || type == PF_INDEXED_32 )
void GL_PixelConverter( byte *dst, const byte *src, int size, int inFormat, int outFormat );
extern uint gl_texframe[MAX_TEXTURES];	// guRender.vert_frame it was last used in, about
#define GL_TouchTexture( texnum ) ( gl_texframe[( texnum )] = guRender.vert_frame )
void GL_PlaceTextures( void );
qboolean GU_UpdateTextureRect( int texnum, int xoff, int yoff, int width, int height, const void *buffer );
int GL_CreateTexture( const char *name, int width, int height, const void *buffer, texFlags_t flags );
int GL_CreateTextureArray( const char *name, int width, int height, int depth, const void *buffer, texFlags_t flags );
void GL_ProcessTexture( int texnum, float gamma, int topColor, int bottomColor );
void GL_UpdateTexSize( int texnum, int width, int height, int depth );
int GL_FindTexture( const char *name );
void GL_FreeTexture( unsigned int texnum );
void R_InitDlightTexture( void );
void R_TextureList_f( void );
void R_InitImages( void );
void R_ShutdownImages( void );
int GL_TexMemory( void );
qboolean R_SearchForTextureReplacement( char *out, size_t size, const char *modelname, const char *fmt, ... ) FORMAT_CHECK( 4 );
void R_TextureReplacementReport( const char *modelname, int gl_texturenum, const char *foundpath );
void R_ShowTextures( void );

//
// gu_rmain.c
//
void R_ClearScene( void );
void R_LoadIdentity( void );
void R_RenderScene( void );
void R_DrawCubemapView( const vec3_t origin, const vec3_t angles, int size );
void R_SetupRefParams( const struct ref_viewpass_s *rvp );
void R_TranslateForEntity( cl_entity_t *e );
void R_RotateForEntity( cl_entity_t *e );
void R_SetupGL( qboolean set_gl_state );
void R_AllowFog( qboolean allowed );
qboolean R_OpaqueEntity( cl_entity_t *ent );
void R_SetupFrustum( void );
void R_FindViewLeaf( void );
void R_PushScene( void );
void R_PopScene( void );
void R_DrawFog( void );
int CL_FxBlend( cl_entity_t *e );

//
// gu_rmath.c
//
void Matrix4x4_ToFMatrix4( const matrix4x4 in, ScePspFMatrix4 *out );
void Matrix4x4_FromFMatrix4( matrix4x4 out, const ScePspFMatrix4 *in );

//
// gu_rsurf.c
//
void R_MarkLeaves( void );
void R_DrawWorld( void );
void R_DrawWaterSurfaces( void );
void R_StaticWaterClear( void );
void R_DrawBrushModel( cl_entity_t *e );
qboolean R_StaticBrushModel( const cl_entity_t *e );
void R_DrawStaticBrushModels( cl_entity_t **ents, int count );
void GL_SubdivideSurface( model_t *mod, msurface_t *fa );
void GL_SetupFogColorForSurfaces( void );
void R_DrawAlphaTextureChains( void );
void GL_RebuildLightmaps( void );
void GL_BuildLightmaps( void );
void GL_ResetFogColor( void );
void R_LightmapCoord( const vec3_t v, const msurface_t *surf, const float sample_size, vec2_t coords );

//
// gu_rpart.c
//
void CL_DrawParticlesExternal( const ref_viewpass_t *rvp, qboolean trans_pass, float frametime );
void CL_DrawParticles( double frametime, particle_t *cl_active_particles, float partsize );
void CL_DrawTracers( double frametime, particle_t *cl_active_tracers );

//
// gu_sprite.c
//
void R_DrawSpriteModel( cl_entity_t *e );

//
// gu_studio.c
//
void R_StudioInit( void );
studiohdr_t *R_StudioGetHeader( void );
void R_StudioLerpMovement( cl_entity_t *e, double time, vec3_t origin, vec3_t angles );
struct mstudiotex_s *R_StudioGetTexture( cl_entity_t *e );
int R_GetEntityRenderMode( cl_entity_t *ent );
void R_DrawStudioModel( cl_entity_t *e );
player_info_t *pfnPlayerInfo( int index );
float R_StudioEstimateFrame( cl_entity_t *e, mstudioseqdesc_t *pseqdesc, double time );
void R_StudioResetPlayerModels( void );
qboolean R_StudioFillAPI( struct engine_studio_api_s *api, struct r_studio_interface_s *pDefaultDraw );
void R_StudioSetDrawInterface( struct r_studio_interface_s *pDraw );
void Mod_StudioLoadTextures( model_t *mod, void *data );
void Mod_StudioUnloadTextures( void *data );

//
// gu_alias.c
//
void Mod_LoadAliasModel( model_t *mod, const void *buffer, qboolean *loaded );
void R_DrawAliasModel( cl_entity_t *e );
void R_AliasInit( void );

//
// gu_warp.c
//
void R_AddSkyBoxSurface( msurface_t *fa );
void R_ClearSkyBox( void );
void R_DrawSkyBox( void );
void R_DrawClouds( void );
void R_UnloadSkybox( void );

//
// renderer exports
//
qboolean R_Init( void );
void R_Shutdown( void );
void GL_SetupAttributes( int safegl );
void GL_OnContextCreated( void );
void GL_InitExtensions( void );
void GL_ClearExtensions( void );
int GL_LoadTexture( const char *name, const byte *buf, size_t size, int flags );
qboolean VID_ScreenShot( const char *filename, int shot_type );
qboolean VID_CubemapShot( const char *base, uint size, const float *vieworg, qboolean skyshot );
void R_GammaChanged( qboolean do_reset_gamma );
void R_BeginFrame( qboolean clearScene );
void R_RenderFrame( const struct ref_viewpass_s *vp );
void R_EndFrame( void );
void R_WaitFrame( void );
void GU_OpenList( qboolean whole );
void R_GetTextureParms( int *w, int *h, int texnum );
void R_DrawStretchPic( float x, float y, float w, float h, float s1, float t1, float s2, float t2, int texnum );
qboolean R_SpeedsMessage( char *out, size_t size );
int R_WorldToScreen( const vec3_t point, vec3_t screen );
void R_ScreenToWorld( const vec3_t screen, vec3_t point );
qboolean R_AddEntity( struct cl_entity_s *pRefEntity, int entityType );
void Mod_UnloadAliasModel( struct model_s *mod );
void Mod_AliasUnloadTextures( void *data );
void R_RunViewmodelEvents( void );
void R_DrawViewModel( void );
void R_DecalShoot( int textureIndex, int entityIndex, int modelIndex, vec3_t pos, int flags, float scale );
void R_DecalRemoveAll( int texture );
int R_CreateDecalList( decallist_t *pList );
void R_ClearAllDecals( qboolean includePermanent );
byte *Mod_GetCurrentVis( void );
void Mod_SetOrthoBounds( const float *mins, const float *maxs );

//
// gu_triapi.c
//
void TriRenderMode( int mode );
void TriBegin( int mode );
void TriEnd( void );
void TriTexCoord2f( float u, float v );
void TriVertex3fv( const float *v );
void TriVertex3f( float x, float y, float z );
void _TriColor4f( float r, float g, float b, float a );
void _TriColor4ub( byte r, byte g, byte b, byte a );
void TriColor4f( float r, float g, float b, float a );
void TriColor4ub( byte r, byte g, byte b, byte a );
void TriBrightness( float brightness );
int TriWorldToScreen( const float *world, float *screen );
int TriSpriteTexture( model_t *pSpriteModel, int frame );
void TriFog( float flFogColor[3], float flStart, float flEnd, int bOn );
void TriGetMatrix( const int pname, float *matrix );
void TriFogParams( float flDensity, int iFogSkybox );
void TriCullFace( TRICULLSTYLE mode );
uint getTriBrightness( float brightness );

//
// gu_clipping.c
//
#define CLIPPING_DEBUGGING 0
void GU_ClipSetWorldFrustum( const matrix4x4 in );
void GU_ClipRestoreWorldFrustum( void );
void GU_ClipSetModelFrustum( const matrix4x4 in );
qboolean GU_ClipBoxInside( const vec3_t mins, const vec3_t maxs );
void GU_WorldSurfDecals( const msurface_t *surf );
void GU_WorldSurfDlit( const msurface_t *surf );
void GU_WorldSurfsNewFrame( void );
void GU_ClipGetWorldPlanes( float planes[4][4] );
void R_MeWorldRegister( void );
void R_MeWorldBegin( void );
qboolean R_MeWorldBusy( void );
#define R_GROUPED_ME	2	// in the flags of the draw lists: drawn by R_MeBrushesDraw
void R_MeBrushesBegin( byte *solid_grouped, byte *trans_grouped, const uint16_t *trans_order );
qboolean R_MeBrushesDraw( void );
qboolean R_StudioMeBonesPosted( const cl_entity_t *e );
// engine/platform/psp/me_psp.c: the renderer is linked into the engine
qboolean PSP_MeStart( void );
uint32_t PSP_MeRunKeep( void (*func)( void *arg ), void *arg );
qboolean PSP_PlayerModelFits( const char *name );
void PSP_MeWaitFor( uint32_t ticket );
qboolean PSP_MeDone( uint32_t ticket );
void PSP_MeWriteback( const void *start, size_t size );
void R_StudioPreCullEnd( void );
qboolean R_ViewModelSolid( void );
#define R_STUDIO_ME	2	// in the culled flags: drawn by R_StudioDrawMeModels
void R_StudioDrawMeModels( void );
void R_StudioFinishMeModels( void );
void R_StudioDrawMeModel( cl_entity_t *e );
extern void (*psp_entities_hook)( qboolean complete );

// a brush model for the Media Engine
typedef struct
{
	short		mins[3], maxs[3];	// rounded outwards
	uint16_t		firstsurf, numsurfs;
	byte		result, cullsurfs, watersides, pad;
	float		waterz;		// water planes up to here aren't drawn
} meworld_brush_t;

// what the Media Engine found out about an entity of the draw lists, before the view was known
enum
{
	MECULL_MAIN = 0,	// nothing
	MECULL_TEST,	// a studio model that may be culled
	MECULL_BONES,	// its bones may be the ME's too
	MECULL_DRAW,	// and its draws
	MECULL_SMALL,	// the same, 1 or 2 bones
	MECULL_PLAYER,
	MECULL_BRUSH,	// a static brush model
};

typedef struct
{
	union
	{
		struct
		{
			vec3_t		org;
			float		r;	// everything of it is this close to org
			float		rpx;	// r for its size on the screen
			const studiohdr_t	*hdr;
		};
		meworld_brush_t	brush;
	};

	uint16_t		body, skin;
	byte		type, small, mode, pad;
} mecull_t;

#define MECULL_MAXSOLID	384
#define MECULL_MAXTRANS	384
#define MECULL_MAX		( MECULL_MAXSOLID + MECULL_MAXTRANS )
#define MESORT_MAIN		255

typedef struct
{
	vec3_t		org;
	byte		rank;	// MESORT_MAIN: not known
	byte		far;	// drawn first
	byte		pad[2];
} mesort_t;

const mecull_t *R_StudioMeTable( int *numsolid, int *numtrans, const mesort_t **sort );
void R_StudioMePrepare( byte *solid_culled, byte *trans_culled, const uint16_t *trans_order );
void R_StudioMeYield( int number );
void R_StudioMeSceneCleared( void );
qboolean R_StudioMostlyAdditive( const studiohdr_t *hdr );
int R_RankForRenderMode( int rendermode );
qboolean R_MeBrushClassify( const cl_entity_t *e, meworld_brush_t *m );
void GU_ClipLoadFrustum( const mplane_t *plane ); // Experimental
int GU_ClipIsRequired( gu_vert_t *uv, int uvc );
void GU_Clip( gu_vert_t *uv, int uvc, gu_vert_t **cv, int *cvc );

/*
=======================================================================

 GU STATE MACHINE

=======================================================================
*/
typedef struct
{
	int		max_texture_size;
	int		max_2d_texture_size;
	qboolean	softwareGammaUpdate;
} glconfig_t;

typedef struct
{
	int		width, height;
	int		activeTMU;
	GLint		currentTexture;
	GLboolean	texIdentityMatrix;
	GLint		isFogEnabled;

	int		faceCull;

	qboolean	stencilEnabled;
	qboolean	in2DMode;
	vec2_t		offset2D;

	uint		fogColor;
	float		fogDensity;
	float		fogStart;
	float		fogEnd;
} glstate_t;

typedef struct
{
	qboolean	initialized;	// GU subsystem started
	qboolean	extended;	// extended context allows to GL_Debug
} glwstate_t;

#define PSP_GU_LIST_SIZE	0x100000 // 1Mb

typedef struct
{
	int		screen_width;
	int		screen_height;
	int		buffer_width;
	int		buffer_format;
	int		buffer_bpp;
	void		*draw_buffer;
	void		*disp_buffer;
	void		*depth_buffer;
	void		*lightmap_vram;	// kept free for GL_BuildLightmaps
	void		*context_list;	// the open one's
	size_t		context_list_size;
	int		prev_list;	// GE id of the list before it, -1 if the GE is done with that
	size_t		list_used;	// by the last one
	qboolean		list_whole;	// the open list has all of the memory
	void		*present[2];	// by vert_frame & 1: shown when the GE is done with that list
	byte		*vert_vram;	// per-frame vertex memory in VRAM (uncached alias)
	size_t		vert_vram_size;
	size_t		vert_vram_used;
	uint		vert_frame;	// bumped with every list: what the GE reads belongs to vert_frame & 1
	qboolean		frame_pending;	// the last list is finished and the next one isn't open yet
} gurender_t;

extern glconfig_t	glConfig;
extern glstate_t	glState;
extern gurender_t	guRender;

void *GU_GetVertexMemory( unsigned int size );
// move to engine
extern glwstate_t	glw_state;

//
// helper funcs
//
static inline cl_entity_t *CL_GetEntityByIndex( int index )
{
	if( unlikely( index < 0 || index >= tr.max_entities || !tr.entchunks || !tr.entchunks[index >> 4] ))
		return NULL;

	return &tr.entchunks[index >> 4][index & 15];
}

static inline model_t *CL_ModelHandle( int index )
{
	if( unlikely( index < 0 || index >= gp_cl->nummodels ))
		return NULL;

	return gp_cl->models[index];
}

static inline byte TextureToGamma( byte b )
{
	return !FBitSet( gp_host->features, ENGINE_LINEAR_GAMMA_SPACE ) ? tr.texgammatable[b] : b;
}

static inline uint LightToTexGamma( uint b )
{
	if( unlikely( b >= 1024 ))
		return 0;

	return !FBitSet( gp_host->features, ENGINE_LINEAR_GAMMA_SPACE ) ? tr.lightgammatable[b] : b;
}

static inline uint ScreenGammaTable( uint b )
{
	if( unlikely( b >= 1024 ))
		return 0;

	return !FBitSet( gp_host->features, ENGINE_LINEAR_GAMMA_SPACE ) ? tr.screengammatable[b] : b;
}

static inline uint LinearGammaTable( uint b )
{
	if( unlikely( b >= 1024 ))
		return 0;

	return !FBitSet( gp_host->features, ENGINE_LINEAR_GAMMA_SPACE ) ? tr.lineargammatable[b] : b;
}

#define WORLDMODEL (tr.worldmodel)

//
// renderer cvars
//
extern convar_t	gl_texture_lodfunc;
extern convar_t	gl_texture_lodbias;
extern convar_t	gl_texture_lodslope;
extern convar_t	gl_texture_nearest;
extern convar_t	gl_lightmap_nearest;
extern convar_t	gl_keeptjunctions;
extern convar_t	gl_round_down;
extern convar_t	gu_texture_maxsize;
extern convar_t	gu_texture_vram;
extern convar_t	r_studio_ge;
extern convar_t	gl_wireframe;
extern convar_t	gl_depthoffset;
extern convar_t	gl_polyoffset;
extern convar_t	gl_polyoffset_bmodels;
extern convar_t	gl_nosort;
extern convar_t	gl_test;		// cvar to testify new effects
extern convar_t	gl_subdivide_size;
extern convar_t	gl_fog;

extern convar_t	r_lighting_ambient;
extern convar_t	r_studio_lambert;
extern convar_t	r_detailtextures;
extern convar_t	r_novis;
#if XASH_PSP
extern convar_t	r_drawworld;
extern convar_t	r_ge_async;
#endif
extern convar_t	r_nocull;
extern convar_t	r_lockpvs;
extern convar_t	r_lockfrustum;
extern convar_t	r_traceglow;
extern convar_t	r_studio_sort_textures;
extern convar_t	r_studio_builtin_renderer;
extern convar_t	r_shadows;

//
// engine callbacks
//
#include "crtlib.h"

#endif // GU_LOCAL_H
