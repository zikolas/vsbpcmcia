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
//  memory, refilled by SNDISR on the codec's own half-ring... rather,
//  per-period IRQ10. No RTC, no tick credit, no PRDY workarounds, no
//  passthrough (the sndisr tap stays unarmed): the engine renders SB PCM + emulated OPL3
//  (CARD_TP755 unmasks NOFM -- there is no FM chip anywhere on the 755C)
//  into the ring via the standard AU_cardbuf_space/AU_writedata path.
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
//     masks real ch0; the DSP-reset hook (ES1688_PT_Watchdog stub) re-
//     unmasks ch0 as a cheap idempotent heal.
//**************************************************************************

#ifdef CARD_TP755

#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <pc.h>
#include <dpmi.h>       /* system header: _go32_dpmi_* chain for the IRQ0  */
#include <go32.h>       /*   guardian (sc_vew211 i8-heartbeat precedent)   */

#include "config.h"
#include "au_cards.h"
#include "dmabuff.h"
#include "platform.h"   /* bool etc. -- must precede ptrap.h */
#include "ptrap.h"
#include "dma.h"

/* NOT linear.h: it pulls the fork's DJDPMI.H which conflicts with the
 * system <dpmi.h> we need for the go32 chain API. NearPtr re-rolled: */
extern uint32_t DSBase;
#define TP_NEARPTR(a) ((void *)((uint32_t)(a) - DSBase))

/* DPMI 0100h: allocate DOS conventional memory; selector stored for free */
static int tp_dos_alloc(unsigned paragraphs, int *sel)
{
 uint32_t eax = 0x0100, edx = 0;
 uint8_t err;
 __asm__ __volatile__("int $0x31; setc %0"
                      : "=q"(err), "+a"(eax), "=d"(edx)
                      : "b"(paragraphs)
                      : "cc", "memory");
 if(err) return -1;
 *sel = (int)(edx & 0xFFFF);
 return (int)(eax & 0xFFFF);
}

//------------------------------------------------------------- geometry ---
#define TP_CTL_IDX   0x15E8         // ThinkPad system control: index port
#define TP_CTL_DATA  0x15E9         //   data port
#define TP_CTL_AUDIO 0x1C           //   index of the audio-enable byte
#define TP_CTL_BIT   0x02           //   bit 1 = CS4248 enabled

#define TP_BASE_DEF  0x4E30         // codec block (env SBEBASE overrides)
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
#define TP_RMHOME_PARA 146u

#define TP_RING_BYTES 8192u         // must be a power of two and a multiple
#define TP_PERIOD_DEF 512u          //   of the period; 8K @ 22050 st16 = ~81ms queue
#define TP_RATE_DEF   22050u
#define TP_RATE_MIN   5510u
#define TP_RATE_MAX   48000u

//---------------------------------------------------------------- state ---
struct tp755_card_s { uint16_t base; };

static uint16_t tp_cb        = TP_BASE_DEF;  // codec base
static char    *tp_ring      = NULL;         // near ptr into the DOS block
static uint32_t tp_ring_phys = 0;            // physical addr of ring start
static int      tp_dos_sel   = 0;            // selector for __dpmi_free_dos_memory
static unsigned tp_period    = TP_PERIOD_DEF;
static unsigned tp_dacrate   = TP_RATE_DEF;  // requested (env DACRATE)
static int      tp_vol       = 8;            // I6/I7 attenuation 0..63 x -1.5dB
                                             // (env SBEVOL; default -12dB --
                                             // 0dB is LOUD on the lid speaker)
static unsigned tp_hw_rate   = 0;            // configured-format record: enables
static unsigned tp_hw_armed  = 0;            //   cheap restart + the watchdog heal
static uint8_t  tp_ctl_was_on = 0;           // enable state found at detect

// TELEMETRY (VEW211 pattern): breadcrumbs into the BIOS IAC area 0x4F0-4FF,
// readable over COMrade mid-wedge (agent alive) or after a WARM reboot
// (IAC survives Ctrl-Alt-Del on IBM BIOSes). Layout:
//   4F0 = ISR nesting depth        4F1 = last phase (see TP_PH_*)
//   4F2/3 = ISR tick count u16     4F4 = reenter (guard-skip) count
//   4F5 = last SR seen at claim    4F6 = heal count (start+watchdog)
//   4F8/9 = last getpos u16        4FA = consecutive futile heals
//   4FB = depth high-water         4FC = IRQ0 polls per tick (last)
//   4FD = polls-per-tick high-water (the IRQ0-saturation meter)
#define TP_PH_CLAIM  1   /* irq_routine claimed the interrupt      */
#define TP_PH_GETPOS 2   /* space computed (render about to start) */
#define TP_PH_WRITE  3   /* writedata reached (render finished)    */
#define TP_PH_EXIT   4   /* ISR completed                          */
static uint8_t *tp_iac = NULL;   /* NearPtr(0x4F0), set in adetect */
#define TP_IAC(off, v) do{ if(tp_iac) tp_iac[off] = (uint8_t)(v); }while(0)

//-------------------------------------------------------------- helpers ---
static uint8_t DPMI_DisableInterrupt(void)
{
 uint32_t f;
 __asm__ __volatile__("pushfl; popl %0; cli":"=r"(f)::"memory");
 return (uint8_t)((f >> 9) & 1);
}
static void DPMI_RestoreInterrupt(uint8_t on)
{
 if(on) __asm__ __volatile__("sti":::"memory");
}

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
static _go32_dpmi_seginfo tp_i8_old, tp_i8_new;
static uint8_t tp_i8_hooked = 0;
static uint8_t tp_g_last = 0, tp_g_frozen = 0, tp_g_begging = 0;
static uint8_t tp_g_futile = 0;      // consecutive heals with no tick advance
static uint8_t tp_g_healtick = 0;    // tick byte at last heal
static uint16_t tp_g_gap = 0;        // IRQ0 polls since last tick advance
static uint8_t tp_g_t2 = 0;          // tier-2 (slave re-ICW) heals fired
// Healthy-baseline IMRs, stashed at guardian install (= stack-up, before any
// guest runs): what tier 2 restores after re-initializing the slave PIC. A
// mid-init PIC returns garbage on reads, so the restore value must come from
// a moment the PIC was known-good -- NOT from the wedge itself.
static uint8_t tp_imr_a1_boot = 0;
static uint8_t tp_imr_21_boot = 0;

// VERIFY-BEFORE-REVIVE (sndisr.c gates on this): after a heal, suppress
// guest VIRQ injection for the first resumed ticks -- resurrection into
// seconds-stale guest SB state crashed MI1 where the un-healed freeze
// didn't. Only the ASYNC guardian sets it; the DSP-reset watchdog heal is
// guest-initiated (fresh state incoming) and must inject promptly.
volatile int tp_revive_squelch = 0;

// sndisr.c depth limiter reads the live nesting depth through this
int TP755_Depth(void){ return tp_iac ? (int)tp_iac[0] : 0; }

static void tp_guardian(void)
{
 if(!tp_pen_on || !tp_iac) return;
 // SATURATION METER: IRQ0 polls between engine ticks. Healthy: gap stays
 // tiny at 18.2Hz PIT (several ticks per poll) and modest at fast PITs.
 // A guest music handler whose OPL trap tax exceeds its own PIT period
 // starves IRQ10 (priority: IRQ0 > cascade) -- the gap tells that story
 // in one number. 4FC = last completed gap (u8 sat), 4FD = high water.
 if(tp_g_gap < 0xFFFF) tp_g_gap++;
 if(tp_iac[2] != tp_g_last){
  tp_g_last = tp_iac[2]; tp_g_frozen = 0; tp_g_begging = 0;
  tp_g_futile = 0;                            // real progress ends dormancy
  tp_iac[0x0C] = (uint8_t)(tp_g_gap > 255 ? 255 : tp_g_gap);
  if(tp_iac[0x0C] > tp_iac[0x0D]) tp_iac[0x0D] = tp_iac[0x0C];
  tp_g_gap = 0;
  return;
 }
 if(tp_g_frozen < 255) tp_g_frozen++;
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
   if(++tp_g_begging < 3) return;
   if(tp_iac[2] == tp_g_healtick) { if(tp_g_futile < 255) tp_g_futile++; }
   else tp_g_futile = 0;
   tp_g_healtick = tp_iac[2];
   TP_IAC(0x0A, (tp_g_t2 << 4) | (tp_g_futile > 15 ? 15 : tp_g_futile));
   // codec begging, nobody serviced: clear any mask on our line (REAL
   // IMRs -- UntrappedIO from PM context reads real hardware, unlike a
   // V86 read), fire safe specific EOIs, re-arm the edge
   v = UntrappedIO_IN(0xA1);
   if(v & 0x04) UntrappedIO_OUT(0xA1, (uint8_t)(v & ~0x04));
   v = UntrappedIO_IN(0x21);
   if(v & 0x04) UntrappedIO_OUT(0x21, (uint8_t)(v & ~0x04));
   UntrappedIO_OUT(0xA0, 0x62);              // specific EOI slave lvl 2
   UntrappedIO_OUT(0x20, 0x62);              // specific EOI master lvl 2 (cascade)
   tp_revive_squelch = 2;                    // gate injection on resume
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
   TP_IAC(6, tp_iac[6] + 1);
   TP_IAC(0x0A, (tp_g_t2 << 4) | (tp_g_futile > 15 ? 15 : tp_g_futile));
   tp_g_begging = 0; tp_g_frozen = 0;
  }else{
   tp_g_begging = 0;
   // ticks frozen and codec NOT asking: starved (masked/killed DMA ch0,
   // e.g. a guest 8237 master reset) -> re-unmask; codec resumes, count
   // expires, clock restarts. Benign if false, but be patient enough
   // that a high-rate PIT can't thrash it (32 polls, not 8).
   if(tp_g_frozen < 32) return;
   UntrappedIO_OUT(DMA_REG_SINGLEMASK, 0x00);
   TP_IAC(6, tp_iac[6] + 1);
   tp_g_frozen = 0;
  }
 }
}

static void tp_i8_install(void)
{
 if(tp_i8_hooked) return;
 tp_imr_a1_boot = (uint8_t)inportb(0xA1);   // healthy-baseline IMRs for the
 tp_imr_21_boot = (uint8_t)inportb(0x21);   // tier-2 restore (pre-guest)
 _go32_dpmi_get_protected_mode_interrupt_vector(0x08, &tp_i8_old);
 tp_i8_new.pm_offset = (unsigned long)tp_guardian;
 tp_i8_new.pm_selector = _go32_my_cs();
 if(_go32_dpmi_chain_protected_mode_interrupt_vector(0x08, &tp_i8_new) == 0)
  tp_i8_hooked = 1;
}

static void tp_i8_remove(void)
{
 if(!tp_i8_hooked) return;
 _go32_dpmi_set_protected_mode_interrupt_vector(0x08, &tp_i8_old);
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

//------------------------------------------------------------ callbacks ---
static int TP755_adetect(struct audioout_info_s *aui)
{
 struct tp755_card_s *card;
 const char *e;
 unsigned char id;
 unsigned long i;
 int seg, par;
 uint32_t lin, start;

 // real ch0 is ours; a guest on /D0 would reprogram it mid-ring
 if(aui->gvars->dma == 0){
  printf("CS4248: guest DMA 0 collides with the codec's real DMA ch0; use /D1 or /D3\n");
  return 0;
 }

 e = getenv("SBEBASE");
 if(e){ long b = strtol(e, NULL, 16); if(b > 0 && b <= 0xFFFC) tp_cb = (uint16_t)b; }
 e = getenv("DACRATE");
 if(e){ unsigned r = (unsigned)atoi(e);
        if(r){ if(r < TP_RATE_MIN) r = TP_RATE_MIN;
               if(r > TP_RATE_MAX) r = TP_RATE_MAX; tp_dacrate = r; } }
 e = getenv("SBEVOL");
 if(e){ int a = atoi(e); if(a >= 0 && a <= 63) tp_vol = a; }

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
 par = (int)((2 * TP_RING_BYTES + 15) >> 4) + TP_RMHOME_PARA;
 seg = tp_dos_alloc((unsigned)par, &tp_dos_sel);
 if(seg == -1){ printf("CS4248: DOS memory alloc failed\n"); goto notfound; }
 lin = (uint32_t)seg << 4;
 start = lin;
 if((start & 0xFFFFUL) + TP_RING_BYTES > 0x10000UL)
  start = (start + 0xFFFFUL) & ~0xFFFFUL;    // dodge the 64K boundary
 tp_ring_phys = start;                        // physical == linear below 1MB
 tp_ring = (char *)TP_NEARPTR(start);
 memset(tp_ring, 0, TP_RING_BYTES);
 PTRAP_SetOplRing(lin + (uint32_t)(par - TP_RMHOME_PARA) * 16);

 card = (struct tp755_card_s *)calloc(1, sizeof(*card));
 if(!card){ __dpmi_free_dos_memory(tp_dos_sel); tp_dos_sel = 0; goto notfound; }
 card->base = tp_cb;
 aui->card_private_data = card;
 aui->card_DMABUFF = tp_ring;
 aui->card_irq = 10;                          // planar-wired, bench-verified

 tp_iac = (uint8_t *)TP_NEARPTR(0x4F0);       // telemetry window (BIOS IAC)
 memset(tp_iac, 0, 16);
 tp_i8_install();                             // the clock guardian

 printf("CS4248 found @ %4.4Xh (I12=%02Xh, TP ctl was %02Xh)\n",
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
 if(tp_period < 128) tp_period = 128;
 if(tp_period > 2048) tp_period = 2048;
 tp_period &= ~3u;                            // whole 16-bit stereo frames
 while(TP_RING_BYTES % tp_period) tp_period -= 4;

 got = tp_codec_config(tp_dacrate);
 aui->freq_card = got;                        // the core resamples guest->this
 aui->chan_card = 2;
 aui->bits_card = 16;                         // codec native == engine native
 MDma_init_pcmoutbuf(aui, TP_RING_BYTES, tp_period);

 printf("CS4248 (TP755 planar WSS) 8237-ch0 autoinit ring @ %4.4Xh, %u Hz, "
        "%u-byte periods on IRQ10\n", tp_cb, got, tp_period);
}

static void TP755_start(struct audioout_info_s *aui)
{
 int tries;
 memset(tp_ring, 0, TP_RING_BYTES);           // 16-bit signed silence = 0
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
  TP_IAC(6, tp_iac[6] + 1);
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
 if(aui->card_private_data){ free(aui->card_private_data); aui->card_private_data = NULL; }
}

// ring play position in bytes: the 8237 current count is remaining-1
static long TP755_getbufpos(struct audioout_info_s *aui)
{
 unsigned cnt = tp_dma_count();
 long pos = (long)((TP_RING_BYTES - 1 - cnt) & (TP_RING_BYTES - 1));
 if(tp_iac){
  tp_iac[1] = TP_PH_GETPOS;
  tp_iac[8] = (uint8_t)pos; tp_iac[9] = (uint8_t)((unsigned long)pos >> 8);
 }
 return pos;
}

// thin MDma_writedata wrapper: phase breadcrumb = "render finished, copying"
static void TP755_writedata(struct audioout_info_s *aui, char *src, unsigned long bytes)
{
 TP_IAC(1, TP_PH_WRITE);
 MDma_writedata(aui, src, bytes);
}

static int TP755_irq(struct audioout_info_s *aui)
{
 unsigned char s = (unsigned char)inportb(tp_cb + TC_SR);
 if(s & SR_INT) outportb(tp_cb + TC_SR, 0);   // any Status write clears INT
 if(tp_iac){ tp_iac[1] = TP_PH_CLAIM; tp_iac[5] = s; }
 // CLAIM UNCONDITIONALLY -- bench-proven mid-DOOM 2026-08-14: returning 0
 // chains to the IBM BIOS default INT 72h stub, which EOIs the MASTER
 // only; the slave's in-service bit sticks and IRQ10..15 (our clock AND
 // the disk) die forever. IRQ10 is exclusively the codec's on this
 // planar; worst case we render a tick early and EOI a spurious edge
 // properly -- infinitely better than the alternative.
 return 1;
}

//------------------------------------------- engine ABI (PT stubs + heal) ---
// sndisr.c/vsb.c link against the historical ES1688_* names regardless of
// backend. This is a render-path card: the engine's tap gate (SNDISR_PassThru,
// owned by sndisr.c) stays 0 and the PT entry points are inert -- except the
// DSP-reset hook, which doubles as our 8237 heal: a guest master-reset
// (OUT 0x0D/0x0F passes through vdma) masks real ch0 and starves the codec;
// one idempotent re-unmask fixes it for free.
//
void ES1688_dbg_tick(void)
{
 if(tp_iac){
  tp_iac[0]++;                                 /* depth */
  if(tp_iac[0] > tp_iac[0x0B]) tp_iac[0x0B] = tp_iac[0]; /* high-water */
  if(!++tp_iac[2]) tp_iac[3]++;                /* tick count u16 */
 }
}
void ES1688_dbg_exit(void)
{
 if(tp_iac){
  if(tp_iac[0]) tp_iac[0]--;
  tp_iac[1] = TP_PH_EXIT;
 }
}
void ES1688_dbg_reenter(void)
{
 if(tp_iac) tp_iac[4]++;
}
int  ES1688_PT_Space(void){ return 0; }
void ES1688_PT_Feed(const unsigned char *p, int n, unsigned r, unsigned b, unsigned c)
{ (void)p;(void)n;(void)r;(void)b;(void)c; }
void ES1688_PT_Watchdog(void)
{
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
    TP_IAC(6, tp_iac[6] + 1);
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
 &MDma_writedata, &TP755_getbufpos, &MDma_clearbuf,   // writedata / getpos / clear
 &TP755_irq,                                          // irq_routine (check+ack)
 NULL, NULL, NULL                                     // mixer slots
};

#endif // CARD_TP755
