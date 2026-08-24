/* sc_scp55.c -- Roland SCP-55 (PCMCIA) CS4231A backend for VSBPCMCIA.
 *
 * Forked from sc_vew211.c: same CS4231A silicon, same RTC-paced PIO pump, same
 * 16-frame-FIFO constraints.  Three things differ, and the first is the reason
 * this is a separate file rather than a /BASE variant:
 *
 * 1. SPLIT REGISTER MAP.  Index/data are at window+8/+9, but status and
 *    playback-data are at window+6/+7 -- not the contiguous window+4..+7 the
 *    VEW211 has.  See the VC_* defines.
 *
 * 2. ONE CRYSTAL.  16.9344 MHz on XI1, so C2SL=0 selects the datasheet's
 *    XTAL2 rate column and C2SL=1 hangs resync.  Eight rates, not fourteen.
 *
 * 3. NO FM.  The VEW211 has a discrete YMF262 that NOFM leaves untrapped at
 *    0x388; this card has neither.  See TODO(fm) in SCP55_adetect.
 *
 * Bring-up: run SCP55GO (or Roland's SCPENA under Card Services) first -- this
 * backend validates the codec, it does not enable the card.
 *
 * The register map and rate column were established with the probes in
 * ~/Projects/scp55-enabler/probes; SCPPUMP.C there is the standalone
 * prototype of this pump (30 s at 11.025 kHz, one dry interrupt in 30,722).
 */
#ifndef NOSCP55

#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "hostsvc.h"      // toolchain compat: LOW_*, inportb (see the header)
#include "hostisr.h"      // chained/iret PM interrupt vectors, both builds
#include "au_cards.h"
#include "ptops.h"        // engine passthrough ops table (we register in adetect)

// ==========================================================================
//  crazii DPMI-API compat shim over DJGPP (same as sc_es1688.c)
// ==========================================================================
#ifndef TRUE
#define TRUE  1
#define FALSE 0
#endif
/* DPMI_ISR_HANDLE / DPMI_InstallISR / DPMI_UninstallISR now live in
 * src/hostisr.h, which keeps the go32 chain wrapper for the 32-bit build and
 * substitutes src/PMISR.ASM's trampolines when there is no go32 (the 16-bit
 * NOTFLAT build). Same names, same signature, same DJGPP object code. */
#define DPMI_DisableInterrupt  HOST_DisableInterrupt   /* int 31h ax=0900h/0901h */
#define DPMI_RestoreInterrupt  HOST_RestoreInterrupt

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
#define SCP_WIN_BASE  0x330         // I/O window base (CIS default cfg 1); /BASE overrides
#define SCP_CODEC_OFF  0            // see VC_* below: this card SPLITS the codec
// SPLIT REGISTER MAP -- the whole reason this is a separate backend.
// On the CF-VEW211 the CS4231A's four registers are contiguous at window+4.
// On the SCP-55 they are NOT: index/data sit at window+8/+9 while status and
// playback-data sit at window+6/+7.  Proven 2026-08-24 (scp55-enabler
// probes/SCPR2.C): with the codec armed in PIO mode and the FIFO starved,
// window+6 reads DF -- SER set, matching the underrun I11 reports
// independently, and PRDY set -- and shows PRDY high on 20000/20000 polls,
// where window+2 shows 0/20000 and never satisfies R2 semantics at all.
// Writing PCM to window+3 (the textbook PDR) goes nowhere; that mistake had
// this card written off as incapable of DMA-less digital audio for 7 weeks.
#define VC_IAR  8                   // Index Address Register (bit6 = MCE)
#define VC_IDR  9                   // Indexed Data Register
#define VC_SR   6                   // Status Register (read commits a PIO sample)
#define VC_PDR  7                   // Playback Data Register (PIO)
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
#define SCP_RS_MIN    5             // fastest pump = 2048 Hz (22.05k design ceiling)
#define SCP_RS_IDLE   11            // idle keep-alive = 32 Hz
#define SCP_RING_LOW  256
#define SCP_BURST_FRAMES 16         // per-pass ceiling = FIFO depth
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
#define SCP_RENDER_DIV   16
#define SCP_RENDER_CAP   512
#define SCP_RENDER_RS    6          // 32768>>5 = 1024 Hz, fixed (not adaptive)

typedef struct scp55_card_s { uint16_t base; } scp55_card_s;

// OWNERSHIP: ring_rd is written ONLY by the pump (serialized by the busy
// guard); trap-context code requests flushes via es_flush_gen and the pump
// snaps rd := wr itself (the sample-boundary race lesson from sc_es1688.c).
static volatile unsigned ring_wr, ring_rd;
static volatile unsigned es_flush_gen, es_flush_ack;
static unsigned char      ring_buf[RING_BYTES];
static uint16_t           scp_base   = SCP_WIN_BASE;
static uint16_t           scp_codec  = SCP_WIN_BASE + SCP_CODEC_OFF;
static unsigned           scp_dacrate = DAC_RATE_DEF;
static int                scp_vol     = -1;           // SBEVOL attenuation 0..63 (-1 = keep enabler's)
static unsigned char      scp_rtc_rs  = SCP_RS_IDLE;  // current armed RTC pump rate
static unsigned char      scp_rs_want = SCP_RS_IDLE;  // the STREAM's proper pump rate; PT_Feed
                                                      // restores it after an idle throttle (the
                                                      // mid-session chop lesson from sc_es1688.c)
static int                scp_adaptive;

static void rtc_enable(void);
static void scp_rtc_setrate(unsigned char rs);
static unsigned char scp_rs_for_frate(unsigned frames_per_sec);
// Time is kept in OUR OWN RTC tick count -- NEVER the BIOS tick at 0x46C.
// Games own the timer chain: Lion King hooks INT8 without chaining (0x46C
// freezes solid, measured on the 235), fast-timer games advance it several
// times too fast. Any 0x46C-keyed decision (idle detection, watchdog gap)
// misfires exactly while a game is running.
static volatile uint32_t scp_tick_seq;   // ++ per delivered RTC tick
static volatile uint32_t scp_feed_seq;   // scp_tick_seq at the last PT_Feed
static int                scp_pt_ever;

// ---- RTC-tick-credit pacing ----------------------------------------------
// Credit is granted in SCP55_irq (one RTC period of frames per delivered
// tick) and spent by the pump. Units: frames * rtc_hz -- one frame costs
// rtc_hz, one tick earns scp_frate. Guest-independent (see header).
static unsigned long scp_frate = DAC_RATE_DEF;  // codec TABLE rate, frames/s
static unsigned long scp_fr_acc;                // pacing credit
#define SCP_RTC_HZ() (32768UL >> (scp_rtc_rs - 1))

// ---- CS4231A indexed-register helpers ------------------------------------
static void scp_iodelay(unsigned n){ while(n--) (void)inportb(0x80); }
#define SCP_MS(x) scp_iodelay((unsigned)(x) * 1000U)
static void scp_ci_wait(uint16_t cb)
{
 unsigned long i; for(i=0;i<400000UL;i++) if(!(inportb(cb+VC_IAR) & 0x80)) return;
}
static void scp_ci_put(uint16_t cb, unsigned char idx, unsigned char v)
{
 scp_ci_wait(cb); outportb(cb+VC_IAR, idx); scp_iodelay(200);
 outportb(cb+VC_IDR, v); scp_iodelay(200);
}
static unsigned char scp_ci_get(uint16_t cb, unsigned char idx)
{
 scp_ci_wait(cb); outportb(cb+VC_IAR, idx); scp_iodelay(200);
 return (unsigned char)inportb(cb+VC_IDR);
}

// ---- passthrough state ---------------------------------------------------
static volatile int scp_pt_active;
static unsigned scp_pt_rate, scp_pt_bits, scp_pt_channels;
static unsigned scp_hw_rate, scp_hw_bits, scp_hw_channels;  // what the CODEC is
                                                            // programmed for (0 =
                                                            // not armed / stopped)
static unsigned char scp_tel_recfg;
static volatile int scp_pump_busy;

// This card populates a SINGLE 16.9344 MHz crystal on XI1, so C2SL=0 selects
// the rate column the datasheet lists under XTAL2.  There is no second
// crystal: setting C2SL=1 hangs the resync until the socket is power-cycled,
// so every code here has bit0 clear.  Measured 2026-08-24: I8=0x02 clocks
// 11,008 samples/sec, confirming the column.
static const struct { unsigned long hz; unsigned char code; } scp_rates[] = {
 { 5512UL,0x00},{ 6620UL,0x0E},{11025UL,0x02},{18900UL,0x04},
 {22050UL,0x06},{33075UL,0x0C},{37800UL,0x08},{44100UL,0x0A}
};
#define SCP_NRATES (int)(sizeof(scp_rates)/sizeof(scp_rates[0]))
// Picks are capped at the 22.05k design ceiling: the pump can sustain at
// most 2048 Hz x 16 frames = 32768 frames/s, so entries above 22050 can
// never be fed continuously -- a 44.1k guest maps to 22050 and the frame
// stepper (below) decimates 2:1: correct pitch and tempo, half bandwidth.
#define SCP_RATE_CEIL 22050UL
// Runtime override via SBEMAXHZ. On a slow host the driver's cost is dominated
// by the INTERRUPT tax, not the byte traffic: rs_for_frate targets ~10.8 bytes
// per tick by design (rate/16 +25%, rounded to a power-of-two RTC divider), so
// bytes-per-interrupt stays ~11-16 whatever the rate -- you cannot fatten the
// transfers, because the FIFO is 16 samples. What DOES scale is how many
// interrupts happen: 11025 needs a 1024 Hz pump, 5512 only 512 Hz. Capping the
// codec rate therefore halves BOTH the interrupt count and the bus traffic.
// The frame stepper folds the guest down nearest-neighbour (correct pitch and
// tempo, less bandwidth) exactly as it already does for a 44.1k guest at 22050.
static unsigned long scp_rate_ceil = SCP_RATE_CEIL;
static int scp_rate_pick(unsigned rate)
{
 int i, best = 2; unsigned long bd = 0xFFFFFFFFUL;   // default = 11025
 for(i=0;i<SCP_NRATES;i++){
  unsigned long d;
  if(scp_rates[i].hz > scp_rate_ceil) continue;
  d = scp_rates[i].hz > rate ? scp_rates[i].hz - rate : rate - scp_rates[i].hz;
  if(d < bd){ bd = d; best = i; }
 }
 return best;
}

// ---- nearest-neighbour frame stepper -------------------------------------
// THIS CARD HAS ONLY 8 RATES, not the CF-VEW211's 14: one 16.9344 MHz crystal
// means the XTAL2 column and nothing else, so 8000, 9600, 16000, 27042, 32000
// and 48000 -- which the VEW211 can hit exactly -- DO NOT EXIST here. Below
// the 22050 ceiling we have only 5512, 6620, 11025, 18900, 22050, so the
// middle of the range SB games actually use is sparse: a guest at 8000 lands
// on 6620 (17% flat), 9600 on 11025 (15% sharp), 16000 on 18900 (18% sharp).
// The stepper fixes the PITCH, but at 17% it drops or duplicates one frame in
// six, which is audibly rough. That is a property of the crystal, not a bug to
// patch out -- it is why this card sounds worse than a VEW211 on mid-rate
// titles, and it is worth measuring the guest's asked-for rate (0x4F2 holds
// rate>>8 at each full reconfig) before blaming anything else.
// SB guests derive arbitrary rates from the time constant; nearest-
// match alone leaves rates like 20000 or 25000 playing 5-8% flat/sharp --
// and playing flat also drags the whole game (ring backpressure throttles
// the guest to the codec's pace). When the mismatch exceeds ~2%, PT_Feed
// re-steps the guest stream frame-wise onto the codec rate with a
// Bresenham accumulator: whole frames are dropped or duplicated during
// the ring copy that happens anyway. No interpolation, no per-sample
// math -- a compare and an add per frame, 486-priced. Exact/near-table
// streams (all the validated titles) keep the raw untouched path.
// SBENORS=1 disables the stepper (A/B).
static int           scp_no_step;      // env SBENORS
static int           scp_step_on;      // engaged for the current format
static unsigned long scp_step_in;      // guest frames/s
static unsigned long scp_step_acc;     // Bresenham accumulator

// Program the codec for {rate,bits,channels} in PIO mode; ms-paced MCE with
// verify/retry (the CS4231A-KQ drops fast cold writes). ~25 ms.
static void scp_codec_config(unsigned rate, unsigned bits, unsigned channels)
{
 uint16_t cb = scp_codec;
 unsigned char i8;
 int tries, ri;
 if(rate < DAC_RATE_MIN) rate = DAC_RATE_MIN;
 if(rate > DAC_RATE_MAX) rate = DAC_RATE_MAX;
 ri = scp_rate_pick(rate);
 i8 = (unsigned char)(scp_rates[ri].code
       | (channels >= 2 ? I8_STEREO : 0)
       | (bits >= 16    ? I8_16BIT  : 0));

 // MODE 2 first: I16 does not exist without it.  The vendor driver runs the
 // card this way (captured live 2026-08-24: I12=CA, I16=C0), so we match it
 // rather than inventing a configuration.
 scp_ci_put(cb, 0x0C, (unsigned char)(scp_ci_get(cb,0x0C) | 0x40));
 for(tries = 0; tries < 8; tries++){
  scp_ci_wait(cb);
  outportb(cb+VC_IAR, (unsigned char)(IAR_MCE|0x08)); SCP_MS(2);
  outportb(cb+VC_IDR, i8);                            SCP_MS(2);
  outportb(cb+VC_IAR, (unsigned char)(IAR_MCE|0x09)); SCP_MS(2);
  outportb(cb+VC_IDR, (unsigned char)(I9_PPIO|I9_ACAL)); SCP_MS(2);
  outportb(cb+VC_IAR, 0x00);                          SCP_MS(2);
  scp_ci_wait(cb); SCP_MS(10);
  { unsigned long w; for(w=0;w<400000UL;w++) if(!(scp_ci_get(cb,0x0B) & I11_ACI)) break; }
  if(scp_ci_get(cb,0x08) == i8 && (scp_ci_get(cb,0x09) & (I9_PPIO|I9_ACAL)) == (I9_PPIO|I9_ACAL))
   break;
 }
 if(scp_vol >= 0){
  scp_ci_put(cb, 0x06, (unsigned char)(scp_vol & 0x3F));
  scp_ci_put(cb, 0x07, (unsigned char)(scp_vol & 0x3F));
 }else{
  scp_ci_put(cb, 0x06, (unsigned char)(scp_ci_get(cb,0x06) & 0x3F));   // un-mute, keep level
  scp_ci_put(cb, 0x07, (unsigned char)(scp_ci_get(cb,0x07) & 0x3F));
 }
 // I16 bit7 OLB = full-scale 2.8 Vpp instead of 2.0.  Bit6 TE (the codec's
 // own timer) stays CLEAR: our pump is the RTC, so the internal timer would
 // just latch TI with nobody servicing it.
 scp_ci_put(cb, 0x10, 0x80);
 scp_ci_put(cb, 0x0F, 0xFE); scp_ci_put(cb, 0x0E, 0xFF);
 for(tries = 0; tries < 8; tries++){
  scp_ci_put(cb, 0x09, (unsigned char)(I9_PPIO|I9_ACAL|I9_PEN)); SCP_MS(2);
  if(scp_ci_get(cb,0x09) & I9_PEN) break;
 }
 scp_pt_rate = rate; scp_pt_bits = bits; scp_pt_channels = channels;
 scp_hw_rate = rate; scp_hw_bits = bits; scp_hw_channels = channels;
 // Pace at the rate the codec will actually PLAY (the table entry).
 scp_frate = scp_rates[ri].hz;
 scp_fr_acc = 0;
 // Arm the frame stepper when the guest rate misses the table by >2%.
 scp_step_in  = rate;
 scp_step_acc = 0;
 { unsigned long d = scp_frate > rate ? scp_frate - rate : rate - scp_frate;
   scp_step_on = (!scp_no_step && d * 50UL > (unsigned long)rate) ? 1 : 0; }
#if !PTDIAG
 LOW_PokeB(0x4F2, (unsigned char)(rate >> 8));  // guest rate >> 8 (pitch-bug forensics)
 LOW_PokeB(0x4FA, ++scp_tel_recfg);          // FULL reconfigs only
#else
 (void)scp_tel_recfg;   // 0x4F2/0x4FA are on loan to the PT-tap forensics
#endif
}
static void scp_codec_stop(void)
{
 uint16_t cb = scp_codec;
 unsigned char sil = (scp_pt_active && scp_pt_bits >= 16) ? 0x00 : 0x80;
 int i;
 for(i=0;i<64;i++){ scp_iodelay(60); outportb(cb+VC_PDR, sil); (void)inportb(cb+VC_SR); }
 SCP_MS(2);
 scp_ci_put(cb, 0x09, 0x00);                          // clear PEN
 scp_hw_rate = scp_hw_bits = scp_hw_channels = 0;     // codec disarmed: no fast-resume
}

// drain ring -> codec FIFO, RTC-credit-paced. Called from the RTC tick
// (which grants the credit first) AND inline from PT_Feed (which only
// spends leftovers -- no time source of its own, so no double-credit).
static void scp_pio_pump(void)
{
 uint16_t cb = scp_codec;
 unsigned unit = 1;
 unsigned char sil;
 unsigned long hz;
 int guard = 0;
 if(scp_pump_busy) return;
 scp_pump_busy = 1;
 if(es_flush_gen != es_flush_ack){                    // deferred ring flush (trap-context requests)
  es_flush_ack = es_flush_gen;
  ring_rd = ring_wr;
 }

 if(scp_pt_active){
  unit = (scp_pt_channels >= 2) ? 2 : 1;
  if(scp_pt_bits >= 16) unit *= 2;
  if(unit > 4) unit = 4;
 }
 sil = (scp_pt_active && scp_pt_bits >= 16) ? 0x00 : 0x80;

 hz = SCP_RTC_HZ();
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
   if(scp_pt_active && (sr & 0x10)){
    static unsigned char scp_tel_ser;
    scp_fr_acc = hz * SCP_BURST_FRAMES;
    LOW_PokeB(0x4F6, ++scp_tel_ser);         // SER catch-up count
   }
   outportb(cb+VC_SR, 0); }                           // clear SER/INT for the next interval
 while(scp_fr_acc >= hz && guard < SCP_BURST_FRAMES){
  unsigned rd = ring_rd, wr = ring_wr, u;
  if(((wr - rd) & RING_MASK) >= unit){
   for(u=0;u<unit;u++){ outportb(cb+VC_PDR, ring_buf[rd]); (void)inportb(cb+VC_SR); rd = (rd+1)&RING_MASK; }
   ring_rd = rd;
  }else{
   for(u=0;u<unit;u++){ outportb(cb+VC_PDR, sil); (void)inportb(cb+VC_SR); }
  }
  scp_fr_acc -= hz;
  guard++;
 }
 scp_pump_busy = 0;
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
static void scp_watchdog(void)
{
 static unsigned char es_tel_revive;
 static uint32_t wd_seq;
 static unsigned char wd_stale, wd_sec, wd_secchg;
 unsigned char b, sec;
 uint8_t f;
 uint32_t seq = scp_tick_seq;
 if(seq != wd_seq){ wd_seq = seq; wd_stale = 0; wd_secchg = 0; return; }
 if(++wd_stale < 2) return;                           // debounce one visit
 wd_stale = 0;
 f = DPMI_DisableInterrupt();
 outportb(0x70,0x8B); b   = (unsigned char)inportb(0x71);
 outportb(0x70,0x80); sec = (unsigned char)inportb(0x71);
 DPMI_RestoreInterrupt(f);
 if(!(b & 0x40)){                                     // PIE killed (the TH-class death)
  rtc_enable();
  LOW_PokeB(0x4F4, ++es_tel_revive);
  return;
 }
 if(inportb(0xA1) & 0x01){                            // IRQ8 masked at the slave PIC
  f = DPMI_DisableInterrupt();
  outportb(0xA1, (unsigned char)(inportb(0xA1) & ~0x01));
  DPMI_RestoreInterrupt(f);
  LOW_PokeB(0x4F4, ++es_tel_revive);
  return;
 }
 if(sec != wd_sec){                                   // armed yet silent: confirm by
  wd_sec = sec;                                       // real elapsed time before the
  if(++wd_secchg >= 2){                               // PF-eating reg-C heal
   wd_secchg = 0;
   f = DPMI_DisableInterrupt();
   outportb(0x70,0x0C); (void)inportb(0x71);
   DPMI_RestoreInterrupt(f);
   LOW_PokeB(0x4F4, ++es_tel_revive);
  }
 }
}

// vsb.c hook: every guest DSP RESET lands here.
static void ES1688_PT_Watchdog(void)
{
 scp_watchdog();
 es_flush_gen++;                                      // pump-executed ring flush
 scp_pt_active = 0;
 scp_pt_rate = scp_pt_bits = scp_pt_channels = 0;
}

static unsigned scp_pt_lat_ms = 250;
static int ES1688_PT_Space(void)
{
 unsigned used = (ring_wr - ring_rd) & RING_MASK;
 unsigned target = RING_BYTES - 64;
 if(scp_pt_rate){
  // The ring holds OUTPUT-rate bytes (the stepper may have re-stepped the
  // guest stream), so the latency target is sized from the codec rate.
  unsigned bps = (unsigned)scp_frate * scp_pt_channels * ((scp_pt_bits + 7) / 8);
  target = (unsigned)((unsigned long)bps * scp_pt_lat_ms / 1000UL);
  if(target > RING_BYTES - 64) target = RING_BYTES - 64;
  if(target < 512) target = 512;
 }
 if(used >= target) return 0;
 { unsigned space = target - used;
   // The caller counts GUEST bytes; the stepper shrinks (or grows) them on
   // the way into the ring. Convert output-byte room to the guest bytes
   // that will fill it, or a decimated 44.1k guest would be throttled to
   // half its real-time rate by its own bandwidth savings.
   if(scp_step_on)
    space = (unsigned)((unsigned long)space * scp_step_in / scp_frate);
   return (int)space; }
}

// feed raw guest PCM (sndisr.c tap); reconfigure the codec on format change.
static volatile int scp_pt_feed_busy;
static unsigned char es_reentry;
static unsigned char es_tel_drop;                     // 0x4FE: ring-full feed clamps
static void ES1688_PT_Feed(const unsigned char *buf, int bytes, unsigned rate, unsigned bits, unsigned channels)
{
 unsigned wr;
 if(scp_pt_feed_busy){ LOW_PokeB(0x4F3, ++es_reentry); return; }
 scp_pt_feed_busy = 1;
 ++es_tel_feed16;
 LOW_PokeB(0x4FB, (unsigned char)es_tel_feed16);
 LOW_PokeB(0x4FF, (unsigned char)(es_tel_feed16 >> 8));
 scp_feed_seq = scp_tick_seq;
 // Waking from the idle throttle: between same-format sounds no reconfig
 // runs, so restore the stream's pump rate HERE (rate-up only).
 if(scp_adaptive && scp_rtc_rs > scp_rs_want) scp_rtc_setrate(scp_rs_want);
 if(!scp_pt_active || rate != scp_pt_rate || bits != scp_pt_bits || channels != scp_pt_channels)
 {
  // FAST RESUME: a guest DSP reset per sound clears the stream state via
  // PT_Watchdog, but the codec usually needs nothing -- still programmed for
  // this exact format with PEN running (a stalled pump only underruns the
  // FIFO, which recovers by feeding; codec_stop is the only real disarm and
  // it clears scp_hw_*). The full MCE sequence costs ~25 ms per call, so
  // skipping it per-SFX matters even more here than on the ES1688.
  if(rate == scp_hw_rate && bits == scp_hw_bits && channels == scp_hw_channels){
   scp_pt_rate = rate; scp_pt_bits = bits; scp_pt_channels = channels;
  }else{
   uint8_t f = DPMI_DisableInterrupt();
   scp_codec_config(rate, bits, channels);
   DPMI_RestoreInterrupt(f);
  }
  scp_pt_active = 1; scp_pt_ever = 1;
  if(scp_adaptive){
   // The pump only has to sustain what the CODEC plays (<= the 22.05k
   // ceiling); a 44.1k guest is stepped down before it reaches the ring.
   scp_rs_want = scp_rs_for_frate((unsigned)scp_frate);
   scp_rtc_rs = scp_rs_want;
   rtc_enable();
  }
 }
 wr = ring_wr;
 if(!scp_step_on){
  // Raw path: guest rate lands on (or within 2% of) a codec table rate.
  { unsigned es_free = (ring_rd - wr - 1u) & RING_MASK;  // never lap the consumer
    if((unsigned)bytes > es_free){
     LOW_PokeB(0x4FE, ++es_tel_drop);
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
  // whole frames, Bresenham). Emits scp_frate/scp_step_in output frames
  // per input frame on average; the accumulator persists across feeds so
  // the ratio holds exactly over time.
  unsigned unit = (channels >= 2 ? 2u : 1u) * (bits >= 16 ? 2u : 1u);
  unsigned es_free = (ring_rd - wr - 1u) & RING_MASK;
  while(bytes >= (int)unit){
   scp_step_acc += scp_frate;
   while(scp_step_acc >= scp_step_in){
    unsigned u;
    scp_step_acc -= scp_step_in;
    if(es_free < unit){                               // ring full: count + stop
     LOW_PokeB(0x4FE, ++es_tel_drop);
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
  LOW_PokeB(0x4FC, (unsigned char)u16);
  LOW_PokeB(0x4FD, (unsigned char)(u16 >> 8));
 }
 ring_wr = wr;
 scp_pio_pump();                                      // keep the FIFO fed inline
 scp_pt_feed_busy = 0;
}

// ---- RTC (IRQ8) periodic: the pump clock ---------------------------------
static void rtc_enable(void)
{
 uint8_t f = DPMI_DisableInterrupt();
 outportb(0x70,0x8A); { unsigned char a=(unsigned char)inportb(0x71); outportb(0x70,0x8A); outportb(0x71,(a&0xF0)|scp_rtc_rs); }
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
static void scp_rtc_setrate(unsigned char rs)
{
 uint8_t f = DPMI_DisableInterrupt();
 outportb(0x70,0x8A); { unsigned char a=(unsigned char)inportb(0x71); outportb(0x70,0x8A); outportb(0x71,(a&0xF0)|(rs&0x0F)); }
 DPMI_RestoreInterrupt(f);
 scp_rtc_rs = rs;
}
static unsigned char scp_rs_for_frate(unsigned frames_per_sec)
{
 // Pass rate must exceed frames/burst with 25% headroom (a pass landing on a
 // full FIFO delivers less than a full burst).
 unsigned need = frames_per_sec / SCP_BURST_FRAMES;
 unsigned char rs;
 need += need / 4 + 1;
 for(rs = SCP_RS_IDLE; rs > SCP_RS_MIN; rs--)
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
static DPMI_ISR_HANDLE scp_i8_handle;
static int scp_i8_on;
static void scp_irq0_isr(void){ scp_watchdog(); }
static void scp_i8_install(void)
{
 if(scp_i8_on) return;
 if(getenv("ESNOI8")) return;                         // diagnostic kill-switch
 if(DPMI_InstallISR(0x08, &scp_irq0_isr, &scp_i8_handle, TRUE) != 0) return;
 scp_i8_on = 1;
}
static void scp_i8_remove(void)
{
 if(!scp_i8_on) return;
 DPMI_UninstallISR(&scp_i8_handle);
 scp_i8_on = 0;
}

// ---- ISR nesting depth: the sndisr depth limiter -------------------------
// PTOPS.H field 8: "live ISR nesting depth for the sndisr depth limiter;
// NULL = none".  sc_vew211 passes NULL -- and SNDISR.C:518 reads
//     if ( PT_Ops->depth && PT_Ops->depth() > 3 ) goto isrexit;
// so a NULL there SHORT-CIRCUITS the limiter completely.  Nothing then stops
// the resonance SNDISR.C's own comment describes: "Trap-tax-stretched passes
// let this resonate to depth 14 = 56KB gone" off the private ISR stack -- the
// march into .data that stackisr.asm's STACKCHECK exists to catch.
//
// sc_tp755 -- the backend that survives real games -- DOES supply it. It reads
// 0x4F0, which SNDISR.C:105 pokes with dbg_MAXdepth, so tp755's limiter
// latches: once nesting has ever hit 4 every later nested pass bails for good.
// We track LIVE depth instead, which is what the field is documented to be and
// does not permanently degrade after one excursion.  Safe to count here
// because dbg_tick (SNDISR.C:480) and dbg_exit (:1086, reached through
// isrexit:1084) are paired on EVERY path, including the early `goto isrexit`
// bails -- checked, not assumed.
static volatile int scp_isr_depth;
static void scp_dbg_tick(void){ scp_isr_depth++; SNDISR_dbg_tick(); }
static void scp_dbg_exit(void){ SNDISR_dbg_exit(); if(scp_isr_depth) scp_isr_depth--; }
static int  SCP55_Depth(void){ return scp_isr_depth; }

// ---- au_cards interface --------------------------------------------------
// Engine passthrough ops (ptops.h): registered from adetect when this card
// wins the session.  NO PTF_REAL_FM: unlike the CF-VEW211 (discrete YMF262)
// this card has no FM chip and no 0x388 window at all, and per sc_tp755.c the
// ABSENCE of the flag is what tells the engine 0x388 is open bus.  Claiming it
// here -- which the VEW211 lineage did, inherited verbatim -- makes the engine
// believe real hardware answers 0x388, and config.h notes NOFM also makes
// PTRAP skip the 0x388 trap.  The init probe catches the lie ("card claims a
// chip at 388h, none answered") and falls back to FMSHIM, but the declaration
// was still false.
static const struct pt_ops_s scp55_pt_ops = {
 PTF_TAP,
 ES1688_PT_Space, ES1688_PT_Feed, ES1688_PT_Watchdog,
 scp_dbg_tick, scp_dbg_exit, SNDISR_dbg_reenter,
 SCP55_Depth,                       // arms SNDISR.C's depth limiter
};

// /RESAMP: no PTF_TAP, so the engine RENDERS (and resamples) the guest stream
// and hands it to SCP55_writedata, exactly the model sc_tp755 uses. That
// removes the passthrough's whole timing problem in one go: the codec clocks
// ONE fixed table rate, so the frame stepper never engages, the sub-2% "raw
// path" rate deficit cannot accumulate, and the pump stops stuffing silence
// to cover it. The cost is CPU -- the engine resamples + mixes per sample in
// ISR context, on a 486 -- which is the thing this switch exists to measure.
// space/feed are NULL like sc_tp755's: unreachable with the tap disarmed.
static const struct pt_ops_s scp55_render_ops = {
 0,                                 // no tap, and no real FM (see above)
 NULL, NULL, ES1688_PT_Watchdog,
 scp_dbg_tick, scp_dbg_exit, SNDISR_dbg_reenter,
 SCP55_Depth,
 SCP_RENDER_CAP,                    // frames per render pass
 SCP_RENDER_DIV,                    // ...and render on 1 tick in N
};

static int SCP55_adetect(struct audioout_info_s *aui)
{
 scp55_card_s *card;
 uint16_t base = SCP_WIN_BASE;
 const char *t = getenv("SBERTC");
 const char *l = getenv("SBEPTLAT");
 if(!PTOPS_CardIs("scp55")) return 0;
 if(getenv("SBENORS")) scp_no_step = 1;
 { const char *mh = getenv("SBEMAXHZ");          // cap the CODEC rate (CPU knob)
   if(mh){ long v = atol(mh);
           if(v >= 4000L && v <= 48000L) scp_rate_ceil = (unsigned long)v; } }               // disable the frame stepper (A/B)
 // /BASE here is the PCMCIA I/O WINDOW (codec at +4), not an SB DSP base --
 // one switch, but only one backend ever reads it because /CARD is required.
 if(FOpts.base) base = (uint16_t)FOpts.base;
 scp_base  = base;
 scp_codec = (uint16_t)(base + SCP_CODEC_OFF);
 if(FOpts.dacrate){ scp_dacrate = (unsigned)FOpts.dacrate;
        if(scp_dacrate<DAC_RATE_MIN) scp_dacrate=DAC_RATE_MIN;
        if(scp_dacrate>DAC_RATE_MAX) scp_dacrate=DAC_RATE_MAX; }
 if(FOpts.cvol >= 0) scp_vol = FOpts.cvol;
 if(t){ int rs = atoi(t); if(rs>=3 && rs<=15) scp_rtc_rs = (unsigned char)rs; }
 else { scp_adaptive = 1; scp_rtc_rs = SCP_RS_IDLE; }
 if(l){ int ms = atoi(l); if(ms >= 30 && ms <= 2000) scp_pt_lat_ms = (unsigned)ms; }

 // Codec must already be reachable (run SCP55GO first): 0xFF on IAR =
 // nothing decoding the port. This is now a VALIDATOR, not a detector --
 // the user named this card, so an absent codec is a mistake worth stating.
 if((unsigned char)inportb(scp_codec + VC_IAR) == 0xFF){
  printf("CS4231A: nothing at %4.4Xh -- run SCP55GO (or SCPENA) first, and check /BASE\n",
         (unsigned)(scp_codec + VC_IAR));
  return 0;
 }
 scp_ci_wait(scp_codec);

 card = (scp55_card_s *)pds_calloc(1,sizeof(scp55_card_s));
 if(!card) return 0;
 card->base = base; aui->card_private_data = card;
 aui->card_irq = 8;                                   // RTC drives the pump
 if(FOpts.resamp){
  // Snap to a REAL table rate first. The engine renders at freq_card and the
  // codec clocks whatever scp_rate_pick lands on; any gap between the two is
  // steady pitch error, which is precisely the bug we are trying to remove.
  scp_dacrate = (unsigned)scp_rates[scp_rate_pick(scp_dacrate)].hz;
  // Fixed pump rate, NOT the adaptive ramp: the render cap is sized against
  // this rate, and an adaptive pump dropping to 32 Hz idle would need ~689
  // frames a tick and starve under the cap.
  scp_adaptive = 0;
  scp_rtc_rs = SCP_RENDER_RS;
  printf("CS4231A: RESAMPLED -- %u Hz, pump %u Hz, render %u Hz (<=%u frames)\n",
         scp_dacrate, 32768u >> (SCP_RENDER_RS - 1),
         (32768u >> (SCP_RENDER_RS - 1)) / SCP_RENDER_DIV, SCP_RENDER_CAP);
 }
 PTOPS_Register(FOpts.resamp ? &scp55_render_ops : &scp55_pt_ops);
 // FM: unlike the CF-VEW211 there is NO YMF262 on this card and no 0x388
 // window at all, so the VEW211's "let AdLib fall through untrapped" trick
 // does not apply -- guest FM writes would land on nothing.
 // TODO(fm): decide between FMSHIM (src/fmshim.h -- timer-only OPL3 status
 // shim, which keeps FM-timing-dependent titles alive without audible FM)
 // and sc_tp755's approach of keeping the software OPL3 and mixing it into
 // the single PCM stream.  The SCP-55 has a real GS Sound Canvas on MPU-401
 // at window+0/+1, so for MIDI titles the better answer may be neither.
 return 1;
}

static void SCP55_setrate(struct audioout_info_s *aui)
{
 scp55_card_s *card = aui->card_private_data;
 aui->freq_card = scp_dacrate;
 aui->chan_card = 2;
 aui->bits_card = 16;
 aui->card_dmasize = RING_BYTES * BYTES_PER_SBSAMPLE;
 scp_base  = card->base;
 scp_codec = (uint16_t)(card->base + SCP_CODEC_OFF);
}

static void SCP55_start(struct audioout_info_s *aui)
{
 scp55_card_s *card = aui->card_private_data;
 // (0x4F6 is the SER catch-up counter now; the 0xAA start marker is retired)
 ring_wr = ring_rd = 0;
 scp_base  = card->base;
 scp_codec = (uint16_t)(card->base + SCP_CODEC_OFF);
 scp_i8_install();
 scp_codec_config(scp_dacrate, 8, 1);                 // baseline arm, PEN on
 // Startup pump = light keep-alive; the first passthrough stream
 // feed-forwards the real rate via PT_Feed.
 if(scp_adaptive){
  unsigned need = scp_dacrate / 96 + 1;
  unsigned char rs;
  for(rs = SCP_RS_IDLE; rs > SCP_RS_MIN; rs--)
   if((32768u >> (rs-1)) >= need) break;
  scp_rs_want = rs;
  scp_rtc_rs = rs;
 }
 rtc_enable();
}

static void SCP55_stop(struct audioout_info_s *aui)
{
 // Quiesce ONLY -- pump + heartbeat stay installed until close() (the
 // "no sound after idle" lesson).
 (void)aui;
 scp_codec_stop();
 scp_pt_active = 0;
 es_flush_gen++;                                      // pump-executed drain
}

static void SCP55_close(struct audioout_info_s *aui)
{
 rtc_disable();
 scp_i8_remove();
 scp_codec_stop();
 if(aui->card_private_data){ pds_free(aui->card_private_data); aui->card_private_data=NULL; }
}

// Render-path pump (non-passthrough): 16-bit stereo -> 8-bit UNSIGNED mono.
static void SCP55_writedata(struct audioout_info_s *aui, char *src, unsigned long bytes)
{
 short *p = (short *)src;
 unsigned long n, free_;
 unsigned wr;
 (void)aui;
 if(scp_pt_active) return;                            // one producer at a time
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

static long SCP55_getbufpos(struct audioout_info_s *aui)
{
 (void)aui;
 return (long)((unsigned long)ring_rd * BYTES_PER_SBSAMPLE);
}

// IRQ8/RTC: ack, feed the codec, self-pace, claim the interrupt.
static int SCP55_irq(struct audioout_info_s *aui)
{
 (void)aui;
 LOW_PokeB(0x4F8, ++es_tel_irq);
 ++scp_tick_seq;
 scp_watchdog();
 { uint8_t f = DPMI_DisableInterrupt();               // cli: the IRQ0 heartbeat could
   outportb(0x70,0x0C); (void)inportb(0x71);          // land between CMOS index+data
   DPMI_RestoreInterrupt(f); }

 { unsigned long hz = SCP_RTC_HZ();                   // one RTC period of feed credit;
   scp_fr_acc += scp_frate;                           // burst-capped so a stall can't
   if(scp_fr_acc > hz * SCP_BURST_FRAMES)             // run the pump ahead into full-
    scp_fr_acc = hz * SCP_BURST_FRAMES; }             // FIFO writes (silently dropped)
 scp_pio_pump();
 LOW_PokeB(0x4F5, (unsigned char)(((ring_wr - ring_rd) & RING_MASK) >> 5));  // ring gauge

 // Self-pacing keyed on FEED RECENCY (our tick units) + RING OCCUPANCY --
 // deliberately NOT on scp_pt_active (which sticks at 1 when a guest exits
 // mid-stream: the "sluggish prompt" field bug). The idle throttle engages
 // only once the ring is EMPTY: throttling with data still queued plays the
 // tail out at 32 Hz -- a 43x slow-motion stretch (the Epic Pinball
 // "returning to titles is very slow" / Lion King fade-sag field bug), and
 // with a full ring it deadlocks (PT_Space=0 blocks the very feed whose
 // arrival would restore the rate). The ratchet only fires while feeds are
 // recent, so a departed guest can't pin the pump high on an empty ring.
 if(scp_adaptive && scp_pt_ever){
  unsigned used = (ring_wr - ring_rd) & RING_MASK;
  uint32_t gap = scp_tick_seq - scp_feed_seq;
  uint32_t lim = (SCP_RTC_HZ() * (220UL + 2UL * scp_pt_lat_ms)) / 1000UL;  // scale-invariant ~720ms
  if(gap >= lim){
   if(!used && scp_rtc_rs != SCP_RS_IDLE) scp_rtc_setrate(SCP_RS_IDLE);
  }else if(used < SCP_RING_LOW && scp_rtc_rs > scp_rs_for_frate((unsigned)scp_frate)){
   // FLOOR IS THE STREAM'S NEED, NOT THE GLOBAL RS_MIN (2026-08-24).
   // The ratchet used to run all the way to SCP_RS_MIN = 2048 Hz even when
   // scp_rs_for_frate() says 1024 Hz suffices -- at 11025 that is 689 f/s per
   // 16-frame burst, +25%% = 862 Hz, i.e. rs=6. Ratcheting past it SILENTLY
   // OVERRODE /DACRATE and halved the tick window to 0.49 ms. The CS4231A
   // costs ~4x the ES1688 per tick (2 I/O ops per byte because R3 only
   // commits on an SR read, and 16-byte passes vs 128 so fixed per-pass
   // overhead is 8x higher per byte), so on a 486SX a guest-trapped pass
   // overruns that window, ticks nest, and the ISR stack marches into
   // .data. Verified on the PC110: SBERTC=6 (pinned 1024 Hz) runs DOOM
   // where the free-running ratchet wedges it.
   scp_rtc_setrate((unsigned char)(scp_rtc_rs - 1));
  }
 }
 return 1;                                            // every IRQ8 is ours by construction
}

// VSBHDA sndcard_info_s: 14 fields. No card_info / fm / mixer slots -- the
// this card has no FM at all (see the pt_ops note) and no mixer slots.
struct sndcard_info_s SCP55_sndcard_info={
 // Name the SILICON, not a board: this backend drives the CS4231A on the
 // Roland SCP-55.  Naming the silicon rather than the board matches
 // sc_vew211 ("CS4231A") and sc_tp755 ("CS4248").
 // Matches sc_tp755 ("CS4248") and sc_es1688 ("ES1688"). This string is
 // display only -- /CARD:SCP55 is matched by PTOPS_CardIs, not by this.
 "CS4231A",                                           // shortname
 0,                                                   // infobits
 &SCP55_adetect,                                     // card_detect
 &SCP55_start, &SCP55_stop, &SCP55_close,          // start / stop / close
 &SCP55_setrate,                                     // card_setrate
 &SCP55_writedata, &SCP55_getbufpos, NULL,          // writedata / getpos / clear
 &SCP55_irq,                                         // irq_routine
 NULL, NULL, NULL                                     // mixer slots
};

#endif // NOSCP55
