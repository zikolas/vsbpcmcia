/* EMU10K2 hardware wavetable -- see emu_wt.h. */

#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "config.h"

#ifdef CARD_AUDIGY

#include "mpxplay.h"
#include "dmabuff.h"
#include "pcibios.h"
#include "physmem.h"
#include "timer.h"
#include "emu10k1.h"
#include "sc_sbliv.h"
#include "linear.h"     /* NearPtr for the video-memory trace */
#include "emu_sf2.h"
#include "emu_wt.h"

/* stdio.h is deliberately not included: src/fileacc.asm defines fopen/fread/
 * fseek/fclose with its own signatures and those objects win the link, so the
 * CRT's prototypes would be a lie here (src/tsf.c does the same). */
int printf(const char *, ...);

/* raw file access from src/fileacc.asm -- these deliberately shadow the CRT's
 * buffered versions (same trick src/tsf.c uses). fseek mode 0 = from start,
 * fread returns bytes transferred. */
void *fopen(const char *, const char *);
void *fclose(void *);
int   fseek(void *, int, int);
int   fread(void *, int, int, void *);

/* ---- provided by sc_sbliv.c, where the originals are static -------------- */
extern void     EMU_WritePtr(struct emu10k1_card *c, uint32_t reg, uint32_t chn,
                             uint32_t data);
extern uint32_t EMU_ReadPtr(struct emu10k1_card *c, uint32_t reg, uint32_t chn);
extern uint32_t EMU_SrToPitch(uint32_t rate);
extern uint32_t EMU_CalcPitchTarget(uint32_t rate);
extern uint32_t EMU_SelectInterprom(struct emu10k1_card *c, uint32_t target);
extern const uint32_t emuwt_maxpages;
extern int emuwt_noio;

#define WT_FIRST_VOICE  3
#define WT_LAST_VOICE   31      /* 3..31 proven. Expanding to 63 wedges the
                                 * machine at AU_start even with CLIPH being
                                 * serviced -- likely the doubled parking-loop
                                 * service rate on the shared fetch engine.
                                 * Revisit as its own experiment, not inline
                                 * with the steal fixes. */
#define WT_NVOICES      (WT_LAST_VOICE - WT_FIRST_VOICE + 1)
#define WT_BASE_PAGE    16      /* clear of the PCM buffer (8 pages) + a guard */
#define WT_SAMPLE_BASE  (((uint32_t)WT_BASE_PAGE * EMUPAGESIZE) / 2)
#define WT_MAXZONES     8       /* layers stacked on one note */
#define WT_CHANNELS     16
#define WT_READCHUNK    32768u

/* Polyphony headroom. Voices sum on the FX bus, so a level that is right for
 * one note clips as soon as four arrive together -- and a sustained organ
 * chord is the worst case, being steady and nearly phase-coherent. 12 dB
 * leaves room for roughly eight simultaneous notes before the sum runs out of
 * scale, which is the usual compromise: less and loud passages clip, more and
 * solo notes are feeble. AUDWTGAIN trims it either way without a rebuild. */
#define WT_HEADROOM_CB  120

/* ---------------------------------------------------------------- tables */

/* 2^(n/12) in 16.16 fixed point, n = 0..11 */
static const uint32_t wt_semi_ratio[12] = {
	 65536,  69433,  73562,  77936,  82570,  87480,
	 92682,  98193, 104032, 110218, 116772, 123715,
};

/* 2^(c/1200) in 16.16 fixed point, c = 0..99 cents */
static const uint32_t wt_cent_ratio[100] = {
	 65536,  65574,  65612,  65650,  65688,  65726,  65764,  65802,  65840,  65878,
	 65916,  65954,  65992,  66030,  66068,  66106,  66144,  66183,  66221,  66259,
	 66297,  66336,  66374,  66412,  66451,  66489,  66528,  66566,  66605,  66643,
	 66682,  66720,  66759,  66797,  66836,  66874,  66913,  66952,  66990,  67029,
	 67068,  67107,  67145,  67184,  67223,  67262,  67301,  67340,  67378,  67417,
	 67456,  67495,  67534,  67573,  67612,  67651,  67691,  67730,  67769,  67808,
	 67847,  67886,  67926,  67965,  68004,  68043,  68083,  68122,  68161,  68201,
	 68240,  68280,  68319,  68359,  68398,  68438,  68477,  68517,  68556,  68596,
	 68635,  68675,  68715,  68755,  68794,  68834,  68874,  68914,  68953,  68993,
	 69033,  69073,  69113,  69153,  69193,  69233,  69273,  69313,  69353,  69393,
};

/* attenuation in centibels for a MIDI velocity: -400*log10(vel/127), i.e. a
 * square gain law, which is what a GM synth is expected to sound like */
static const uint16_t wt_vel_atten_cb[128] = {
	 960,  842,  721,  651,  601,  562,  530,  503,
	 480,  460,  442,  425,  410,  396,  383,  371,
	 360,  349,  339,  330,  321,  313,  305,  297,
	 289,  282,  276,  269,  263,  257,  251,  245,
	 239,  234,  229,  224,  219,  214,  210,  205,
	 201,  196,  192,  188,  184,  180,  176,  173,
	 169,  165,  162,  158,  155,  152,  149,  145,
	 142,  139,  136,  133,  130,  127,  125,  122,
	 119,  116,  114,  111,  109,  106,  103,  101,
	  99,   96,   94,   91,   89,   87,   85,   82,
	  80,   78,   76,   74,   72,   70,   68,   66,
	  64,   62,   60,   58,   56,   54,   52,   50,
	  49,   47,   45,   43,   42,   40,   38,   36,
	  35,   33,   31,   30,   28,   27,   25,   23,
	  22,   20,   19,   17,   16,   14,   13,   11,
	  10,    8,    7,    6,    4,    3,    1,    0,
};

/* linear gain in Q16 for whole-decibel attenuation, 10^(-dB/20). The voice's
 * live volume register is linear, so level is set there rather than through
 * IFATN's attenuation field -- whose step size is documented as 0.375 dB but
 * would be one more unverified assumption in the chain. */
static const uint16_t wt_db_gain[97] = {
	65536, 58409, 52057, 46396, 41350, 36854, 32846, 29274,
	26090, 23253, 20724, 18471, 16462, 14672, 13076, 11654,
	10387,  9257,  8250,  7353,  6554,  5841,  5206,  4640,
	 4135,  3685,  3285,  2927,  2609,  2325,  2072,  1847,
	 1646,  1467,  1308,  1165,  1039,   926,   825,   735,
	  655,   584,   521,   464,   414,   369,   328,   293,
	  261,   233,   207,   185,   165,   147,   131,   117,
	  104,    93,    83,    74,    66,    58,    52,    46,
	   41,    37,    33,    29,    26,    23,    21,    18,
	   16,    15,    13,    12,    10,     9,     8,     7,
	    7,     6,     5,     5,     4,     4,     3,     3,
	    3,     2,     2,     2,     2,     1,     1,     1,
	    1,
};

/* centibels of attenuation -> 16-bit linear volume, interpolated between
 * whole-decibel table entries */
static uint32_t wt_cb_to_vol(int cb)
{
	int db, rem;
	uint32_t a, b;

	if (cb <= 0)   return 0xffff;
	if (cb >= 960) return 0;
	db  = cb / 10;
	rem = cb - db * 10;
	a = wt_db_gain[db];
	b = wt_db_gain[db + 1];
	return a - ((a - b) * (uint32_t)rem) / 10;
}

/* ----------------------------------------------------------------- state */

struct wt_chan {
	int16_t bank, prog, preset;
	int16_t volume, expr, pan, bend, bend_range;
	uint8_t sustain;
};

#define PH_HOLD 0               /* at note level, waiting out holdVolEnv  */
#define PH_DECAY 1              /* ramping toward the sustain level        */
#define PH_SUS   2              /* parked at sustain level                 */
#define PH_REL   3              /* note-off taken: ramping toward silence  */

struct wt_vc {
	int8_t   ch, key;
	uint8_t  used;
	uint8_t  held;          /* key released, kept alive by the sustain pedal */
	uint8_t  phase;
	uint8_t  atten;         /* current IFATN attenuation steps */
	uint8_t  base_atten;    /* note level (envelope attenuates from here) */
	uint8_t  sus_atten;     /* absolute atten target of the sustain level */
	uint8_t  frac;          /* envelope rate accumulator (1/256 steps) */
	uint16_t dec_rate;      /* decay: 8.8 fixed-point steps per poll tick */
	uint16_t rel_rate;      /* release: 8.8 fixed-point steps per poll tick */
	uint16_t hold_ticks;    /* polls to sit at note level before decay */
	uint16_t excl;
	uint32_t serial;
};

static struct emu10k1_card *wt_card;
static struct cardmem_s     wt_pool;
static char                *wt_pool_base;       /* page-aligned start */
static uint32_t             wt_pool_pages;
static uint32_t             wt_pool_samples;
/* Per-sample relocation: the pool is NOT a verbatim copy of the smpl chunk.
 * Each sample is copied out with a 512-sample zero pad after it, because the
 * EMU's fetch cache reads 64 samples ahead and a loop SHORTER than that makes
 * the engine wrap repeatedly inside one cache service -- the machine-wedging
 * hazard CANYON.MID exposed (this font has 165 loops of 10-63 samples: the
 * single-cycle synth waves its pads use). Short loops are unrolled into the
 * pad (they sit at the sample end, verified for the whole font), one-shots
 * loop a 192-sample zero stretch of the pad, and so every loop the hardware
 * ever sees is >=64 samples with real zeros around it. */
static int32_t             *wt_reloc;       /* pool pos - dwStart, per sample */
static uint16_t            *wt_loopext;     /* unroll: samples added to loop */
#define WT_RELOC_BAD  ((int32_t)0x80000000)
#define WT_PAD        512                   /* zero samples after each sample */
#define WT_GUARD      46                    /* spec zeros copied from the file */
#define WT_MINLOOP    64                    /* fetch-cache size */
#define WT_UNROLL_TO  192                   /* unroll short loops to >= this */
#define WT_OSLOOP_OFF (WT_GUARD + 280)      /* one-shot loop: past any unroll */
#define WT_OSLOOP_LEN 192
static sf2_file             wt_sf;
static int                  wt_ready;
static uint32_t             wt_serial;
static struct wt_chan       wt_ch[WT_CHANNELS];
static struct wt_vc         wt_v[WT_NVOICES];
static unsigned int         wt_master = 32768;  /* q15 */
static int                  wt_trim_cb;         /* AUDWTGAIN bench trim */
/* bisect switches, read from env at init: AUDWTMUTE=1 keeps the MIDI pump and
 * parser running but makes every synth entry point return before touching
 * card state; AUDWTNOCACHE=1 runs the full synth minus the per-note cache
 * invalidation writes. */
static int                  wt_mute;
static int                  wt_nocache;
/* AUDWTSKIP bitmask -- skip groups of per-note register writes:
 * 1=setup-silence  2=addresses  8=MAPA/MAPB  16=envelope/LFO  32=routing
 * 64=trigger+kill (voices never start) */
static int                  wt_skip;
/* AUDWTMAXMB: cap the sample pool at N megabytes. Samples that fall beyond
 * the cap are dropped (their zones stay silent). Diagnostic for the
 * suspicion that this notebook TINA2 implements fewer page-table index bits
 * than a desktop Audigy: a 30 MB font wedges where a 5.8 MB one is stable,
 * matching a boundary somewhere between. */
static uint32_t             wt_cap_samples;
/* AUDWTLAYERS: cap zones started per note-on. GUGS-class fonts stack up to 8
 * layers on one note = ~90 register writes in one burst inside one ISR pass,
 * a bus pattern the stable single-layer font never produces. */
static int                  wt_layers = WT_MAXZONES;
/* AUDWTTRACE: flight recorder into text video memory. The bottom-right of
 * the screen (DOSMID's static version string) is painted once and never
 * refreshed, so whatever the driver pokes there is still readable after a
 * hard wedge: note counter, prog, key, zone index, and a step letter that
 * advances through the voice-start write sequence. The screen at the moment
 * of death names the poison event and how far the writes got. */
static volatile uint16_t   *wt_scr;
static uint16_t             wt_note_ctr;

static void wt_trace_hex(int cell, unsigned int val, int digits)
{
	static const char hx[] = "0123456789ABCDEF";
	int i;
	if (!wt_scr)
		return;
	for (i = digits - 1; i >= 0; i--) {
		wt_scr[cell + i] = 0x4F00 | (uint8_t)hx[val & 0xf];
		val >>= 4;
	}
}

static void wt_trace_ch(int cell, char c)
{
	if (wt_scr)
		wt_scr[cell] = 0x4F00 | (uint8_t)c;
}
/* the parking loop: a silent stretch of pad every idle voice loops forever */
static uint32_t             wt_park_start, wt_park_end;
static int                  wt_booted;

static void *wt_alloc(uint32_t n) { return malloc((size_t)n); }
static void  wt_freep(void *p)    { free(p); }

/* little-endian shdr field access (records are 46 bytes) */
static uint32_t wt_rd32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
	     | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ------------------------------------------------------------ pitch math */

/* cents -> 16.16 frequency ratio */
static uint32_t wt_cents_ratio(int cents)
{
	int oct, semi, c;
	uint32_t r;

	if (cents < -12000) cents = -12000;
	if (cents >  12000) cents =  12000;

	semi = cents / 100;
	c    = cents - semi * 100;
	if (c < 0) { c += 100; semi--; }
	oct = semi / 12;
	semi -= oct * 12;
	if (semi < 0) { semi += 12; oct--; }

	r = (uint32_t)(((uint64_t)wt_semi_ratio[semi] * wt_cent_ratio[c]) >> 16);
	if (oct > 0)      r <<= oct;
	else if (oct < 0) r >>= -oct;
	return r;
}

/* cents -> the units srToPitch works in (2^20 per octave). Split so the
 * multiply cannot overflow 32 bits. */
static int32_t wt_cents_pitch(int cents)
{
	int32_t o = (int32_t)cents / 1200;
	int32_t r = (int32_t)cents - o * 1200;
	if (r < 0) { r += 1200; o--; }
	return o * 1048576L + (r * 1048576L) / 1200;
}

/* -------------------------------------------------------- voice handling */

/* Voices are NEVER stopped once running -- the whole bisect campaign
 * (WTTEST A-G) converged on the stop->run transition under a loaded engine
 * as the machine-wedging event, while running voices + config writes are
 * proven stable. "Off" is therefore just a mute; the voice keeps looping
 * whatever silent stretch its zone parked it in, and note-on retargets it. */
static void wt_voice_off(int v)
{
	if (wt_skip & 64)
		return;
	EMU_WritePtr(wt_card, IFATN, v, 0xffff);    /* full attenuation */
	EMU_WritePtr(wt_card, VTFT,  v, 0x0000ffff); /* volume target 0 */
}

static void wt_voice_start(int v, const sf2_zone *z, int key, int vel, int ch)
{
	uint32_t start, loopstart, loopend, target, ratio, rate_eff;
	int32_t  ip;
	int      cents, cb, steps, pan, looping;
	uint8_t  senda, sendb;

	/* ---- pitch ---- */
	cents = z->gen[SF2_GEN_SCALETUNING] * (key - z->root_key)
	      + z->gen[SF2_GEN_COARSETUNE] * 100
	      + z->gen[SF2_GEN_FINETUNE]
	      + z->pitch_correction;
	if (wt_ch[ch].bend != 8192)
		cents += ((int32_t)(wt_ch[ch].bend - 8192)
		          * wt_ch[ch].bend_range * 100) / 8192;

	ip    = (int32_t)EMU_SrToPitch(z->sample_rate) + wt_cents_pitch(cents);
	if (ip < 0) ip = 0;
	ratio = wt_cents_ratio(cents);
	rate_eff = (uint32_t)(((uint64_t)z->sample_rate * ratio) >> 16);
	if (rate_eff < 100)    rate_eff = 100;
	if (rate_eff > 192000) rate_eff = 192000;
	target = EMU_CalcPitchTarget(rate_eff);
	if (target > 0xffff) target = 0xffff;

	/* ---- level ---- */
	cb = (z->gen[SF2_GEN_INITIALATTEN] * 2) / 5;
	cb += (int)wt_vel_atten_cb[vel & 0x7f];
	cb += (int)wt_vel_atten_cb[wt_ch[ch].volume & 0x7f];
	cb += (int)wt_vel_atten_cb[wt_ch[ch].expr & 0x7f];
	if (wt_master < 32768)
		cb += (int)wt_vel_atten_cb[(wt_master >> 8) & 0x7f];
	cb += WT_HEADROOM_CB;
	cb -= wt_trim_cb;
	if (cb < 0) cb = 0;
	steps = (cb * 4) / 15;              /* IFATN: 0.375 dB per step */
	if (steps > 255) steps = 255;

	/* Volume envelope, software-stepped from the poll (~12 ms ticks) as an
	 * IFATN staircase. Steps are clamped to 3 (1.125 dB) per tick: larger
	 * jumps at the 83 Hz poll rate are audible as zipper fizz. The decay->
	 * sustain phase is NOT optional polish -- this font marks most of its
	 * cymbals and toms as looping samples whose zones rely on the envelope
	 * to take them to silence; without decay they ring forever (the
	 * "lingering hiss" bench report). */
	{
		struct wt_vc *vc = &wt_v[v - WT_FIRST_VOICE];
		uint32_t ms, rate;
		int tc, sus;

		vc->atten      = (uint8_t)steps;
		vc->base_atten = (uint8_t)steps;

		/* hold: polls at full level before decay begins */
		tc = z->gen[SF2_GEN_HOLDVOLENV];
		if (tc <= -12000)
			vc->hold_ticks = 0;
		else {
			ms = ((uint64_t)1000 * wt_cents_ratio(tc)) >> 16;
			ms /= 12;
			vc->hold_ticks = (ms > 0xffff) ? 0xffff : (uint16_t)ms;
		}

		/* sustain level: gen37 is 0.1 dB of attenuation below the note
		 * level; 0.375 dB per IFATN step -> steps = cb10 * 4 / 15 */
		sus = z->gen[SF2_GEN_SUSTAINVOLENV];
		if (sus < 0)    sus = 0;
		if (sus > 1440) sus = 1440;
		{
			int t = steps + (sus * 4) / 15;
			vc->sus_atten = (t > 255) ? 255 : (uint8_t)t;
		}

		/* Rates are 8.8 fixed point: an integer floor of 1 step/tick would
		 * cap every fade at ~3.2 s, chopping long piano/pad decays short
		 * ("notes ending sooner than they should"). Fractions accumulate
		 * in vc->frac, so a 17 s decay really takes 17 s. The whole-step
		 * clamp of 3 (1.125 dB) per tick still applies for zipper. */

		/* decay: spec time covers the full 100 dB (267 step) span */
		tc = z->gen[SF2_GEN_DECAYVOLENV];
		ms = ((uint64_t)1000 * wt_cents_ratio(tc)) >> 16;
		if (ms < 12) ms = 12;
		rate = (267u * 12u * 256u) / ms;
		if (rate < 1)        rate = 1;
		if (rate > 3u * 256) rate = 3u * 256;
		vc->dec_rate = (uint16_t)rate;

		tc = z->gen[SF2_GEN_RELEASEVOLENV];
		ms = ((uint64_t)1000 * wt_cents_ratio(tc)) >> 16;
		if (ms < 12) ms = 12;
		rate = (255u * 12u * 256u) / ms;
		if (rate < 1)        rate = 1;
		if (rate > 3u * 256) rate = 3u * 256;
		vc->rel_rate = (uint16_t)rate;

		vc->frac  = 0;
		vc->phase = PH_HOLD;
	}

	/* ---- pan ---- */
	pan = z->gen[SF2_GEN_PAN];
	if (wt_ch[ch].pan != 64)
		pan += (wt_ch[ch].pan - 64) * 1000 / 127;
	if (pan < -500) pan = -500;
	if (pan >  500) pan =  500;
	if (pan <= 0) {
		senda = 255;
		sendb = (uint8_t)(255 + (pan * 255) / 500);
	} else {
		senda = (uint8_t)(255 - (pan * 255) / 500);
		sendb = 255;
	}

	/* ---- addresses: relocate into the padded pool layout ---- */
	{
		int32_t rel;
		uint32_t end_p, ext;

		if (z->sample_index < 0 || z->sample_index >= wt_sf.n_shdr - 1)
			return;
		rel = wt_reloc[z->sample_index];
		if (rel == WT_RELOC_BAD)
			return;
		ext = wt_loopext[z->sample_index];

		start     = WT_SAMPLE_BASE + (uint32_t)((int32_t)z->start + rel);
		end_p     = WT_SAMPLE_BASE + (uint32_t)((int32_t)z->end + rel);
		loopstart = WT_SAMPLE_BASE + (uint32_t)((int32_t)z->loopstart + rel);
		loopend   = WT_SAMPLE_BASE + (uint32_t)((int32_t)z->loopend + rel) + ext;

		looping = ((z->gen[SF2_GEN_SAMPLEMODES] & 3) == SF2_LOOP_CONT
		        || (z->gen[SF2_GEN_SAMPLEMODES] & 3) == SF2_LOOP_SUSTAIN);
		if (looping && (z->loopend <= z->loopstart + 3
		                || z->loopend > z->end || z->loopstart < z->start))
			looping = 0;
		if (looping && loopend - loopstart < WT_MINLOOP)
			looping = 0;
		if (!looping) {
			loopstart = end_p + WT_OSLOOP_OFF;
			loopend   = end_p + WT_OSLOOP_OFF + WT_OSLOOP_LEN;
		}
	}

	/* ---- retarget the (already running) voice. Mute, move, retune,
	 * unmute: every write here is a plain full-register write of a class
	 * the bisect proved safe against the live engine. The voice cache
	 * still holds the silence it was parked in, which doubles as a couple
	 * of milliseconds of free declick on the attack. ---- */
	if (!(wt_skip & 64)) {
		wt_trace_ch(17, 'a');
		EMU_WritePtr(wt_card, IFATN, v, 0xffff);            /* mute */
		/* NO halt here: halting an advancing voice on this TINA2 kills it
		 * permanently ("tracks died one at a time"). The swap below has its
		 * own collision window on fast-advancing voices -- the open GUGS
		 * wedge -- but is proven stable at TimGM6mb retarget rates. See the
		 * handoff notes: safe voice mutation on TINA2 is the open problem. */
		wt_trace_ch(17, 'b');
		EMU_WritePtr(wt_card, DSL,  v, loopend);
		wt_trace_ch(17, 'c');
		EMU_WritePtr(wt_card, PSST, v, loopstart);
		wt_trace_ch(17, 'd');
		EMU_WritePtr(wt_card, CCCA, v, start
		             | EMU_SelectInterprom(wt_card, target));
		wt_trace_ch(17, 'e');
		EMU_WritePtr(wt_card, Z1, v, 0);
		EMU_WritePtr(wt_card, Z2, v, 0);
		wt_trace_ch(17, 'f');
		EMU_WritePtr(wt_card, PTRX, v,
		             (target << 16) | ((uint32_t)senda << 8) | sendb);
		wt_trace_ch(17, 'g');
		EMU_WritePtr(wt_card, CPF,  v, (target << 16));
		wt_trace_ch(17, 'h');
		EMU_WritePtr(wt_card, IP,   v, (uint32_t)(ip >> 8));
		wt_trace_ch(17, 'i');
		/* Level lives in IFATN's attenuation field -- the hardware derives
		 * the volume target from it and ramps the current volume there,
		 * which also gives the mute->level transition a free declick ramp.
		 * Writing VTFT/CVCF directly is pointless: the engine overwrites
		 * them from IFATN (measured via the register dump earlier). */
		EMU_WritePtr(wt_card, IFATN, v,
		             (uint32_t)(IFATN_FILTERCUTOFF_MASK | (steps & 0xff)));
		wt_trace_ch(17, 'j');
	}
}

/* Bring every wavetable voice up ONCE, at init time -- before AU_start, the
 * window in which starting voices is demonstrably safe (the milestone-1 probe,
 * the demo and the held-chord test all started voices here and coexisted with
 * the running engine indefinitely). Each voice gets its full static
 * configuration and is left RUNNING, muted, looping the parking silence. */
static void wt_voices_boot(struct emu10k1_card *card)
{
	uint32_t silent, park_s, park_e, target;
	int v;

	if (wt_booted)
		return;
	wt_booted = 1;

	silent = ((uint32_t)pds_cardmem_physicalptr(card->dm,
	                                            card->silentpage) << 1)
	         | MAP_PTI_MASK;
	park_s = WT_SAMPLE_BASE + wt_park_start;
	park_e = WT_SAMPLE_BASE + wt_park_end;
	target = EMU_CalcPitchTarget(22050);

	for (v = WT_FIRST_VOICE; v <= WT_LAST_VOICE; v++) {
		EMU_WritePtr(card, DCYSUSV, v, 0);
		EMU_WritePtr(card, VTFT,    v, 0x0000ffff);
		EMU_WritePtr(card, CVCF,    v, 0x0000ffff);
		EMU_WritePtr(card, PTRX,    v, (target << 16) | 0xffff);
		EMU_WritePtr(card, CPF,     v, 0);
		EMU_WritePtr(card, DSL,     v, park_e);
		EMU_WritePtr(card, PSST,    v, park_s);
		EMU_WritePtr(card, CCCA,    v, park_s
		             | EMU_SelectInterprom(card, target));
		EMU_WritePtr(card, Z1, v, 0);
		EMU_WritePtr(card, Z2, v, 0);
		EMU_WritePtr(card, CD0,     v, 0);
		EMU_WritePtr(card, CD0 + 1, v, 0);
		EMU_WritePtr(card, CCR, v, 0);
		EMU_WritePtr(card, CCR, v, (uint32_t)30 << 25);
		EMU_WritePtr(card, MAPA, v, silent);
		EMU_WritePtr(card, MAPB, v, silent);
		EMU_WritePtr(card, ATKHLDM, v, 0x7f00);
		EMU_WritePtr(card, DCYSUSM, v, 0);
		EMU_WritePtr(card, LFOVAL1, v, 0x8000);
		EMU_WritePtr(card, LFOVAL2, v, 0x8000);
		EMU_WritePtr(card, FMMOD,   v, 0);
		EMU_WritePtr(card, TREMFRQ, v, 0);
		EMU_WritePtr(card, FM2FRQ2, v, 0);
		EMU_WritePtr(card, ENVVAL,  v, 0xefff);
		EMU_WritePtr(card, ATKHLDV, v,
		             ATKHLDV_HOLDTIME_MASK | ATKHLDV_ATTACKTIME_MASK);
		EMU_WritePtr(card, ENVVOL,  v, 0x8000);
		EMU_WritePtr(card, PEFE_FILTERAMOUNT, v, 0);
		EMU_WritePtr(card, PEFE_PITCHAMOUNT,  v, 0);
		EMU_WritePtr(card, A_FXRT1, v, 0x03020100);
		EMU_WritePtr(card, A_FXRT2, v, 0x3f3f3f3f);
		EMU_WritePtr(card, A_SENDAMOUNTS, v, 0);
		/* trigger, muted: the voice runs from here on and never stops */
		EMU_WritePtr(card, IFATN, v, 0xffff);
		EMU_WritePtr(card, DCYSUSV, v, (DCYSUSV_CHANNELENABLE_MASK | 0x7f00));
		EMU_WritePtr(card, PTRX, v, (target << 16) | 0xffff);
		EMU_WritePtr(card, CPF,  v, (target << 16));
		EMU_WritePtr(card, IP,   v, EMU_SrToPitch(22050) >> 8);
	}
}

static int wt_alloc_voice(void)
{
	int i, best = -1;
	uint32_t oldest = 0xffffffffu;

	for (i = 0; i < WT_NVOICES; i++)
		if (!wt_v[i].used)
			return i;
	/* steal order: quietest releasing voice, then the quietest decaying
	 * one (a dying cymbal), and only then the oldest note -- so drum tails
	 * absorb the voice pressure instead of held melodic notes */
	{
		int worst_att = -1;
		for (i = 0; i < WT_NVOICES; i++)
			if (wt_v[i].phase == PH_REL && (int)wt_v[i].atten > worst_att) {
				worst_att = wt_v[i].atten;
				best = i;
			}
		if (best >= 0)
			return best;
		for (i = 0; i < WT_NVOICES; i++)
			if (wt_v[i].phase == PH_DECAY
			    && (int)wt_v[i].atten > wt_v[i].base_atten + 32
			    && (int)wt_v[i].atten > worst_att) {
				worst_att = wt_v[i].atten;
				best = i;
			}
		if (best >= 0)
			return best;
	}
	/* last resort: the oldest note. (A "refuse and drop the note" variant
	 * tried during the GUGS chase starved polyphony -- tracks died one by
	 * one as long decays held voices. Canyon-class songs NEED steals.) */
	best = 0;
	for (i = 0; i < WT_NVOICES; i++)
		if (wt_v[i].serial < oldest) {
			oldest = wt_v[i].serial;
			best = i;
		}
	return best;
}

static void wt_kill(int i)
{
	if (wt_v[i].used) {
		wt_voice_off(WT_FIRST_VOICE + i);
		wt_v[i].used = 0;
		wt_v[i].held = 0;
	}
}

/* note-off: don't cut -- start the zone's release fade. The abrupt IFATN
 * mute was the audible "pop when a sample stops": a full-level waveform
 * gated in under a millisecond. */
static void wt_release(int i)
{
	if (wt_v[i].used && wt_v[i].phase != PH_REL) {
		wt_v[i].phase = PH_REL;
		wt_v[i].held = 0;
	}
}

/* ------------------------------------------------------------- MIDI side */

/* Zone scratch lives OFF the stack: note-on runs inside the sound ISR, whose
 * private stack is only STACKCORR (4 KB, stackisr.asm) per level, and eight
 * resolved zones are 1.3 KB by themselves. Single-threaded by construction:
 * during playback the only caller is the ISR's MIDI pump, and the sound IRQ
 * does not re-enter itself. */
static sf2_zone wt_zscratch[WT_MAXZONES];

void EMUWT_NoteOn(int ch, int key, int vel)
{
	sf2_zone *z = wt_zscratch;
	int n, i;

	if (!wt_ready || wt_mute || ch < 0 || ch >= WT_CHANNELS)
		return;
	if (vel <= 0) { EMUWT_NoteOff(ch, key); return; }
	if (key < 0 || key > 127)
		return;
	if (wt_ch[ch].preset < 0)
		return;

	wt_note_ctr++;
	wt_trace_hex(0, wt_note_ctr, 3);
	wt_trace_hex(4, (unsigned)wt_ch[ch].prog, 2);
	wt_trace_hex(7, (unsigned)key, 2);
	n = sf2_resolve(&wt_sf, wt_ch[ch].preset, key, vel, z, WT_MAXZONES);
	if (n > wt_layers)
		n = wt_layers;
	wt_trace_hex(10, (unsigned)n, 1);
	for (i = 0; i < n; i++) {
		int v;
		/* exclusive class: a new hi-hat silences the open one */
		if (z[i].gen[SF2_GEN_EXCLUSIVECLASS]) {
			int j;
			for (j = 0; j < WT_NVOICES; j++)
				if (wt_v[j].used && wt_v[j].ch == ch
				    && wt_v[j].excl == (uint16_t)z[i].gen[SF2_GEN_EXCLUSIVECLASS])
					wt_kill(j);
		}
		v = wt_alloc_voice();
		if (v < 0)
			continue;       /* all voices loud: drop rather than hot-steal */
		wt_kill(v);
		wt_v[v].ch     = (int8_t)ch;
		wt_v[v].key    = (int8_t)key;
		wt_v[v].used   = 1;
		wt_v[v].held   = 0;
		wt_v[v].excl   = (uint16_t)z[i].gen[SF2_GEN_EXCLUSIVECLASS];
		wt_v[v].serial = ++wt_serial;
		wt_trace_hex(12, (unsigned)i, 1);
		wt_trace_hex(14, (unsigned)(WT_FIRST_VOICE + v), 2);
		wt_voice_start(WT_FIRST_VOICE + v, &z[i], key, vel, ch);
		wt_trace_ch(17, '.');
	}
}

void EMUWT_NoteOff(int ch, int key)
{
	int i;
	if (!wt_ready || wt_mute)
		return;
	if (ch == 9)
		return;                 /* GM: percussion ignores note-off */
	for (i = 0; i < WT_NVOICES; i++)
		if (wt_v[i].used && wt_v[i].ch == ch && wt_v[i].key == key
		    && !wt_v[i].held && wt_v[i].phase != PH_REL) {
			if (wt_ch[ch].sustain)
				wt_v[i].held = 1;
			else
				wt_release(i);
		}
}

void EMUWT_SetBankPreset(int ch, int bank, int prog)
{
	int p;
	if (!wt_ready || ch < 0 || ch >= WT_CHANNELS)
		return;
	wt_ch[ch].bank = (int16_t)bank;
	wt_ch[ch].prog = (int16_t)prog;
	p = sf2_find_preset(&wt_sf, bank, prog);
	if (p < 0 && bank != 0 && bank != 128)
		p = sf2_find_preset(&wt_sf, 0, prog);       /* fall back to GM */
	if (p < 0 && bank == 128)
		p = sf2_find_preset(&wt_sf, 128, 0);        /* any drum kit */
	if (p < 0)
		p = sf2_find_preset(&wt_sf, 0, 0);
	wt_ch[ch].preset = (int16_t)p;
}

void EMUWT_ProgramChange(int ch, int prog)
{
	if (ch < 0 || ch >= WT_CHANNELS)
		return;
	EMUWT_SetBankPreset(ch, wt_ch[ch].bank, prog);
}

void EMUWT_ControlChange(int ch, int cc, int val)
{
	if (!wt_ready || wt_mute || ch < 0 || ch >= WT_CHANNELS)
		return;
	switch (cc) {
	case 0:                                  /* bank select MSB */
		if (ch != 9)
			wt_ch[ch].bank = (int16_t)val;
		break;
	case 7:  wt_ch[ch].volume = (int16_t)val; break;
	case 10: wt_ch[ch].pan    = (int16_t)val; break;
	case 11: wt_ch[ch].expr   = (int16_t)val; break;
	case 64:
		wt_ch[ch].sustain = (uint8_t)(val >= 64);
		if (!wt_ch[ch].sustain) {
			int i;
			for (i = 0; i < WT_NVOICES; i++)
				if (wt_v[i].used && wt_v[i].ch == ch && wt_v[i].held)
					wt_release(i);
		}
		break;
	case 120:                                /* all sound off: immediate */
		{
			int i;
			for (i = 0; i < WT_NVOICES; i++)
				if (wt_v[i].used && wt_v[i].ch == ch)
					wt_kill(i);
		}
		break;
	case 123:                                /* all notes off: with release */
		{
			int i;
			for (i = 0; i < WT_NVOICES; i++)
				if (wt_v[i].used && wt_v[i].ch == ch)
					wt_release(i);
		}
		break;
	case 121:                                /* reset all controllers */
		wt_ch[ch].volume = 100;
		wt_ch[ch].expr   = 127;
		wt_ch[ch].pan    = 64;
		wt_ch[ch].bend   = 8192;
		wt_ch[ch].sustain = 0;
		break;
	}
}

void EMUWT_PitchBend(int ch, int bend14)
{
	if (!wt_ready || wt_mute || ch < 0 || ch >= WT_CHANNELS)
		return;
	wt_ch[ch].bend = (int16_t)bend14;
	/* Retuning sounding voices needs a per-voice cache of their zone; until
	 * that exists the bend takes effect on the next note. */
}

void EMUWT_ChannelPressure(int ch, int val) { (void)ch; (void)val; }

void EMUWT_SetMasterVolume(unsigned int q15) { wt_master = q15; }

void EMUWT_AllNotesOff(void)
{
	int i;
	for (i = 0; i < WT_NVOICES; i++)
		wt_kill(i);
}

void EMUWT_Reset(void)
{
	int i;
	EMUWT_AllNotesOff();
	for (i = 0; i < WT_CHANNELS; i++) {
		wt_ch[i].bank   = (i == 9) ? 128 : 0;
		wt_ch[i].prog   = 0;
		wt_ch[i].volume = 100;
		wt_ch[i].expr   = 127;
		wt_ch[i].pan    = 64;
		wt_ch[i].bend   = 8192;
		wt_ch[i].bend_range = 2;
		wt_ch[i].sustain = 0;
		wt_ch[i].preset = -1;
		if (wt_ready)
			EMUWT_SetBankPreset(i, wt_ch[i].bank, 0);
	}
}

int EMUWT_Active(void) { return wt_ready; }

/* Called once per sound interrupt, right after the MIDI pump: steps every
 * releasing voice's IFATN attenuation toward silence at its zone's release
 * rate, and frees it when it gets there. Plain IFATN writes only. */
void EMUWT_Poll(void)
{
	int i;
	if (!wt_ready || wt_mute)
		return;
	for (i = 0; i < WT_NVOICES; i++) {
		struct wt_vc *vc = &wt_v[i];
		int rate, target, acc;

		if (!vc->used)
			continue;
		switch (vc->phase) {
		case PH_HOLD:
			if (vc->hold_ticks) {
				vc->hold_ticks--;
				continue;
			}
			vc->phase = PH_DECAY;
			/* fall through */
		case PH_DECAY:
			rate = vc->dec_rate;
			target = vc->sus_atten;
			break;
		case PH_REL:
			rate = vc->rel_rate;
			target = 255;
			break;
		default:                        /* PH_SUS: nothing to step */
			continue;
		}

		/* accumulate the 8.8 rate; only whole steps touch the register */
		acc = (int)vc->frac + rate;
		vc->frac = (uint8_t)(acc & 0xff);
		rate = acc >> 8;
		if (rate == 0)
			continue;

		if ((int)vc->atten + rate >= target) {
			vc->atten = (uint8_t)target;
			if (target >= 255) {
				/* faded out entirely: free the voice and halt it -- an
				 * idle voice has no business keeping the fetch engine
				 * serviced */
				vc->used = 0;
				vc->phase = PH_SUS;
				EMU_WritePtr(wt_card, IFATN, WT_FIRST_VOICE + i, 0xffff);
				EMU_WritePtr(wt_card, VTFT,  WT_FIRST_VOICE + i, 0x0000ffff);
				continue;
			}
			if (vc->phase == PH_DECAY)
				vc->phase = PH_SUS;
		} else
			vc->atten = (uint8_t)(vc->atten + rate);

		EMU_WritePtr(wt_card, IFATN, WT_FIRST_VOICE + i,
		             (uint32_t)(IFATN_FILTERCUTOFF_MASK | vc->atten));
	}
}

/* ---------------------------------------------------------------- loader */

static void *wt_fh;

static int wt_io_read(void *h, void *buf, uint32_t len)
{
	return fread(buf, (int)len, 1, h) == (int)len;
}

static int wt_io_seek(void *h, uint32_t off)
{
	return fseek(h, (int)off, 0) != -1;
}

/* rotate-and-add: catches transposed or dropped chunks, which a plain sum
 * would not */
static uint32_t wt_checksum(const uint8_t *p, uint32_t n)
{
	uint32_t s = 0, i;
	for (i = 0; i < n; i++)
		s = ((s << 1) | (s >> 31)) + p[i];
	return s;
}

int EMUWT_Init(struct emu10k1_card *card, const char *sf2path)
{
	sf2_io io;
	uint32_t need, got, i;
	char *p;

	if (wt_ready)
		return 1;
	wt_card = card;

	wt_fh = fopen(sf2path, "rb");
	if (!wt_fh) {
		printf("Wavetable: cannot open %s\n", sf2path);
		return 0;
	}
	io.h = wt_fh; io.read = wt_io_read; io.seek = wt_io_seek;

	if (!sf2_open(&wt_sf, &io, wt_alloc, wt_freep)) {
		printf("Wavetable: %s is not a usable SoundFont\n", sf2path);
		fclose(wt_fh);
		return 0;
	}

	/* worst-case padded size for the page budget check */
	wt_pool_pages = ((wt_sf.smpl_bytes / 2
	                  + (uint32_t)(wt_sf.n_shdr - 1) * (WT_GUARD + WT_PAD))
	                 * 2 + EMUPAGESIZE - 1) / EMUPAGESIZE;
	if (WT_BASE_PAGE + wt_pool_pages > emuwt_maxpages) {
		printf("Wavetable: %s needs %lu kB of sample space, limit is %lu kB\n",
		       sf2path, (unsigned long)(wt_sf.smpl_bytes >> 10),
		       (unsigned long)((emuwt_maxpages - WT_BASE_PAGE) * EMUPAGESIZE
		                       >> 10));
		goto fail;
	}

	/* Lay the pool out sample by sample: copy [dwStart .. dwEnd+46) for each,
	 * then a 512-sample zero pad. Unroll sub-64-sample loops into the pad so
	 * the fetch cache never sees a loop shorter than itself. */
	{
		int ns = wt_sf.n_shdr - 1;
		uint32_t cursor = 0;        /* pool position, in samples */
		int i, unrolled = 0, dropped = 0;

		wt_reloc   = (int32_t *)wt_alloc((uint32_t)ns * sizeof(int32_t));
		wt_loopext = (uint16_t *)wt_alloc((uint32_t)ns * sizeof(uint16_t));
		if (!wt_reloc || !wt_loopext)
			goto fail;

		/* total size first */
		need = 0;
		for (i = 0; i < ns; i++) {
			const uint8_t *sh = wt_sf.shdr + (long)i * 46;
			uint32_t st = wt_rd32(sh + 20), en = wt_rd32(sh + 24);
			if (en > st)
				need += (en - st) + WT_GUARD + WT_PAD;
		}
		wt_pool_samples = need;
		need = need * 2 + EMUPAGESIZE;
		if (!MDma_alloc_cardmem(&wt_pool, need)) {
			printf("Wavetable: could not get %lu kB of DMA memory for samples\n",
			       (unsigned long)(need >> 10));
			goto fail;
		}
		wt_pool_base = (char *)(((uint32_t)wt_pool.pMem + EMUPAGESIZE - 1)
		                        & ~((uint32_t)EMUPAGESIZE - 1));

		for (i = 0; i < ns; i++) {
			const uint8_t *sh = wt_sf.shdr + (long)i * 46;
			uint32_t st = wt_rd32(sh + 20), en = wt_rd32(sh + 24);
			uint32_t ls = wt_rd32(sh + 28), le = wt_rd32(sh + 32);
			uint32_t copy, got2, avail;
			char *dst;

			wt_loopext[i] = 0;
			if (en <= st) {
				wt_reloc[i] = WT_RELOC_BAD;
				continue;
			}
			if (wt_cap_samples
			    && cursor + (en - st) + WT_GUARD + WT_PAD > wt_cap_samples) {
				wt_reloc[i] = WT_RELOC_BAD;
				dropped++;
				continue;
			}
			wt_reloc[i] = (int32_t)cursor - (int32_t)st;

			/* bytes of real data available in the chunk from dwStart */
			copy  = (en - st) + WT_GUARD;
			avail = (st * 2 < wt_sf.smpl_bytes)
			      ? (wt_sf.smpl_bytes - st * 2) / 2 : 0;
			if (copy > avail)
				copy = avail;

			if (!wt_io_seek(wt_fh, wt_sf.smpl_off + st * 2)) {
				printf("Wavetable: seek failed at sample %d\n", i);
				goto fail_dma;
			}
			dst = wt_pool_base + cursor * 2;
			for (got2 = 0; got2 < copy * 2; ) {
				uint32_t want = copy * 2 - got2;
				if (want > WT_READCHUNK)
					want = WT_READCHUNK;
				if (fread(dst + got2, (int)want, 1, wt_fh) != (int)want) {
					printf("Wavetable: short read in sample %d\n", i);
					goto fail_dma;
				}
				got2 += want;
			}
			/* zero the rest of guard+pad */
			{
				uint32_t total = (en - st) + WT_GUARD + WT_PAD;
				memset(dst + copy * 2, 0, (total - copy) * 2);
			}

			/* unroll a short loop (it sits at/near the sample end) */
			if (le > ls && le - ls < WT_MINLOOP && le <= en && en - le <= WT_MINLOOP) {
				uint32_t L = le - ls, span = L, k = 0;
				int16_t *pool = (int16_t *)wt_pool_base;
				uint32_t src = (uint32_t)((int32_t)ls + wt_reloc[i]);
				uint32_t at  = (uint32_t)((int32_t)le + wt_reloc[i]);
				while (span < WT_UNROLL_TO) {
					uint32_t j;
					for (j = 0; j < L; j++)
						pool[at + k * L + j] = pool[src + j];
					k++;
					span += L;
				}
				wt_loopext[i] = (uint16_t)(k * L);
				unrolled++;
			}

			cursor += (en - st) + WT_GUARD + WT_PAD;
		}
		fclose(wt_fh);
		wt_fh = NULL;

		/* parking loop = the first valid sample's one-shot pad region */
		wt_park_start = wt_park_end = 0;
		for (i = 0; i < ns; i++)
			if (wt_reloc[i] != WT_RELOC_BAD) {
				const uint8_t *sh = wt_sf.shdr + (long)i * 46;
				uint32_t en = wt_rd32(sh + 24);
				uint32_t e = (uint32_t)((int32_t)en + wt_reloc[i]);
				wt_park_start = e + WT_OSLOOP_OFF;
				wt_park_end   = e + WT_OSLOOP_OFF + WT_OSLOOP_LEN;
				break;
			}
		if (wt_park_end == 0) {
			printf("Wavetable: no usable samples in %s\n", sf2path);
			goto fail_dma;
		}
		printf("Wavetable: %d samples laid out, %d short loops unrolled",
		       ns - dropped, unrolled);
		if (dropped)
			printf(", %d DROPPED by the pool cap", dropped);
		printf("\n");
	}

	/* map the pool into the card's page table */
	wt_pool_pages = (wt_pool_samples * 2 + EMUPAGESIZE - 1) / EMUPAGESIZE;
	for (i = 0; i < wt_pool_pages; i++)
		card->virtualpagetable[WT_BASE_PAGE + i] =
			((uint32_t)pds_cardmem_physicalptr(wt_pool,
			                 wt_pool_base + i * EMUPAGESIZE) << 1)
			| (WT_BASE_PAGE + i);

	{
		const char *g = getenv("AUDWTGAIN");
		wt_trim_cb = g ? atoi(g) : 0;
		wt_mute    = getenv("AUDWTMUTE")    ? 1 : 0;
		wt_nocache = getenv("AUDWTNOCACHE") ? 1 : 0;
		emuwt_noio = getenv("AUDWTNOIO")    ? 1 : 0;
		g = getenv("AUDWTSKIP");
		wt_skip = g ? atoi(g) : 0;
		if (getenv("AUDWTTRACE")) {
			wt_scr = (volatile uint16_t *)NearPtr(0xB8000)
			       + (24 * 80 + 62);
			printf("Wavetable: screen-corner trace ON (AUDWTTRACE)\n");
		}
		g = getenv("AUDWTLAYERS");
		wt_layers = g ? atoi(g) : WT_MAXZONES;
		if (wt_layers < 1)          wt_layers = 1;
		if (wt_layers > WT_MAXZONES) wt_layers = WT_MAXZONES;
		if (wt_layers != WT_MAXZONES)
			printf("Wavetable: max %d layer(s) per note (AUDWTLAYERS)\n",
			       wt_layers);
		g = getenv("AUDWTMAXMB");
		wt_cap_samples = g ? ((uint32_t)atoi(g) << 20) / 2 : 0;
		if (wt_cap_samples)
			printf("Wavetable: sample pool capped at %s MB (AUDWTMAXMB)\n", g);
		if (wt_skip)
			printf("Wavetable: skipping write groups %d for bisect (AUDWTSKIP)\n",
			       wt_skip);
		if (emuwt_noio)
			printf("Wavetable: card I/O stubbed OUT for bisect (AUDWTNOIO)\n");
		if (wt_mute)
			printf("Wavetable: MUTED for bisect (AUDWTMUTE)\n");
		if (wt_nocache)
			printf("Wavetable: cache invalidation OFF for bisect (AUDWTNOCACHE)\n");
	}
	wt_voices_boot(card);

	wt_ready = 1;
	EMUWT_Reset();

	printf("Wavetable: %s, SF2 v%d.%02d, %d presets, pool %lu kB "
	       "(%lu pages from %d)\n",
	       sf2path, wt_sf.ver_major, wt_sf.ver_minor,
	       sf2_preset_count(&wt_sf), (unsigned long)(wt_pool_samples * 2 >> 10),
	       (unsigned long)wt_pool_pages, WT_BASE_PAGE);
	printf("Wavetable: voices %d-%d on the EMU10K2, host CPU does no mixing "
	       "(headroom %d.%d dB)\n", WT_FIRST_VOICE, WT_LAST_VOICE,
	       (WT_HEADROOM_CB - wt_trim_cb) / 10,
	       ((WT_HEADROOM_CB - wt_trim_cb) % 10 + 10) % 10);
	return 1;

fail_dma:
	MDma_free_cardmem(&wt_pool);
fail:
	sf2_close(&wt_sf, wt_freep);
	if (wt_fh) { fclose(wt_fh); wt_fh = NULL; }
	return 0;
}

void EMUWT_Exit(void)
{
	int v;
	if (!wt_ready)
		return;
	EMUWT_AllNotesOff();
	/* stop the voices for real -- the driver is tearing the chip down, and
	 * the pool pages are about to be freed under them */
	for (v = WT_FIRST_VOICE; v <= WT_LAST_VOICE; v++) {
		EMU_WritePtr(wt_card, IFATN, v, 0xffff);
		EMU_WritePtr(wt_card, IP,    v, 0);
		EMU_WritePtr(wt_card, CPF,   v, 0);
	}
	wt_booted = 0;
	wt_ready = 0;
	/* leave the page-table entries pointing at our pages until the driver
	 * tears the whole table down -- nothing reads them once the voices are
	 * stopped */
	MDma_free_cardmem(&wt_pool);
	if (wt_reloc)   { wt_freep(wt_reloc);   wt_reloc = NULL; }
	if (wt_loopext) { wt_freep(wt_loopext); wt_loopext = NULL; }
	sf2_close(&wt_sf, wt_freep);
}

/* ------------------------------------------------------------------ demo */

void EMUWT_Demo(int bank, int prog)
{
	static const int scale[8] = { 60, 62, 64, 65, 67, 69, 71, 72 };
	static const int chord[4] = { 60, 64, 67, 72 };
	char nm[21];
	int i, reps;

	if (!wt_ready)
		return;
	EMUWT_SetBankPreset(0, bank, prog);
	if (wt_ch[0].preset < 0) {
		printf("Wavetable: no preset for bank %d program %d\n", bank, prog);
		return;
	}
	sf2_preset_info(&wt_sf, wt_ch[0].preset, nm, NULL, NULL);
	{
		const char *r = getenv("AUDWTREP");
		reps = r ? atoi(r) : 1;
		if (reps < 1)  reps = 1;
		if (reps > 60) reps = 60;
	}
	printf("Wavetable: playing '%s' (bank %d program %d)"
	       "%s%d time(s), about %d s...\n", nm, bank, prog,
	       reps > 1 ? ", " : ", ", reps, reps * 4);

  again:
	for (i = 0; i < 8; i++) {
		EMUWT_NoteOn(0, scale[i], 100);
		pds_delay_10us(30000);          /* 300 ms */
		EMUWT_NoteOff(0, scale[i]);
		pds_delay_10us(5000);
	}
	for (i = 0; i < 4; i++)
		EMUWT_NoteOn(0, chord[i], 100);
	if (getenv("AUDWTDUMP")) {
		int j;
		for (j = 0; j < 4; j++) {
			int hv = WT_FIRST_VOICE + j;
			printf("  v%-2d CCCA %08lX IFATN %04lX CVCF %08lX VTFT %08lX "
			       "PTRX %08lX DCYSUSV %04lX\n", hv,
			       (unsigned long)EMU_ReadPtr(wt_card, CCCA, hv),
			       (unsigned long)EMU_ReadPtr(wt_card, IFATN, hv),
			       (unsigned long)EMU_ReadPtr(wt_card, CVCF, hv),
			       (unsigned long)EMU_ReadPtr(wt_card, VTFT, hv),
			       (unsigned long)EMU_ReadPtr(wt_card, PTRX, hv),
			       (unsigned long)EMU_ReadPtr(wt_card, DCYSUSV, hv));
		}
	}
	if (getenv("AUDWTHOLD")) {
		/* leave it sounding: a held chord is the only way to catch the card
		 * mid-fetch, and CCCA advancing inside the loop bounds is the proof
		 * that the DSP really is reading our pages out of host RAM */
		printf("Wavetable: holding a chord on voices %d-%d "
		       "(unset AUDWTHOLD to stop)\n", WT_FIRST_VOICE,
		       WT_FIRST_VOICE + 3);
		return;
	}
	pds_delay_10us(150000);             /* 1.5 s */
	for (i = 0; i < 4; i++)
		EMUWT_NoteOff(0, chord[i]);
	if (--reps > 0) {
		pds_delay_10us(50000);          /* half a second between passes */
		goto again;
	}
	EMUWT_AllNotesOff();                /* never leave a voice bus-mastering */
	printf("Wavetable: demo done\n");
}

/* ------------------------------------------------------- milestone-1 probe
 * A generated tone on one voice with no soundfont involved. Kept separate so
 * "the sample path is alive" and "the soundfont is parsed correctly" stay
 * independently testable. */

#define WTS_VOICE   3
#define WTS_PAGE    (WT_BASE_PAGE - 4)  /* its own pages, clear of the pool */
#define WTS_FRAMES  1024
#define WTS_PERIOD  128

static struct cardmem_s wts_dm;
static int wts_done;

void EMUWT_Selftest(struct emu10k1_card *card)
{
	unsigned int i, npages, startsmp, endsmp;
	uint32_t silent;
	char *page;
	int16_t *smp;
	int v = WTS_VOICE;

	if (wts_done)
		return;
	wts_done = 1;
	wt_card = card;

	if (!MDma_alloc_cardmem(&wts_dm, WTS_FRAMES * 2 + EMUPAGESIZE)) {
		dbgprintf(("EMUWT_Selftest: cardmem alloc failed\n"));
		return;
	}
	page = (char *)(((uint32_t)wts_dm.pMem + EMUPAGESIZE - 1)
	                & ~((uint32_t)EMUPAGESIZE - 1));

	/* triangle wave -- integer only, this build has no business using the FPU */
	smp = (int16_t *)page;
	for (i = 0; i < WTS_FRAMES; i++) {
		unsigned int p = i % WTS_PERIOD;
		int val = (p < WTS_PERIOD / 2) ? (int)(p * 2)
		                               : (int)((WTS_PERIOD - p) * 2);
		smp[i] = (int16_t)((val - WTS_PERIOD / 2) * 400);
	}

	npages = (WTS_FRAMES * 2 + EMUPAGESIZE - 1) / EMUPAGESIZE;
	for (i = 0; i < npages; i++)
		card->virtualpagetable[WTS_PAGE + i] =
			((uint32_t)pds_cardmem_physicalptr(wts_dm,
			                 page + i * EMUPAGESIZE) << 1)
			| (WTS_PAGE + i);

	startsmp = ((uint32_t)WTS_PAGE * EMUPAGESIZE) / 2;
	endsmp   = startsmp + WTS_FRAMES;

	dbgprintf(("EMUWT_Selftest: page %u, samples %X..%X\n",
	           WTS_PAGE, startsmp, endsmp));

	EMU_WritePtr(card, DCYSUSV, v, 0);
	EMU_WritePtr(card, VTFT,    v, 0xffff);
	EMU_WritePtr(card, CVCF,    v, 0xffff);
	EMU_WritePtr(card, PTRX,    v, (0xff << 8) | 0xff);
	EMU_WritePtr(card, CPF,     v, 0);
	EMU_WritePtr(card, DSL,     v, endsmp);
	EMU_WritePtr(card, PSST,    v, startsmp);
	EMU_WritePtr(card, CCCA,    v, startsmp
	             | EMU_SelectInterprom(card, card->voice_pitch_target));
	EMU_WritePtr(card, Z1, v, 0);
	EMU_WritePtr(card, Z2, v, 0);

	/* cache invalidation, as the PCM path does -- else the first ~64 samples
	 * are stale and the tone starts with a pop */
	EMU_WritePtr(card, CD0,     v, 0);
	EMU_WritePtr(card, CD0 + 1, v, 0);
	EMU_WritePtr(card, CCR_CACHEINVALIDSIZE, v, 0);
	EMU_WritePtr(card, CCR_READADDRESS, v, 64);
	EMU_WritePtr(card, CCR_CACHEINVALIDSIZE, v, 30);

	silent = ((uint32_t)pds_cardmem_physicalptr(card->dm, card->silentpage) << 1)
	         | MAP_PTI_MASK;
	EMU_WritePtr(card, MAPA, v, silent);
	EMU_WritePtr(card, MAPB, v, silent);

	EMU_WritePtr(card, ATKHLDM, v, 0x7f00);
	EMU_WritePtr(card, DCYSUSM, v, 0);
	EMU_WritePtr(card, LFOVAL1, v, 0x8000);
	EMU_WritePtr(card, LFOVAL2, v, 0x8000);
	EMU_WritePtr(card, FMMOD,   v, 0);
	EMU_WritePtr(card, TREMFRQ, v, 0);
	EMU_WritePtr(card, FM2FRQ2, v, 0);
	EMU_WritePtr(card, ENVVAL,  v, 0xefff);
	EMU_WritePtr(card, ATKHLDV, v,
	             ATKHLDV_HOLDTIME_MASK | ATKHLDV_ATTACKTIME_MASK);
	EMU_WritePtr(card, ENVVOL,  v, 0x8000);
	EMU_WritePtr(card, PEFE_FILTERAMOUNT, v, 0);
	EMU_WritePtr(card, PEFE_PITCHAMOUNT,  v, 0);

	EMU_WritePtr(card, A_FXRT1, v, 0x03020100);
	EMU_WritePtr(card, A_FXRT2, v, 0x3f3f3f3f);
	EMU_WritePtr(card, A_SENDAMOUNTS, v, 0);

	EMU_WritePtr(card, IFATN, v, IFATN_FILTERCUTOFF_MASK);
	EMU_WritePtr(card, DCYSUSV, v, (DCYSUSV_CHANNELENABLE_MASK | 0x7f00));
	EMU_WritePtr(card, PTRX_PITCHTARGET, v, card->voice_pitch_target);
	EMU_WritePtr(card, CPF_CURRENTPITCH, v, card->voice_pitch_target);
	EMU_WritePtr(card, IP, v, card->voice_initial_pitch);

	printf("Wavetable probe: looping a tone on hardware voice %d "
	       "(page %u). Unset AUDWT to silence.\n", v, WTS_PAGE);
}

#endif /* CARD_AUDIGY */
