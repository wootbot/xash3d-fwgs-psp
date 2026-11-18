/*
vid_psp.c - PSP video backend
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
The screen has a fixed size and there are no windows. The hardware renderer (ref_gu)
owns the GE and draws by itself, the software renderer gets a 16-bit framebuffer here.
*/

#include "platform/platform.h"
#if XASH_VIDEO == VIDEO_PSP
#include <malloc.h>
#include <pspkernel.h>
#include <pspdisplay.h>
#include "client.h"
#include "vid_common.h"

#define PSP_SCREEN_WIDTH  480
#define PSP_SCREEN_HEIGHT 272
#define PSP_LINE_SIZE     512
#define PSP_FB_FORMAT     PSP_DISPLAY_PIXEL_FORMAT_565
#define PSP_FB_BPP        2

static int psp_dummy_window; // host.hWnd must be non-NULL while the screen is open

// debugging aid: there is no keyboard under PSPLink, so screenshots can be taken periodically
// (e.g. "+vid_autoshot 5" in the arguments), they are written through host0: when run from the PC
static CVAR_DEFINE_AUTO( vid_autoshot, "0", 0, "take a screenshot every N seconds (0 - disabled)" );
static double vid_autoshot_next;

// debugging aid: commands written into this file (relative to the root directory, so on the PC
// when running from host0:) are executed and the file is deleted, e.g. "+sys_cmdfile cmd.txt"
static CVAR_DEFINE_AUTO( sys_cmdfile, "", 0, "execute and delete this file once a second (debugging)" );
static double sys_cmdfile_next;

static struct
{
	qboolean opened;
	qboolean vsync;
	void *draw_buffer; // software renderer only
	void *disp_buffer;
} vid_psp;

void Platform_Minimize_f( void )
{
}

platform_orientation_t Platform_GetDisplayOrientation( void )
{
	return ORIENTATION_LANDSCAPE;
}

qboolean SW_CreateBuffer( int width, int height, uint *stride, uint *bpp, uint *r, uint *g, uint *b )
{
	size_t size = PSP_LINE_SIZE * PSP_SCREEN_HEIGHT * PSP_FB_BPP;

	if( !vid_psp.draw_buffer )
		vid_psp.draw_buffer = memalign( 64, size );
	if( !vid_psp.disp_buffer )
		vid_psp.disp_buffer = memalign( 64, size );

	if( !vid_psp.draw_buffer || !vid_psp.disp_buffer )
		return false;

	memset( vid_psp.draw_buffer, 0, size );
	memset( vid_psp.disp_buffer, 0, size );

	*stride = PSP_LINE_SIZE;
	*bpp = PSP_FB_BPP;
	*r = 31;
	*g = 63 << 5;
	*b = 31 << 11;

	sceDisplaySetMode( 0, PSP_SCREEN_WIDTH, PSP_SCREEN_HEIGHT );
	sceDisplaySetFrameBuf( vid_psp.disp_buffer, PSP_LINE_SIZE, PSP_FB_FORMAT, PSP_DISPLAY_SETBUF_NEXTFRAME );

	return true;
}

void *SW_LockBuffer( void )
{
	return vid_psp.draw_buffer;
}

void SW_UnlockBuffer( void )
{
	void *swap = vid_psp.disp_buffer;

	vid_psp.disp_buffer = vid_psp.draw_buffer;
	vid_psp.draw_buffer = swap;

	sceKernelDcacheWritebackInvalidateAll();
	sceDisplaySetFrameBuf( vid_psp.disp_buffer, PSP_LINE_SIZE, PSP_FB_FORMAT, PSP_DISPLAY_SETBUF_NEXTFRAME );

	if( vid_psp.vsync )
		sceDisplayWaitVblankStart();
}

int R_MaxVideoModes( void )
{
	return 0;
}

vidmode_t *R_GetVideoMode( int num )
{
	return NULL;
}

void *GL_GetProcAddress( const char *name )
{
	return NULL;
}

void GL_UpdateSwapInterval( void )
{
	if( FBitSet( gl_vsync.flags, FCVAR_CHANGED ))
	{
		ClearBits( gl_vsync.flags, FCVAR_CHANGED );
		vid_psp.vsync = gl_vsync.value > 0;
	}
}

static void VID_CheckAutoShot( void )
{
	if( vid_autoshot.value <= 0.0f )
		return;

	if( host.realtime < vid_autoshot_next )
		return;

	vid_autoshot_next = host.realtime + vid_autoshot.value;
	Cbuf_AddText( "screenshot\n" );
}


static void VID_CheckCmdFile( void )
{
	char buf[1024];

	if( COM_StringEmpty( sys_cmdfile.string ) || host.realtime < sys_cmdfile_next )
		return;

	sys_cmdfile_next = host.realtime + 1.0;

	FILE *f = fopen( sys_cmdfile.string, "rb" );
	if( !f )
		return;

	size_t len = fread( buf, 1, sizeof( buf ) - 2, f );
	fclose( f );
	remove( sys_cmdfile.string );

	buf[len] = '\n';
	buf[len + 1] = 0;
	Cbuf_AddText( buf );
}

void GL_SwapBuffers( void )
{
	VID_CheckAutoShot();
	VID_CheckCmdFile();

	// ref_gu swaps the buffers itself
	if( vid_psp.vsync )
		sceDisplayWaitVblankStart();
}

int GL_SetAttribute( int attr, int val )
{
	return 0;
}

int GL_GetAttribute( int attr, int *val )
{
	*val = 0;
	return 0;
}

ref_window_type_t R_GetWindowHandle( void **handle, ref_window_type_t type )
{
	return REF_WINDOW_TYPE_NULL;
}

qboolean VID_SetMode( void )
{
	int w = PSP_SCREEN_WIDTH, h = PSP_SCREEN_HEIGHT;

	if( !vid_psp.opened )
	{
		vid_psp.opened = true;
		host.hWnd = (void *)&psp_dummy_window;
		refState.window_mode = WINDOW_MODE_FULLSCREEN;
		SetBits( gl_vsync.flags, FCVAR_CHANGED );
		Cvar_RegisterVariable( &vid_autoshot );
		Cvar_RegisterVariable( &sys_cmdfile );
		PSP_RegisterCommands();
	}

	VID_SetDisplayTransform( &w, &h );
	R_SaveVideoMode( PSP_SCREEN_WIDTH, PSP_SCREEN_HEIGHT, w, h, false );

	return true;
}

rserr_t R_ChangeDisplaySettings( int width, int height, window_mode_t window_mode )
{
	// screen size is fixed
	return VID_SetMode() ? rserr_ok : rserr_invalid_mode;
}

qboolean R_Init_Video( ref_graphic_apis_t type )
{
	refState.desktopBitsPixel = PSP_FB_BPP * 8;

	switch( type )
	{
	case REF_SOFTWARE:
		glw_state.software = true;
		break;
	case REF_GL: // ref_gu
		glw_state.software = false;
		break;
	default:
		Con_Printf( S_ERROR "%s: unsupported context type %d\n", __func__, type );
		return false;
	}

	if( !VID_SetMode( ))
		return false;

	if( type == REF_GL )
		ref.dllFuncs.GL_InitExtensions();

	host.renderinfo_changed = false;

	return true;
}

void R_Free_Video( void )
{
	ref.dllFuncs.GL_ClearExtensions();

	free( vid_psp.draw_buffer );
	free( vid_psp.disp_buffer );
	vid_psp.draw_buffer = vid_psp.disp_buffer = NULL;

	vid_psp.opened = false;
	host.hWnd = NULL;
}

void VID_Info_f( void )
{
	Con_Printf( "Video: " S_GREEN "PSP" S_DEFAULT ", %dx%d, %s\n", refState.width, refState.height,
		glw_state.software ? "software" : "GU" );
}

#endif // XASH_VIDEO == VIDEO_PSP
