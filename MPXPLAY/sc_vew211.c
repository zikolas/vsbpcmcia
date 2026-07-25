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
//  *** OUTPUT PATH: CS4231A PIO, PIT-time-paced (NOT flag-gated) ***
//  The CS4231A has a 16-sample playback FIFO clocked by its own crystal at
//  the programmed rate. On the CF-VEW211's CS4231A-KQ behind the MEI ASIC,
//  the PRDY flag is USELESS for pacing -- it reads "ready" continuously while
//  overfeed is silently dropped. So the feed is paced by ABSOLUTE TIME off
//  PIT channel 0 (the proven VEWPLAY design):
//    * perb = PIT ticks per output byte, from the CODEC's actual table rate
//      (not the guest's nominal rate -- the 10989-asked/11025-played mismatch
//      otherwise underruns ~36x/s into a steady crackle).
//    * each pump pass feeds only the frames elapsed real time has earned,
//      capped at the 16-deep FIFO.
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
//  Layout: I/O window base 0x530 (CIS idx 0x20; SBEBASE overrides); the
//  CS4231A answers at base+4..+7 (IAR/IDR/SR/PDR). FM is a DISCRETE YMF262
//  (OPL3) at 0x388 decoding all four ports natively: with NOFM the guest's
//  AdLib I/O reaches it untrapped -- real OPL3, no relay needed. The card
//  must be brought up by VEW21XGO first.
//
//  BACKEND SELECTION: define CARD_VEW211 (config.h) to build this backend;
//  sc_es1688.c self-excludes -- both provide the same passthrough ABI
//  (ES1688_PT_* -- historical names, card-agnostic) and must never both
//  be compiled in.
//**************************************************************************
#ifdef CARD_VEW211

#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <go32.h>
#include <sys/farptr.h>   // telemetry pokes into the BIOS IAC area (0x4F0)
#include <dpmi.h>         // _go32_dpmi_* : the IRQ0 watchdog heartbeat
#include <stdlib.h>
#include <pc.h>
#include "au_cards.h"

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
int ES1688_PT = 0;                 // tap armed (sndisr.c reads this)
volatile int es_in_render = 0;     // sndisr.c render-reentrancy guard flag

static int es_has_tsc;
static int es_tsc_check(void)
{
 unsigned a, b, d;
 __asm__ __volatile__(
  "pushfl; popl %0; movl %0, %1; xorl $0x200000, %0;"
  "pushl %0; popfl; pushfl; popl %0; pushl %1; popfl"
  : "=&r"(a), "=&r"(b));
 if(!((a ^ b) & 0x200000)) return 0;
 __asm__ __volatile__("cpuid" : "=d"(d) : "a"(1) : "ebx", "ecx");
 return (d >> 4) & 1;
}
static uint16_t es_tel_sndisr, es_tel_feed16;
static unsigned long es_tel_bytes;
static unsigned char es_tel_irq;
static unsigned char es_dbg_depth, es_dbg_maxdepth, es_dbg_skips;
static unsigned long long es_dbg_t0;
static unsigned es_dbg_maxdur;
static unsigned long long es_rdtsc(void){ unsigned long long v; __asm__ __volatile__("rdtsc" : "=A"(v)); return v; }
void ES1688_dbg_tick(void)
{
 ++es_tel_sndisr;
 _farpokeb(_dos_ds, 0x4F7, (unsigned char)es_tel_sndisr);
 _farpokeb(_dos_ds, 0x4F9, (unsigned char)(es_tel_sndisr >> 8));
 if(++es_dbg_depth > es_dbg_maxdepth){ es_dbg_maxdepth = es_dbg_depth; _farpokeb(_dos_ds, 0x4F0, es_dbg_maxdepth); }
 if(es_has_tsc && es_dbg_depth == 1) es_dbg_t0 = es_rdtsc();
}
void ES1688_dbg_exit(void)
{
 if(es_has_tsc && es_dbg_depth == 1){
  unsigned long long dt = es_rdtsc() - es_dbg_t0;
  unsigned u = ((dt >> 8) > 0xFFFFULL) ? 0xFFFFu : (unsigned)(dt >> 8);
  if(u > es_dbg_maxdur){
   es_dbg_maxdur = u;
   _farpokeb(_dos_ds, 0x4FC, (unsigned char)u);
   _farpokeb(_dos_ds, 0x4FD, (unsigned char)(u >> 8));
  }
 }
 if(es_dbg_depth) es_dbg_depth--;
}
void ES1688_dbg_reenter(void){ _farpokeb(_dos_ds, 0x4F1, ++es_dbg_skips); }

// ---- card geometry -------------------------------------------------------
#define VEW_WIN_BASE  0x530         // I/O window base (CIS idx 0x20); SBEBASE overrides
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
#define VEW_IDLE_GAP  4             // base value; widened by SBEPTLAT (see adetect)
#define VEW_BURST_FRAMES 16         // per-pass ceiling = FIFO depth

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
static volatile uint32_t vew_last_tick;
static volatile uint32_t vew_last_feed;
static int                vew_pt_ever;

// ---- PIT-channel-0 absolute-time pacing ----------------------------------
static unsigned long vew_pit_per_sec = 1193182UL / 2;
static unsigned long vew_perb = 100;
static unsigned      vew_pit_prev;
static unsigned long vew_acc;

static unsigned vew_pit_read(void)
{
 unsigned lo, hi;
 outportb(0x43, 0x00);
 lo = (unsigned char)inportb(0x40);
 hi = (unsigned char)inportb(0x40);
 return (unsigned)((hi << 8) | lo);
}
static void vew_pit_calibrate(void)
{
 uint32_t t0, guard = 0; unsigned prev, now; unsigned long cal = 0;
 t0 = _farpeekl(_dos_ds, 0x46C);
 while(_farpeekl(_dos_ds, 0x46C) == t0 && ++guard < 4000000UL) ;
 prev = vew_pit_read(); t0 = _farpeekl(_dos_ds, 0x46C); guard = 0;
 while(_farpeekl(_dos_ds, 0x46C) == t0 && ++guard < 4000000UL){
  now = vew_pit_read(); cal += (unsigned long)((prev - now) & 0xFFFFU); prev = now;
 }
 if(cal < 30000UL) cal = 65536UL;
 vew_pit_per_sec = cal * 182UL / 10UL;
}
static void vew_set_byterate(unsigned brate)
{
 if(!brate) brate = 22050U;
 vew_perb = vew_pit_per_sec / brate;
 if(!vew_perb) vew_perb = 1;
}

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
static int vew_rate_pick(unsigned rate)
{
 int i, best = 7; unsigned long bd = 0xFFFFFFFFUL;
 for(i=0;i<VEW_NRATES;i++){
  unsigned long d = vew_rates[i].hz > rate ? vew_rates[i].hz - rate : rate - vew_rates[i].hz;
  if(d < bd){ bd = d; best = i; }
 }
 return best;
}

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
 vew_set_byterate((unsigned)vew_rates[ri].hz * ((channels>=2)?2U:1U) * ((bits>=16)?2U:1U));
 vew_acc = 0; vew_pit_prev = vew_pit_read();
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

// drain ring -> codec FIFO, PIT-time-paced. Called from the RTC tick AND
// inline from PT_Feed.
static void vew_pio_pump(void)
{
 uint16_t cb = vew_codec;
 unsigned unit = 1;
 unsigned char sil;
 unsigned now; unsigned long owed;
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

 (void)inportb(cb+VC_SR);                             // clear stale INT/PUR
 now = vew_pit_read();
 vew_acc += (unsigned long)((vew_pit_prev - now) & 0xFFFFU);
 vew_pit_prev = now;
 owed = vew_perb * unit * VEW_BURST_FRAMES;
 if(vew_acc > owed) vew_acc = owed;

 while(vew_acc >= vew_perb * unit && guard < VEW_BURST_FRAMES){
  unsigned rd = ring_rd, wr = ring_wr, u;
  if(((wr - rd) & RING_MASK) >= unit){
   for(u=0;u<unit;u++){ outportb(cb+VC_PDR, ring_buf[rd]); (void)inportb(cb+VC_SR); rd = (rd+1)&RING_MASK; }
   ring_rd = rd;
  }else{
   for(u=0;u<unit;u++){ outportb(cb+VC_PDR, sil); (void)inportb(cb+VC_SR); }
  }
  vew_acc -= vew_perb * unit;
  guard++;
 }
 vew_pump_busy = 0;
}

// ==========================================================================
//  Passthrough backend ABI (ES1688_PT_* names are the engine's ABI --
//  card-agnostic; here they drive the CS4231A).
// ==========================================================================
static void vew_watchdog(void)
{
 uint32_t now = _farpeekl(_dos_ds, 0x46C);
 if(now - vew_last_tick >= 2){
  static unsigned char es_tel_revive;
  _farpokeb(_dos_ds, 0x4F4, ++es_tel_revive);         // RTC revivals
  rtc_enable();
 }
}

// vsb.c hook: every guest DSP RESET lands here.
void ES1688_PT_Watchdog(void)
{
 vew_watchdog();
 es_flush_gen++;                                      // pump-executed ring flush
 vew_pt_active = 0;
 vew_pt_rate = vew_pt_bits = vew_pt_channels = 0;
}

static unsigned vew_pt_lat_ms = 250;
static unsigned vew_idle_gap_ticks = VEW_IDLE_GAP;
int ES1688_PT_Space(void)
{
 unsigned used = (ring_wr - ring_rd) & RING_MASK;
 unsigned target = RING_BYTES - 64;
 if(vew_pt_rate){
  unsigned bps = vew_pt_rate * vew_pt_channels * ((vew_pt_bits + 7) / 8);
  target = (unsigned)((unsigned long)bps * vew_pt_lat_ms / 1000UL);
  if(target > RING_BYTES - 64) target = RING_BYTES - 64;
  if(target < 512) target = 512;
 }
 if(used >= target) return 0;
 return (int)(target - used);
}

int ES1688_PT_Used(void)
{
 return (int)((ring_wr - ring_rd) & RING_MASK);
}

// feed raw guest PCM (sndisr.c tap); reconfigure the codec on format change.
static volatile int vew_pt_feed_busy;
static unsigned char es_reentry;
void ES1688_PT_Feed(const unsigned char *buf, int bytes, unsigned rate, unsigned bits, unsigned channels)
{
 unsigned wr;
 if(vew_pt_feed_busy){ _farpokeb(_dos_ds, 0x4F3, ++es_reentry); return; }
 vew_pt_feed_busy = 1;
 ++es_tel_feed16;
 _farpokeb(_dos_ds, 0x4FB, (unsigned char)es_tel_feed16);
 _farpokeb(_dos_ds, 0x4FF, (unsigned char)(es_tel_feed16 >> 8));
 vew_last_feed = _farpeekl(_dos_ds, 0x46C);
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
   vew_rs_want = vew_rs_for_frate(rate);              // frames/s = sample rate
   vew_rtc_rs = vew_rs_want;
   rtc_enable();
  }
 }
 wr = ring_wr;
 { unsigned es_free = (ring_rd - wr - 1u) & RING_MASK;  // never lap the consumer
   if((unsigned)bytes > es_free){
    static unsigned char es_tel_drop;
    _farpokeb(_dos_ds, 0x4FE, ++es_tel_drop);
    bytes = (int)es_free;
   } }
 es_tel_bytes += (unsigned long)bytes;
 if(!es_has_tsc){
  unsigned u16 = (unsigned)((es_tel_bytes >> 4) & 0xFFFF);
  _farpokeb(_dos_ds, 0x4FC, (unsigned char)u16);
  _farpokeb(_dos_ds, 0x4FD, (unsigned char)(u16 >> 8));
 }
 while(bytes > 0){
  int chunk = (int)(RING_BYTES - wr);
  if(chunk > bytes) chunk = bytes;
  memcpy(ring_buf + wr, buf, chunk);
  buf += chunk; bytes -= chunk;
  wr = (wr + chunk) & RING_MASK;
 }
 ring_wr = wr;
 vew_pio_pump();                                      // keep the FIFO fed inline
 vew_pt_feed_busy = 0;
}

// Full soft reset: quiesce, clear passthrough state, re-arm.
void ES1688_PT_FullReset(void)
{
 es_flush_gen++;
 vew_pt_active = 0;
 vew_pt_rate = vew_pt_bits = vew_pt_channels = 0;
 vew_codec_config(vew_dacrate, 8, 1);
 rtc_enable();
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
static int VEW211_adetect(struct audioout_info_s *aui)
{
 vew211_card_s *card;
 uint16_t base = VEW_WIN_BASE;
 const char *e = getenv("SBEBASE");
 const char *r = getenv("DACRATE");
 const char *t = getenv("SBERTC");
 const char *v = getenv("SBEVOL");
 const char *l = getenv("SBEPTLAT");
 if(e) base = (uint16_t)strtol(e, NULL, 16);
 vew_base  = base;
 vew_codec = (uint16_t)(base + VEW_CODEC_OFF);
 if(r){ vew_dacrate = (unsigned)atoi(r);
        if(vew_dacrate<DAC_RATE_MIN) vew_dacrate=DAC_RATE_MIN;
        if(vew_dacrate>DAC_RATE_MAX) vew_dacrate=DAC_RATE_MAX; }
 if(v){ int a = atoi(v); if(a>=0 && a<=63) vew_vol = a; }
 if(t){ int rs = atoi(t); if(rs>=3 && rs<=15) vew_rtc_rs = (unsigned char)rs; }
 else { vew_adaptive = 1; vew_rtc_rs = VEW_RS_IDLE; }
 if(l){ int ms = atoi(l); if(ms >= 30 && ms <= 2000) vew_pt_lat_ms = (unsigned)ms; }
 // Idle detection keys on FEED RECENCY; the gap must exceed one full latency
 // drain or steady playback (ring riding above target) would false-idle.
 vew_idle_gap_ticks = VEW_IDLE_GAP + (vew_pt_lat_ms * 2U) / 55U;

 // Codec must already be reachable (run VEW21XGO first): 0xFF on IAR =
 // nothing decoding the port -> card not enabled.
 if((unsigned char)inportb(vew_codec + VC_IAR) == 0xFF) return 0;
 vew_ci_wait(vew_codec);

 es_has_tsc = es_tsc_check();
 card = (vew211_card_s *)pds_calloc(1,sizeof(vew211_card_s));
 if(!card) return 0;
 card->base = base; aui->card_private_data = card;
 aui->card_irq = 8;                                   // RTC drives the pump
 ES1688_PT = 1;                                       // arm the sndisr passthrough tap
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
 _farpokeb(_dos_ds, 0x4F6, 0xAA);                     // DIAG: start ran
 ring_wr = ring_rd = 0;
 vew_base  = card->base;
 vew_codec = (uint16_t)(card->base + VEW_CODEC_OFF);
 vew_pit_calibrate();
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
 vew_watchdog();
 vew_last_tick = _farpeekl(_dos_ds, 0x46C);
 { uint8_t f = DPMI_DisableInterrupt();               // cli: the IRQ0 heartbeat could
   outportb(0x70,0x0C); (void)inportb(0x71);          // land between CMOS index+data
   DPMI_RestoreInterrupt(f); }

 vew_pio_pump();
 _farpokeb(_dos_ds, 0x4F5, (unsigned char)(((ring_wr - ring_rd) & RING_MASK) >> 5));  // ring gauge

 // Self-pacing keyed on FEED RECENCY alone -- deliberately NOT on
 // vew_pt_active (which sticks at 1 when a guest exits mid-stream and would
 // pin the pump at max on an empty ring: the "sluggish prompt" field bug).
 if(vew_adaptive && vew_pt_ever){
  unsigned used = (ring_wr - ring_rd) & RING_MASK;
  if((vew_last_tick - vew_last_feed) >= vew_idle_gap_ticks){
   if(vew_rtc_rs != VEW_RS_IDLE) vew_rtc_setrate(VEW_RS_IDLE);
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

#endif // CARD_VEW211
