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

static portable_samplepair_t roombuffer[(PAINTBUFFER_SIZE+1)], paintbuffer[(PAINTBUFFER_SIZE+1)];

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
		S_FIXED_STEP( offset_frac, rate_scale ); \
		for( int i = 0; i < num_samples; i++ ) \
		{ \
			pbuf[i].left  += ( data[sample_idx] * volume[0] ) >> ( x - 8 ); \
			pbuf[i].right += ( data[sample_idx] * volume[1] ) >> ( x - 8 ); \
			S_FIXED_ADVANCE( 0 ); \
		} \
	} \

#define S_MakeMixStereoPitch( x ) \
	static void S_MixStereoPitch ## x( portable_samplepair_t *pbuf, const int *volume, const void *buf, uint32_t offset_frac, s_fixstep_t rate_scale, int num_samples ) \
	{ \
		const int##x##_t *data = buf; \
		S_FIXED_STEP( offset_frac, rate_scale ); \
		for( int i = 0; i < num_samples; i++ ) \
		{ \
			pbuf[i].left  += ( data[sample_idx+0] * volume[0] ) >> ( x - 8 ); \
			pbuf[i].right += ( data[sample_idx+1] * volume[1] ) >> ( x - 8 ); \
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
the same as the generic version below, but the source position is stepped in 32.32
fixed point: with no double precision FPU, its double math cost ~30 us per channel
and paint call (~60 software calls), a third of all mixing in a firefight.
chan->sample stays a double for the rest of the engine, converted once each way
*/
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

	uint32_t pos = (uint32_t)chan->sample;
	uint32_t frac = (uint32_t)(( chan->sample - (double)pos ) * 4294967296.0 );

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

		S_MixAudio( pbuf + offset, pvol, audio, wav->channels, wav->width, frac, read_step, out_count, lerp );

		S_FixedAdvance( &pos, &frac, pos_step, out_count );
		offset += out_count;
		num_samples -= out_count;
	}

	chan->sample = (double)pos + (double)frac * ( 1.0 / 4294967296.0 );

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

		wavdata_t *sc = S_LoadSound( ch->sfx );

		if( !sc )
		{
			S_FreeChannel( ch );
			continue;
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
			S_MixChannelToBuffer( dst, ch, num_samples, out_rate, pitch, 0, 0 );

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

static void S_TransferPaintBuffer( const portable_samplepair_t *src, int endtime )
{
	const int *snd_p = (const int *)src;
	const int sampleMask = ((snd.samples >> 1) - 1);
	int lpaintedtime = snd.paintedtime;

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

void S_PaintChannels( int endtime )
{
	int gain = S_GetMasterVolume() * 256;

	while( snd.paintedtime < endtime )
	{
		// if paintbuffer is smaller than DMA buffer
		int end = endtime;
		if( end - snd.paintedtime > PAINTBUFFER_SIZE )
			end = snd.paintedtime + PAINTBUFFER_SIZE;

		const int num_samples = end - snd.paintedtime;

		S_ClearBuffers( num_samples );

		int room_channels = S_MixNormalChannelsToRoombuffer( roombuffer, end );

		room_channels += S_MixRawChannels( end );

		// now process DSP and mix result into paintbuffer
		if( cls.key_dest != key_menu )
			SX_RoomFX( roombuffer, num_samples );

		if( room_channels > 0 || cls.key_dest != key_menu )
			S_MixBufferWithGain( paintbuffer, roombuffer, num_samples, gain );

		// transfer out according to DMA format
		S_TransferPaintBuffer( paintbuffer, end );
		snd.paintedtime = end;
	}
}
