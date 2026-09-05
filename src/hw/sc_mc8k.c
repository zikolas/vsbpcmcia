/* sc_mc8k.c -- TDK MC-8000 / DMC-9000 (EMU8200) backend for VSBPCMCIA.
 *
 * >>> UNTESTED SKELETON, written off-bench 2026-08-29. It compiles and the
 * >>> design follows bench-proven recon, but NOTHING here has run on the
 * >>> card. The one genuine unknown is the DRAM WRITE PATH: every prior
 * >>> write attempt (TDKWTST, 8 variants) ran on an incomplete chip init,
 * >>> and TDKWT2 (tdk-dmc9000 docs/dmc8000-blackbox/) exists to settle the
 * >>> formulation before this file's first hardware run. Bench gates below.
 *
 * WHY THIS BACKEND IS ARCHITECTURALLY DIFFERENT. The card has no codec data
 * port: the CS4216 is the EMU8200's DAC, not an addressable register file.
 * The vendor plays PCM by streaming samples into EMU sample DRAM via SMLD
 * and running a looping voice over a ring buffer -- during WAV playback PTR
 * parks on SMLD (0x3A) for the whole stream (TDKWAVE, 2026-08-25). So this
 * backend feeds a DRAM ring and manages ONE voice over it:
 *
 *   guest DMA -> tap -> host ring (mono 16-bit) -> RTC pump -> SMLD -> DRAM
 *                                                              ring <- voice
 *                                                              loops at the
 *                                                              GUEST rate
 *
 * The voice's pitch register plays the ring at any rate, continuously --
 * so unlike every codec backend there is NO rate table, NO frame stepper
 * and NO resample: pitch is always exact. A rate change is three register
 * writes, not a 25 ms MCE dance.
 *
 * CHANNEL BUDGET (shared chip with TDKSYN -- see COEXISTENCE below):
 *   0..26  TDKSYN's synth voices (it must cap NVOICE at 27 -- pairing note)
 *   27     this backend's RIGHT PCM voice (stereo streams)
 *   28     this backend's LEFT/mono PCM voice
 *   29     this backend's DRAM write channel (CCCA write mode, silent)
 *   30/31  DRAM refresh (init_fm) -- MUST KEEP RUNNING, IFATN-muted:
 *          released refresh = DRAM contents evaporate (TDKWTST lesson).
 *
 * COEXISTENCE WITH TDKSYN. PTR (win0+0Eh) is shared chip state. This pump
 * runs in the RTC ISR, TDKSYN synthesises in INT 2Fh/trap context, so the
 * ISR can preempt TDKSYN mid PTR+DATA pair -- but never the reverse. The
 * interlock is therefore ONE-SIDED and lives in TDKSYN: it must cli/sti
 * bracket each PTR+DATA transaction (queued paired change, not yet made).
 * On OUR side every transaction is cli-bracketed anyway because our own ISR
 * nests (SETIF=1) and the IRQ0 heartbeat can land mid-sequence.
 *
 * BENCH GATES (gate 1 SETTLED 2026-08-29 -- TDKWT2 on the DMC-9000: writes
 * land 48/48 per-sample under the full init, ear-proven "beautiful clean
 * buzzy tone"; legacy init lands 0/48, so init_arrays/HWCF4-6 was the gate):
 *   2. Pitch constant: M8_PT44K=0x6000 comes from calc_pt(0xE000) and the
 *      proven "IP=0xE000 plays 44100" correspondence; verify a 11025 Hz
 *      sine from SBDIAG plays at pitch (a 2:1 error would be unmissable).
 *   3. CCCA position readback stability in ISR context (the resync path
 *      tolerates failure but should mostly succeed).
 *   4. TDKSYN pairing: NVOICE 30->28 + cli/sti around its poke/peek pairs,
 *      then GOTSYN + this backend on one boot (music + SFX, one card).
 *
 * Bring-up: run MC8KGO first -- this backend validates the EMU, it does not
 * enable the card. /CARD:MC8K [/BASE240|/BASE260]; base is probed 240h then
 * 260h when /BASE is absent (MC-8000 = 240h, DMC-9000 = 260h).
 *
 * Port map (bench-proven, sc-relative): D0lo=+4 D0hi=+6 D1=+8 D2=+A D3=+C
 * PTR=+E; dword regs write lo->port, hi->port+2. Read protocol and all
 * register semantics: tdk-dmc9000 HANDOVER.md + the project memory.
 */
#ifndef NOMC8K

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

#define pds_calloc            calloc
#define pds_free              free

// hostsvc.h only carries byte I/O (no earlier backend needed words); the
// EMU8200 is a 16-bit register file, so spell the word forms here.
#ifdef DJGPP
# define m8_inw(p)      inportw((unsigned short)(p))
# define m8_outw(p,v)   outportw((unsigned short)(p),(unsigned short)(v))
#else
# define m8_inw(p)      inpw(p)
# define m8_outw(p,v)   outpw((p),(unsigned)(v))
#endif

// ==========================================================================
//  Telemetry (the 0x4F0-0x4FF map -- see doc/NOTES.md). This backend's own
//  bytes: 0x4F8 irq_routine calls, 0x4FB/0x4FF 16-bit feed count, 0x4FE
//  ring-full feed clamps, 0x4F4 watchdog revivals, 0x4F5 ring gauge,
//  0x4FA reconfigs, 0x4F2 guest rate>>8, 0x4F6 CCCA resync corrections.
// ==========================================================================
static uint16_t m8_tel_feed16;
static unsigned long m8_tel_bytes;
static unsigned char m8_tel_irq;
static unsigned char m8_tel_recfg;
static unsigned char m8_tel_rsync;

// ==========================================================================
//  EMU8200 register layer
// ==========================================================================
#define M8_WIN_BASE 0x240           // MC-8000 default; DMC-9000 sits at 0x260
static uint16_t m8_base = M8_WIN_BASE;
#define M8_PTR ((uint16_t)(m8_base + 0x0E))
#define M8_D0  ((uint16_t)(m8_base + 0x04))
#define M8_D1  ((uint16_t)(m8_base + 0x08))
#define M8_D2  ((uint16_t)(m8_base + 0x0A))
#define M8_D3  ((uint16_t)(m8_base + 0x0C))
#define M8_CMD(r,c) ((unsigned)(((r) << 5) | (c)))

static void m8_iodelay(unsigned n){ while(n--) (void)inportb(0x80); }

// Every transaction is a PTR select + data access and must not be split by
// an interrupt that also selects PTR (our own nested ISR, the IRQ0
// heartbeat, or -- once armed -- nothing else: TDKSYN can't preempt an ISR).
// Init-context callers take the same per-transaction lock rather than one
// long cli: chip_init holds a 23 ms wait and the COMrade serial ISR must
// keep breathing through it.
static void m8_wr(uint16_t dp, unsigned cmd, unsigned v)
{
 uint8_t f = DPMI_DisableInterrupt();
 m8_outw(M8_PTR, cmd); m8_outw(dp, v);
 DPMI_RestoreInterrupt(f);
}
static void m8_wrdw(uint16_t dp, unsigned cmd, uint32_t v)
{
 uint8_t f = DPMI_DisableInterrupt();
 m8_outw(M8_PTR, cmd);
 m8_outw(dp, (unsigned)(v & 0xFFFF));
 m8_outw((uint16_t)(dp + 2), (unsigned)(v >> 16));
 DPMI_RestoreInterrupt(f);
}
static unsigned m8_rd(uint16_t dp, unsigned cmd)
{
 unsigned v; uint8_t f = DPMI_DisableInterrupt();
 m8_outw(M8_PTR, cmd); v = m8_inw(dp);
 DPMI_RestoreInterrupt(f);
 return v;
}
static uint32_t m8_rddw(uint16_t dp, unsigned cmd)
{
 unsigned lo, hi; uint8_t f = DPMI_DisableInterrupt();
 m8_outw(M8_PTR, cmd); lo = m8_inw(dp); hi = m8_inw((uint16_t)(dp + 2));
 DPMI_RestoreInterrupt(f);
 return ((uint32_t)hi << 16) | lo;
}

#define DCYSUSV(c,v) m8_wr(M8_D1, M8_CMD(5,c), v)
#define IFATN(c,v)   m8_wr(M8_D3, M8_CMD(1,c), v)
#define CPF(c,v)     m8_wrdw(M8_D0, M8_CMD(0,c), v)
#define PTRX(c,v)    m8_wrdw(M8_D0, M8_CMD(1,c), v)
#define CVCF(c,v)    m8_wrdw(M8_D0, M8_CMD(2,c), v)
#define VTFT(c,v)    m8_wrdw(M8_D0, M8_CMD(3,c), v)
#define PSST(c,v)    m8_wrdw(M8_D0, M8_CMD(6,c), v)
#define CSL(c,v)     m8_wrdw(M8_D0, M8_CMD(7,c), v)
#define CCCA(c,v)    m8_wrdw(M8_D1, M8_CMD(0,c), v)
#define CCCA_RD(c)   m8_rddw(M8_D1, M8_CMD(0,c))
#define IP(c,v)      m8_wr(M8_D3, M8_CMD(0,c), v)
#define PEFE(c,v)    m8_wr(M8_D3, M8_CMD(2,c), v)
#define FMMOD(c,v)   m8_wr(M8_D3, M8_CMD(4,c), v)
#define TREMFRQ(c,v) m8_wr(M8_D3, M8_CMD(5,c), v)
#define FM2FRQ2(c,v) m8_wr(M8_D3, M8_CMD(6,c), v)
#define ATKHLDV(c,v) m8_wr(M8_D2, M8_CMD(4,c), v)
#define LFO1VAL(c,v) m8_wr(M8_D2, M8_CMD(5,c), v)
#define ATKHLD(c,v)  m8_wr(M8_D2, M8_CMD(6,c), v)
#define LFO2VAL(c,v) m8_wr(M8_D2, M8_CMD(7,c), v)
#define ENVVOL(c,v)  m8_wr(M8_D1, M8_CMD(4,c), v)
#define ENVVAL(c,v)  m8_wr(M8_D1, M8_CMD(6,c), v)
#define DCYSUS(c,v)  m8_wr(M8_D1, M8_CMD(7,c), v)
#define SMALR(v)     m8_wrdw(M8_D1, M8_CMD(1,20), v)
#define SMALW(v)     m8_wrdw(M8_D1, M8_CMD(1,22), v)
#define SMARR(v)     m8_wrdw(M8_D1, M8_CMD(1,21), v)
#define SMARW(v)     m8_wrdw(M8_D1, M8_CMD(1,23), v)
#define HWCF1(v)     m8_wr(M8_D1, M8_CMD(1,29), v)
#define HWCF2(v)     m8_wr(M8_D1, M8_CMD(1,30), v)
#define HWCF3(v)     m8_wr(M8_D1, M8_CMD(1,31), v)
#define HWCF4(v)     m8_wrdw(M8_D1, M8_CMD(1,9), v)
#define HWCF5(v)     m8_wrdw(M8_D1, M8_CMD(1,10), v)
#define HWCF6(v)     m8_wrdw(M8_D1, M8_CMD(1,13), v)
#define INIT1(c,v)   m8_wr(M8_D1, M8_CMD(2,c), v)
#define INIT2(c,v)   m8_wr(M8_D2, M8_CMD(2,c), v)
#define INIT3(c,v)   m8_wr(M8_D1, M8_CMD(3,c), v)
#define INIT4(c,v)   m8_wr(M8_D2, M8_CMD(3,c), v)
#define R0080(c,v)   m8_wrdw(M8_D0, M8_CMD(4,c), v)
#define R00A0(c,v)   m8_wrdw(M8_D0, M8_CMD(5,c), v)
#define SMLD_CMD     M8_CMD(1,26)

#include "emu8kini.h"     // init1..init4 (GPL-2.0, Linux emu8000.c verbatim)

// ==========================================================================
//  Geometry: voices, DRAM ring, host ring, pacing
// ==========================================================================
#define M8_VCH  28                  // LEFT/mono PCM voice
#define M8_VCHR 27                  // RIGHT PCM voice (stereo streams)
#define M8_WCH  29                  // DRAM write channel (silent, mode CCCA)
#define M8_PT44K 0x6000U            // CPF/PTRX pitch that plays 44100 f/s
                                    // (= calc_pt(0xE000); bench gate 2)

// DRAM ring: word address space, RAM starts at 0x200000 on both cards
// (MC-8000: 512KB real, aliasing above; DMC-9000: 2MB). 0x210000 is the
// vendor-populated-proven region. 8192 words = 8192 mono frames: 371 ms at
// 22050, 1 s at 8000 -- the write lead (latency target) always fits.
#define M8_DRAM_BASE   0x210000UL   // LEFT/mono ring
#define M8_DRAM_BASE_R 0x214000UL   // RIGHT ring (stereo), clear of L's end
#define M8_RING_W    8192U
#define M8_RING_WMASK (M8_RING_W - 1U)

// Host ring: raw tap bytes normalised to mono 16-bit LE (2 bytes/frame).
#define RING_BYTES   8192U
#define RING_MASK    (RING_BYTES - 1U)
#define BYTES_PER_SBSAMPLE 4        // engine render frames are 16-bit stereo

#define M8_RS_MIN    6              // fastest pump = 1024 Hz. NOT 2048: on the
                                    // 486SX floor the fixed per-tick cost
                                    // (SNDISR + DPMI + chunk overhead) is a
                                    // large fraction of a 488us period, so a
                                    // 2048 Hz pump livelocks -- the first
                                    // smoke runs wedged the PC110 (2026-08-29)
#define M8_RS_IDLE   11             // idle keep-alive = 32 Hz
#define M8_BURST     32             // frames per pump pass (one cli section);
                                    // 32 x 1024 Hz = 32768 f/s sustained --
                                    // covers 22050 with 48% headroom, and
                                    // 44100 guests get the 2048 Hz pump.
#define M8_PRIME     256            // silence frames written at voice arm so
                                    // the voice never plays stale ring data
                                    // before the pump's lead builds

static volatile unsigned ring_wr, ring_rd;            // host ring, byte idx
static volatile unsigned m8_flush_gen, m8_flush_ack;
static unsigned char ring_buf[RING_BYTES];

static unsigned      m8_dacrate = 22050;              // /DACRATE: render-path rate only
static unsigned char m8_rtc_rs  = M8_RS_IDLE;
static unsigned char m8_rs_want = M8_RS_IDLE;
static int           m8_adaptive;
static unsigned      m8_pt_lat_ms = 250;              // SBEPTLAT: write lead target

static volatile uint32_t m8_tick_seq;                 // ++ per delivered RTC tick
static volatile uint32_t m8_feed_seq;                 // tick at last PT_Feed
static int               m8_pt_ever;

static volatile int m8_pt_active;
static unsigned m8_pt_rate, m8_pt_bits, m8_pt_channels;
static unsigned long m8_frate;                        // voice consume rate, f/s
static volatile int m8_voice_on;
static volatile int m8_pump_busy;
static volatile int m8_pt_feed_busy;
static unsigned char m8_reentry;
static unsigned char m8_tel_drop;

// DRAM cursor state. All frame counters are free-running 32-bit absolutes;
// ring positions are (x & M8_RING_WMASK). OWNERSHIP: wr_abs/play_est/accs
// are written only by the pump (busy-guarded) and by voice arm, which runs
// in feed context under the feed busy guard with the voice provably off.
static uint32_t m8_wr_abs;                            // frames written to DRAM
static uint32_t m8_play_est;                          // est. frames consumed
static uint32_t m8_pe_acc;                            // frac accumulator (per-hz)
static uint32_t m8_last_ticks;                        // tick_seq at last credit
static uint32_t m8_lead;                              // target wr_abs - play_est
static unsigned m8_prime_left;                        // silence frames still to
static uint32_t m8_arm_start;                         // lay before voice start
static unsigned m8_rs_ct;                             // resync divider
// OPEN-LOOP BY DEFAULT (settled on the 235, 2026-08-29): with TADA at
// ear-verified correct pitch -- i.e. the voice provably consumes at the
// model rate -- CCCA readback still claimed ~2.1x that rate (breadcrumb
// err ~ -1576/round, 136 corrections/session), so in this context the
// position read is unreliable and every "correction" jerked the chase
// target ~70 ms and forced a catch-up burst = audible roughness. Idle
// re-arms re-zero the write/play relationship at every silence gap, so
// open-loop error has no path to accumulate. SBEM8RS=1 re-enables the
// closed loop for experiments; the 0x4F9 breadcrumb only exists there.
static int      m8_norsync = 1;
static int      m8_noflush;                           // SBEM8NF=1: no ring flush
                                                      // on guest DSP reset
// Deferred pitch: a rate change is a BOUNDARY in the ring, not an event.
// DMX-class guests (DOOM) flap between the SFX rate and a low-rate silence
// loop constantly; retargeting the voice instantly replayed ~60 ms of
// queued 11025 audio at ~2 kHz and mangled sounds triggered mid-flap (the
// 235 cutouts, 2026-08-29: 61 genuine retargets in one session). The
// change is stamped at wr_abs when it arrives; the pump applies it when
// play_est crosses that stamp, so content always plays at the rate it was
// written at. A second change before the first applies keeps the earliest
// boundary and the newest rate -- at most one lead's worth plays slightly
// off, never garbled.
static unsigned m8_pend_rate;                         // 0 = none pending (RETIRED --
static uint32_t m8_pend_at;                           // see the stepper note below)
// FIXED-PITCH + FRAME STEPPER (the fleet-proven shape, adopted 2026-08-29
// late after a night of chasing SB_Rate): VSB's inferred rate is UNSTABLE
// GARBAGE on single-cycle/direct-DAC playback (13 retargets in one TADA,
// values as low as ~300 Hz -- impossible via time constant). The codec
// backends are immune because their clock NEVER follows SB_Rate; the
// stepper re-steps the DATA. Same here now: the voice's pitch is set once
// at arm and never retargeted; a claimed-rate change only adjusts the
// Bresenham+lerp fold of guest frames onto the voice's fixed rate. Garbage
// rates then cause at worst brief mis-stepping, exactly as on the fleet.
static unsigned long m8_step_in;                      // claimed guest frames/s
static unsigned long m8_step_acc;
static int           m8_step_pv, m8_step_pvr;         // previous frame (L, R)
static int           m8_step_pvok;
// Stereo = the emu8000_pcm dual-voice model: LEFT voice on its ring, RIGHT
// voice on a second ring, same pitch, armed back-to-back (constant sub-ms
// skew, same as the reference). Host ring frames are 4 bytes (L16,R16) in
// stereo mode, 2 in mono; the pump writes one word per ring per frame.
static int      m8_st;                                // current stream stereo?
static unsigned m8_fbytes = 2;                        // host ring bytes/frame
// Bisect gate (235, 2026-08-29 late): 0 = retarget pitch immediately on a
// rate change (the behaviour of the correct-pitch/both-channels build);
// 1 = defer to the ring boundary. The deferred build coincided with a
// left-only/broken-sine regression that a constant-rate sine should never
// have exercised -- separate code from card state before trusting it.
#define M8_DEFER_PITCH 0
// WC (CMD(1,28), 16-bit, +1 per output frame at 44100) is the chip's own
// wall clock. NOT CMD(1,27) -- that is SMRD, the sample-memory read-data
// port, and polling it per tick froze the estimate AND poked the memory
// pipeline (the beep-loop/no-sound/EP-wedge build, 2026-08-29 late). The
// proof of 28: init_fm's documented tail writes PTR=0x3C, D1=0 -- the
// reference's WC clear, and 0x3C = CMD(1,28).
// Advancing play_est from WC deltas replaces the
// modeled RTC arithmetic with the hardware's truth: RTC tick loss stops
// mattering to the estimate, and deferred-pitch boundaries land exactly.
// Wraps every 1.49 s -- fine at any live pump rate; idle re-arms re-prime.
static unsigned m8_wc_last;
static uint32_t m8_wc_acc;
// OFF until bench-verified (SBEM8WC=1): on the 235 the WC read at CMD(1,28)
// came back CONSTANT -- and a constant clock passes the read-pair agreement
// gate, so the estimate froze and the pump stopped after one lead (the
// steady-beep builds, 2026-08-29 late). Verify the register with a probe
// tool before trusting it; the modeled advance below is the shipped path.
static int      m8_usewc;

// Write formulation -- SETTLED ON THE BENCH 2026-08-29 (TDKWT2, DMC-9000):
// per-word SMALW seek -> SMLD write -> busy-bit wait = 48/48 across three
// addresses (variants 1 and 9), needs only the write channel armed, and the
// busy bit clears in <=5 polls. Auto-increment carries a ONE-DEEP PIPELINE
// LAG: each burst's last word stays latched and lands at the next seek's
// first address (variants 2/4, 15/48) -- usable only with never-re-seek
// streaming or slow settle pacing, so it is the EXPERIMENT here, not the
// default: SBEWRAI=1 selects it for A/B (with the wait kept).
static int m8_wrai;

// Guest-rate ceiling (SBEMAXHZ, default 22050): above it the feed DECIMATES
// 2:1/4:1 and the voice plays at rate/dec -- exact pitch, half the words.
// Pump load is rate-proportional whatever the tick rate, and 44100 words/s
// of the proven 10-I/O recipe is beyond the SX/33; a capped stream is the
// difference between "half bandwidth" and "wedged box".
static unsigned m8_rate_ceil = 22050;
static unsigned m8_dec = 1;                           // store every Nth frame
static unsigned m8_dec_ct;

#define M8_RTC_HZ() (32768UL >> (m8_rtc_rs - 1))

static void rtc_enable(void);
static void m8_rtc_setrate(unsigned char rs);

// ==========================================================================
//  Pitch: rate -> IP (initial pitch, exponential) and CPF/PTRX target
// ==========================================================================
// IP = 0xE000 + 0x1000/octave relative to 44100 (bench-proven: TDKGM plays
// a correct scale from these semantics). The pitch engine derives the
// running pitch from IP (PEFE=0, LFOs nulled), so IP must be right; the
// CPF/PTRX write just jump-starts the slew.
static const uint32_t m8_semi[13] = {   // 2^(i/12) in 16.16
 65536UL, 69433UL, 73562UL, 77936UL, 82570UL, 87480UL, 92682UL,
 98193UL, 104032UL, 110218UL, 116772UL, 123715UL, 131072UL
};
static unsigned m8_ip_for_rate(unsigned rate)
{
 long oct = 0; int s; long ip, units;
 uint32_t ratio;
 if(rate < 690) rate = 690;                           // 6 octaves down: IP floor
 // ratio = rate/44100 in 16.16; 44100<<16 fits 32 bits, rate<<16 may not
 // for rate>65535 -- rates are capped well below that.
 ratio = (uint32_t)(((uint32_t)rate << 16) / 44100UL);
 while(ratio < 65536UL)  { ratio <<= 1; oct--; }
 while(ratio >= 131072UL){ ratio >>= 1; oct++; }
 for(s = 11; s > 0; s--) if(ratio >= m8_semi[s]) break;
 // linear interpolation inside the semitone: worst-case pitch error ~0.06%
 units  = ((long)s * 4096L) / 12L;
 units += (long)(((ratio - m8_semi[s]) * (uint32_t)((((long)(s+1)*4096L)/12L) - units))
                 / (m8_semi[s+1] - m8_semi[s]));
 ip = 0xE000L + oct * 0x1000L + units;
 if(ip < 0) ip = 0;
 if(ip > 0xFFFFL) ip = 0xFFFFL;
 return (unsigned)ip;
}
static unsigned m8_pt_for_rate(unsigned rate)
{
 uint32_t pt = (uint32_t)rate * (uint32_t)M8_PT44K / 44100UL;
 if(pt > 0xFFFFUL) pt = 0xFFFFUL;
 if(pt < 1) pt = 1;
 return (unsigned)pt;
}

// ==========================================================================
//  Chip init (SYNTH.C chip_init, keepref semantics forced ON: DRAM needs
//  the refresh channels RUNNING; they are IFATN-muted, the /M baseline)
// ==========================================================================
static void m8_send_array(const unsigned *d)
{
 int i, k = 0;
 for(i = 0; i < 32; i++) INIT1(i, d[k++]);
 for(i = 0; i < 32; i++) INIT2(i, d[k++]);
 for(i = 0; i < 32; i++) INIT3(i, d[k++]);
 for(i = 0; i < 32; i++) INIT4(i, d[k++]);
}
static void m8_init_fm(void)
{
 long g;
 DCYSUSV(30, 0x80); PSST(30, 0xFFFFFFE0UL); CSL(30, 0x00FFFFE8UL);
 PTRX(30, 0); CPF(30, 0); CCCA(30, 0x00FFFFE3UL);
 DCYSUSV(31, 0x80); PSST(31, 0x00FFFFF0UL); CSL(31, 0x00FFFFF8UL);
 PTRX(31, 0); CPF(31, 0x8000UL); CCCA(31, 0x00FFFFF3UL);
 m8_wr(M8_D0, M8_CMD(1,30), 0);
 g = 0; while(!(m8_inw(M8_PTR) & 0x1000)){ if(++g > 200000L) break; }
 g = 0; while( (m8_inw(M8_PTR) & 0x1000)){ if(++g > 200000L) break; }
 m8_wr(M8_D0, M8_CMD(1,30), 0x4828);
 { uint8_t f = DPMI_DisableInterrupt();
   outportb(M8_PTR, 0x3C); outportb(M8_D1, 0);
   DPMI_RestoreInterrupt(f); }
}
static void m8_w1cfg(void)
{
 // window1+4/+6 = the onboard 16550's FCR/LCR (FIFO on, 8N1) -- part of the
 // vendor's persistent state, harmless, and MC8KGO/TDKSYN write it too.
 // Window1 tracks window0's model: 0x320 on both cards.
 outportb(0x324, 0xC1); outportb(0x326, 0x03);
}
static void m8_chip_init(void)
{
 int c;
 HWCF1(0x0059); HWCF2(0x0020); HWCF3(0x0000);
 m8_w1cfg();
 for(c = 0; c < 32; c++) DCYSUSV(c, 0x80);
 for(c = 0; c < 32; c++){
  ENVVOL(c, 0); ENVVAL(c, 0); DCYSUS(c, 0);
  ATKHLDV(c, 0); LFO1VAL(c, 0); ATKHLD(c, 0); LFO2VAL(c, 0);
  IP(c, 0); IFATN(c, 0); PEFE(c, 0);
  FMMOD(c, 0); TREMFRQ(c, 0); FM2FRQ2(c, 0);
  PTRX(c, 0); VTFT(c, 0); PSST(c, 0); CSL(c, 0); CCCA(c, 0);
 }
 for(c = 0; c < 32; c++){ CPF(c, 0); CVCF(c, 0); }
 SMALR(0); SMARR(0); SMALW(0); SMARW(0);
 m8_send_array(emu_init1);
 m8_iodelay(40000);                                   // 1024 sample clocks
 m8_send_array(emu_init2);
 m8_send_array(emu_init3);
 HWCF4(0); HWCF5(0x83UL); HWCF6(0x8000UL);
 m8_send_array(emu_init4);
 m8_init_fm();
 for(c = 0; c < 32; c++) DCYSUSV(c, 0x807F);
 // Vendor triple, LAST (writes earlier get overwritten by the init; the
 // readback never matches verbatim -- 0038->0039 etc. -- and that is a
 // proven red herring: audibility is what settles HWCF).
 HWCF1(0x0038); HWCF3(0x0007);
 m8_w1cfg();
 IFATN(30, 0xFFFF); IFATN(31, 0xFFFF);                // refresh stays RUNNING
}

// ==========================================================================
//  DRAM write channel + voice
// ==========================================================================
static void m8_dma_chan(int ch, uint32_t mode)        // 0x06000000 = write
{
 DCYSUSV(ch, 0x80);
 VTFT(ch, 0); CVCF(ch, 0);
 PSST(ch, 0); CSL(ch, 0);
 CCCA(ch, mode);
 PTRX(ch, 0x40000000UL);
 CPF(ch, 0x40000000UL);
}
static void m8_dma_close(int ch){ CCCA(ch, 0); DCYSUSV(ch, 0x807F); }

// Stream n words to the DRAM ring. Frames come from the host ring while it
// has them, silence (0x0000) past that -- the pump's contract is that
// wr_abs advances by exactly n either way, so the write lead over the voice
// never depends on the guest. silence=1 forces the source to silence
// without touching the host ring (the arm prime).
//
// CLI IS CHUNKED, NOT PASS-WIDE (first smoke run 2026-08-29 wedged the
// PC110: a 32-word pass is ~10 port I/O per word on a slow PCMCIA bus, and
// holding one cli across it at pump rate is a near-total interrupt
// blackout -- serial ISR starves, box livelocks, voice loops unattended).
// Each cli section covers M8_CLI_WORDS words and re-seeks SMALW itself, so
// an interrupting PTR user (our own nested ISR, IRQ0, later TDKSYN)
// between chunks costs nothing but the re-seek.
#define M8_CLI_WORDS 8
// UNLOCKED single-word put, the bench-proven TDKWT2 v1/v9 recipe: seek,
// write, wait-until-clear. Poll cap sized from the bench (max 5 seen), so
// a misbehaving busy bit cannot stretch a cli section. Callers hold cli.
static void m8_put(uint32_t a, unsigned sv)
{
 unsigned p;
 m8_outw(M8_PTR, M8_CMD(1,22));
 m8_outw(M8_D1, (unsigned)(a & 0xFFFF));
 m8_outw((uint16_t)(M8_D1 + 2), (unsigned)(a >> 16));
 m8_outw(M8_PTR, SMLD_CMD);
 m8_outw(M8_D1, sv);
 m8_outw(M8_PTR, M8_CMD(1,22));
 for(p = 0; p < 24; p++) if(!(m8_inw((uint16_t)(M8_D1 + 2)) & 0x8000)) break;
}
static void m8_dram_pass(unsigned n, int silence)
{
 unsigned i = 0;
 unsigned rd = ring_rd, wr = ring_wr;
 // Per cli chunk: cw frames; stereo writes TWO words per frame (its own
 // ring each), so halve the chunk to keep the blackout bounded. The wrai
 // auto-inc experiment cannot interleave two rings and stays mono-only.
 unsigned cw = m8_st ? (M8_CLI_WORDS / 2U) : M8_CLI_WORDS;
 while(i < n){
  unsigned stop = i + cw;
  uint8_t f;
  if(stop > n) stop = n;
  f = DPMI_DisableInterrupt();
  if(m8_wrai && !m8_st){                              // experiment: park + auto-inc
   uint32_t a = M8_DRAM_BASE + ((m8_wr_abs + i) & M8_RING_WMASK);
   m8_outw(M8_PTR, M8_CMD(1,22));
   m8_outw(M8_D1, (unsigned)(a & 0xFFFF));
   m8_outw((uint16_t)(M8_D1 + 2), (unsigned)(a >> 16));
   m8_outw(M8_PTR, SMLD_CMD);
  }
  for(; i < stop; i++){
   unsigned sl = 0, sr = 0;
   uint32_t idx = (m8_wr_abs + i) & M8_RING_WMASK;
   if(!silence && ((wr - rd) & RING_MASK) >= m8_fbytes){
    sl = (unsigned)ring_buf[rd] | ((unsigned)ring_buf[(rd + 1) & RING_MASK] << 8);
    if(m8_st)
     sr = (unsigned)ring_buf[(rd + 2) & RING_MASK]
        | ((unsigned)ring_buf[(rd + 3) & RING_MASK] << 8);
    rd = (rd + m8_fbytes) & RING_MASK;
   }
   if(!m8_wrai || m8_st){
    m8_put(M8_DRAM_BASE + idx, sl);
    if(m8_st) m8_put(M8_DRAM_BASE_R + idx, sr);
   }else{
    m8_outw(M8_D1, sl);
    // auto-inc marched to the ring end: the next chunk's preamble re-seeks
    if(((m8_wr_abs + i + 1) & M8_RING_WMASK) == 0) { i++; break; }
   }
  }
  DPMI_RestoreInterrupt(f);
 }
 if(!silence) ring_rd = rd;
 m8_wr_abs += n;
}

// Voice over the DRAM ring: the TDKGM/SYNTH.C recipe (vendor envelopes:
// hold forever, no decay -- a PCM stream wants no shaping) with the ring as
// the loop. Off-by-ones per the recon: CCCA start-1, PSST loopstart-1, CSL
// loopend-1. PSST[31:24] = LEFT level, PTRX[7:0] = RIGHT level (the stereo
// fix); PTRX[15:8] reverb and CSL[31:24] chorus sends stay 0 -- nonzero
// sends sink the voice unless the fx engine is programmed the EMU8200 way
// (the reverted 2026-08-29 fx round).
// pan_volumes[128]/[127] = 0xDF/0xDF -- SYNTH.C's measured centre pair
// (PSST[31:24] = LEFT level, PTRX[7:0] = RIGHT level, the TDKPLAY stereo
// discovery). The first 235 run used raw FF/FF and older TDKGM-era targets
// (VTFT/CVCF full) and came out LEFT-ONLY with onset artifacts; this is now
// the play_zone flat recipe verbatim: amplitude belongs to the envelope
// engine (vtarget 0 -- ALSA's voltarget path is #if 0'd "leads to some
// clicks"), CVCF starts at 0x0000FF00.
// Centre pair DF/DF = pan_volumes[128]/[127]. SBEM8PAN=LLRR (hex bytes,
// e.g. DFFF) trims it by ear: the PSST pan path and the PTRX aux path are
// different buses on this silicon and a gain mismatch between them shows
// as a left bias on mono content (PC110 observation, 2026-08-30).
static unsigned m8_pan_l = 0xDF, m8_pan_r = 0xDF;
#define M8_PAN_L ((uint32_t)m8_pan_l)
#define M8_PAN_R ((uint32_t)m8_pan_r)
static void m8_voice_go(int ch, unsigned rate, uint32_t dbase,
                        uint32_t start_frame, unsigned panl, unsigned panr)
{
 unsigned ip = m8_ip_for_rate(rate);
 unsigned pt = m8_pt_for_rate(rate);
 uint32_t st = dbase + (start_frame & M8_RING_WMASK);
 DCYSUSV(ch, 0x0080);
 VTFT(ch, 0x0000FFFFUL);
 CVCF(ch, 0x0000FFFFUL);
 PTRX(ch, 0); CPF(ch, 0);
 IP(ch, ip);
 ENVVAL(ch, 0x8000); ATKHLD(ch, 0xFF7F); DCYSUS(ch, 0xFF00);
 ENVVOL(ch, 0x8000); ATKHLDV(ch, 0xFF7F);
 IFATN(ch, 0xFF00);
 PEFE(ch, 0);
 LFO1VAL(ch, 0x8000); LFO2VAL(ch, 0x8000);
 FMMOD(ch, 0); TREMFRQ(ch, 0); FM2FRQ2(ch, 0);
 PSST(ch, ((uint32_t)panl << 24) | (dbase - 1UL));
 CSL(ch, (dbase + M8_RING_W) - 1UL);                          // chorus byte 0
 CCCA(ch, st - 1UL);                                          // Q=0
 R0080(ch, 0); R00A0(ch, 0);
 VTFT(ch, 0x0000FFFFUL);                                      // vtarget 0, filter open
 CVCF(ch, 0x0000FF00UL);
 PTRX(ch, ((uint32_t)pt << 16) | panr);
 CPF(ch, (uint32_t)pt << 16);
 DCYSUSV(ch, 0xFF00);                                         // sustain, no decay
}
// PSST[31:24] = LEFT level, PTRX[7:0] = RIGHT level: the LEFT voice sends
// left only, the RIGHT voice right only; a mono stream centres on DF/DF.
static void m8_voice_start(unsigned rate, uint32_t start_frame)
{
 if(m8_st){
  m8_voice_go(M8_VCH,  rate, M8_DRAM_BASE,   start_frame, 0xFFU, 0x00U);
  m8_voice_go(M8_VCHR, rate, M8_DRAM_BASE_R, start_frame, 0x00U, 0xFFU);
 }else
  m8_voice_go(M8_VCH,  rate, M8_DRAM_BASE,   start_frame,
              (unsigned)M8_PAN_L, (unsigned)M8_PAN_R);
}
static void m8_voice_pitch(unsigned rate)
{
 unsigned pt = m8_pt_for_rate(rate), ip = m8_ip_for_rate(rate);
 IP(M8_VCH, ip);
 PTRX(M8_VCH, ((uint32_t)pt << 16) | (m8_st ? 0x00U : (unsigned)M8_PAN_R));
 CPF(M8_VCH, (uint32_t)pt << 16);
 if(m8_st){
  IP(M8_VCHR, ip);
  PTRX(M8_VCHR, ((uint32_t)pt << 16) | 0xFFU);
  CPF(M8_VCHR, (uint32_t)pt << 16);
 }
}
static void m8_voice_stop(void)
{
 DCYSUSV(M8_VCH, 0x807F); IFATN(M8_VCH, 0xFFFF);
 CVCF(M8_VCH, 0); VTFT(M8_VCH, 0);
 DCYSUSV(M8_VCHR, 0x807F); IFATN(M8_VCHR, 0xFFFF);            // harmless if
 CVCF(M8_VCHR, 0); VTFT(M8_VCHR, 0);                          // never started
}

// Arm a fresh stream: write PRIME frames of silence at the cursor, then
// start the voice at the FRONT of that silence -- it plays known-silent
// DRAM while the pump builds the lead, and never touches stale ring data.
// The prime NO LONGER runs here: laying 256 frames of silence inside one
// tap call was the backend's one unbounded ISR pass (~9 ms measured), and
// under a DOS4GW game's trap tax it spanned enough RTC periods to march
// the ISR stack -- the Duke3D 22 kHz first-sound wedge (235, 2026-08-29
// night; 11 kHz survived only because the slower tick halves the nesting
// exposure). The pump lays the prime across normal burst-capped ticks and
// starts the voice when the runway is done; stream start costs ~8 ticks of
// latency instead of one killer pass.
static void m8_stream_arm(unsigned rate)
{
 m8_st     = (m8_pt_channels >= 2);
 m8_fbytes = m8_st ? 4U : 2U;
 m8_frate  = rate;
 m8_pe_acc = 0;
 m8_lead   = (uint32_t)((unsigned long)rate * m8_pt_lat_ms / 1000UL);
 if(m8_lead < 256) m8_lead = 256;
 if(m8_lead > M8_RING_W - 2U * M8_BURST) m8_lead = M8_RING_W - 2U * M8_BURST;
 m8_pend_rate = 0;
 m8_step_in = rate; m8_step_acc = 0; m8_step_pvok = 0;
 m8_arm_start  = m8_wr_abs;
 m8_prime_left = M8_PRIME;
}

// ==========================================================================
//  Passthrough ops
// ==========================================================================
static void m8_watchdog(void)
{
 // RTC-death watchdog, verbatim design from sc_scp55.c: trigger on OUR tick
 // counter going stale, verify the arm state, fix only what is provably
 // wrong; the PF-eating reg-C heal gated behind confirmed wall-clock silence.
 static unsigned char m8_tel_revive;
 static uint32_t wd_seq;
 static unsigned char wd_stale, wd_sec, wd_secchg;
 unsigned char b, sec;
 uint8_t f;
 uint32_t seq = m8_tick_seq;
 if(seq != wd_seq){ wd_seq = seq; wd_stale = 0; wd_secchg = 0; return; }
 if(++wd_stale < 2) return;
 wd_stale = 0;
 f = DPMI_DisableInterrupt();
 outportb(0x70,0x8B); b   = (unsigned char)inportb(0x71);
 outportb(0x70,0x80); sec = (unsigned char)inportb(0x71);
 DPMI_RestoreInterrupt(f);
 if(!(b & 0x40)){
  rtc_enable();
  LOW_PokeB(0x4F4, ++m8_tel_revive);
  return;
 }
 if(inportb(0xA1) & 0x01){
  f = DPMI_DisableInterrupt();
  outportb(0xA1, (unsigned char)(inportb(0xA1) & ~0x01));
  DPMI_RestoreInterrupt(f);
  LOW_PokeB(0x4F4, ++m8_tel_revive);
  return;
 }
 if(sec != wd_sec){
  wd_sec = sec;
  if(++wd_secchg >= 2){
   wd_secchg = 0;
   f = DPMI_DisableInterrupt();
   outportb(0x70,0x0C); (void)inportb(0x71);
   DPMI_RestoreInterrupt(f);
   LOW_PokeB(0x4F4, ++m8_tel_revive);
  }
 }
}

static unsigned char m8_tel_rst;
static void M8_PT_Watchdog(void)                      // vsb.c: every guest DSP reset
{
 m8_watchdog();
 LOW_PokeB(0x4F9, ++m8_tel_rst);                      // guest DSP resets seen
 // SBEM8NF=1 skips the ring flush. Each flush empties the ring, so the
 // pump writes SILENCE into DRAM for a full lead while the voice loops
 // over it = one click per guest DSP reset. DOOM resets ~every 2 s (30
 // resets in a 1-minute run), which matches the observed 2 s tick exactly.
 // Kept as a knob rather than a default until it is bench-decided: with no
 // flush, audio the guest queued before the reset still plays out.
 // The flush IS required (235, 2026-08-30): dropping it to chase the
 // combined-stack tick DISTORTED SFX (stale ring content overlaps the new
 // sound) and did NOT stop the tick -- because the tick is NOT from the
 // flush. It persists at the DOS prompt after the game exits, i.e. with no
 // guest DMA and no DSP resets at all, so it is the two residents
 // (TDKSYN + this backend) driving the EMU with uncoordinated PTR access.
 // The real fix is the TDKSYN pairing (cli/sti PTR interlock + NVOICE<=27),
 // not anything here. Keep the flush.
 if(!m8_noflush) m8_flush_gen++;
 m8_pt_active = 0;
 m8_pt_rate = m8_pt_bits = m8_pt_channels = 0;
}

static int M8_PT_Space(void)
{
 unsigned used = (ring_wr - ring_rd) & RING_MASK;
 unsigned target = RING_BYTES - 64;
 if(m8_pt_rate){
  // Host-ring latency budget rides ON TOP of the DRAM write lead; keep the
  // host share small (a third of SBEPTLAT) so total latency ~= the lead.
  unsigned long bps = m8_frate * (unsigned long)m8_fbytes;
  unsigned t = (unsigned)(bps * m8_pt_lat_ms / 3000UL);
  if(t < 512) t = 512;
  if(t < target) target = t;
 }
 if(used >= target) return 0;
 { unsigned space = target - used;
   // Callers count GUEST bytes; conversion to mono16 (and the decimator)
   // changes the byte rate.
   unsigned gbpf = (m8_pt_channels >= 2 ? 2U : 1U) * (m8_pt_bits >= 16 ? 2U : 1U);
   return (int)((unsigned long)space * gbpf * m8_dec / (unsigned long)m8_fbytes); }
}

static void m8_pump(void);

static void M8_PT_Feed(const unsigned char *buf, int bytes, unsigned rate, unsigned bits, unsigned channels)
{
 unsigned wr, gbpf;
 if(m8_pt_feed_busy){ LOW_PokeB(0x4F3, ++m8_reentry); return; }
 m8_pt_feed_busy = 1;
 // Consume a pending DSP-reset flush BEFORE this feed's bytes land. Left
 // to the pump it executes lazily, and DMX-class guests (DOOM) reset the
 // DSP then stream the new SFX immediately -- the deferred flush then wiped
 // the NEW sound's head whenever no pump tick ran in between, which ate
 // short SFX nondeterministically (235 bench, 2026-08-29). Skip if the
 // pump owns the ring right now; it will honour the flush itself.
 if(m8_flush_gen != m8_flush_ack && !m8_pump_busy){
  m8_flush_ack = m8_flush_gen;
  ring_rd = ring_wr;
 }
 ++m8_tel_feed16;
 LOW_PokeB(0x4FB, (unsigned char)m8_tel_feed16);
 LOW_PokeB(0x4FF, (unsigned char)(m8_tel_feed16 >> 8));
 m8_feed_seq = m8_tick_seq;
 if(m8_adaptive && m8_rtc_rs > m8_rs_want) m8_rtc_setrate(m8_rs_want);
 // RATE DEFENSE (DOOM/DMX, 235 bench 2026-08-29): the SFX-per-block churn
 // makes VSB's time-constant inference emit occasional garbage (a ~2.1 kHz
 // rate was latched mid-game, 61 reconfig entries). A codec backend's rate
 // TABLE clamps absurd values and its pitch is crystal-locked either way;
 // here pitch IS the rate, so garbage warbles the voice. Two gates: the
 // SB-legal clamp, and no retarget off a near-empty feed -- bogus rates
 // ride block-boundary feeds that carry no real audio.
 if((!m8_pt_active || rate != m8_pt_rate || bits != m8_pt_bits || channels != m8_pt_channels)
    && (bytes >= 64 || !m8_voice_on)){
  unsigned eff;
  m8_pt_rate = rate; m8_pt_bits = bits; m8_pt_channels = channels;
  m8_pt_active = 1; m8_pt_ever = 1;
  // Below ~3906 Hz is impossible via SB time constant = provably garbage
  // (direct-DAC estimates); a garbage claimed rate keeps the previous
  // stepper input rather than stretching the fold 30x.
  if(rate > 45454U) rate = 45454U;
  m8_dec = 1; eff = (rate >= 4000U) ? rate : 0;
  while(eff > m8_rate_ceil && m8_dec < 4){ m8_dec <<= 1; eff = rate / m8_dec; }
  m8_dec_ct = 0;
  if(!m8_voice_on){
   m8_stream_arm(eff ? eff : 11025U);                 // VOICE PITCH FIXED HERE
   m8_voice_on = 1;
   LOW_PokeB(0x4F6, ++m8_tel_rsync);                  // voice arms (0x4F6 is
   LOW_PokeB(0x4FA, ++m8_tel_recfg);                  // free in open-loop)
  }else if(eff && (eff != m8_step_in || (channels >= 2) != m8_st)){
   if(((ring_wr - ring_rd) & RING_MASK) < 64 || (channels >= 2) != m8_st){
    // Ring near-empty = a STREAM boundary, not a mid-stream wobble: re-arm
    // the voice at the new stream's own rate. Fixes the cross-stream
    // staleness (DOOM's 11025 voice surviving into EP because DOOM left
    // the RTC dead long enough that the idle-stop never fired -- EP then
    // played folded to DOOM's pitch, half its bandwidth gone).
    m8_voice_stop();
    m8_stream_arm(eff);
    LOW_PokeB(0x4F6, ++m8_tel_rsync);
   }else{
    // Mid-stream wobble (EP's claimed rate jitters by a few % constantly:
    // 28 changes in one session): adjust the fold ratio IN PLACE. The old
    // acc/pvok reset was a discontinuity per change -- 28 clicks = the
    // "missing samples" texture; the ratio change itself is benign.
    m8_step_in = eff;
   }
   LOW_PokeB(0x4F2, (unsigned char)(rate >> 8));
   LOW_PokeB(0x4FA, ++m8_tel_recfg);
  }
  if(m8_adaptive){
   unsigned need = (unsigned)(m8_frate / M8_BURST);
   unsigned char rs;
   need += need / 4 + 1;
   for(rs = M8_RS_IDLE; rs > M8_RS_MIN; rs--)
    if((32768U >> (rs - 1)) >= need) break;
   m8_rs_want = rs;
   if(m8_rtc_rs > rs) m8_rtc_setrate(rs);
  }
 }
 // Normalise guest PCM to mono 16-bit LE into the host ring. 8-bit guest
 // data is unsigned (SB convention), 16-bit is signed LE; stereo downmixes
 // (TODO(stereo): a second voice hard-panned, like emu8000_pcm's pair).
 gbpf = (channels >= 2 ? 2U : 1U) * (bits >= 16 ? 2U : 1U);
 wr = ring_wr;
 { unsigned free_ = (ring_rd - wr - 1U) & RING_MASK;
   unsigned gmax = (unsigned)((unsigned long)free_ / m8_fbytes * gbpf);
   if((unsigned)bytes > gmax){
    LOW_PokeB(0x4FE, ++m8_tel_drop);
    bytes = (int)gmax;
   } }
 // Normalise each guest frame (stereo keeps L/R separate now -- the dual
 // voices carry them), then FOLD onto the voice's fixed rate: Bresenham +
 // linear interpolation, the sc_scp55 stepper in spirit (w = 256 -
 // acc*256/in, clamp NOT optional, lossless at an exact rate match).
 // m8_dec's coarse 2:1 pre-decimation still runs first.
 { unsigned long in_r = m8_step_in ? m8_step_in : m8_frate;
   while(bytes >= (int)gbpf){
    int sl, sr;
    if(m8_dec > 1 && ++m8_dec_ct < m8_dec){           // rate-ceiling decimation
     buf += gbpf; bytes -= (int)gbpf;
     continue;
    }
    m8_dec_ct = 0;
    if(bits >= 16){
     if(channels >= 2){
      sl = (int)(short)((unsigned)buf[0] | ((unsigned)buf[1] << 8));
      sr = (int)(short)((unsigned)buf[2] | ((unsigned)buf[3] << 8));
     }else
      sl = sr = (int)(short)((unsigned)buf[0] | ((unsigned)buf[1] << 8));
    }else{
     if(channels >= 2){
      sl = ((int)buf[0] ^ 0x80) << 8;
      sr = ((int)buf[1] ^ 0x80) << 8;
     }else
      sl = sr = ((int)buf[0] ^ 0x80) << 8;
    }
    if(!m8_st){                                       // mono stream mode:
     if(channels >= 2) sl = (sl >> 1) + (sr >> 1);    // downmix a stereo
     sr = sl;                                         // claim just in case
    }
    m8_step_acc += m8_frate;
    while(m8_step_acc >= in_r){
     long w;
     int vl, vr;
     m8_step_acc -= in_r;
     if(((ring_rd - wr - 1U) & RING_MASK) < m8_fbytes) break;  // ring full
     w = 256L - (long)((m8_step_acc << 8) / in_r);
     if(w < 0)   w = 0;
     if(w > 256) w = 256;
     if(!m8_step_pvok) w = 256;
     vl = m8_step_pv  + (int)((((long)(sl - m8_step_pv))  * w) >> 8);
     ring_buf[wr] = (unsigned char)(vl & 0xFF);        wr = (wr + 1) & RING_MASK;
     ring_buf[wr] = (unsigned char)((vl >> 8) & 0xFF); wr = (wr + 1) & RING_MASK;
     if(m8_st){
      vr = m8_step_pvr + (int)((((long)(sr - m8_step_pvr)) * w) >> 8);
      ring_buf[wr] = (unsigned char)(vr & 0xFF);        wr = (wr + 1) & RING_MASK;
      ring_buf[wr] = (unsigned char)((vr >> 8) & 0xFF); wr = (wr + 1) & RING_MASK;
     }
     m8_tel_bytes += m8_fbytes;
    }
    m8_step_pv = sl; m8_step_pvr = sr; m8_step_pvok = 1;
    buf += gbpf; bytes -= (int)gbpf;
   } }
 if(!SNDISR_HasTsc){
  unsigned u16 = (unsigned)((m8_tel_bytes >> 4) & 0xFFFF);
  LOW_PokeB(0x4FC, (unsigned char)u16);
  LOW_PokeB(0x4FD, (unsigned char)(u16 >> 8));
 }
 ring_wr = wr;
 m8_pump();                                           // keep the lead fed inline
 m8_pt_feed_busy = 0;
}

// ==========================================================================
//  The pump: chase play_est + lead, burst-capped; resync from CCCA
// ==========================================================================
static void m8_pump(void)
{
 uint32_t hz, want;
 unsigned n;
 if(m8_pump_busy) return;
 m8_pump_busy = 1;
 if(m8_flush_gen != m8_flush_ack){
  m8_flush_ack = m8_flush_gen;
  ring_rd = ring_wr;
 }
 if(!m8_voice_on){ m8_pump_busy = 0; return; }

 if(m8_prime_left){                                   // lay the runway, one
  n = (m8_prime_left > M8_BURST) ? M8_BURST : m8_prime_left;   // burst per visit
  m8_dram_pass(n, 1);
  m8_prime_left -= n;
  if(!m8_prime_left){
   m8_play_est   = m8_arm_start;
   m8_pe_acc     = 0;
   m8_last_ticks = m8_tick_seq;
   m8_wc_last = (unsigned)m8_rd(M8_D1, M8_CMD(1,28));
   m8_wc_acc  = 0;
   m8_voice_start((unsigned)m8_frate, m8_arm_start);
  }
  m8_pump_busy = 0;
  return;
 }

 hz = M8_RTC_HZ();
 if(m8_usewc){
  unsigned a = m8_rd(M8_D1, M8_CMD(1,28));
  unsigned b = m8_rd(M8_D1, M8_CMD(1,28));
  if((unsigned)((b - a) & 0xFFFF) <= 4 && b != m8_wc_last){
   unsigned d = (unsigned)((b - m8_wc_last) & 0xFFFF);
   m8_wc_last = b;
   m8_wc_acc += (uint32_t)d * m8_frate;
   m8_play_est += m8_wc_acc / 44100UL;
   m8_wc_acc %= 44100UL;
  }else{
   m8_pe_acc += m8_frate;
   m8_play_est += m8_pe_acc / hz;
   m8_pe_acc %= hz;
  }
 }else{
  // Credit from the TICK COUNTER, not per pump entry: a tick that lands
  // while a pass is running hits the busy guard and used to contribute
  // ZERO advance -- at 22050 the pass occupies enough of the 1024 Hz
  // period that roughly half the credit vanished, the model ran at ~half
  // reality, and the voice lapped the write frontier: TADA elongated with
  // mid-sample restarts, the CCCA "2.1x" reading, DOOM's cutouts -- one
  // mechanism (235, 2026-08-29 late). Delta-crediting pays skipped ticks
  // on the next entry. Clamp at ~1 s so an idle wake cannot burst.
  uint32_t dt = m8_tick_seq - m8_last_ticks;
  m8_last_ticks = m8_tick_seq;
  if(dt > hz) dt = hz;
  m8_pe_acc += m8_frate * dt;
  m8_play_est += m8_pe_acc / hz;
  m8_pe_acc %= hz;
 }


 // Occasional closed-loop correction: CCCA is a live counter, so accept two
 // reads within 2 frames of each other (the read-until-stable protocol,
 // adapted for a moving target). Corrections only when the estimate has
 // drifted past 64 frames -- RTC vs EMU crystal drift is slow; anything
 // large means a lost-tick burst or a wrong pitch constant (bench gate 2).
 if(!m8_norsync && ++m8_rs_ct >= 64){
  uint32_t a, b;
  m8_rs_ct = 0;
  a = CCCA_RD(M8_VCH) & 0x00FFFFFFUL;
  b = CCCA_RD(M8_VCH) & 0x00FFFFFFUL;
  if((a <= b && b - a <= 2) || (a > b && a - b <= 2)){
   if(b >= M8_DRAM_BASE && b < M8_DRAM_BASE + M8_RING_W){
    uint32_t rel = b - M8_DRAM_BASE;
    uint32_t play = m8_wr_abs - ((m8_wr_abs - rel) & M8_RING_WMASK);
    long err = (long)(m8_play_est - play);
    // 0x4F9 breadcrumb: (err+2048)>>4, so 0x80 = in sync, above = the
    // voice consumes SLOWER than the model, below = faster. First 235 run
    // corrected on every round (0x4F6=205) -- this byte says which way and
    // how much, which is what calibrates M8_PT44K if it is off.
    { long e = err; if(e > 2047) e = 2047; if(e < -2048) e = -2048;
      LOW_PokeB(0x4F9, (unsigned char)((e + 2048) >> 4)); }
    if(err > 64 || err < -64){
     m8_play_est = play;
     LOW_PokeB(0x4F6, ++m8_tel_rsync);
    }
   }
  }
 }

 want = m8_play_est + m8_lead - m8_wr_abs;
 if((long)want > 0){
  n = ((long)want > (long)M8_BURST) ? M8_BURST : (unsigned)want;
  m8_dram_pass(n, 0);
  // A persistently-behind pump is NOT chased with a faster tick: the first
  // smoke runs proved that ratchet is a death spiral on the SX floor (each
  // step multiplies demanded work on a box already saturated). Falling
  // behind now just shortens the effective lead -- degraded, alive.
 }
 m8_pump_busy = 0;
}

// ==========================================================================
//  RTC plumbing (verbatim shape from sc_scp55.c)
// ==========================================================================
static void rtc_enable(void)
{
 uint8_t f = DPMI_DisableInterrupt();
 outportb(0x70,0x8A); { unsigned char a=(unsigned char)inportb(0x71); outportb(0x70,0x8A); outportb(0x71,(a&0xF0)|m8_rtc_rs); }
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
static void m8_rtc_setrate(unsigned char rs)
{
 uint8_t f = DPMI_DisableInterrupt();
 outportb(0x70,0x8A); { unsigned char a=(unsigned char)inportb(0x71); outportb(0x70,0x8A); outportb(0x71,(a&0xF0)|(rs&0x0F)); }
 DPMI_RestoreInterrupt(f);
 m8_rtc_rs = rs;
}

// ---- IRQ0 heartbeat: guest-independent watchdog host (watchdog ONLY,
// keep it tiny -- it runs on whatever stack the guest's INT8 was on) ------
static DPMI_ISR_HANDLE m8_i8_handle;
static int m8_i8_on;
static void m8_irq0_isr(void){ m8_watchdog(); }
static void m8_i8_install(void)
{
 if(m8_i8_on) return;
 if(getenv("ESNOI8")) return;
 if(DPMI_InstallISR(0x08, &m8_irq0_isr, &m8_i8_handle, TRUE) != 0) return;
 m8_i8_on = 1;
}
static void m8_i8_remove(void)
{
 if(!m8_i8_on) return;
 DPMI_UninstallISR(&m8_i8_handle);
 m8_i8_on = 0;
}

// ---- live ISR nesting depth for the sndisr depth limiter (sc_scp55 note:
// a NULL depth hook short-circuits the limiter entirely) -------------------
static volatile int m8_isr_depth;
static void m8_dbg_tick(void){ m8_isr_depth++; SNDISR_dbg_tick(); }
static void m8_dbg_exit(void){ SNDISR_dbg_exit(); if(m8_isr_depth) m8_isr_depth--; }
static int  M8_Depth(void){ return m8_isr_depth; }

// ==========================================================================
//  au_cards interface
// ==========================================================================
typedef struct mc8k_card_s { uint16_t base; } mc8k_card_s;

// No PTF_REAL_FM: no FM silicon on either card and no 0x388 window at all.
// /FMSHIM keeps FM-timing detection alive; music belongs to the MPUSHIM +
// TDKSYN stack on the same EMU8200 (the GOTSYN pairing).
static const struct pt_ops_s mc8k_pt_ops = {
 PTF_TAP,
 M8_PT_Space, M8_PT_Feed, M8_PT_Watchdog,
 m8_dbg_tick, m8_dbg_exit, SNDISR_dbg_reenter,
 M8_Depth,
};

// EMU presence probe, non-destructive: PTR echoes the written CMD in its
// low byte (TDKSWEEP-proven). Safe next to a resident TDKSYN -- every
// TDKSYN transaction re-selects PTR itself, and this holds the lock.
static int m8_emu_here(uint16_t base)
{
 unsigned v1, v2; uint8_t f;
 m8_base = base;
 if(m8_inw(M8_PTR) == 0xFFFF) return 0;
 f = DPMI_DisableInterrupt();
 m8_outw(M8_PTR, 0x3D); v1 = m8_inw(M8_PTR);
 m8_outw(M8_PTR, 0x3E); v2 = m8_inw(M8_PTR);
 DPMI_RestoreInterrupt(f);
 return (v1 & 0xFF) == 0x3D && (v2 & 0xFF) == 0x3E;
}

static int MC8K_adetect(struct audioout_info_s *aui)
{
 mc8k_card_s *card;
 uint16_t base = 0;
 const char *t = getenv("SBERTC");
 const char *l = getenv("SBEPTLAT");
 if(!PTOPS_CardIs("mc8k")) return 0;
 if(getenv("SBEWRAI")) m8_wrai = 1;                   // auto-inc A/B (see knob note)
 if(getenv("SBEM8RS")) m8_norsync = 0;                // closed-loop resync A/B
 if(getenv("SBEM8WC")) m8_usewc = 1;                  // chip-clock pacing (unverified)
 if(getenv("SBEM8NF")) m8_noflush = 1;                // no flush on DSP reset (A/B)
 { const char *pn = getenv("SBEM8PAN");               // LLRR hex pan trim
   if(pn && pn[0]){ long v = strtol(pn, NULL, 16);
     if(v > 0 && v <= 0xFFFFL){ m8_pan_l = (unsigned)((v >> 8) & 0xFF);
                                m8_pan_r = (unsigned)(v & 0xFF); } } }
 { const char *mh = getenv("SBEMAXHZ");
   if(mh){ long v = atol(mh);
           if(v >= 4000L && v <= 48000L) m8_rate_ceil = (unsigned)v; } }
 if(FOpts.base) base = (uint16_t)FOpts.base;
 if(base){
  if(!m8_emu_here(base)){
   printf("EMU8200: nothing at %4.4Xh -- run MC8KGO first, and check /BASE\n", (unsigned)base);
   return 0;
  }
 }else if(m8_emu_here(0x240)) base = 0x240;           // MC-8000
 else if(m8_emu_here(0x260))  base = 0x260;           // DMC-9000
 else{
  printf("EMU8200: nothing at 240h or 260h -- run MC8KGO first (or give /BASE)\n");
  return 0;
 }
 m8_base = base;
 if(FOpts.dacrate){ m8_dacrate = (unsigned)FOpts.dacrate;
        if(m8_dacrate < 4000)  m8_dacrate = 4000;
        if(m8_dacrate > 44100) m8_dacrate = 44100; }
 if(t){ int rs = atoi(t); if(rs>=3 && rs<=15) m8_rtc_rs = (unsigned char)rs; }
 else { m8_adaptive = 1; m8_rtc_rs = M8_RS_IDLE; }
 if(l){ int ms = atoi(l); if(ms >= 30 && ms <= 2000) m8_pt_lat_ms = (unsigned)ms; }
 if(FOpts.resamp)
  printf("EMU8200: /RESAMP ignored -- passthrough already plays exact pitch\n");

 // Full chip init unless told not to. Safe under a resident TDKSYN: the
 // init redoes exactly what TDKSYN's own chip_init did (same tables, same
 // HWCF triple), cuts any currently-sounding notes once, and TDKSYN
 // re-programs every voice per note anyway. What it CHANGES is the refresh
 // disposition: TDKSYN releases ch30/31 (ROM playback needs no refresh);
 // DRAM playback does, so they end RUNNING and IFATN-muted here.
 // SBENOEMUINIT=1 skips it for A/B -- but then refresh must already run.
 if(!getenv("SBENOEMUINIT"))
  m8_chip_init();
 else{
  m8_init_fm();                                       // refresh only
  IFATN(30, 0xFFFF); IFATN(31, 0xFFFF);
 }
 m8_dma_chan(M8_WCH, 0x06000000UL);                   // DRAM write channel, silent

 card = (mc8k_card_s *)aui->card_private_data;   // v2.0: engine-allocated (private_data_size)
 card->base = base;
 aui->card_irq = 8;                                   // RTC drives the pump
 PTOPS_Register(&mc8k_pt_ops);
 printf("EMU8200: window %3.3Xh, DRAM ring %u frames at %6.6lXh, voice %d, write ch %d\n",
        (unsigned)base, (unsigned)M8_RING_W, (unsigned long)M8_DRAM_BASE, M8_VCH, M8_WCH);
 // Bench breadcrumb: the vendor-init readback is 0038/0053/0007-family
 // (low bits never match verbatim -- known red herring, do not chase).
 printf("EMU8200: HWCF %4.4X/%4.4X/%4.4X\n",
        m8_rd(M8_D1, M8_CMD(1,29)), m8_rd(M8_D1, M8_CMD(1,30)), m8_rd(M8_D1, M8_CMD(1,31)));
 return 1;
}

static void MC8K_setrate(struct audioout_info_s *aui)
{
 mc8k_card_s *card = aui->card_private_data;
 aui->freq_card = m8_dacrate;
 aui->chan_card = 2;
 aui->bits_card = 16;
 aui->card_dmasize = (unsigned long)(RING_BYTES / 2) * BYTES_PER_SBSAMPLE;
 m8_base = card->base;
}

static void MC8K_start(struct audioout_info_s *aui)
{
 mc8k_card_s *card = aui->card_private_data;
 ring_wr = ring_rd = 0;
 m8_base = card->base;
 m8_i8_install();
 rtc_enable();
}

static void MC8K_stop(struct audioout_info_s *aui)
{
 (void)aui;
 m8_voice_stop();
 m8_voice_on = 0;
 m8_pt_active = 0;
 m8_flush_gen++;
}

static void MC8K_close(struct audioout_info_s *aui)
{
 rtc_disable();
 m8_i8_remove();
 m8_voice_stop();
 m8_voice_on = 0;
 m8_dma_close(M8_WCH);
 // Refresh ch30/31 stay running (muted): stopping them evaporates DRAM and
 // a resident TDKSYN neither needs nor minds them.
 // v2.0: card_private_data is engine-owned (private_data_size); nothing to free here
}

// Render-path fallback (never the shipped path -- see /RESAMP note): engine
// 16-bit stereo -> mono16 into the host ring; the pump and voice downstream
// are identical. Since VSBHDA 2.0 AU_writedata advances card_dmalastput
// itself (see the sc_scp55 ENGINE WRITE POINTER note); the clamp here only
// guards the ring.
static void MC8K_writedata(struct audioout_info_s *aui, char *src, unsigned int bytes)
{
 short *p = (short *)src;
 unsigned long n, free_;
 unsigned wr;
 if(m8_pt_active) return;
 n = bytes / BYTES_PER_SBSAMPLE;
 free_ = (unsigned long)(((ring_rd - ring_wr - 1U) & RING_MASK) / 2U);
 if(n > free_) n = free_;
 wr = ring_wr;
 while(n--){
  int l = (int)p[0], r = (int)p[1];
  p += 2;
  if(!m8_st) l = (l >> 1) + (r >> 1);
  ring_buf[wr] = (unsigned char)(l & 0xFF);        wr = (wr+1)&RING_MASK;
  ring_buf[wr] = (unsigned char)((l >> 8) & 0xFF); wr = (wr+1)&RING_MASK;
  if(m8_st){
   ring_buf[wr] = (unsigned char)(r & 0xFF);        wr = (wr+1)&RING_MASK;
   ring_buf[wr] = (unsigned char)((r >> 8) & 0xFF); wr = (wr+1)&RING_MASK;
  }
 }
 ring_wr = wr;   // v2.0: AU_writedata advances card_dmalastput itself
}

static unsigned int MC8K_getbufpos(struct audioout_info_s *aui)
{
 (void)aui;
 return (unsigned int)((unsigned long)((ring_rd >> 1) & ((RING_BYTES / 2U) - 1U)) * BYTES_PER_SBSAMPLE);
}

static int MC8K_irq(struct audioout_info_s *aui)
{
 (void)aui;
 LOW_PokeB(0x4F8, ++m8_tel_irq);
 ++m8_tick_seq;
 m8_watchdog();
 { uint8_t f = DPMI_DisableInterrupt();
   outportb(0x70,0x0C); (void)inportb(0x71);
   DPMI_RestoreInterrupt(f); }
 m8_pump();
 LOW_PokeB(0x4F5, (unsigned char)(((ring_wr - ring_rd) & RING_MASK) >> 5));

 // Idle: unlike a codec FIFO, the ring voice CONSUMES whether or not we
 // feed -- a 32 Hz pump cannot keep silence ahead of it, so idling means
 // STOPPING the voice, and the next feed's stream_arm restarts it (~0.7 ms,
 // paid once per silence gap, not per SFX -- DSP resets keep it running).
 if(m8_adaptive && m8_pt_ever){
  unsigned used = (ring_wr - ring_rd) & RING_MASK;
  uint32_t gap = m8_tick_seq - m8_feed_seq;
  // ~6 s, NOT the codec backends' ~0.7 s. Idling stops the voice, and its
  // restart on the next sound is not seamless = a tick. Under a resident
  // MIDI stack (TDKSYN+MPUSHIM) the added delivery jitter pushed DOOM's
  // between-SFX gaps past a short window, so the voice cycled every few
  // seconds (235, 2026-08-30). Keeping it alive through multi-second gaps
  // (the pump just feeds silence at the stream rate) costs idle CPU we have
  // during quiet, and only a genuine lull (menus) now parks it.
  uint32_t lim = M8_RTC_HZ() * 6UL;
  if(gap >= lim && !used){
   if(m8_voice_on){ m8_voice_stop(); m8_voice_on = 0; }
   m8_pt_active = 0;
   if(m8_rtc_rs != M8_RS_IDLE) m8_rtc_setrate(M8_RS_IDLE);
  }
 }
 return 1;
}

struct sndcard_info_s MC8K_sndcard_info={
 // Name the silicon, matching the house convention (ES1688/CS4231A/CS4248).
 // Display only -- /CARD:MC8K is matched by PTOPS_CardIs, not by this.
 "EMU8200",
 0,
 &MC8K_adetect,
 &MC8K_start, &MC8K_stop, &MC8K_close,
 &MC8K_setrate,
 &MC8K_writedata, &MC8K_getbufpos,
 &MC8K_irq,
 NULL, NULL, NULL,
 sizeof(mc8k_card_s)   // private_data_size: the engine allocates it (v2.0)
};

#endif // NOMC8K
