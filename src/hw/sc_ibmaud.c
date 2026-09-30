/* sc_ibmaud.c -- IBM PCMCIA Audio Adapter backend for VSBPCMCIA.
 *
 * The card: IBM "PCMCIA Audio Adapter", CIS "IBM | NON-DSP AUDIO | 0933967",
 * MANFID 00A4/0022. No DMA, no FM. Bring-up: run IBMAUDGO first (no IRQ: this
 * backend needs no card interrupt). This backend drives the card, it does not
 * enable it.
 *
 * INTERFACE (ibmaudgo doc/recon.md, 2026-09-29: recovered by I/O trace
 * of the vendor DOS driver, then confirmed by our own player, IBMPLAY). There
 * is no host-visible codec register file: an IBM ASIC fronts a serial codec,
 * whose control fields match the Crystal CS4215/AD1849 layout, and keeps a
 * card-side sample ring.
 *   base+0..4   codec control frame, sent when base+8 is written with bit 7
 *   base+4..7   data mode: left attenuation (+ bit 7 output enable), right
 *               attenuation, input gains, monitor
 *   base+8      mode: bit 5 16-bit, bit 4 stereo, bit 2 data mode, bit 0 run
 *   base+9      bit 7 holds playback
 *   win1+0      word, write: the sample stream, appended to the ring
 *   win1+2      word: status (bit 4 = idle) / command
 *   win1+6      word, read: ring play position in words, 14 bits
 * win1 is the 8-byte block of the alias pair: base 250h -> win1 340h, E50h ->
 * F40h (IBMAUDGO pairs them the same way).
 *
 * MODEL. No host ring: the card's ring IS the buffer. The codec is armed at
 * ONE fixed format and the passthrough tap folds every guest format onto it
 * with the frame stepper (Bresenham + linear interpolation, sc_scp55's shape),
 * writing straight to the card. The clock never follows SB_Rate
 * (sc_mc8k's settled lesson: VSB's inferred rate is garbage on single-cycle
 * and direct-DAC playback). PT_Space reports card room from the card's own
 * play position -- closed-loop pacing on the codec clock, no credit model.
 * The RTC tick only tracks that position, pads silence below a low-water
 * mark, catches a dead codec link and closes the card when the guest goes
 * quiet.
 *
 * OUTPUT FORMAT. Sound Blaster class, sized for the 486SX/33 floor: 8-bit
 * unsigned stereo at 11025 Hz by default -- SB Pro output. One card word per
 * frame, left in the low byte, so that is 11025 word writes a second (22 kHz
 * 16-bit stereo would be 44100). Epic Pinball plays clean this way on the
 * PC110 (bench, 2026-09-30). /RESAMP, where the engine mixes and software FM
 * is rendered, defaults to 16-bit stereo instead (22050 writes a second): at
 * 8 bits the FM's note tails and fades turned grainy. /DACRATE picks the table
 * rate (22050 for 22 kHz titles), SBEIBM16=0/1 overrides the depth either way,
 * and SBEIBMST=0 drops to mono for the slowest hosts: two 8-bit samples per
 * word, 5512 writes a second, with the stepper averaging L and R. By ear that
 * downmix played only one of Epic Pinball's two channels, at T3 and at T4,
 * and the cause was not found; the IBSTDIAG build below measures what the
 * guest sends.
 *
 * TWO HARD RULES (bench, PC110, 2026-09-29):
 *  - never let the ring run dry while armed. An underrun kills the codec
 *    link (status reads idle, the codec status byte reads FFh) and only a
 *    full re-arm recovers. Hence the silence pad and the dead-link check.
 *  - never queue 8K words. 8192 ahead overflowed with the same symptoms;
 *    4096 is proven, IB_QMAX stays below 6K.
 *
 * ONE WRITER. SETIF lets a second SNDISR nest, and a nested pass runs this
 * backend's irq_routine while the outer pass may be inside PT_Feed writing
 * words to the card. ib_busy makes every card write sequence and the position
 * tracker exclusive; the pump simply skips a tick it would have interleaved.
 * (SNDISR's own es_in_render guard already keeps feed and render from
 * nesting inside themselves.)
 */
#ifndef NOIBMAUD

#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "hostsvc.h"      // toolchain compat: LOW_*, inportb (see the header)
#include "hostisr.h"      // chained/iret PM interrupt vectors, both builds
#include "au_cards.h"
#include "ptops.h"        // engine passthrough ops table (we register in adetect)

// ==========================================================================
//  crazii DPMI-API compat shim over DJGPP (same as sc_scp55.c)
// ==========================================================================
#ifndef TRUE
#define TRUE  1
#define FALSE 0
#endif
#define DPMI_DisableInterrupt  HOST_DisableInterrupt   /* int 31h ax=0900h/0901h */
#define DPMI_RestoreInterrupt  HOST_RestoreInterrupt

// hostsvc.h only carries byte I/O; the card's stream and status are words
// (spelled as in sc_mc8k.c).
#ifdef DJGPP
# define ib_inw(p)      inportw((unsigned short)(p))
# define ib_outw(p,v)   outportw((unsigned short)(p),(unsigned short)(v))
#else
# define ib_inw(p)      inpw(p)
# define ib_outw(p,v)   outpw((p),(unsigned)(v))
#endif

// ==========================================================================
//  Telemetry (the 0x4F0-0x4FF map -- see doc/NOTES.md). This backend's own
//  bytes: 0x4F8 irq_routine calls, 0x4FB/0x4FF 16-bit feed count, 0x4FE
//  feed frames dropped for want of card room, 0x4F4 watchdog revivals,
//  0x4F5 card queue gauge (words >> 5), 0x4F6 dead codec links caught,
//  0x4F3 feed re-entries, 0x4FA arms and 0x4F2 guest rate >> 8 (both on
//  loan to the diag builds, as in sc_scp55.c).
// ==========================================================================
static uint16_t      ib_tel_feed16;
static unsigned long ib_tel_bytes;
static unsigned char ib_tel_irq, ib_tel_drop, ib_tel_dead, ib_tel_arm, ib_reentry;

// ---- card geometry ---------------------------------------------------------
#define IB_B0_DEF     0x250         // IBMAUDGO's default /IO1
#define IB_QMAX       5632U         // words queued, hard cap (8192 overflowed)
#define IB_IDLE_MS    1000U         // this long without a feed, close the card
#define IB_RS_RUN     9             // pump 128 Hz while armed (256 if the pad floor
                                    // is under ~40 ms -- see adetect)
#define IB_RS_IDLE    11            // 32 Hz keep-alive (watchdog only) while closed
#define IB_PT_HOLD    64            // ticks the tap keeps the card after its last feed
#define IB_RENDER_DIV 2             // /RESAMP: render on 1 tick in 2 = 64 Hz ...
#define IB_RENDER_CAP 512           // ... <= 512 frames a pass (345 at 22050)
#define DAC_RATE_DEF  11025
#define BYTES_PER_SBSAMPLE 4        // engine render frames are 16-bit stereo

// STEREO FORENSICS (diag builds only, see IB_SD below). Borrows 0x4F3, 0x4F5,
// 0x4F6, 0x4FA, 0x4FC and 0x4FD, and adds SBEIBMCH (1 = left, 2 = right, else
// the mix) for the mono downmix.
#ifndef IBSTDIAG
#define IBSTDIAG 0
#endif

typedef struct ibmaud_card_s { uint16_t base; } ibmaud_card_s;

static uint16_t ib_b0 = IB_B0_DEF, ib_b1 = 0x340;
#define IB_FIFO  ((uint16_t)(ib_b1 + 0))
#define IB_CMD   ((uint16_t)(ib_b1 + 2))
#define IB_POS   ((uint16_t)(ib_b1 + 6))

// Codec rate table: the Crystal table, both crystals (the vendor manual lists
// exactly these fourteen). Bench-verified by consumption rate, 2026-09-29:
// 8000, 11025, 22050, 44100, 48000. The others follow the same fields.
static const struct { unsigned long hz; unsigned char xtal, dfs; } ib_rates[] = {
 { 8000UL,1,0},{16000UL,1,1},{27429UL,1,2},{32000UL,1,3},{48000UL,1,6},{ 9600UL,1,7},
 { 5513UL,2,0},{11025UL,2,1},{18900UL,2,2},{22050UL,2,3},{37800UL,2,4},{44100UL,2,5},
 {33075UL,2,6},{ 6615UL,2,7}
};
#define IB_NRATES (int)(sizeof(ib_rates)/sizeof(ib_rates[0]))

static unsigned long ib_frate = DAC_RATE_DEF;  // codec (output) rate, frames/s
static unsigned char ib_dfr, ib_scr;           // codec data format / crystal select
static unsigned char ib_mbits;                 // mode bits: 20h 16-bit, 10h stereo
static int           ib_o16, ib_ost;           // output 16-bit? stereo?
static unsigned      ib_bpf = 1;               // output bytes per frame (1, 2 or 4)
static unsigned      ib_sil = 0x8080;          // one card word of silence
static unsigned char ib_rs_run = IB_RS_RUN;    // armed pump rate
static unsigned      ib_obyte;                 // 8-bit mono: first sample of the
static int           ib_ohave;                 // next word, waiting for its pair
static unsigned char ib_att;                   // output attenuation 0..63 (/CVOL)
static unsigned      ib_lat_ms = 80;           // SBEPTLAT: queued-audio target
static unsigned      ib_qtarget, ib_qlow;      // card words: latency target, pad floor
static unsigned      ib_virt_frames = 1024;    // /RESAMP: buffer the engine is shown
static int           ib_resamp;

// Card state. OWNERSHIP: everything below is touched only with ib_busy held
// (see ONE WRITER in the header).
static volatile int  ib_busy;
static volatile int  ib_armed;
static uint32_t      ib_wr;                    // words written since arm
static uint32_t      ib_cons;                  // words played since arm
static unsigned      ib_lastpos;               // last 14-bit position read

static unsigned char ib_rtc_rs = IB_RS_IDLE;   // current armed RTC pump rate
static int           ib_rtc_fixed;             // SBERTC pins it
static uint32_t      ib_idle_ticks = 256;
// Time is kept in OUR OWN RTC tick count, never the BIOS tick at 0x46C:
// games own the timer chain (see sc_scp55.c).
static volatile uint32_t ib_tick_seq;          // ++ per delivered RTC tick
static volatile uint32_t ib_feed_seq;          // ib_tick_seq at the last feed (any path)
static volatile uint32_t ib_pt_seq;            // ib_tick_seq at the last TAP feed
static uint32_t      ib_arm_fail;              // tick of the last failed arm, 0 = none

// Passthrough stream + frame stepper.
static volatile int  ib_pt_active;
static unsigned      ib_pt_rate, ib_pt_bits, ib_pt_channels;
static unsigned long ib_step_acc;              // Bresenham phase, 0..rate-1
static unsigned long ib_step_inv;              // (1 << 24) / rate: w without a divide
static int           ib_pl, ib_pr, ib_pvok;    // previous guest frame, 16-bit L/R
static long          ib_dsl, ib_dsr;           // decimator: sums of the guest frames
static unsigned      ib_dn;                    // ... in the current output interval

#if IBSTDIAG
// What the guest's two channels hold, in 8-bit steps, per window of 8192
// frames of one format: mean |L|, |R|, |L-R|, |L+R|/2 and |L(n)-L(n-1)|. The
// loudest window is kept, a stereo window beating any mono one:
//   0x4F3 channels << 4 | bytes per sample   0x4FC |L|   0x4FD |R|
//   0x4F6 |L-R|   0x4FA |L+R|/2   0x4F5 |L(n)-L(n-1)|
// Reading: real stereo gives |L-R| near |L|+|R|; the same signal in both
// gives |L-R| near 0; one side silent gives |L| or |R| near 0; R the inverse
// of L gives |L+R|/2 near 0. Mono data read as pairs of adjacent samples
// gives |L-R| near |L(n)-L(n-1)|/2.
static unsigned long ib_sd_l, ib_sd_r, ib_sd_d, ib_sd_s, ib_sd_dl;
static unsigned      ib_sd_n, ib_sd_best;
static int           ib_sd_prev;
static int           ib_sd_ch;                 // SBEIBMCH
#define IB_SD_ABS(x) ((unsigned)((x) < 0 ? -(x) : (x)))
#define IB_SD_B(x)   ((unsigned char)((x) > 255UL ? 255UL : (x)))
static void ib_sd_reset(void)
{
 ib_sd_l = ib_sd_r = ib_sd_d = ib_sd_s = ib_sd_dl = 0; ib_sd_n = 0;
}
static void ib_sd_frame(int a, int b, unsigned channels, unsigned bits)
{
 ib_sd_l += IB_SD_ABS(a);
 ib_sd_r += IB_SD_ABS(b);
 ib_sd_d += IB_SD_ABS(a - b);
 ib_sd_s += IB_SD_ABS(a + b);
 ib_sd_dl += IB_SD_ABS(a - ib_sd_prev);
 ib_sd_prev = a;
 if(++ib_sd_n >= 8192u){
  unsigned l = (unsigned)(ib_sd_l >> 13), r = (unsigned)(ib_sd_r >> 13);
  unsigned score = l + r + (channels >= 2 ? 0x1000u : 0u);
  if(score > ib_sd_best){
   ib_sd_best = score;
   LOW_PokeB(0x4F3, (unsigned char)((channels << 4) | (bits >= 16 ? 2u : 1u)));
   LOW_PokeB(0x4FC, IB_SD_B(ib_sd_l >> 13));
   LOW_PokeB(0x4FD, IB_SD_B(ib_sd_r >> 13));
   LOW_PokeB(0x4F6, IB_SD_B(ib_sd_d >> 13));
   LOW_PokeB(0x4FA, IB_SD_B(ib_sd_s >> 14));
   LOW_PokeB(0x4F5, IB_SD_B(ib_sd_dl >> 13));
  }
  ib_sd_reset();
 }
}
#endif

static void rtc_enable(void);
static void ib_rtc_setrate(unsigned char rs);

static void ib_iodelay(unsigned n){ while(n--) (void)inportb(0x80); }

// One attenuation step per write: the vendor's anti-pop ramp. from and to
// must agree in bit 7 (the left register's output enable).
static void ib_fade(uint16_t port, unsigned from, unsigned to)
{
 if(from < to){ while(from < to) outportb(port, ++from); }
 else         { while(from > to) outportb(port, --from); }
}

static int ib_rate_pick(unsigned long rate, unsigned long ceil)
{
 int i, best = 9; unsigned long bd = 0xFFFFFFFFUL;   // default = 22050
 for(i = 0; i < IB_NRATES; i++){
  unsigned long d;
  if(ib_rates[i].hz > ceil) continue;
  d = ib_rates[i].hz > rate ? ib_rates[i].hz - rate : rate - ib_rates[i].hz;
  if(d < bd){ bd = d; best = i; }
 }
 return best;
}

// ---- card position -----------------------------------------------------------
// ib_busy held. The 14-bit position wraps every 16K words -- 3 s at the 8-bit
// mono default, 186 ms at 44.1 kHz 16-bit stereo; the pump samples it at
// 128-256 Hz while armed.
static void ib_track(void)
{
 unsigned p = ib_inw(IB_POS) & 0x3FFFu;
 ib_cons += (uint32_t)((p - ib_lastpos) & 0x3FFFu);
 ib_lastpos = p;
 if(ib_cons > ib_wr) ib_cons = ib_wr;          // it cannot play past our writes
}
#define IB_QUEUED() ((unsigned)(ib_wr - ib_cons))

// ---- codec arm / close ---------------------------------------------------------
// One five-byte control frame, strobed by base+8 bit 7; base+9 is pulsed
// between frames. Returns the codec status byte read back.
static unsigned char ib_frame(unsigned char clb, int pulse)
{
 outportb(ib_b0+0, clb);
 outportb(ib_b0+1, ib_dfr);
 outportb(ib_b0+2, ib_scr);
 outportb(ib_b0+3, 0);
 outportb(ib_b0+4, 0);
 if(pulse){ outportb(ib_b0+9, 0x86); outportb(ib_b0+9, 0x06); }
 outportb(ib_b0+8, 0x88 | ib_mbits);
 ib_iodelay(100);
 return (unsigned char)inportb(ib_b0+0);
}

// ib_busy held. The vendor driver's sequence (recon.md), in its order: close,
// mute, codec control handshake, data mode, prepare, start with playback
// held, prime the ring with silence, release the hold, fade in. ~10 ms.
// Also resets the card's ring pointers, which is why a dead link recovers
// here. Returns 0 when armed.
static int ib_arm(void)
{
 unsigned char st = 0, a;
 unsigned i;
 ib_armed = 0;
 ib_outw(IB_CMD, 0x0200);
 a = (unsigned char)(inportb(ib_b0+5) & 0x3F); ib_fade(ib_b0+5, a, 0x3E);
 a = (unsigned char)inportb(ib_b0+4);          ib_fade(ib_b0+4, a, (a & 0x80u) | 0x3Eu);
 ib_outw(IB_CMD, 0x0001);
 ib_outw(IB_CMD, 0x0002);
 for(i = 0; i < 40; i++){                        // control latch 0 until echoed (20h)
  st = ib_frame(0x00, i > 0);
  if((st & 0x24) == 0x20) break;
 }
 if(i == 40) return 1;
 for(i = 0; i < 40; i++){                        // then 1 until echoed (24h)
  st = ib_frame(0x04, 1);
  if(i == 0){ ib_iodelay(5000); continue; }
  if((st & 0x24) == 0x24) break;
 }
 if(i == 40) return 2;
 outportb(ib_b0+8, 0x04 | ib_mbits);             // data mode
 outportb(ib_b0+9, 0x06);
 // prepare
 outportb(ib_b0+4, 0x3F); outportb(ib_b0+5, 0x3F);
 outportb(ib_b0+6, 0x0F); outportb(ib_b0+7, 0xFF);
 ib_outw(IB_FIFO, ib_sil); ib_outw(IB_FIFO, ib_sil);
 for(i = 0; i < 1001; i++) outportb(ib_b0+8, 0x01);
 outportb(ib_b0+8, 0x04 | ib_mbits);
 ib_outw(IB_CMD, 0x0302); ib_outw(IB_CMD, 0x0302);
 outportb(ib_b0+9, 0x1E);
 outportb(ib_b0+8, 0x04 | ib_mbits);
 outportb(ib_b0+0x0F, 0xFF);
 outportb(ib_b0+9, 0x86); outportb(ib_b0+9, 0x86);
 // start, held (base+9 bit 7)
 ib_outw(IB_CMD, 0x0302);
 outportb(ib_b0+9, 0x1E);
 outportb(ib_b0+8, 0x04 | ib_mbits);
 outportb(ib_b0+0x0F, 0xFF);
 outportb(ib_b0+9, 0x86);
 ib_outw(IB_FIFO, ib_sil); ib_outw(IB_FIFO, ib_sil);
 outportb(ib_b0+8, 0x01);
 ib_outw(IB_CMD, 0x0102);
 // the play position restarts here; the two silence words above are queued
 ib_lastpos = ib_inw(IB_POS) & 0x3FFFu;
 ib_wr = 2; ib_cons = 0; ib_ohave = 0;
 for(i = 0; i < ib_qlow; i++) ib_outw(IB_FIFO, ib_sil);
 ib_wr += ib_qlow;
 // release the hold: the card starts playing the ring
 outportb(ib_b0+8, 0x05 | ib_mbits);
 outportb(ib_b0+9, 0x06);
 outportb(ib_b0+6, 0x00); outportb(ib_b0+7, 0xF0);
 ib_fade(ib_b0+5, 0x3F, ib_att);                 // fade in, right then left
 outportb(ib_b0+4, 0xBF);                        // output on, still attenuated
 ib_fade(ib_b0+4, 0xBF, 0x80u | ib_att);
 ib_armed = 1;
 return 0;
}

// ib_busy held. Fade out and close. Words still queued are harmless: the
// close does not need the ring drained, and the next arm resets it.
static void ib_close(void)
{
 if(ib_armed){
  unsigned char a = (unsigned char)(inportb(ib_b0+5) & 0x3F);
  ib_fade(ib_b0+5, a, 0x3E);
  a = (unsigned char)inportb(ib_b0+4);
  ib_fade(ib_b0+4, a, (a & 0x80u) | 0x3Eu);
 }
 ib_outw(IB_CMD, 0x0200);
 ib_armed = 0;
}

// ib_busy held. Arm on demand from the feed paths. A failed arm (card gone,
// IBMAUDGO not run) is retried about once a second, not per call: each
// attempt busy-waits ~10 ms. Returns 1 when armed.
static int ib_ensure_armed(void)
{
 if(ib_armed) return 1;
 if(ib_arm_fail && ib_tick_seq - ib_arm_fail < 256u) return 0;
 if(ib_arm()){ ib_arm_fail = ib_tick_seq | 1u; return 0; }
 ib_arm_fail = 0;
#if !PTDIAG && !RATEDIAG && !IBSTDIAG
 LOW_PokeB(0x4FA, ++ib_tel_arm);
#else
 (void)ib_tel_arm;
#endif
 ib_pvok = 0;
 if(!ib_rtc_fixed && ib_rtc_rs != ib_rs_run) ib_rtc_setrate(ib_rs_run);
 return 1;
}

// ib_busy held, room checked by the caller. One output frame in the card's
// format; l/r are 16-bit signed, and mono output takes l (callers downmix).
// 8-bit mono packs two frames per word, first in the low byte.
static void ib_emit(int l, int r)
{
 if(ib_o16){
  ib_outw(IB_FIFO, (unsigned)l); ib_wr++;
  if(ib_ost){ ib_outw(IB_FIFO, (unsigned)r); ib_wr++; }
 }else{
  unsigned bl = (unsigned)((l >> 8) + 128) & 0xFFu;
  if(ib_ost){
   ib_outw(IB_FIFO, bl | (((unsigned)((r >> 8) + 128) & 0xFFu) << 8)); ib_wr++;
  }else if(ib_ohave){
   ib_outw(IB_FIFO, ib_obyte | (bl << 8)); ib_wr++; ib_ohave = 0;
  }else{
   ib_obyte = bl; ib_ohave = 1;
  }
 }
}
// output frames that fit in `words` card words
#define IB_WORDS_TO_FRAMES(w) ((unsigned)(((unsigned long)(w) * 2UL) / ib_bpf))

// ---- the pump: RTC tick -------------------------------------------------------
static void ib_pump(void)
{
 unsigned q, n;
 if(ib_busy) return;                             // an outer pass is writing
 ib_busy = 1;
 if(ib_armed){
  if(ib_inw(IB_CMD) & 0x0010){                   // idle while armed: the link died
   ib_armed = 0;
#if !RATEDIAG && !IBSTDIAG
   LOW_PokeB(0x4F6, ++ib_tel_dead);              // (0x4F6 on loan to RATEDIAG)
#endif
  }else{
   ib_track();
   q = IB_QUEUED();
   // Keep the ring from running dry while the guest is silent. Only the
   // passthrough path pads: the render path is fed every render tick, and
   // words the engine did not write would skew its buffer arithmetic.
   if(!ib_resamp && q < ib_qlow){
    n = ib_qlow - q;
    if(ib_ohave){                                // complete the half-written word
     ib_outw(IB_FIFO, ib_obyte | 0x8000u); ib_wr++; ib_ohave = 0;
     if(n) n--;
    }
    if(ib_o16 && ib_ost) n = (n + 1u) & ~1u;     // keep L/R word pairs aligned
    ib_wr += n;
    while(n--) ib_outw(IB_FIFO, ib_sil);
    q = IB_QUEUED();
   }
#if !IBSTDIAG
   TEL_PokeB(0x4F5, (unsigned char)(q >> 5));    // card queue gauge
#endif
   if(!ib_resamp && ib_tick_seq - ib_feed_seq > ib_idle_ticks)
    ib_close();                                  // guest gone quiet
  }
 }
 ib_busy = 0;
}

// ==========================================================================
//  Passthrough backend ABI
// ==========================================================================
// RTC-death watchdog: verbatim logic from sc_scp55.c (read its note). Trigger
// on OUR tick counter going stale, verify the RTC arm state, fix only what is
// provably wrong; the PF-eating register-C heal waits for 1-2 s of silence
// measured by the RTC's own seconds register.
static void ib_watchdog(void)
{
 static unsigned char tel_revive;
 static uint32_t wd_seq;
 static unsigned char wd_stale, wd_sec, wd_secchg;
 unsigned char b, sec;
 uint8_t f;
 uint32_t seq = ib_tick_seq;
 if(seq != wd_seq){ wd_seq = seq; wd_stale = 0; wd_secchg = 0; return; }
 if(++wd_stale < 2) return;                      // debounce one visit
 wd_stale = 0;
 f = DPMI_DisableInterrupt();
 outportb(0x70,0x8B); b   = (unsigned char)inportb(0x71);
 outportb(0x70,0x80); sec = (unsigned char)inportb(0x71);
 DPMI_RestoreInterrupt(f);
 if(!(b & 0x40)){                                // PIE killed (the TH-class death)
  rtc_enable();
  LOW_PokeB(0x4F4, ++tel_revive);
  return;
 }
 if(inportb(0xA1) & 0x01){                       // IRQ8 masked at the slave PIC
  f = DPMI_DisableInterrupt();
  outportb(0xA1, (unsigned char)(inportb(0xA1) & ~0x01));
  DPMI_RestoreInterrupt(f);
  LOW_PokeB(0x4F4, ++tel_revive);
  return;
 }
 if(sec != wd_sec){                              // armed yet silent: confirm by
  wd_sec = sec;                                  // real elapsed time before the
  if(++wd_secchg >= 2){                          // PF-eating reg-C heal
   wd_secchg = 0;
   f = DPMI_DisableInterrupt();
   outportb(0x70,0x0C); (void)inportb(0x71);
   DPMI_RestoreInterrupt(f);
   LOW_PokeB(0x4F4, ++tel_revive);
  }
 }
}

// vsb.c hook: every guest DSP reset lands here. No ring flush: games reset
// per sound, and flushing the card means a re-arm. At most one latency
// target of stale audio plays out instead.
static void IB_PT_Watchdog(void)
{
 ib_watchdog();
 ib_pt_active = 0;
}

// Guest bytes the card accepts now: room up to the latency target, scaled
// back through the stepper to guest frames.
static int IB_PT_Space(void)
{
 unsigned q, unit;
 unsigned long room, g;
 if(!ib_armed) return 1024;                      // closed: the next feed arms it
 if(ib_busy) return 0;
 ib_busy = 1; ib_track(); q = IB_QUEUED(); ib_busy = 0;
 if(q >= ib_qtarget) return 0;
 room = IB_WORDS_TO_FRAMES(ib_qtarget - q);      // output frames
 if(!ib_pt_rate) return 1024;
 unit = (ib_pt_channels >= 2 ? 2u : 1u) * (ib_pt_bits >= 16 ? 2u : 1u);
 g = room * ib_pt_rate / ib_frate * unit;
 if(g > 16384UL) g = 16384UL;                    // int is 16 bits in the NOTFLAT build
 return (int)g;
}

// Feed raw guest PCM (sndisr.c tap): step it onto the codec rate, 16-bit
// stereo, and write it straight to the card.
static void IB_PT_Feed(const unsigned char *buf, int bytes, unsigned rate, unsigned bits, unsigned channels)
{
 unsigned unit, q, room;
 if(ib_busy){ ++ib_reentry;
#if !RATEDIAG && !IBSTDIAG
  LOW_PokeB(0x4F3, ib_reentry);                  // 0x4F3 on loan to RATEDIAG
#endif
  return; }
 ib_busy = 1;
 ++ib_tel_feed16;
 TEL_PokeB(0x4FB, (unsigned char)ib_tel_feed16);
 TEL_PokeB(0x4FF, (unsigned char)(ib_tel_feed16 >> 8));
 ib_feed_seq = ib_pt_seq = ib_tick_seq;
 if(!rate) rate = (unsigned)ib_frate;
 if(!ib_ensure_armed()){ ib_busy = 0; return; }
 if(!ib_pt_active || rate != ib_pt_rate || bits != ib_pt_bits || channels != ib_pt_channels){
  // A format change only re-aims the stepper: the codec keeps its clock.
  ib_pt_rate = rate; ib_pt_bits = bits; ib_pt_channels = channels;
  ib_step_acc = 0;
  ib_step_inv = (1UL << 24) / rate;
  ib_dsl = ib_dsr = 0; ib_dn = 0;
  ib_pt_active = 1;
#if IBSTDIAG
  ib_sd_reset();
#endif
#if !PTDIAG && !RATEDIAG
  LOW_PokeB(0x4F2, (unsigned char)(rate >> 8));  // guest rate >> 8 (pitch forensics)
#endif
 }
 ib_track();
 q = IB_QUEUED();
 room = q < IB_QMAX ? IB_WORDS_TO_FRAMES(IB_QMAX - q) : 0;   // output frames
 unit = (channels >= 2 ? 2u : 1u) * (bits >= 16 ? 2u : 1u);
 ib_tel_bytes += (unsigned long)bytes;
 while(bytes >= (int)unit){
  int l, r;
  if(bits >= 16){
   l = (int)(short)((unsigned)buf[0] | ((unsigned)buf[1] << 8));
   r = channels >= 2 ? (int)(short)((unsigned)buf[2] | ((unsigned)buf[3] << 8)) : l;
  }else{
   l = ((int)buf[0] - 128) << 8;
   r = channels >= 2 ? ((int)buf[1] - 128) << 8 : l;
  }
#if IBSTDIAG
  ib_sd_frame(l >> 8, r >> 8, channels, bits);
  if(!ib_ost && channels >= 2)                                   // mono out: downmix
   l = ib_sd_ch == 1 ? l : ib_sd_ch == 2 ? r : (int)(((long)l + r) >> 1);
#else
  if(!ib_ost && channels >= 2) l = (int)(((long)l + r) >> 1);   // mono out: downmix
#endif
  ib_step_acc += ib_frate;
  if(rate > ib_frate){
   // DECIMATING (guest faster than the codec): average every guest frame in
   // the output frame's interval instead of interpolating between two of
   // them. Point sampling folds everything above the codec's Nyquist back
   // into the audible band; averaging is a crude low-pass for one add per
   // guest frame and one divide per output frame. It also mixes an SB Pro
   // stereo stream that the guest sends to an SB 2.0 (T3): that arrives as
   // mono at twice the frame rate, alternating L,R -- point-sampled, the
   // L-R difference folded to a tone (Epic Pinball, 2026-09-29).
   ib_dsl += l; ib_dsr += r; ib_dn++;
   if(ib_step_acc >= rate){
    int ol, orr = 0;
    ib_step_acc -= rate;
    ol = (int)(ib_dsl / (long)ib_dn);
    if(ib_ost) orr = (int)(ib_dsr / (long)ib_dn);
    ib_dsl = ib_dsr = 0; ib_dn = 0;
    if(!room){                                   // card full: drop the rest
     LOW_PokeB(0x4FE, ++ib_tel_drop);
     bytes = 0;
     break;
    }
    ib_emit(ol, orr);
    room--;
   }
  }else
  // INTERPOLATING (guest at or below the codec rate): rate-to-frate output
  // frames per input frame on average. w runs 0..256 across the gap from the
  // previous guest frame to this one; at an exact rate match every frame
  // lands on w = 256, i.e. untouched. (The difference is formed in long: it
  // spans 17 bits, and int is 16 in the NOTFLAT build.)
  while(ib_step_acc >= rate){
   long w;
   int ol, orr = 0;
   ib_step_acc -= rate;
   w = 256L - (long)((ib_step_acc * ib_step_inv) >> 16);
   if(w < 0) w = 0;
   if(w > 256) w = 256;
   if(!ib_pvok) w = 256;                         // no previous frame yet
   ol = ib_pl + (int)((((long)l - (long)ib_pl) * w) >> 8);
   if(ib_ost) orr = ib_pr + (int)((((long)r - (long)ib_pr) * w) >> 8);
   if(!room){                                    // card full: drop the rest
    LOW_PokeB(0x4FE, ++ib_tel_drop);
    bytes = 0;
    break;
   }
   ib_emit(ol, orr);
   room--;
  }
  ib_pl = l; ib_pr = r; ib_pvok = 1;
  buf += unit; bytes -= (int)unit;
 }
 if(!SNDISR_HasTsc && !IBSTDIAG){
  unsigned u16 = (unsigned)((ib_tel_bytes >> 4) & 0xFFFF);
  TEL_PokeB(0x4FC, (unsigned char)u16);
  TEL_PokeB(0x4FD, (unsigned char)(u16 >> 8));
 }
 ib_busy = 0;
}

// ---- RTC (IRQ8) periodic: the pump clock (verbatim shape from sc_scp55.c) ---
static void rtc_enable(void)
{
 uint8_t f = DPMI_DisableInterrupt();
 outportb(0x70,0x8A); { unsigned char a=(unsigned char)inportb(0x71); outportb(0x70,0x8A); outportb(0x71,(a&0xF0)|ib_rtc_rs); }
 outportb(0x70,0x8B); { unsigned char b=(unsigned char)inportb(0x71); outportb(0x70,0x8B); outportb(0x71,b|0x40); }
 outportb(0x70,0x0C); inportb(0x71);
 DPMI_RestoreInterrupt(f);
}
static void rtc_disable(void)
{
 uint8_t f = DPMI_DisableInterrupt();
 outportb(0x70,0x8B); { unsigned char b=(unsigned char)inportb(0x71); outportb(0x70,0x8B); outportb(0x71,b&~0x40); }
 outportb(0x70,0x0C); inportb(0x71);
 DPMI_RestoreInterrupt(f);
}
static void ib_rtc_setrate(unsigned char rs)
{
 uint8_t f = DPMI_DisableInterrupt();
 outportb(0x70,0x8A); { unsigned char a=(unsigned char)inportb(0x71); outportb(0x70,0x8A); outportb(0x71,(a&0xF0)|(rs&0x0F)); }
 DPMI_RestoreInterrupt(f);
 ib_rtc_rs = rs;
}

// ---- IRQ0 heartbeat: guest-independent watchdog host (watchdog ONLY -- it
// runs on whatever stack the guest's INT8 was taken on; see sc_scp55.c) -----
static DPMI_ISR_HANDLE ib_i8_handle;
static int ib_i8_on;
static void ib_irq0_isr(void){ ib_watchdog(); }
static void ib_i8_install(void)
{
 if(ib_i8_on) return;
 if(getenv("ESNOI8")) return;                    // diagnostic kill-switch
 if(DPMI_InstallISR(0x08, &ib_irq0_isr, &ib_i8_handle, TRUE) != 0) return;
 ib_i8_on = 1;
}
static void ib_i8_remove(void)
{
 if(!ib_i8_on) return;
 DPMI_UninstallISR(&ib_i8_handle);
 ib_i8_on = 0;
}

// ---- live ISR nesting depth for the sndisr depth limiter (see sc_scp55.c:
// a NULL depth op short-circuits the limiter completely) --------------------
static volatile int ib_isr_depth;
static void ib_dbg_tick(void){ ib_isr_depth++; SNDISR_dbg_tick(); }
static void ib_dbg_exit(void){ SNDISR_dbg_exit(); if(ib_isr_depth) ib_isr_depth--; }
static int  IBMAUD_Depth(void){ return ib_isr_depth; }

// ==========================================================================
//  au_cards interface
// ==========================================================================
// NO PTF_REAL_FM: there is no FM silicon on this card (the vendor's MIDI is a
// software wavetable), so 0x388 is open bus; launch with /FMSHIM.
static const struct pt_ops_s ibmaud_pt_ops = {
 PTF_TAP,
 IB_PT_Space, IB_PT_Feed, IB_PT_Watchdog,
 ib_dbg_tick, ib_dbg_exit, SNDISR_dbg_reenter,
 IBMAUD_Depth,
};
// /RESAMP: no tap; the engine renders 16-bit stereo at the codec rate -- the
// card's own format, so writedata is a straight copy.
static const struct pt_ops_s ibmaud_render_ops = {
 0,
 NULL, NULL, IB_PT_Watchdog,
 ib_dbg_tick, ib_dbg_exit, SNDISR_dbg_reenter,
 IBMAUD_Depth,
 IB_RENDER_CAP,
 IB_RENDER_DIV,
};

static int IBMAUD_adetect(struct audioout_info_s *aui)
{
 ibmaud_card_s *card;
 const char *t = getenv("SBERTC");
 const char *l = getenv("SBEPTLAT");
 unsigned long ceil = 48000UL, q;
 unsigned st;
 int ri;
 if(!PTOPS_CardIs("ibmaud")) return 0;

 // /BASE is the card's 16-byte block (IBMAUDGO's /IO1); the 8-byte block
 // pairs with it by alias.
 if(FOpts.base) ib_b0 = (uint16_t)FOpts.base;
 ib_b1 = (uint16_t)((ib_b0 & 0xFC00u) | 0x340u);
 if(FOpts.maxhz) ceil = (unsigned long)FOpts.maxhz;
 ri = ib_rate_pick(FOpts.dacrate ? (unsigned long)FOpts.dacrate : DAC_RATE_DEF, ceil);
 ib_frate = ib_rates[ri].hz;
 { const char *e = getenv("SBEIBMST");            // stereo unless SBEIBMST=0
   ib_ost = !e || atoi(e) != 0; }
#if IBSTDIAG
 { const char *c = getenv("SBEIBMCH"); ib_sd_ch = c ? atoi(c) : 0; }
#endif
 ib_resamp = FOpts.resamp;
 // 16-bit where the engine mixes (/RESAMP: 16-bit frames, FM rendered at
 // full depth -- cut to 8 bits, note tails and fades came through grainy,
 // T2130CT 2026-09-30); 8-bit on the tap, whose SB guests are 8-bit already.
 { const char *e = getenv("SBEIBM16");            // SBEIBM16=0/1 overrides
   ib_o16 = e ? atoi(e) != 0 : ib_resamp; }
 ib_bpf = (ib_ost ? 2u : 1u) * (ib_o16 ? 2u : 1u);
 ib_sil = ib_o16 ? 0x0000u : 0x8080u;
 ib_mbits = (unsigned char)((ib_o16 ? 0x20 : 0) | (ib_ost ? 0x10 : 0));
 // data format: rate select, stereo bit, format (11 = 8-bit unsigned, 00 = 16-bit linear)
 ib_dfr = (unsigned char)((ib_rates[ri].dfs << 3) | (ib_ost ? 0x04 : 0) | (ib_o16 ? 0 : 0x03));
 ib_scr = (unsigned char)(0x86 | (ib_rates[ri].xtal << 4));  // crystal select
 if(FOpts.cvol >= 0) ib_att = (unsigned char)(FOpts.cvol & 0x3F);
 if(t){ int rs = atoi(t); if(rs >= 3 && rs <= 15){ ib_rtc_rs = (unsigned char)rs; ib_rtc_fixed = 1; } }
 if(l){ int ms = atoi(l); if(ms >= 20 && ms <= 1000) ib_lat_ms = (unsigned)ms; }

 // Queue geometry in card words. The pad floor is half the latency target:
 // the tap keeps the queue near the target, so the pad only fills in for a
 // guest that has stopped (or lost ticks), and the pump must visit at least
 // four times per floor -- 128 Hz down to a 32 ms floor, 256 Hz below it.
 { unsigned long wps = ib_frate * ib_bpf / 2UL;           // card words per second
   q = wps * ib_lat_ms / 1000UL;
   if(q < 256UL) q = 256UL;
   if(q > (unsigned long)(IB_QMAX - 512U)) q = IB_QMAX - 512U;
   ib_qtarget = (unsigned)q & ~1u;
   ib_qlow = (ib_qtarget / 2u) & ~1u;
   if(ib_qlow < 128u) ib_qlow = 128u;
   if(!ib_rtc_fixed && wps * 4UL > (unsigned long)ib_qlow * 128UL) ib_rs_run = 8; }
 { unsigned long hz = 32768UL >> ((ib_rtc_fixed ? ib_rtc_rs : ib_rs_run) - 1);
   ib_idle_ticks = hz * IB_IDLE_MS / 1000UL; }
 { unsigned long want = ib_frate * ib_lat_ms / 1000UL;   // /RESAMP buffer, frames
   unsigned f = 256;
   while((unsigned long)(f << 1) <= want
         && (unsigned long)(f << 1) * ib_bpf / 2UL <= IB_QMAX / 2u) f <<= 1;
   ib_virt_frames = f; }

 // The card must already decode (IBMAUDGO first): an empty window reads
 // FFFFh, and the status word has nothing above bit 8.
 st = ib_inw(IB_CMD);
 if(st == 0xFFFFu || (st & 0xFE00u)){
  printf("IBMAUD: no card at %3.3Xh/%3.3Xh -- run IBMAUDGO first, and check /BASE\n",
         (unsigned)ib_b0, (unsigned)ib_b1);
  return 0;
 }
 card = (ibmaud_card_s *)aui->card_private_data;  // v2.0: engine-allocated
 card->base = ib_b0;
 aui->card_irq = 8;                               // RTC drives the pump
 PTOPS_Register(ib_resamp ? &ibmaud_render_ops : &ibmaud_pt_ops);
 // One console line (79 columns at most; worst case 77: E50h, 48000 Hz,
 // a 1000 ms queue and SBERTC=3). The queue is shown in ms: under /RESAMP the engine's
 // buffer, otherwise the card-side latency target.
 { unsigned long wps = ib_frate * ib_bpf / 2UL;           // card words per second
   unsigned ms = (unsigned)(ib_resamp ? (unsigned long)ib_virt_frames * 1000UL / ib_frate
                                      : (unsigned long)ib_qtarget * 1000UL / wps);
   printf("IBMAUD %3.3Xh/%3.3Xh: %lu Hz %s %s, queue %u ms, pump %u Hz%s\n",
          (unsigned)ib_b0, (unsigned)ib_b1, ib_frate, ib_o16 ? "16-bit" : "8-bit",
          ib_ost ? "stereo" : "mono", ms,
          (unsigned)(32768UL >> ((ib_rtc_fixed ? ib_rtc_rs : ib_rs_run) - 1)),
          ib_resamp ? ", RESAMP" : ""); }
#if IBSTDIAG
 printf("IBMAUD: STEREO DIAG build, downmix = %s\n",
        ib_sd_ch == 1 ? "LEFT only" : ib_sd_ch == 2 ? "RIGHT only" : "L+R mix");
#endif
 return 1;
}

static void IBMAUD_setrate(struct audioout_info_s *aui)
{
 ibmaud_card_s *card = aui->card_private_data;
 aui->freq_card = ib_frate;
 aui->chan_card = 2;
 aui->bits_card = 16;
 aui->card_dmasize = (unsigned long)ib_virt_frames * BYTES_PER_SBSAMPLE;
 ib_b0 = card->base;
 ib_b1 = (uint16_t)((ib_b0 & 0xFC00u) | 0x340u);
}

static void IBMAUD_start(struct audioout_info_s *aui)
{
 (void)aui;
 ib_i8_install();
 ib_pt_active = 0;
 ib_busy = 1;
 // The render path is fed continuously, so arm now; the passthrough path
 // arms on its first feed and closes again when the guest goes quiet.
 if(ib_resamp && !ib_armed) (void)ib_arm();
 ib_busy = 0;
 if(!ib_rtc_fixed) ib_rtc_rs = ib_armed ? ib_rs_run : IB_RS_IDLE;
 rtc_enable();
}

static void IBMAUD_stop(struct audioout_info_s *aui)
{
 // Quiesce only: pump + heartbeat stay until close(); an armed card pads
 // silence and closes itself after IB_IDLE_MS.
 (void)aui;
 ib_pt_active = 0;
}

static void IBMAUD_close(struct audioout_info_s *aui)
{
 (void)aui;
 rtc_disable();
 ib_i8_remove();
 ib_busy = 1;
 ib_close();
 ib_busy = 0;
}

// /RESAMP render path: engine frames are the card's format (16-bit stereo),
// so they go straight to the card, clamped to the ring's safe depth.
// Passthrough builds land here too, for what the tap does not take (direct
// DAC, ADPCM); there the engine hands over a fixed PT_MODE_SAMPLES frames a
// tick whatever the card can take, so the write stops at the latency target
// instead of the hard cap. (Direct DAC under the tap also carries the engine's
// parked rate estimate -- see doc/NOTES.md -- which no backend can repair.)
// ENGINE WRITE POINTER: AU_writedata advances card_dmalastput itself (v2.0).
static void IBMAUD_writedata(struct audioout_info_s *aui, char *src, unsigned int bytes)
{
 const short *p = (const short *)src;
 unsigned n = bytes / BYTES_PER_SBSAMPLE, q, room, cap = ib_resamp ? IB_QMAX : ib_qtarget;
 (void)aui;
 if(ib_busy) return;                             // one writer
 // One producer: a live tap stream owns the card. ib_pt_active alone would
 // latch until the next guest DSP reset -- sc_scp55's shape -- and silence
 // an ADPCM or direct-DAC sound that follows a DMA one without a reset.
 if(ib_pt_active && ib_tick_seq - ib_pt_seq < IB_PT_HOLD) return;
 ib_busy = 1;
 // Passthrough builds also land here for what the tap does not take
 // (direct DAC, ADPCM): that is live audio too, so it holds off the idle close.
 ib_feed_seq = ib_tick_seq;
 if(!ib_ensure_armed()){ ib_busy = 0; return; }
 ib_track();
 q = IB_QUEUED();
 room = q < cap ? IB_WORDS_TO_FRAMES(cap - q) : 0;
 if(n > room) n = room;
 while(n--){                                     // engine frames are 16-bit stereo
  int l = p[0], r = p[1];
  p += 2;
  if(!ib_ost) l = (int)(((long)l + r) >> 1);
  ib_emit(l, r);
 }
 ib_busy = 0;
}

static unsigned int IBMAUD_getbufpos(struct audioout_info_s *aui)
{
 (void)aui;
 // play position inside the VIRTUAL buffer the engine was shown (a power of
 // two, so it stays continuous across the counter's own wrap)
 return (unsigned int)((((ib_cons * 2UL) / ib_bpf) & (ib_virt_frames - 1)) * BYTES_PER_SBSAMPLE);
}

// IRQ8/RTC: ack, pump, settle the pump rate, claim the interrupt.
static int IBMAUD_irq(struct audioout_info_s *aui)
{
 (void)aui;
 TEL_PokeB(0x4F8, ++ib_tel_irq);
 ++ib_tick_seq;
 ib_watchdog();
 // ack RTC (runs before sndisr enables interrupts: no cli pair needed)
 outportb(0x70,0x0C); (void)inportb(0x71);
 ib_pump();
 if(!ib_rtc_fixed){
  unsigned char want = ib_armed ? ib_rs_run : IB_RS_IDLE;
  if(ib_rtc_rs != want) ib_rtc_setrate(want);
 }
 return 1;                                        // every IRQ8 is ours by construction
}

// VSBHDA sndcard_info_s: 14 fields, no mixer slots.
struct sndcard_info_s IBMAUD_sndcard_info={
 "IBMAUD",                                         // shortname (display only;
                                                  // /CARD:IBMAUD matches via PTOPS_CardIs)
 0,                                               // infobits
 &IBMAUD_adetect,                                 // card_detect
 &IBMAUD_start, &IBMAUD_stop, &IBMAUD_close,      // start / stop / close
 &IBMAUD_setrate,                                 // card_setrate
 &IBMAUD_writedata, &IBMAUD_getbufpos,            // writedata / getpos
 &IBMAUD_irq,                                     // irq_routine
 NULL, NULL, NULL,                                // mixer slots
 sizeof(ibmaud_card_s)                            // private_data_size
};

#endif // NOIBMAUD
