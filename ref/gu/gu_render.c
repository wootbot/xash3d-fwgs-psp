/*
gu_render.c - render initialization
Copyright (C) 2010 Uncle Mike
Copyright (C) 2022 Sergey Galushko

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
#include "gu_extension.h"

CVAR_DEFINE_AUTO( gl_texture_lodfunc, "2", FCVAR_GLCONFIG, "LOD func for mipmapped textures" );
CVAR_DEFINE_AUTO( gl_texture_lodbias, "-6.0", FCVAR_GLCONFIG, "LOD bias for mipmapped textures (perfomance|quality)" );
CVAR_DEFINE_AUTO( gl_texture_lodslope, "0.3", FCVAR_GLCONFIG, "LOD slope for mipmapped textures" );
CVAR_DEFINE_AUTO( gl_texture_nearest, "0", FCVAR_GLCONFIG, "disable texture filter" );
CVAR_DEFINE_AUTO( gl_lightmap_nearest, "0", FCVAR_GLCONFIG, "disable lightmap filter" );
CVAR_DEFINE_AUTO( gl_keeptjunctions, "1", FCVAR_GLCONFIG, "removing tjuncs causes blinking pixels" );
CVAR_DEFINE_AUTO( gl_round_down, "1", FCVAR_GLCONFIG|FCVAR_READ_ONLY, "round texture sizes to nearest POT value" );
CVAR_DEFINE_AUTO( gu_texture_maxsize, "256", FCVAR_ARCHIVE, "largest world, model and sprite texture (8 to 256), smaller ones fit in VRAM; applies to textures loaded afterwards" );
CVAR_DEFINE_AUTO( gu_texture_vram, "1", FCVAR_ARCHIVE, "0 keeps all textures in RAM (for comparisons); applies to textures loaded afterwards" );
CVAR_DEFINE_AUTO( gl_wireframe, "0", FCVAR_GLCONFIG|FCVAR_SPONLY, "show wireframe overlay" );
CVAR_DEFINE_AUTO( gl_depthoffset, "256.0", FCVAR_GLCONFIG, "depth offset for decals" );
CVAR_DEFINE_AUTO( gl_polyoffset, "2", FCVAR_GLCONFIG, "polygon offset for decals" );
CVAR_DEFINE_AUTO( gl_polyoffset_bmodels, "2", FCVAR_GLCONFIG, "polygon offset for brush models" );
CVAR_DEFINE_AUTO( gl_nosort, "0", FCVAR_GLCONFIG, "disable sorting of translucent surfaces" );
CVAR_DEFINE_AUTO( gl_test, "0", 0, "engine developer cvar for quick testing new features" );
CVAR_DEFINE_AUTO( gl_subdivide_size, "256.0", FCVAR_GLCONFIG, "the division value for the sky brushes" );
CVAR_DEFINE_AUTO( gl_fog, "1", FCVAR_GLCONFIG, "allow for rendering fog using built-in GU fog implementation" );

CVAR_DEFINE_AUTO( r_lighting_ambient, "0.3", FCVAR_GLCONFIG, "map ambient lighting scale" );
CVAR_DEFINE_AUTO( r_detailtextures, "0", FCVAR_GLCONFIG, "enable detail textures support" );
CVAR_DEFINE_AUTO( r_novis, "0", 0, "ignore vis information (perfomance test)" );
#if XASH_PSP
CVAR_DEFINE_AUTO( r_drawworld, "1", FCVAR_CHEAT, "draw the world and water surfaces (perfomance test)" );
CVAR_DEFINE_AUTO( r_ge_async, "1", FCVAR_ARCHIVE, "build a frame while the GE is still drawing the last one" );
#endif
CVAR_DEFINE_AUTO( r_nocull, "0", 0, "ignore frustrum culling (perfomance test)" );
CVAR_DEFINE_AUTO( r_lockpvs, "0", FCVAR_CHEAT, "lockpvs area at current point (pvs test)" );
CVAR_DEFINE_AUTO( r_lockfrustum, "0", FCVAR_CHEAT, "lock frustrum area at current point (cull test)" );
CVAR_DEFINE_AUTO( r_traceglow, "0", FCVAR_GLCONFIG, "cull flares behind models" );

extern convar_t r_fast_particles;

gl_globals_t	tr;
glconfig_t	glConfig;
glstate_t	glState;
glwstate_t	glw_state;
gurender_t	guRender;

// Set frame buffer
#define PSP_FB_WIDTH		480
#define PSP_FB_HEIGHT		272
#define PSP_FB_BWIDTH		512
#define PSP_FB_FORMAT		GU_PSM_5650 //4444,5551,5650,8888

#if   PSP_FB_FORMAT == GU_PSM_4444
#define PSP_FB_BPP			2
#elif PSP_FB_FORMAT == GU_PSM_5551
#define PSP_FB_BPP			2
#elif PSP_FB_FORMAT == GU_PSM_5650
#define PSP_FB_BPP			2
#elif PSP_FB_FORMAT == GU_PSM_8888
#define PSP_FB_BPP			4
#endif


static byte context_list[PSP_GU_LIST_SIZE] __attribute__( ( aligned( 64 ) ) );

// on the GE's finish interrupt: the list's frame onto the screen
static void GU_Present( int id )
{
	if( id < 2 && guRender.present[id] )
		sceDisplaySetFrameBuf( guRender.present[id], guRender.buffer_width, guRender.buffer_format, PSP_DISPLAY_SETBUF_IMMEDIATE );
}

/*
===============
GU_OpenList

the next display list: half of the memory, the GE may still be reading the other one
(guRender.vert_frame & 1 tells which), or all of it
===============
*/
void GU_OpenList( qboolean whole )
{
	const uint half = guRender.vert_frame & 1;

	guRender.list_whole = whole;
	guRender.context_list = context_list + ( whole ? 0 : half * ( sizeof( context_list ) / 2 ));
	guRender.context_list_size = whole ? sizeof( context_list ) : sizeof( context_list ) / 2;
	guRender.vert_vram_used = half * ( guRender.vert_vram_size / 2 );

	extGuStart( GU_DIRECT, guRender.context_list, guRender.context_list_size );

	// sceGuSwapBuffers isn't used: the GE may not be done with the last frame
	sceGuDrawBufferList( guRender.buffer_format, vrelptr( guRender.draw_buffer ), guRender.buffer_width );

	// the GE list ids are reused
	if( guRender.prev_list == extGuListId( ))
		guRender.prev_list = -1;
}

/*
===============
GU_Init
===============
*/
static qboolean GU_Init( void )
{
	size_t fb_size;

	memset( &guRender, 0, sizeof( guRender ));

	guRender.screen_width = PSP_FB_WIDTH;
	guRender.screen_height = PSP_FB_HEIGHT;
	guRender.buffer_width = PSP_FB_BWIDTH;
	guRender.buffer_format = PSP_FB_FORMAT;
	guRender.buffer_bpp = PSP_FB_BPP;

	fb_size = guRender.buffer_width * guRender.screen_height * guRender.buffer_bpp;
	guRender.draw_buffer = valloc( fb_size );
	guRender.disp_buffer = valloc( fb_size );
	guRender.depth_buffer = valloc( guRender.buffer_width * guRender.screen_height * 2 ); // always 16-bit

	if( !guRender.draw_buffer || !guRender.disp_buffer || !guRender.depth_buffer )
	{
		gEngfuncs.Con_Printf( S_ERROR "%s: can't allocate framebuffers in VRAM\n", __func__ );
		return false;
	}

	guRender.context_list = context_list;
	guRender.context_list_size = sizeof( context_list );
	guRender.prev_list = -1;

	// per-frame vertices and lighting ramps: CPU writes to VRAM are cheaper than to
	// RAM while the GE reads its display list from RAM
	guRender.vert_vram_size = 64 * 1024;
	guRender.vert_vram = valloc( guRender.vert_vram_size );
	if( guRender.vert_vram )
		guRender.vert_vram = (byte *)((uint)guRender.vert_vram | 0x40000000 );
	else guRender.vert_vram_size = 0;

	// the first map's lightmaps, before its textures and models fill the VRAM: the
	// lightmap pass covers the screen. Later maps reuse the old ones' memory
	guRender.lightmap_vram = valloc( 320 * 1024 );

	// Initialise the GU.
	sceGuInit();
	sceGuSetCallback( GU_CALLBACK_FINISH, GU_Present );

	// Set up the GU.
	sceGuStart( GU_DIRECT, guRender.context_list );

	sceGuDrawBuffer( guRender.buffer_format, vrelptr( guRender.draw_buffer ), guRender.buffer_width );
	sceGuDispBuffer( guRender.screen_width, guRender.screen_height, vrelptr( guRender.disp_buffer ), guRender.buffer_width );
	sceGuDepthBuffer( vrelptr( guRender.depth_buffer ), guRender.buffer_width );

	// Set the rendering offset and viewport.
	sceGuOffset( 2048 - ( guRender.screen_width / 2 ), 2048 - ( guRender.screen_height / 2 ));
	sceGuViewport( 2048, 2048, guRender.screen_width, guRender.screen_height );

	// Set up scissoring.
	sceGuEnable( GU_SCISSOR_TEST );
	sceGuScissor( 0, 0, guRender.screen_width, guRender.screen_height );

	// Xash default
	sceGuClearColor( GU_COLOR( 0.5f, 0.5f, 0.5f, 1.0f ));

	sceGuEnable( GU_DEPTH_TEST );
	sceGuDisable( GU_CULL_FACE );
	sceGuEnable( GU_CLIP_PLANES );
	sceGuDepthFunc( GU_LEQUAL );
	sceGuColor( 0xffffffff );

	// Set up stencil
	if( glState.stencilEnabled )
	{
		sceGuDisable( GU_STENCIL_TEST );
		sceGuStencilFunc( GU_EQUAL, 0, ~0 );
		sceGuStencilOp( GU_KEEP, GU_INCR, GU_INCR );
	}

	sceGuDepthRange( 0, 65535 );
	sceGuDepthOffset( 0 );

	sceGuDisable( GU_BLEND );
	sceGuDisable( GU_ALPHA_TEST );

	sceGuBlendFunc( GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0 );
	sceGuAlphaFunc( GU_GREATER, DEFAULT_ALPHATEST, 0xff );
	sceGuEnable( GU_TEXTURE_2D );
	sceGuShadeModel( GU_SMOOTH );
	sceGuFrontFace( GU_CCW );

	// Set the default matrices.
	sceGumMatrixMode( GU_PROJECTION );
	sceGumLoadIdentity();
	sceGumMatrixMode( GU_VIEW );
	sceGumLoadIdentity();
	sceGumMatrixMode( GU_MODEL );
	sceGumLoadIdentity();
	sceGumMatrixMode( GU_TEXTURE );
	sceGumLoadIdentity();

	sceGumUpdateMatrix();

	sceGuFinish();
	sceGuSync( GU_SYNC_FINISH, GU_SYNC_WAIT );

	// Turn on the display.
	sceDisplayWaitVblankStart();
	sceGuDisplay( GU_TRUE );

	// Start a new render.
	GU_OpenList( true );

	return true;
}

/*
===============
GU_Shutdown
===============
*/
static void GU_Shutdown( void )
{
	R_WaitFrame();
	// Finish rendering.
	sceGuFinish();
	sceGuSync( GU_SYNC_FINISH, GU_SYNC_WAIT );

	// Shut down the display.
	sceGuTerm();

	// Free the buffers.
	if( guRender.draw_buffer )
		vfree( guRender.draw_buffer );
	if( guRender.disp_buffer )
		vfree( guRender.disp_buffer );
	if( guRender.depth_buffer )
		vfree( guRender.depth_buffer );

	guRender.draw_buffer = NULL;
	guRender.disp_buffer = NULL;
	guRender.depth_buffer = NULL;
}

/*
===============
GL_SetDefaultState
===============
*/
static void GL_SetDefaultState( void )
{
	memset( &glState, 0, sizeof( glState ));

	// init draw stack
	tr.draw_list = &tr.draw_stack[0];
	tr.draw_stack_pos = 0;

	// init glState struct
	glState.currentTexture = -1;
	glState.texIdentityMatrix = true;
	glState.fogColor = 0;
	glState.fogDensity = 0;
	glState.fogStart = 100.0f;
	glState.fogEnd = 1000.0f;
}

/*
=================
R_RenderInfo_f
=================
*/
static void R_RenderInfo_f( void )
{
	gEngfuncs.Con_Printf( "\n" );
	gEngfuncs.Con_Printf( "HARDWARE RENDER: PSP GU\n" );
	gEngfuncs.Con_Printf( "MAX_TEXTURE_SIZE: %i\n", glConfig.max_texture_size );
	gEngfuncs.Con_Printf( "MODE: %ix%i\n", gpGlobals->width, gpGlobals->height );
	gEngfuncs.Con_Printf( "VERTICAL SYNC: %s\n", gl_vsync->value ? "enabled" : "disabled" );
	gEngfuncs.Con_Printf( "VRAM: %u KB, %u bytes free (largest block %u)\n", sceGeEdramGetSize() >> 10, (uint)vmemavail(), (uint)vlargestblock( ));
}

void GL_InitExtensions( void )
{
	char value[MAX_VA_STRING];

	glState.stencilEnabled = true;
	glConfig.max_texture_size = 256;
	glConfig.max_2d_texture_size = 512; // GE hardware limit

	gEngfuncs.Con_Reportf( "^3Video^7: PSP GU\n" );

	Q_snprintf( value, sizeof( value ), "%i", glConfig.max_texture_size );
	gEngfuncs.Cvar_Get( "gl_max_size", value, 0, "opengl texture max dims (compatibility cvar, does nothing)" );

	R_RenderInfo_f();

	tr.framecount = tr.visframecount = 1;
	glw_state.initialized = true;
}

void GL_ClearExtensions( void )
{
	// now all extensions are disabled
	glw_state.initialized = false;
}

//=======================================================================

/*
=================
GL_InitCommands
=================
*/
/*
===============
GU_GetVertexMemory

per-frame memory for the GE: VRAM while there is room left, else the display
list. No stall update: callers batch draws and call extGuUpdateStall
===============
*/
void *GU_GetVertexMemory( unsigned int size )
{
	size = ( size + 15 ) & ~15u;
	if( guRender.vert_vram_used + size <= guRender.vert_vram_size / 2 * ( 1 + ( guRender.vert_frame & 1 )))
	{
		void *p = guRender.vert_vram + guRender.vert_vram_used;
		guRender.vert_vram_used += size;
		return p;
	}
	return extGuGetMemoryNoStall( size );
}

static void GL_InitCommands( void )
{
	gEngfuncs.Cvar_RegisterVariable( &r_lighting_ambient );
	gEngfuncs.Cvar_RegisterVariable( &r_novis );
#if XASH_PSP
	gEngfuncs.Cvar_RegisterVariable( &r_drawworld );
	gEngfuncs.Cvar_RegisterVariable( &r_ge_async );
	R_MeWorldRegister();
#endif
	gEngfuncs.Cvar_RegisterVariable( &r_nocull );
	gEngfuncs.Cvar_RegisterVariable( &r_detailtextures );
	gEngfuncs.Cvar_RegisterVariable( &r_lockpvs );
	gEngfuncs.Cvar_RegisterVariable( &r_lockfrustum );
	gEngfuncs.Cvar_RegisterVariable( &r_traceglow );
	gEngfuncs.Cvar_RegisterVariable( &r_studio_sort_textures );
	gEngfuncs.Cvar_RegisterVariable( &r_studio_builtin_renderer );
	gEngfuncs.Cvar_RegisterVariable( &r_studio_ge );
	gEngfuncs.Cvar_RegisterVariable( &r_shadows );
	gEngfuncs.Cvar_RegisterVariable( &r_fast_particles );

	gEngfuncs.Cvar_RegisterVariable( &gl_texture_lodfunc );
	gEngfuncs.Cvar_RegisterVariable( &gl_texture_lodbias );
	gEngfuncs.Cvar_RegisterVariable( &gl_texture_lodslope );
	gEngfuncs.Cvar_RegisterVariable( &gl_texture_nearest );
	gEngfuncs.Cvar_RegisterVariable( &gl_lightmap_nearest );
	gEngfuncs.Cvar_RegisterVariable( &gl_keeptjunctions );
	gEngfuncs.Cvar_RegisterVariable( &gl_round_down );
	gEngfuncs.Cvar_RegisterVariable( &gu_texture_maxsize );
	gEngfuncs.Cvar_RegisterVariable( &gu_texture_vram );
	gEngfuncs.Cvar_RegisterVariable( &gl_wireframe );
	gEngfuncs.Cvar_RegisterVariable( &gl_depthoffset );
	gEngfuncs.Cvar_RegisterVariable( &gl_polyoffset );
	gEngfuncs.Cvar_RegisterVariable( &gl_polyoffset_bmodels );
	gEngfuncs.Cvar_RegisterVariable( &gl_nosort );
	gEngfuncs.Cvar_RegisterVariable( &gl_test );
	gEngfuncs.Cvar_RegisterVariable( &gl_subdivide_size );
	gEngfuncs.Cvar_RegisterVariable( &gl_fog );

	// make sure gl_vsync is checked after vid_restart
	SetBits( gl_vsync->flags, FCVAR_CHANGED );

	gEngfuncs.Cmd_AddCommand( "r_info", R_RenderInfo_f, "display renderer info" );
	gEngfuncs.Cmd_AddCommand( "timerefresh", SCR_TimeRefresh_f, "turn quickly and print rendering statistcs" );
}

/*
=================
GL_RemoveCommands
=================
*/
static void GL_RemoveCommands( void )
{
	gEngfuncs.Cmd_RemoveCommand( "r_info" );
	gEngfuncs.Cmd_RemoveCommand( "timerefresh" );
}

/*
===============
R_Init
===============
*/
qboolean R_Init( void )
{
	if( glw_state.initialized )
		return true;

	if( vinit() < 0 )
	{
		gEngfuncs.Con_Printf( S_ERROR "Can't initialize video subsystem: VRAM unavailable\n" );
		return false;
	}

	GL_InitCommands();
	GL_InitRandomTable();

	GL_SetDefaultState();

	r_temppool = Mem_AllocPool( "Render Zone" );

	// set up the screen
	if( !gEngfuncs.R_Init_Video( REF_GL ))
	{
		GL_RemoveCommands();
		gEngfuncs.R_Free_Video();
		Mem_FreePool( &r_temppool );
		return false;
	}

	if( !GU_Init( ))
	{
		GU_Shutdown();
		GL_RemoveCommands();
		gEngfuncs.R_Free_Video();
		Mem_FreePool( &r_temppool );
		return false;
	}

	// see R_ProcessEntData for tr.entities initialization
	tr.world = (struct world_static_s *)ENGINE_GET_PARM( PARM_GET_WORLD_PTR );
	tr.palette = (color24 *)ENGINE_GET_PARM( PARM_GET_PALETTE_PTR );
	tr.viewent = (cl_entity_t *)ENGINE_GET_PARM( PARM_GET_VIEWENT_PTR );
	tr.texgammatable = (byte *)ENGINE_GET_PARM( PARM_GET_TEXGAMMATABLE_PTR );
	tr.lightgammatable = (uint16_t *)ENGINE_GET_PARM( PARM_GET_LIGHTGAMMATABLE_PTR );
	tr.screengammatable = (uint16_t *)ENGINE_GET_PARM( PARM_GET_SCREENGAMMATABLE_PTR );
	tr.lineargammatable = (uint16_t *)ENGINE_GET_PARM( PARM_GET_LINEARGAMMATABLE_PTR );
	tr.elights = (dlight_t *)ENGINE_GET_PARM( PARM_GET_ELIGHTS_PTR );

	R_InitImages();
	R_StudioInit();
	R_AliasInit();
	R_ClearDecals();
	R_ClearScene();

	return true;
}

/*
===============
R_Shutdown
===============
*/
void R_Shutdown( void )
{
	if( !glw_state.initialized )
		return;

	GL_RemoveCommands();
	R_ShutdownImages();
	GU_Shutdown();

	Mem_FreePool( &r_temppool );

	// shut down OS specific stuff
	gEngfuncs.R_Free_Video();
}

void GL_SetupAttributes( int safegl )
{
}
