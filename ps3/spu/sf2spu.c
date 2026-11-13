/*
 * Copyright (C) 2026 the EDuke32-PS3 contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 *
 * TinySoundFont (tsf.h) by Bernhard Schelling, MIT license: the voice
 * rendering below is tsf_voice_render and its helpers, with the voice's
 * region carried in the voice and the font's samples read through a
 * cache in local store.
 *
 * =======================================================================
 *
 * The SoundFont synth's voices on an SPU (sf2spu_wire.h has the protocol).
 *
 * One SPU thread, started once, waits on signal notification 1. Every
 * signal is a job: the job block (its EA came as the thread's argument),
 * the voices, the samples they read; the voices render into a buffer in
 * local store, the buffer and the voices go back, and a user event on
 * SF2SPU_PORT wakes the PPU's audio thread.
 *
 * The font's samples (floats, in main memory) go through a direct-mapped
 * cache of 128 byte lines; before rendering, the lines each voice is
 * going to read in this chunk are asked for all at once, so a job waits
 * for main memory about once, not once per line.
 *
 * =======================================================================
 */

#include <stdint.h>
#include <string.h>
#include <math.h>
#include <spu_intrinsics.h>
#include <spu_mfcio.h>

#include "sf2spu_wire.h"

/* tsf.h's names, so the code below stays tsf's */
#define TSF_BOOL char
#define TSF_TRUE 1
#define TSF_FALSE 0
#define TSF_FASTRELEASETIME 0.01f
#define TSF_RENDER_EFFECTSAMPLEBLOCK effect_block
#define TSF_PI 3.14159265358979323846264338327950288
#define TSF_POW  pow
#define TSF_POWF powf
#define TSF_EXPF expf
#define TSF_LOG  log
#define TSF_TAN  tan

enum { TSF_SEGMENT_NONE, TSF_SEGMENT_DELAY, TSF_SEGMENT_ATTACK, TSF_SEGMENT_HOLD, TSF_SEGMENT_DECAY, TSF_SEGMENT_SUSTAIN, TSF_SEGMENT_RELEASE, TSF_SEGMENT_DONE };
enum { TSF_INTERP_NEAREST, TSF_INTERP_LINEAR, TSF_INTERP_CUBIC };

#define tsf_voice_envelope sf2w_voice_envelope
#define tsf_voice_lowpass  sf2w_voice_lowpass
#define tsf_voice_lfo      sf2w_voice_lfo

#define TAG_IO   1
#define TAG_PRE  2
#define TAG_SYNC 3

/* ------------------------------------------------------------------------
   The sample cache
   ------------------------------------------------------------------------ */

#define LINE_FLOATS 32                  /* 128 bytes */
#define CACHE_LINES 1024                /* 128 KB */
#define PREFETCH_MAX_LINES 48           /* per voice and range */

static float    cache[CACHE_LINES][LINE_FLOATS] __attribute__((aligned(128)));
static uint32_t cache_tag[CACHE_LINES];     /* EA >> 7 of the line, or ~0 */
static uint32_t cache_round[CACHE_LINES];   /* asked for in this job's prefetch */
static uint32_t round_no;
static uint64_t font_ea;
static uint32_t misses;
static uint32_t waits;
static int      effect_block = 64;

static void cache_flush(void)
{
    for (int i = 0; i < CACHE_LINES; i++)
        cache_tag[i] = ~0u, cache_round[i] = 0;
}

static void __attribute__((noinline)) cache_fetch(uint32_t slot, uint32_t line)
{
    mfc_get(cache[slot], (uint64_t)line << 7, 128, TAG_SYNC, 0, 0);
    mfc_write_tag_mask(1 << TAG_SYNC);
    mfc_read_tag_status_all();
    cache_tag[slot] = line;
    misses++;
    waits++;
}

static inline float S(uint32_t idx)
{
    uint64_t const ea = font_ea + ((uint64_t)idx << 2);
    uint32_t const line = (uint32_t)(ea >> 7);
    uint32_t const slot = line & (CACHE_LINES - 1);

    if (__builtin_expect(cache_tag[slot] != line, 0))
        cache_fetch(slot, line);

    return cache[slot][((uint32_t)ea & 127) >> 2];
}

/* asks for the lines of samples first..last (no waiting) */
static void prefetch(uint32_t first, uint32_t last)
{
    if (last < first)
        return;

    uint32_t line = (uint32_t)((font_ea + ((uint64_t)first << 2)) >> 7);
    uint32_t const end = (uint32_t)((font_ea + ((uint64_t)last << 2)) >> 7);
    int budget = PREFETCH_MAX_LINES;

    for (; line <= end && budget > 0; line++, budget--)
    {
        uint32_t const slot = line & (CACHE_LINES - 1);

        /* there already, or this job asked for another line here: the
           second one comes on demand, after the prefetch has landed */
        if (cache_tag[slot] == line || cache_round[slot] == round_no)
            continue;

        mfc_get(cache[slot], (uint64_t)line << 7, 128, TAG_PRE, 0, 0);
        cache_tag[slot] = line;
        cache_round[slot] = round_no;
        misses++;
    }
}

/* ------------------------------------------------------------------------
   tsf's voice rendering
   ------------------------------------------------------------------------ */

static double tsf_timecents2Secsd(double timecents) { return TSF_POW(2.0, timecents / 1200.0); }
static float tsf_cents2Hertz(float cents) { return 8.176f * TSF_POWF(2.0f, cents / 1200.0f); }
static float tsf_decibelsToGain(float db) { return (db > -100.f ? TSF_POWF(10.0f, db * 0.05f) : 0); }

static int tsf_voice_envelope_release_samples(struct tsf_voice_envelope* e, float outSampleRate)
{
	return (int)((e->parameters.release <= 0 ? TSF_FASTRELEASETIME : e->parameters.release) * outSampleRate);
}

static void tsf_voice_envelope_nextsegment(struct tsf_voice_envelope* e, short active_segment, float outSampleRate)
{
	switch (active_segment)
	{
		case TSF_SEGMENT_NONE:
			e->samplesUntilNextSegment = (int)(e->parameters.delay * outSampleRate);
			if (e->samplesUntilNextSegment > 0)
			{
				e->segment = TSF_SEGMENT_DELAY;
				e->segmentIsExponential = TSF_FALSE;
				e->level = 0.0;
				e->slope = 0.0;
				return;
			}
			/* fall through */
		case TSF_SEGMENT_DELAY:
			e->samplesUntilNextSegment = (int)(e->parameters.attack * outSampleRate);
			if (e->samplesUntilNextSegment > 0)
			{
				if (!e->isAmpEnv)
				{
					//mod env attack duration scales with velocity (velocity of 1 is full duration, max velocity is 0.125 times duration)
					e->samplesUntilNextSegment = (int)(e->parameters.attack * ((145 - e->midiVelocity) / 144.0f) * outSampleRate);
				}
				e->segment = TSF_SEGMENT_ATTACK;
				e->segmentIsExponential = TSF_FALSE;
				e->level = 0.0f;
				e->slope = 1.0f / e->samplesUntilNextSegment;
				return;
			}
			/* fall through */
		case TSF_SEGMENT_ATTACK:
			e->samplesUntilNextSegment = (int)(e->parameters.hold * outSampleRate);
			if (e->samplesUntilNextSegment > 0)
			{
				e->segment = TSF_SEGMENT_HOLD;
				e->segmentIsExponential = TSF_FALSE;
				e->level = 1.0f;
				e->slope = 0.0f;
				return;
			}
			/* fall through */
		case TSF_SEGMENT_HOLD:
			e->samplesUntilNextSegment = (int)(e->parameters.decay * outSampleRate);
			if (e->samplesUntilNextSegment > 0)
			{
				e->segment = TSF_SEGMENT_DECAY;
				e->level = 1.0f;
				if (e->isAmpEnv)
				{
					// I don't truly understand this; just following what LinuxSampler does.
					float mysterySlope = -9.226f / e->samplesUntilNextSegment;
					e->slope = TSF_EXPF(mysterySlope);
					e->segmentIsExponential = TSF_TRUE;
					if (e->parameters.sustain > 0.0f)
					{
						// Again, this is following LinuxSampler's example, which is similar to
						// SF2-style decay, where "decay" specifies the time it would take to
						// get to zero, not to the sustain level.  The SFZ spec is not that
						// specific about what "decay" means, so perhaps it's really supposed
						// to specify the time to reach the sustain level.
						e->samplesUntilNextSegment = (int)(TSF_LOG(e->parameters.sustain) / mysterySlope);
					}
				}
				else
				{
					e->slope = -1.0f / e->samplesUntilNextSegment;
					e->samplesUntilNextSegment = (int)(e->parameters.decay * (1.0f - e->parameters.sustain) * outSampleRate);
					e->segmentIsExponential = TSF_FALSE;
				}
				return;
			}
			/* fall through */
		case TSF_SEGMENT_DECAY:
			e->segment = TSF_SEGMENT_SUSTAIN;
			e->level = e->parameters.sustain;
			e->slope = 0.0f;
			e->samplesUntilNextSegment = 0x7FFFFFFF;
			e->segmentIsExponential = TSF_FALSE;
			return;
		case TSF_SEGMENT_SUSTAIN:
			e->segment = TSF_SEGMENT_RELEASE;
			e->samplesUntilNextSegment = tsf_voice_envelope_release_samples(e, outSampleRate);
			if (e->isAmpEnv)
			{
				// I don't truly understand this; just following what LinuxSampler does.
				float mysterySlope = -9.226f / e->samplesUntilNextSegment;
				e->slope = TSF_EXPF(mysterySlope);
				e->segmentIsExponential = TSF_TRUE;
			}
			else
			{
				e->slope = -e->level / e->samplesUntilNextSegment;
				e->segmentIsExponential = TSF_FALSE;
			}
			return;
		case TSF_SEGMENT_RELEASE:
		default:
			e->segment = TSF_SEGMENT_DONE;
			e->segmentIsExponential = TSF_FALSE;
			e->level = e->slope = 0.0f;
			e->samplesUntilNextSegment = 0x7FFFFFF;
	}
}


static void tsf_voice_envelope_process(struct tsf_voice_envelope* e, int numSamples, float outSampleRate)
{
	if (e->slope)
	{
		if (e->segmentIsExponential) e->level *= TSF_POWF(e->slope, (float)numSamples);
		else e->level += (e->slope * numSamples);
	}
	if ((e->samplesUntilNextSegment -= numSamples) <= 0)
		tsf_voice_envelope_nextsegment(e, e->segment, outSampleRate);
}

static void tsf_voice_lowpass_setup(struct tsf_voice_lowpass* e, float Fc)
{
	// Lowpass filter from http://www.earlevel.com/main/2012/11/26/biquad-c-source-code/
	double K = TSF_TAN(TSF_PI * Fc), KK = K * K;
	double norm = 1 / (1 + K * e->QInv + KK);
	e->a0 = KK * norm;
	e->a1 = 2 * e->a0;
	e->b1 = 2 * (KK - 1) * norm;
	e->b2 = (1 - K * e->QInv + KK) * norm;
}

static inline float tsf_voice_lowpass_process(struct tsf_voice_lowpass* e, double In)
{
	double Out = In * e->a0 + e->z1; e->z1 = In * e->a1 + e->z2 - e->b1 * Out; e->z2 = In * e->a0 - e->b2 * Out; return (float)Out;
}

static void tsf_voice_lfo_process(struct tsf_voice_lfo* e, int blockSamples)
{
	if (e->samplesUntil > blockSamples) { e->samplesUntil -= blockSamples; return; }
	e->level += e->delta * blockSamples;
	if      (e->level >  1.0f) { e->delta = -e->delta; e->level =  2.0f - e->level; }
	else if (e->level < -1.0f) { e->delta = -e->delta; e->level = -2.0f - e->level; }
}

static inline float tsf_voice_interpolate(unsigned int *pos, float alpha, int interpolatemode)
{
	switch (interpolatemode)
	{
		case TSF_INTERP_LINEAR:
		{
			float const a = S(pos[1]), b = S(pos[2]);
			return a + (b - a) * alpha;
		}
		case TSF_INTERP_CUBIC:
		{
			const float pointa = S(pos[0]), pointb = S(pos[1]), pointc = S(pos[2]), pointd = S(pos[3]);
			float a0, a1, a2, a3, alpha2;

			alpha2 = alpha * alpha;
			a0 = -0.5f * pointa + 1.5f * pointb - 1.5f * pointc + 0.5f * pointd;
			a1 = pointa - 2.5f * pointb + 2.f * pointc - 0.5f * pointd;
			a2 = -0.5f * pointa + 0.5f * pointc;
			a3 = pointb;
			return (a0 * alpha * alpha2 + a1 * alpha2 + a2 * alpha + a3);
		}
		case TSF_INTERP_NEAREST:
		default:
			return S(pos[1]);
	}
}

/* tsf_voice_render for TSF_STEREO_INTERLEAVED (stereo) and TSF_MONO */
static void render_voice(sf2spu_voice* v, float* outputBuffer, int numSamples, int stereo, int interpolatemode, float outSampleRate)
{
	float* outL = outputBuffer;

	TSF_BOOL updateModEnv = (v->modEnvToPitch || v->modEnvToFilterFc);
	TSF_BOOL updateModLFO = (v->modlfo.delta && (v->modLfoToPitch || v->modLfoToFilterFc || v->modLfoToVolume));
	TSF_BOOL updateVibLFO = (v->viblfo.delta && (v->vibLfoToPitch));
	TSF_BOOL isLooping    = (v->loopStart < v->loopEnd);
	unsigned int tmpLoopStart = v->loopStart, tmpLoopEnd = v->loopEnd, regionEnd = v->regionEnd, pos[4] = {0};
	double tmpSampleEndDbl = (double)regionEnd, tmpLoopEndDbl = (double)tmpLoopEnd + 1.0;
	double tmpSourceSamplePosition = v->sourceSamplePosition;
	float alpha           = 0;
	struct tsf_voice_lowpass tmpLowpass = v->lowpass;

	TSF_BOOL dynamicLowpass = (v->modLfoToFilterFc || v->modEnvToFilterFc);
	float tmpSampleRate = outSampleRate, tmpInitialFilterFc, tmpModLfoToFilterFc, tmpModEnvToFilterFc;

	TSF_BOOL dynamicPitchRatio = (v->modLfoToPitch || v->modEnvToPitch || v->vibLfoToPitch);
	double pitchRatio;
	float tmpModLfoToPitch, tmpVibLfoToPitch, tmpModEnvToPitch;

	TSF_BOOL dynamicGain = (v->modLfoToVolume != 0);
	float noteGain = 0, tmpModLfoToVolume;

	if (dynamicLowpass) tmpInitialFilterFc = (float)v->initialFilterFc, tmpModLfoToFilterFc = (float)v->modLfoToFilterFc, tmpModEnvToFilterFc = (float)v->modEnvToFilterFc;
	else tmpInitialFilterFc = 0, tmpModLfoToFilterFc = 0, tmpModEnvToFilterFc = 0;

	if (dynamicPitchRatio) pitchRatio = 0, tmpModLfoToPitch = (float)v->modLfoToPitch, tmpVibLfoToPitch = (float)v->vibLfoToPitch, tmpModEnvToPitch = (float)v->modEnvToPitch;
	else pitchRatio = tsf_timecents2Secsd(v->pitchInputTimecents) * v->pitchOutputFactor, tmpModLfoToPitch = 0, tmpVibLfoToPitch = 0, tmpModEnvToPitch = 0;

	if (dynamicGain) tmpModLfoToVolume = (float)v->modLfoToVolume * 0.1f;
	else noteGain = tsf_decibelsToGain(v->noteGainDB), tmpModLfoToVolume = 0;

	while (numSamples)
	{
		float gainMono, gainLeft, gainRight;
		int blockSamples = (numSamples > TSF_RENDER_EFFECTSAMPLEBLOCK ? TSF_RENDER_EFFECTSAMPLEBLOCK : numSamples);
		numSamples -= blockSamples;

		if (dynamicLowpass)
		{
			float fres = tmpInitialFilterFc + v->modlfo.level * tmpModLfoToFilterFc + v->modenv.level * tmpModEnvToFilterFc;
			float lowpassFc = (fres <= 13500 ? tsf_cents2Hertz(fres) / tmpSampleRate : 1.0f);
			tmpLowpass.active = (lowpassFc < 0.499f);
			if (tmpLowpass.active) tsf_voice_lowpass_setup(&tmpLowpass, lowpassFc);
		}

		if (dynamicPitchRatio)
			pitchRatio = tsf_timecents2Secsd(v->pitchInputTimecents + (v->modlfo.level * tmpModLfoToPitch + v->viblfo.level * tmpVibLfoToPitch + v->modenv.level * tmpModEnvToPitch)) * v->pitchOutputFactor;

		if (dynamicGain)
			noteGain = tsf_decibelsToGain(v->noteGainDB + (v->modlfo.level * tmpModLfoToVolume));

		gainMono = noteGain * v->ampenv.level;

		// Update EG.
		tsf_voice_envelope_process(&v->ampenv, blockSamples, tmpSampleRate);
		if (updateModEnv) tsf_voice_envelope_process(&v->modenv, blockSamples, tmpSampleRate);

		// Update LFOs.
		if (updateModLFO) tsf_voice_lfo_process(&v->modlfo, blockSamples);
		if (updateVibLFO) tsf_voice_lfo_process(&v->viblfo, blockSamples);

		gainLeft = gainMono * v->panFactorLeft, gainRight = gainMono * v->panFactorRight;

		while (blockSamples-- && tmpSourceSamplePosition < tmpSampleEndDbl)
		{
			// Get samples.
			pos[1] = (unsigned int)tmpSourceSamplePosition;
			if (interpolatemode != TSF_INTERP_NEAREST)
			{
				alpha = (float)(tmpSourceSamplePosition - pos[1]);
				pos[2] = isLooping ? (pos[1] >= tmpLoopEnd ? tmpLoopStart : pos[1] + 1) : (pos[1] + 1 == regionEnd ? pos[1] : pos[1] + 1);
				if (interpolatemode == TSF_INTERP_CUBIC)
					pos[0] = (pos[1] == tmpLoopStart && isLooping ? tmpLoopEnd : pos[1] == 0 ? 0 : pos[1] - 1), pos[3] = isLooping ? (pos[2] >= tmpLoopEnd ? tmpLoopStart : pos[2] + 1) : (pos[2] + 1 == regionEnd ? pos[2] : pos[2] + 1);
			}

			// Interpolation.
			float val = tsf_voice_interpolate(pos, alpha, interpolatemode);

			// Low-pass filter.
			if (tmpLowpass.active) val = tsf_voice_lowpass_process(&tmpLowpass, val);

			if (stereo)
			{
				*outL++ += val * gainLeft;
				*outL++ += val * gainRight;
			}
			else
				*outL++ += val * gainMono;

			// Next sample.
			tmpSourceSamplePosition += pitchRatio;
			if (tmpSourceSamplePosition >= tmpLoopEndDbl && isLooping) tmpSourceSamplePosition -= (tmpLoopEnd - tmpLoopStart + 1.0);
		}

		if (tmpSourceSamplePosition >= tmpSampleEndDbl || v->ampenv.segment == TSF_SEGMENT_DONE)
		{
			v->ended = 1;           /* tsf_voice_kill */
			return;
		}
	}

	v->sourceSamplePosition = tmpSourceSamplePosition;
	if (tmpLowpass.active || dynamicLowpass) v->lowpass = tmpLowpass;
}

/* the lines a voice reads in the next numSamples, about */
static void prefetch_voice(sf2spu_voice const* v, int numSamples)
{
	double ratio = tsf_timecents2Secsd(v->pitchInputTimecents) * v->pitchOutputFactor;
	double pos = v->sourceSamplePosition;

	if (v->modLfoToPitch || v->modEnvToPitch || v->vibLfoToPitch)
		ratio *= 1.25;          /* some room for the modulation */

	double span = ratio * numSamples + 3.0;
	if (span > (double)(PREFETCH_MAX_LINES * LINE_FLOATS))
		span = (double)(PREFETCH_MAX_LINES * LINE_FLOATS);

	if (pos < 0.0 || pos >= (double)v->regionEnd)
		return;

	uint32_t const p0 = (uint32_t)pos;
	uint32_t const p1 = p0 + (uint32_t)span;

	if (v->loopStart < v->loopEnd && p1 > v->loopEnd && p0 <= v->loopEnd)
	{
		uint32_t const wrap = p1 - v->loopEnd;
		uint32_t last = v->loopStart + wrap;

		if (last > v->loopEnd)
			last = v->loopEnd;

		prefetch(p0 > 0 ? p0 - 1 : 0, v->loopEnd);
		prefetch(v->loopStart, last);
	}
	else
	{
		uint32_t last = p1;

		if (v->regionEnd && last >= v->regionEnd)
			last = v->regionEnd - 1;

		prefetch(p0 > 0 ? p0 - 1 : 0, last);
	}
}

/* ------------------------------------------------------------------------
   The job loop
   ------------------------------------------------------------------------ */

_Static_assert(__builtin_offsetof(sf2spu_job, done) % 16 == 0, "job.done must be 16-aligned");
_Static_assert(sizeof(sf2spu_voice) % 16 == 0, "voices must be 16 byte multiples");

static sf2spu_job   job __attribute__((aligned(128)));
static sf2spu_voice voices[SF2SPU_MAX_VOICES] __attribute__((aligned(128)));
static float        outbuf[SF2SPU_MAX_SAMPLES * 2 + 32] __attribute__((aligned(128)));

/* mfc_get/put of any size (16 byte multiples, 16 KB at a time) */
static void dma_large(void *ls, uint64_t ea, uint32_t size, int put)
{
	char *p = (char *)ls;

	while (size)
	{
		uint32_t const n = size > 16384 ? 16384 : size;

		if (put)
			mfc_put(p, ea, n, TAG_IO, 0, 0);
		else
			mfc_get(p, ea, n, TAG_IO, 0, 0);

		p += n, ea += n, size -= n;
	}
}

static void wait_tag(int tag)
{
	mfc_write_tag_mask(1 << tag);
	mfc_read_tag_status_all();
}

/* sys_spu_thread_send_event (libsputhread), here so the SPU side needs
   nothing but spu-gcc */
static void send_event(uint32_t data0, uint32_t data1)
{
	spu_writech(SPU_WrOutMbox, data1);
	spu_writech(SPU_WrOutIntrMbox, ((uint32_t)SF2SPU_PORT << 24) | (data0 & 0x00FFFFFFu));
	(void)spu_readch(SPU_RdInMbox);
}

int main(uint64_t job_ea, uint64_t arg2, uint64_t arg3, uint64_t arg4)
{
	uint32_t seq = 0;

	(void)arg2, (void)arg3, (void)arg4;

	cache_flush();
	spu_writech(SPU_WrDec, 0xFFFFFFFFu);

	for (;;)
	{
		uint32_t const signal = spu_readch(SPU_RdSigNotify1);

		if (signal & SF2SPU_SIGNAL_QUIT)
			break;

		uint32_t const t0 = spu_readch(SPU_RdDec);

		mfc_get(&job, job_ea, sizeof(job), TAG_IO, 0, 0);
		wait_tag(TAG_IO);

		uint32_t nv = job.numVoices;
		int n = (int)job.numSamples;
		int const stereo = job.stereo ? 1 : 0;

		if (nv > SF2SPU_MAX_VOICES) nv = SF2SPU_MAX_VOICES;
		if (n > SF2SPU_MAX_SAMPLES) n = SF2SPU_MAX_SAMPLES;
		if (n < 0) n = 0;

		if (job.flush || font_ea != job.fontSamples)
			cache_flush();

		font_ea = job.fontSamples;
		effect_block = (int)job.effectBlock;
		if (effect_block < 1 || effect_block > SF2SPU_MAX_SAMPLES)
			effect_block = 64;
		misses = 0;
		waits = 0;
		if (++round_no == 0)
			round_no = 1;

		dma_large(voices, job.voices, nv * (uint32_t)sizeof(sf2spu_voice), 0);

		int const outfloats = n << stereo;
		uint32_t const outbytes = ((uint32_t)outfloats * 4 + 15) & ~15u;
		memset(outbuf, 0, outbytes);

		wait_tag(TAG_IO);

		for (uint32_t i = 0; i < nv; i++)
			prefetch_voice(&voices[i], n);

		wait_tag(TAG_PRE);

		for (uint32_t i = 0; i < nv; i++)
			render_voice(&voices[i], outbuf, n, stereo, (int)job.interp, job.outSampleRate);

		dma_large(voices, job.voices, nv * (uint32_t)sizeof(sf2spu_voice), 1);
		if (outbytes)
			dma_large(outbuf, job.out, outbytes, 1);

		job.done = ++seq;
		job.misses = misses;
		job.ticks = t0 - spu_readch(SPU_RdDec);
		job.waits = waits;

		/* done, misses, ticks, waits: 16 bytes, 16-aligned on both sides */
		mfc_put(&job.done, job_ea + __builtin_offsetof(sf2spu_job, done), 16, TAG_IO, 0, 0);
		wait_tag(TAG_IO);

		send_event(seq, misses);
	}

	spu_writech(SPU_WrOutMbox, 0);
	spu_stop(0x102);            /* sys_spu_thread_exit */
	return 0;
}
