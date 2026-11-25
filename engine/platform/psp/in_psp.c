/*
in_psp.c - PSP input backend
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
The buttons are gamepad keys, the analog stick turns the view. With HOME held,
buttons send K_AUX16 + index instead, for extra binds.
The key names match the old PSP port, so its configs keep working.
*/

#include "platform/platform.h"
#if XASH_INPUT == INPUT_PSP
#include <pspctrl.h>
#include <psppower.h>
#include <pspthreadman.h>
#include "input.h"
#include "keydefs.h"
#include "client.h"

#define PSP_EXT_KEY PSP_CTRL_HOME

static struct
{
	uint src;
	int dst;
	qboolean std_pressed;
	qboolean ext_pressed;
} psp_keymap[] =
{
{ PSP_CTRL_SELECT,   K_MODE_BUTTON },
{ PSP_CTRL_START,    K_START_BUTTON },
{ PSP_CTRL_UP,       K_UPARROW },
{ PSP_CTRL_RIGHT,    K_RIGHTARROW },
{ PSP_CTRL_DOWN,     K_DOWNARROW },
{ PSP_CTRL_LEFT,     K_LEFTARROW },
{ PSP_CTRL_LTRIGGER, K_L1_BUTTON },
{ PSP_CTRL_RTRIGGER, K_R1_BUTTON },
{ PSP_CTRL_TRIANGLE, K_Y_BUTTON },
{ PSP_CTRL_CIRCLE,   K_B_BUTTON },
{ PSP_CTRL_CROSS,    K_A_BUTTON },
{ PSP_CTRL_SQUARE,   K_X_BUTTON },
};

static CVAR_DEFINE_AUTO( psp_joy_dz_min, "15", FCVAR_ARCHIVE, "analog stick inner deadzone (0 - 127)" );
static CVAR_DEFINE_AUTO( psp_joy_dz_max, "0", FCVAR_ARCHIVE, "analog stick outer deadzone (0 - 127)" );
static CVAR_DEFINE_AUTO( psp_joy_cv_power, "2", FCVAR_ARCHIVE, "analog stick curve power (0 - 10)" );
static CVAR_DEFINE_AUTO( psp_joy_cv_expo, "0.5", FCVAR_ARCHIVE, "analog stick curve expo (0 - 1.0)" );

static short psp_joymap[256]; // raw stick value to axis value
static qboolean psp_joy_init;
static uint psp_last_buttons;
static short psp_last_x, psp_last_y;

static float PSP_JoyAxisCompute( float axis, float deadzone_min, float deadzone_max, float power, float expo )
{
	qboolean flip = false;
	float abs_axis, r_deadzone_max, scale;

	expo = bound( 0.0f, expo, 1.0f );
	power = bound( 0.0f, power, 10.0f );
	deadzone_min = bound( 0.0f, deadzone_min, 127.0f );
	deadzone_max = bound( 0.0f, deadzone_max, 127.0f );

	// (-127) - (0) - (+127)
	abs_axis = axis - 128.0f;
	if( abs_axis < 0.0f )
	{
		abs_axis = -abs_axis - 1.0f;
		flip = true;
	}

	if( abs_axis <= deadzone_min )
		return 0.0f;

	r_deadzone_max = 127.0f - deadzone_max;
	if( abs_axis >= r_deadzone_max )
		return flip ? -127.0f : 127.0f;

	scale = 127.0f / ( r_deadzone_max - deadzone_min );
	abs_axis = ( abs_axis - deadzone_min ) * scale;

	if( expo )
	{
		// x * ( x^power * expo + x * ( 1.0 - expo ))
		float x = abs_axis / 127.0f;
		float curve = powf( x, power ) * expo + x * ( 1.0f - expo );
		abs_axis = x * curve * 127.0f;
	}

	return flip ? -abs_axis : abs_axis;
}

static void PSP_BuildJoyMap( void )
{
	for( int i = 0; i < 256; i++ )
	{
		psp_joymap[i] = (short)( PSP_JoyAxisCompute( i, psp_joy_dz_min.value, psp_joy_dz_max.value,
			psp_joy_cv_power.value, psp_joy_cv_expo.value ) * 256.0f );
	}
}

void Platform_RunEvents( void )
{
	static uint32_t last_tick;
	SceCtrlData buf;
	short x, y;

	// SDL sets this on the first focus event: the PSP's window always has the focus
	if( host.status == HOST_INIT )
		host.status = HOST_FRAME;

	// HOME, then Yes
	if( PSP_ExitRequested( ))
		Sys_Quit( "HOME" );

	PSP_NetFrame();
	PSP_DbgConFrame();

	// the XMB's display off / auto sleep timers count from the last button
	// press: keep the screen on and the PSP awake while the game runs (an
	// online game or a demo can go minutes without input)
	if( sceKernelGetSystemTimeLow() - last_tick > 1000000 )
	{
		scePowerTick( PSP_POWER_TICK_ALL );
		last_tick = sceKernelGetSystemTimeLow();
	}

	if( !psp_joy_init )
		return;

	sceCtrlPeekBufferPositive( &buf, 1 );

	for( int i = 0; i < ARRAYSIZE( psp_keymap ); i++ )
	{
		qboolean changed = ( psp_last_buttons ^ buf.Buttons ) & psp_keymap[i].src;

		if( buf.Buttons & PSP_EXT_KEY )
		{
			if( !changed )
				continue;

			if( psp_keymap[i].std_pressed )
			{
				// released while HOME is held
				psp_keymap[i].std_pressed = false;
				Key_Event( psp_keymap[i].dst, false );
			}
			else
			{
				psp_keymap[i].ext_pressed = ( buf.Buttons & psp_keymap[i].src ) != 0;
				Key_Event( K_AUX16 + i, psp_keymap[i].ext_pressed );
			}
		}
		else
		{
			if( psp_keymap[i].ext_pressed )
			{
				psp_keymap[i].ext_pressed = false;
				Key_Event( K_AUX16 + i, false );
			}

			if( changed )
			{
				psp_keymap[i].std_pressed = ( buf.Buttons & psp_keymap[i].src ) != 0;
				Key_Event( psp_keymap[i].dst, psp_keymap[i].std_pressed );
			}
		}
	}

	psp_last_buttons = buf.Buttons;

	if( FBitSet( psp_joy_dz_min.flags | psp_joy_dz_max.flags | psp_joy_cv_power.flags | psp_joy_cv_expo.flags, FCVAR_CHANGED ))
	{
		PSP_BuildJoyMap();
		ClearBits( psp_joy_dz_min.flags, FCVAR_CHANGED );
		ClearBits( psp_joy_dz_max.flags, FCVAR_CHANGED );
		ClearBits( psp_joy_cv_power.flags, FCVAR_CHANGED );
		ClearBits( psp_joy_cv_expo.flags, FCVAR_CHANGED );
	}

	x = psp_joymap[buf.Lx];
	y = psp_joymap[buf.Ly];

	// the only stick turns the view, right and down are positive
	if( psp_last_x != x )
		Joy_AxisMotionEvent( JOY_AXIS_YAW, x );

	if( psp_last_y != y )
		Joy_AxisMotionEvent( JOY_AXIS_PITCH, y );

	psp_last_x = x;
	psp_last_y = y;
}

int Platform_JoyInit( void )
{
	if( psp_joy_init )
		return 1;

	Cvar_RegisterVariable( &psp_joy_dz_min );
	Cvar_RegisterVariable( &psp_joy_dz_max );
	Cvar_RegisterVariable( &psp_joy_cv_power );
	Cvar_RegisterVariable( &psp_joy_cv_expo );

	sceCtrlSetSamplingCycle( 0 );
	sceCtrlSetSamplingMode( PSP_CTRL_MODE_ANALOG );

	PSP_BuildJoyMap();

	// no preset: those are for pads with more sticks and buttons
	Joy_SetCapabilities( false, 0, "" );

	psp_joy_init = true;

	return 1;
}

void Platform_JoyShutdown( void )
{
	psp_joy_init = false;
}

void Platform_CalibrateGamepadGyro( void )
{
}

key_modifier_t Platform_GetKeyModifiers( void )
{
	return KeyModifier_None;
}

void Platform_EnableTextInput( qboolean enable, int x, int y, int w, int h )
{
	// TODO: sceUtilityOsk
}

#endif // XASH_INPUT == INPUT_PSP
