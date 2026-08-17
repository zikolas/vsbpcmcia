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
#ifndef NOES1688
   /* All backends now coexist in one binary: the passthrough ABI they used to
    * export in common is dispatched through the ptops.h table, so nothing here
    * clashes with sc_vew211.c or sc_tp755.c any more. */

#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <go32.h>
#include <sys/farptr.h>   // telemetry pokes into the BIOS IAC area (0x4F0)
#include <dpmi.h>         // _go32_dpmi_* : the IRQ0 watchdog heartbeat is a REAL hook now
#include <stdlib.h>
#include <pc.h>
#include "au_cards.h"
#include "ptops.h"        // engine passthrough ops table (we register in adetect)

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
/* REAL install now (was a fail-safe stub, deferred pending evidence). The
 * evidence arrived on the T2130CT: Theme Hospital (DOS4GW/Miles) kills the
 * RTC periodic mid-game -- ticks16 frozen for 6.5s -- and with es_watchdog's
 * only hosts being the RTC ISR itself (dead) and guest DSP resets (none
 * mid-game), the pump stayed dead: silence, then the exit path wedged
 * waiting on SB IRQs. The chained IRQ0 heartbeat is the guest-independent
 * watchdog host (crazii-proven design): the PIT always ticks, so RTC death
 * heals within ~110ms. go32's chain wrapper handles the jump to the old
 * handler (BIOS EOIs); our handler just runs the watchdog and returns. */
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
static void DPMI_CallOldISR(DPMI_ISR_HANDLE *h){ (void)h; }   // still unused: IRQ5 TC handler stays opt-in (see es_i5_install)
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

// DIAG map for this backend's own bytes (the SNDISR entry/depth/duration
// telemetry -- 0x4F0/0x4F1/0x4F7/0x4F9/0x4FC/0x4FD -- moved to sndisr.c
// with the generic dbg trio; full map in doc/NOTES.md):
//   0x4F6 = 0xAA once ES1688_start runs (also confirms the _farpokeb mechanism
//           works in the resident context -- we KNOW start ran: the pop).
//   0x4F8 = count of ES1688_irq (irq_routine) calls, i.e. SNDISR reaching AU_isirq.
//   0x4FB/0x4FF = 16-bit PT_Feed count (lo/hi; 8-bit counters wrap-alias
//           hopelessly over multi-second windows -- the SC2K chop diagnosis
//           needed exactly that).
//   0x4FE = PT_Feed overfeed clamps (ring full; should stay 0 -- sndisr paces
//           by ES1688_PT_Space now)
// On no-TSC CPUs 0x4FC/0x4FD carry a 16-bit PT byte accumulator (>>4)
// instead of the engine's duration probe (SNDISR_HasTsc gates both uses).
static uint16_t es_tel_feed16;
static unsigned long es_tel_bytes;
static unsigned char es_tel_irq;
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

// ring: 8-bit UNSIGNED mono, written by SBEMU pump, drained into the FIFO.
// OWNERSHIP: ring_rd is written ONLY by es_fifo_pump (serialized by
// es_pump_busy); ring_wr only by the feed path. Trap-context code must NEVER
// write either -- it requests a flush via es_flush_gen and the pump snaps
// rd := wr itself (see ES1688_PT_Watchdog for the race this prevents).
static volatile unsigned ring_wr, ring_rd;
static volatile unsigned es_flush_gen, es_flush_ack;
static unsigned char      ring_buf[RING_BYTES];
static uint16_t           es_base = ES_DEF_BASE;
static unsigned           es_dacrate = DAC_RATE_DEF;
static unsigned char      es_rtc_rs = RTC_RS_DEF;    // current armed RTC pump rate (SET SBERTC=n forces it fixed)
static unsigned char      es_rs_want = ES_RS_IDLE;   // the CURRENT STREAM's proper pump rate (feed-forward result).
                                                     // PT_Feed restores it after an idle throttle: between two
                                                     // same-format sounds NO reconfig runs, so nothing else would --
                                                     // the pump stayed at 32Hz and the 256-byte FIFO drained dry
                                                     // between 31ms pump visits (the SC2K mid-session chop).
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
 if(now - es_last_tick >= 2){                 // >=~110ms without a tick: re-arm
  static unsigned char es_tel_revive;
  _farpokeb(_dos_ds, 0x4F4, ++es_tel_revive); // DIAG: RTC revivals (how often a guest kills the RTC)
  rtc_enable();
 }
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
 outportb(base+0x06,3); es_iodelay(100);   // ESS reset: bit1 clears the FIFO (ALSA: 'SB -> 1')
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
static unsigned char es_tel_recfg;                             // telemetry counter (FULL reconfigs only)
// What the CHIP is armed with (0 = not armed), distinct from the STREAM state
// es_pt_*: PT_Watchdog clears the stream state on every guest DSP reset (real
// SB semantics), but the chip usually needs no re-arm at all -- see the
// fast-resume path in es_pt_reconfig.
static unsigned es_hw_rate, es_hw_bits, es_hw_channels;
static volatile uint32_t es_last_drain;   // BIOS tick when FIFO_HE last seen set = chip provably draining

// main.c hook: every guest DSP RESET calls this, so a pump that died while the
// system sat idle (no polls, DAC halted, no hosts) is revived BEFORE the next
// game's SB detect runs. Active playback never needs it; idle death did.
static void ES1688_PT_Watchdog(void)
{
 es_watchdog();               // revive the RTC pump if it died while idle
 // Real-SB semantics: a DSP reset kills the current transfer. But NEVER touch
 // ring_rd/ring_wr from this (trap) context: this runs mid-guest-OUT and can
 // preempt the pump between its ring_rd read and write-back -- one lost race
 // at a sample boundary leaves rd permanently skewed into stale ring bytes
 // (constant fuzz under all audio thereafter; latched at the end of a random
 // sample -- the Theme Hospital hiss). Request the flush instead: the pump
 // (sole owner of ring_rd) executes it within one tick.
 es_flush_gen++;
 es_pt_active = 0;
 es_pt_rate = es_pt_bits = es_pt_channels = 0;
}

static void es_pt_reconfig(unsigned rate, unsigned bits, unsigned channels)
{
 unsigned a1, a2, brate; int i; uint16_t base = es_base;
 if(rate < 4000) rate = 4000; if(rate > 44100) rate = 44100;
 // FAST RESUME: guests that DSP-reset per sound (SC2K: one reset per SFX)
 // clear the stream state via PT_Watchdog, but the CHIP usually needs no
 // re-arm -- same format, DAC running, pump feeding. The full surgery below
 // (DSP reset + ~20 reg writes + 256-byte silence prime) injected a multi-ms
 // gap PLUS ~23ms of primed silence PER SOUND (the SC2K per-SFX chop;
 // 0x4FA showed one reconfig per sound). Skip it when the chip is armed with
 // this exact format AND provably draining: FIFO_HE seen within ~4 BIOS
 // ticks. An idle-STALLED or stopped chip fails the drain test and still
 // gets the full re-arm (the pinball->idle->pinball lesson stands).
 if(rate == es_hw_rate && bits == es_hw_bits && channels == es_hw_channels
    && (uint32_t)(_farpeekl(_dos_ds, 0x46C) - es_last_drain) <= 4){
  es_pt_rate = rate; es_pt_bits = bits; es_pt_channels = channels;
  if(es_adaptive){                            // same feed-forward pump re-arm as the full path
   unsigned fifob2 = rate * ((channels >= 2) ? 2U : 1U);
   if(fifob2 > 44100) fifob2 = 44100;
   fifob2 *= (bits >= 16) ? 2U : 1U;
   es_rs_want = es_rs_for_brate(fifob2);
   es_rtc_rs = es_rs_want;
   rtc_enable();
  }
  return;
 }
 // STEREO RECIPE (2026-07-29, STEREO88 probe, hardware-validated on the
 // REX-5571/235; ES1868 DS Table 11 + PIO section; ALSA es1688_lib.c):
 //  - A1/A2 from the FRAME rate: the chip doubles byte consumption in stereo
 //    itself (the old byte-rate hack only corrected mono-mode playback of
 //    interleaved data and makes true stereo run 2x fast).
 //  - B7's 2nd byte encodes channels (98h/BCh stereo, D0h/F4h mono); the DAC
 //    believes B7 over A8 -- the mono B7 here was the fleet-wide stereo bug.
 //  - B9=01h (2-byte demand) in stereo makes the FIFO unload pair-atomic =
 //    deterministic first-byte-LEFT channel phase (else a per-stream coin
 //    flip). Requires the paced fills in es_fifo_pump.
 //  - B2=10h (PIO: bits 7:5 low), B6 by signedness, prime AFTER the run bit
 //    (B8 bit0 gates system->FIFO fills). 16-bit stereo values are Table-11
 //    faithful but bench-untested (dormant: VSB emulates SBPro, 8-bit only).
 brate = rate * ((channels >= 2) ? 2U : 1U);        // BYTE rate: pump sizing only
 if(brate > 44100) brate = 44100;
 if(rate >= 6215) a1 = 256 - (unsigned)((795444UL + rate/2)/rate);
 else             a1 = 128 - (unsigned)((397722UL + rate/2)/rate);
 a2 = 256 - (unsigned)((218293UL + rate/2)/rate);
 es_dsp_reset(base);
 es_dsp_cmd(base, 0xC6);
 es_ewr(base, 0xB8, 0x04);                          // auto-init
 es_ewr(base, 0xA8, (channels >= 2) ? 0x11 : 0x12); // stereo : mono (bit4 kept set)
 es_ewr(base, 0xB9, (channels >= 2) ? 0x01 : 0x00); // stereo: 2-byte demand (pair-atomic)
 es_ewr(base, 0xA1, (unsigned char)a1);
 es_ewr(base, 0xA2, (unsigned char)a2);
 es_ewr(base, 0xA4, 0x00); es_ewr(base, 0xA5, 0xFE);
 if(bits >= 16){ es_ewr(base,0xB6,0x00); es_ewr(base,0xB7,0x71);
                 es_ewr(base,0xB7,(channels >= 2) ? 0xBC : 0xF4); }
 else          { es_ewr(base,0xB6,0x80); es_ewr(base,0xB7,0x51);
                 es_ewr(base,0xB7,(channels >= 2) ? 0x98 : 0xD0); }
 es_ewr(base, 0xB1, 0x50); es_ewr(base, 0xB2, 0x10);
 if(channels < 2){                                  // mono keeps the fleet-proven
  outportb(base + 0x04, 0x0E);                      // filter-off write; stereo leaves
  outportb(base + 0x05, 0x20);                      // mixer 0x0E untouched (probe-exact)
 }
 es_dsp_cmd(base, 0xD1);
 es_ewr(base, 0xB8, 0x05);
 // SBPro-quirk polarity (the famous reversed-stereo SBPro trait; ES1868 DS
 // compat mode: first byte -> RIGHT): games pre-compensate, so byte 0 of the
 // guest stream must come out the RIGHT channel. The odd-prime trick was a
 // NO-OP under B9 demand-2 (fill/unload is pair-quantized) -- the flip is done
 // by swapping bytes per frame in es_fifo_pump (swap2). Prime stays even.
 for(i=0;i<256;i++) outportb(base+ES_FIFO, (bits>=16)?0x00:0x80);  // prime AFTER run
 es_pt_rate = rate; es_pt_bits = bits; es_pt_channels = channels;
 es_hw_rate = rate; es_hw_bits = bits; es_hw_channels = channels;  // chip armed with this format
 es_last_drain = _farpeekl(_dos_ds, 0x46C);                        // fresh arm = draining
 _farpokeb(_dos_ds, 0x4FA, ++es_tel_recfg);                    // telemetry (FULL reconfigs only)
 if(es_adaptive){                             // FEED-FORWARD: size the pump to this stream, re-arm.
  unsigned fifob = brate * ((bits >= 16) ? 2U : 1U);  // true FIFO drain bytes/sec (16-bit doubles it)
  es_rs_want = es_rs_for_brate(fifob);        // feedback ratchets faster from here if the game starves it
  es_rtc_rs = es_rs_want;
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
 unsigned unit = 1; int pace = 0, swap2 = 0;
 if(es_pump_busy) return;            // RTC tick vs inline PT_Feed (or a nested light-path tick):
 es_pump_busy = 1;                   // the outer call owns the ring_rd walk -- don't corrupt it
 if(es_flush_gen != es_flush_ack){   // deferred ring flush (requested from trap context):
  es_flush_ack = es_flush_gen;       // rd := wr from the CONSUMER side is race-free -- at worst a
  ring_rd = ring_wr;                 // few concurrently-produced bytes survive the flush
 }
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
  pace = (es_pt_channels >= 2);  // B9 demand-2 wants a settle after each frame
                                 // group, or it hashes the 2nd byte (right ch)
  swap2 = (es_pt_channels >= 2 && es_pt_bits < 16);  // SBPro-quirk R-first
 }
 while((inportb(base+ES_STAT) & FIFO_HE) && guard < 512){
  unsigned rd = ring_rd, wr = ring_wr; int k;
  es_last_drain = _farpeekl(_dos_ds, 0x46C);   // FIFO_HE set = the DAC is consuming (fast-resume liveness)
  for(k=0;k<128;k+=(int)unit){
   unsigned u;
   if((((wr - rd) & RING_MASK)) >= unit){
    if(swap2){                                   // frame = [L,R] from the guest;
     outportb(base+ES_FIFO, ring_buf[(rd+1)&RING_MASK]);  // emit R first
     outportb(base+ES_FIFO, ring_buf[rd]);
     rd = (rd+2)&RING_MASK;
    }else
    for(u=0;u<unit;u++){ outportb(base+ES_FIFO, ring_buf[rd]); rd = (rd+1)&RING_MASK; }
   }else{
    for(u=0;u<unit;u++) outportb(base+ES_FIFO, sil);
   }
   if(pace){ inportb(0x80); inportb(0x80); }  // ~1us pair settle (STEREO88 v1.7)
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
static int ES1688_PT_Space(void)
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

// feed raw guest PCM (called by SBEMU core, reconfigures the chip on format change)
// REENTRANCY: VSBHDA's SNDISR runs with interrupts ENABLED (SETIF, sndisr.c:325),
// so while our tap is mid-flight the next RTC tick can re-enter SNDISR -> our tap.
// es_pt_reconfig is LONG (DSP resets + 256-byte FIFO prime + port-I/O spins); a
// re-entry there corrupted the ISR stack and #GP'd into crt0 (hardware-test 2).
// Fix: a busy guard on PT_Feed + interrupts OFF around the one long reconfig.
static volatile int es_pt_feed_busy;
static unsigned char es_reentry;   // DIAG: reentrant PT_Feed calls skipped (0x4F3)
static void ES1688_PT_Feed(const unsigned char *buf, int bytes, unsigned rate, unsigned bits, unsigned channels)
{
 unsigned wr;
 _farpokeb(_dos_ds, 0x4F2, 1);                                 // DIAG stage: entry
 if(es_pt_feed_busy){ _farpokeb(_dos_ds, 0x4F3, ++es_reentry); return; }   // re-entered -> skip
 es_pt_feed_busy = 1;
 ++es_tel_feed16;                                              // telemetry (16-bit, lo/hi)
 _farpokeb(_dos_ds, 0x4FB, (unsigned char)es_tel_feed16);
 _farpokeb(_dos_ds, 0x4FF, (unsigned char)(es_tel_feed16 >> 8));
 es_last_feed = _farpeekl(_dos_ds, 0x46C);                     // audio flowing -> keep the pump fast
 // Waking from the idle throttle: between same-format sounds no reconfig runs,
 // so restore the stream's pump rate HERE, on the first feed. Rate-UP only
 // (never slow down mid-play), one compare per feed.
 if(es_adaptive && es_rtc_rs > es_rs_want) es_rtc_setrate(es_rs_want);
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
 es_tel_bytes += (unsigned long)bytes;         // PT byte-rate telemetry (no-TSC boxes: 0x4FC/D = bytes>>4, 16-bit)
 if(!SNDISR_HasTsc){
  unsigned u16 = (unsigned)((es_tel_bytes >> 4) & 0xFFFF);
  _farpokeb(_dos_ds, 0x4FC, (unsigned char)u16);
  _farpokeb(_dos_ds, 0x4FD, (unsigned char)(u16 >> 8));
 }
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
 // NOT es_hw_*: this baseline config isn't byte-identical to a PT reconfig
 // (mixer 0x0E stereo/filter bits differ), so never fast-resume onto it --
 // the first PT stream after any bring-up does one full reconfig.
 es_hw_rate = es_hw_bits = es_hw_channels = 0;
 es_last_drain = _farpeekl(_dos_ds, 0x46C);
}
static void es_fifo_stop(uint16_t base)
{
 es_ewr(base, 0xB8, 0x00);              // clear run
 es_dsp_reset(base);
 es_dsp_cmd(base, 0xD3);                // speaker off
 es_hw_rate = es_hw_bits = es_hw_channels = 0;   // chip disarmed: no fast-resume onto a stopped DAC
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

// Engine passthrough ops (ptops.h): registered from adetect when this card
// wins the session. PTF_TAP arms the sndisr tap; PTF_REAL_FM = the ES1688's
// real ESFM answers 0x388, so no FM detection shim is needed.
static const struct pt_ops_s es1688_pt_ops = {
 PTF_TAP | PTF_REAL_FM,
 ES1688_PT_Space, ES1688_PT_Feed, ES1688_PT_Watchdog,
 SNDISR_dbg_tick, SNDISR_dbg_exit, SNDISR_dbg_reenter,
 NULL,                                  // no nesting instrument -> no depth limiter
};

static int ES1688_adetect(struct audioout_info_s *aui)
{
 es1688_card_s *card;
 uint16_t base = ES_DEF_BASE;
 const char *e;
 if(!PTOPS_CardWanted("es1688")) return 0;
 // SBEBASE keeps its historical meaning HERE and only here: the emulated SB's
 // DSP base on an ES1688 card (the deployed GO.BATs set it). The other
 // backends read their own variables -- one binary, three different meanings
 // of "base", so they must not share a name.
 e = getenv("SBEBASE");
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
 PTOPS_Register(&es1688_pt_ops);        // arm the SNDISR passthrough tap (raw guest DMA -> chip)
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
 if(getenv("ESNOI8")) return;   // diagnostic kill-switch: run without the IRQ0 heartbeat (v0.3 behavior)
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
 // OPT-IN only (SET ESIRQ5=1): every stream on this stack has run fine with
 // the card's TC IRQ unserviced, and DPMI_InstallISR is REAL now -- silently
 // unmasking IRQ5 with a non-chaining handler is a behavior change we don't
 // take without evidence.
 if(!getenv("ESIRQ5")) return;
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

 if(es_adaptive){ es_rs_want = es_rs_for_brate(es_dacrate); es_rtc_rs = es_rs_want; }  // sane baseline for the initial/non-PT case; PT streams override via feed-forward
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
 es_pt_active = 0;            // next passthrough feed does a full chip re-arm
 es_flush_gen++;              // DRAIN the ring (pump-executed; see ring ownership
                              // note): with the DAC halted a full ring would
                              // report no space, so the tap never feeds, so the
                              // re-arm (inside PT_Feed) never runs -- a permanent
                              // silence deadlock (pinball reruns, Wolf3D one-shot
                              // SFX, SBDIAG ordering).
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
 { uint8_t f = DPMI_DisableInterrupt();                     // ack RTC. cli: with the IRQ0 heartbeat live,
   outportb(0x70,0x0C); (void)inportb(0x71);                // a tick landing between index and data would
   DPMI_RestoreInterrupt(f); }                              // leave the CMOS index clobber-prone

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
 _farpokeb(_dos_ds, 0x4F5, (unsigned char)(((ring_wr - ring_rd) & RING_MASK) >> 5));  // DIAG: ring fill, 32-byte units
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
