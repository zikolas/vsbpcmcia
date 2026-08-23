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
  2026-07-26 field bug). The codec has 14 fixed crystal-divided rates; a
  guest rate missing the table by >2% engages a nearest-neighbour FRAME
  STEPPER in PT_Feed (Bresenham drop/dup of whole frames during the ring
  copy -- no interpolation, 486-priced; `SBENORS=1` disables it for A/B).
  Table picks cap at the 22.05k design ceiling, so a 44.1k guest decimates
  2:1 to correct pitch/tempo at half bandwidth. Exact/near-table streams
  keep the raw untouched path. ms-paced verified MCE bring-up, codec
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
* **CS4231A fade dropouts are FIFO physics (closed 2026-07-26).** Some
  games black out ALL interrupts for several ms at a time (Lion King's
  screen fades, measured: SER starvation events fire in volume while the
  guest-DSP-reset counter stays flat -- no reset storm, nothing to feed
  with). The ES1688 rides the identical blackouts on its 256-BYTE chip
  FIFO (~12 ms of cushion at 21 kHz); the CS4231A holds 16 SAMPLES
  (~0.8 ms) and punctures. No driver can extend a hardware FIFO and the
  PCMCIA bridge has no DMA. Verdict: fade-heavy titles sound best on the
  ES1688-family cards; the VEW211 keeps native OPL3 + stereo + the frame
  stepper's correct pitch everywhere else. (Two mitigation experiments were
  field-WITHDRAWN the same day after a long session ended in crackle then
  total silence: pumping from the IRQ0 heartbeat -- heavy work on a
  borrowed, possibly slim guest ISR stack -- and a MODE2/DACZ underrun-
  silence poke, an unverified write on a codec known to drop hasty writes.
  The wedge state was lost to a reboot, so blame is split; neither had
  shown audible benefit.)
* **SBPro stereo-bit rate cache**: toggling the mixer stereo bit did not
  invalidate vsb.SampleRate (CalcSampleRate divides by channels) -- a 2x rate
  skew for guests that toggle stereo without resending the time constant.
  Hit in the field 2026-07-26 (games fast + pitched up). FIXED on the
  vew211-backend branch: stereo changes (mixer 0x0E write, ADPCM/silence
  cmds forcing mono) invalidate the cache on pre-SB16 DSP versions. The fix
  is `#ifdef CARD_VEW211`-guarded ONLY so the default ES build stays
  byte-identical during bench testing -- UNGUARD AT MERGE (the ES1688 build
  shares the bug).

## Short one-shot SFX play stretched (the tap loop is NOT the culprit)

Duke Nukem II's intro SFX arrive as a stream of ~12-byte 8-bit single-cycle
blocks (DSP `0x14`), each ended by a TC-IRQ that prompts the guest to program
the next. Under passthrough they play stretched. The standing theory was that
`sndisr.c`'s tap loop "breaks after one SB block" and the fix was to let it
iterate until `pt_space` is spent. **Reading the code says that theory is
wrong, and the proposed change would be a no-op.**

The loop tail already continues on a completed block:

```c
if( VSB_GetIRQStatus() ) {
    if ( VSB_IsAuto() ) VSB_SetPos(0); else VSB_Stop();
    if ( !SNDISR_ReviveSquelch ) VIRQ_Invoke();   /* <- guest ISR runs HERE */
} else break;                                     /* only a PARTIAL block exits */
```

and `VIRQ_Invoke()` is **synchronous**: `SBIsrCall` in `sbisr.asm` does a plain
`int 8+irq`, so the guest's SB ISR runs to completion inside our tap loop. A
DSP play command issued from that ISR lands in `DSP_DoCommand` and sets
`vsb.Started = true` with no "we are inside the ISR" guard, so `VSB_Running()`
in the `for` condition is true again and the loop carries straight on to the
next block. Nothing else binds it either: `samples` is `PT_MODE_SAMPLES` (1024)
against ~12 guest samples per block, and `pt_space` at the default 250 ms
latency target is ~5.5 KB against the same 12 bytes -- which is also why the
SBEPTLAT ladder produced byte-identical results.

So the loop stops for exactly one reason: **the guest did not re-arm inside its
own ISR**, and the next block cannot arrive until its main loop runs, which
cannot happen until our ISR returns. That makes playback speed one block per
RTC tick -- the same failure class as SimCity 2000's 4-byte torrent.

The rate arithmetic fits: `vew_rs_for_frate()` picks the pump rate from the
codec rate, so `/DACRATE11025` gives rs=6 = **1024 Hz** (12288 B/s delivered)
and `/DACRATE22050` gives rs=5 = **2048 Hz** (24576 B/s) against a 21376 Hz
guest that wants 21376 B/s. That is why `/DACRATE22050` helped -- it crossed
from below demand to 15% above it -- and why it did not cure: at 15% margin
every lost tick is an audible gap.

### Measuring it: PTDIAG + TEST05

`PTDIAG` (`src/ptops.h`) builds the tap forensics into `sndisr.c` and silences
the two `sc_vew211.c` pokes whose slots it borrows:

| slot | meaning |
|---|---|
| 0x4F2 | most guest DMA blocks consumed in ONE tick. **1 = never more than one** |
| 0x4FA | bitmap of loop-exit reasons: 01 guest never re-armed, 02 sample bound, 04 ring full (correct backpressure), 08 partial block, 10 a tick took 2+ blocks |

`test/test05.asm` (derived from upstream's TEST01) reproduces the block pattern
without the game, in 3 KB -- so it runs with comrade resident and the whole
measurement is scriptable, instead of needing Duke's 560 K and a human at the
keyboard:

```
TEST05 [blocksize] [rate] [mode] [seconds]      ; defaults 12 21376 0 5
  mode 0 = re-arm inside the SB ISR   (the tap loop CAN chase this)
  mode 1 = re-arm from the main loop  (it cannot -- the worst case)
```

It reports achieved bytes/sec and a stretch factor x100, so `100` means the
engine keeps up. Mode 0 vs mode 1 is the decisive pair: if mode 0 reaches ~100
while mode 1 stretches, the engine is fine and the guest's re-arm placement is
the whole story -- which rules the tap loop out for good and points at
servicing the block at trap time (SimCity 2000's "option A") as the only fix
that does not need a faster tick. `SBERTC=5` is not an option: it hard-wedges
the 486.

### First bench result: an in-ISR re-arm WEDGES the box

`TEST05 12 21376 0 5 7` -- the mode where the guest re-arms inside its own SB
ISR -- hard-hung the T2130CT within seconds (comrade stopped answering
entirely; physical power cycle required). Mode 1 has not been run yet, so this
is not yet isolated from a bug in the test program -- **run mode 1 first.**

The mechanism that fits: when the guest *does* re-arm in-ISR, the tap loop
iterates freely, bounded only by `pt_space` and `PT_MODE_SAMPLES`. At
SBEPTLAT=60 that is ~1.3 KB, i.e. **~85-110 twelve-byte blocks in ONE tick**,
each costing a full `VIRQ_Invoke` round trip plus ~10 trapped I/O ops. That is
milliseconds inside a 0.49 ms tick period at 2048 Hz; SETIF lets the following
ticks nest, and each nested `SwitchStackISR` carving takes STACKCORR off the
private ISR stack -- the founding bug's march into `.data`.

So "let the tap loop iterate until `pt_space` is spent" is not merely a no-op,
it is **the hazard**. `pt_space` is sized by the ring's *latency target*, not by
what one tick can *drain*, so it licenses a single tick to swallow roughly 120
ticks' worth of audio. Anything built here needs a **blocks-per-tick cap**.

`SNDISR_PtBlkCap` (env `SBEPTBLK`, PTDIAG builds, default 8, 0 = uncapped) is
that cap: it stops the tick *after* the completion IRQ has been delivered, so
the guest is simply throttled to the next tick exactly as a real SB would
throttle it. `0x4FA` bit `20` records when it fired.

### The number that does not add up yet

At `/DACRATE22050` the picker gives rs=5 = **2048 Hz**, so even at today's
~1 block/tick the tap delivers 2048 x 12 = **24576 B/s against Duke's 21376
B/s** -- 15% *more* than demand. Duke should not stretch at all, and it does.
Either the pump is not really at 2048 Hz during the game, ticks are being lost,
or the guest's own re-arm latency exceeds a tick. **Measure the live tick rate
before designing a fix**: read the 16-bit counter (`0x4F7` lo / `0x4F9` hi)
twice a known interval apart *while a sound plays* -- it wraps every 32 s at
2048 Hz, and idle throttles to 32 Hz so an idle read tells you nothing.
