/*
me_psp.c - Media Engine (second CPU) jobs
Copyright (C) 2026 Xash3D FWGS contributors

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
The Media Engine is a second Allegrex core at the CPU clock, normally running
the firmware's audio/video codecs, which this port doesn't use. It is started
the way mcidclan's me-minimal-handler does it (MIT): a small reset handler is
copied to the ME reset vector (0xbfc00000, kernel only) and the ME is pulsed
out of reset, from kernel mode through the custom firmware's kuKernelCall.

The handler jumps to PSP_MeMain, ordinary engine code in main RAM: the ME runs
it in kernel mode, without syscalls (no libc that locks or allocates, no
kernel calls, no printing) and without a VFPU. The two CPUs' caches aren't
coherent: a job's memory is written back from the main CPU's cache before the
ME starts it (PSP_MeRun) and the ME writes its cache back before it reports
the job done. Between run and wait the main CPU must not touch the job's
memory, and output buffers must not share a 64 byte cache line with anything
the main CPU uses meanwhile. Jobs queue up (ME_QUEUE) and run in order:
PSP_MeRun returns a ticket to wait for one of them.
*/

#include "common.h"
#include "platform/platform.h"
#include <pspkernel.h>
#include <psputils.h>
#include <malloc.h>

#define ME_SRAM			0xbfc00000	// ME reset vector, uncached
#define ME_SRAM_ENTRY		0x100		// words the handler loads: entry, argument, stack
#define HW( addr )		( *(volatile uint32_t *)( addr ))
#define ME_STACK_SIZE		( 32 * 1024 )
#define ME_QUEUE		8		// jobs posted and not done yet, a power of 2

typedef void (*psp_mejob_t)( void *arg );

typedef struct
{
	volatile uint32_t	alive;		// ME: 1 running the job loop, 2 parked
	volatile uint32_t	stop;		// main: park
	volatile uint32_t	req;		// main: job number posted
	volatile uint32_t	done;		// ME: last job number finished
	volatile uint32_t	idle;		// ME: idle polls, to see it's alive
	uint32_t		fcsr;		// main: its FPU control word, for the ME's
	uint32_t		pad[10];
	struct
	{
		psp_mejob_t volatile	func;
		void * volatile		arg;
	} queue[ME_QUEUE];			// job n in queue[n % ME_QUEUE]
} me_ctl_t;				// two cache lines of its own

// SystemCtrlForUser / KUBridge, as in sys_psp.c
typedef struct
{
	uint32_t arg[12];
	uint32_t ret1, ret2;
} psp_kcall_args_t;
int kuKernelCall( void *func_addr, psp_kcall_args_t *args );

void (*psp_entities_hook)( void );	// the renderer's symbols are local to it: it sets this one

static me_ctl_t *me_ctl;		// uncached view
static void *me_ctl_mem, *me_stack;
static int me_state;			// 0 not tried, 1 running, -1 failed
static int me_local;			// a job is running on the main CPU (the ME reads 0 here)
static char me_status[128] = "not started";

/*
reset handler, copied to ME_SRAM: RAM access and bus clocks (read-modify-write,
the register is shared with the main CPU), kernel mode with the FPU on and
interrupts off, caches cleared by index, then a jump to the entry with the
argument and stack the main CPU left behind the handler
*/
extern const uint32_t psp_me_boot[], psp_me_boot_end[];
__asm__(
	"	.section .rodata\n"
	"	.align 6\n"
	"	.global psp_me_boot\n"
	"psp_me_boot:\n"
	"	.set push\n"
	"	.set noreorder\n"
	"	.set noat\n"
	"	lui	$k0, 0xbc10\n"
	"	lw	$k1, 0x40($k0)\n"	// 64 MB of RAM
	"	ori	$k1, $k1, 0x02\n"
	"	sw	$k1, 0x40($k0)\n"
	"	lw	$k1, 0x50($k0)\n"	// ME, AW bus A/B, eDRAM clocks
	"	ori	$k1, $k1, 0x0f\n"
	"	sw	$k1, 0x50($k0)\n"
	"	sync\n"
	"	lui	$k0, 0x3000\n"		// CU0 | CU1
	"	mtc0	$k0, $12\n"
	"	sync\n"
	"	lui	$k0, 0x8000\n"
	"	ori	$k1, $zero, 0x4000\n"
	"1:	cache	0x04, 0($k0)\n"		// I-cache index invalidate
	"	cache	0x14, 0($k0)\n"		// D-cache index writeback invalidate
	"	addiu	$k1, $k1, -64\n"
	"	bnez	$k1, 1b\n"
	"	addiu	$k0, $k0, 64\n"
	"	sync\n"
	"	lui	$k0, 0xbfc0\n"
	"	lw	$k1, 0x100($k0)\n"
	"	lw	$a0, 0x104($k0)\n"
	"	lw	$sp, 0x108($k0)\n"
	"	jr	$k1\n"
	"	nop\n"
	"	.set pop\n"
	"	.global psp_me_boot_end\n"
	"psp_me_boot_end:\n"
	"	.previous\n"
);

static inline void PSP_MeDcacheFlushAll( void )
{
	__asm__ volatile( "sync" );
	for( uint32_t a = 0x80000000; a < 0x80004000; a += 64 )
		__asm__ volatile( "cache 0x14, 0(%0)" :: "r"( a ) : "memory" );
	__asm__ volatile( "sync" ::: "memory" );
}

/*
the ME's job loop: it polls the uncached control block, one read every ~10 us.
Uncached reads go over the bus the main CPU uses for RAM: polling as fast as
it could (~600k reads a second) cost the main CPU ~2% of a frame
*/
static void __attribute__(( noinline )) PSP_MeMain( me_ctl_t *ctl )
{
	uint32_t done = ctl->done, polls = 0;

	// the same rounding and denormal flushing as the main CPU
	__asm__ volatile( "ctc1 %0, $31" :: "r"( ctl->fcsr ));
	ctl->alive = 1;

	for( ;; )
	{
		if( ctl->req != done )
		{
			const uint32_t next = done + 1;

			PSP_MeDcacheFlushAll(); // nothing stale from the last job
			ctl->queue[next % ME_QUEUE].func( ctl->queue[next % ME_QUEUE].arg );
			PSP_MeDcacheFlushAll(); // results into RAM before they're reported
			ctl->done = done = next;
			continue;
		}

		if(( ++polls & 255 ) == 0 )
		{
			ctl->idle = polls;
			if( ctl->stop )
				break;
		}

		// ~3300 cycles
		for( int i = 1100; i > 0; i-- )
			__asm__ volatile( "" );
	}

	ctl->alive = 2;
	for( ;; )
		__asm__ volatile( "nop" );
}

/*
PSP_MeWriteback

for a job on the ME that reports results before it ends: the range from its
data cache into RAM (the main CPU still has to invalidate it before reading
it). Run on the main CPU (the ME wasn't running) it does nothing: the cache
instructions are kernel only there
*/
void PSP_MeWriteback( const void *start, size_t size )
{
	const uintptr_t end = (uintptr_t)start + size;

	if( me_local )
		return;

	for( uintptr_t a = (uintptr_t)start & ~63; a < end; a += 64 )
		__asm__ volatile( "cache 0x1a, 0(%0)" :: "r"( a ) : "memory" ); // D-cache hit writeback
	__asm__ volatile( "sync" ::: "memory" );
}

static void PSP_MeRunLocal( psp_mejob_t func, void *arg )
{
	me_local = 1;
	func( arg );
	me_local = 0;
}

static int PSP_MeBootKernel( void )
{
	const uint32_t *src = psp_me_boot;
	volatile uint32_t *dst = (volatile uint32_t *)ME_SRAM;

	while( src < psp_me_boot_end )
		*dst++ = *src++;

	HW( ME_SRAM + ME_SRAM_ENTRY + 0 ) = (uint32_t)PSP_MeMain;
	HW( ME_SRAM + ME_SRAM_ENTRY + 4 ) = (uint32_t)me_ctl;
	HW( ME_SRAM + ME_SRAM_ENTRY + 8 ) = (uint32_t)me_stack + ME_STACK_SIZE - 64;
	__asm__ volatile( "sync" );

	HW( 0xbc10004c ) |= 0x04; // ME reset on
	__asm__ volatile( "sync" );
	HW( 0xbc10004c ) &= ~0x04; // and off: it starts at the reset vector
	__asm__ volatile( "sync" );

	return 0;
}

static int PSP_MeHaltKernel( void )
{
	HW( 0xbc10004c ) |= 0x04; // held in reset
	__asm__ volatile( "sync" );
	return 0;
}

qboolean PSP_MeStart( void )
{
	psp_kcall_args_t args = { 0 };
	int ret;

	if( me_state )
		return me_state > 0;

	me_state = -1;
	me_ctl_mem = memalign( 64, sizeof( me_ctl_t ));
	me_stack = memalign( 64, ME_STACK_SIZE );

	if( !me_ctl_mem || !me_stack )
	{
		Q_strncpy( me_status, "out of memory", sizeof( me_status ));
		return false;
	}

	memset( me_ctl_mem, 0, sizeof( me_ctl_t ));
	__asm__ volatile( "cfc1 %0, $31" : "=r"( ((me_ctl_t *)me_ctl_mem )->fcsr ));
	sceKernelDcacheWritebackInvalidateAll();
	me_ctl = (me_ctl_t *)( 0x40000000 | (uint32_t)me_ctl_mem );

	ret = kuKernelCall( (void *)PSP_MeBootKernel, &args );

	// it sets alive within microseconds
	for( int i = 0; i < 1000 && !me_ctl->alive; i++ )
		sceKernelDelayThread( 100 );

	if( me_ctl->alive != 1 )
	{
		Q_snprintf( me_status, sizeof( me_status ), "no answer (kuKernelCall 0x%08x)", ret );
		kuKernelCall( (void *)PSP_MeHaltKernel, &args );
		return false;
	}

	Q_strncpy( me_status, "running", sizeof( me_status ));
	me_state = 1;
	return true;
}

qboolean PSP_MeActive( void )
{
	return me_state > 0;
}

static int PSP_MeHaltKernel( void );

/*
PSP_MeWaitFor

until job ticket is done (0: nothing to wait for)
*/
void PSP_MeWaitFor( uint32_t ticket )
{
	uint32_t t0;

	if( me_state <= 0 || !ticket || (int32_t)( me_ctl->done - ticket ) >= 0 )
		return;

	t0 = sceKernelGetSystemTimeLow();
	while( (int32_t)( me_ctl->done - ticket ) < 0 )
	{
		// a crashed ME never answers: give up on it, the jobs run here from now on
		if( sceKernelGetSystemTimeLow() - t0 > 500000 )
		{
			psp_kcall_args_t args = { 0 };

			kuKernelCall( (void *)PSP_MeHaltKernel, &args );
			Q_snprintf( me_status, sizeof( me_status ), "stopped answering (job %u, done %u)", (uint)me_ctl->req, (uint)me_ctl->done );
			Con_Printf( S_ERROR "ME: %s\n", me_status );
			me_state = -1;
			return;
		}
	}
}

qboolean PSP_MeDone( uint32_t ticket )
{
	return me_state <= 0 || !ticket || (int32_t)( me_ctl->done - ticket ) >= 0;
}

void PSP_MeWait( void )
{
	if( me_state > 0 )
		PSP_MeWaitFor( me_ctl->req );
}

qboolean PSP_MeBusy( void )
{
	return me_state > 0 && me_ctl->req != me_ctl->done;
}

/*
PSP_MeRun

queues func( arg ) on the ME and returns its ticket, or runs it here (ticket 0)
if the ME isn't running
*/
static uint32_t PSP_MePost( psp_mejob_t func, void *arg, qboolean invalidate )
{
	uint32_t next;

	if( me_state <= 0 )
	{
		PSP_MeRunLocal( func, arg );
		return 0;
	}

	// the queue is full: wait for the oldest
	next = me_ctl->req + 1;
	PSP_MeWaitFor( next - ME_QUEUE );
	if( me_state <= 0 )
	{
		PSP_MeRunLocal( func, arg );
		return 0;
	}

	if( invalidate )
		sceKernelDcacheWritebackInvalidateAll();
	else sceKernelDcacheWritebackAll();
	me_ctl->queue[next % ME_QUEUE].func = func;
	me_ctl->queue[next % ME_QUEUE].arg = arg;
	__asm__ volatile( "sync" ::: "memory" );
	me_ctl->req = next;
	return next;
}

uint32_t PSP_MeRun( psp_mejob_t func, void *arg )
{
	return PSP_MePost( func, arg, true );
}

/*
PSP_MeRunKeep

the same, but this CPU keeps its cache (a full invalidate costs the misses to
fill it again): it has to invalidate the ranges the ME wrote itself, or read
them uncached
*/
uint32_t PSP_MeRunKeep( psp_mejob_t func, void *arg )
{
	return PSP_MePost( func, arg, false );
}

void PSP_MeShutdown( void )
{
	psp_kcall_args_t args = { 0 };

	if( me_state <= 0 )
		return;

	PSP_MeWait();
	me_ctl->stop = 1;
	for( int i = 0; i < 100 && me_ctl->alive != 2; i++ )
		sceKernelDelayThread( 100 );

	kuKernelCall( (void *)PSP_MeHaltKernel, &args );
	me_state = 0;
}

/*
==================
psp_me_test

diagnostic: round trip of an empty job, and the same float and integer work on both CPUs
==================
*/
typedef struct
{
	int	n;
	float	*data;
	float	fsum;
	uint32_t	isum;
	uint32_t	pad[12];
} me_testjob_t;

static void PSP_MeTestEmpty( void *arg )
{
}

static void PSP_MeTestWork( void *arg )
{
	me_testjob_t *job = arg;
	float f = 0.0f;
	uint32_t h = 2166136261u;

	for( int r = 0; r < 4; r++ )
	{
		for( int i = 0; i < job->n; i++ )
		{
			f += job->data[i] * 0.5f + f * 0.0001f;
			h = ( h ^ (uint32_t)( job->data[i] * 16.0f )) * 16777619u;
		}
	}

	job->fsum = f;
	job->isum = h;
}

static void PSP_MeTest_f( void )
{
	static me_testjob_t job __attribute__(( aligned( 64 )));
	me_testjob_t ref;
	uint32_t t0, t1, idle0;

	if( !PSP_MeStart( ))
	{
		Con_Printf( "ME: %s\n", me_status );
		return;
	}

	idle0 = me_ctl->idle;
	sceKernelDelayThread( 10000 );
	Con_Printf( "ME: %s, %u idle polls in 10 ms\n", me_status, me_ctl->idle - idle0 );

	t0 = sceKernelGetSystemTimeLow();
	for( int i = 0; i < 100; i++ )
	{
		PSP_MeRun( PSP_MeTestEmpty, NULL );
		PSP_MeWait();
	}
	t1 = sceKernelGetSystemTimeLow();
	Con_Printf( "ME: empty job round trip %.2f us\n", ( t1 - t0 ) / 100.0f );

	job.n = 16384;
	job.data = memalign( 64, job.n * sizeof( float ));
	if( !job.data )
		return;
	for( int i = 0; i < job.n; i++ )
		job.data[i] = (float)(( i * 7919 ) % 1000 ) * 0.01f;

	ref = job;
	t0 = sceKernelGetSystemTimeLow();
	PSP_MeTestWork( &ref );
	t1 = sceKernelGetSystemTimeLow();
	Con_Printf( "ME: work on the main CPU %u us: %f %08x\n", t1 - t0, ref.fsum, ref.isum );

	t0 = sceKernelGetSystemTimeLow();
	PSP_MeRun( PSP_MeTestWork, &job );
	PSP_MeWait();
	t1 = sceKernelGetSystemTimeLow();
	Con_Printf( "ME: work on the ME %u us: %f %08x, %s\n", t1 - t0,
		job.fsum, job.isum, job.fsum == ref.fsum && job.isum == ref.isum ? "same" : "DIFFERENT" );

	// both at once: does the ME slow the main CPU down?
	PSP_MeRun( PSP_MeTestWork, &job );
	t0 = sceKernelGetSystemTimeLow();
	PSP_MeTestWork( &ref );
	t1 = sceKernelGetSystemTimeLow();
	PSP_MeWait();
	Con_Printf( "ME: main CPU work while the ME works %u us: %f\n", t1 - t0, ref.fsum );

	free( job.data );
	job.data = NULL;
}

void PSP_MeRegisterCommands( void )
{
	Cmd_AddCommand( "psp_me_test", PSP_MeTest_f, "start the Media Engine and time jobs on it" );
}
