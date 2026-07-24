//**************************************************************************
//*  sc_es1688.c - SBEMU output driver for the ESS ES1688 AudioDrive,
//*  as found on the Ratoc REX-5571 PCMCIA sound card.
//*
//*  (C) copyright 2026 zikolas
//*  Built against SBEMU (https://github.com/crazii/SBEMU) and its MPXplay
//*  "au_cards" driver interface, (C) PDSoft (Attila Padar).
//*
//*  This is free software: you may redistribute it and/or modify it under
//*  the terms of the GNU General Public License version 2 as published by
//*  the Free Software Foundation. Distributed WITHOUT ANY WARRANTY. See the
//*  COPYING file at the root of this project.
//**************************************************************************
//  sc_es1688.c - SBEMU output driver for the ESS ES1688 (Ratoc REX-5571).
//
//  The PC110/755C PCMCIA bridge has no ISA DMA to the socket, so this card
//  can't service DMA-driven SB playback the normal way. This driver bridges
//  that gap: SBEMU emulates the SB in V86, renders 16-bit stereo, and hands us
//  the stream; we push it to the real ES1688 by PIO.
//
//  *** OUTPUT PATH: extended-mode FIFO block feed (not direct-DAC) ***
//  The earlier revision of this driver hand-fed ONE sample per IRQ0 tick via
//  direct-DAC (DSP 0x10). That costs DAC_RATE heavy V86 interrupts/second and
//  saturates a 486 by ~11 kHz. Instead we now run the ES1688's 256-byte PLAY
//  FIFO in extended mode: the chip clocks samples itself at the programmed
//  rate, and we merely top the FIFO up in 128-byte blocks, paced by the
//  FIFO-half-empty flag (base+0xC bit 3), from inside the RTC pump we already
//  run at 1024 Hz. So there is NO per-sample interrupt at all -- the same audio
//  moves in cheap bursts, removing the CPU ceiling that limited direct-DAC.
//
//  Timers:
//    * IRQ8 / RTC at 1024 Hz (card_irq = 8): SBEMU's output pump. Our irq
//      routine acks the RTC (so SBEMU's MAIN_Interrupt refills the ring) AND
//      tops up the ES1688 FIFO from the ring. That's the only timer we use.
//
//  The real chip does the DAC, rate conversion and clocking; we generate NO
//  host interrupt from the card (B1=0x20), so we never collide with the SB IRQ
//  that SBEMU injects into the guest.
//
//  SBEMU feeds 16-bit stereo; we down-mix to 8-bit mono into the ring in
//  ES1688_writedata. The SB must be brought up at its base by ES1688GO first.
//**************************************************************************
#ifndef NOES1688   /* VSBHDA inclusion convention (was crazii AU_CARDS_LINK_ES1688) */

#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <go32.h>
#include <sys/farptr.h>   // telemetry pokes into the BIOS IAC area (0x4F0)
#include <stdlib.h>
#include <pc.h>
#include "au_cards.h"

// ==========================================================================
//  crazii DPMI-API compat shim over DJGPP (VSBHDA port)
//  ----------------------------------------------------------------------
//  This driver was written against crazii SBEMU's dpmi/dpmi.h. On VSBHDA the
//  audio-path ISR is installed by VSBHDA itself: it hooks our card_irq (=8,
//  RTC) with SNDISR_Interrupt, which calls our irq_routine (ES1688_irq). So
//  the driver needs NO ISR-install of its own for playback.
//
//  The aux WATCHDOG handlers (card TC on IRQ5, PIT heartbeat on IRQ0) are
//  DEFERRED for this first port: the install shims fail-safe (return !=0), so
//  es_i5_on/es_i8_on stay 0 and those handlers never run. If the chip turns
//  out to need its TC IRQ serviced on this stack, wire a REAL go32 install
//  here (_go32_dpmi_*) -- but MEASURE on the box before adding interrupt code.
//
//  Only the interrupt save/restore is real: PUSHF captures the (virtual) IF,
//  int 31h ax=0900h/0901h is VSBHDA's own DPMI disable/enable mechanism.
// ==========================================================================
#ifndef TRUE
#define TRUE  1
#define FALSE 0
#endif
typedef struct { int intno; } DPMI_ISR_HANDLE;
static int  DPMI_InstallISR(int intno, void (*isr)(void), DPMI_ISR_HANDLE *h, int chain)
{ (void)intno; (void)isr; (void)h; (void)chain; return -1; }   // deferred (see note)
static void DPMI_UninstallISR(DPMI_ISR_HANDLE *h){ (void)h; }
static void DPMI_CallOldISR(DPMI_ISR_HANDLE *h){ (void)h; }
static uint8_t DPMI_DisableInterrupt(void)
{
 unsigned f;
 __asm__ __volatile__("pushfl; popl %0" : "=r"(f));           // capture virtual IF
 __asm__ __volatile__("movw $0x0900,%%ax; int $0x31" ::: "eax","cc");
 return (f & 0x200) ? 1 : 0;
}
static void DPMI_RestoreInterrupt(uint8_t prev)
{ if(prev) __asm__ __volatile__("movw $0x0901,%%ax; int $0x31" ::: "eax","cc"); }

// crazii mpxplay heap/print helpers -> DJGPP libc
#define pds_calloc            calloc
#define pds_free              free
#define pds_textdisplay_printf(s)  printf("%s\n", (s))

// SNDISR passthrough tap enable. Set to 1 in ES1688_adetect when our card is the
// selected AU output; sndisr.c reads it (extern) to route the RAW guest DMA to
// ES1688_PT_Feed() -- native passthrough, no resample -- instead of the render
// path. 0 for every other card, so stock cards are unaffected.
int ES1688_PT = 0;
// Set by sndisr.c around the render loop: 1 while the owning SNDISR is in the
// heavy render+tap path, so a re-entrant SNDISR (SETIF=1) skips the render and
// can't march the private ISR stack into the data segment (#GP fix).
volatile int es_in_render = 0;

// DIAG (hardware-test 1 follow-up): prove whether SNDISR fires on our RTC.
//   0x4F6 = 0xAA once ES1688_start runs (also confirms the _farpokeb mechanism
//           works in the resident context -- we KNOW start ran: the pop).
//   0x4F7 = count of SNDISR_Interrupt entries (via ES1688_dbg_tick, called at
//           the very top of SNDISR before AU_isirq).
//   0x4F8 = count of ES1688_irq (irq_routine) calls, i.e. SNDISR reaching AU_isirq.
// If 0x4F6==0xAA but 0x4F7==0 -> SNDISR never runs (RTC not reaching our PM ISR).
//
// Reentrancy + duration probes (hardware-test 3 follow-up; the fix is option
// (ii): keep SNDISR inside one RTC period so it never re-enters at all):
//   0x4F0 = MAX SNDISR nesting depth seen (1 = never re-entered = goal)
//   0x4F1 = render-guard skips (SNDISR re-entered while the owner rendered)
//   0x4FC/0x4FD = longest outermost SNDISR pass, 16-bit LE, in 256-TSC-cycle
//                 units (~1.1us @233MHz). RTC period @1024Hz ~ 890 units --
//                 max duration must stay well under that.
//   0x4FE = PT_Feed overfeed clamps (ring full; should stay 0 -- sndisr paces
//           by ES1688_PT_Space now)
#define ES_DIAG_TSC 1   /* rdtsc: fine on the 235 (P-MMX has a TSC); set 0 for a real 486 build! */
static unsigned char es_tel_sndisr, es_tel_irq;
static unsigned char es_dbg_depth, es_dbg_maxdepth, es_dbg_skips;
#if ES_DIAG_TSC
static unsigned long long es_dbg_t0;
static unsigned es_dbg_maxdur;
static unsigned long long es_rdtsc(void){ unsigned long long v; __asm__ __volatile__("rdtsc" : "=A"(v)); return v; }
#endif
void ES1688_dbg_tick(void)
{
 _farpokeb(_dos_ds, 0x4F7, ++es_tel_sndisr);
 if(++es_dbg_depth > es_dbg_maxdepth){ es_dbg_maxdepth = es_dbg_depth; _farpokeb(_dos_ds, 0x4F0, es_dbg_maxdepth); }
#if ES_DIAG_TSC
 if(es_dbg_depth == 1) es_dbg_t0 = es_rdtsc();
#endif
}
void ES1688_dbg_exit(void)
{
#if ES_DIAG_TSC
 if(es_dbg_depth == 1){
  unsigned long long dt = es_rdtsc() - es_dbg_t0;
  unsigned u = ((dt >> 8) > 0xFFFFULL) ? 0xFFFFu : (unsigned)(dt >> 8);
  if(u > es_dbg_maxdur){
   es_dbg_maxdur = u;
   _farpokeb(_dos_ds, 0x4FC, (unsigned char)u);
   _farpokeb(_dos_ds, 0x4FD, (unsigned char)(u >> 8));
  }
 }
#endif
 if(es_dbg_depth) es_dbg_depth--;
}
void ES1688_dbg_reenter(void){ _farpokeb(_dos_ds, 0x4F1, ++es_dbg_skips); }
// which: 0 = SNDISR render-loop body entered (VSB_Running() true) -> 0x4F4;
//        1 = reached the tap point (inside !IsSilent, after the guest-DMA read) -> 0x4F5.
// loop>0 & reach=0 => IsSilent always true; loop=0 => VSB_Running() false; reach>0 & feed=0 => tap condition fails.
static unsigned char es_dbg_loop, es_dbg_reach;
void ES1688_dbg_mark(int which){
 if(which==0) _farpokeb(_dos_ds, 0x4F4, ++es_dbg_loop);
 else         _farpokeb(_dos_ds, 0x4F5, ++es_dbg_reach);
}

#define ES_DEF_BASE   0x220
#define RING_BYTES    8192U         // MUST keep card_dmasize (=RING*4) within SBEMU's MAIN_PCM
#define RING_MASK     (RING_BYTES-1)
#define BYTES_PER_SBSAMPLE 4        // SBEMU renders 16-bit stereo
#define DAC_RATE_DEF  22050         // default rate (override with env DACRATE)
#define DAC_RATE_MIN  2000
#define DAC_RATE_MAX  44100
#define RTC_RS_DEF    0x06          // RTC rate select 6 -> 1024 Hz (tune with SET SBERTC=6/7/8)
#define ES_FIFO       0x0F          // extended FIFO data port (base+0xF)
#define ES_STAT       0x0C          // DSP write-status; bit7 busy, bit3 FIFO-half-empty
#define FIFO_HE       0x08          // base+0xC bit 3
// SELF-PACING pump (default, unless SBERTC forces a fixed rate). RTC rate-select:
// freq = 32768 >> (rs-1), so a HIGHER rs = LOWER Hz. We ride between a fast active
// cap and a slow idle floor. RTC_RS = 6/7/8/9/10/11 -> 1024/512/256/128/64/32 Hz.
#define ES_RS_MIN     6             // fastest pump = 1024 Hz (CPU cap; feedback never exceeds it)
#define ES_RS_IDLE    11            // idle keep-alive = 32 Hz (light on the DOS prompt)
#define ES_RING_LOW   256           // ring-used below this while streaming = starving -> ratchet up
#define ES_IDLE_GAP   4             // BIOS ticks (~220ms) with no passthrough feed -> idle-throttle. The
                                    // hysteresis is CRUCIAL: single-cycle detection calls SBEMU_Stop every
                                    // cycle (es_pt_active flickers), so idle must key on feed RECENCY, not
                                    // es_pt_active, or the pump stalls to 32Hz mid-detect -> seconds-long launch.

typedef struct es1688_card_s { uint16_t base; } es1688_card_s;

// 16-point unsigned sine (one cycle) -> tiled = a ~rate/16 Hz test tone
static const unsigned char es_sine16[16] =
    {128,177,218,246,255,246,218,177,128,79,38,10,0,10,38,79};

// ring: 8-bit UNSIGNED mono, written by SBEMU pump, drained into the FIFO
static volatile unsigned ring_wr, ring_rd;
static unsigned char      ring_buf[RING_BYTES];
static uint16_t           es_base = ES_DEF_BASE;
static unsigned           es_dacrate = DAC_RATE_DEF;
static unsigned char      es_rtc_rs = RTC_RS_DEF;    // current armed RTC pump rate (SET SBERTC=n forces it fixed)
static int                es_adaptive;               // 1 = self-pace the pump (no SBERTC override)
static DPMI_ISR_HANDLE    es_i5_handle;     // real IRQ5 handler for the card's TC IRQ
static int                es_i5_on;
static int                es_irqtone;       // IRQTONE diag: feed a tone FROM ES1688_irq
static int                es_ringtone;      // RINGTONE diag: tone through the RING like real audio
static unsigned           es_tonepos;

// The card's terminal-count IRQ (IRQ5) must be genuinely serviced or auto-init
// stalls. Lean handler: acknowledge the chip (read base+0x0E) and EOI. Runs
// independently of SBEMU's guest SB IRQ (IRQ7), so no collision.
static void rtc_enable(void);                 // fwd: watchdog re-arm below
static void es_rtc_setrate(unsigned char rs); // fwd: ISR-safe periodic-rate change (feedback ratchet)
static unsigned char es_rs_for_brate(unsigned brate); // fwd: feed-forward rate from stream byte-rate
static volatile uint32_t es_last_tick;        // BIOS tick of last pump run
static volatile uint32_t es_last_feed;        // BIOS tick of last PASSTHROUGH feed -> active(streaming/detecting) vs idle
static int                es_pt_ever;         // a passthrough stream has run -> trust the feed clock; self-pace only then (SBEMU's own render path feeds silence continuously, so it can't drive idle detection)

// WATCHDOG: something in the field (most likely the BIOS INT 70h handler
// catching a tick it didn't request) silently DISARMS the RTC periodic
// interrupt after a while -- the pump dies and all audio freezes until reboot
// (proven on the ThinkPad 235: manual re-arm over COMrade resurrected a dead
// stack mid-game). Any code that still runs without RTC ticks re-arms it when
// ticks look stale: the card's own IRQ5 TC handler (fires as long as the DAC
// cycles, guest-independent) and ES1688_irq itself (reached via the DSP
// poll-pump when a guest polls during starvation).
static void es_watchdog(void)
{
 uint32_t now = _farpeekl(_dos_ds, 0x46C);
 if(now - es_last_tick >= 2) rtc_enable();    // >=~110ms without a tick: re-arm
}


static void es_irq5_isr(void)
{
 es_watchdog();                        // pump heartbeat: UNCONDITIONAL. This handler is
                                       // the guest-independent revival host (issue #2);
                                       // gating it on the status bit killed the pump
                                       // mid-game when the bit read 0 on TC interrupts.
 if(inportb(es_base + 0x0E) & 0x80)    // ours: the status read is also the ack
  outportb(0x20, 0x20);                // EOI master PIC (IRQ5)
 else
  DPMI_CallOldISR(&es_i5_handle);      // not ours: chain (CD-20X shares this line with ATAPI)
}

static void es_iodelay(unsigned n){ while(n--) inportb(0x80); }
static int  es_dsp_reset(uint16_t base)
{
 int i;
 outportb(base+0x06,1); es_iodelay(100);
 outportb(base+0x06,0); es_iodelay(300);
 for(i=0;i<4000;i++) if(inportb(base+0x0E)&0x80) break;
 return inportb(base+0x0A)==0xAA;
}
static void es_dsp_cmd(uint16_t base, unsigned char c)
{
 int i; for(i=0;i<4000;i++) if(!(inportb(base+ES_STAT)&0x80)){ outportb(base+ES_STAT,c); return; }
}
static void es_ewr(uint16_t base, unsigned char reg, unsigned char val)
{
 es_dsp_cmd(base, reg); es_dsp_cmd(base, val);   // ESS extended-register write
}

// ==========================================================================
//  PASSTHROUGH: play the guest's RAW PCM natively -- no resample, no format
//  conversion, no mix. SBEMU's core loop hands us the raw guest bytes plus its
//  rate/bits/channels; we set the ES1688 to that exact format and feed the
//  bytes straight to the FIFO. 8-bit SB PCM is UNSIGNED, so with B6=0x00 the
//  bytes need no conversion at all; 16-bit SB PCM is signed (B6=0x80).
// ==========================================================================
static volatile int es_pt_active;
static unsigned es_pt_rate, es_pt_bits, es_pt_channels;
static unsigned char es_tel_stop, es_tel_recfg, es_tel_feed;   // telemetry counters

// main.c hook: every guest DSP RESET calls this, so a pump that died while the
// system sat idle (no polls, DAC halted, no hosts) is revived BEFORE the next
// game's SB detect runs. Active playback never needs it; idle death did.
void ES1688_PT_Watchdog(void)
{
 es_watchdog();               // revive the RTC pump if it died while idle
 // Real-SB semantics: a DSP reset kills the current transfer. Flush the ring
 // and force a FULL chip re-arm on the next feed -- an idle-stalled chip with
 // a format-matching next stream otherwise skips the reconfig and stays dead
 // (the 'pinball -> idle -> pinball silent' case).
 ring_wr = ring_rd = 0;
 es_pt_active = 0;
 es_pt_rate = es_pt_bits = es_pt_channels = 0;
}

static void es_pt_reconfig(unsigned rate, unsigned bits, unsigned channels)
{
 unsigned a1, a2, brate; int i; uint16_t base = es_base;
 if(rate < 4000) rate = 4000; if(rate > 44100) rate = 44100;
 // The ESS clock counts BYTES: in stereo each channel gets half of it (DOSBox-X
 // halves its channel rate for ESS stereo). SBEMU hands us the per-channel
 // rate, so program A1/A2 from the byte rate or stereo runs at half speed
 // (field-reported on Duke3D's sound test).
 brate = rate * ((channels >= 2) ? 2U : 1U);
 if(brate > 44100) brate = 44100;
 if(brate >= 6215) a1 = 256 - (unsigned)((795444UL + brate/2)/brate);
 else              a1 = 128 - (unsigned)((397722UL + brate/2)/brate);
 a2 = 256 - (unsigned)((218293UL + brate/2)/brate);
 es_dsp_reset(base);
 es_dsp_cmd(base, 0xC6);
 es_ewr(base, 0xB8, 0x04);                          // auto-init
 es_ewr(base, 0xA8, (channels >= 2) ? 0x11 : 0x12); // stereo : mono (bit4: reserved, ALWAYS 1
                                                    // per the DOSBox-X-documented A8 layout --
                                                    // with it clear the chip never entered
                                                    // stereo: L/R collapsed to centre + buzz)
 es_ewr(base, 0xA1, (unsigned char)a1);
 es_ewr(base, 0xA2, (unsigned char)a2);
 es_ewr(base, 0xA4, 0x00); es_ewr(base, 0xA5, 0xFE);
 if(bits >= 16){ es_ewr(base,0xB6,0x80); es_ewr(base,0xB7,0x71); es_ewr(base,0xB7,0xF4); }
 else          { es_ewr(base,0xB6,0x00); es_ewr(base,0xB7,0x51); es_ewr(base,0xB7,0xD0); }
 es_ewr(base, 0xB1, 0x50); es_ewr(base, 0xB2, 0x50);
 // SBPro-style stereo bit (mixer reg 0x0E bit1): on ES688/1688-class chips this
 // is the strong candidate for what ACTUALLY engages stereo (A8+B7 alone left
 // the chip in mono: L/R collapsed to centre and panned content buzzed).
 outportb(base + 0x04, 0x0E);
 outportb(base + 0x05, (channels >= 2) ? 0x22 : 0x20);  // bit1 stereo, bit5 filter off
 for(i=0;i<256;i++) outportb(base+ES_FIFO, (bits>=16)?0x00:0x80);  // prime silence (8-bit FIFO is UNSIGNED per B7 bit5=0)
 es_dsp_cmd(base, 0xD1);
 es_ewr(base, 0xB8, 0x05);
 es_pt_rate = rate; es_pt_bits = bits; es_pt_channels = channels;
 _farpokeb(_dos_ds, 0x4FA, ++es_tel_recfg);                    // telemetry
 if(es_adaptive){                             // FEED-FORWARD: size the pump to this stream, re-arm.
  unsigned fifob = brate * ((bits >= 16) ? 2U : 1U);  // true FIFO drain bytes/sec (16-bit doubles it)
  es_rtc_rs = es_rs_for_brate(fifob);         // feedback ratchets faster from here if the game starves it
  rtc_enable();
 }
}

// drain ring -> FIFO, paced by the FIFO-half-empty flag. Called both from the
// RTC tick AND inline from PT_Feed, so the FIFO stays fed even during a long
// ring-fill -- decoupling smooth output from the (CPU-costly) interrupt rate.
static volatile int es_pump_busy;   // reentrancy guard: only one pump may walk ring_rd at a time
static void es_fifo_pump(void)
{
 uint16_t base = es_base; int guard = 0;
 unsigned char sil = (es_pt_active && es_pt_bits >= 16) ? 0x00 : 0x80;
 unsigned unit = 1;
 if(es_pump_busy) return;            // RTC tick vs inline PT_Feed (or a nested light-path tick):
 es_pump_busy = 1;                   // the outer call owns the ring_rd walk -- don't corrupt it
 // NOTE (DOSBox-X authority): B7 bit5=0 in our 0xD0 config = FIFO data is
 // UNSIGNED. The old XOR here fed signed data - a periodic tone masks that
 // mangling, real audio doesn't. Everything 8-bit is unsigned end-to-end now.
 // Data/silence must be spliced in WHOLE SAMPLE FRAMES: in stereo the FIFO is
 // interleaved L,R and a lone silence byte flips channel parity mid-stream -
 // a click per flip and a swapped image after (Duke3D stereo crackle; mono
 // immune, exactly as field-reported).
 if(es_pt_active){
  unit = (es_pt_channels >= 2) ? 2 : 1;
  if(es_pt_bits >= 16) unit *= 2;
  if(unit > 4) unit = 4;
 }
 while((inportb(base+ES_STAT) & FIFO_HE) && guard < 512){
  unsigned rd = ring_rd, wr = ring_wr; int k;
  for(k=0;k<128;k+=(int)unit){
   unsigned u;
   if((((wr - rd) & RING_MASK)) >= unit){
    for(u=0;u<unit;u++){ outportb(base+ES_FIFO, ring_buf[rd]); rd = (rd+1)&RING_MASK; }
   }else{
    for(u=0;u<unit;u++) outportb(base+ES_FIFO, sil);
   }
  }
  ring_rd = rd; guard += 128;
 }
 es_pump_busy = 0;
}

// how many raw bytes the ring can accept right now (called by SBEMU core).
// The fill is CAPPED to a target depth: a full 8KB ring is ~0.75s of standing
// latency at 11kHz (audible as a press-to-sound lag in DOOM). The cap keeps
// enough buffered to ride out pump jitter and no more. SET SBEPTLAT=ms tunes
// it (default 250ms; raise it if audio breaks up on a slower host).
static unsigned es_pt_lat_ms = 250;
int ES1688_PT_Space(void)
{
 unsigned used = (ring_wr - ring_rd) & RING_MASK;
 unsigned target = RING_BYTES - 64;
 if(es_pt_rate){
  unsigned bps = es_pt_rate * es_pt_channels * ((es_pt_bits + 7) / 8);
  target = (unsigned)((unsigned long)bps * es_pt_lat_ms / 1000UL);
  if(target > RING_BYTES - 64) target = RING_BYTES - 64;
  if(target < 512) target = 512;                  // never starve the FIFO pump
 }
 if(used >= target) return 0;
 return (int)(target - used);
}

// telemetry helper (BIOS IAC-area reporting in main.c's tap)
int ES1688_PT_Used(void)
{
 return (int)((ring_wr - ring_rd) & RING_MASK);
}

// feed raw guest PCM (called by SBEMU core, reconfigures the chip on format change)
// REENTRANCY: VSBHDA's SNDISR runs with interrupts ENABLED (SETIF, sndisr.c:325),
// so while our tap is mid-flight the next RTC tick can re-enter SNDISR -> our tap.
// es_pt_reconfig is LONG (DSP resets + 256-byte FIFO prime + port-I/O spins); a
// re-entry there corrupted the ISR stack and #GP'd into crt0 (hardware-test 2).
// Fix: a busy guard on PT_Feed + interrupts OFF around the one long reconfig.
static volatile int es_pt_feed_busy;
static unsigned char es_reentry;   // DIAG: reentrant PT_Feed calls skipped (0x4F3)
void ES1688_PT_Feed(const unsigned char *buf, int bytes, unsigned rate, unsigned bits, unsigned channels)
{
 unsigned wr;
 _farpokeb(_dos_ds, 0x4F2, 1);                                 // DIAG stage: entry
 if(es_pt_feed_busy){ _farpokeb(_dos_ds, 0x4F3, ++es_reentry); return; }   // re-entered -> skip
 es_pt_feed_busy = 1;
 _farpokeb(_dos_ds, 0x4FB, ++es_tel_feed);                     // telemetry
 es_last_feed = _farpeekl(_dos_ds, 0x46C);                     // audio flowing -> keep the pump fast
 if(!es_pt_active || rate != es_pt_rate || bits != es_pt_bits || channels != es_pt_channels)
 {
  uint8_t f = DPMI_DisableInterrupt();         // IF off: SNDISR can't re-enter during the long reconfig
  _farpokeb(_dos_ds, 0x4F2, 2);                                // DIAG stage: reconfig
  es_pt_reconfig(rate, bits, channels);
  es_pt_active = 1; es_pt_ever = 1;            // passthrough confirmed -> self-pacer runs
  DPMI_RestoreInterrupt(f);
 }
 _farpokeb(_dos_ds, 0x4F2, 3);                                 // DIAG stage: ring fill
 wr = ring_wr;
 // Ring overrun guard: never write past the read pointer. sndisr.c paces the
 // tap by ES1688_PT_Space(), so this should never clamp (0x4FE counts if it
 // does) -- but PT_Feed must not corrupt the ring however it's called.
 { unsigned es_free = (ring_rd - wr - 1u) & RING_MASK;
   if((unsigned)bytes > es_free){
    static unsigned char es_tel_drop;
    _farpokeb(_dos_ds, 0x4FE, ++es_tel_drop);
    bytes = (int)es_free;
   } }
 while(bytes > 0){
  int chunk = (int)(RING_BYTES - wr);      // contiguous space to end of ring
  if(chunk > bytes) chunk = bytes;
  memcpy(ring_buf + wr, buf, chunk);
  buf += chunk; bytes -= chunk;
  wr = (wr + chunk) & RING_MASK;
 }
 ring_wr = wr;
 _farpokeb(_dos_ds, 0x4F2, 4);                                 // DIAG stage: pump
 es_fifo_pump();   // keep the FIFO fed inline, so it never drains during the fill
 _farpokeb(_dos_ds, 0x4F2, 5);                                 // DIAG stage: done
 es_pt_feed_busy = 0;
}

//--- bring the ES1688 up in extended mode for continuous FIFO playback -------
// A1 = sample-rate divider, A2 = filter clock, both derived from the rate.
// B1/B2 = 0x50 is the only config confirmed to actually RUN the DAC in auto-init
// (proven audible in the rex5571-piodma tests); 0x20/0x00 leaves the DAC idle
// and the FIFO never drains -> silence. B8=0x04/0x05 => auto-init so playback
// never terminal-counts to a stop; we keep the FIFO fed by hand. The card's
// resulting terminal-count IRQ (on IRQ 5) must be genuinely serviced or auto-init
// stalls, so es_irq5_isr handles it (ack base+0x0E + EOI) on its own vector --
// independent of the guest SB IRQ (IRQ 7) SBEMU injects, so no collision. 8-bit
// signed mono (B6=0x80), so UNSIGNED ring bytes are XORed with 0x80 into the FIFO.
static void es_fifo_setup(uint16_t base, unsigned rate)
{
 unsigned a1, a2; int i;
 if(rate >= 6215) a1 = 256 - (unsigned)((795444UL + rate/2)/rate);   // high clock
 else             a1 = 128 - (unsigned)((397722UL + rate/2)/rate);   // low clock
 a2 = 256 - (unsigned)((218293UL + rate/2)/rate);                    // reconstruction filter

 es_dsp_reset(base);
 es_dsp_cmd(base, 0xC6);                 // enable extended mode
 es_ewr(base, 0xB8, 0x04);              // auto-init
 es_ewr(base, 0xA8, 0x12);              // mono (bit4 always 1)
 es_ewr(base, 0xA1, (unsigned char)a1);
 es_ewr(base, 0xA2, (unsigned char)a2);
 es_ewr(base, 0xA4, 0x00); es_ewr(base, 0xA5, 0xFE);   // reload count -512
 es_ewr(base, 0xB6, 0x80);              // (inert: B6 is the DAC direct-access holding reg, not a format reg)
 es_ewr(base, 0xB7, 0x51); es_ewr(base, 0xB7, 0xD0);   // 8-bit mono transfer format
 es_ewr(base, 0xB1, 0x50);              // IRQ + FIFO run enable (0x20/0x00 stays idle)
 es_ewr(base, 0xB2, 0x50);              // DMA-mode enable -> DAC actually consumes FIFO
 for(i=0;i<256;i++) outportb(base+ES_FIFO, 0x80);      // prime FIFO with UNSIGNED silence (B7=0xD0, bit5=0)
 es_dsp_cmd(base, 0xD1);                // speaker on
 es_ewr(base, 0xB8, 0x05);              // start (run bit)
}
// Full driver reset for SBRESET (soft reset instead of a reboot): quiesce,
// clear all cached passthrough state, bring the chip back up fresh, re-arm the
// pump. Called from MAIN_Interrupt when the reset magic is seen on the trap.
void ES1688_PT_FullReset(void);

static void es_fifo_stop(uint16_t base)
{
 es_ewr(base, 0xB8, 0x00);              // clear run
 es_dsp_reset(base);
 es_dsp_cmd(base, 0xD3);                // speaker off
}

void ES1688_PT_FullReset(void)
{
 ring_wr = ring_rd = 0;
 es_pt_active = 0;
 es_pt_rate = es_pt_bits = es_pt_channels = 0;  // force a full reconfig on next feed
 es_dsp_reset(es_base);
 es_fifo_setup(es_base, es_dacrate);            // fresh chip bring-up, DAC running
 rtc_enable();                                  // and make sure the pump is alive
}

//--- RTC (IRQ8) periodic: SBEMU's pump clock (card_irq=8) --------------------
static void rtc_enable(void)
{
 uint8_t f = DPMI_DisableInterrupt();
 outportb(0x70,0x8A); { unsigned char a=(unsigned char)inportb(0x71); outportb(0x70,0x8A); outportb(0x71,(a&0xF0)|es_rtc_rs); }
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
// Change ONLY the periodic rate (reg A); leave the enable (reg B) alone. ISR-safe
// (brief IF disable) so the feedback ratchet can retune from inside ES1688_irq.
static void es_rtc_setrate(unsigned char rs)
{
 uint8_t f = DPMI_DisableInterrupt();
 outportb(0x70,0x8A); { unsigned char a=(unsigned char)inportb(0x71); outportb(0x70,0x8A); outportb(0x71,(a&0xF0)|(rs&0x0F)); }
 DPMI_RestoreInterrupt(f);
 es_rtc_rs = rs;
}
// Feed-forward: the smallest RTC frequency (largest rate-select = least CPU) that
// still refills the 256-byte FIFO in ~128-byte bites at the stream's byte rate.
// The feedback loop ratchets FASTER from here when the game starves the pump.
static unsigned char es_rs_for_brate(unsigned brate)
{
 unsigned need = brate / 96 + 1;              // 128-byte refills, ~1.3x headroom
 unsigned char rs;
 for(rs = ES_RS_IDLE; rs > ES_RS_MIN; rs--)
  if((32768u >> (rs-1)) >= need) break;
 return rs;                                    // clamped to [ES_RS_MIN..ES_RS_IDLE]
}

static int ES1688_adetect(struct audioout_info_s *aui)
{
 es1688_card_s *card;
 uint16_t base = ES_DEF_BASE;
 const char *e = getenv("SBEBASE");
 const char *r = getenv("DACRATE");
 const char *t = getenv("SBERTC");
 if(e) base = (uint16_t)strtol(e, NULL, 16);
 if(r){ es_dacrate = (unsigned)atoi(r);
        if(es_dacrate<DAC_RATE_MIN) es_dacrate=DAC_RATE_MIN;
        if(es_dacrate>DAC_RATE_MAX) es_dacrate=DAC_RATE_MAX; }
 if(t){ int rs = atoi(t); if(rs>=3 && rs<=15) es_rtc_rs = (unsigned char)rs; }  // SBERTC forces a FIXED rate
 else { es_adaptive = 1; es_rtc_rs = ES_RS_IDLE; }                             // no SBERTC -> self-pace (start idle, ramp on first feed)
 { const char *l = getenv("SBEPTLAT");                                          // passthrough latency cap (ms)
   if(l){ int ms = atoi(l); if(ms >= 30 && ms <= 2000) es_pt_lat_ms = (unsigned)ms; } }
 if(!es_dsp_reset(base)) return 0;
 card = (es1688_card_s *)pds_calloc(1,sizeof(es1688_card_s));
 if(!card) return 0;
 card->base = base; aui->card_private_data = card; es_base = base;
 aui->card_irq = 8;                     // RTC drives SBEMU's pump
 ES1688_PT = 1;                         // arm the SNDISR passthrough tap (raw guest DMA -> chip)
 // NOFM build: 0x388 is left UNtrapped (ptrap.c skips it when opl3=0), so guest
 // AdLib writes reach the ES1688's real ESFM directly -- no fm_write hook needed.
 return 1;
}

static void ES1688_card_info(struct audioout_info_s *aui)
{
 es1688_card_s *card = aui->card_private_data;
 char sout[100];
 sprintf(sout,"ES1688 (REX-5571) extended-mode FIFO PIO @ %4.4Xh, %u Hz (RTC-fed)", card->base, es_dacrate);
 pds_textdisplay_printf(sout);
}

static void ES1688_setrate(struct audioout_info_s *aui)
{
 es1688_card_s *card = aui->card_private_data;
 aui->freq_card = es_dacrate;
 aui->chan_card = 2;
 aui->bits_card = 16;
 aui->card_dmasize = RING_BYTES * BYTES_PER_SBSAMPLE;   // VSBHDA has no card_dma_buffer_size
 es_base = card->base;
}

// IRQ0 heartbeat for the watchdog. During a guest's silent IRQ-wait (DOOM's
// I_StartupSound sits 5-6s doing NO port I/O) neither the poll path nor IRQ5
// runs -- a latched RTC stays dead the whole wait. The PIT always ticks, so a
// tiny CHAINED IRQ0 handler gives the watchdog a guaranteed host: the pump can
// never stay dead longer than one timer tick.
static DPMI_ISR_HANDLE es_i8_handle;
static int es_i8_on;
static void es_irq0_isr(void)
{
 es_watchdog();             // re-arms the RTC if the pump has gone stale
}
static void es_i8_install(void)
{
 if(es_i8_on) return;
 if(DPMI_InstallISR(0x08, &es_irq0_isr, &es_i8_handle, TRUE) != 0) return;  // IRQ0, CHAINED
 es_i8_on = 1;
}
static void es_i8_remove(void)
{
 if(!es_i8_on) return;
 DPMI_UninstallISR(&es_i8_handle);
 es_i8_on = 0;
}

static void es_i5_install(void)
{
 if(es_i5_on) return;
 if(DPMI_InstallISR(0x0D, &es_irq5_isr, &es_i5_handle, FALSE) != 0) return;  // IRQ5 = INT 0x0D
 outportb(0x21, inportb(0x21) & ~0x20);   // unmask IRQ5
 es_i5_on = 1;
}
static void es_i5_remove(void)
{
 if(!es_i5_on) return;
 outportb(0x21, inportb(0x21) | 0x20);    // mask IRQ5
 DPMI_UninstallISR(&es_i5_handle);
 es_i5_on = 0;
}

static void ES1688_start(struct audioout_info_s *aui)
{
 es1688_card_s *card = aui->card_private_data;
 _farpokeb(_dos_ds, 0x4F6, 0xAA);         // DIAG: start ran + poke mechanism works
 ring_wr = ring_rd = 0;
 es_base = card->base;
 es_irqtone = getenv("IRQTONE") ? 1 : 0;  // diag: feed a tone from the IRQ handler
 es_ringtone = getenv("RINGTONE") ? 1 : 0; // diag: tone through ring+pump (the music path)
 es_i5_install();                         // service the card's TC IRQ (IRQ5)
 es_i8_install();                         // IRQ0 heartbeat for the RTC watchdog
 es_fifo_setup(card->base, es_dacrate);   // extended FIFO playback running

 // Diagnostic: SET FIFOTEST=1 -> feed a ~2s tone STRAIGHT into the FIFO (no
 // SBEMU ring). Audible => FIFO path works under SBEMU, bug is the ring-feed.
 // Silent => the extended-mode FIFO itself isn't playing on this host.
 if(getenv("FIFOTEST")){
  unsigned long fed = 0, target = (unsigned long)es_dacrate * 2, guard = 0;
  while(fed < target && guard < 60000000UL){
   if(inportb(card->base+ES_STAT) & FIFO_HE){
    int k; for(k=0;k<128;k++){ outportb(card->base+ES_FIFO, es_sine16[fed&15]); fed++; }
   }
   guard++;
  }
 }

 if(es_adaptive) es_rtc_rs = es_rs_for_brate(es_dacrate);  // sane baseline for the initial/non-PT case; PT streams override via feed-forward
 rtc_enable();                            // IRQ8 -> SBEMU pump + our FIFO feed
}

static void ES1688_stop(struct audioout_info_s *aui)
{
 es1688_card_s *card = aui->card_private_data;
 // Quiesce ONLY -- do not tear the heartbeat down. SBEMU's core calls
 // card_stop on every guest rate change (au_cards.c) and only calls start
 // again once ITS OWN pcm buffer refills, which in passthrough mode may never
 // happen (we tap the guest data before SBEMU's mixer). Killing the RTC pump
 // here therefore killed ALL further guest servicing -- the "no sound after
 // idle" stall (755C) and the order-dependent SBDIAG failures (235). The RTC
 // pump and the IRQ5 TC handler stay installed until close().
 es_fifo_stop(card->base);
 _farpokeb(_dos_ds, 0x4F9, ++es_tel_stop);                     // telemetry
 es_pt_active = 0;            // next passthrough feed does a full chip re-arm
 ring_wr = ring_rd = 0;       // DRAIN the ring: with the DAC halted a full ring
                              // would report no space, so the tap never feeds,
                              // so the re-arm (inside PT_Feed) never runs -- a
                              // permanent silence deadlock (pinball reruns,
                              // Wolf3D one-shot SFX, SBDIAG ordering).
}

static void ES1688_close(struct audioout_info_s *aui)
{
 rtc_disable();
 es_i8_remove();
 es_i5_remove();
 if(aui->card_private_data){ es_fifo_stop(((es1688_card_s*)aui->card_private_data)->base);
                             pds_free(aui->card_private_data); aui->card_private_data=NULL; }
}

// SBEMU pump: 16-bit stereo in -> 8-bit UNSIGNED mono into the ring
static void ES1688_writedata(struct audioout_info_s *aui, char *src, unsigned long bytes)
{
 short *p = (short *)src;
 // The ring has ONE producer at a time. While a passthrough stream is active
 // (or the RINGTONE diag runs), SBEMU's render tail must not also write here:
 // any tick where its digital flag flickers spliced a chunk of rendered
 // silence into the middle of the guest's music -- THE background crackle on
 // continuous streamers (and the chopped RINGTONE that exposed it).
 if(es_pt_active || es_ringtone) return;
 unsigned long n = bytes / BYTES_PER_SBSAMPLE;
 unsigned wr = ring_wr;
 while(n--){
  int mono = ((int)p[0] + (int)p[1]) >> 1;
  p += 2;
  ring_buf[wr] = (unsigned char)((mono >> 8) + 128);
  wr = (wr+1)&RING_MASK;
 }
 ring_wr = wr;
}

static long ES1688_getbufpos(struct audioout_info_s *aui)
{
 return (long)((unsigned long)ring_rd * BYTES_PER_SBSAMPLE);
}

// IRQ8/RTC: ack, top up the ES1688 FIFO from the ring (bit3-paced), and tell
// SBEMU whether it was our interrupt so MAIN_Interrupt refills the ring.
static int ES1688_irq(struct audioout_info_s *aui)
{
 int guard; uint16_t base = es_base;
 _farpokeb(_dos_ds, 0x4F8, ++es_tel_irq);                   // DIAG: irq_routine called (SNDISR reached AU_isirq)
 es_watchdog();                                             // re-arm if ticks died
 es_last_tick = _farpeekl(_dos_ds, 0x46C);                  // stamp this run
 outportb(0x70,0x0C); (void)inportb(0x71);                  // ack RTC

 if(es_irqtone){                                            // diagnostic tone path
  guard = 0;
  while((inportb(base+ES_STAT) & FIFO_HE) && guard < 256){
   int k; for(k=0;k<128;k++) outportb(base+ES_FIFO, es_sine16[es_tonepos++ & 15]);
   guard += 128;
  }
 }
 else if(es_ringtone){
  // RINGTONE: exercise the EXACT music machinery with a known-pure signal --
  // per tick, top the ring up like a producer would, then drain normally.
  // Crackle here = ring/pump defect, provable with no guest and no SBEMU.
  unsigned target = (es_dacrate * 30U) / 1000U;
  while((( ring_wr - ring_rd) & RING_MASK) < target){
   ring_buf[ring_wr] = es_sine16[es_tonepos++ & 15];
   ring_wr = (ring_wr + 1) & RING_MASK;
  }
  es_fifo_pump();
 }
 else es_fifo_pump();                                       // normal / passthrough feed
 // SELF-PACING. Idle needs BOTH signals, because each alone is wrong somewhere:
 //  - es_pt_active flickers off every cycle during single-cycle DMA detection
 //    (SBEMU_Stop per cycle) -> using it alone stalls the pump mid-detect (slow launch)
 //  - the feed clock goes stale when the ring is FULL during steady playback (no
 //    space -> PT loop stops calling PT_Feed) -> using it alone idles mid-play (crawl)
 //  Requiring !es_pt_active AND feed-stale is true only at the real DOS prompt.
 //  Ramp FASTER whenever the ring runs dry (detection or a game starving the RTC).
 if(es_adaptive && es_pt_ever){
  unsigned used = (ring_wr - ring_rd) & RING_MASK;
  // Idle also when the stream is provably dead WITH es_pt_active still set:
  // nothing clears pt_active when a guest just stops (AU never calls card_stop
  // here and no DSP reset follows), so the pump used to park at the stream
  // rate (~128Hz) at the DOS prompt forever. Feed-stale AND ring drained below
  // one frame (<4 covers every unit size) can't be mid-detect (SC2K feeds every
  // cycle -> gap false) nor steady play (ring holds data -> used>=4).
  if((!es_pt_active || used < 4) && (es_last_tick - es_last_feed) >= ES_IDLE_GAP){
   if(es_rtc_rs != ES_RS_IDLE) es_rtc_setrate(ES_RS_IDLE);
  }else if(used < ES_RING_LOW && es_rtc_rs > ES_RS_MIN){
   es_rtc_setrate((unsigned char)(es_rtc_rs - 1));
  }
 }
 // Every IRQ8 is ours by construction (we armed the RTC periodic rate), so
 // always claim it -- SBEMU then runs MAIN_Interrupt and services the guest.
 // Do NOT gate this on the reg-C IRQF readback: on CardBus-era stacks (e.g.
 // ThinkPad 235) the RTC ports read virtualized/zero from this context, and
 // gating on it silently disables ALL guest servicing (pump ticks, no audio).
 // The card's TC IRQ is serviced by es_irq5_isr, not here.
 return 1;
}

// Hardware FM: main.c's MAIN_HW_OPL3IODT traps guest AdLib I/O at 388-38Bh and
// relays it here. The ESFM quad lives at base+0..+3 (OPL3 layout: +0/+1 primary,
// +2/+3 secondary). Do NOT use the +8/+9 mirror as the quad base: on the IBM
// CD-20X's A4-only glue, +A/+B decode as DSP ports and the writes would vanish
// (same mapping CDXMIR proved live: 388-38B -> 220-223). This gives games REAL
// FM on cards with no native 388 window; /OPL0 disables the traps entirely for
// layouts where 388 decodes natively (REX GO.BAT default).
static void ES1688_fm_write(struct audioout_info_s *aui, unsigned int idx, uint8_t data)
{
 outportb(es_base + idx, data);
}
static uint8_t ES1688_fm_read(struct audioout_info_s *aui, unsigned int idx)
{
 return (uint8_t)inportb(es_base + idx);
}

// VSBHDA sndcard_info_s: 14 fields. No card_info / fm / mixer slots -- card_info
// folds into adetect+setrate, FM rides the real ESFM (NOFM leaves 0x388 untrapped).
struct sndcard_info_s ES1688_sndcard_info={
 "ES1688",                                              // shortname
 0,                                                     // infobits
 &ES1688_adetect,                                       // card_detect
 &ES1688_start, &ES1688_stop, &ES1688_close,            // start / stop / close
 &ES1688_setrate,                                       // card_setrate
 &ES1688_writedata, &ES1688_getbufpos, NULL,            // writedata / getpos / clear
 &ES1688_irq,                                           // irq_routine (check+ack)
 NULL, NULL, NULL                                       // writemixer / readmixer / mixerchans
};

#endif // NOES1688
