//**************************************************************************
//*  sc_vew211.c - VSBPCMCIA output driver for the Crystal CS4231A codec,
//*  as found on the Panasonic CF-VEW211 (and CF-VEW212 / PC-9801N-J04)
//*  PCMCIA sound card.
//*
//*  (C) copyright 2026 zikolas
//*  Ported from the rex5571-sbemu vew211-backend branch onto VSBHDA's
//*  au_cards interface, (C) PDSoft (Attila Padar).
//*
//*  This is free software: you may redistribute it and/or modify it under
//*  the terms of the GNU General Public License version 2 as published by
//*  the Free Software Foundation. Distributed WITHOUT ANY WARRANTY. See the
//*  COPYING file at the root of this project.
//**************************************************************************
//  Same problem the ES1688 driver solves, different chip: the PCMCIA bridge
//  has no ISA DMA to the socket, so the CS4231A can't service DMA-driven SB
//  playback. VSBPCMCIA emulates the SB, taps the raw guest stream, and we
//  push it to the real CS4231A by PIO.
//
//  *** OUTPUT PATH: CS4231A PIO, RTC-tick-credit-paced (NOT flag-gated) ***
//  The CS4231A has a 16-sample playback FIFO clocked by its own crystal at
//  the programmed rate. On the CF-VEW211's CS4231A-KQ behind the MEI ASIC,
//  the PRDY flag is USELESS for pacing -- it reads "ready" continuously while
//  overfeed is silently dropped. So the feed is paced by ABSOLUTE TIME:
//    * each delivered RTC tick grants exactly one RTC period's worth of
//      frames at the CODEC's actual table rate (not the guest's nominal
//      rate -- the 10989-asked/11025-played mismatch otherwise underruns
//      ~36x/s into a steady crackle); the pump spends the credit, capped
//      at the 16-deep FIFO per pass.
//    * the timebase is the RTC period, NOT PIT channel 0 (the earlier
//      VEWPLAY design): the PIT is GUEST hardware -- games reprogram ch0's
//      mode and reload for their own timers, which rescales the decrement
//      rate and moves the wrap point, so a PIT-side accumulator gains a
//      huge bogus "elapsed" at every guest-timer wrap and the pump bursts
//      ahead of real time. Overfeed into a full FIFO is silently DROPPED
//      -> sample decimation = fast, pitched-up, crackling playback in
//      exactly the games that reprogram the timer. The RTC periodic is
//      OURS (reg A; rate = 32768 >> (rs-1), crystal-exact) and needs no
//      boot-time calibration.
//    * DATASHEET R3: a sample COMMITS to the FIFO only when the Status
//      register is read after its bytes are written -> outp(PDR); inp(SR)
//      per byte, or total silence.
//
//  *** CODEC BRING-UP: ms-paced MCE + read-back verify/retry ***
//  The CS4231A-KQ silently drops microsecond-paced MCE register writes from
//  a cold codec; every MCE sequence is ms-paced and verified with retry.
//  Format changes cost ~25 ms -- which is why same-format stream resumes
//  FAST-RESUME past the whole sequence (see ES1688_PT_Feed).
//
//  Timers: RTC (IRQ8, card_irq=8) is the pump clock, self-pacing 32..2048 Hz
//  (2048 covers the 22.05 kHz design ceiling; 44.1k is out of scope -- 486
//  hosts, SB-era guests). A chained IRQ0 heartbeat revives a dead RTC. The
//  CS4231A in PIO mode needs no card-IRQ servicing (no ES1688-style TC IRQ).
//
//  Layout: I/O window base 0x530 (CIS idx 0x20; /BASE overrides); the
//  CS4231A answers at base+4..+7 (IAR/IDR/SR/PDR). FM is a DISCRETE YMF262
//  (OPL3) at 0x388 decoding all four ports natively: with NOFM the guest's
//  AdLib I/O reaches it untrapped -- real OPL3, no relay needed. The card
//  must be brought up by VEW21XGO first.
//
//  BACKEND SELECTION: /CARD:VEW211 on the command line. This backend is
//  compiled into the default build alongside sc_es1688.c and sc_tp755.c;
//  the passthrough ABI they once shared is dispatched through ptops.h.
//  Nothing is probed -- a backend runs only when it is named.
//**************************************************************************
#ifndef NOVEW211

#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <go32.h>
#include <sys/farptr.h>   // telemetry pokes into the BIOS IAC area (0x4F0)
#include <dpmi.h>         // _go32_dpmi_* : the IRQ0 watchdog heartbeat
#include <stdlib.h>
#include <pc.h>
#include "au_cards.h"
#include "ptops.h"        // engine passthrough ops table (we register in adetect)

// ==========================================================================
//  crazii DPMI-API compat shim over DJGPP (same as sc_es1688.c)
// ==========================================================================
#ifndef TRUE
#define TRUE  1
#define FALSE 0
#endif
typedef struct { int intno; _go32_dpmi_seginfo si, oldsi; } DPMI_ISR_HANDLE;
static int DPMI_InstallISR(int intno, void (*isr)(void), DPMI_ISR_HANDLE *h, int chain)
{
 h->intno = intno;
 if(_go32_dpmi_get_protected_mode_interrupt_vector(intno, &h->oldsi)) return -1;
 h->si.pm_offset = (unsigned long)isr;
 h->si.pm_selector = _go32_my_cs();
 if(chain)
  return _go32_dpmi_chain_protected_mode_interrupt_vector(intno, &h->si) ? -1 : 0;
 if(_go32_dpmi_allocate_iret_wrapper(&h->si)) return -1;
 return _go32_dpmi_set_protected_mode_interrupt_vector(intno, &h->si) ? -1 : 0;
}
static void DPMI_UninstallISR(DPMI_ISR_HANDLE *h)
{ _go32_dpmi_set_protected_mode_interrupt_vector(h->intno, &h->oldsi); }
static uint8_t DPMI_DisableInterrupt(void)
{
 unsigned f;
 __asm__ __volatile__("pushfl; popl %0" : "=r"(f));
 __asm__ __volatile__("movw $0x0900,%%ax; int $0x31" ::: "eax","cc");
 return (f & 0x200) ? 1 : 0;
}
static void DPMI_RestoreInterrupt(uint8_t prev)
{ if(prev) __asm__ __volatile__("movw $0x0901,%%ax; int $0x31" ::: "eax","cc"); }

#define pds_calloc            calloc
#define pds_free              free

// ==========================================================================
//  Passthrough ABI globals + telemetry (layout identical to sc_es1688.c;
//  see doc/NOTES.md for the 0x4F0-0x4FF map)
// ==========================================================================
// The SNDISR entry/depth/duration telemetry and the tap-gate/reentrancy
// globals moved to sndisr.c with the generic dbg trio (see ptops.h); this
// backend keeps only its own bytes of the 0x4F0-0x4FF map:
//   0x4F8 = irq_routine calls, 0x4FB/0x4FF = 16-bit feed count,
//   0x4FE = ring-full feed clamps, 0x4F4 = watchdog revivals.
static uint16_t es_tel_feed16;
static unsigned long es_tel_bytes;
static unsigned char es_tel_irq;

// ---- card geometry -------------------------------------------------------
#define VEW_WIN_BASE  0x530         // I/O window base (CIS idx 0x20); /BASE overrides
#define VEW_CODEC_OFF  4            // CS4231A sits at window base + 4
#define VC_IAR  0                   // Index Address Register (bit6 = MCE)
#define VC_IDR  1                   // Indexed Data Register
#define VC_SR   2                   // Status Register (read commits a PIO sample)
#define VC_PDR  3                   // Playback Data Register (PIO)
#define I8_STEREO 0x10
#define I8_16BIT  0x40
#define I9_PEN    0x01
#define I9_ACAL   0x08
#define I9_PPIO   0x40
#define IAR_MCE   0x40
#define I11_ACI   0x20

// ---- ring + pump (shared design with sc_es1688.c) ------------------------
#define RING_BYTES    8192U
#define RING_MASK     (RING_BYTES-1)
#define BYTES_PER_SBSAMPLE 4
#define DAC_RATE_DEF  22050
#define DAC_RATE_MIN  4000
#define DAC_RATE_MAX  48000
#define VEW_RS_MIN    5             // fastest pump = 2048 Hz (22.05k design ceiling)
#define VEW_RS_IDLE   11            // idle keep-alive = 32 Hz
#define VEW_RING_LOW  256
#define VEW_BURST_FRAMES 16         // per-pass ceiling = FIFO depth
// /RESAMP pacing. The pump ISR is also the engine's render tick, so a pass
// must fit inside one tick: at the FIXED 1024 Hz below, 22050 Hz needs ~22
// frames per tick, so 128 is ~6x headroom while keeping each pass cheap.
// The pump also stays fast because the codec FIFO is only 16 frames deep
// (~0.7 ms at 22 kHz) -- which is exactly why the rate cannot simply be
// lowered to TP755's ~43 Hz period model.
// Pump stays at 1024 Hz (the 16-frame FIFO demands it); the RENDER runs on
// one tick in 16 = 64 Hz, close to sc_tp755's 43 Hz period model. At 22050 Hz
// a 64 Hz render interval needs ~345 frames, so cap 512 leaves headroom and
// still bounds a single pass well below one pump tick's worth of work.
#define VEW_RENDER_DIV   16
#define VEW_RENDER_CAP   512
#define VEW_RENDER_RS    6          // 32768>>5 = 1024 Hz, fixed (not adaptive)

typedef struct vew211_card_s { uint16_t base; } vew211_card_s;

// OWNERSHIP: ring_rd is written ONLY by the pump (serialized by the busy
// guard); trap-context code requests flushes via es_flush_gen and the pump
// snaps rd := wr itself (the sample-boundary race lesson from sc_es1688.c).
static volatile unsigned ring_wr, ring_rd;
static volatile unsigned es_flush_gen, es_flush_ack;
static unsigned char      ring_buf[RING_BYTES];
static uint16_t           vew_base   = VEW_WIN_BASE;
static uint16_t           vew_codec  = VEW_WIN_BASE + VEW_CODEC_OFF;
static unsigned           vew_dacrate = DAC_RATE_DEF;
static int                vew_vol     = -1;           // SBEVOL attenuation 0..63 (-1 = keep enabler's)
static unsigned char      vew_rtc_rs  = VEW_RS_IDLE;  // current armed RTC pump rate
static unsigned char      vew_rs_want = VEW_RS_IDLE;  // the STREAM's proper pump rate; PT_Feed
                                                      // restores it after an idle throttle (the
                                                      // mid-session chop lesson from sc_es1688.c)
static int                vew_adaptive;

static void rtc_enable(void);
static void vew_rtc_setrate(unsigned char rs);
static unsigned char vew_rs_for_frate(unsigned frames_per_sec);
// Time is kept in OUR OWN RTC tick count -- NEVER the BIOS tick at 0x46C.
// Games own the timer chain: Lion King hooks INT8 without chaining (0x46C
// freezes solid, measured on the 235), fast-timer games advance it several
// times too fast. Any 0x46C-keyed decision (idle detection, watchdog gap)
// misfires exactly while a game is running.
static volatile uint32_t vew_tick_seq;   // ++ per delivered RTC tick
static volatile uint32_t vew_feed_seq;   // vew_tick_seq at the last PT_Feed
static int                vew_pt_ever;

// ---- RTC-tick-credit pacing ----------------------------------------------
// Credit is granted in VEW211_irq (one RTC period of frames per delivered
// tick) and spent by the pump. Units: frames * rtc_hz -- one frame costs
// rtc_hz, one tick earns vew_frate. Guest-independent (see header).
static unsigned long vew_frate = DAC_RATE_DEF;  // codec TABLE rate, frames/s
static unsigned long vew_fr_acc;                // pacing credit
#define VEW_RTC_HZ() (32768UL >> (vew_rtc_rs - 1))

// ---- CS4231A indexed-register helpers ------------------------------------
static void vew_iodelay(unsigned n){ while(n--) (void)inportb(0x80); }
#define VEW_MS(x) vew_iodelay((unsigned)(x) * 1000U)
static void vew_ci_wait(uint16_t cb)
{
 unsigned long i; for(i=0;i<400000UL;i++) if(!(inportb(cb+VC_IAR) & 0x80)) return;
}
static void vew_ci_put(uint16_t cb, unsigned char idx, unsigned char v)
{
 vew_ci_wait(cb); outportb(cb+VC_IAR, idx); vew_iodelay(200);
 outportb(cb+VC_IDR, v); vew_iodelay(200);
}
static unsigned char vew_ci_get(uint16_t cb, unsigned char idx)
{
 vew_ci_wait(cb); outportb(cb+VC_IAR, idx); vew_iodelay(200);
 return (unsigned char)inportb(cb+VC_IDR);
}

// ---- passthrough state ---------------------------------------------------
static volatile int vew_pt_active;
static unsigned vew_pt_rate, vew_pt_bits, vew_pt_channels;
static unsigned vew_hw_rate, vew_hw_bits, vew_hw_channels;  // what the CODEC is
                                                            // programmed for (0 =
                                                            // not armed / stopped)
static unsigned char vew_tel_recfg;
static volatile int vew_pump_busy;

static const struct { unsigned long hz; unsigned char code; } vew_rates[] = {
 { 5510UL,0x01},{ 6620UL,0x0F},{ 8000UL,0x00},{ 9600UL,0x0E},
 {11025UL,0x03},{16000UL,0x02},{18900UL,0x05},{22050UL,0x07},
 {27042UL,0x04},{32000UL,0x06},{33075UL,0x0D},{37800UL,0x09},
 {44100UL,0x0B},{48000UL,0x0C}
};
#define VEW_NRATES (int)(sizeof(vew_rates)/sizeof(vew_rates[0]))
// Picks are capped at the 22.05k design ceiling: the pump can sustain at
// most 2048 Hz x 16 frames = 32768 frames/s, so entries above 22050 can
// never be fed continuously -- a 44.1k guest maps to 22050 and the frame
// stepper (below) decimates 2:1: correct pitch and tempo, half bandwidth.
#define VEW_RATE_CEIL 22050UL
static int vew_rate_pick(unsigned rate)
{
 int i, best = 7; unsigned long bd = 0xFFFFFFFFUL;
 for(i=0;i<VEW_NRATES;i++){
  unsigned long d;
  if(vew_rates[i].hz > VEW_RATE_CEIL) continue;
  d = vew_rates[i].hz > rate ? vew_rates[i].hz - rate : rate - vew_rates[i].hz;
  if(d < bd){ bd = d; best = i; }
 }
 return best;
}

// ---- nearest-neighbour frame stepper -------------------------------------
// The CS4231A has 14 fixed crystal-divided rates and nothing in between,
// while SB guests derive arbitrary rates from the time constant; nearest-
// match alone leaves rates like 20000 or 25000 playing 5-8% flat/sharp --
// and playing flat also drags the whole game (ring backpressure throttles
// the guest to the codec's pace). When the mismatch exceeds ~2%, PT_Feed
// re-steps the guest stream frame-wise onto the codec rate with a
// Bresenham accumulator: whole frames are dropped or duplicated during
// the ring copy that happens anyway. No interpolation, no per-sample
// math -- a compare and an add per frame, 486-priced. Exact/near-table
// streams (all the validated titles) keep the raw untouched path.
// SBENORS=1 disables the stepper (A/B).
static int           vew_no_step;      // env SBENORS
static int           vew_step_on;      // engaged for the current format
static unsigned long vew_step_in;      // guest frames/s
static unsigned long vew_step_acc;     // Bresenham accumulator

// Program the codec for {rate,bits,channels} in PIO mode; ms-paced MCE with
// verify/retry (the CS4231A-KQ drops fast cold writes). ~25 ms.
static void vew_codec_config(unsigned rate, unsigned bits, unsigned channels)
{
 uint16_t cb = vew_codec;
 unsigned char i8;
 int tries, ri;
 if(rate < DAC_RATE_MIN) rate = DAC_RATE_MIN;
 if(rate > DAC_RATE_MAX) rate = DAC_RATE_MAX;
 ri = vew_rate_pick(rate);
 i8 = (unsigned char)(vew_rates[ri].code
       | (channels >= 2 ? I8_STEREO : 0)
       | (bits >= 16    ? I8_16BIT  : 0));

 for(tries = 0; tries < 8; tries++){
  vew_ci_wait(cb);
  outportb(cb+VC_IAR, (unsigned char)(IAR_MCE|0x08)); VEW_MS(2);
  outportb(cb+VC_IDR, i8);                            VEW_MS(2);
  outportb(cb+VC_IAR, (unsigned char)(IAR_MCE|0x09)); VEW_MS(2);
  outportb(cb+VC_IDR, (unsigned char)(I9_PPIO|I9_ACAL)); VEW_MS(2);
  outportb(cb+VC_IAR, 0x00);                          VEW_MS(2);
  vew_ci_wait(cb); VEW_MS(10);
  { unsigned long w; for(w=0;w<400000UL;w++) if(!(vew_ci_get(cb,0x0B) & I11_ACI)) break; }
  if(vew_ci_get(cb,0x08) == i8 && (vew_ci_get(cb,0x09) & (I9_PPIO|I9_ACAL)) == (I9_PPIO|I9_ACAL))
   break;
 }
 if(vew_vol >= 0){
  vew_ci_put(cb, 0x06, (unsigned char)(vew_vol & 0x3F));
  vew_ci_put(cb, 0x07, (unsigned char)(vew_vol & 0x3F));
 }else{
  vew_ci_put(cb, 0x06, (unsigned char)(vew_ci_get(cb,0x06) & 0x3F));   // un-mute, keep level
  vew_ci_put(cb, 0x07, (unsigned char)(vew_ci_get(cb,0x07) & 0x3F));
 }
 vew_ci_put(cb, 0x0F, 0xFE); vew_ci_put(cb, 0x0E, 0xFF);
 for(tries = 0; tries < 8; tries++){
  vew_ci_put(cb, 0x09, (unsigned char)(I9_PPIO|I9_ACAL|I9_PEN)); VEW_MS(2);
  if(vew_ci_get(cb,0x09) & I9_PEN) break;
 }
 vew_pt_rate = rate; vew_pt_bits = bits; vew_pt_channels = channels;
 vew_hw_rate = rate; vew_hw_bits = bits; vew_hw_channels = channels;
 // Pace at the rate the codec will actually PLAY (the table entry).
 vew_frate = vew_rates[ri].hz;
 vew_fr_acc = 0;
 // Arm the frame stepper when the guest rate misses the table by >2%.
 vew_step_in  = rate;
 vew_step_acc = 0;
 { unsigned long d = vew_frate > rate ? vew_frate - rate : rate - vew_frate;
   vew_step_on = (!vew_no_step && d * 50UL > (unsigned long)rate) ? 1 : 0; }
 _farpokeb(_dos_ds, 0x4F2, (unsigned char)(rate >> 8));  // guest rate >> 8 (pitch-bug forensics)
 _farpokeb(_dos_ds, 0x4FA, ++vew_tel_recfg);          // FULL reconfigs only
}
static void vew_codec_stop(void)
{
 uint16_t cb = vew_codec;
 unsigned char sil = (vew_pt_active && vew_pt_bits >= 16) ? 0x00 : 0x80;
 int i;
 for(i=0;i<64;i++){ vew_iodelay(60); outportb(cb+VC_PDR, sil); (void)inportb(cb+VC_SR); }
 VEW_MS(2);
 vew_ci_put(cb, 0x09, 0x00);                          // clear PEN
 vew_hw_rate = vew_hw_bits = vew_hw_channels = 0;     // codec disarmed: no fast-resume
}

// drain ring -> codec FIFO, RTC-credit-paced. Called from the RTC tick
// (which grants the credit first) AND inline from PT_Feed (which only
// spends leftovers -- no time source of its own, so no double-credit).
static void vew_pio_pump(void)
{
 uint16_t cb = vew_codec;
 unsigned unit = 1;
 unsigned char sil;
 unsigned long hz;
 int guard = 0;
 if(vew_pump_busy) return;
 vew_pump_busy = 1;
 if(es_flush_gen != es_flush_ack){                    // deferred ring flush (trap-context requests)
  es_flush_ack = es_flush_gen;
  ring_rd = ring_wr;
 }

 if(vew_pt_active){
  unit = (vew_pt_channels >= 2) ? 2 : 1;
  if(vew_pt_bits >= 16) unit *= 2;
  if(unit > 4) unit = 4;
 }
 sil = (vew_pt_active && vew_pt_bits >= 16) ? 0x00 : 0x80;

 hz = VEW_RTC_HZ();
 // TICK-LOSS RECOVERY off the codec's own starvation flag. Per-tick credit
 // is guest-proof but lost RTC periods (a game's cli bursts, a saturating
 // game-timer ISR -- LK fades, EP in-game) are lost credit: the pump then
 // chronically underfeeds by the loss fraction and the codec stretches
 // samples = steady pitch sag. SER (SR bit 4) asserts whenever the DAC
 // missed a sample since the last SR WRITE cleared it (reads don't clear --
 // VEWPIO-proven on this card), and it is clocked by the codec crystal: a
 // guest-proof underrun detector. Starved -> the FIFO is empty by
 // definition, so a full 16-frame refill always fits: snap the credit to a
 // full burst. Average delivery stays locked to the codec clock as long as
 // delivered ticks * burst >= the frame rate (33% headroom at the ceiling).
 { unsigned char sr = (unsigned char)inportb(cb+VC_SR);
   if(vew_pt_active && (sr & 0x10)){
    static unsigned char vew_tel_ser;
    vew_fr_acc = hz * VEW_BURST_FRAMES;
    _farpokeb(_dos_ds, 0x4F6, ++vew_tel_ser);         // SER catch-up count
   }
   outportb(cb+VC_SR, 0); }                           // clear SER/INT for the next interval
 while(vew_fr_acc >= hz && guard < VEW_BURST_FRAMES){
  unsigned rd = ring_rd, wr = ring_wr, u;
  if(((wr - rd) & RING_MASK) >= unit){
   for(u=0;u<unit;u++){ outportb(cb+VC_PDR, ring_buf[rd]); (void)inportb(cb+VC_SR); rd = (rd+1)&RING_MASK; }
   ring_rd = rd;
  }else{
   for(u=0;u<unit;u++){ outportb(cb+VC_PDR, sil); (void)inportb(cb+VC_SR); }
  }
  vew_fr_acc -= hz;
  guard++;
 }
 vew_pump_busy = 0;
}

// ==========================================================================
//  Passthrough backend ABI (ES1688_PT_* names are the engine's ABI --
//  card-agnostic; here they drive the CS4231A).
// ==========================================================================
// RTC-death watchdog. The old version compared 0x46C BIOS ticks and blindly
// re-armed on a >=110ms gap -- under Lion King's timer hooking it fired 141
// times during the LOAD SCREEN alone (measured), and every spurious
// rtc_enable() reads register C, which can eat a pending PF and STEAL the
// very tick it claims to guard: lost pump ticks = underfeed = the codec
// stretches samples = pitch sags + fuzz. Redesign: trigger on OUR tick
// counter going stale, then VERIFY the arm state and fix only what is
// provably wrong. The one destructive heal (the reg-C read that clears a
// latched-PF wedge) is gated behind 1-2 s of confirmed silence measured by
// the RTC's own seconds register -- the only guest-proof wall clock here.
static void vew_watchdog(void)
{
 static unsigned char es_tel_revive;
 static uint32_t wd_seq;
 static unsigned char wd_stale, wd_sec, wd_secchg;
 unsigned char b, sec;
 uint8_t f;
 uint32_t seq = vew_tick_seq;
 if(seq != wd_seq){ wd_seq = seq; wd_stale = 0; wd_secchg = 0; return; }
 if(++wd_stale < 2) return;                           // debounce one visit
 wd_stale = 0;
 f = DPMI_DisableInterrupt();
 outportb(0x70,0x8B); b   = (unsigned char)inportb(0x71);
 outportb(0x70,0x80); sec = (unsigned char)inportb(0x71);
 DPMI_RestoreInterrupt(f);
 if(!(b & 0x40)){                                     // PIE killed (the TH-class death)
  rtc_enable();
  _farpokeb(_dos_ds, 0x4F4, ++es_tel_revive);
  return;
 }
 if(inportb(0xA1) & 0x01){                            // IRQ8 masked at the slave PIC
  f = DPMI_DisableInterrupt();
  outportb(0xA1, (unsigned char)(inportb(0xA1) & ~0x01));
  DPMI_RestoreInterrupt(f);
  _farpokeb(_dos_ds, 0x4F4, ++es_tel_revive);
  return;
 }
 if(sec != wd_sec){                                   // armed yet silent: confirm by
  wd_sec = sec;                                       // real elapsed time before the
  if(++wd_secchg >= 2){                               // PF-eating reg-C heal
   wd_secchg = 0;
   f = DPMI_DisableInterrupt();
   outportb(0x70,0x0C); (void)inportb(0x71);
   DPMI_RestoreInterrupt(f);
   _farpokeb(_dos_ds, 0x4F4, ++es_tel_revive);
  }
 }
}

// vsb.c hook: every guest DSP RESET lands here.
static void ES1688_PT_Watchdog(void)
{
 vew_watchdog();
 es_flush_gen++;                                      // pump-executed ring flush
 vew_pt_active = 0;
 vew_pt_rate = vew_pt_bits = vew_pt_channels = 0;
}

static unsigned vew_pt_lat_ms = 250;
static int ES1688_PT_Space(void)
{
 unsigned used = (ring_wr - ring_rd) & RING_MASK;
 unsigned target = RING_BYTES - 64;
 if(vew_pt_rate){
  // The ring holds OUTPUT-rate bytes (the stepper may have re-stepped the
  // guest stream), so the latency target is sized from the codec rate.
  unsigned bps = (unsigned)vew_frate * vew_pt_channels * ((vew_pt_bits + 7) / 8);
  target = (unsigned)((unsigned long)bps * vew_pt_lat_ms / 1000UL);
  if(target > RING_BYTES - 64) target = RING_BYTES - 64;
  if(target < 512) target = 512;
 }
 if(used >= target) return 0;
 { unsigned space = target - used;
   // The caller counts GUEST bytes; the stepper shrinks (or grows) them on
   // the way into the ring. Convert output-byte room to the guest bytes
   // that will fill it, or a decimated 44.1k guest would be throttled to
   // half its real-time rate by its own bandwidth savings.
   if(vew_step_on)
    space = (unsigned)((unsigned long)space * vew_step_in / vew_frate);
   return (int)space; }
}

// feed raw guest PCM (sndisr.c tap); reconfigure the codec on format change.
static volatile int vew_pt_feed_busy;
static unsigned char es_reentry;
static unsigned char es_tel_drop;                     // 0x4FE: ring-full feed clamps
static void ES1688_PT_Feed(const unsigned char *buf, int bytes, unsigned rate, unsigned bits, unsigned channels)
{
 unsigned wr;
 if(vew_pt_feed_busy){ _farpokeb(_dos_ds, 0x4F3, ++es_reentry); return; }
 vew_pt_feed_busy = 1;
 ++es_tel_feed16;
 _farpokeb(_dos_ds, 0x4FB, (unsigned char)es_tel_feed16);
 _farpokeb(_dos_ds, 0x4FF, (unsigned char)(es_tel_feed16 >> 8));
 vew_feed_seq = vew_tick_seq;
 // Waking from the idle throttle: between same-format sounds no reconfig
 // runs, so restore the stream's pump rate HERE (rate-up only).
 if(vew_adaptive && vew_rtc_rs > vew_rs_want) vew_rtc_setrate(vew_rs_want);
 if(!vew_pt_active || rate != vew_pt_rate || bits != vew_pt_bits || channels != vew_pt_channels)
 {
  // FAST RESUME: a guest DSP reset per sound clears the stream state via
  // PT_Watchdog, but the codec usually needs nothing -- still programmed for
  // this exact format with PEN running (a stalled pump only underruns the
  // FIFO, which recovers by feeding; codec_stop is the only real disarm and
  // it clears vew_hw_*). The full MCE sequence costs ~25 ms per call, so
  // skipping it per-SFX matters even more here than on the ES1688.
  if(rate == vew_hw_rate && bits == vew_hw_bits && channels == vew_hw_channels){
   vew_pt_rate = rate; vew_pt_bits = bits; vew_pt_channels = channels;
  }else{
   uint8_t f = DPMI_DisableInterrupt();
   vew_codec_config(rate, bits, channels);
   DPMI_RestoreInterrupt(f);
  }
  vew_pt_active = 1; vew_pt_ever = 1;
  if(vew_adaptive){
   // The pump only has to sustain what the CODEC plays (<= the 22.05k
   // ceiling); a 44.1k guest is stepped down before it reaches the ring.
   vew_rs_want = vew_rs_for_frate((unsigned)vew_frate);
   vew_rtc_rs = vew_rs_want;
   rtc_enable();
  }
 }
 wr = ring_wr;
 if(!vew_step_on){
  // Raw path: guest rate lands on (or within 2% of) a codec table rate.
  { unsigned es_free = (ring_rd - wr - 1u) & RING_MASK;  // never lap the consumer
    if((unsigned)bytes > es_free){
     _farpokeb(_dos_ds, 0x4FE, ++es_tel_drop);
     bytes = (int)es_free;
    } }
  es_tel_bytes += (unsigned long)bytes;
  while(bytes > 0){
   int chunk = (int)(RING_BYTES - wr);
   if(chunk > bytes) chunk = bytes;
   memcpy(ring_buf + wr, buf, chunk);
   buf += chunk; bytes -= chunk;
   wr = (wr + chunk) & RING_MASK;
  }
 }else{
  // Frame stepper: re-step guest frames onto the codec rate (drop/dup
  // whole frames, Bresenham). Emits vew_frate/vew_step_in output frames
  // per input frame on average; the accumulator persists across feeds so
  // the ratio holds exactly over time.
  unsigned unit = (channels >= 2 ? 2u : 1u) * (bits >= 16 ? 2u : 1u);
  unsigned es_free = (ring_rd - wr - 1u) & RING_MASK;
  while(bytes >= (int)unit){
   vew_step_acc += vew_frate;
   while(vew_step_acc >= vew_step_in){
    unsigned u;
    vew_step_acc -= vew_step_in;
    if(es_free < unit){                               // ring full: count + stop
     _farpokeb(_dos_ds, 0x4FE, ++es_tel_drop);
     bytes = 0;
     break;
    }
    for(u = 0; u < unit; u++){ ring_buf[wr] = buf[u]; wr = (wr + 1) & RING_MASK; }
    es_free -= unit;
    es_tel_bytes += unit;
   }
   if(bytes < (int)unit) break;
   buf += unit; bytes -= (int)unit;
  }
 }
 if(!SNDISR_HasTsc){
  unsigned u16 = (unsigned)((es_tel_bytes >> 4) & 0xFFFF);
  _farpokeb(_dos_ds, 0x4FC, (unsigned char)u16);
  _farpokeb(_dos_ds, 0x4FD, (unsigned char)(u16 >> 8));
 }
 ring_wr = wr;
 vew_pio_pump();                                      // keep the FIFO fed inline
 vew_pt_feed_busy = 0;
}

// ---- RTC (IRQ8) periodic: the pump clock ---------------------------------
static void rtc_enable(void)
{
 uint8_t f = DPMI_DisableInterrupt();
 outportb(0x70,0x8A); { unsigned char a=(unsigned char)inportb(0x71); outportb(0x70,0x8A); outportb(0x71,(a&0xF0)|vew_rtc_rs); }
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
static void vew_rtc_setrate(unsigned char rs)
{
 uint8_t f = DPMI_DisableInterrupt();
 outportb(0x70,0x8A); { unsigned char a=(unsigned char)inportb(0x71); outportb(0x70,0x8A); outportb(0x71,(a&0xF0)|(rs&0x0F)); }
 DPMI_RestoreInterrupt(f);
 vew_rtc_rs = rs;
}
static unsigned char vew_rs_for_frate(unsigned frames_per_sec)
{
 // Pass rate must exceed frames/burst with 25% headroom (a pass landing on a
 // full FIFO delivers less than a full burst).
 unsigned need = frames_per_sec / VEW_BURST_FRAMES;
 unsigned char rs;
 need += need / 4 + 1;
 for(rs = VEW_RS_IDLE; rs > VEW_RS_MIN; rs--)
  if((32768u >> (rs-1)) >= need) break;
 return rs;
}

// ---- IRQ0 heartbeat: guest-independent watchdog host ---------------------
// WATCHDOG ONLY -- keep this handler tiny. It chains on the guest's INT8
// and runs on WHATEVER STACK that interrupt was taken on, and games run
// slim ISR stacks. A field experiment (2026-07-26) that ran the full pump
// from here -- deployed together with an unverified MODE2/DACZ codec poke
// -- ended a long Lion King session in crackle then total silence; the
// wedge state was lost to a reboot before telemetry could assign blame, so
// which of the two killed it is unproven. Both were withdrawn. Regardless
// of the verdict: do not put heavy work on a borrowed interrupt stack.
static DPMI_ISR_HANDLE vew_i8_handle;
static int vew_i8_on;
static void vew_irq0_isr(void){ vew_watchdog(); }
static void vew_i8_install(void)
{
 if(vew_i8_on) return;
 if(getenv("ESNOI8")) return;                         // diagnostic kill-switch
 if(DPMI_InstallISR(0x08, &vew_irq0_isr, &vew_i8_handle, TRUE) != 0) return;
 vew_i8_on = 1;
}
static void vew_i8_remove(void)
{
 if(!vew_i8_on) return;
 DPMI_UninstallISR(&vew_i8_handle);
 vew_i8_on = 0;
}

// ---- au_cards interface --------------------------------------------------
// Engine passthrough ops (ptops.h): registered from adetect when this card
// wins the session. PTF_REAL_FM = the CF-VEW211's discrete YMF262 answers
// 0x388 natively, so no FM detection shim is needed.
static const struct pt_ops_s vew211_pt_ops = {
 PTF_TAP | PTF_REAL_FM,
 ES1688_PT_Space, ES1688_PT_Feed, ES1688_PT_Watchdog,
 SNDISR_dbg_tick, SNDISR_dbg_exit, SNDISR_dbg_reenter,
 NULL,                              // no nesting instrument -> no depth limiter
};

// /RESAMP: no PTF_TAP, so the engine RENDERS (and resamples) the guest stream
// and hands it to VEW211_writedata, exactly the model sc_tp755 uses. That
// removes the passthrough's whole timing problem in one go: the codec clocks
// ONE fixed table rate, so the frame stepper never engages, the sub-2% "raw
// path" rate deficit cannot accumulate, and the pump stops stuffing silence
// to cover it. The cost is CPU -- the engine resamples + mixes per sample in
// ISR context, on a 486 -- which is the thing this switch exists to measure.
// space/feed are NULL like sc_tp755's: unreachable with the tap disarmed.
static const struct pt_ops_s vew211_render_ops = {
 PTF_REAL_FM,
 NULL, NULL, ES1688_PT_Watchdog,
 SNDISR_dbg_tick, SNDISR_dbg_exit, SNDISR_dbg_reenter,
 NULL,
 VEW_RENDER_CAP,                    // frames per render pass
 VEW_RENDER_DIV,                    // ...and render on 1 tick in N
};

static int VEW211_adetect(struct audioout_info_s *aui)
{
 vew211_card_s *card;
 uint16_t base = VEW_WIN_BASE;
 const char *t = getenv("SBERTC");
 const char *l = getenv("SBEPTLAT");
 if(!PTOPS_CardIs("vew211")) return 0;
 if(getenv("SBENORS")) vew_no_step = 1;               // disable the frame stepper (A/B)
 // /BASE here is the PCMCIA I/O WINDOW (codec at +4), not an SB DSP base --
 // one switch, but only one backend ever reads it because /CARD is required.
 if(FOpts.base) base = (uint16_t)FOpts.base;
 vew_base  = base;
 vew_codec = (uint16_t)(base + VEW_CODEC_OFF);
 if(FOpts.dacrate){ vew_dacrate = (unsigned)FOpts.dacrate;
        if(vew_dacrate<DAC_RATE_MIN) vew_dacrate=DAC_RATE_MIN;
        if(vew_dacrate>DAC_RATE_MAX) vew_dacrate=DAC_RATE_MAX; }
 if(FOpts.cvol >= 0) vew_vol = FOpts.cvol;
 if(t){ int rs = atoi(t); if(rs>=3 && rs<=15) vew_rtc_rs = (unsigned char)rs; }
 else { vew_adaptive = 1; vew_rtc_rs = VEW_RS_IDLE; }
 if(l){ int ms = atoi(l); if(ms >= 30 && ms <= 2000) vew_pt_lat_ms = (unsigned)ms; }

 // Codec must already be reachable (run VEW21XGO first): 0xFF on IAR =
 // nothing decoding the port. This is now a VALIDATOR, not a detector --
 // the user named this card, so an absent codec is a mistake worth stating.
 if((unsigned char)inportb(vew_codec + VC_IAR) == 0xFF){
  printf("CS4231A: nothing at %4.4Xh -- run VEW21XGO first, and check /BASE\n", vew_codec);
  return 0;
 }
 vew_ci_wait(vew_codec);

 card = (vew211_card_s *)pds_calloc(1,sizeof(vew211_card_s));
 if(!card) return 0;
 card->base = base; aui->card_private_data = card;
 aui->card_irq = 8;                                   // RTC drives the pump
 if(FOpts.resamp){
  // Snap to a REAL table rate first. The engine renders at freq_card and the
  // codec clocks whatever vew_rate_pick lands on; any gap between the two is
  // steady pitch error, which is precisely the bug we are trying to remove.
  vew_dacrate = (unsigned)vew_rates[vew_rate_pick(vew_dacrate)].hz;
  // Fixed pump rate, NOT the adaptive ramp: the render cap is sized against
  // this rate, and an adaptive pump dropping to 32 Hz idle would need ~689
  // frames a tick and starve under the cap.
  vew_adaptive = 0;
  vew_rtc_rs = VEW_RENDER_RS;
  printf("CS4231A: RESAMPLED -- %u Hz, pump %u Hz, render %u Hz (<=%u frames)\n",
         vew_dacrate, 32768u >> (VEW_RENDER_RS - 1),
         (32768u >> (VEW_RENDER_RS - 1)) / VEW_RENDER_DIV, VEW_RENDER_CAP);
 }
 PTOPS_Register(FOpts.resamp ? &vew211_render_ops : &vew211_pt_ops);
 // NOFM: guest AdLib I/O at 0x388 goes untrapped to the card's DISCRETE
 // YMF262, which decodes all four OPL3 ports natively. Real FM for free.
 return 1;
}

static void VEW211_setrate(struct audioout_info_s *aui)
{
 vew211_card_s *card = aui->card_private_data;
 aui->freq_card = vew_dacrate;
 aui->chan_card = 2;
 aui->bits_card = 16;
 aui->card_dmasize = RING_BYTES * BYTES_PER_SBSAMPLE;
 vew_base  = card->base;
 vew_codec = (uint16_t)(card->base + VEW_CODEC_OFF);
}

static void VEW211_start(struct audioout_info_s *aui)
{
 vew211_card_s *card = aui->card_private_data;
 // (0x4F6 is the SER catch-up counter now; the 0xAA start marker is retired)
 ring_wr = ring_rd = 0;
 vew_base  = card->base;
 vew_codec = (uint16_t)(card->base + VEW_CODEC_OFF);
 vew_i8_install();
 vew_codec_config(vew_dacrate, 8, 1);                 // baseline arm, PEN on
 // Startup pump = light keep-alive; the first passthrough stream
 // feed-forwards the real rate via PT_Feed.
 if(vew_adaptive){
  unsigned need = vew_dacrate / 96 + 1;
  unsigned char rs;
  for(rs = VEW_RS_IDLE; rs > VEW_RS_MIN; rs--)
   if((32768u >> (rs-1)) >= need) break;
  vew_rs_want = rs;
  vew_rtc_rs = rs;
 }
 rtc_enable();
}

static void VEW211_stop(struct audioout_info_s *aui)
{
 // Quiesce ONLY -- pump + heartbeat stay installed until close() (the
 // "no sound after idle" lesson).
 (void)aui;
 vew_codec_stop();
 vew_pt_active = 0;
 es_flush_gen++;                                      // pump-executed drain
}

static void VEW211_close(struct audioout_info_s *aui)
{
 rtc_disable();
 vew_i8_remove();
 vew_codec_stop();
 if(aui->card_private_data){ pds_free(aui->card_private_data); aui->card_private_data=NULL; }
}

// Render-path pump (non-passthrough): 16-bit stereo -> 8-bit UNSIGNED mono.
static void VEW211_writedata(struct audioout_info_s *aui, char *src, unsigned long bytes)
{
 short *p = (short *)src;
 unsigned long n, free_;
 unsigned wr;
 (void)aui;
 if(vew_pt_active) return;                            // one producer at a time
 n = bytes / BYTES_PER_SBSAMPLE;
 free_ = (unsigned long)((ring_rd - ring_wr - 1) & RING_MASK);
 if(n > free_) n = free_;                             // never lap the consumer
 wr = ring_wr;
 while(n--){
  int mono = ((int)p[0] + (int)p[1]) >> 1;
  p += 2;
  ring_buf[wr] = (unsigned char)((mono >> 8) + 128);
  wr = (wr+1)&RING_MASK;
 }
 ring_wr = wr;
}

static long VEW211_getbufpos(struct audioout_info_s *aui)
{
 (void)aui;
 return (long)((unsigned long)ring_rd * BYTES_PER_SBSAMPLE);
}

// IRQ8/RTC: ack, feed the codec, self-pace, claim the interrupt.
static int VEW211_irq(struct audioout_info_s *aui)
{
 (void)aui;
 _farpokeb(_dos_ds, 0x4F8, ++es_tel_irq);
 ++vew_tick_seq;
 vew_watchdog();
 { uint8_t f = DPMI_DisableInterrupt();               // cli: the IRQ0 heartbeat could
   outportb(0x70,0x0C); (void)inportb(0x71);          // land between CMOS index+data
   DPMI_RestoreInterrupt(f); }

 { unsigned long hz = VEW_RTC_HZ();                   // one RTC period of feed credit;
   vew_fr_acc += vew_frate;                           // burst-capped so a stall can't
   if(vew_fr_acc > hz * VEW_BURST_FRAMES)             // run the pump ahead into full-
    vew_fr_acc = hz * VEW_BURST_FRAMES; }             // FIFO writes (silently dropped)
 vew_pio_pump();
 _farpokeb(_dos_ds, 0x4F5, (unsigned char)(((ring_wr - ring_rd) & RING_MASK) >> 5));  // ring gauge

 // Self-pacing keyed on FEED RECENCY (our tick units) + RING OCCUPANCY --
 // deliberately NOT on vew_pt_active (which sticks at 1 when a guest exits
 // mid-stream: the "sluggish prompt" field bug). The idle throttle engages
 // only once the ring is EMPTY: throttling with data still queued plays the
 // tail out at 32 Hz -- a 43x slow-motion stretch (the Epic Pinball
 // "returning to titles is very slow" / Lion King fade-sag field bug), and
 // with a full ring it deadlocks (PT_Space=0 blocks the very feed whose
 // arrival would restore the rate). The ratchet only fires while feeds are
 // recent, so a departed guest can't pin the pump high on an empty ring.
 if(vew_adaptive && vew_pt_ever){
  unsigned used = (ring_wr - ring_rd) & RING_MASK;
  uint32_t gap = vew_tick_seq - vew_feed_seq;
  uint32_t lim = (VEW_RTC_HZ() * (220UL + 2UL * vew_pt_lat_ms)) / 1000UL;  // scale-invariant ~720ms
  if(gap >= lim){
   if(!used && vew_rtc_rs != VEW_RS_IDLE) vew_rtc_setrate(VEW_RS_IDLE);
  }else if(used < VEW_RING_LOW && vew_rtc_rs > VEW_RS_MIN){
   vew_rtc_setrate((unsigned char)(vew_rtc_rs - 1));
  }
 }
 return 1;                                            // every IRQ8 is ours by construction
}

// VSBHDA sndcard_info_s: 14 fields. No card_info / fm / mixer slots -- the
// discrete YMF262 rides NOFM's untrapped 0x388 directly.
struct sndcard_info_s VEW211_sndcard_info={
 "VEW211",                                            // shortname
 0,                                                   // infobits
 &VEW211_adetect,                                     // card_detect
 &VEW211_start, &VEW211_stop, &VEW211_close,          // start / stop / close
 &VEW211_setrate,                                     // card_setrate
 &VEW211_writedata, &VEW211_getbufpos, NULL,          // writedata / getpos / clear
 &VEW211_irq,                                         // irq_routine
 NULL, NULL, NULL                                     // mixer slots
};

#endif // NOVEW211
