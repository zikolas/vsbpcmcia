# VSBPCMCIA internals

## Why this exists

The target laptops' PCMCIA bridges have **no ISA DMA to the socket**, so an
ES1688-class card cannot service DMA-driven SB playback the normal way.
VSBPCMCIA runs VSBHDA's SB emulation (port traps + virtual DSP/DMA/IRQ) and
delivers the guest's audio to the real chip by **PIO passthrough**: the raw
guest PCM is tapped before any conversion and fed to the chip's 256-byte
extended-mode FIFO, which the chip clocks itself at the programmed rate.
No per-sample interrupts, no resampling. FM is not emulated: with `NOFM`,
guest AdLib I/O at 0x388 goes untrapped to the card's real ESFM.

## Data path

    guest DMA buffer
      -> sndisr.c render loop (VSB/VDMA bookkeeping, guest IRQ injection)
      -> passthrough tap (raw bytes, before cv_* conversions)
      -> ES1688_PT_Feed -> 8KB ring -> es_fifo_pump -> chip FIFO (base+0xF)

* **Pacing**: guest-DMA consumption per tick is capped by ring space up to a
  latency target (`SBEPTLAT`, default 250ms). Ring full = stop consuming =
  the guest throttles exactly as against a real SB. `AU_cardbuf_space` is
  meaningless for this driver (card_dmalastput never advances) and is bypassed.
* **Clock**: the RTC (IRQ8, `card_irq=8`) drives SNDISR. The pump self-paces:
  feed-forward from the stream byte rate (`es_rs_want`), feedback ratchet when
  the ring runs low, 32Hz idle floor when a stream is provably dead, and
  PT_Feed restores the stream rate on the first feed after an idle throttle.
* **Survival**: a chained IRQ0 (PIT) heartbeat hosts the watchdog -- guests
  that kill the RTC periodic (Theme Hospital does, ~10x/session) are healed
  within ~110ms. `0x4F4` counts revivals.
* **Reconfig**: full chip surgery (DSP reset + regs + FIFO prime) only on
  genuine format changes; same-format resumes fast-resume if the chip is
  provably draining (FIFO-half-empty seen within 4 BIOS ticks).
* **Ring ownership**: `ring_rd` is written ONLY by the pump; trap-context code
  requests flushes via a generation counter (never writes the pointers).

## SNDISR reentrancy (the founding bug)

VSBHDA runs its sound ISR with interrupts on (`SETIF=1`); each nested entry
carves 4KB (`STACKCORR`) off a private stack. The original port ran the whole
render tail on passthrough data and outlasted the RTC period every tick --
runaway nesting marched the stack into `.data` (#GP). Fixed by making PT
ticks cheap (skip conversions/mixer/writedata when the tap consumed; idle
ticks exit early). `STACKCHECK=1` in stackisr.asm trips fatal_error(3) at
16 nested levels as a tripwire; a render guard skips re-entrant render passes
(`0x4F1` counts -- should stay 0).

## Telemetry (BIOS scratch 0x4F0-0x4FF)

Cleared before a test with 16 zero bytes. All counters wrap.

| Addr | Meaning |
|------|---------|
| 0x4F0 | max SNDISR nesting depth seen (goal: 1) |
| 0x4F1 | render-guard skips (re-entered while rendering; goal: 0) |
| 0x4F2 | VEW211 build: guest rate >> 8 at each full reconfig (pitch forensics: 11025→43, 22050→86, a bogus 2x 43478→169); ES build: PT_Feed stage (1 entry, 2 reconfig, 3 ring fill, 4 pump, 5 done) |
| 0x4F3 | PT_Feed busy-guard skips |
| 0x4F4 | RTC revivals by the watchdog (VEW211 build: verified re-arms only -- PIE re-set, PIC unmask, or the seconds-confirmed reg-C wedge heal) |
| 0x4F5 | ring fill, 32-byte units |
| 0x4F6 | VEW211 build: SER catch-up count (codec-starvation refills -- pump ticks were lost to the guest; sustained growth in-game = tick loss, benign at idle); ES build: 0xAA once ES1688_start ran |
| 0x4F7/0x4F9 | SNDISR tick counter, 16-bit lo/hi |
| 0x4F8 | ES1688_irq calls (8-bit; tracks 0x4F7 lo) |
| 0x4FA | FULL chip reconfigs (fast-resumes not counted) |
| 0x4FB/0x4FF | PT_Feed calls, 16-bit lo/hi |
| 0x4FC/0x4FD | TSC boxes: longest outermost ISR pass, 256-cycle units; no-TSC boxes: PT bytes >> 4, 16-bit |
| 0x4FE | PT_Feed ring-overfeed clamps (goal: 0) |

Rate measurements: read 0x46C (BIOS tick dword) and the counters in ONE
mem_read (0x46C, 148 bytes spans both) and clock deltas against the BIOS
tick -- wall-clock between tool calls is unreliable.

**In-game caveats (measured on the 235, 2026-07-26):** games own the timer
chain -- Lion King hooks INT8 without chaining, freezing 0x46C solid, and
fast-timer games advance it several times too fast, so in-game deltas
against 0x46C are meaningless. Lion King also SCRIBBLES the 0x4F0-0x4FF IAC
area itself (it is a shared inter-application scratch), so telemetry read
mid-LK is garbage. Read telemetry at the DOS prompt right after game exit
instead (the scratch survives). This is also why the VEW211 driver keeps
all its timing in its own RTC tick counter, never 0x46C.

## Card backends

Two compile-time-exclusive backends share one passthrough ABI (the
`ES1688_PT_*` symbols -- historical names, card-agnostic):

* **ES1688** (default; `vsbpcm.exe`): Ratoc REX-5571/5572, Panasonic
  KXL-C101. 256-byte chip FIFO, FIFO-half-empty-paced feed, enabler ES1688GO.
* **CS4231A** (`CARD=VEW211 tools/build.sh` -> `vsbpcmv.exe`): Panasonic
  CF-VEW211/212, PC-9801N-J04. 16-sample FIFO, RTC-tick-credit-paced PIO
  (PRDY is unreliable on this card; PIT ch0 is guest hardware -- games
  reprogram its mode/reload, which corrupted a PIT-side elapsed-time
  accumulator into burst overfeed = fast/pitched-up/crackling playback, the
  2026-07-26 field bug), ms-paced verified MCE bring-up, codec
  at window base+4, discrete YMF262 for FM (native 4-port 0x388 decode --
  NOFM passthrough needs no enabler window tricks). Enabler VEW21XGO
  (github.com/zikolas/vew21xgo); see deploy/GO-VEW211.BAT. 22.05 kHz design
  ceiling. Ported from the rex5571-sbemu vew211-backend branch; carries the
  same session machinery as the ES1688 backend (flush-generation ring
  ownership, rs_want pump-restore, fast-resume on same-format re-arms, IRQ0
  heartbeat, runtime TSC probe, telemetry map).

## Build

`tools/build.sh` (Linux container; DJGPP cross + JWasm); `CARD=VEW211` selects the CS4231A backend. The tree is
case-normalized for case-sensitive filesystems. Always clean-builds.
CPU target is i486; the TSC duration probe is runtime-gated (EFLAGS.ID ->
CPUID -> TSC), so one binary serves 486s and Pentiums. Upstream's `/SD`
option still executes rdtsc unconditionally -- don't use it on a 486.

## Deploy

See `deploy/GO.BAT` for the proven order: JEMM386 (NOEMS + attribute-window
exclusion) -> ES1688GO (real chip 0x220, FM, window) -> env -> JLOAD
QPIEMU.DLL -> HDPMI32i 3.21+ (-r -x) -> VSBPCM /A240. Real chip and emulated
SB must be at different bases. To replace a resident VSBPCM: reboot. (The
DSP-reset 0x55 uninstall backdoor runs in the caller's context -- triggering
it from a remote-control agent kills the agent.)

## Open issues

* **Sam & Max (talkie) faults under HDPMI** -- not a VSBPCMCIA bug: SETMUSE
  and the game both die with DOS/4GW error 2001 / exception 0Eh at
  25F:00438A89 (EAX=90641BE0 wild pointer, deterministic, identical
  registers every run) with ONLY Jemm+QPIEMU+HDPMI32i loaded, no VSBPCM.
  Swapping the extender for DOS32A faults identically; running with
  `HDPMI32I -d` (DPMI refused -> VCPI fallback) works -- the host is the
  common factor. Unaffected by HDPMI -a / -x5 / -n. iMUSE driver init does
  something HDPMI mishandles. Candidate upstream report for
  Baron-von-Riedesel (HX). Workaround: none with sound (VCPI clients are
  untappable); on the PC110 the game runs bare on the internal ES488.
* **SOLVED (2026-07-24): game-native ESFM silence** was ES1688GO's 2-port
  FM window (CIS-literal 388-389): the ESFM native-mode enable lives on the
  OPL3 secondary pair at 38A/38B and never reached the chip -- AdLib worked,
  ESFMPLAY (quad at the SB base) worked, every game's ESFM mode was silent.
  Fixed in ES1688GO 1.4 (4-port FM window; CS falls back to 2 if refused).
  Warcraft 2 'ESFM Enhanced' verified on cold boot through the full stack.
  Related guidance: ESS-aware FM apps that read BLASTER (e.g. ESFMPLAY)
  should be pointed at the REAL chip base (SET BLASTER=A220...) for the
  probe; the emulated base has no FM.
* **Theme Hospital (demo) plonk-hiss**: staff-placement sounds
  (pause -> single-cycle -> resume on the active Miles stream) latch a
  constant hiss. Measured: our delivered stream is byte-identical to the
  clean state -- the noise is mixed by the guest. Suspected Miles
  buffer-half desync from emulated IRQ cadence around 0xD0/0xD4 pause/resume
  (vsb.c). Clears on stream restart. Full version of TH untested.
* **Direct-DAC under passthrough** uses the render path with a poor rate
  estimate (pre-existing; SC2000 uses DSP 0x14, not direct-DAC).
* **ADPCM** (<8-bit) falls back to the full render path, paced by the broken
  AU sawtooth (rare in practice).
* **SBPro stereo-bit rate cache**: toggling the mixer stereo bit did not
  invalidate vsb.SampleRate (CalcSampleRate divides by channels) -- a 2x rate
  skew for guests that toggle stereo without resending the time constant.
  Hit in the field 2026-07-26 (games fast + pitched up). FIXED on the
  vew211-backend branch: stereo changes (mixer 0x0E write, ADPCM/silence
  cmds forcing mono) invalidate the cache on pre-SB16 DSP versions. The fix
  is `#ifdef CARD_VEW211`-guarded ONLY so the default ES build stays
  byte-identical during bench testing -- UNGUARD AT MERGE (the ES1688 build
  shares the bug).
