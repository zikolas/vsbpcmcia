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

The per-tick bytes (0x4F0, 0x4F5, 0x4F7/0x4F9, 0x4F8, 0x4FB/0x4FF, the
no-TSC 0x4FC/0x4FD, the ES build's 0x4F2 stage) sit behind `SNDISR_TELEMETRY`
(src/ptops.h, default 1). `RELEASE=1 tools/build.sh` sets it to 0 together
with `PTDIAG 0`: a dozen far stores per pump tick is real 486 time, and a
release never reads them. The rare-event bytes stay in every build.


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
| 0x4FA | FULL chip reconfigs (fast-resumes not counted). Under `RATEDIAG`: the direct-DAC inferred rate, see below |
| 0x4FB/0x4FF | PT_Feed calls, 16-bit lo/hi |
| 0x4FC/0x4FD | TSC boxes: longest outermost ISR pass, 256-cycle units; no-TSC boxes: PT bytes >> 4, 16-bit |
| 0x4FE | PT_Feed ring-overfeed clamps (goal: 0) |

IBMAUD (sc_ibmaud.c's header has its full map): 0x4F4 RTC revivals, 0x4F6
codec links lost, 0x4FA arms, 0x4FE feed frames dropped for want of room.
In builds without `SNDISR_TELEMETRY` two per-tick bytes change jobs: 0x4F5
counts silence padded into a live tap stream (a gap in the audio), 0x4F8
play-position reads refused as mid-step reads (goal for both: 0).

TP755 (sc_tp755.c's header has its full map; its own IAC instrument, 0x4F0-0x4FD,
in every build). In tap mode two more: 0x4FE silence padded into a live tap
stream (a gap the guest left), 0x4FF steps where the 8237 overtook the tap's
writer and the ring replayed stale audio (goal for both: 0).

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
  estimate on cards without a `clock` op (pre-existing; SC2000 uses DSP
  0x14, not direct-DAC). IBMAUD has one: there `SNDISR_DacFeed` measures the
  guest's rate against the card's play position and feeds the tap at it
  (see "VSBPCMJ and the direct-DAC ring" below).
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

### Measuring it: PTDIAG + PTBLKS (was TEST05)

`PTDIAG` (`src/ptops.h`) builds the tap forensics into `sndisr.c` and silences
the two `sc_vew211.c` pokes whose slots it borrows:

| slot | meaning |
|---|---|
| 0x4F2 | most guest DMA blocks consumed in ONE tick. **1 = never more than one** |
| 0x4FA | bitmap of loop-exit reasons: 01 guest never re-armed, 02 sample bound, 04 ring full (correct backpressure), 08 partial block, 10 a tick took 2+ blocks |

`RATEDIAG` (`src/ptops.h`) measures what the guest actually asks of the DSP
rate path, and silences every other writer of the slots it borrows -- the
`sc_es1688.c` stage markers, `sndisr.c`'s PTDIAG block counter and exit
bitmap, and `sc_es1688.c`'s reconfig count:

| slot | meaning |
|---|---|
| 0x4F2 | rate >> 8 as computed from the guest's time constant (169=43478, 88=22727, 43=11025) |
| 0x4F3 | the raw time constant the guest wrote |
| 0x4F6 | sticky OR: 01 computed in high-speed, 02 computed out of it, 04 the ceiling clamped the value, 08 stereo at compute time |
| 0x4FA | direct-DAC (DSP cmd 0x10) inferred rate >> 8; **0 = that path has not run** |

0x4FA exists because the other three cannot see direct-DAC at all. Cmd 0x10
does not set `vsb.Started`, so `VSB_Running()` is false, `sndisr.c`'s block
loop never entered, and `VSB_GetSampleRate()` -- its only route into
`CalcSampleRate`, where 0x4F2/0x4F3/0x4F6 are written -- never called. Through
an entire direct-DAC session those three hold whatever the game's SB detection
left there at startup, which reads exactly like a live measurement. Compare
0x4FA against 0x4F2 directly: same units, different derivation (0x4FA is
`IdxSm * freq / samples`, an inference from how many samples arrived per tick,
not a time constant).

`Test/PTBLKS.ASM` (derived from upstream's TEST01; it was `test/test05.asm` until the VSBHDA 2.0 merge, whose own TEST05 is an FM test) reproduces the block pattern
without the game, in 3 KB -- so it runs with comrade resident and the whole
measurement is scriptable, instead of needing Duke's 560 K and a human at the
keyboard:

```
PTBLKS [blocksize] [rate] [mode] [seconds]      ; defaults 12 21376 0 5
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

`PTBLKS 12 21376 0 5 7` -- the mode where the guest re-arms inside its own SB
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

## The render tail, and the formats that reach it (`Test/PTBYPASS.ASM`, was `test/test06.asm`)

`sndisr.c` gates the passthrough tap on

```c
pt_block = pt_mode && VSB_GetBits() >= 8;
```

so **ADPCM** (2/3/4 bits) fails it, and **direct-DAC** (DSP cmd 10h) never sets
`vsb.Started` at all -- `VSB_Running()` is false, the block loop never runs.
Both therefore leave `pt_took` at 0, the ISR does not take its `goto isrexit`
shortcut, and the whole render tail executes: `DecodeADPCM`, `cv_rate`, the
silence `memset`, the volume pass, `AU_writedata`.

That tail is the least-exercised code in the driver and TEST01..TEST05 cannot
reach it -- they are all 8-bit or wider, so they all take the tap. PTBYPASS
drives both formats:

    PTBYPASS [mode] [rate] [seconds] [blocksize] [irq]
      mode 0   4-bit ADPCM single-cycle (DSP 75h), DMA + SB IRQ
      mode 1   direct-DAC (DSP 10h), no DMA, no IRQ, tick-paced

Mode 0 is the better coverage run: it reaches `DecodeADPCM` *and* `cv_rate`,
i.e. both users of the ISR scratch buffer. Mode 1 is the minimal reproducer
for the wedge below.

**It doubles as a wedge reproducer.** On a passthrough backend `samples` is
forced to `PT_MODE_SAMPLES` (1024) whatever the guest supplied, and
`sc_es1688`, `sc_vew211` and `sc_scp55` all register `NULL` for the ops
table's `depth` hook -- so sndisr's nesting limiter

```c
if ( PT_Ops->depth && PT_Ops->depth() > 3 ) goto isrexit;
```

short-circuits and never runs on those three. Watch while it runs:

| slot | meaning |
|---|---|
| 0x4F0 | max SNDISR nesting depth. **Healthy = 1**; climbing = the tail is overrunning its tick |
| 0x4F1 | render-guard skips. **Healthy = 0**; nonzero confirms re-entry |
| 0x4FA | RATEDIAG builds, mode 1: direct-DAC inferred rate >> 8, should track the `rate` argument |

On an SB-compatible card this is a driver-only concern -- a direct-DAC guest
needs no DMA, so it can simply talk to the real chip with the enabler alone
and no vsbpcm loaded. On the CS4231A, CS4248 and EMU8200 cards there is no SB
silicon to fall back to, so the render tail is the only path and this matters.

## VSBPCMJ and the direct-DAC ring

`jlm/VSBPCMJ.ASM` is a Jemm loadable module that serves the dense V86 ports
at ring 0 (README, "VSBPCMJ.DLL"). It shares one block of DOS memory with
VSBPCM (`src/JLMSHARE.H`, layout version 6); the block's segment is on
VSBPCM's load line. Without the JLM the same struct is a static in `ptrap.c`
and carries only the direct-DAC ring, which vsb.c fills for protected-mode
and QPI-trapped guests.

| offset | written by | meaning |
|---|---|---|
| 00-17 | VSBPCM | signature `VSBJ`, layout version, ring size, LPT, flags, SB base, stub entry, write-status cell |
| 18, 19 | vsb.c, JLM | the next DSP write is a command byte; a 10h was taken and its sample is next |
| 1A-1F | JLM, sndisr.c | ring producer and consumer index; the rate direct DAC is fed at (Hz) |
| 20-3F | JLM | DAC samples received and lost (ring full); FM writes, status reads, delay reads patched; DSP writes passed to the stub; write-status reads; its version once armed |
| 40-57 | JLM | the last write-status poll and the last DAC write: linear address and 8 code bytes |
| 58-77 | sndisr.c | drains, samples fed, flushed at stream end, longest time between drains, drains cut by card room, drains held for a first estimate, ring peak, streams, rate changes, sample-less ticks |
| 78-7C | sndisr.c, ptrap.c | sound interrupts counted; pump guard on |
| 80-8B | JLM | RTC periodic interrupt switched back on, IRQ8 unmasked, stuck flags cleared; write-status polls patched |
| 8C-9F | ptrap.c, JLM | the stub's PIC word (FFFFh while an SB interrupt is emulated); EOIs done at ring 0; 10h-and-sample pairs made one fault; samples put by the V86 producer; sequences made calls to it |
| A0 | both | the ring, 2048 samples |
| 8A0 | JLM | its copy of the V86 ring producer (16-bit code, called with the sample in AH) |
| 900-90D | sndisr.c | lowest and highest one-second arrival rate (Hz); most card room left unfilled while samples arrived; the fill trim now; gaps filled with a held sample; the fill trim's lowest and highest (Hz) |
| 90E-91B | JLM, ptrap.c | FM ring producer and consumer index, entries (0 = none), writes dropped with the ring full, most entries waiting at a drain |
| 91C | JLM | the FM ring (software-OPL3 builds only): dwords, value \| index << 8 \| register array << 16 |

The rate measurement (`SNDISR_DacFeed`) counts samples received per unit of
the card's `clock` op: in 1/16 s windows, folded into a running total that
remembers about 16 s, checked once a second for a guest that changed rate.
The first tick after a pause only syncs, a gap under 1/8 s counts as part of
the stream, and half a second without a sample ends it. IBMAUD's clock is
the card's play position while it plays and RTC ticks while it is closed.

In the builds with the software OPL3 (VSBPCMT, VSBPCMA: no `NOFM`), the JLM
takes the V86 FM ports too when the emulation is on (`VSBJ_F_OPL`). Status
comes from its timer model, as for the shim, delay reads are patched, and
each data write goes into the FM ring with the index latched for its array.
`PTRAP_DrainJlmFm` replays the ring into vopl3 at the top of each sound
interrupt, before the render, and before any FM access that still comes to
VSBPCM (a protected-mode guest's), so the order holds. It takes 32 entries
per interrupts-off window. The TP755 stub's own ring covered array 0 only
and sent timer writes, array 1 and the SB aliases through an RMCB; the JLM
takes all of them. The ring is 512 entries (2 KB) and exists only in those
builds, so the others keep the smaller block.

The tap is fed at that estimate with a fill trim. e = samples waiting in the
ring minus the card's room below its target, in units of 1/12 s of samples;
the trim is 0.3 of a negative e (the card short: an underrun coming) or 0.05
of a positive one (only latency), plus an integral of 1/1024 of e per tick,
within 12%. The integral stands still while the direct part alone is at the
limit, and nothing is trimmed in a stream's first half second. A look that
finds no samples marks a gap; when samples resume and the card is more than
1/48 s short, the shortfall is filled with the first new sample held, as a
real SB holds its DAC through the guest's pause. IBMAUD's stepper takes each
rate change without a click (accumulator rescaled, previous frame kept).

Test/JLMTEST.ASM (run it from V86 with VSBPCM loaded): `JLMTEST S` prints the
block; `JLMTEST` alone also runs AdLib detection, FM writes with their delay
reads and direct-DAC samples, and the counters again; `JLMTEST P [hz]` is the
busy-loop cost of a fast PIT (bare ISR, Another World's DAC ISR, and three
PUSHF/CLI/POPF); `JLMTEST T` reads IBMAUD's play position back to back;
`JLMTEST W [s] [us]` plays a 440 Hz tone by direct DAC in Another World's
code shape, optionally blocking interrupts for [us] every other BIOS tick.

Bench, Another World on the T2130CT (IBMAUD, DX4/75, 2026-10-02):

* The game's IRQ0 handler mixes four channels and writes one sample per PIT
  tick, polling the write status before the 10h and before the sample
  (`IN AL,DX / OR AL,AL / JS` back): four QPI traps per sample, unplayable.
  With VSBPCMJ the polls are patched, the 10h and sample pair is made one
  fault, then the whole sequence a call into the ring producer: no traps.
* IRQ0 heartbeat: 53 us per 10 kHz PIT tick for the stack with it, and the
  RTC pump was found switched off 20 times in one run. The pump guard took
  its place. JLMTEST P now: 21.9 us a bare tick (the EOI), 35.4 us in
  Another World's shape (the sample and the card feed), 9.0 under Jemm
  alone. PUSHF/CLI/POPF cost 0.13 us each, so the CPU's VME is in use.
* IBMAUD's play position (346h) can be read mid-step: a read that lands on
  the count's increment returns the new high bits over the old low ones
  (2C77h, 2C7Fh, 2C78h; up to 2^k - 1 ahead on a carry into bit k), about
  one read in 14000 back to back (`JLMTEST T`). ib_track took the step back
  to the true count as 16K words played, its clamp emptied the queue, and
  the pump padded 40 ms of silence into a full card every 20-40 s; the fill
  trim then pitched the music down to win back a shortfall that was never
  there. A position is now two reads that agree, and a step longer than the
  queue is refused for 16 ticks (IAC 0x4F8 counts refusals in builds without
  SNDISR_TELEMETRY). Every IBMAUD stream had the same exposure.
* IBMAUD's stepper averaged guest frames whenever the guest rate was above
  the codec's. Near 1:1 that averages two frames now and then: a frame
  dropped every few hundred, a grit. Direct DAC trimmed past 11025 Hz did
  it, and an SB time constant of 166 (11111 Hz) would too. It averages only
  above 3:2 now and interpolates below.
* Result: no silence padded into live audio, no position reads refused, the
  card never more than 9 ms short of its target, the trim within -1.75% and
  +2.7%. A crackle remains in the heaviest scenes: the game's own mixer short
  of CPU (its last 0.2 s of samples hold no repeated or stuck runs, and
  `JLMTEST W` stays clean with interrupts held off 3 ms in every 110).

Next, by expected gain for a 486: the EOI trap (13 us a tick) only while an
SB interrupt is emulated; FM for the software-OPL builds (VSBPCMT, VSBPCMA),
whose V86 FM still pays one QPI nested execution per access (41 per
register write with the AdLib padding): the JLM could write the register
pairs into the OPL ring rmcode1.asm already fills for `PTRAP_DrainOplRing`,
answer status from its timer model, and patch the delay reads.
