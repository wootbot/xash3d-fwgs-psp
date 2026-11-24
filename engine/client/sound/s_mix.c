/*
s_mix.c - portable code to mix sounds
Copyright (C) 2009 Uncle Mike

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

#include "common.h"
#include "sound.h"
#include "client.h"
#if XASH_PSP
#include "platform/platform.h"
#include <psputils.h>
#endif

static portable_samplepair_t roombuffer[(PAINTBUFFER_SIZE+1)], paintbuffer[(PAINTBUFFER_SIZE+1)];

#if XASH_PSP
// the sample and the volumes in locals: through the pointers the compiler had to
// load them again after every store into pbuf (it may alias them).
// 8 bit mono: both products from one multiply, s * ( vl + vr * 65536 ): with
// |s| <= 128 and volumes <= 255 the left one fits the low 16 bits, signed
#define S_MIX8_PACKED( pb, s, vp ) \
	do { const int _p = (s) * (vp); const int _l = (int16_t)_p; ( pb ).left += _l; ( pb ).right += ( _p - _l ) >> 16; } while( 0 )

#define S_MakeMixMono( x ) \
	static void S_MixMono ## x( portable_samplepair_t *pbuf, const int *volume, const void *buf, int num_samples ) \
	{ \
		const int##x##_t *data = buf; \
		const int vl = volume[0], vr = volume[1]; \
		if( x == 8 ) \
		{ \
			const int vp = vl + ( vr << 16 ); \
			for( int i = 0; i < num_samples; i++ ) \
				S_MIX8_PACKED( pbuf[i], data[i], vp ); \
			return; \
		} \
		for( int i = 0; i < num_samples; i++ ) \
		{ \
			const int s = data[i]; \
			pbuf[i].left  += ( s * vl ) >> ( x - 8 ); \
			pbuf[i].right += ( s * vr ) >> ( x - 8 ); \
		} \
	} \

#else
#define S_MakeMixMono( x ) \
	static void S_MixMono ## x( portable_samplepair_t *pbuf, const int *volume, const void *buf, int num_samples ) \
	{ \
		const int##x##_t *data = buf; \
		for( int i = 0; i < num_samples; i++ ) \
		{ \
			pbuf[i].left  += ( data[i] * volume[0] ) >> ( x - 8 ); \
			pbuf[i].right += ( data[i] * volume[1] ) >> ( x - 8 ); \
		} \
	} \

#endif

#define S_MakeMixStereo( x ) \
	static void S_MixStereo ## x( portable_samplepair_t *pbuf, const int *volume, const void *buf, int num_samples ) \
	{ \
		const int##x##_t *data = buf; \
		for( int i = 0; i < num_samples; i++ ) \
		{ \
			pbuf[i].left  += ( data[i * 2 + 0] * volume[0] ) >> ( x - 8 ); \
			pbuf[i].right += ( data[i * 2 + 1] * volume[1] ) >> ( x - 8 ); \
		} \
	} \

#if XASH_PSP
// the PSP's FPU is single precision only: double math in these per-sample loops
// is emulated in software and made mixing the most expensive part of a firefight.
// Step through the source with a 32-bit fixed point fraction instead: the caller
// passes the position's fraction and the step in 32.32 fixed point
typedef struct { uint i; uint32_t f; } s_fixstep_t;

#define S_FIXED_STEP( offset_frac, rate_scale ) \
	const uint step_int = ( rate_scale ).i; \
	const uint32_t step_frac = ( rate_scale ).f; \
	uint32_t frac = ( offset_frac ); \
	uint sample_idx = 0

#define S_FIXED_ADVANCE( shift ) \
	do { uint32_t old = frac; frac += step_frac; sample_idx += ( step_int + ( frac < old )) << ( shift ); } while( 0 )

// source sample a..b at the current fraction, 16 bits of weight
#define S_FIXED_LERP( a, b ) ( (a) + (int)((( (int)(b) - (int)(a) ) * (int)( frac >> 16 )) >> 16 ))

#define S_MakeMixMonoPitch( x ) \
	static void S_MixMonoPitch ## x( portable_samplepair_t *pbuf, const int *volume, const void *buf, uint32_t offset_frac, s_fixstep_t rate_scale, int num_samples ) \
	{ \
		const int##x##_t *data = buf; \
		const int vl = volume[0], vr = volume[1]; \
		S_FIXED_STEP( offset_frac, rate_scale ); \
		if( x == 8 ) \
		{ \
			const int vp = vl + ( vr << 16 ); \
			for( int i = 0; i < num_samples; i++ ) \
			{ \
				S_MIX8_PACKED( pbuf[i], data[sample_idx], vp ); \
				S_FIXED_ADVANCE( 0 ); \
			} \
			return; \
		} \
		for( int i = 0; i < num_samples; i++ ) \
		{ \
			const int s = data[sample_idx]; \
			pbuf[i].left  += ( s * vl ) >> ( x - 8 ); \
			pbuf[i].right += ( s * vr ) >> ( x - 8 ); \
			S_FIXED_ADVANCE( 0 ); \
		} \
	} \

#define S_MakeMixStereoPitch( x ) \
	static void S_MixStereoPitch ## x( portable_samplepair_t *pbuf, const int *volume, const void *buf, uint32_t offset_frac, s_fixstep_t rate_scale, int num_samples ) \
	{ \
		const int##x##_t *data = buf; \
		const int vl = volume[0], vr = volume[1]; \
		S_FIXED_STEP( offset_frac, rate_scale ); \
		for( int i = 0; i < num_samples; i++ ) \
		{ \
			pbuf[i].left  += ( data[sample_idx+0] * vl ) >> ( x - 8 ); \
			pbuf[i].right += ( data[sample_idx+1] * vr ) >> ( x - 8 ); \
			S_FIXED_ADVANCE( 1 ); \
		} \
	} \

#define S_MakeMixMonoLerp( x ) \
	static void S_MixMonoLerp ## x( portable_samplepair_t *pbuf, const int *volume, const void *buf, uint32_t offset_frac, s_fixstep_t rate_scale, int num_samples ) \
	{ \
		const int##x##_t *data = buf; \
		S_FIXED_STEP( offset_frac, rate_scale ); \
		for( int i = 0; i < num_samples; i++ ) \
		{ \
			int s = S_FIXED_LERP( data[sample_idx], data[sample_idx + 1] ); \
			pbuf[i].left  += ( s * volume[0] ) >> ( x - 8 ); \
			pbuf[i].right += ( s * volume[1] ) >> ( x - 8 ); \
			S_FIXED_ADVANCE( 0 ); \
		} \
	} \

#define S_MakeMixStereoLerp( x ) \
	static void S_MixStereoLerp ## x( portable_samplepair_t *pbuf, const int *volume, const void *buf, uint32_t offset_frac, s_fixstep_t rate_scale, int num_samples ) \
	{ \
		const int##x##_t *data = buf; \
		S_FIXED_STEP( offset_frac, rate_scale ); \
		for( int i = 0; i < num_samples; i++ ) \
		{ \
			int sl = S_FIXED_LERP( data[sample_idx + 0], data[sample_idx + 2] ); \
			int sr = S_FIXED_LERP( data[sample_idx + 1], data[sample_idx + 3] ); \
			pbuf[i].left  += ( sl * volume[0] ) >> ( x - 8 ); \
			pbuf[i].right += ( sr * volume[1] ) >> ( x - 8 ); \
			S_FIXED_ADVANCE( 1 ); \
		} \
	} \

#else // !XASH_PSP

#define S_MakeMixMonoPitch( x ) \
	static void S_MixMonoPitch ## x( portable_samplepair_t *pbuf, const int *volume, const void *buf, double offset_frac, double rate_scale, int num_samples ) \
	{ \
		const int##x##_t *data = buf; \
		uint sample_idx = 0; \
		for( int i = 0; i < num_samples; i++ ) \
		{ \
			pbuf[i].left  += ( data[sample_idx] * volume[0] ) >> ( x - 8 ); \
			pbuf[i].right += ( data[sample_idx] * volume[1] ) >> ( x - 8 ); \
			offset_frac += rate_scale; \
			sample_idx += (uint)offset_frac; \
			offset_frac -= (uint)offset_frac; \
		} \
	} \

#define S_MakeMixStereoPitch( x ) \
	static void S_MixStereoPitch ## x( portable_samplepair_t *pbuf, const int *volume, const void *buf, double offset_frac, double rate_scale, int num_samples ) \
	{ \
		const int##x##_t *data = buf; \
		uint sample_idx = 0; \
		for( int i = 0; i < num_samples; i++ ) \
		{ \
			pbuf[i].left  += ( data[sample_idx+0] * volume[0] ) >> ( x - 8 ); \
			pbuf[i].right += ( data[sample_idx+1] * volume[1] ) >> ( x - 8 ); \
			offset_frac += rate_scale; \
			sample_idx += (uint)offset_frac << 1; \
			offset_frac -= (uint)offset_frac; \
		} \
	} \

#define S_MakeMixMonoLerp( x ) \
	static void S_MixMonoLerp ## x( portable_samplepair_t *pbuf, const int *volume, const void *buf, double offset_frac, double rate_scale, int num_samples ) \
	{ \
		const int##x##_t *data = buf; \
		uint sample_idx = 0; \
		for( int i = 0; i < num_samples; i++ ) \
		{ \
			int s = (int)( data[sample_idx] * ( 1.0 - offset_frac ) + data[sample_idx + 1] * offset_frac ); \
			pbuf[i].left  += ( s * volume[0] ) >> ( x - 8 ); \
			pbuf[i].right += ( s * volume[1] ) >> ( x - 8 ); \
			offset_frac += rate_scale; \
			sample_idx += (uint)offset_frac; \
			offset_frac -= (uint)offset_frac; \
		} \
	} \

#define S_MakeMixStereoLerp( x ) \
	static void S_MixStereoLerp ## x( portable_samplepair_t *pbuf, const int *volume, const void *buf, double offset_frac, double rate_scale, int num_samples ) \
	{ \
		const int##x##_t *data = buf; \
		uint sample_idx = 0; \
		for( int i = 0; i < num_samples; i++ ) \
		{ \
			int sl = (int)( data[sample_idx + 0] * ( 1.0 - offset_frac ) + data[sample_idx + 2] * offset_frac ); \
			int sr = (int)( data[sample_idx + 1] * ( 1.0 - offset_frac ) + data[sample_idx + 3] * offset_frac ); \
			pbuf[i].left  += ( sl * volume[0] ) >> ( x - 8 ); \
			pbuf[i].right += ( sr * volume[1] ) >> ( x - 8 ); \
			offset_frac += rate_scale; \
			sample_idx += (uint)offset_frac << 1; \
			offset_frac -= (uint)offset_frac; \
		} \
	} \

#endif // !XASH_PSP

#if XASH_PSP
typedef uint32_t s_mixfrac_t;
typedef s_fixstep_t s_mixstep_t;
#define S_MIX_UNIT_RATE( frac, rate ) ( (rate).i == 1 && !(rate).f && !(frac) )
#else
typedef double s_mixfrac_t;
typedef double s_mixstep_t;
#define S_MIX_UNIT_RATE( frac, rate ) ( Q_equal( rate, 1.0 ) && Q_equal( frac, 0.0 ))
#endif

S_MakeMixMono( 8 )
S_MakeMixMono( 16 )
S_MakeMixStereo( 8 )
S_MakeMixStereo( 16 )
S_MakeMixMonoPitch( 8 )
S_MakeMixMonoPitch( 16 )
S_MakeMixStereoPitch( 8 )
S_MakeMixStereoPitch( 16 )
S_MakeMixMonoLerp( 8 )
S_MakeMixMonoLerp( 16 )
S_MakeMixStereoLerp( 8 )
S_MakeMixStereoLerp( 16 )

static void S_MixAudio( portable_samplepair_t *pbuf, const int *pvol, const void *buf, int channels, int width, s_mixfrac_t offset_frac, s_mixstep_t rate_scale, int num_samples, qboolean lerp )
{
	if( S_MIX_UNIT_RATE( offset_frac, rate_scale ))
	{
		if( channels == 1 )
		{
			if( width == 1 )
				S_MixMono8( pbuf, pvol, buf, num_samples );
			else
				S_MixMono16( pbuf, pvol, buf, num_samples );
		}
		else
		{
			if( width == 1 )
				S_MixStereo8( pbuf, pvol, buf, num_samples );
			else
				S_MixStereo16( pbuf, pvol, buf, num_samples );
		}
	}
	else if( lerp )
	{
		if( channels == 1 )
		{
			if( width == 1 )
				S_MixMonoLerp8( pbuf, pvol, buf, offset_frac, rate_scale, num_samples );
			else
				S_MixMonoLerp16( pbuf, pvol, buf, offset_frac, rate_scale, num_samples );
		}
		else
		{
			if( width == 1 )
				S_MixStereoLerp8( pbuf, pvol, buf, offset_frac, rate_scale, num_samples );
			else
				S_MixStereoLerp16( pbuf, pvol, buf, offset_frac, rate_scale, num_samples );
		}
	}
	else
	{
		if( channels == 1 )
		{
			if( width == 1 )
				S_MixMonoPitch8( pbuf, pvol, buf, offset_frac, rate_scale, num_samples );
			else
				S_MixMonoPitch16( pbuf, pvol, buf, offset_frac, rate_scale, num_samples );
		}
		else
		{
			if( width == 1 )
				S_MixStereoPitch8( pbuf, pvol, buf, offset_frac, rate_scale, num_samples );
			else
				S_MixStereoPitch16( pbuf, pvol, buf, offset_frac, rate_scale, num_samples );
		}
	}
}

#if !XASH_PSP
static int S_AdjustNumSamples( channel_t *chan, int num_samples, double rate, double timecompress_rate )
{
	if( FBitSet( chan->flags, FL_CHAN_FINISHED ))
		return 0;

	// if channel is set to end at specific sample,
	// detect if it's the last mixing pass and truncate
	if( chan->forced_end )
	{
		// calculate the last sample position
		double end_sample = chan->sample + rate * num_samples * timecompress_rate;

		if( end_sample >= chan->forced_end )
		{
			SetBits( chan->flags, FL_CHAN_FINISHED );
			return floor(( chan->forced_end - chan->sample ) / ( rate * timecompress_rate ));
		}
	}

	return num_samples;
}
#endif // !XASH_PSP

#if XASH_PSP
typedef float s_pitch_t;

static s_fixstep_t S_FixedStep( float rate )
{
	s_fixstep_t s;

	s.i = (uint)rate;
	s.f = (uint32_t)(( rate - (float)s.i ) * 4294967296.0f );
	return s;
}

static inline void S_FixedAdvance( uint32_t *pos, uint32_t *frac, s_fixstep_t step, uint n )
{
	const uint64_t f = (uint64_t)*frac + (uint64_t)step.f * n;

	*pos += step.i * n + (uint32_t)( f >> 32 );
	*frac = (uint32_t)f;
}

// source sample (relative to the start) the mixer reads for output sample n - 1
static inline uint S_FixedLastRead( uint32_t frac, s_fixstep_t step, uint n )
{
	return step.i * ( n - 1 ) + (uint)(( (uint64_t)frac + (uint64_t)step.f * ( n - 1 )) >> 32 );
}

/*
chan->sample, a double, to 32.32 fixed point and back from its bits: the
casts and the double math were ~9 software calls, ~10 us a channel and
mix. Back to a double the bits past 53 are cut, not rounded (from 2^21
samples on, ~3 minutes into a sound)
*/
static void S_DoubleToFixed( double d, uint32_t *pos, uint32_t *frac )
{
	uint64_t b, f;

	memcpy( &b, &d, sizeof( b ));

	// zero, negative or NaN: the start
	if( (int64_t)b <= 0 || ( b >> 52 ) >= 0x7ff )
	{
		*pos = *frac = 0;
		return;
	}

	const int e = (int)( b >> 52 ) - 1023; // d = m * 2^( e - 52 )
	const uint64_t m = ( b & (( 1ull << 52 ) - 1 )) | ( 1ull << 52 );
	const int shift = e - 20; // the fixed point value is m * 2^( e - 20 )

	if( e >= 32 )
		f = ~0ull;
	else if( shift >= 0 )
		f = m << shift;
	else if( shift > -64 )
		f = m >> -shift;
	else f = 0;

	*pos = (uint32_t)( f >> 32 );
	*frac = (uint32_t)f;
}

static double S_FixedToDouble( uint32_t pos, uint32_t frac )
{
	const uint64_t f = ((uint64_t)pos << 32 ) | frac;
	double d;

	if( !f )
		return 0.0;

	const int lz = pos ? __builtin_clz( pos ) : 32 + __builtin_clz( frac );
	const uint64_t m = ( f << lz ) >> 11; // 53 bits, the top one implicit
	const uint64_t b = ((uint64_t)( 63 - lz - 32 + 1023 ) << 52 ) | ( m & (( 1ull << 52 ) - 1 ));

	memcpy( &d, &b, sizeof( d ));
	return d;
}

/*
the same as the generic version below, but the source position is stepped in 32.32
fixed point: with no double precision FPU, its double math cost ~30 us per channel
and paint call (~60 software calls), a third of all mixing in a firefight.
chan->sample stays a double for the rest of the engine, converted once each way
*/
/*
the loudest s_mixchannels channels are mixed, the others only move on through
their sound (s_mix_dry): a firefight has ~45 at once, the quiet ones under the
gunfire of the loud ones, and mixing them was ~0.4 ms a frame
*/
CVAR_DEFINE_AUTO( s_mixchannels, "16", FCVAR_ARCHIVE, "without the Media Engine, mix only this many of the loudest sounds, the others play on silently (0 = all)" );
static qboolean s_mix_dry;

/*
the sample loops run on the Media Engine (me_psp.c): the main CPU does all of
the bookkeeping, records what S_MixAudio would do for the normal channels and
mixes the rest (raw streams, sentences) itself. During the next frame the ME
mixes the recorded channels, adds the main CPU's room and paint buffers, runs
the room DSP and the gain and clips it into 16 bit samples, which the next
paint copies into the DMA buffer. That is a frame later, well inside the
mixahead. The ME reads sample data and the main CPU's buffers and writes only
s_me and the DSP state (sx_me, cache line aligned); sounds aren't freed while it
runs (S_PaintSync)
*/
CVAR_DEFINE_AUTO( s_mixme, "1", 0, "mix sounds on the Media Engine" );

typedef struct
{
	const void	*audio;
	int		offset;
	int		pvol[2];
	uint32_t	frac;
	s_fixstep_t	step;
	short		count;
	byte		channels;
	byte		width;
	int		lerp;
} s_mixop_t;

#define S_ME_MAXOPS 256

typedef struct
{
	int			num_ops;
	int			num_samples;
	const s_mixop_t		*ops;
	portable_samplepair_t	*out;
	const portable_samplepair_t	*room_in;	// the main CPU's part
	const portable_samplepair_t	*paint_in;
	int			room_channels;
	int			gain;
	qboolean		dsp;
	short			*out16;
} s_mejob_t;

static struct
{
	s_mixop_t		ops[S_ME_MAXOPS] __attribute__(( aligned( 64 )));
	portable_samplepair_t	room[PAINTBUFFER_SIZE + 8] __attribute__(( aligned( 64 ))); // whole cache lines
	short			out16[PAINTBUFFER_SIZE * 2] __attribute__(( aligned( 64 ))); // the ME's output
	s_mejob_t		job __attribute__(( aligned( 64 )));
	byte			pad[64] __attribute__(( aligned( 64 ))); // nothing else in job's line
} s_me __attribute__(( aligned( 64 )));

static int s_me_numops;
static qboolean s_mix_defer; // set around the normal channels when the ME is used

// the chunk waiting for the ME
static struct
{
	qboolean	active;
	int		start, end;
	uint32_t	ticket;
} s_me_pending;

static int S_MixChannelToBuffer( portable_samplepair_t *pbuf, channel_t *chan, int num_samples, int out_rate, s_pitch_t pitch, int offset, int timecompress )
{
	const int initial_offset = offset;
	const int pvol[2] =
	{
		bound( 0, chan->leftvol, 255 ),
		bound( 0, chan->rightvol, 255 ),
	};
	const wavdata_t *wav = chan->sfx->cache;
	const float rate = pitch * (float)wav->rate / (float)out_rate;
	uint64_t forced_end_bits;

	// timecompress at 100% is skipping the entire sfx, so mark as finished and exit
	if( timecompress >= 100 )
	{
		SetBits( chan->flags, FL_CHAN_FINISHED );
		return 0;
	}

	if( FBitSet( chan->flags, FL_CHAN_FINISHED ) || !( rate > 0.0f ))
		return 0;

	// the read position advances by rate per output sample, the channel by rate * timecompress
	const float advance = timecompress ? rate / ( 1.0f - timecompress * 0.01f ) : rate;
	const s_fixstep_t read_step = S_FixedStep( rate );
	const s_fixstep_t pos_step = timecompress ? S_FixedStep( advance ) : read_step;

	// if channel is set to end at specific sample, detect if it's the last mixing pass
	// and truncate (rare: test the bits, a double compare is a software call too)
	memcpy( &forced_end_bits, &chan->forced_end, sizeof( forced_end_bits ));
	if( forced_end_bits << 1 )
	{
		double end_sample = chan->sample + (double)advance * num_samples;

		if( end_sample >= chan->forced_end )
		{
			SetBits( chan->flags, FL_CHAN_FINISHED );
			num_samples = floor(( chan->forced_end - chan->sample ) / advance );
		}
	}

	if( num_samples <= 0 )
		return 0;

	uint32_t pos, frac;
	S_DoubleToFixed( chan->sample, &pos, &frac );

	// linear interpolation needs one sample of lookahead beyond the last read position
	const int lookahead = s_lerping.value ? 1 : 0;

	while( num_samples > 0 )
	{
		// get total amount of samples we want, up to the last position, including lookahead
		uint32_t end_pos = pos, end_frac = frac;
		S_FixedAdvance( &end_pos, &end_frac, pos_step, num_samples );
		int request_num_samples = (int)( end_pos - pos ) + ( end_frac ? 1 : 0 ) + lookahead;

		// get sample pointer and also amount of samples available
		const void *audio = NULL;
		int available = S_RetrieveAudioSamples( wav, &audio, pos, request_num_samples, FBitSet( chan->flags, FL_CHAN_USE_LOOP ));

		// no samples available, exit
		if( !available )
			break;

		// can interpolate only when at least two source samples are available
		qboolean lerp = lookahead && available >= 2;

		// this is how much data we output
		int out_count = num_samples;
		if( request_num_samples > available )
		{
			const float sample_frac = frac * ( 1.0f / 4294967296.0f );

			// lerp needs sample_idx + 1 to stay within available, so cap by available - 1
			if( lerp )
				out_count = (int)floorf(( available - 1 - sample_frac ) / rate );
			else
				out_count = (int)ceilf(( available - sample_frac ) / rate );

			if( out_count > num_samples )
				out_count = num_samples;

			// float rounding: back off until the last read is within available
			while( out_count > 0 && S_FixedLastRead( frac, read_step, out_count ) + lerp >= (uint)available )
				out_count--;
		}

		// near a buffer boundary (e.g. just before a loop wrap) lerp may yield zero;
		// fall back to nearest for one sample to keep the position advancing
		if( out_count <= 0 )
		{
			lerp = false;
			out_count = 1;
		}

		if( s_mix_dry )
			;
		else if( s_mix_defer && s_me_numops < S_ME_MAXOPS )
		{
			s_mixop_t *op = &s_me.ops[s_me_numops++];

			op->audio = audio;
			op->offset = pbuf + offset - roombuffer;
			op->pvol[0] = pvol[0];
			op->pvol[1] = pvol[1];
			op->frac = frac;
			op->step = read_step;
			op->count = out_count;
			op->channels = wav->channels;
			op->width = wav->width;
			op->lerp = lerp;
		}
		else
			S_MixAudio( pbuf + offset, pvol, audio, wav->channels, wav->width, frac, read_step, out_count, lerp );

		S_FixedAdvance( &pos, &frac, pos_step, out_count );
		offset += out_count;
		num_samples -= out_count;
	}

	chan->sample = S_FixedToDouble( pos, frac );

	// samples couldn't be retrieved, mark as finished
	if( num_samples > 0 )
		SetBits( chan->flags, FL_CHAN_FINISHED );

	// total amount of samples mixed
	return offset - initial_offset;
}
#else // !XASH_PSP
typedef double s_pitch_t;

static int S_MixChannelToBuffer( portable_samplepair_t *pbuf, channel_t *chan, int num_samples, int out_rate, double pitch, int offset, int timecompress )
{
	const int initial_offset = offset;
	const int pvol[2] =
	{
		bound( 0, chan->leftvol, 255 ),
		bound( 0, chan->rightvol, 255 ),
	};
	double rate = pitch * chan->sfx->cache->rate / (double)out_rate;

	// timecompress at 100% is skipping the entire sfx, so mark as finished and exit
	if( timecompress >= 100 )
	{
		SetBits( chan->flags, FL_CHAN_FINISHED );
		return 0;
	}

	double timecompress_rate = 1 / ( 1 - timecompress / 100.0 );

	num_samples = S_AdjustNumSamples( chan, num_samples, rate, timecompress_rate );
	if( num_samples == 0 )
		return 0;

	// linear interpolation needs one sample of lookahead beyond the last read position
	const int lookahead = s_lerping.value ? 1 : 0;

	while( num_samples > 0 )
	{
		// calculate the last sample position
		double end_sample = chan->sample + rate * num_samples * timecompress_rate;

		// and get total amount of samples we want, including lookahead for interpolation
		int request_num_samples = (int)(ceil( end_sample ) - floor( chan->sample )) + lookahead;

		// get sample pointer and also amount of samples available
		const void *audio = NULL;
		int available = S_RetrieveAudioSamples( chan->sfx->cache, &audio, chan->sample, request_num_samples, FBitSet( chan->flags, FL_CHAN_USE_LOOP ));

		// no samples available, exit
		if( !available )
			break;

		double sample_frac = chan->sample - floor( chan->sample );

		// can interpolate only when at least two source samples are available
		qboolean lerp = lookahead && available >= 2;

		// this is how much data we output
		int out_count = num_samples;
		if( request_num_samples > available )
		{
			// lerp needs sample_idx + 1 to stay within available, so cap by available - 1
			if( lerp )
				out_count = (int)floor(( available - 1 - sample_frac ) / rate );
			else
				out_count = (int)ceil(( available - sample_frac ) / rate );

			if( out_count > num_samples )
				out_count = num_samples;
		}

		// near a buffer boundary (e.g. just before a loop wrap) lerp may yield zero;
		// fall back to nearest for one sample to keep chan->sample advancing
		if( out_count <= 0 )
		{
			lerp = false;
			out_count = 1;
		}

		const wavdata_t *wav = chan->sfx->cache;
		S_MixAudio( pbuf + offset, pvol, audio, wav->channels, wav->width, sample_frac, rate, out_count, lerp );

		chan->sample += out_count * rate * timecompress_rate;
		offset += out_count;
		num_samples -= out_count;
	}

	// samples couldn't be retrieved, mark as finished
	if( num_samples > 0 )
		SetBits( chan->flags, FL_CHAN_FINISHED );

	// total amount of samples mixed
	return offset - initial_offset;
}
#endif // !XASH_PSP

static int VOX_MixChannelToBuffer( portable_samplepair_t *pbuf, channel_t *chan, int num_samples, int out_rate, s_pitch_t pitch )
{
	int	offset = 0;

	if( FBitSet( chan->flags, FL_CHAN_SENTENCE_FINISHED ))
		return 0;

	while( num_samples > 0 && !FBitSet( chan->flags, FL_CHAN_SENTENCE_FINISHED ))
	{
		int	outputCount = S_MixChannelToBuffer( pbuf, chan, num_samples, out_rate, pitch, offset, chan->words[chan->word_index].timecompress );

		offset += outputCount;
		num_samples -= outputCount;

		// if we finished load a next word
		if( FBitSet( chan->flags, FL_CHAN_FINISHED ))
		{
			VOX_FreeWord( chan );
			chan->word_index++;

			if( chan->word_index < CVOXWORDMAX && chan->words[chan->word_index].sfx && S_SoundPending( chan->words[chan->word_index].sfx ))
			{
				// the sentence goes on when it's there
				chan->sfx = chan->words[chan->word_index].sfx;
				break;
			}

			VOX_LoadWord( chan );

			if( !FBitSet( chan->flags, FL_CHAN_SENTENCE_FINISHED ))
				chan->sfx = chan->words[chan->word_index].sfx;
		}
	}

	return offset;
}

static int S_MixNormalChannelsToRoombuffer( portable_samplepair_t *dst, int end )
{
	const qboolean sp = Host_IsSinglePlayerGame();
	const qboolean ingame = CL_IsInGame();
	const int out_rate = snd.format.speed;
	const int num_samples = end - snd.paintedtime;

	// FWGS feature: make everybody sound like chipmunks when we're going fast
	const float pitch_mult = ( sys_timescale.value + 1 ) / 2;

	int num_mixed_channels = 0;

#if XASH_PSP
	// the volume from which on channels are mixed, and how many at exactly that one
	int mix_volume = 0, mix_at_volume = MAX_CHANNELS;

	// the Media Engine has the time for all of them
	if( s_mixchannels.value > 0 && !( s_mixme.value && PSP_MeStart( )))
	{
		static int count[256];
		int audible = 0;

		memset( count, 0, sizeof( count ));
		for( int i = 0; i < snd.total_channels; i++ )
		{
			const channel_t *ch = &snd.channels[i];
			const int vol = Q_max( ch->leftvol, ch->rightvol );

			if( ch->sfx && !ch->words && vol >= 8 )
			{
				count[bound( 0, vol, 255 )]++;
				audible++;
			}
		}

		if( audible > s_mixchannels.value )
		{
			int left = s_mixchannels.value;

			for( mix_volume = 255; mix_volume > 0 && count[mix_volume] < left; mix_volume-- )
				left -= count[mix_volume];
			mix_at_volume = left;
		}
	}
#endif

	if( num_samples <= 0 )
		return num_mixed_channels;

	if( cl.background && cls.key_dest == key_console )
		return num_mixed_channels; // no sounds in console with background map

	for( int i = 0; i < snd.total_channels; i++ )
	{
		channel_t *ch = &snd.channels[i];

		if( !ch->sfx )
			continue;

		if( !cl.background )
		{
			if( cls.key_dest == key_console && FBitSet( ch->flags, FL_CHAN_LOCAL_SOUND ))
			{
				// play, playvol
			}
			else if(( cls.key_dest == key_menu || cl.paused ) && !FBitSet( ch->flags, FL_CHAN_LOCAL_SOUND ) && sp )
			{
				// play only local sounds, keep pause for other
				continue;
			}
			else if( cls.key_dest != key_menu && !ingame && !FBitSet( ch->flags, FL_CHAN_STATIC_SOUND ))
			{
				// play only ambient sounds, keep pause for other
				continue;
			}
		}

		if( !ch->sfx->cache && S_SoundPending( ch->sfx ))
			continue;

		wavdata_t *sc = S_LoadSound( ch->sfx );

		if( !sc )
		{
			S_FreeChannel( ch );
			continue;
		}

		// it was waited for
		if( ch->words && !ch->data )
		{
			VOX_LoadWord( ch );
			if( FBitSet( ch->flags, FL_CHAN_SENTENCE_FINISHED ))
			{
				S_FreeChannel( ch );
				continue;
			}
		}

		if( FBitSet( ch->flags, FL_CHAN_PENDING ))
		{
			ClearBits( ch->flags, FL_CHAN_PENDING );
			if( host.realtime > ch->inauduble_free_time && !( FBitSet( sc->flags, SOUND_LOOPED ) && FBitSet( ch->flags, FL_CHAN_USE_LOOP )))
			{
				S_FreeChannel( ch ); // too late
				continue;
			}
			ch->inauduble_free_time = 0.0f;
		}

		// if the sound is unaudible, skip it
		// if it's also not looping, free it
		if( ch->leftvol < 8 && ch->rightvol < 8 )
		{
			if( !FBitSet( sc->flags, SOUND_LOOPED ) || !FBitSet( ch->flags, FL_CHAN_USE_LOOP ))
			{
				if( ch->inauduble_free_time == 0.0f )
					ch->inauduble_free_time = host.realtime + MAX_CHANNEL_INAUDIBLE_TIME;
				else if( host.realtime > ch->inauduble_free_time )
					S_FreeChannel( ch );
			}

			continue;
		}

		ch->inauduble_free_time = 0.0f;

		if( ch->entchannel == CHAN_VOICE || ch->entchannel == CHAN_STREAM )
		{
			cl_entity_t *ent = CL_GetEntityByIndex( ch->entnum );

			if( ent != NULL )
			{
				int mouth_count = (int)( num_samples * (double)sc->rate / out_rate );

				if( sc->width == 1 )
					SND_MoveMouth8( &ent->mouth, ch->sample, sc, mouth_count, FBitSet( ch->flags, FL_CHAN_USE_LOOP ));
				else
					SND_MoveMouth16( &ent->mouth, ch->sample, sc, mouth_count, FBitSet( ch->flags, FL_CHAN_USE_LOOP ));
			}
		}

		s_pitch_t pitch = VOX_ModifyPitch( ch, ch->basePitch * 0.01f ) * pitch_mult;

		num_mixed_channels++;

		if( ch->words )
		{
			VOX_MixChannelToBuffer( dst, ch, num_samples, out_rate, pitch );

			if( FBitSet( ch->flags, FL_CHAN_SENTENCE_FINISHED ))
				S_FreeChannel( ch );
		}
		else
		{
#if XASH_PSP
			const int vol = bound( 0, Q_max( ch->leftvol, ch->rightvol ), 255 );
			s_mix_dry = vol < mix_volume || ( vol == mix_volume && mix_at_volume-- <= 0 );
			s_mix_defer = dst == roombuffer && s_mixme.value && PSP_MeStart();
#endif
			S_MixChannelToBuffer( dst, ch, num_samples, out_rate, pitch, 0, 0 );
#if XASH_PSP
			s_mix_dry = false;
			s_mix_defer = false;
#endif

			if( FBitSet( ch->flags, FL_CHAN_FINISHED ))
				S_FreeChannel( ch );
		}
	}

	return num_mixed_channels;
}

static int S_MixRawChannels( int end )
{
	int num_room_channels = 0;

	if( cl.paused )
		return 0;

	// paint in the raw channels
	for( size_t i = 0; i < snd.max_raw_channels; i++ )
	{
		// copy from the streaming sound source
		rawchan_t *ch = snd.raw_channels[i];

		if( !ch )
			continue;

		// not audible
		if( !ch->leftvol && !ch->rightvol )
			continue;

		qboolean is_voice = CL_IsPlayerIndex( ch->entnum )
			|| ch->entnum == VOICE_LOOPBACK_INDEX
			|| ch->entnum == VOICE_LOCALCLIENT_INDEX;

		portable_samplepair_t *pbuf;
		if( is_voice || ch->entnum == S_RAW_SOUND_BACKGROUNDTRACK )
		{
			// for streams we don't have fancy things like volume controls
			// or DSP processing or upsampling, so paint it directly into result buffer
			pbuf = paintbuffer;
		}
		else
		{
			pbuf = roombuffer;
			num_room_channels++;
		}

		uint stop = (end < ch->s_rawend) ? end : ch->s_rawend;
		const uint mask = ch->max_samples - 1;

		for( size_t i = 0, j = snd.paintedtime; j < stop; i++, j++ )
		{
			pbuf[i].left  += ( ch->rawsamples[j & mask].left * ch->leftvol ) >> 8;
			pbuf[i].right += ( ch->rawsamples[j & mask].right * ch->rightvol ) >> 8;
		}

		if( ch->entnum > 0 )
		{
			cl_entity_t *ent = CL_GetEntityByIndex( ch->entnum );
			int pos = snd.paintedtime & ( ch->max_samples - 1 );
			int count = bound( 0, ch->max_samples - pos, stop - snd.paintedtime );

			if( ent )
				SND_MoveMouthRaw( &ent->mouth, &ch->rawsamples[pos], count );
		}
	}

	return num_room_channels;
}

static void S_MixBufferWithGain( portable_samplepair_t *dst, const portable_samplepair_t *src, size_t count, int gain )
{
	if( gain == 256 )
	{
		for( size_t i = 0; i < count; i++ )
		{
			dst[i].left += src[i].left;
			dst[i].right += src[i].right;
		}
	}
	else
	{
		for( size_t i = 0; i < count; i++ )
		{
			dst[i].left += ( src[i].left * gain ) >> 8;
			dst[i].right += ( src[i].right * gain ) >> 8;
		}
	}
}

static void S_WriteLinearBlastStereo16( short *snd_out, const int *snd_p, size_t count )
{
	for( size_t i = 0; i < count; i += 2 )
	{
		snd_out[i+0] = CLIP16( snd_p[i+0] );
		snd_out[i+1] = CLIP16( snd_p[i+1] );
	}
}

static void S_TransferPaintBuffer( const portable_samplepair_t *src, int starttime, int endtime )
{
	const int *snd_p = (const int *)src;
	const int sampleMask = ((snd.samples >> 1) - 1);
	int lpaintedtime = starttime;

	SNDDMA_BeginPainting ();

	while( lpaintedtime < endtime )
	{
		// handle recirculating buffer issues
		int lpos = lpaintedtime & sampleMask;

		short *snd_out = (short *)snd.buffer + (lpos << 1);

		int snd_linear_count = (snd.samples>>1) - lpos;
		if( lpaintedtime + snd_linear_count > endtime )
			snd_linear_count = endtime - lpaintedtime;

		snd_linear_count <<= 1;

		// write a linear blast of samples
		S_WriteLinearBlastStereo16( snd_out, snd_p, snd_linear_count );

		snd_p += snd_linear_count;
		lpaintedtime += (snd_linear_count >> 1);
	}

	SNDDMA_Submit();
}

void S_ClearBuffers( int num_samples )
{
	const size_t num_bytes = ( num_samples + 1 ) * sizeof( portable_samplepair_t );

	memset( roombuffer, 0, num_bytes );
	memset( paintbuffer, 0, num_bytes );
}

#if XASH_PSP
// on the ME, without syscalls: the rest of S_PaintChunk
static void S_MeMixJob( void *arg )
{
	const s_mejob_t *job = arg;
	portable_samplepair_t *out = job->out;
	const int n = job->num_samples;

	memcpy( out, job->room_in, n * sizeof( portable_samplepair_t ));

	for( int i = 0; i < job->num_ops; i++ )
	{
		const s_mixop_t *op = &job->ops[i];

		S_MixAudio( out + op->offset, op->pvol, op->audio, op->channels, op->width, op->frac, op->step, op->count, op->lerp );
	}

	if( job->dsp )
		SX_RoomFXProcess( out, n );

	if( job->room_channels > 0 || job->dsp )
	{
		const int gain = job->gain;

		for( int i = 0; i < n; i++ )
		{
			out[i].left = job->paint_in[i].left + (( out[i].left * gain ) >> 8 );
			out[i].right = job->paint_in[i].right + (( out[i].right * gain ) >> 8 );
		}
	}
	else memcpy( out, job->paint_in, n * sizeof( portable_samplepair_t ));

	for( int i = 0; i < n; i++ )
	{
		job->out16[i * 2 + 0] = CLIP16( out[i].left );
		job->out16[i * 2 + 1] = CLIP16( out[i].right );
	}
}

// the ME's samples into the DMA buffer
static void S_TransferSamples16( const short *src, int starttime, int endtime )
{
	const int sampleMask = ((snd.samples >> 1) - 1);

	SNDDMA_BeginPainting ();

	while( starttime < endtime )
	{
		const int lpos = starttime & sampleMask;
		int count = (snd.samples >> 1) - lpos;

		if( starttime + count > endtime )
			count = endtime - starttime;

		memcpy( (short *)snd.buffer + ( lpos << 1 ), src, count * 2 * sizeof( short ));
		src += count * 2;
		starttime += count;
	}

	SNDDMA_Submit();
}
#endif

// the room buffer is mixed: DSP, into the paint buffer and out to the DMA buffer
static void S_PaintChunk( int start, int end, int room_channels, int gain, qboolean dsp )
{
	const int num_samples = end - start;

	// now process DSP and mix result into paintbuffer
	if( dsp )
		SX_RoomFX( roombuffer, num_samples );

	if( room_channels > 0 || dsp )
		S_MixBufferWithGain( paintbuffer, roombuffer, num_samples, gain );

	// transfer out according to DMA format
	S_TransferPaintBuffer( paintbuffer, start, end );
}

#if XASH_PSP
/*
S_PaintSync: the ME no longer reads sample data (before a sound is freed)
S_PaintFinish: the chunk the ME mixed goes out
S_PaintDiscard: or it's dropped (the DMA buffer is cleared)
*/
void S_PaintSync( void )
{
	PSP_MeWaitFor( s_me_pending.ticket );
}

static void S_PaintFinish( void )
{
	if( !s_me_pending.active )
		return;

	s_me_pending.active = false;
	PSP_MeWaitFor( s_me_pending.ticket );
	sceKernelDcacheInvalidateRange( s_me.out16, sizeof( s_me.out16 ));
	S_TransferSamples16( s_me.out16, s_me_pending.start, s_me_pending.end );
}

// the ME isn't done, and what's in the DMA buffer lasts for a while
qboolean S_PaintBusy( void )
{
	return s_me_pending.active && !PSP_MeDone( s_me_pending.ticket )
		&& (int)( s_me_pending.start - snd.soundtime ) > SOUND_DMA_SPEED / 20;
}

void S_PaintDiscard( void )
{
	PSP_MeWaitFor( s_me_pending.ticket );
	s_me_pending.active = false;
}
#endif

void S_PaintChannels( int endtime )
{
	int gain = S_GetMasterVolume() * 256;

#if XASH_PSP
	S_PaintFinish();
#endif

	while( snd.paintedtime < endtime )
	{
		// if paintbuffer is smaller than DMA buffer
		int end = endtime;
		if( end - snd.paintedtime > PAINTBUFFER_SIZE )
			end = snd.paintedtime + PAINTBUFFER_SIZE;

		const int num_samples = end - snd.paintedtime;

		S_ClearBuffers( num_samples );

		int room_channels;
#if XASH_PSP
		s_me_numops = 0;
#endif
		room_channels = S_MixNormalChannelsToRoombuffer( roombuffer, end );

		room_channels += S_MixRawChannels( end );

#if XASH_PSP
		if( s_mixme.value && PSP_MeStart( ))
		{
			s_me.job.num_ops = s_me_numops;
			s_me.job.num_samples = num_samples;
			s_me.job.ops = s_me.ops;
			s_me.job.out = s_me.room;
			s_me.job.room_in = roombuffer;
			s_me.job.paint_in = paintbuffer;
			s_me.job.room_channels = room_channels;
			s_me.job.gain = gain;
			s_me.job.dsp = cls.key_dest != key_menu && SX_RoomFXPrepare();
			s_me.job.out16 = s_me.out16;
			s_me_pending.ticket = PSP_MeRun( S_MeMixJob, &s_me.job );

			s_me_pending.active = true;
			s_me_pending.start = snd.paintedtime;
			s_me_pending.end = end;
			snd.paintedtime = end;

			// more to paint: now, if the next update may come too late
			if( snd.paintedtime < endtime )
			{
				if( S_PaintBusy( ))
					return;
				S_PaintFinish();
			}
			continue;
		}
#endif

		S_PaintChunk( snd.paintedtime, end, room_channels, gain, cls.key_dest != key_menu );
		snd.paintedtime = end;
	}
}
