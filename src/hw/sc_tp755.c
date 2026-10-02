//**************************************************************************
//*  sc_tp755.c - VSBPCMCIA output driver for the ThinkPad 755C internal
//*  audio card (IBM FRU 84G4289): Crystal CS4248 (AD1848/WSS codec) on the
//*  planar ISA bus. Also fits the 750 family / 360PE per ThinkWiki.
//*
//*  (C) copyright 2026 zikolas
//*  Built on VSBHDA's au_cards interface, (C) PDSoft (Attila Padar).
//*
//*  This is free software: you may redistribute it and/or modify it under
//*  the terms of the GNU General Public License version 2 as published by
//*  the Free Software Foundation. Distributed WITHOUT ANY WARRANTY. See the
//*  COPYING file at the root of this project.
//**************************************************************************
//  UNLIKE every PCMCIA backend in this tree, this card has REAL ISA DMA:
//  codec register block at I/O 0x4E30 (Index/Data/Status/PIO), IRQ 10, and
//  8237 channel 0 -- all planar-wired, all hardware-verified on the bench
//  2026-08-14 (TP755PRB probe: IRQ10 fires per codec count expiry, 8237
//  pointer advance measured 11027 Hz against a programmed 11025 = crystal-
//  exact; PIO fallback also works and PRDY gates honestly).
//
//  So this driver is the SC_ICH model, not the sc_es1688/sc_vew211 pump
//  model: a hardware-paced 8237 ch0 autoinit ring in DOS conventional
//  memory, refilled by SNDISR on the codec's per-period IRQ10. No RTC, no
//  tick credit, no PRDY workarounds. Two ways to fill the ring:
//   * TAP (the default since 2026-10-02): the sndisr passthrough tap hands
//     the guest's raw PCM to TP_PT_Feed, which steps it onto the codec's
//     fixed rate (11025 Hz unless /DACRATE) with sc_ibmaud.c's stepper and
//     writes it into the ring ahead of the 8237 play position. The 8237
//     position is also the pt_ops clock, so direct DAC is fed at a measured
//     rate (SNDISR_DacFeed). DOOM2's SFX were quiet and choppy through the
//     render path on this codec and clean through the IBM card's tap on the
//     same 755.
//   * RENDER: the engine renders SB PCM + emulated OPL3 (CARD_TP755 unmasks
//     NOFM -- there is no FM chip anywhere on the 755C) into the ring via
//     the standard AU_cardbuf_space/AU_writedata path. Taken when the
//     software OPL3 is live (a tap never runs the mixer) or under /RESAMP.
//
//  Enable: the card powers up dark; ThinkPad system control port 0x15E8
//  (index) / 0x15E9 (data), index 0x1C, bit 0x02 = codec enable (from the
//  Linux wss_lib.c ThinkPad twiddle; bench-verified: disabled card reads
//  0x80 on all four codec ports -- the AD1848 "busy" pattern, NOT float FF).
//
//  Codec gotchas (bench-measured, 2026-08-14):
//   * I6/I7 (DAC attenuators) power up MUTED (0x80) -- unmute is mandatory.
//   * A format write under MCE reads back 0x80 while the chip re-syncs its
//     clocks -- verify AFTER the MCE drop + autocal, never immediately.
//   * Every MCE drop runs a real ~100ms-class autocalibration; wait for
//     I11.ACI to clear (the sc_vew211 ms-paced verified-MCE recipe, minus
//     its PIO bits).
//
//  Hazards owned here (see doc in the tp755 project notes):
//   * Our own 8237 pokes MUST use UntrappedIO_OUT/IN: ports 0x08-0x0F are
//     always PM-trapped and a plain OUT from the TSR would bounce through
//     VDMA_Write and corrupt the guest-visible shadow state.
//   * Guest /D0 would collide with our real channel 0 -> adetect refuses.
//   * A guest master-resetting the 8237 (OUT 0x0D/0x0F) passes through and
//     masks real ch0; the DSP-reset hook (tp_watchdog, via the ptops
//     table) re-unmasks ch0 as a cheap idempotent heal.
//**************************************************************************

#ifndef NOTP755

#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include "hostsvc.h"    /* toolchain compat: LOW_*, inportb                */
#include "hostisr.h"    /* chained/iret PM interrupt vectors, both builds  */

#include "config.h"
#include "au_cards.h"
#include "ptops.h"      /* engine passthrough ops table (we register in adetect) */
#include "dmabuff.h"
#include "platform.h"   /* bool etc. -- must precede ptrap.h */
#include "ptrap.h"
#include "dma.h"

/* NOT linear.h: it pulls the fork's DJDPMI.H which conflicts with the
 * system <dpmi.h> hostisr.h includes for the go32 chain API. DSBase is all
 * that was wanted from it. NearPtr works in BOTH builds: init1632.asm gives
 * the 16-bit build's DGROUP a full 4GB limit, so a near pointer wraps round
 * to any linear address there just as it does under DJGPP. */
extern uint32_t DSBase;
#define TP_NEARPTR(a) ((void *)((uint32_t)(a) - DSBase))

/* DPMI 0100h (allocate DOS conventional memory) is HOST_DosAlloc in
 * src/hostsvc.c now -- same body under DJGPP, a #pragma aux under OW. */
#define tp_dos_alloc  HOST_DosAlloc

//------------------------------------------------------------- geometry ---
#define TP_CTL_IDX   0x15E8         // ThinkPad system control: index port
#define TP_CTL_DATA  0x15E9         //   data port
#define TP_CTL_AUDIO 0x1C           //   index of the audio-enable byte
#define TP_CTL_BIT   0x02           //   bit 1 = CS4248 enabled

#define TP_BASE_DEF  0x4E30         // codec block (/BASE overrides)
#define TC_IAR  0                   // Index Address Register (bit6 = MCE)
#define TC_IDR  1                   // Indexed Data Register
#define TC_SR   2                   // Status Register (bit0 = INT, write clears)
#define TC_PDR  3                   // PIO data (unused -- DMA build)

#define IAR_MCE   0x40
#define I8_16BIT  0x40
#define I8_STEREO 0x10
#define I9_PEN    0x01
#define I9_ACAL   0x08
#define I10_IEN   0x02
#define I11_ACI   0x20
#define SR_INT    0x01

// 8237 controller 1, channel 0 (dma.h has the register names)
#define TP_DMA_PAGE0 0x87
#define TP_DMA_MODE_CH0 (0x00 /*ch0*/ | DMA_REG_MODE_OP_READ | DMA_REG_MODE_AUTO | 0x40 /*single*/)
// = 0x58: single mode, autoinit, read (mem->device), channel 0

// real-mode home for ptrap.c: 256 stub + 1024-entry OPL ring (2048) + 32
// spare = 2336 bytes. Was 46 paragraphs when the ring held 256 entries; the
// bench MEASURED that ring full (occupancy high-water pinned 255/255 through
// MI1 crescendos), which drops those writes back onto the synchronous trap
// path. Keep in step with OPLRING_ENTRIES in ptrap.c/rmcode1.asm.
#define TP_RMHOME_PARA 162u        // = PTRAP_RMHOME_PARA in ptrap.c

#define TP_RING_BYTES 8192u         // must be a power of two and a multiple
#define TP_PERIOD_DEF 512u          //   of the period; 8K @ 22050 st16 = ~81ms queue
#define TP_RATE_DEF   22050u        // render path
#define TP_RATE_TAP   11025u        // tap: the stepper folds every guest rate onto it
#define TP_RATE_MIN   5510u
#define TP_RATE_MAX   48000u
#define TP_RF   (TP_RING_BYTES / 4u)  // ring frames (16-bit stereo)
#define TP_QMAX (TP_RF - TP_RF / 8u)  // tap: frames queued, hard cap (play never
                                      //   catches the writer from behind)

//---------------------------------------------------------------- state ---
struct tp755_card_s { uint16_t base; };

static uint16_t tp_cb        = TP_BASE_DEF;  // codec base
static char    *tp_ring      = NULL;         // near ptr into the DOS block
static uint32_t tp_ring_phys = 0;            // physical addr of ring start
static int      tp_dos_sel   = 0;            // selector for __dpmi_free_dos_memory
static unsigned tp_period    = TP_PERIOD_DEF;
static unsigned tp_dacrate   = TP_RATE_DEF;  // requested (/DACRATE)
static int      tp_vol       = 8;            // I6/I7 attenuation 0..63 x -1.5dB
                                             // (/CVOL; default -12dB --
                                             // 0dB is LOUD on the lid speaker)
static unsigned tp_hw_rate   = 0;            // configured-format record: enables
static unsigned tp_hw_armed  = 0;            //   cheap restart + the watchdog heal
static uint8_t  tp_ctl_was_on = 0;           // enable state found at detect

// Tap state (TP_PT_*). OWNERSHIP: the ring write side and the position
// tracker are touched only with tp_busy held -- SETIF lets a second SNDISR
// nest, and its irq_routine pad must not interleave with an outer feed
// (sc_ibmaud.c's ONE WRITER rule).
static int      tp_tap       = 0;            // PTF_TAP registered (adetect)
static volatile int tp_busy;
static uint32_t tp_wr, tp_cons;              // frames written / played since start
static unsigned tp_lastp;                    // last ring frame position tracked
static uint8_t  tp_bad;                      // consecutive refused position steps
static unsigned tp_lat_ms    = 80;           // SBEPTLAT: queued-audio target
static unsigned tp_qtarget, tp_qlow;         // frames: latency target, pad floor
static uint32_t tp_pt_cons;                  // tp_cons at the last tap feed
static uint8_t  tp_tel_gap, tp_tel_under;    // 0x4FE / 0x4FF (tap builds)
// pt_ops clock, 1/32768 s: the 8237's own consumption, which keeps counting
// through lost or late IRQ10s
static volatile unsigned long tp_clock;
static unsigned long tp_clock_frac;          // remainder, 1/(32768 * rate) s
// frame stepper (sc_ibmaud.c's: Bresenham + linear interpolation, averaging
// only above 3:2)
static int           tp_pt_active;
static unsigned      tp_pt_rate, tp_pt_bits, tp_pt_channels;
static unsigned long tp_step_acc;            // Bresenham phase, 0..rate-1
static unsigned long tp_step_inv;            // (1 << 24) / rate
static int           tp_pl, tp_pr, tp_pvok;  // previous guest frame, 16-bit L/R
static long          tp_dsl, tp_dsr;         // decimator sums ...
static unsigned      tp_dn;                  // ... over this many guest frames
#define TP_DECIMATE(f) ((f) + ((f) >> 1))

// TELEMETRY (VEW211 pattern): breadcrumbs into the BIOS IAC area 0x4F0-4FF,
// readable over COMrade mid-wedge (agent alive) or after a WARM reboot
// (IAC survives Ctrl-Alt-Del on IBM BIOSes). Layout:
//   4F0 = ISR nesting depth        4F1 = last phase (see TP_PH_*)
//   4F2/3 = ISR tick count u16     4F4 = reenter (guard-skip) count
//   4F5 = last SR seen at claim    4F6 = heal count (start+watchdog)
//   4F8/9 = last getpos u16        4FA = consecutive futile heals
// 4F2/3, 4F6 and 4FA are LOANED to RATEDIAG (src/ptops.h) and are not
// published while it is 1 -- build with RATEDIAG 0 to diagnose this card.
//   4FB = depth high-water         4FC = IRQ0 polls per tick (last)
//   4FD = polls-per-tick high-water (the IRQ0-saturation meter)
//   tap only: 4FE = silence padded into a live stream (a gap the guest left)
//             4FF = play position overtook the writer (stale ring replayed),
//                   counted once the step has stood 2 looks
#define TP_PH_CLAIM  1   /* irq_routine claimed the interrupt      */
#define TP_PH_GETPOS 2   /* space computed (render about to start) */
#define TP_PH_WRITE  3   /* writedata reached (render finished)    */
#define TP_PH_EXIT   4   /* ISR completed                          */
static uint8_t *tp_iac = NULL;   /* NearPtr(0x4F0), set in adetect */
#define TP_IAC(off, v) do{ if(tp_iac) tp_iac[off] = (uint8_t)(v); }while(0)

/* THE GUARDIAN'S CLOCK MUST NOT BE A BORROWABLE TELEMETRY BYTE. It used to
 * read tp_iac[2] (= 0x4F2) as its freeze detector -- the same byte RATEDIAG
 * lends to the rate readout. With anything else writing there the guardian
 * sees ticks while the clock is dead, or a frozen count while it is alive,
 * and can suppress a real heal or fire a spurious one -- on a card whose
 * ONLY clock is this IRQ. It owns a private counter now, in every build.
 * 0x4F2/0x4F3 (tick count) and 0x4F6 (heal count) are still published for
 * COMrade, just not when RATEDIAG owns those slots. */
static volatile uint8_t tp_tick8;   /* guardian's own tick clock, never loaned */
static uint8_t tp_tel_heal;         /* heal count; published unless on loan */
#if RATEDIAG
#define TP_IAC_R(off, v)  ((void)0)      /* 0x4F2/0x4F3/0x4F6/0x4FA on loan */
#else
#define TP_IAC_R(off, v)  TP_IAC(off, v)
#endif
#define TP_HEAL()  do{ tp_tel_heal++; TP_IAC_R(6, tp_tel_heal); }while(0)

//-------------------------------------------------------------- helpers ---
/* This backend masks with a RAW cli/sti, not the DPMI host's 0900h/0901h
 * that sc_es1688 and sc_vew211 use. Which is right is a bench question, so
 * hostsvc.h carries both and each backend keeps the one it was proven with. */
#define DPMI_DisableInterrupt  HOST_CliSave
#define DPMI_RestoreInterrupt  HOST_StiRestore

static void tp_iodelay(unsigned n){ while(n--) (void)inportb(0x80); }
#define TP_MS(x) tp_iodelay((unsigned)(x) * 1000U)

// codec index/data pair -- ports are untrapped, plain I/O is fine
static void tp_ci_wait(void)
{
 unsigned long i;
 for(i = 0; i < 400000UL; i++)
  if(!(inportb(tp_cb + TC_IAR) & 0x80)) return;
}
static void tp_ci_put(unsigned char idx, unsigned char v)
{
 tp_ci_wait();
 outportb(tp_cb + TC_IAR, idx); tp_iodelay(200);
 outportb(tp_cb + TC_IDR, v);   tp_iodelay(200);
}
static unsigned char tp_ci_get(unsigned char idx)
{
 tp_ci_wait();
 outportb(tp_cb + TC_IAR, idx); tp_iodelay(200);
 return (unsigned char)inportb(tp_cb + TC_IDR);
}

// ThinkPad enable twiddle; returns the previous ctl byte
static uint8_t tp_twiddle(int on)
{
 uint8_t v;
 outportb(TP_CTL_IDX, TP_CTL_AUDIO);
 v = (uint8_t)inportb(TP_CTL_DATA);
 outportb(TP_CTL_IDX, TP_CTL_AUDIO);
 outportb(TP_CTL_DATA, on ? (v | TP_CTL_BIT) : (v & ~TP_CTL_BIT));
 return v;
}

//------------------------------------------------------------ rate table ---
// The standard WSS 14-rate set (I8 low nibble; bit0 = crystal select).
// Identical on AD1848/CS4248/CS4231A; both crystals are fitted on the
// 84G4289 (Y1+Y2 on the PCB). No pump, no ceiling: the full table.
static const struct { unsigned long hz; unsigned char code; } tp_rates[] = {
 { 5510UL,0x01},{ 6620UL,0x0F},{ 8000UL,0x00},{ 9600UL,0x0E},
 {11025UL,0x03},{16000UL,0x02},{18900UL,0x05},{22050UL,0x07},
 {27042UL,0x04},{32000UL,0x06},{33075UL,0x0D},{37800UL,0x09},
 {44100UL,0x0B},{48000UL,0x0C}
};
#define TP_NRATES (int)(sizeof(tp_rates)/sizeof(tp_rates[0]))

static int tp_rate_pick(unsigned rate)
{
 int i, best = 7; unsigned long bd = 0xFFFFFFFFUL;
 for(i = 0; i < TP_NRATES; i++){
  unsigned long d = tp_rates[i].hz > rate ? tp_rates[i].hz - rate
                                          : rate - tp_rates[i].hz;
  if(d < bd){ bd = d; best = i; }
 }
 return best;
}

//------------------------------------------------------- codec bring-up ---
// ms-paced verified-MCE format program (the sc_vew211 recipe minus PPIO):
// raw IAR writes with MCE held, 2ms between bytes, MCE drop, INIT wait,
// I11.ACI autocal wait, read-back verify, retry x8. Returns the actual Hz.
static unsigned tp_codec_config(unsigned rate)
{
 int ri = tp_rate_pick(rate), tries;
 unsigned char i8 = (unsigned char)(tp_rates[ri].code | I8_STEREO | I8_16BIT);
 unsigned frames = tp_period / 4;             // 16-bit stereo: 4 bytes/frame

 for(tries = 0; tries < 8; tries++){
  tp_ci_wait();
  outportb(tp_cb + TC_IAR, (unsigned char)(IAR_MCE|0x08)); TP_MS(2);
  outportb(tp_cb + TC_IDR, i8);                            TP_MS(2);
  outportb(tp_cb + TC_IAR, (unsigned char)(IAR_MCE|0x09)); TP_MS(2);
  outportb(tp_cb + TC_IDR, I9_ACAL);                       TP_MS(2);
  outportb(tp_cb + TC_IAR, 0x00);                          TP_MS(2);
  tp_ci_wait(); TP_MS(10);
  { unsigned long w;
    for(w = 0; w < 400000UL; w++) if(!(tp_ci_get(0x0B) & I11_ACI)) break; }
  if(tp_ci_get(0x08) == i8 && (tp_ci_get(0x09) & I9_ACAL) == I9_ACAL)
   break;
 }

 tp_ci_put(0x06, (unsigned char)(tp_vol & 0x3F));   // DAC unmute + level
 tp_ci_put(0x07, (unsigned char)(tp_vol & 0x3F));   // (bench: default is MUTED)

 // playback base count = frames per IRQ, minus one; reloads each expiry,
 // so this is the interrupt cadence -- the 8237 rolls the ring on its own
 tp_ci_put(0x0F, (unsigned char)((frames - 1) & 0xFF));
 tp_ci_put(0x0E, (unsigned char)((frames - 1) >> 8));

 tp_hw_rate = (unsigned)tp_rates[ri].hz;
 return tp_hw_rate;
}

// PEN on/off without MCE (legal per datasheet), verified with retry
static volatile uint8_t tp_pen_on = 0;   // guardian gate: heal only while playing
static void tp_pen(int on)
{
 int tries;
 unsigned char want = on ? (unsigned char)(I9_ACAL|I9_PEN)
                         : (unsigned char)I9_ACAL;
 for(tries = 0; tries < 8; tries++){
  tp_ci_put(0x09, want); TP_MS(2);
  if((tp_ci_get(0x09) & I9_PEN) == (on ? I9_PEN : 0)) break;
 }
 tp_pen_on = (uint8_t)(on ? 1 : 0);
}

//-------------------------------------------------- the IRQ0 guardian ---
// Bench 2026-08-14, MI1 live post-mortem: the engine clock died with the
// slave PIC ISR *and* IRR both empty and the codec INT line begging -- the
// REAL slave IMR had our bit masked by guest PIC traffic that slips the
// byte-port VPIC filter (16-bit OUTs to 0xA0 hit both ports in one cycle).
// Forcing the mask clear + re-arming the edge resurrected audio mid-game.
// The codec IRQ is this backend's only clock, so ANY clock death (mask,
// lost edge, stray EOI state, masked DMA ch0) must self-heal without it:
// a tiny chained IRQ0 hook (the sc_vew211 i8-heartbeat pattern -- keep it
// TRIVIAL, it borrows the guest's ISR stack) watches our own tick counter
// and unsticks everything when it freezes while PEN is on.
static DPMI_ISR_HANDLE tp_i8_handle;
static uint8_t tp_i8_hooked = 0;
static uint8_t tp_g_last = 0;
static uint16_t tp_g_frozen = 0, tp_g_begging = 0;
static uint8_t tp_g_futile = 0;      // consecutive heals with no tick advance
static uint8_t tp_g_healtick = 0;    // tick byte at last heal
static uint16_t tp_g_gap = 0;        // IRQ0 polls since last tick advance
static uint8_t tp_g_t2 = 0;          // tier-2 (slave re-ICW) heals fired
static uint16_t tp_g_norm8 = 0;      // polls per engine tick x 8, smoothed 1/8
#define tp_g_norm (tp_g_norm8 >> 3)
// Healthy-baseline IMRs, stashed at guardian install (= stack-up, before any
// guest runs): what tier 2 restores after re-initializing the slave PIC. A
// mid-init PIC returns garbage on reads, so the restore value must come from
// a moment the PIC was known-good -- NOT from the wedge itself.
static uint8_t tp_imr_a1_boot = 0;
static uint8_t tp_imr_21_boot = 0;

// VERIFY-BEFORE-REVIVE (SNDISR_ReviveSquelch, engine-owned in sndisr.c):
// after a heal, suppress guest VIRQ injection for the first resumed ticks --
// resurrection into seconds-stale guest SB state crashed MI1 where the
// un-healed freeze didn't. Only the ASYNC guardian sets it; the DSP-reset
// watchdog heal is guest-initiated (fresh state incoming) and must inject
// promptly.

// sndisr.c depth limiter reads the live nesting depth through the ops table
static int TP755_Depth(void){ return tp_iac ? (int)tp_iac[0] : 0; }

static void tp_guardian(void)
{
 if(!tp_pen_on || !tp_iac) return;
 // SATURATION METER: IRQ0 polls between engine ticks. Healthy: gap stays
 // tiny at 18.2Hz PIT (several ticks per poll) and modest at fast PITs.
 // A guest music handler whose OPL trap tax exceeds its own PIT period
 // starves IRQ10 (priority: IRQ0 > cascade) -- the gap tells that story
 // in one number. 4FC = last completed gap (u8 sat), 4FD = high water.
 if(tp_g_gap < 0xFFFF) tp_g_gap++;
 if(tp_tick8 != tp_g_last){
  tp_g_last = tp_tick8; tp_g_frozen = 0; tp_g_begging = 0;
  tp_g_futile = 0;                            // real progress ends dormancy
  tp_g_norm8 = (uint16_t)(tp_g_norm8 - (tp_g_norm8 >> 3)
                          + (tp_g_gap > 4095 ? 4095 : tp_g_gap));
  tp_iac[0x0C] = (uint8_t)(tp_g_gap > 255 ? 255 : tp_g_gap);
  if(tp_iac[0x0C] > tp_iac[0x0D]) tp_iac[0x0D] = tp_iac[0x0C];
  tp_g_gap = 0;
  return;
 }
 if(tp_g_frozen < 0xFFFF) tp_g_frozen++;
 if(tp_g_frozen < 8) return;
 // FUTILITY BUDGET (MI1 heal-storm lesson, 2026-08-14 pt.2): 250+ heals
 // in seconds while the true blocker (IRQ0 monopoly / stack exhaustion)
 // was untouchable by EOIs. A heal that didn't move the tick counter by
 // the time we re-qualify was futile; after 8 consecutive futile heals
 // go DORMANT (stop poking hardware, stop re-arming the squelch) until
 // the engine advances on its own. The un-healed freeze was survivable;
 // the storm never is.
 if(tp_g_futile >= 8) return;
 {
  uint8_t v = (uint8_t)inportb(tp_cb + TC_SR);
  if(v & SR_INT){
   // PIT-RATE-PROOF QUALIFICATION (second MI1 lesson): this hook runs at
   // the GUEST's PIT rate -- under a reprogrammed PIT (SCUMM ~270Hz, some
   // engines 1kHz) 8 polls can be SHORTER than one codec period, and a
   // false heal at a healthy engine EOIs a level that IS in service =
   // PIC state corruption. A live engine clears SR within microseconds
   // and advances the tick counter; SR begging on 3 CONSECUTIVE polls
   // with the counter frozen is impossible unless delivery is truly
   // dead, at any PIT rate.
   // POLL-RATE SCALED (JLMTEST W on the 755, 2026-10-02): a guest PIT of
   // several kHz makes three polls a fraction of a millisecond, and IRQ10
   // can wait that long behind a saturated IRQ0 -- heals on a healthy
   // engine, each one a PEN bounce and a gap. So the begging must also
   // outlast two codec periods' worth of polls, from tp_g_norm (no port
   // I/O here: every 8237 read is an HDPMI call, and adding them to this
   // hook made it worse).
   if(tp_g_begging < 0xFFFF) tp_g_begging++;
   if(tp_g_begging < 3 || tp_g_begging < 2u * tp_g_norm) return;
   if(tp_tick8 == tp_g_healtick) { if(tp_g_futile < 255) tp_g_futile++; }
   else tp_g_futile = 0;
   tp_g_healtick = tp_tick8;
   TP_IAC_R(0x0A, (tp_g_t2 << 4) | (tp_g_futile > 15 ? 15 : tp_g_futile));
   // codec begging, nobody serviced: clear any mask on our line (REAL
   // IMRs -- UntrappedIO from PM context reads real hardware, unlike a
   // V86 read), fire safe specific EOIs, re-arm the edge
   v = UntrappedIO_IN(0xA1);
   if(v & 0x04) UntrappedIO_OUT(0xA1, (uint8_t)(v & ~0x04));
   v = UntrappedIO_IN(0x21);
   if(v & 0x04) UntrappedIO_OUT(0x21, (uint8_t)(v & ~0x04));
   UntrappedIO_OUT(0xA0, 0x62);              // specific EOI slave lvl 2
   UntrappedIO_OUT(0x20, 0x62);              // specific EOI master lvl 2 (cascade)
   SNDISR_ReviveSquelch = 2;                 // gate injection on resume
   // VERIFIED ACK (MI2 wedge autopsy, 2026-08-14 night, live-probed over
   // COMrade): with PEN ON the CS4248 IGNORES status-register writes -- SR
   // stayed 0x89 through repeated OUTs, then cleared on the FIRST write
   // once PEN was dropped. So the old unconditional ack was a no-op in
   // exactly the state that needs it, and all 8 futile heals were spent
   // re-doing it. Ack, verify, and only if INT is still latched bounce
   // PEN around the ack (audio is already dead; a bounce costs nothing).
   // tp_pen touches the codec INDEX register. A depth==0 gate here (first
   // attempt) DISABLED the bounce in the real MI2 wedge: the clock died
   // with two SNDISR frames parked on the stack (depth stuck at 2), so the
   // gate held while futile climbed to dormancy and the wedge matured into
   // a game crash. A frozen mid-frame SNDISR only ever resumes if we heal;
   // the worst the bounce can do to it is leave IAR at 0x09 so one resumed
   // access hits the wrong register once -- strictly better than never
   // resuming. Bounce whenever the ack doesn't take.
   outportb(tp_cb + TC_SR, 0);
   if(inportb(tp_cb + TC_SR) & SR_INT){
    tp_pen(0);
    outportb(tp_cb + TC_SR, 0);
    tp_pen(1);
   }
   // TIER 2 -- same autopsy, the deeper failure: after a by-hand PEN
   // bounce the codec re-asserted a FRESH edge and the slave IRR still
   // read 0x00 -- the 8259 wasn't registering edges at all (the ISR=00 +
   // IRR=00 + codec-begging signature; prime suspect a word OUT landing
   // ICW1 on the real slave, latching it mid-init-sequence, the sibling
   // of the fixed IMR word-clobber). No ack can cure that, so after 4
   // futile rounds re-run the slave's init sequence (standard AT wiring:
   // vector base 70h -- box-verified, INT 72h in use -- cascade ID 2,
   // 8086 mode) and restore the boot-time IMR. Master is left alone.
   if(tp_g_futile >= 4){
    UntrappedIO_OUT(0xA0, 0x11);             // ICW1: edge, cascade, ICW4
    UntrappedIO_OUT(0xA1, 0x70);             // ICW2: vector base 70h
    UntrappedIO_OUT(0xA1, 0x02);             // ICW3: slave ID 2
    UntrappedIO_OUT(0xA1, 0x01);             // ICW4: 8086 mode
    UntrappedIO_OUT(0xA1, (uint8_t)(tp_imr_a1_boot & ~0x04));
    v = UntrappedIO_IN(0x21);                // cascade line must be open
    if(v & 0x04) UntrappedIO_OUT(0x21, (uint8_t)(v & ~0x04));
    if(tp_g_t2 < 15) tp_g_t2++;
   }
   TP_HEAL();
   TP_IAC_R(0x0A, (tp_g_t2 << 4) | (tp_g_futile > 15 ? 15 : tp_g_futile));
   tp_g_begging = 0; tp_g_frozen = 0;
  }else{
   tp_g_begging = 0;
   // ticks frozen and codec NOT asking: starved (masked/killed DMA ch0,
   // e.g. a guest 8237 master reset) -> re-unmask; codec resumes, count
   // expires, clock restarts. Benign if false, but be patient enough
   // that a high-rate PIT can't thrash it (32 polls, not 8, and three
   // periods' worth at a PIT of several kHz -- see POLL-RATE SCALED).
   if(tp_g_frozen < 32 || tp_g_frozen < 3u * tp_g_norm) return;
   UntrappedIO_OUT(DMA_REG_SINGLEMASK, 0x00);
   TP_HEAL();
   tp_g_frozen = 0;
  }
 }
}

static void tp_i8_install(void)
{
 if(tp_i8_hooked) return;
 tp_imr_a1_boot = (uint8_t)inportb(0xA1);   // healthy-baseline IMRs for the
 tp_imr_21_boot = (uint8_t)inportb(0x21);   // tier-2 restore (pre-guest)
 if(DPMI_InstallISR(0x08, &tp_guardian, &tp_i8_handle, 1) == 0)
  tp_i8_hooked = 1;
}

static void tp_i8_remove(void)
{
 if(!tp_i8_hooked) return;
 DPMI_UninstallISR(&tp_i8_handle);
 tp_i8_hooked = 0;
}

//------------------------------------------------------------- 8237 ch0 ---
// ALL 8237 access via UntrappedIO (0x08-0x0F are always PM-trapped; the
// ch0 addr/count/page are trapped too under guest /D0 -- uniform is safest).
static void tp_dma_arm(void)
{
 uint8_t f = DPMI_DisableInterrupt();
 UntrappedIO_OUT(DMA_REG_SINGLEMASK, 0x04);              // mask ch0
 UntrappedIO_OUT(DMA_REG_FLIPFLOP,   0x00);
 UntrappedIO_OUT(DMA_REG_MODE,       TP_DMA_MODE_CH0);   // 0x58
 UntrappedIO_OUT(DMA_REG_CH0_ADDR,   (uint8_t)(tp_ring_phys & 0xFF));
 UntrappedIO_OUT(DMA_REG_CH0_ADDR,   (uint8_t)((tp_ring_phys >> 8) & 0xFF));
 UntrappedIO_OUT(TP_DMA_PAGE0,       (uint8_t)((tp_ring_phys >> 16) & 0xFF));
 UntrappedIO_OUT(DMA_REG_CH0_COUNTER,(uint8_t)((TP_RING_BYTES - 1) & 0xFF));
 UntrappedIO_OUT(DMA_REG_CH0_COUNTER,(uint8_t)((TP_RING_BYTES - 1) >> 8));
 UntrappedIO_OUT(DMA_REG_SINGLEMASK, 0x00);              // unmask ch0
 DPMI_RestoreInterrupt(f);
 tp_hw_armed = 1;
}

// flipflop-safe current-count read; DMA keeps running under cli (cli only
// serializes our own two-byte pair against nested ISRs), so read twice and
// require a stable high byte -- a mid-pair transfer tears the low byte only.
static unsigned tp_dma_count(void)
{
 unsigned c1, c2; int tries = 3;
 uint8_t f = DPMI_DisableInterrupt();
 do{
  UntrappedIO_OUT(DMA_REG_FLIPFLOP, 0x00);
  c1  = UntrappedIO_IN(DMA_REG_CH0_COUNTER);
  c1 |= (unsigned)UntrappedIO_IN(DMA_REG_CH0_COUNTER) << 8;
  UntrappedIO_OUT(DMA_REG_FLIPFLOP, 0x00);
  c2  = UntrappedIO_IN(DMA_REG_CH0_COUNTER);
  c2 |= (unsigned)UntrappedIO_IN(DMA_REG_CH0_COUNTER) << 8;
 }while((c1 >> 8) != (c2 >> 8) && --tries);
 DPMI_RestoreInterrupt(f);
 return c2;
}

//------------------------------------------------------------------ tap ---
// Ring play position in frames for the tap. ONE count per IRQ10, from the
// pump: every 8237 access is an HDPMI untrapped-I/O call (a count is six),
// and reading it from space and feed as well -- the IBM card's habit, where a
// position read is one INW -- cost enough on the DX4/75 to delay IRQ10.
// tp_dma_count already insists on a stable high byte; a torn count that
// still gets through is caught by tp_track's step check.
static unsigned tp_pos_frames(void)
{
 return ((TP_RING_BYTES - 1 - tp_dma_count()) & (TP_RING_BYTES - 1)) >> 2;
}

#define TP_QUEUED() ((unsigned)(tp_wr - tp_cons))

// tp_busy held, pump only. Advance tp_cons and the clock from the 8237
// position; space, feed and writedata work from this snapshot. A step
// longer than the queue is either the play position overtaking the writer
// (the ring replays stale audio; the writer jumps ahead) or a bad read: it
// is refused for 2 looks first.
static void tp_track(void)
{
 unsigned p = tp_pos_frames(), d = (p - tp_lastp) & (TP_RF - 1);
 if(d > TP_QUEUED() + 16u){
  if(tp_bad < 2){ tp_bad++; return; }
  if(tp_iac) tp_iac[0x0F] = ++tp_tel_under;
 }
 tp_bad = 0;
 tp_cons += d;
 tp_lastp = p;
 tp_clock_frac += (unsigned long)d * 32768UL;
 tp_clock += tp_clock_frac / tp_hw_rate;
 tp_clock_frac %= tp_hw_rate;
 if((long)(tp_cons - tp_wr) > 0) tp_wr = tp_cons;
}

// tp_busy held, room checked by the caller: one 16-bit stereo frame
static void tp_put(int l, int r)
{
 int16_t *f = (int16_t *)tp_ring + (unsigned)(tp_wr & (TP_RF - 1)) * 2u;
 f[0] = (int16_t)l; f[1] = (int16_t)r;
 tp_wr++;
}

// IRQ10, tp_busy free: track, and pad silence below the floor so the 8237
// never reaches ring contents we did not write this lap.
static void tp_pump(void)
{
 unsigned q;
 if(tp_busy) return;                           // an outer pass is writing
 tp_busy = 1;
 tp_track();
 q = TP_QUEUED();
 if(q < tp_qlow){
  if(tp_pt_active && tp_cons - tp_pt_cons < (uint32_t)(tp_hw_rate / 8u) && tp_iac)
   tp_iac[0x0E] = ++tp_tel_gap;               // the guest left a gap
  while(q++ < tp_qlow) tp_put(0, 0);
 }
 tp_busy = 0;
}

static unsigned long TP755_Clock(void){ return tp_clock; }

// Guest bytes the ring accepts now: room up to the latency target, scaled
// back through the stepper to guest frames.
static int TP_PT_Space(void)
{
 unsigned q, unit;
 unsigned long g;
 if(tp_busy) return 0;
 q = TP_QUEUED();                              // as of this tick's pump
 if(q >= tp_qtarget) return 0;
 if(!tp_pt_rate) return 1024;
 unit = (tp_pt_channels >= 2 ? 2u : 1u) * (tp_pt_bits >= 16 ? 2u : 1u);
 g = (unsigned long)(tp_qtarget - q) * tp_pt_rate / tp_hw_rate * unit;
 if(g > 16384UL) g = 16384UL;                  // int is 16 bits in the NOTFLAT build
 return (int)g;
}

// Feed raw guest PCM (sndisr.c tap): step it onto the codec rate, 16-bit
// stereo, into the ring. The stepper is sc_ibmaud.c's IB_PT_Feed; see the
// notes there for why it averages only above 3:2 and keeps its phase on a
// rate-only change.
static void TP_PT_Feed(const unsigned char *buf, int bytes, unsigned rate, unsigned bits, unsigned channels)
{
 unsigned unit, q, room;
 if(tp_busy){ if(tp_iac) tp_iac[4]++; return; }   // counted as a reenter
 tp_busy = 1;
 if(!rate) rate = tp_hw_rate;
 if(tp_pt_active && rate != tp_pt_rate && bits == tp_pt_bits && channels == tp_pt_channels){
  // rate only (direct DAC's fill trim): keep the phase and the previous frame
  tp_step_acc = tp_step_acc * rate / tp_pt_rate;
  if((rate > TP_DECIMATE(tp_hw_rate)) != (tp_pt_rate > TP_DECIMATE(tp_hw_rate))){
   tp_dsl = tp_dsr = 0; tp_dn = 0;              // across the decimator's edge
  }
  tp_pt_rate = rate;
  tp_step_inv = (1UL << 24) / rate;
 }else if(!tp_pt_active || rate != tp_pt_rate || bits != tp_pt_bits || channels != tp_pt_channels){
  // a format change only re-aims the stepper: the codec keeps its clock
  tp_pt_rate = rate; tp_pt_bits = bits; tp_pt_channels = channels;
  tp_step_acc = 0;
  tp_step_inv = (1UL << 24) / rate;
  tp_dsl = tp_dsr = 0; tp_dn = 0;
  tp_pvok = 0;
  tp_pt_active = 1;
 }
 tp_pt_cons = tp_cons;
 q = TP_QUEUED();
 room = q < TP_QMAX ? TP_QMAX - q : 0;
 unit = (channels >= 2 ? 2u : 1u) * (bits >= 16 ? 2u : 1u);
 while(bytes >= (int)unit){
  int l, r;
  if(bits >= 16){
   l = (int)(short)((unsigned)buf[0] | ((unsigned)buf[1] << 8));
   r = channels >= 2 ? (int)(short)((unsigned)buf[2] | ((unsigned)buf[3] << 8)) : l;
  }else if(channels >= 2){
   // SB Pro 8-bit stereo: the first byte of a frame is the RIGHT channel
   // (the render path's swap in sndisr.c, kept so the image does not flip)
   r = ((int)buf[0] - 128) << 8;
   l = ((int)buf[1] - 128) << 8;
  }else
   l = r = ((int)buf[0] - 128) << 8;
  tp_step_acc += tp_hw_rate;
  if(rate > TP_DECIMATE(tp_hw_rate)){
   // decimating: average the guest frames of each output interval
   tp_dsl += l; tp_dsr += r; tp_dn++;
   if(tp_step_acc >= rate){
    tp_step_acc -= rate;
    if(!room){ bytes = 0; break; }             // ring full: drop the rest
    tp_put((int)(tp_dsl / (long)tp_dn), (int)(tp_dsr / (long)tp_dn));
    tp_dsl = tp_dsr = 0; tp_dn = 0;
    room--;
   }
  }else
  // interpolating: w runs 0..256 from the previous guest frame to this one
  while(tp_step_acc >= rate){
   long w;
   tp_step_acc -= rate;
   w = 256L - (long)((tp_step_acc * tp_step_inv) >> 16);
   if(w < 0) w = 0;
   if(w > 256) w = 256;
   if(!tp_pvok) w = 256;                        // no previous frame yet
   if(!room){ bytes = 0; break; }               // ring full: drop the rest
   tp_put(tp_pl + (int)((((long)l - (long)tp_pl) * w) >> 8),
          tp_pr + (int)((((long)r - (long)tp_pr) * w) >> 8));
   room--;
  }
  tp_pl = l; tp_pr = r; tp_pvok = 1;
  buf += unit; bytes -= (int)unit;
 }
 tp_busy = 0;
}

//------------------------------------------------------------ callbacks ---
// Engine passthrough ops (ptops.h; bodies live in the engine-ABI section at
// the end of this file). NO real FM anywhere on a 755C -- the absence of
// PTF_REAL_FM is what tells the engine 0x388 is open bus here.
static void tp_dbg_tick(void);
static void tp_dbg_exit(void);
static void tp_dbg_reenter(void);
static void tp_watchdog(void);
static const struct pt_ops_s tp755_tap_ops = {
 PTF_TAP,
 TP_PT_Space, TP_PT_Feed, tp_watchdog,
 tp_dbg_tick, tp_dbg_exit, tp_dbg_reenter,
 TP755_Depth,
 0, 0,                                         // render_cap, render_div: no render path
 TP755_Clock,                                  // direct DAC rate measurement (sndisr.c)
};
// render path: the software OPL3 is live, or /RESAMP
static const struct pt_ops_s tp755_render_ops = {
 0,
 NULL, NULL, tp_watchdog,
 tp_dbg_tick, tp_dbg_exit, tp_dbg_reenter,
 TP755_Depth,
};

static int TP755_adetect(struct audioout_info_s *aui)
{
 struct tp755_card_s *card;
 const char *e;
 unsigned char id;
 unsigned long i;
 int seg, par;
 uint32_t lin, start;

 // Named explicitly like every backend now, which matters most here: this
 // one cannot look before it leaps. The 755C codec powers up dark, so
 // bringing it up REQUIRES writing ThinkPad system-control port 0x15E8 --
 // something we must never do on a machine that only might be a ThinkPad.
 if(!PTOPS_CardIs("tp755")) return 0;

 // real ch0 is ours; a guest on /D0 would reprogram it mid-ring
 if(aui->gvars->dma == 0){
  printf("CS4248: guest DMA 0 collides with the codec's real DMA ch0; use /D1 or /D3\n");
  return 0;
 }

 // Tap unless the mixer has work only it can do: the software OPL3 (live
 // unless main.c gives 388h to /LPT or /FMVOL) is rendered there, and a tap
 // never runs it. /RESAMP picks the render path by hand.
 tp_tap = !FOpts.resamp;
#ifdef CARD_TP755
 if(aui->gvars->opl3 && !FOpts.lpt && aui->gvars->fmvol < 0) tp_tap = 0;
#endif
 if(tp_tap) tp_dacrate = TP_RATE_TAP;
 if((e = getenv("SBEPTLAT")) != NULL){ int ms = atoi(e);
        if(ms >= 20 && ms <= 1000) tp_lat_ms = (unsigned)ms; }

 // /BASE here is the planar codec block (0x4E30).
 if(FOpts.base > 0 && FOpts.base <= 0xFFFC) tp_cb = (uint16_t)FOpts.base;
 if(FOpts.dacrate){ unsigned r = (unsigned)FOpts.dacrate;
        if(r < TP_RATE_MIN) r = TP_RATE_MIN;
        if(r > TP_RATE_MAX) r = TP_RATE_MAX; tp_dacrate = r; }
 if(FOpts.cvol >= 0) tp_vol = FOpts.cvol;

 // PRE-GATE, no writes: a genuine 755C answers this port whether its codec
 // is enabled or not (a disabled one reads 0x80); open bus on every other
 // machine reads 0xFF. Checking BEFORE tp_twiddle means we never poke the
 // ThinkPad system-control port 0x15E8 on a machine that is not a ThinkPad --
 // which matters now that this probe runs in a binary that also serves
 // PC110s, T2130CTs and OmniBooks. (A BIOS model/submodel gate via INT 15h
 // AH=C0h belongs on top of this, but its values have to be measured on the
 // bench first -- no machine identification exists in this tree yet.)
 if((unsigned char)inportb(tp_cb + TC_IAR) == 0xFF){
  printf("CS4248: nothing at %04Xh -- is this really a 755C? (check /BASE)\n", tp_cb);
  return 0;
 }

 tp_ctl_was_on = tp_twiddle(1);               // enable the card
 if(!(tp_ctl_was_on & TP_CTL_BIT)) TP_MS(60); // fresh power-up: let it settle

 for(i = 0; i < 400000UL; i++)                // wait INIT clear, bounded
  if(!(inportb(tp_cb + TC_IAR) & 0x80)) break;
 if(i == 400000UL) goto notfound;             // stuck busy / nothing there

 id = tp_ci_get(0x0C);                        // I12: CS4248 reads 0x8A
 if((id & 0x0F) != 0x0A) goto notfound;

 // ring in DOS conventional memory: ISA DMA needs <16MB and no 64K
 // crossing; below 1MB is identity-mapped under every stack we run
 // (the stock MDma_alloc_cardmem XMS path guarantees neither).
 // +TP_RMHOME_PARA on the tail: the whole real-mode home (v86 stub + the
 // OPL write ring -- layout owned by ptrap.c) lives there; the stub
 // outgrew the PSP and the audio ring never reaches this slack (the 64K
 // dodge keeps start within the first half).
#ifdef CARD_TP755
 par = (int)((2 * TP_RING_BYTES + 15) >> 4) + TP_RMHOME_PARA;
#else
 // No software OPL in this build, so no write ring and no relocated v86
 // stub: the stub stays in the PSP (142 bytes of the 160, 159 with the IRQ7
 // stub after it) and we
 // keep TP_RMHOME_PARA paragraphs of DOS memory that would go unused.
 par = (int)((2 * TP_RING_BYTES + 15) >> 4);
#endif
 seg = tp_dos_alloc((unsigned)par, &tp_dos_sel);
 if(seg == -1){ printf("CS4248: DOS memory alloc failed\n"); goto notfound; }
 lin = (uint32_t)seg << 4;
 start = lin;
 if((start & 0xFFFFUL) + TP_RING_BYTES > 0x10000UL)
  start = (start + 0xFFFFUL) & ~0xFFFFUL;    // dodge the 64K boundary
 tp_ring_phys = start;                        // physical == linear below 1MB
 tp_ring = (char *)TP_NEARPTR(start);
 memset(tp_ring, 0, TP_RING_BYTES);
#ifdef CARD_TP755
 PTRAP_SetOplRing(lin + (uint32_t)(par - TP_RMHOME_PARA) * 16);
#endif

 card = (struct tp755_card_s *)aui->card_private_data;   // v2.0: engine-allocated (private_data_size)
 card->base = tp_cb;
 aui->card_pDmaBuffer = tp_ring;
 aui->card_irq = 10;                          // planar-wired, bench-verified

 tp_iac = (uint8_t *)TP_NEARPTR(0x4F0);       // telemetry window (BIOS IAC)
 memset(tp_iac, 0, 16);
 tp_i8_install();                             // the clock guardian
 PTOPS_Register(tp_tap ? &tp755_tap_ops : &tp755_render_ops);

 printf("CS4248 found @ %04Xh (I12=%02Xh, TP ctl was %02Xh)\n",
        tp_cb, id, tp_ctl_was_on);
 return 1;

notfound:
 if(!(tp_ctl_was_on & TP_CTL_BIT)) tp_twiddle(0);  // leave it as we found it
 return 0;
}

static void TP755_setrate(struct audioout_info_s *aui)
{
 unsigned got;

 tp_period = aui->gvars->period_size ? (unsigned)aui->gvars->period_size
                                     : TP_PERIOD_DEF;
 // TAP: the engine runs once a period, and that is when the guest's SB
 // blocks are taken and its SB IRQs delivered. At 43 Hz (1024-byte periods
 // at 11025) DOOM2's SFX were quiet and choppy -- through the render path
 // too -- and at 172 Hz (256-byte) they were clean (755, 2026-10-02); the
 // IBM card's tap pumps at 128 Hz. So the tap caps the period at 1/128 s,
 // whatever /PS says (the launchers carry /PS1024 for the render path).
 if(tp_tap){
  unsigned cap = (unsigned)(tp_rates[tp_rate_pick(tp_dacrate)].hz / 128UL) * 4u;
  if(tp_period > cap) tp_period = cap;
 }
 if(tp_period < 128) tp_period = 128;
 if(tp_period > 2048) tp_period = 2048;
 tp_period &= ~3u;                            // whole 16-bit stereo frames
 while(TP_RING_BYTES % tp_period) tp_period -= 4;

 got = tp_codec_config(tp_dacrate);
 aui->freq_card = got;                        // the core resamples guest->this
 aui->chan_card = 2;
 aui->bits_card = 16;                         // codec native == engine native
 MDma_initbuf(aui, TP_RING_BYTES);            // v2.0: sets card_dmasize only

 // Tap queue in frames. The pump (IRQ10) visits once a period, so the pad
 // floor must outlast one period with margin, and the target sits a period
 // above the floor and a period below the hard cap.
 { unsigned pf = tp_period / 4u;
   unsigned long q = (unsigned long)got * tp_lat_ms / 1000UL;
   unsigned low;
   if(q > (unsigned long)(TP_QMAX - pf)) q = TP_QMAX - pf;
   low = (unsigned)q / 2u;
   if(low < pf + pf / 2u) low = pf + pf / 2u;
   if(q < (unsigned long)(low + pf)) q = low + pf;
   tp_qtarget = (unsigned)q; tp_qlow = low; }

 // one console line, 79 columns at most (worst case 77: 48000 Hz, 2048-byte
 // periods, a 3-digit queue -- the ring caps it at 325 ms)
 if(tp_tap)
  printf("CS4248 %04Xh: %u Hz, 8237 ch0 ring, %u-byte periods on IRQ10, tap %u ms\n",
         tp_cb, got, tp_period, (unsigned)((unsigned long)tp_qtarget * 1000UL / got));
 else
  printf("CS4248 %04Xh: %u Hz, 8237 ch0 ring, %u-byte periods on IRQ10, render\n",
         tp_cb, got, tp_period);
}

static void TP755_start(struct audioout_info_s *aui)
{
 int tries;
 memset(tp_ring, 0, TP_RING_BYTES);           // 16-bit signed silence = 0
 // the 8237 restarts at ring offset 0; the zeroed floor counts as queued.
 // tp_clock runs on across a restart.
 tp_busy = 1;
 tp_cons = 0; tp_lastp = 0; tp_bad = 0;
 tp_wr = tp_qlow;
 tp_pt_active = 0;
 tp_busy = 0;
 tp_dma_arm();
 outportb(tp_cb + TC_SR, 0);                  // clear any stale codec INT
 tp_ci_put(0x0A, I10_IEN);                    // interrupt pin enable
 tp_pen(1);

 // LOST-FIRST-EDGE HEAL (bench 2026-08-14): the codec IRQ is this design's
 // only clock, and ISA IRQs are edge-triggered -- if the first count-expiry
 // edge is swallowed (install-time delivery window), INT sticks high and
 // the whole engine is silent forever. After PEN, wait >3 periods: a live
 // ISR clears INT within microseconds, so INT still set = stuck line ->
 // write Status to drop it and let the next expiry raise a fresh edge.
 for(tries = 0; tries < 3; tries++){
  TP_MS(20);
  if(!(inportb(tp_cb + TC_SR) & SR_INT)) break;
  outportb(tp_cb + TC_SR, 0);
  TP_HEAL();
 }
}

// QUIESCE only: AU_stop fires on every guest rate change and start comes
// later -- tear nothing down (the sc_es1688 "no sound after idle" lesson)
static void TP755_stop(struct audioout_info_s *aui)
{
 tp_pen(0);
 tp_ci_put(0x0A, 0x00);                       // IEN off
}

static void TP755_close(struct audioout_info_s *aui)
{
 uint8_t f;
 tp_i8_remove();
 TP755_stop(aui);
 f = DPMI_DisableInterrupt();
 UntrappedIO_OUT(DMA_REG_SINGLEMASK, 0x04);   // mask ch0
 DPMI_RestoreInterrupt(f);
 tp_hw_armed = 0;
 if(!(tp_ctl_was_on & TP_CTL_BIT)) tp_twiddle(0);
 if(tp_dos_sel){ __dpmi_free_dos_memory(tp_dos_sel); tp_dos_sel = 0; }
 tp_ring = NULL;
 // v2.0: card_private_data is engine-owned (private_data_size); nothing to free here
}

// ring play position in bytes: the 8237 current count is remaining-1
static unsigned int TP755_getbufpos(struct audioout_info_s *aui)
{
 unsigned cnt = tp_dma_count();
 unsigned int pos = (unsigned int)((TP_RING_BYTES - 1 - cnt) & (TP_RING_BYTES - 1));
 if(tp_iac){
  tp_iac[1] = TP_PH_GETPOS;
  tp_iac[8] = (uint8_t)pos; tp_iac[9] = (uint8_t)((unsigned long)pos >> 8);
 }
 return pos;
}

// Render path: MDma_writedata at the engine's write pointer. Tap builds land
// here too for what the tap does not take (ADPCM; direct DAC in the 16-bit
// build, which has no DAC ring): engine frames are the ring's format, so they
// are appended at the tap's writer, up to the latency target. A live tap
// stream owns the ring for half a second after its last feed.
static void TP755_writedata(struct audioout_info_s *aui, char *src, unsigned int bytes)
{
 const int16_t *p = (const int16_t *)src;
 unsigned n, q;
 TP_IAC(1, TP_PH_WRITE);
 if(!tp_tap){ MDma_writedata(aui, src, bytes); return; }
 if(tp_busy) return;
 if(tp_pt_active && tp_cons - tp_pt_cons < (uint32_t)(tp_hw_rate / 2u)) return;
 tp_busy = 1;
 q = TP_QUEUED();
 n = bytes / 4u;
 if(q >= tp_qtarget) n = 0;
 else if(n > tp_qtarget - q) n = tp_qtarget - q;
 while(n--){ tp_put(p[0], p[1]); p += 2; }
 tp_busy = 0;
}

static int TP755_irq(struct audioout_info_s *aui)
{
 unsigned char s = (unsigned char)inportb(tp_cb + TC_SR);
 if(s & SR_INT) outportb(tp_cb + TC_SR, 0);   // any Status write clears INT
 if(tp_iac){ tp_iac[1] = TP_PH_CLAIM; tp_iac[5] = s; }
 if(tp_tap && tp_pen_on) tp_pump();           // track + pad ahead of the 8237
 // CLAIM UNCONDITIONALLY -- bench-proven mid-DOOM 2026-08-14: returning 0
 // chains to the IBM BIOS default INT 72h stub, which EOIs the MASTER
 // only; the slave's in-service bit sticks and IRQ10..15 (our clock AND
 // the disk) die forever. IRQ10 is exclusively the codec's on this
 // planar; worst case we render a tick early and EOI a spurious edge
 // properly -- infinitely better than the alternative.
 return 1;
}

//------------------------------------- engine ops (dbg instrument + heal) ---
// Registered through the ptops.h table in adetect. This is a render-path
// card: no tap (space/feed stay NULL, the engine's SNDISR_PassThru stays 0)
// -- except the DSP-reset hook, which doubles as our 8237 heal: a guest
// master-reset (OUT 0x0D/0x0F passes through vdma) masks real ch0 and
// starves the codec; one idempotent re-unmask fixes it for free. The dbg
// trio is this card's own IAC-window instrument (depth feeds the sndisr
// depth limiter), NOT the generic SNDISR_dbg_* BIOS-byte telemetry.
//
static void tp_dbg_tick(void)
{
 tp_tick8++;                                   /* guardian's clock: unconditional */
 if(tp_iac){
  tp_iac[0]++;                                 /* depth */
  if(tp_iac[0] > tp_iac[0x0B]) tp_iac[0x0B] = tp_iac[0]; /* high-water */
#if !RATEDIAG
  if(!++tp_iac[2]) tp_iac[3]++;                /* tick count u16 */
#endif
 }
}
static void tp_dbg_exit(void)
{
 if(tp_iac){
  if(tp_iac[0]) tp_iac[0]--;
  tp_iac[1] = TP_PH_EXIT;
 }
}
static void tp_dbg_reenter(void)
{
 if(tp_iac) tp_iac[4]++;
}
static void tp_watchdog(void)
{
 tp_pt_active = 0;                             // tap: the next feed re-aims the stepper
 if(tp_hw_armed){
  uint8_t f = DPMI_DisableInterrupt();
  UntrappedIO_OUT(DMA_REG_SINGLEMASK, 0x00);  // re-unmask ch0
  DPMI_RestoreInterrupt(f);
  // stuck-INT heal, double-read qualified: an in-flight interrupt is
  // serviced within microseconds, so INT still set after ~2ms = the edge
  // was lost and the clock is dead -> re-arm it. Only then write Status
  // (a blind clear could race a pending-but-unserviced edge and the
  // spurious chain path would leave the slave PIC without an EOI).
  if(inportb(tp_cb + TC_SR) & SR_INT){
   tp_iodelay(2000);
   if(inportb(tp_cb + TC_SR) & SR_INT){
    // clock provably dead >2ms. Un-stick a possibly-wedged in-service
    // bit first: specific EOI for level 2 on both PICs -- an 8259
    // specific EOI for a level NOT in service is a hardware no-op, so
    // this is free when the PICs are healthy. Real ports (UntrappedIO):
    // 0x20 writes would otherwise be eaten by VPIC's EOI virtualizer.
    f = DPMI_DisableInterrupt();
    UntrappedIO_OUT(0xA0, 0x62);              // slave: specific EOI IRQ10
    UntrappedIO_OUT(0x20, 0x62);              // master: specific EOI IRQ2
    DPMI_RestoreInterrupt(f);
    outportb(tp_cb + TC_SR, 0);               // re-arm the edge
    TP_HEAL();
   }
  }
 }
}
//---------------------------------------------------------------- struct ---
// VSBHDA sndcard_info_s: 14 fields. Mixer slots NULL (SBEVOL sets I6/I7 at
// config; the DS1669 analog master pots stay under the volume buttons).
struct sndcard_info_s TP755_sndcard_info={
 "CS4248",                                            // shortname
 0,                                                   // infobits
 &TP755_adetect,                                      // card_detect
 &TP755_start, &TP755_stop, &TP755_close,             // start / stop / close
 &TP755_setrate,                                      // card_setrate
 &TP755_writedata, &TP755_getbufpos,                  // writedata / getpos (v2.0: no clear slot)
 &TP755_irq,                                          // irq_routine (check+ack)
 NULL, NULL, NULL,                                    // mixer slots
 sizeof(struct tp755_card_s)                          // private_data_size: the engine allocates it
};

#endif // NOTP755
