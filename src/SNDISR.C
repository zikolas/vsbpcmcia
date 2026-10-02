
/* sound hardware interrupt routine */

#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#include "CONFIG.H"
#include "PLATFORM.H"
#include "PIC.H"
#include "LINEAR.H"
#include "VDMA.H"
#include "VIRQ.H"
#include "VOPL3.H"
#include "VSB.H"
#include "PTRAP.H"
#include "PTOPS.H"
#if DACRING
#include "JLMSHARE.H"   /* the direct-DAC ring (SNDISR_DacFeed) */
#endif

#include "HOSTSVC.H"       /* LOW_PokeB: the engine telemetry pokes below */
#include "ADPCM.H"

#ifdef _DEBUG
//#define SNDISRLOG /* enables sound interrupt logs */
#include <stdio.h>

/* optionally emit PCM data;
 * if activated, file logfile.asm (HDLFUNC!) must also be changed!
 * LOGPCM8DATA: log happens BEFORE sample rate conversion
 * LOGPCM16DATA: log happens AFTER sample rate conversion
 */
#define LOGPCM8DATA  1 /* support /LM1 - 8-bit PCM data, mono only */
#define LOGPCM16DATA 1 /* support /LM2 - 16-bit PCM data, mono only */

# if LOGPCM8DATA
#  ifdef DJGPP
static inline void writepcm8data(unsigned char x) { asm("movb %0, %%dl\n\t" "movw $0x81, %%ax\n\t" "int $0x41" ::"r" (x): "%eax", "%edx"); }
#  else
void writepcm8data(unsigned char);
#pragma aux writepcm8data = \
    "mov ax, 0081h" \
    "int 41h" \
    parm [dl] \
    modify exact [eax edx]
#  endif
# endif
# if LOGPCM16DATA
#  ifdef DJGPP
static inline void writepcm16data(short x) { asm("movw %0, %%dx\n\t" "movw $0x82, %%ax\n\t" "int $0x41" ::"r" (x): "%eax", "%edx" ); }
#  else
void writepcm16data(short);
#pragma aux writepcm16data = \
    "mov ax, 0082h" \
    "int 41h" \
    parm [dx] \
    modify exact [eax edx]
#  endif
# endif

#endif

#include "AU.H"

#if SOUNDFONT
#include "VMPU.H"
#ifdef CARD_AUDIGY
#include "emu_wt.h"     /* EMU10K2 hardware wavetable: parse MIDI, card renders */
#endif
//#include "../tsf/TSF.H"
//extern tsf* tsfrenderer;
extern void* tsfrenderer;
void tsf_render_short(void *, short *, int, int);
#endif

#define SUP16BITUNSIGNED 1 /* support 16-bit unsigned format */

#define MIXERROUTINE 0

#define VOICELR 1

/* Engine-owned passthrough state. These used to be per-backend copies of
 * ES1688_PT / es_in_render duplicated across every sc_* card file; they are
 * engine state, not card state, so they live here once.
 *   SNDISR_PassThru: the backend arms this in its adetect when its card is
 *     the selected AU output -- raw guest DMA is then routed to the PT feed
 *     instead of the render path. 0 = stock render path, other cards
 *     unaffected.
 *   es_in_render: 1 while the owning SNDISR pass is in the heavy render+tap
 *     path, so a re-entrant SNDISR (SETIF=1) skips the render and can't
 *     march the private ISR stack into the data segment (#GP fix). */
int SNDISR_PassThru = 0;
int PTOPS_PumpGuard = 0;
volatile int es_in_render = 0;

/* Post-heal guest-IRQ squelch (formerly sc_tp755's tp_revive_squelch): a
 * backend's clock guardian sets it after resurrecting a dead engine clock;
 * the two gates below then drop the seconds-stale pending completions
 * instead of injecting them into a guest that has long moved on. Stays 0
 * unless a backend with a guardian arms it, so the gates cost nothing
 * elsewhere. */
volatile int SNDISR_ReviveSquelch = 0;

/* ---- engine-generic ISR telemetry + the passthrough ops table ----------
 * The BIOS 0x4F0-0x4FF map (doc/NOTES.md), formerly duplicated per-backend:
 * 0x4F0 = max nesting depth, 0x4F1 = render-guard skips, 0x4F7/0x4F9 =
 * 16-bit SNDISR entry count, 0x4FC/0x4FD = longest outermost pass in
 * 256-TSC-cycle units. rdtsc #UDs on a real 486 -- the fleet this port
 * exists for -- so the duration probe is gated on a one-time CPUID check
 * run at PTOPS_Register time (adetect context), never in the ISR. */
int SNDISR_HasTsc = 0;
static uint16_t dbg_tel_sndisr;
static unsigned char dbg_depth, dbg_maxdepth, dbg_skips;
static unsigned long long dbg_t0;
static unsigned dbg_maxdur;
/* Kept spelled out for DJGPP rather than routed through platform.h/hostsvc.h
 * so this build's object code does not move; the Open Watcom equivalents are
 * platform.h's rdtsc() and hostsvc.c's HOST_HasTsc(). */
#ifdef DJGPP
static unsigned long long dbg_rdtsc(void){ unsigned long long v; __asm__ __volatile__("rdtsc" : "=A"(v)); return v; }
static int dbg_tsc_check(void)
{
    unsigned a, b, d;
    __asm__ __volatile__(
        "pushfl; popl %0; movl %0, %1; xorl $0x200000, %0;"
        "pushl %0; popfl; pushfl; popl %0; pushl %1; popfl"
        : "=&r"(a), "=&r"(b));
    if(!((a ^ b) & 0x200000)) return 0;  /* ID stuck -> no CPUID -> 486-class */
    __asm__ __volatile__("cpuid" : "=d"(d) : "a"(1) : "ebx", "ecx");
    return (d >> 4) & 1;
}
#else
#define dbg_rdtsc()     rdtsc()
#define dbg_tsc_check() HOST_HasTsc()
#endif
void SNDISR_dbg_tick(void)
{
    ++dbg_tel_sndisr;
    TEL_PokeB(0x4F7, (unsigned char)dbg_tel_sndisr);
    TEL_PokeB(0x4F9, (unsigned char)(dbg_tel_sndisr >> 8));
    if(++dbg_depth > dbg_maxdepth){ dbg_maxdepth = dbg_depth; TEL_PokeB(0x4F0, dbg_maxdepth); }

    if(SNDISR_HasTsc && dbg_depth == 1) dbg_t0 = dbg_rdtsc();
}
void SNDISR_dbg_exit(void)
{
    if(SNDISR_HasTsc && dbg_depth == 1){
        unsigned long long dt = dbg_rdtsc() - dbg_t0;
        unsigned u = ((dt >> 8) > 0xFFFFULL) ? 0xFFFFu : (unsigned)(dt >> 8);
        if(u > dbg_maxdur){
            dbg_maxdur = u;
            TEL_PokeB(0x4FC, (unsigned char)u);
            TEL_PokeB(0x4FD, (unsigned char)(u >> 8));

        }
    }
    if(dbg_depth) dbg_depth--;
}
void SNDISR_dbg_reenter(void){ LOW_PokeB(0x4F1, ++dbg_skips); }

/* BLOCKS-PER-TICK CAP -- NOT a diagnostic, despite having been written
 * inside the PTDIAG gate. `pt_space` is sized by the ring LATENCY TARGET
 * (~1.3 KB at SBEPTLAT=60), not by what one tick can drain (~11 bytes at
 * 2048 Hz), so a guest that re-arms inside its own SB ISR lets one tick
 * swallow ~100 twelve-byte blocks -- each an `int 8+irq` round trip plus ~10
 * trapped I/O ops. That is milliseconds inside a 0.49 ms tick, the next ticks
 * nest, and the private ISR stack marches into .data. Measured: it hard-hung
 * the T2130CT. 0 = uncapped (the wedge).
 *
 * It lives OUTSIDE #if PTDIAG so that turning the forensics off -- which
 * ptops.h used to recommend for shipping -- cannot silently ship the wedge.
 * Only the counters and the exit bitmap are diagnostics. */
int SNDISR_PtBlkCap = 8;

#if PTDIAG
/* PT-TAP FORENSICS (short-SFX stretch, 2026-08-21). The open question is why
 * the tap loop stops after ~one guest DMA block per tick: Duke Nukem II's
 * intro SFX arrive as ~12-byte single-cycle blocks, so one-block-per-tick
 * slaves the guest's playback speed to our RTC rate. The loop is NOT written
 * to stop there -- VIRQ_Invoke() runs the guest's SB ISR synchronously (an
 * int 8+irq in sbisr.asm), so a guest that re-arms inside its own ISR would
 * let the for-condition carry straight on to the next block. These two
 * counters say whether it ever does, and what stops it when it doesn't.
 *   0x4F2 = most blocks consumed in ONE tick (1 => never more than one)
 *   0x4FA = bitmap of the loop-exit reasons seen since load */
#define PTD_NOREARM  0x01   /* !VSB_Running(): guest never re-armed in its ISR */
#define PTD_SAMPBND  0x02   /* IdxSm >= samples: PT_MODE_SAMPLES bound hit */
#define PTD_NOSPACE  0x04   /* pt_space spent: ring full = correct backpressure */
#define PTD_PARTIAL  0x08   /* block unfinished: our credit < the guest's block */
#define PTD_MULTI    0x10   /* at least one tick consumed 2+ blocks */
#define PTD_BLKCAP   0x20   /* SNDISR_PtBlkCap stopped the tick */
static unsigned char dbg_pt_maxblk, dbg_pt_exit;
static void dbg_pt_why(unsigned char bit)
{
    if(!(dbg_pt_exit & bit)){ dbg_pt_exit |= bit;
#if !RATEDIAG
        LOW_PokeB(0x4FA, dbg_pt_exit);   /* 0x4FA belongs to RATEDIAG's direct-DAC rate */
#endif
    }
}
#endif

static const struct pt_ops_s pt_ops_default = {
    0,                          /* no tap; no card has claimed the session */
    NULL, NULL, NULL,
    SNDISR_dbg_tick, SNDISR_dbg_exit, SNDISR_dbg_reenter,
    NULL
};
const struct pt_ops_s *PT_Ops = &pt_ops_default;

int PTOPS_CardIs( const char *id )
{
    const char *e = FOpts.card;
    int i;
    if ( !e )
        return 0;                       /* no /CARD: main() already refused */
    for ( i = 0; e[i] && id[i]; i++ ) {
        char a = e[i], b = id[i];
        if ( a >= 'A' && a <= 'Z' ) a += 'a' - 'A';
        if ( b >= 'A' && b <= 'Z' ) b += 'a' - 'A';
        if ( a != b )
            return 0;
    }
    return e[i] == 0 && id[i] == 0;
}

void PTOPS_Register( const struct pt_ops_s *ops )
{
    PT_Ops = ops;
    SNDISR_PassThru = ( ops->flags & PTF_TAP ) ? 1 : 0;
    SNDISR_HasTsc = dbg_tsc_check();    /* adetect context, never ISR */
    /* SBEPTBLK tunes the cap, or disables it at 0. Out of the PTDIAG gate
     * with the cap itself, so a shipping build can still be run uncapped
     * deliberately rather than losing the cap by accident. */
    { const char *e = getenv("SBEPTBLK");
      if(e){ int n = atoi(e); if(n >= 0 && n <= 255) SNDISR_PtBlkCap = n; } }
}

bool _SND_InstallISR( uint8_t, int(*ISR)(void) );
bool _SND_UninstallISR( uint8_t );

#if MUXERROUTINE==2
extern void SNDISR_Mixer( uint16_t *, uint16_t *, uint32_t, uint32_t, uint32_t );
#endif
extern void fatal_error( int );

extern struct globalvars gvars;

struct SNDISR_s {
	int16_t *pPCM;
	uint32_t DMA_linearBase; /* linear start address of current DMA buffer */
	uint32_t DMA_Base;       /* (physical) base address of DMA buffer at last remapping */
	uint32_t DMA_Size;       /* size of DMA buffer at last remapping */
	uint32_t Block_Handle;   /* handle of remapping block */
	uint32_t Block_Addr;     /* linear base of remapping block ( page aligned ) */
#if PT0V86
	uint32_t PageTab0v86;	 /* v1.8: linear address v86 pagetab 0 */
#endif
	void *hAU;
#if SETABSVOL
	uint16_t SB_VOL;
#endif
	uint8_t SndIrq;
#ifdef _LOGBUFFMAX /* log the usage of the PCM buffer? */
	uint32_t dwMaxBytes;
#endif
#ifdef _DEBUG
    int max_samples;
    int total_samples;
    int cntTotal;
    int cntDigital;
#endif
};

static struct SNDISR_s isr = {NULL,-1,0,0};

/* ---- ISR scratch buffer -------------------------------------------------
 * DecodeADPCM and cv_rate each need a temporary the size of one conversion
 * pass, and both used to malloc()/free() it PER CALL in interrupt context.
 * DJGPP's malloc is not reentrant, so that was a latent hazard as much as a
 * cost. One linear block is taken at init instead, with the same uncommitted
 * guard page pPCM gets, so an overrun faults loudly instead of corrupting.
 *
 * The busy flag exists because SETIF=1 lets a second SNDISR nest. The two
 * users are never live at once within a pass -- DecodeADPCM has copied back
 * and released before cv_rate runs -- but a nested pass could ask while the
 * outer one holds the buffer. That case falls back to malloc, i.e. exactly
 * the behaviour being replaced, so the fast path is allocation-free and the
 * slow path is no worse than today. (This also replaces MALLOCSTATIC, whose
 * two branches differed only in where the same temporary came from.) */
static uint8_t *isr_scratch;
static uint32_t isr_scratch_size;
static volatile int isr_scratch_busy;

static void *ISR_ScratchGet( uint32_t need, int *owned )
{
    if ( !isr_scratch_busy && isr_scratch && need <= isr_scratch_size ) {
        isr_scratch_busy = 1;
        *owned = 1;
        return isr_scratch;
    }
    *owned = 0;
    return malloc( need );
}

static void ISR_ScratchPut( void *p, int owned )
{
    if ( owned )
        isr_scratch_busy = 0;
    else
        free( p );
}

#if SLOWDOWN

static void delay_10us(unsigned int ticks)
//////////////////////////////////////////
{
	static uint64_t oldtsc = 0;
	uint64_t newtsc;

	/* RDTSC is an invalid opcode on a real 486 -- the fleet this port
	 * exists for -- and this runs in ISR context, so an ungated /SD was a
	 * #UD wedge on every non-Pentium box. Ignore the slowdown there
	 * rather than fault (main.c says so at startup). */
	if ( !SNDISR_HasTsc )
		return;

	do {
		newtsc = rdtsc();
	} while ( (newtsc - oldtsc) < ( ticks << 18 ) );
	oldtsc = newtsc;
}
#endif

/* rate conversion.
 * src & dst are 16-bit, channels is either 1 or 2; if it's 2, nSamples is even!
 * out: new sample cnt.
 *
 * example: 16 samples, 1 channel, srcrate=11025, dstrate=44100:
 * 1. instep = (0 << 12) | ((4096 * ( 11025 % 44100 ) / 44100 + 1) & 0xfff)
 *           = (( 4096 * 11025 ) / 44100 + 1) & 0xfff
 *           = ( 45.158.400 / 44100 + 1) & 0xfff
 *           = 1025 & 0xfff -> 1025
 * 2. loops: 65536 / 1025 = 63
 *
 */

static unsigned int cv_rate( PCM_CV_TYPE_S *pcmsrc, const unsigned int nSamples, const unsigned int channels, unsigned int srcrate, unsigned int dstrate)
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
{
	/* v2.0: new instep calculation seems a bit more intuitive */
	//const unsigned int instep = ((srcrate / dstrate) << 12) | (((((srcrate % dstrate) << 12 ) + dstrate - 1 ) / dstrate) & 0xFFF);
	const unsigned int instep = ((srcrate / dstrate) << 12) | ((((srcrate % dstrate) << 12 ) / dstrate + 1 ) & 0xFFF);

	const unsigned int inend = (nSamples >> (channels - 1)) << 12;
	PCM_CV_TYPE_S *pcmdst;
#ifdef _DEBUG
	unsigned int idx;
#endif
	//unsigned int inpos = (srcrate < dstrate) ? (instep >> 1) : 0;
	unsigned int inpos = 0;
	PCM_CV_TYPE_S* buff;
	int buffowned;

	//if(!nSamples)
	//	return 0;

	buff = (PCM_CV_TYPE_S*)ISR_ScratchGet(
	           (uint32_t)((nSamples+2) * sizeof(PCM_CV_TYPE_S)), &buffowned );
	if ( !buff )
		return 0;   /* was an unchecked deref; 0 leaves the block unconverted */
	memcpy( buff, pcmsrc, (nSamples+2) * sizeof(PCM_CV_TYPE_S) );

	pcmdst = pcmsrc;

    /* v2.0: one additional sample is now supplied, so the last sample won't
     *       need special treatment ( variable total removed ).
     */

	for ( inpos = 0; inpos < inend; inpos += instep ) {
		unsigned int m1,m2;
#ifndef _DEBUG
		unsigned int idx;
#endif
		PCM_CV_TYPE_S *incurr,*innext;

		idx = (inpos >> 12 ) << ( channels - 1);
		m2 = inpos & 0xFFF;
		m1 = 4096 - m2;
		incurr = buff + idx;
		innext = buff + idx + channels;
		*pcmdst++ = ( *incurr * m1 + *innext * m2 ) >> 12;
		if ( channels > 1 )
			*pcmdst++ = ( *(incurr+1) * m1 + *(innext+1) * m2 ) >> 12;
	}

#ifdef SNDISRLOG
	dbgprintf(("cv_rate(smpl=%u, chn=%u) in step/end=%u/%u idx=%u new smpl=%u\n", nSamples, channels, instep, inend, idx, (pcmdst - pcmsrc) >> ( channels - 1) ));
#endif

	ISR_ScratchPut( buff, buffowned );
    //return ( pcmdst - pcmsrc ); /* v2.0: shift added to return "true" sample count */
	return ( (pcmdst - pcmsrc) >> ( channels - 1 ) );
}

/* convert 8-bits signed/unsigned to 16-bits signed. */

static void cv_bits_8_to_16( PCM_CV_TYPE_S *pcm, unsigned int nSamples, uint8_t issigned )
//////////////////////////////////////////////////////////////////////////////////////////
{
	PCM_CV_TYPE_UC *srcu;
	PCM_CV_TYPE_SC *srcs;
	PCM_CV_TYPE_S *dst = pcm + nSamples - 1;

    if ( issigned ) {
        srcs = (PCM_CV_TYPE_SC *)pcm + nSamples - 1;
        for ( ; nSamples; nSamples-- )
            *dst-- = (PCM_CV_TYPE_S)((*srcs--) << 8);
    } else {
        srcu = (PCM_CV_TYPE_UC *)pcm + nSamples - 1;
        for ( ; nSamples; nSamples-- )
            *dst-- = (PCM_CV_TYPE_S)((*srcu-- ^ 0x80) << 8);
    }
}

/* convert mono to stereo. */

#if 1
static void cv_channels_1_to_2( PCM_CV_TYPE_S *pcm_sample, unsigned int nSamples )
//////////////////////////////////////////////////////////////////////////////////
{
    PCM_CV_TYPE_S *src = pcm_sample + nSamples - 1;
    PCM_CV_TYPE_S *dst = pcm_sample + nSamples * 2 - 1;

    for( ; nSamples; nSamples-- ) {
        *dst-- = *src; *dst-- = *src--;
    }
    return;
}
#else
extern void cv_channels_1_to_2( PCM_CV_TYPE_S *pcm_sample, unsigned int nSamples );
#endif

#if DACRING && !defined(NOES1688)
/* DIRECT DAC ON A PASSTHROUGH CARD (DSP cmd 10h, from the ring of jlmshare.h).
 * A direct-DAC guest writes one sample per tick of a timer of its own and
 * states no rate anywhere. The render path below derives one per tick as
 * samples x codec rate / frames requested, which holds only while the engine
 * requests one tick of output; on a passthrough card it requests
 * PT_MODE_SAMPLES whatever the card needs, and the estimate came out about 12x
 * low on the IBM card (128 Hz pump, ~86 frames a tick). So with a card clock
 * (PT_Ops->clock, 1/32768 s) the rate is measured instead, as samples
 * received per unit of it: in 1/16 s windows, folded into a running total
 * that remembers about 16 s. The first tick that brings samples after a
 * pause is partial, so it only starts the count; a gap under 1/8 s is part
 * of the stream, a longer one discards the window and waits for the next
 * first tick. A guest that writes in bursts (PTBYPASS: one BIOS tick's worth
 * at a time) puts one or two bursts in a window, so single windows are not
 * judged: each second of windows is compared with the estimate instead, and
 * one more than 1/8 off means the guest changed rate, so the estimate
 * restarts from that second. Half a second without a sample ends the stream.
 * The tap is fed at the estimate, trimmed by the fill (below), as much as
 * the card has room for. Until the first window is done, samples wait in
 * the ring. */
#define DAC_WIN    2048UL           /* a window: 1/16 s */
#define DAC_GAP    4096UL           /* 1/8 s without a sample: a pause */
#define DAC_QUIET  16384UL          /* 1/2 s without a sample: stream over */
#define DAC_CHECK  32768UL          /* a second of windows: rate changed? */
#define DAC_KEEP   (32768UL * 16)   /* running total: halved past ~16 s */
#define DAC_TAKE   1024             /* samples handed to the tap per tick */

static struct {
    unsigned long clk;              /* the clock at the last look */
    uint32_t cnt;                   /* the ring's dcount at the last look */
    unsigned long wt, st, tt;       /* time: window, second, running total */
    uint32_t ws, ss, ts;            /* samples: the same three */
    unsigned long quiet;            /* time since the last sample */
    unsigned rate;                  /* the estimate, Hz; 0 = none yet */
    unsigned fed;                   /* the rate the tap was last fed at */
    long trimi;                     /* the fill trim's integral, 1/65536 */
    unsigned livet;                 /* ticks with samples since the stream began */
    uint8_t live;                   /* the last look brought samples */
    uint8_t gap;                    /* a look since the last samples found none */
} dac;
static uint8_t dac_buf[DAC_TAKE];

static void SNDISR_DacFeed( void )
//////////////////////////////////
{
    struct vsbj_share *s = PTRAP_Share();
    unsigned long clk = PT_Ops->clock(), dt = clk - dac.clk;
    uint32_t cnt = s->dcount, ds = cnt - dac.cnt;
    uint16_t tail;
    int n, i, room, resume;

    dac.clk = clk;
    dac.cnt = cnt;
    s->dcalls++;
    if ( dac.live && dt > s->dmaxdt )
        s->dmaxdt = dt;                 /* a starved tick shows here */
    if ( !ds ) {
        dac.gap = 1;
        if ( dac.quiet < DAC_QUIET && ( dac.quiet += dt ) >= DAC_QUIET ) {
            dac.rate = 0;               /* the stream is over */
            dac.tt = dac.ts = dac.st = dac.ss = 0;
            dac.trimi = 0;
            dac.livet = 0;
            s->dflush += ( s->dhead - s->dtail ) & VSBJ_RMASK;
            s->dtail = s->dhead;        /* anything left never got a rate */
        }
        if ( dac.quiet >= DAC_GAP ) {
            dac.live = 0;               /* a pause: drop the window */
            dac.wt = dac.ws = 0;
        } else if ( dac.live ) {
            dac.wt += dt;               /* a gap inside a bursty stream */
            s->dgaps++;
        }
    } else {
        dac.quiet = 0;
        if ( !dac.live )
            dac.live = 1;               /* partial tick: it only syncs */
        else {
            dac.wt += dt;
            dac.ws += ds;
            if ( dac.wt >= DAC_WIN ) {
                if ( !dac.rate )
                    s->dstreams++;
                dac.tt += dac.wt;
                dac.ts += dac.ws;
                dac.st += dac.wt;
                dac.ss += dac.ws;
                dac.wt = dac.ws = 0;
                if ( dac.tt > DAC_KEEP ) {
                    dac.tt >>= 1;
                    dac.ts >>= 1;
                }
                if ( dac.st >= DAC_CHECK ) {
                    unsigned sec = (unsigned)( ( (unsigned long long)dac.ss << 15 ) / dac.st );
                    unsigned diff = sec > dac.rate ? sec - dac.rate : dac.rate - sec;
                    if ( !s->secmin || sec < s->secmin )
                        s->secmin = (uint16_t)sec;
                    if ( sec > s->secmax )
                        s->secmax = (uint16_t)sec;
                    if ( diff > dac.rate / 8 ) {
                        dac.tt = dac.st;    /* the guest changed rate */
                        dac.ts = dac.ss;
                        s->drestart++;
                    }
                    dac.st = dac.ss = 0;
                }
                dac.rate = (unsigned)( ( (unsigned long long)dac.ts << 15 ) / dac.tt );
            }
        }
    }

    tail = s->dtail;
    n = ( s->dhead - tail ) & VSBJ_RMASK;
    if ( n > s->dmaxocc )
        s->dmaxocc = (uint16_t)n;
    if ( !n )
        return;
    if ( !dac.rate ) {
        s->dnorate++;
        return;
    }
    room = PT_Ops->space();             /* guest bytes; 8-bit mono = samples */
    /* AFTER A GAP. A look that found no samples was the guest's DAC standing
     * still (a real SB holds its last value) while the card played on from
     * its queue, so the samples after it find the card short by the gap. The
     * trim below would win that back by playing the music slow. Instead a
     * shortfall over 1/48 s is filled with the first new sample, held: the
     * guest's pause, played as a pause. */
    resume = 0;
    if ( ds && dac.gap ) {
        dac.gap = 0;
        resume = room - n > (int)( dac.rate / 48 );
    }
    if ( ds && dac.live && !resume && room - n > (int)s->dmaxroom )
        s->dmaxroom = (uint16_t)( room - n );   /* the card short of its target */
    /* THE FILL TRIM. The estimate above is a long-term average, and a guest
     * that writes from its own timer interrupt slows down when it runs short
     * of CPU: its ticks merge, and Another World's rate sags 4% in a heavy
     * second (9516..10170 Hz against 9890). Fed the average, the card then
     * plays faster than the samples come, its queue runs down, and the pump
     * pads silence into the music: crackle. So the rate is trimmed on each
     * tick that brought samples, from the fill: e = samples waiting minus the
     * card's room below its target, + backlog, - shortfall, in units of
     * 1/12 s of samples. Directly 0.3 of a shortfall, which is an underrun
     * coming, but 0.05 of a backlog, which is only latency: a stream starts
     * with the 1/16 s its first estimate took waiting in the ring, and 0.3 of
     * that put the music 12% sharp, past the codec rate. Plus an integral of
     * 1/1024 of e a tick that brings the queue back to its target, within
     * 12%. The stepper takes each change without a click (sc_ibmaud.c). Not
     * in the first half second of a stream, while the card fills from
     * nothing. The integral stands still while the direct part alone is at
     * the limit: a shortfall that big is an event, not a rate, and must not
     * wind it up. */
    {
        long trim = 0;
        if ( ds && dac.live && !resume ) {
            long unit = dac.rate / 12 ? (long)( dac.rate / 12 ) : 1;
            long e = (long)n - room;
            if ( dac.livet < 64 )
                dac.livet++;
            else {
                long p = e * ( e < 0 ? 19661L : 3277L ) / unit;
                if ( p > -7864 && p < 7864 ) {
                    dac.trimi += e * 64 / unit;
                    if ( dac.trimi > 7864 ) dac.trimi = 7864;
                    if ( dac.trimi < -7864 ) dac.trimi = -7864;
                }
                trim = p + dac.trimi;
                if ( trim > 7864 ) trim = 7864;
                if ( trim < -7864 ) trim = -7864;
            }
        } else
            trim = dac.trimi;           /* between samples, hold the integral */
        dac.fed = (unsigned)( (long)dac.rate + (long)dac.rate * trim / 65536 );
        s->dacrate = (uint16_t)dac.fed;
        s->dtrim = (int16_t)( (long)dac.rate * trim / 65536 );
        if ( s->dtrim < s->dtrimmin )
            s->dtrimmin = s->dtrim;
        if ( s->dtrim > s->dtrimmax )
            s->dtrimmax = s->dtrim;
    }
    if ( resume ) {
        int hold = room - n;
        if ( hold > DAC_TAKE )
            hold = DAC_TAKE;
        memset( dac_buf, s->ring[tail], (size_t)hold );
        PT_Ops->feed( dac_buf, hold, dac.fed, 8, 1 );
        room -= hold;
        s->dholds++;
    }
    if ( n > room ) {
        n = room;
        s->dspace++;
    }
    if ( n > DAC_TAKE )
        n = DAC_TAKE;
    if ( n <= 0 )
        return;
    for ( i = 0; i < n; i++ ) {
        dac_buf[i] = s->ring[tail];
        tail = (uint16_t)( ( tail + 1 ) & VSBJ_RMASK );
    }
    s->dtail = tail;
    s->dfed += n;
    PT_Ops->feed( dac_buf, n, dac.fed, 8, 1 );
}
#endif

static int SNDISR_Interrupt( void )
///////////////////////////////////
{
    uint32_t mastervol;
    uint32_t voicevol;
    uint32_t midivol;
#if VOICELR
    uint32_t mastervol2;
    uint32_t voicevol2;
#endif
    int16_t* pPCMOPL;
    uint32_t freq;
    int nSamples; /* # of samples requested by sound hardware */
    int IdxSm; /* sample index in 16bit PCM buffer */
    int i;
#if COMPAT4
    uint16_t mask;
#endif
#ifdef _DEBUG
    int loop;
#endif
#ifndef NOES1688
    /* ES1688 passthrough plumbing. pt_mode: our card is the AU output and the
     * tap is armed. pt_space: raw bytes the driver ring still accepts this
     * tick -- the REAL pacing governor: AU_cardbuf_space() is meaningless for
     * the PT ring (our driver never advances card_dmalastput, so its result is
     * just a ring_rd sawtooth that can pin at 0 and starve the loop). pt_took:
     * at least one block went to the card raw this tick -> the whole render
     * tail (conversions/mixer/AU_writedata) is dead weight and is skipped;
     * that tail is what made SNDISR outlast the RTC period and caused the
     * runaway re-entry that marched the ISR stack into .data (#GP). */
    int pt_mode = 0, pt_took = 0;
    int pt_space = 0;
    int pt_blocks = 0;      /* the block cap reads this, PTDIAG or not */
#if PTDIAG
    int pt_brk = 0;
#endif
#endif

#ifndef NOES1688
    PT_Ops->dbg_tick();   /* DIAG: entry count (0x4F7) + nesting depth (0x4F0) */
#endif
#if DACRING
    PTRAP_Share()->tick++;  /* VSBPCMJ's pump guard: we are being called */
#endif

    /* check if the sound hw does request an interrupt. */
    if( !AU_isirq( isr.hAU ) ) {
#ifndef NOES1688
        PT_Ops->dbg_exit();
#endif
        return(0);
    }

#ifndef NOES1688
    /* Render-rate divider (ptops.h). AU_isirq above has already run the
     * card's irq_routine -- which is where a pump card feeds its FIFO -- so
     * bailing here keeps the feed at full rate and only thins out the heavy
     * render path. */
    if ( PT_Ops->render_div > 1 ) {
        /* Countdown, not a modulo. render_div is loaded from the ops table,
         * so the compiler cannot strength-reduce the % into a mask even
         * though every value we ship is a power of two -- it emitted a
         * 32-bit DIV (~40 cycles on a 486) on EVERY ISR entry purely to
         * decide whether to render. Same 1-in-N rate, different phase
         * (renders on the first tick rather than the Nth), which nothing
         * depends on. */
        static unsigned rdiv_cnt;
        if ( rdiv_cnt ) {
            rdiv_cnt--;
            goto isrexit;
        }
        rdiv_cnt = (unsigned)PT_Ops->render_div - 1;
    }
#endif

#if COMPAT4
    /* v1.8: /CF4 */
    if ( gvars.compatflags & CF_MASKPIT ) {
        mask = PIC_GetIRQMask();
        PIC_SetIRQMask(mask | 1);
    }
#endif
    /* DEPTH LIMITER (MI1 nesting fossil, 2026-08-14 pt.2): SETIF re-enables
     * interrupts below, and an IRQ edge landing in the post-render exit
     * window (es_in_render already 0, EOI pending) nests a fresh SNDISR
     * frame -- each one carves STACKCORR (4KB) off the private ISR stack.
     * Trap-tax-stretched passes let this resonate to depth 14 = 56KB gone.
     * Three live frames is already pathological: ack + EOI and get out.
     * Runtime-dispatched: only a backend with a live nesting instrument
     * (sc_tp755's IAC window) supplies depth. */
    if ( PT_Ops->depth && PT_Ops->depth() > 3 ) goto isrexit;
#ifdef CARD_TP755
    /* apply the tick's buffered guest OPL writes before rendering them */
    { extern void PTRAP_DrainOplRing(void);
      PTRAP_DrainOplRing(); }
#endif
#if DACRING && defined(VSBJ_FMRING)
    /* and the ones VSBPCMJ took from a V86 guest, in its FM ring */
    PTRAP_DrainJlmFm();
#endif
    /* since the client context is now restored when a SB IRQ is emulated,
     * it's safe to call VIRQ_Invoke here. This will happen only for
     * DSP cmds 0xF2/0xF3 (trigger IRQ).
     * Todo: check if SB emulated Irq is masked; if yes, don't trigger!
     */
    /* VERIFY-BEFORE-REVIVE (MI1 crash lesson, 2026-08-14): after a backend's
     * clock guardian resurrects a dead engine clock, the guest's SB driver
     * state is seconds stale -- injecting the PRE-freeze completion (or the
     * first fresh ones) crashed a game that had survived the freeze itself.
     * For the first squelched ticks after a heal, drop pending status
     * instead. SNDISR_ReviveSquelch stays 0 on guardian-less backends. */
    if ( SNDISR_ReviveSquelch ) {
        SNDISR_ReviveSquelch--;
        VSB_ClearIRQStatus();
    } else if ( VSB_GetIRQStatus() )
        VIRQ_Invoke();

#if SETIF
    _enable_ints();
#endif

#ifndef NOES1688
#define PT_MODE_SAMPLES 1024
    if ( SNDISR_PassThru ) {
        /* PT mode: pace by ring space instead (see decl comment). nSamples
         * becomes a plain loop bound; keeping it small also caps the mixer /
         * direct-DAC tails so a non-PT tick stays cheap. 1024 >> any real
         * per-tick need (worst sustained stream ~350 guest bytes/tick at the
         * idle pump rate).
         * AU_cardbuf_space() is NOT called on this path: its result was
         * overwritten right here on every tick -- a getpos call and two
         * 32-bit divides for nothing -- and the only consumer of its side
         * effects on a PT tick is the direct-DAC tail, which now calls it
         * itself. */
        pt_mode = 1;
        pt_space = PT_Ops->space();
        nSamples = PT_MODE_SAMPLES;
    } else
#endif
    {
        //AU_setoutbytes( isr.hAU ); //v1.9: now obsolete
        nSamples = AU_cardbuf_space( isr.hAU ) / ( sizeof(int16_t) * 2 ); //16 bit, 2 channels
#ifndef NOES1688
        /* keep one render pass inside one pump tick (see render_cap in ptops.h) */
        if ( PT_Ops->render_cap && nSamples > PT_Ops->render_cap )
            nSamples = PT_Ops->render_cap;
#endif
    }

    if ( !nSamples ) { /* no free space in DMA buffer? Shouldn't happen... */
        dbgprintf(("isr: ERROR - AU_cardbuf_space() returned 0 samples\n" ));
        goto isrexit;
    }
    freq = AU_getfreq( isr.hAU );

#ifndef NOES1688
    /* RENDER-REENTRANCY GUARD (safety net): SETIF=1 enabled interrupts above,
     * so while the outer SNDISR is in the render+tap path the next RTC tick
     * can re-enter SNDISR. Nested SwitchStackISR carvings each take STACKCORR
     * (4KB, flat build) off the private ISR stack; runaway nesting marched it
     * into .data and #GP'd (hardware test 3). With the PT-mode speedups the
     * ISR should now always finish inside one RTC period, so this path should
     * be RARE -- 0x4F1 counts it. The re-entrant path still EOIs via isrexit:
     * Build A empirically ran full nested passes WITH EOI for 52 feeds, so
     * EOI-per-delivered-tick is known-compatible with this PIC stack.
     * Do NOT clear the flag on the re-entrant path -- only the owner clears it. */
    { if(es_in_render) { PT_Ops->dbg_reenter(); goto isrexit; }
      es_in_render = 1; }
#endif

#ifdef _DEBUG
    if ( nSamples > isr.max_samples )
        isr.max_samples = nSamples;
    isr.total_samples += nSamples;
    isr.cntTotal++;
    //dbgprintf(("isr: samples:%u ",nSamples));
    loop = 0;
    for ( IdxSm = 0, isr.cntDigital++; VSB_Running() && IdxSm < nSamples; loop++ ) {
        int ocnt;
#else
    for ( IdxSm = 0; VSB_Running() && IdxSm < nSamples; ) {
#endif
        /* a loop that may run 2 (or multiple) times if a SB buffer overrun occured */
        int i,j;
        int dmachannel = VSB_GetDMA();
        int bytes; /* no of bytes to be copied from SB DMA buffer */
        int bits = VSB_GetBits();
        int channels = VSB_GetChannels();
        int samplesize = ( bits + 7 ) >> 3;
        int count = nSamples - IdxSm; /* samples to handle in this turn */
        int sbcnt;
        bool resample;
        uint32_t DMA_Base;
        uint32_t DMA_Index;
        int32_t DMA_Count;
        uint32_t SB_BuffSpace = VSB_GetBuffSpace(); /* remaining buffer size in bytes */
        uint32_t SB_Rate = VSB_GetSampleRate();
        int IsSilent = VSB_IsSilent();
#ifndef NOES1688
        /* ADPCM (<8 bits) can't pass through raw -- those blocks take the
         * full render path (decode+convert+writedata) even in PT mode. */
        int pt_block = pt_mode && VSB_GetBits() >= 8;
#endif

        /* (0x4F4 is the RTC-revival counter now; the loop-entered diag is retired) */

        if ( !IsSilent ) {
            DMA_Base = VDMA_GetBase(dmachannel);
            DMA_Index = VDMA_GetIndex(dmachannel);
            DMA_Count = VDMA_GetCount(dmachannel);
            /* check if the current DMA buffer is within the mapped region. */
#if PT0V86
            /* v1.8: if access to v86 pagetab 0 is installed, translate upper memory address
             * to physical address; this is needed because hdpmi is a VCPI client, hence has
             * no knowledge of the current v86 mappings.
             */
            if ( DMA_Base < 0x100000 && DMA_Base >= 0xA0000 && isr.PageTab0v86 ) {
#ifdef _DEBUG
                uint32_t tmp = DMA_Base;
#endif
                DMA_Base = (*((uint32_t *)NearPtr(isr.PageTab0v86) + (DMA_Base >> 12 )) & ~0xfff) | (DMA_Base & 0xFFF);
                dbgprintf(("isr(%u), conv address %X -> phys address %X [pgtab0=%X]\n", loop, tmp, DMA_Base, isr.PageTab0v86 ));
            }
#endif
            if( !(DMA_Base >= isr.DMA_Base && (DMA_Base + DMA_Index + DMA_Count) <= (isr.DMA_Base + isr.DMA_Size) )) {
                isr.DMA_linearBase = -1;
            }
            /* if there's no mapped region, create one that covers current DMA op. */
            if( isr.DMA_linearBase == -1 ) {
                isr.DMA_Base = DMA_Base;
                isr.DMA_Size = min( max(DMA_Index + DMA_Count, 0x4000 ), 0x20000 );
                if ( DMA_Base < 0x100000 ) {
                    isr.DMA_linearBase = DMA_Base;
                } else {
                    /* size is in pages, phys. address must have bits 0-11 cleared */
                    if( __dpmi_map_physical_device(isr.Block_Handle, 0, (isr.DMA_Size + (isr.DMA_Base & 0xfff) + 4095 ) >> 12 , isr.DMA_Base & ~0xfff ) == -1 )
                        fatal_error( 2 );
                    isr.DMA_linearBase = isr.Block_Addr | (isr.DMA_Base & 0xFFF);
                }
                dbgprintf(("isr(%u), ISR_DMA address (re)mapped: isr.DMA_Base(%d)=%x, isr.DMA_Size=%x, isr.DMA_linearBase=%x\n",
                           loop, dmachannel, isr.DMA_Base, isr.DMA_Size, isr.DMA_linearBase ));
            }
        }
        /* don't resample if sample rates are close? */
#ifndef NOES1688
        if ( pt_block )
            /* PT: the card plays the guest's own rate and cv_rate never runs,
             * so the resampled-count arithmetic below (two or three 32-bit
             * divides per block, per tick) was dead weight -- count is a loop
             * bound here that pt_space and the block cap govern. It stays in
             * guest frames, as the PT comments further down already assume;
             * the one visible difference is that a tick with lots of ring
             * room may take up to PT_MODE_SAMPLES guest frames instead of
             * PT_MODE_SAMPLES * SB_Rate / freq. */
            resample = false;
        else
#endif
        if( SB_Rate != freq ) {

            int tmpcnt = count * SB_Rate / freq;
            resample = true;
            //count = max( channels, count / ( ( freq + SB_Rate-1) / SB_Rate ));
            /* v2.0: fixed: operands for modulus op were wrong - count was ALWAYS increased,
             * even if freq was an exact multiple of SB_Rate.
             */
            //if ( SB_Rate < freq && SB_Rate % freq ) count++;
            /* in Quake, count = 0 seems to occure?  */
            //if ( SB_Rate < freq && freq % SB_Rate ) count++;
            //if ( ( SB_Rate < freq && freq % SB_Rate ) || !count ) count++;
            /* v2.0: even if freq is an exact multiple of SB_Rate, the division
             * may have given a too small value of count!
             */
            while ( count > ( tmpcnt * freq / SB_Rate ) )
                tmpcnt++;
            count = tmpcnt;
        } else
            resample = false;
#ifdef _DEBUG
        ocnt = count;
        //dbgprintf(("isr(%u): c=0x%02X ocnt=0x%02X\n", loop, count, ocnt ));
#endif
#if ADPCM
        if( bits < 8 ) { /* ADPCM? */
            sbcnt = SB_BuffSpace - adpcm_state.useRef;
            //count += count % ( 6 - bits );
            count = min( count, sbcnt * (6 - bits) );
            bytes = (count+(6 - bits)-1) / (6 - bits) + adpcm_state.useRef;
# ifdef SNDISRLOG
            dbgprintf(("isr(%u): ADPCM bits=%u bytes=%u samples=%u count=%u SB BuffSpace=%u\n", loop, bits, bytes, nSamples, count, SB_BuffSpace ));
# endif
        } else
#endif
        {
            /* samplesize and channels can be either 1 or 2 */
            sbcnt = SB_BuffSpace / (samplesize * channels);
            /* v2.0: ensure that count hasn't become < samples - that would distort sound */
            if ( SB_BuffSpace % (samplesize * channels) )
                sbcnt++;

            count = min( count, max(1, sbcnt));
            bytes = count * samplesize * channels;
        }

#ifndef NOES1688
        /* PT pacing: consume only what the driver ring accepts this tick.
         * When the ring is at its latency target, STOP consuming guest DMA --
         * exactly how a real SB throttles the guest: the DMA position simply
         * doesn't advance until the card has played some of it. Also bounds
         * the per-tick ISR work (part of the keep-it-inside-one-RTC-period
         * reentrancy fix). */
        if ( pt_block ) {
            if ( pt_space < samplesize * channels ) {
#if PTDIAG
                pt_brk = 1; dbg_pt_why( PTD_NOSPACE );
#endif
                break;
            }
            if ( bytes > pt_space ) {
                count = pt_space / (samplesize * channels);
                bytes = count * samplesize * channels;
            }
            pt_space -= bytes;
            pt_took = 1;
            if ( pt_blocks < 255 ) pt_blocks++;
        }
#endif

        /* copy samples to our PCM buffer */
        if( IsSilent ) {
#ifndef NOES1688
            if ( !pt_block )   /* PT: nothing downstream reads pPCM */
#endif
            memset( isr.pPCM + IdxSm * 2, 0, bytes + 1 ); /* v2.0: one extra byte for resampling */
        } else {
            char *pDest = (char *)(isr.pPCM + IdxSm * 2);
            if ( DMA_Count < bytes ) {
                /* v2.0: DMA buffer underrun handled here now; this approach avoids
                 *       multiple format conversions if DMA buffer size is small.
                 */
                int chunk;
                int tmpbytes;
#ifdef SNDISRLOG
                dbgprintf(("isr(%u): DMA space < bytes (0x%X) samples=0x%X DMA Idx/Cnt=0x%X/0x%X\n", loop, bytes, nSamples, DMA_Index, DMA_Count ));
#endif
                if ( !VDMA_IsAuto(dmachannel) ) {
                    count = DMA_Count / (samplesize * channels );
                    bytes = DMA_Count;
                }
                for ( tmpbytes = 0; tmpbytes < bytes; tmpbytes += chunk ) {
                    chunk = min( DMA_Count, bytes - tmpbytes );
                    memcpy( pDest + tmpbytes, NearPtr(isr.DMA_linearBase + ( DMA_Base - isr.DMA_Base) + DMA_Index ), chunk );
                    DMA_Index = VDMA_SetIndexCount(dmachannel, DMA_Index + chunk, DMA_Count - chunk );
                    DMA_Count = VDMA_GetCount(dmachannel);
#ifdef SNDISRLOG
                    dbgprintf(("isr(%u): chunk=%X tmpbytes=%X DMA Idx/Cnt=0x%X/0x%X\n", loop, chunk, tmpbytes, DMA_Index, DMA_Count ));
#endif
                }
            } else {
                memcpy( pDest, NearPtr(isr.DMA_linearBase + ( DMA_Base - isr.DMA_Base) + DMA_Index ), bytes );
                DMA_Index = VDMA_SetIndexCount(dmachannel, DMA_Index + bytes, DMA_Count - bytes);
#ifdef SNDISRLOG /* v1.8: needed for debug logs only */
                DMA_Count = VDMA_GetCount( dmachannel );
#endif
            }
#ifndef NOES1688
            /* ES1688 PASSTHROUGH TAP: feed the RAW guest PCM straight to the card
             * HERE -- before the cv_bits/cv_rate/cv_channels conversions below --
             * so nothing is resampled (the ES1688 is a real SB codec and plays the
             * guest's native rate/format directly). Both DMA read paths above have
             * filled pDest[0..bytes) contiguously. bytes was already capped to the
             * ring's free space above, so PT_Feed never overruns the ring. */
            /* (0x4F5 is the ring-fill probe now; the reached-tap diag is retired) */
            if ( pt_block )
                PT_Ops->feed( (const unsigned char *)pDest, bytes, SB_Rate, VSB_GetBits(), channels );
#endif
            /* v2.0: copy 1 more sample for cv_rate() */
            if ( resample
#ifndef NOES1688
                 && !pt_block   /* PT: cv_rate below is skipped */
#endif
               ) {
                /* copy the next sample is the best strategy, but
                 * may be a problem if SB buffer is at its end
                 * ( especially if DSP cmd is single-cycle only );
                 * in that case, just copy the last sample!
                 * ADPCM is special, it's handled inside DecodeADPCM().
                 */
                memcpy( pDest + bytes,
                       ( bytes == SB_BuffSpace ) ?
                       pDest + bytes - samplesize * channels :
                       NearPtr(isr.DMA_linearBase + ( DMA_Base - isr.DMA_Base) + DMA_Index ),
                       samplesize * channels );
            }
        }

        /* update DSP regs */
        VSB_ReduceBuffSpace( bytes ); /* will set mixer IRQ status if space becomes <= 0 */

        /* format conversion needed? (PT: the card already played the raw
         * bytes; all conversion below is dead weight and is skipped. count
         * then stays in guest samples, which is fine -- on the PT path IdxSm
         * is only a loop bound.) */
#ifndef NOES1688
        if ( !pt_block ) {
#endif
#if ADPCM
        if( bits < 8 )
            count = DecodeADPCM((uint8_t*)(isr.pPCM + IdxSm * 2), bytes - adpcm_state.useRef, bits );
#endif
        if( samplesize != 2 ) {
#ifdef _DEBUG
# if LOGPCM8DATA
            if ( gvars.logmode == 1 ) {
                unsigned char *tmp = (unsigned char *)isr.pPCM + IdxSm * 2;
                for ( i = 0; i < count; i++, tmp++ )
                    writepcm8data(*tmp);
            }
# endif
#endif
            cv_bits_8_to_16( isr.pPCM + IdxSm * 2, (count+1) * channels, VSB_IsSigned() ); /* converts unsigned 8-bit to signed 16-bit */
        }
#if SUP16BITUNSIGNED
        else if ( !VSB_IsSigned() )
            for ( i = IdxSm * 2, j = i + (count+1) * channels; i < j; *(isr.pPCM+i) ^= 0x8000, i++ );
#endif
        if( resample ) /* SB_Rate != freq? */
            count = cv_rate( isr.pPCM + IdxSm * 2, count * channels, channels, SB_Rate, freq );

#ifdef _DEBUG
# if LOGPCM16DATA /* log 16-bit PCM data; file logfile.asm (HDLFUNC!) must also be changed! */
        if ( gvars.logmode == 2 ) {
            short *tmp = isr.pPCM + IdxSm * 2;
            for ( i = 0; i < count; i++, tmp++ )
                writepcm16data(*tmp);
        }
# endif
#endif

        if( channels == 1) //should be the last step
            cv_channels_1_to_2( isr.pPCM + IdxSm * 2, count);
        else if ( samplesize == 1 ) {
            /* SB Pro stereo quirk: on real hardware the FIRST byte of a stereo
             * frame comes out the RIGHT channel, and games pre-compensate for
             * that -- so rendering the guest stream straight as [L,R] gives a
             * mirrored image (Duke3D with its own "reverse stereo" set OFF).
             * The ES1688 passthrough already handles this in es_fifo_pump()
             * (swap2); software-rendered cards need the same flip here.
             * Gated to 8-bit stereo, i.e. SB Pro mode -- same condition the
             * passthrough uses (es_pt_bits < 16); SB16 16-bit stereo has no
             * such quirk. Passthrough never reaches this code (pt_block). */
            PCM_CV_TYPE_S *sw = isr.pPCM + IdxSm * 2;
            int n;
            for ( n = count; n; n--, sw += 2 ) {
                PCM_CV_TYPE_S t = sw[0]; sw[0] = sw[1]; sw[1] = t;
            }
        }
#ifndef NOES1688
        }
#endif

        IdxSm += count;

        if( VSB_GetIRQStatus() ) {
#ifdef SNDISRLOG
            dbgprintf(("isr(%u): s/c/b=0x%02X/0x%02X/0x%03X SB BufSpace=%u DMA Idx/Cnt=%X/%X\n", loop, nSamples, count, bytes, SB_BuffSpace, DMA_Index, DMA_Count ));
#endif
            if ( VSB_IsAuto() ) {
                VSB_ResetBuffSpace();
            } else
                VSB_Stop(); /* v1.8: does no longer reset SB position */
            /* revival squelch: skip the injection, keep the bookkeeping;
             * the pending status is delivered late (or dropped) by the
             * top-of-tick gate once the squelch window has passed */
            if ( !SNDISR_ReviveSquelch ) VIRQ_Invoke();
            /* Cap AFTER the completion IRQ: the guest has been told this
             * block finished, so stopping here just defers its successor to
             * the next tick -- the same throttle a real SB applies. */
            if ( pt_mode && SNDISR_PtBlkCap && pt_blocks >= SNDISR_PtBlkCap ) {
#if PTDIAG
                pt_brk = 1; dbg_pt_why( PTD_BLKCAP );
#endif
                break;
            }
        } else {
#ifdef SNDISRLOG
            dbgprintf(("isr(%u): s/c(o)/b=0x%02X/0x%02X(0x%02X)/0x%03X SB Space=0x%X DMA Idx/Cnt=%X/%X\n", loop, nSamples, count, ocnt, bytes, SB_BuffSpace, DMA_Index, DMA_Count ));
#endif
            /* v1.9: to exit the loop here (unconditionally) was incorrect -
             *       might be that DMA buffer < SB buffer!
             *       test case: Open Cubic Player.
             *       however, exit if DMA autoinit isn't active should be ok.
             * v2.0: now unconditional exit is correct - DMA underrun is handled within loop.
             */
#if PTDIAG
            if ( pt_mode ) { pt_brk = 1; dbg_pt_why( PTD_PARTIAL ); }
#endif
            break;
            //if ( !VDMA_IsAuto(dmachannel) ) break;
        }
    };

#ifndef NOES1688
#if DACRING
    /* direct DAC on a card with a clock goes to the tap here, at a measured
     * rate (SNDISR_DacFeed), and skips the render tail below altogether --
     * still inside the render owner's guard, like the DMA tap above */
    if ( pt_mode && !IdxSm && PT_Ops->clock ) {
        SNDISR_DacFeed();
        es_in_render = 0;
        goto isrexit;
    }
#endif
    es_in_render = 0;   /* render owner done (re-entrant path skipped this via goto isrexit) */
#if PTDIAG
    if ( pt_mode ) {
        if ( pt_blocks > dbg_pt_maxblk ) {
            dbg_pt_maxblk = (unsigned char)pt_blocks;
#if !RATEDIAG
            LOW_PokeB(0x4F2, dbg_pt_maxblk);   /* 0x4F2 belongs to VSB.C under RATEDIAG */
#endif
        }
        if ( pt_blocks >= 2 ) dbg_pt_why( PTD_MULTI );
        /* Attribute an exit reason only when the loop actually CONSUMED
         * something. An idle tick never enters the body at all -- the
         * for-condition is false on the first test -- and scoring that as
         * "the guest did not re-arm" lights PTD_NOREARM from the moment the
         * driver loads, which is exactly what the first bench read showed. */
        if ( pt_blocks && !pt_brk ) {   /* fell out of the for-condition */
            if ( !VSB_Running() )        dbg_pt_why( PTD_NOREARM );
            else if ( IdxSm >= nSamples ) dbg_pt_why( PTD_SAMPBND );
        }
    }
#endif
    if ( pt_took )
        goto isrexit;   /* PT consumed this tick's audio raw: there is no render
                         * output to pad/mix/write, and skipping that tail is
                         * what keeps SNDISR inside one RTC period. */
#endif

    if (IdxSm) {
        /* in case there weren't enough samples copied, fill the rest with silence.
         * v1.5: it's better to reduce samples to IdxSm. If mode isn't autoinit,
         * the program may want to instantly initiate another DSP play cmd.
         * v1.8: returned to filling the rest with silence...
         * v2.0: in case there were MORE samples produced than required ( may happen
         * because of rate conversion or ADPCM ), adjust # of samples!
         */
#ifdef _DEBUG
# ifdef SNDISRLOG
        if ( IdxSm < nSamples ) dbgprintf(("isr: %u samples to add\n", nSamples - IdxSm ));
# endif
#endif
        /* memset, not a per-sample walk: 16-bit silence is byte-zero, so this
         * is one rep stosd instead of (nSamples-IdxSm) iterations of an index
         * multiply and two 16-bit stores. On the render path that bound is
         * render_cap - up to 512 frames per ISR tick on the VEW211. */
        if ( IdxSm < nSamples )
            memset( isr.pPCM + IdxSm * 2, 0,
                    (size_t)(nSamples - IdxSm) * 2 * sizeof(int16_t) );
        else
            /* v2.0: MORE samples produced than requested (rate conversion or
             * ADPCM): keep them, the hardware buffers can take the excess. */
            nSamples = IdxSm;

    } else if ( IdxSm = VSB_ReadDirectSamples( (uint8_t *)isr.pPCM ) ) {

        char *pDest = (char *)isr.pPCM;


        //uint32_t freq = AU_getfreq( isr.hAU );

        /* calc the src frequency by formula:
         * x / dst-freq = src-smpls / dst-smpls
         * x = src-smpl * dst-freq / dst-smpls
         */
        uint32_t SB_Rate = IdxSm * freq / nSamples;
#if RATEDIAG
        /* DIRECT-DAC RATE (the RATEDIAG blind spot). DSP cmd 0x10 never sets
         * vsb.Started, so VSB_Running() is false, the block loop never runs,
         * and VSB_GetSampleRate() -- sndisr's only route into CalcSampleRate,
         * where the 0x4F2/0x4F3 readout lives -- is never reached. Throughout
         * direct-DAC playback that pair therefore holds a STALE time-constant
         * reading left by the guest's SB detection, which is very easy to
         * misread as live.
         * This path derives its own rate above, so publish it: same rate>>8
         * units as 0x4F2, so the two are directly comparable, and 0 until
         * direct-DAC has actually run (which is the provenance signal -- no
         * spare byte exists for a flag).
         * 0x4FA IS ON LOAN. It normally carries sc_es1688's FULL-reconfig
         * count and, under PTDIAG, the tap loop's exit bitmap; both are
         * frozen while this path runs, since neither the tap nor PT_Feed
         * executes without vsb.Started. Those two already wrote the same byte
         * as each other, so RATEDIAG taking it removes an existing ambiguity
         * rather than creating one. Both are silenced under RATEDIAG. */
        LOW_PokeB( 0x4FA, (uint8_t)( SB_Rate >> 8 ) );
#endif

        /* v2.0: cv_rate() now expects an extra, final sample */
        *(pDest + IdxSm) = *(pDest + IdxSm - 1);
#ifdef SNDISRLOG
        dbgprintf(("isr, direct samples: IdxSm=%d, samples=%d, rate=%u\n", IdxSm, nSamples, SB_Rate ));
#endif
        cv_bits_8_to_16( isr.pPCM, IdxSm + 1, 0 );
        IdxSm = cv_rate( isr.pPCM, IdxSm, 1, SB_Rate, freq );
        cv_channels_1_to_2( isr.pPCM, IdxSm );
        /* memset, not a per-sample walk: 16-bit silence is byte-zero, so this
         * is one rep stosd instead of (nSamples-IdxSm) iterations of an index
         * multiply and two 16-bit stores. On the render path that bound is
         * render_cap - up to 512 frames per ISR tick on the VEW211. */
        if ( IdxSm < nSamples )
            memset( isr.pPCM + IdxSm * 2, 0,
                    (size_t)(nSamples - IdxSm) * 2 * sizeof(int16_t) );
    }

#ifndef NOES1688
    /* PT idle tick (no digital loop output, no direct-DAC bytes): don't burn
     * CPU zero-filling + mixing + downmixing PT_MODE_SAMPLES frames nobody
     * will hear -- the driver's ring/pump plays silence on its own. This was
     * real per-tick 486 work at the DOS prompt before. */
    if ( pt_mode && !IdxSm )
        goto isrexit;
#endif

    /* get volumes for software mixer */

    if( gvars.type < 4) { //SB2.0 and before
        mastervol = (VSB_GetMixerReg( SB_MIXERREG_MASTERVOL) & 0xF) << 4; /* 3 bits (1-3) */
        voicevol  = (VSB_GetMixerReg( SB_MIXERREG_VOICEVOL)  & 0x7) << 5; /* 2 bits (1-2) */
        midivol   = (VSB_GetMixerReg( SB_MIXERREG_MIDIVOL)   & 0xF) << 4; /* 3 bits (1-3) */
#if VOICELR
        mastervol2 = mastervol;
        voicevol2  = voicevol;
#endif
    } else {
        /* SBPro: L&R, bits 1-3/5-7, bits 0,3=1 */
        /* SB16:  L&R, bits 0-3/4-7 */
        mastervol = VSB_GetMixerReg( SB_MIXERREG_MASTERSTEREO) & 0xF0; /* 00,10,...F0 */
        voicevol  = VSB_GetMixerReg( SB_MIXERREG_VOICESTEREO)  & 0xF0;
        midivol   = VSB_GetMixerReg( SB_MIXERREG_MIDISTEREO)   & 0xF0;
#if VOICELR
        mastervol2 = (VSB_GetMixerReg( SB_MIXERREG_MASTERSTEREO) & 0xF) << 4;
        voicevol2  = (VSB_GetMixerReg( SB_MIXERREG_VOICESTEREO) & 0xF ) << 4;
#endif
    }
#if SETABSVOL
    if( isr.SB_VOL != mastervol * gvars.vol / 9) {
        isr.SB_VOL =  mastervol * gvars.vol / 9;
        //uint8_t buffer[FPU_SRSIZE];
        //fpu_save(buffer); /* needed if AU_setmixer_one() uses floats */
        AU_setmixer_one( isr.hAU, AU_MIXCHAN_MASTER, MIXER_SETMODE_ABSOLUTE, mastervol * 100 / 256 ); /* convert to percentage 0-100 */
        //fpu_restore(buffer);
        //dbgprintf(("isr: set master volume=%u\n", SNDISR_SB_VOL ));
    }
#else
    /* min: 10*10-1=ff ; ff >> 8 = 0, max: 100*100-1=ffff ; ffff >> 8 = ff */
    /* PRECEDENCE: '+' binds tighter than '|', so "v | 0xF + 1" was "v | 0x10",
     * never (v | 0xF) + 1. The comment above states the intent exactly -- the
     * operand range is meant to be 0x10..0x100 -- but 0xF0|0x10 is 0xF0, so
     * the product topped out at (0xF0*0xF0-1)>>8 = 0xE0. The 0xff test on the
     * next line was therefore UNREACHABLE, unity never happened, and every
     * sample paid a multiply for a permanent ~1.2 dB of attenuation. */
    voicevol = ( ((voicevol | 0xF) + 1) * ((mastervol | 0xF) + 1) - 1) >> 8;
    if ( voicevol == 0xff ) voicevol = 0x100;
    midivol  = ( ((midivol  | 0xF) + 1) * ((mastervol | 0xF) + 1) - 1) >> 8;
    if ( midivol == 0xff ) midivol = 0x100;
#endif

    /* software mixer: very simple implemented - but should work quite well */

    //if( gvars.opl3 ) {
#ifndef NOFM
    if( VOPL3_IsActive() ) {
        int channels;
        pPCMOPL = IdxSm ? isr.pPCM + nSamples * 2 : isr.pPCM;
        VOPL3_GenSamples( pPCMOPL, nSamples ); //will generate samples*2 if stereo
        //always use 2 channels
        channels = VOPL3_GetMode() ? 2 : 1;
        if( channels == 1 )
            cv_channels_1_to_2( pPCMOPL, nSamples );

        if( IdxSm ) {
# if MIXERROUTINE==0
#  if VOICELR
            voicevol2 = ( ((voicevol2 | 0xF) + 1) * ((mastervol2 | 0xF) + 1) - 1) >> 8;
            if ( voicevol2 == 0xff ) voicevol2 = 0x100;
#  endif
            /* a and b are PROVABLY 0..65535 once the +32768 bias is applied
             * (the scaled sample spans -32768..32767), so mix in UNSIGNED.
             * Two reasons, and the first one is a bug:
             *   - a*b reaches 65535*65535 = 0xFFFE0001, which OVERFLOWS a
             *     signed 32-bit int. The wrapped product fed the screen-blend
             *     branch a bogus term: e.g. a=b=50000 should mix to 58171 but
             *     computed 189240 and hit the full-scale clamp instead. So on
             *     loud FM-plus-digital material this clipped where it should
             *     have mixed. Unsigned holds the product exactly.
             *   - /256 and /32768 on a SIGNED value are not shifts; the
             *     compiler has to emit the round-toward-zero bias sequence.
             *     Unsigned makes them >>8 and >>15. Together with the three
             *     IMULs (13-42 cycles each on a 486) this loop is the whole
             *     per-sample cost of an FM build, so it is worth the care.
             * The sample scaling stays SIGNED - the PCM is signed - and >>8
             * there rounds toward -inf rather than toward zero: one LSB, at
             * -90 dBFS. The non-FM path below already scales with >>8. */
            for( i = 0; i < nSamples * 2; i++ ) {
                unsigned a = (unsigned)(((*(isr.pPCM+i) * (int)voicevol) >> 8) + 32768);
                unsigned b = (unsigned)(((*(pPCMOPL+i) * (int)midivol)  >> 8) + 32768);
                unsigned mixed = (a < 32768 || b < 32768) ? ((a*b) >> 15)
                                 : ((a+b)*2 - ((a*b) >> 15) - 65536);
                *(isr.pPCM+i) = (mixed > 65535 ) ? 0x7fff : (int16_t)(mixed - 32768);
#  if VOICELR
                i++;
                a = (unsigned)(((*(isr.pPCM+i) * (int)voicevol2) >> 8) + 32768);
                b = (unsigned)(((*(pPCMOPL+i) * (int)midivol)   >> 8) + 32768);
                mixed = (a < 32768 || b < 32768) ? ((a*b) >> 15)
                        : ((a+b)*2 - ((a*b) >> 15) - 65536);
                *(isr.pPCM+i) = (mixed > 65535 ) ? 0x7fff : (int16_t)(mixed - 32768);
#  endif
            }
# elif MIXERROUTINE==1
            /* this variant is simple, but quiets too much ... */
            for( i = 0; i < nSamples * 2; i++ ) *(isr.pPCM+i) = ( *(isr.pPCM+i) * voicevol + *(pPCMOPL+i) * midivol ) >> (8+1);
# else
            /* in assembly it's probably easier to handle signed/unsigned shifts */
            SNDISR_Mixer( isr.pPCM, pPCMOPL, nSamples * 2, voicevol, midivol );
# endif
# ifdef _LOGBUFFMAX
            if ( (( pPCMOPL + nSamples * 2 ) - isr.pPCM ) * sizeof(int16_t) > isr.dwMaxBytes )
                isr.dwMaxBytes = (( pPCMOPL + nSamples * 2 ) - isr.pPCM ) * sizeof(int16_t);
# endif
        } else if ( midivol != 0x100 )   /* unity: x * 0x100 >> 8 == x, skip */
            for( i = 0; i < nSamples * 2; i++, pPCMOPL++ ) *pPCMOPL = ( *pPCMOPL * midivol ) >> 8;
    } else {
#endif
        if( IdxSm ) {
# if VOICELR
            voicevol2 = ( ((voicevol2 | 0xF) + 1) * ((mastervol2 | 0xF) + 1) - 1) >> 8;
            if ( voicevol2 == 0xff ) voicevol2 = 0x100;
# endif
            /* Unity is the COMMON case -- both mixer sliders at max -- and the
             * precedence fix above is what finally lets voicevol reach 0x100.
             * x * 0x100 >> 8 == x, so this whole pass over nSamples*2 values is
             * a no-op there; skipping it is free CPU at full volume. */
            if ( voicevol == 0x100
# if VOICELR
                 && voicevol2 == 0x100
# endif
               ) {
                pPCMOPL = isr.pPCM + nSamples * 2;  /* where the loop would end */
            } else
            for( i = 0, pPCMOPL = isr.pPCM; i < nSamples * 2; i++, pPCMOPL++ ) {
                *pPCMOPL = ( *pPCMOPL * voicevol ) >> 8;
# if VOICELR
                pPCMOPL++; i++;
                *pPCMOPL = ( *pPCMOPL * voicevol2 ) >> 8;
# endif
            }
#ifdef _LOGBUFFMAX
            if ( ( pPCMOPL - isr.pPCM ) * sizeof(int16_t) > isr.dwMaxBytes )
                isr.dwMaxBytes = (( pPCMOPL + nSamples * 2 ) - isr.pPCM ) * sizeof(int16_t);
#endif
        } else
            memset( isr.pPCM, 0, nSamples * sizeof(int16_t) * 2 );
#ifndef NOFM
    }
#endif
    //aui.samplenum = nSamples * 2;
    //aui.pcm_sample = ISR_PCM;
#if SOUNDFONT
    if (tsfrenderer) {
        unsigned char fpu_buffer[FPU_SRSIZE];
        fpu_save( fpu_buffer );
        VMPU_Process_Messages();
        //tsf_set_samplerate_output(tsfrenderer, AU_getfreq( isr.hAU ));
        tsf_render_short(tsfrenderer, isr.pPCM, nSamples, 1);
        fpu_restore( fpu_buffer );
    }
#ifdef CARD_AUDIGY
    /* hardware wavetable: the EMU10K2 renders on its own voices, so only the
     * MIDI ring needs pumping -- integer-only, no FPU state to save */
    else if ( EMUWT_Active() ) {
        extern int SBALL_WTTick(void);
        VMPU_Process_Messages();
        /* AUDTIMER pump: envelope stepping assumes the ~83 Hz loop-
         * interrupt cadence -- SBALL_WTTick divides the RTC tick rate
         * back down to it (unity in interrupt mode). MIDI parsing
         * stays every tick: finer timing is strictly better. */
        if ( SBALL_WTTick() )
            EMUWT_Poll();
    }
#endif
#endif
#ifndef NOES1688
    /* PT mode skipped AU_cardbuf_space() at the top of the tick, and
     * AU_writedata below is paced by the card_dmaspace figure that call
     * maintains (writedata() hands the card nothing once it reads 0 -- and
     * it starts at 0). Everything that reaches this tail on a PT tick needs
     * it: direct-DAC bytes, and ADPCM blocks, which take the render path
     * even in PT mode because the card cannot play them raw. The first cut
     * refreshed it on the direct-DAC branch only and silenced Duke Nukem
     * II's ADPCM sound effects on the PC110 (bench 2026-09-05). */
    if ( pt_mode )
        AU_cardbuf_space( isr.hAU );
#endif
    AU_writedata( isr.hAU, isr.pPCM, nSamples * 2 );


#if SLOWDOWN
    if ( gvars.slowdown )
        delay_10us(gvars.slowdown);
#endif

isrexit:
#ifndef NOES1688
    PT_Ops->dbg_exit();   /* DIAG: depth-- + outermost-pass duration (0x4FC/D) */
#endif
    /* IRQ8 is the RTC periodic interrupt, which is not ours to own.  Other
       resident software can legitimately need the same tick - a MIDI synth
       engine that runs off the RTC, for one - and unlike a device IRQ there is
       no per-consumer flag to arbitrate with, so whoever hooks INT 70h last
       would otherwise take every tick.  Report "not handled" so STACKISR falls
       through to dfOldSndVec, and leave the EOI to the last handler in the
       chain: sending one here as well can dismiss a pending interrupt early.
       Our pump work is already done by this point.  Backends on a normal
       device IRQ are unaffected. */
    if ( isr.SndIrq == 8 )
        return(0);
    PIC_SendEOI( isr.SndIrq );
#if COMPAT4
    if ( gvars.compatflags & CF_MASKPIT )
        return( 2 | (mask << 8 ));
#endif
    return(1);
}

#if IRQONPORTACC
/* This function is meant to allow a sound HW IRQ if interrupts are disabled.
 * It's supposed to be called while trapped FM/MPU ports are handled.
 */
void SNDISR_IrqOnPortAcc( void )
////////////////////////////////
{
    uint16_t mask = PIC_GetIRQMask();
    PIC_SetIRQMask(mask & ~(1 << AU_getirq(isr.hAU)));
    _enable_ints();
    _disable_ints();
    PIC_SetIRQMask(mask);
    return;
}
#endif

/* init sound hw - called by main() */

bool SNDISR_Init( void *hAU, uint16_t vol )
///////////////////////////////////////////
{
#if PT0V86
#define PT0SIZE 0x1000
    uint32_t tmp;
#else
#define PT0SIZE 0
#endif
    __dpmi_meminfo info;

    /* allocate PCM buffer (def. 64k), used for format conversions */
    info.address = 0;
    info.size = ( gvars.buffsize + 1 ) * 4096;
    if (__dpmi_allocate_linear_memory( &info, 1 ) == -1 )
        return false;

    /* uncommit the page behind the buffer so a buffer overflow will cause a page fault */
    __dpmi_set_page_attr( info.handle, gvars.buffsize * 4096, 1, 0);
    isr.pPCM = NearPtr( info.address );
    dbgprintf(("SNDISR_Init: pPCM=%X\n", isr.pPCM ));

    /* ISR scratch (see ISR_ScratchGet): same size and same guard page as the
     * PCM buffer, which covers both users comfortably -- cv_rate needs
     * (nSamples+2)*2 bytes for at most 2*samples samples, and DecodeADPCM's
     * output is about one guest block. Failure is NOT fatal: ScratchGet then
     * falls back to malloc, i.e. the previous behaviour. */
    info.address = 0;
    info.size = ( gvars.buffsize + 1 ) * 4096;
    if ( __dpmi_allocate_linear_memory( &info, 1 ) != -1 ) {
        __dpmi_set_page_attr( info.handle, gvars.buffsize * 4096, 1, 0);
        isr_scratch = (uint8_t *)NearPtr( info.address );
        isr_scratch_size = gvars.buffsize * 4096;
        dbgprintf(("SNDISR_Init: scratch=%X size=%X\n", isr_scratch, isr_scratch_size ));
    }

    /* allocate a 128k uncommitted region used for DMA mappings */
    info.address = 0;
    info.size = 0x20000 + 0x1000 + PT0SIZE;
    if ( __dpmi_allocate_linear_memory( &info, 0 ) == -1 )
        return false;

    isr.Block_Handle = info.handle;
    isr.Block_Addr   = info.address;

#if PT0V86
    /* v1.8: get phys. address of VCPI host's page table 0 and map it into
     * protected-mode address space. This allows to access physical addresses
     * within the v86 conventional address space (EMS page frame).
     */
    if ( tmp = PTRAP_GetPageTab0v86() ) {
        if( __dpmi_map_physical_device(isr.Block_Handle, 0x20000 + 0x1000, 1, tmp ) == 0 ) {
            __dpmi_set_page_attr(isr.Block_Handle, 0x20000 + 0x1000, 1, 3 ); /* 3 = set page to r/o */
            isr.PageTab0v86 = info.address + 0x20000 + 0x1000;
            dbgprintf(("SNDISR_Init: v86 PT0=%X mapped at %X\n", tmp, isr.PageTab0v86 ));
        }
    }
#endif
    isr.hAU = hAU;
    isr.SndIrq = AU_getirq( hAU );

#if SETABSVOL
    isr.SB_VOL = vol;
#endif
    return _SND_InstallISR( PIC_IRQ2VEC( AU_getirq( hAU ) ), &SNDISR_Interrupt );
}

bool SNDISR_Exit( void )
////////////////////////
{
#ifdef _LOGBUFFMAX
    printf("SNDISR_Exit: max PCM buffer usage=%u\n", isr.dwMaxBytes );
#endif
#ifdef _DEBUG
    printf("SNDISR_Exit: cnt total/voice=%u/%u max/avg samples=%u/%u\n", isr.cntTotal, isr.cntDigital, isr.max_samples, isr.cntTotal ? isr.total_samples / isr.cntTotal : 0 );
#endif
    return ( _SND_UninstallISR( PIC_IRQ2VEC( AU_getirq( isr.hAU ) ) ) );
}



