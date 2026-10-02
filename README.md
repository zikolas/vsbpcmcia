# VSBPCMCIA

Sound Blaster emulation for DOS laptops whose sound card has no ISA DMA: 486
and Pentium-era PCMCIA cards, the ThinkPad 755C's planar codec, and a CardBus
Audigy. Games see a Sound Blaster at 220h; VSBPCM traps it and sends the audio
to the real card by programmed I/O.

It is a fork of Baron-von-Riedesel's VSBHDA
(https://github.com/Baron-von-Riedesel/VSBHDA), itself a fork of crazii's SBEMU
(https://github.com/crazii/SBEMU). It runs on unmodified HDPMI (v3.21+), so it
is compatible with HX. Tested from a Pentium MMX down to a 486SLC/25 on a
386 bus (HP OmniBook 425); COMPATIBILITY.md lists games and the slow-CPU floor.

Emulated: Sound Blaster 1.0, 2.0, Pro, Pro 2 and 16; 8 and 16-bit, mono and
stereo, high-speed DMA.

## Supported hardware

| Card | Enabler, run first | VSBPCM switches | FM music |
|------|--------------------|-----------------|----------|
| Ratoc REX-5571/5572, Panasonic KXL-C101 (ES1688) | [ES1688GO](https://github.com/zikolas/es1688go) `/SB=240 /FM /W=DC00` | `/CARD:ES1688 /BASE240` | the card's ESFM |
| Panasonic CF-VEW211 and CF-VEW212 (CS4231A) | [VEW21XGO](https://github.com/zikolas/vew21xgo) `/PCIC /IO=530 /VOL=0 /W=DC00` (2.5+ for the 212) | `/CARD:VEW211 /DACRATE11025 /CVOL0` | the card's YMF262 (211) or OPL4 (212) |
| Roland SCP-55 | [SCP55GO](https://github.com/zikolas/scp55-enabler) `/PCIC /I=0 /W=DC00` | `/CARD:SCP55 /CVOL0` | General MIDI on the card's Sound Canvas |
| IBM PCMCIA Audio Adapter (P/N 0933967) | [IBMAUDGO](https://github.com/zikolas/ibmaudgo) `/W=DC00` | `/CARD:IBMAUD` | OPL3LPT, or software OPL3 |
| TDK MusicCard MC-8000, DMC-9000 (EMU8200) | [MC8KGO](https://github.com/zikolas/mc8kgo) `/W=DC00` | `/CARD:MC8K` | General MIDI through TDKSYN |
| ThinkPad 755C planar CS4248 | none | `/CARD:TP755` | OPL3LPT, or software OPL3 |
| Audigy 2 ZS Notebook (CardBus) | [AUD2GO](https://github.com/zikolas/aud2go) | VSBPCMA `/CARD:AUDIGY` | software OPL3; General MIDI on the card's wavetable |

The binaries:

| File | What it is |
|------|------------|
| VSBPCM.EXE | every PCMCIA backend and the 755C; no software FM |
| VSBPCMT.EXE | the same, plus DOSBox's OPL3 emulation (needs an FPU) |
| VSBPCMA.EXE | the Audigy build, with software OPL3 and the wavetable |
| VSBPCM16.EXE | for 16-bit protected-mode games (Tyrian and the Borland RTM catalogue) |
| VSBPCMJ.DLL | optional Jemm module that speeds up real-mode FM and direct DAC |

## Requirements

* Jemm v5.84 or later (JEMM386 or JEMMEX), with JLOAD and QPIEMU.DLL from the
  same release zip: https://github.com/Baron-von-Riedesel/Jemm. Mixed
  releases refuse to load, lose real-mode support, or hang. Exclude the card
  window from Jemm's UMB scan (`X=DC00-EFFF`): a scan over a live PCMCIA or
  planar window hangs the machine.
* HDPMI32i v3.21 or later, the "i" variant, from HX
  (https://github.com/Baron-von-Riedesel/HX, BIN\HDPMI32i.EXE in the HXRT zip).
  v3.20 and older fail with "Failed installing IO port trap for
  protected-mode".
* The card's enabler, from the table above.

## Getting started

A launcher for an ES1688 card; the other cards swap the first and last lines
for the ones in the table:

    ES1688GO /SB=240 /FM /W=DC00
    SET BLASTER=A220 I7 D1 T4
    JLOAD QPIEMU.DLL
    HDPMI32I -r -x -v
    VSBPCM /CARD:ES1688 /BASE240 /A220

Jemm goes in CONFIG.SYS, or as `JEMM386.EXE LOAD NOEMS X=DC00-EFFF` at the top
of the batch. deploy/ has a working batch for each card, and `VSBPCM /?` prints
the switches with a recipe per card. Set games to Sound Blaster, 220h, IRQ 7,
DMA 1.

`/A` is the emulated SB, where games look; keep it at 220. `/BASE` is the
real card; it defaults to the enabler's default (220 ES1688, 250 IBMAUD, 330
SCP55, 530 VEW211, 4E30 TP755; MC8K tries 240, then 260). The ES1688 recipe
moves the card to 240 because a game that finds the real chip at 220 picks a
card with no DMA to the socket and goes silent.

## Switches

| Switch | Meaning |
|--------|---------|
| `/CARD:name` | backend, required: ES1688, VEW211, SCP55, MC8K, IBMAUD, TP755 (AUDIGY in VSBPCMA) |
| `/BASE` | real card's I/O base, hex |
| `/A /I /D /T /H` | emulated SB base, IRQ, DMA, type, high DMA (also read from BLASTER) |
| `/DACRATE` | codec rate in Hz. The codec's rate on VEW211, SCP55, IBMAUD and TP755; on the ES1688 only the idle rate, as the guest's format wins |
| `/MAXHZ` | codec rate ceiling, VEW211 and SCP55 (default 22050). Fewer pump interrupts on a slow host |
| `/CVOL` | codec attenuation 0-63, 1.5 dB steps (VEW211, SCP55, TP755) |
| `/RESAMP` | the engine resamples and mixes onto one codec rate (the render path) in place of the passthrough (VEW211, IBMAUD, TP755) |
| `/LPT` | FM to an OPL3LPT: `/LPT` (378), `/LPT278`, `/LPT3BC` |
| `/AUX[n]` | TP755: line-in jack on, n x 1.5 dB down (0-23, default 0) |
| `/FMVOL` | volume trim on a real OPL3, 0-63 TL steps |
| `/FMSHIM` | treat the card as FM-less (diagnostic) |
| `/OPL0` | VSBPCMT, VSBPCMA: software OPL3 off |
| `/PM0`, `/RM0` | leave protected-mode or real-mode games unserved |
| `/PS /B /BP /BS` | period size, period count, buffer guard, PCM buffer |
| `/CF` | VSBHDA compatibility flags |

Settings that persist belong in switches. The environment variables left are
for one experiment at a prompt:

| Variable | Effect |
|----------|--------|
| `SBEPTLAT` | passthrough queue target, ms |
| `SBEMAXHZ` | SCP55: codec rate cap. MC8K: guest rate cap, above which the feed decimates |
| `SBERTC` | pin the RTC pump rate select, 3-15 |
| `SBEFMPATCH=1` | rewrite protected-mode games' FM delay reads to `NOP` (with VSBPCMJ, real-mode games get this by default; `=0` turns it off) |
| `SBEIBMST=0`, `SBEIBM16=0/1` | IBMAUD: mono output; 8 or 16-bit output |
| `SBENOJLM=1`, `SBEDSPPATCH=0`, `SBEJLMPIC=0` | VSBPCMJ: leave it unarmed; leave game code unpatched; leave port 20h on QPI |
| `SBENOSTUB=1`, `SBELPTDLY`, `SBELPTCLI=0` | `/LPT` A/B: V86 stub off; settle reads per strobe (6); interrupts on in the stub |
| `SBENORS=1`, `ESNOI8`, `ESIRQ5`, `IRQTONE`, `FIFOTEST` | bench diagnostics: frame stepper off, IRQ0 heartbeat off, others in the source |

## Real-mode and protected-mode games

One resident serves both kinds of game, through the two port-trapping hosts
it loads under:

* Real-mode games (Monkey Island, Dune II, Another World) reach it through
  Jemm's QPIEMU. Each trapped port access costs a nested V86 execution, which
  VSBPCMJ.DLL (below) removes for the busiest ports.
* 32-bit protected-mode games (DOS/4GW and DJGPP titles: DOOM, Duke Nukem 3D,
  SimCity 2000, Quake) run as clients of the resident HDPMI32i, which traps
  their port accesses and calls VSBPCM in protected mode with no switch to
  real mode. With the FM shim, `SBEFMPATCH=1` removes their FM delay reads.
* 16-bit protected-mode games (Tyrian and the Borland RTM, Phar Lap 286 and
  DOS16M catalogue) get their own interrupt tables under DPMI 0.9, so they
  need VSBPCM16.EXE with `HDPMI16I -x` in place of `HDPMI32I` (see below).

`/PM0` or `/RM0` turns either world off. COMPATIBILITY.md lists what runs.

### VSBPCMJ.DLL

A Jemm module that serves the busiest real-mode ports at ring 0: FM while the
shim or the software OPL3 owns it, the DSP write port for direct DAC (command
10h), and port 20h. `JLOAD VSBPCMJ.DLL` after QPIEMU (the OPL3LPT launcher
below shows the order); VSBPCM arms it and its load line says what it serves.
On a DX4/75 it takes FM from 1,456 to 20,222 register writes a second and
direct DAC from 5,200 to 36,400 samples a second; with it Theme Hospital's
FM plays at normal speed and Another World in its normal audio mode. It
patches the game's code as it runs (FM delay reads and DSP busy-waits become
`NOP`); `SBEFMPATCH=0` and `SBEDSPPATCH=0` turn that off. It needs a 32-bit
build.

### VSBPCM16.EXE

Launch it in place of VSBPCM with `HDPMI16I -x` (deploy/go16*.bat); it serves
real-mode games as well. Run UNINST.EXE before switching between the 16-bit
and 32-bit builds, since neither detects the other. Verified on a ThinkPad
235 with a KXL-C101: Tyrian, sound and AdLib music, and Jazz Jackrabbit;
Tyrian's menu runs slightly slow (doc/16bit.md). `SBEFMPATCH` and VSBPCMJ
are 32-bit only.

## FM music

On a card with an FM chip (ES1688, VEW211, VEW212), 388h is left untrapped and
games use the chip directly.

On a card without one, games still run an AdLib timer test before they touch
the DSP, so VSBPCM answers 388h and the SB's FM aliases from a timer-only shim.
Detection passes and digital sound works; music needs one of these:

* An OPL3LPT on the parallel port, on any card (next section).
* The software OPL3 in VSBPCMT.EXE. The engine mixes it, so it needs the
  render path: `/RESAMP` on the IBM card, automatic on the 755C. Each FM
  access costs a trap. On the 755C's DX4/75, measured before VSBPCMJ, Monkey
  Island 1 stutters in dense passages, Monkey Island 2 slows until it fails,
  and DOOM's music crashes.
* General MIDI instead of AdLib: the SCP-55's Sound Canvas, the TDK cards via
  TDKSYN, the Audigy's wavetable.

### OPL3LPT

An OPL3LPT is a YMF262 on the printer port. `/LPT` sends every FM register
write to it, from 388h-38Bh and the SB-base aliases alike, on any `/CARD`:
`/LPT` for 378h, `/LPT278`, or `/LPT3BC`. The chip is write-only, so the timer
shim answers detection, and a card's FM chip, if it has one, goes unused.
VSBPCM silences the chip at load and at unload.

    SET BLASTER=A220 I7 D1 T4
    SET SBEFMPATCH=1
    JLOAD QPIEMU.DLL
    JLOAD VSBPCMJ.DLL
    HDPMI32I -r -x -v
    VSBPCM /CARD:IBMAUD /A220 /LPT

How each kind of game reaches the chip:

* Real mode: a V86 stub writes to the LPT and answers the delay reads with no
  switch to protected mode. With VSBPCMJ the FM ports are served at ring 0
  and the delay reads are patched out of the game's code.
* 32-bit protected mode: every FM access is an HDPMI trap, and era drivers
  pad each register write with about 40 status reads; DOOM's music lost
  tempo. `SBEFMPATCH=1` rewrites those reads to `NOP` in the game's code
  (index 20h or above, the ADLiPT rule), and DOOM's and SimCity 2000's music
  keep tempo.
* VSBPCM16 forwards FM to the LPT without the stub or the patch.

Verified on a Toshiba T2130CT (DX4) with the IBM card: Monkey Island, Dune
II, Flashback, DOOM and SimCity 2000, and Theme Hospital with VSBPCMJ. On the
755C, `/AUX` mixes the OPL3LPT into the laptop's output. `/LPT` is not
available in VSBPCMT or with `/FMVOL`. FastDoom drives an OPL3LPT with no help
from VSBPCM.

## Card notes

### Roland SCP-55

The card has no FM chip and carries an MPU-401 Sound Canvas. Set the game's
music to General MIDI on port 330 and its sound to Sound Blaster on 220; both
play at once. Never pass `/P` or put `P=` in BLASTER: either traps 330 and
takes the Sound Canvas away. Verified on a Pentium MMX; on a 486 digital audio
can crackle, and `SBEMAXHZ` or `SBERTC` lower the load.

### IBM PCMCIA Audio Adapter

A WAV player with a 16K-word sample ring on the card and no SB logic, DMA or
FM. VSBPCM tops the ring up from the RTC and paces on the card's play
position, so no card IRQ is needed; it pads silence when the guest falls
behind (an empty ring kills the codec link).

Output is 8-bit stereo at 11025 Hz. `/DACRATE` picks another rate,
`SBEIBMST=0` mono for the slowest hosts, `SBEIBM16=1` 16-bit, which is the
`/RESAMP` default. FM music: VSBPCMT `/RESAMP` (deploy/goibmf.bat), or `/LPT`.

Verified on an IBM PC110 (486SX/33): DOOM and Epic Pinball. Open issue: with
`SBEIBMST=0`, Epic Pinball's downmix was heard as one channel.

### TDK MC-8000 and DMC-9000

The EMU8200 plays a ring in the card's sample DRAM at the guest's rate.
`SBEMAXHZ` caps that rate (default 22050; use 11025 on a 486). AdLib music is
silent; TDKSYN in the MC8KGO repository plays General MIDI on the card.
Verified on a ThinkPad 235 and a Toshiba T2130CT. Known issue: Epic Pinball has
a rare stutter that sounds like part of an earlier sample replaying.

### ThinkPad 755C

No enabler: the driver wakes the CS4248 through ThinkPad control port 15E8h
(index 1Ch, bit 1), a write that is only safe on a ThinkPad, so the backend
runs only when `/CARD:TP755` names it. The 750 family and the 360PE carry the
same codec.

The 755C has ISA DMA to the codec: an 8237 channel 0 ring in conventional
memory, paced by the codec's IRQ10. Use guest DMA 1 or 3 and SB type T4
(games with a saved SB Pro setting play stereo at this mono DSP, double
speed), and leave out `/CF4`.

VSBPCM.EXE converts guest audio to 11025 Hz (`/DACRATE`) and caps the codec
period at 1/128 s: guest SB blocks are taken once a period, and at 43 periods
a second DOOM2's sound effects came out quiet and choppy. VSBPCMT with its
software OPL3 on uses the render path, with `/PS1024 /DACRATE11025`.

The second 3.5 mm jack is a stereo line in that the codec mixes into its
output. `/AUX` unmutes it, so an OPL3LPT or a synth plugged in there plays
through the laptop:

    VSBPCM /CARD:TP755 /A220 /LPT3BC /AUX

Verified on a 755C (486DX4/75): SBDIAG, DOOM and DOOM2 sound effects, Epic
Pinball, Duke Nukem II. doc/tp755-handoff.md covers the internals.

### Audigy 2 ZS Notebook (CardBus)

A PCI card, driven by the SB Live/Audigy driver with a DMA ring, on machines
whose BIOS supports CardBus (tested: ThinkPad 235, Pentium 233MMX). It plays
Sound Blaster digital, software OPL3 (`/OPL0` to save CPU when a game can use
MIDI), and General MIDI on the EMU10K2's hardware voices from a SoundFont 2
file, trapped at port 330:

    JEMM386 LOAD X=D000-DFFF
    AUD2GO
    JLOAD QPIEMU.DLL
    HDPMI32I -r -x -v
    SET AUDSF2=C:\VSBPCM\TIMGM6MB.SF2
    VSBPCMA /CARD:AUDIGY /A220 /P330 /OPL0

No SoundFont ships here; TimGM6mb (GPL v2, tested) comes from Debian's
timgm6mb-soundfont package or MuseScore 1.x. `AUDWTGAIN` trims the level in
centibels. Small and mid-size GM fonts are stable; large layered fonts
(GeneralUser GS) can hang the machine mid-song. Chip tools: tools/audigy/.

## Building

| Command | Output |
|---------|--------|
| `tools/build.sh` | VSBPCM.EXE |
| `CARD=TP755 tools/build.sh` | VSBPCMT.EXE (`OPLGEN=TABLELOG` or `HANDLER` picks dbopl's wave generator) |
| `CARD=AUDIGY tools/build.sh` | VSBPCMA.EXE |
| `tools/build16.sh` | VSBPCM16.EXE (Open Watcom) |
| `tools/buildjlm.sh` | VSBPCMJ.DLL (JWasm and wlink; fetch Jemm's JLM.INC as the script says) |

build.sh runs DJGPP v2.05 and JWasm v2.17+ in a Linux container (doc/NOTES.md);
`RELEASE=1` leaves out the per-tick telemetry. The engine is VSBHDA 2.0.
Released binaries always correspond to the tagged source in this repository.

## Credits and licence

VSBPCMCIA is GNU General Public License v2 (see COPYING). It is built on the
projects below; each entry says what came from where, and copyright in those
parts stays with their authors.

* VSBHDA: https://github.com/Baron-von-Riedesel/VSBHDA - the SB emulation
  core this is a fork of
* MPXPlay (C) PDSoft (Attila Padar): https://mpxplay.sourceforge.net/ - the
  au_cards sound-card driver interface every backend here implements
* SBEMU: https://github.com/crazii/SBEMU - the ES1688 passthrough backend was
  originally developed against SBEMU, and the DPMI helper API the backends
  call (DPMI_InstallISR and friends, the pds_* helpers) keeps crazii's shape;
  the DJGPP implementations behind it were written here
* Linux ALSA, sound/isa/wss/wss_lib.c (GPL v2) - the ThinkPad system-control
  twiddle that wakes the 755C's planar codec (port 0x15E8, index 0x1C, bit
  0x02) in src/hw/sc_tp755.c. sc_es1688.c separately cites ALSA for one ES1688
  reset behaviour (reset bit 1 clears the FIFO), a documented register effect
  we cross-checked, not code taken from it
* Linux ALSA snd-emu10k1 (GPL v2), (C) Jaroslav Kysela and contributors - the
  EMU10K2/CA0108 register definitions (src/hw/EMU10K1.H), the Audigy 2 ZS
  Notebook initialisation, the BAR+0x38 wake-up and the WM8768 DAC sequences.
  The bench tools in tools/audigy/ take their chip knowledge from the same
  source (see tools/audigy/README.md)
* Linux ALSA, sound/isa/sb/emu8000.c (GPL v2 or later), (C) Jaroslav Kysela,
  Steve Ratcliffe and Takashi Iwai - the EMU8000 initialisation arrays in
  src/hw/emu8kini.h, carried verbatim (emu8000.c notes they come from
  Creative's ADIP), which sc_mc8k.c loads into the TDK cards' EMU8200
* DOSBox's DBOPL (GPL v2) - OPL3 emulation, linked only by the builds that
  need it (CARD=TP755, CARD=AUDIGY)
* FastDoom: https://github.com/viti95/FastDoom - the OPL3LPT write sequence
  (control-port values and the settle reads, FASTDOOM/ns_sbmus.c) that
  src/FMSHIM.C, src/RMCODE1.ASM and jlm/VSBPCMJ.ASM send
* Jemm (Japheth): https://github.com/Baron-von-Riedesel/Jemm - the JLM
  interface jlm/VSBPCMJ.ASM is built against (JLM.INC, not carried here), and
  the public-domain QPIEMU and IOTRAP samples its module shape and its
  hand-over to VSBPCM's stub follow
* ADLiPT: https://github.com/pdewacht/adlipt - the rule for patching FM delay
  reads (index 20h or above, `IN AL,DX` to `NOP`), which VSBPCMJ applies to
  real-mode games and SBEFMPATCH to protected-mode ones; no code is taken
  from it
* TinySoundFont (MIT, vendored in tsf/) - optional software-synth fallback;
  the hardware wavetable does not use it

Written here and (C) 2026 zikolas, GPL v2 with the rest of the tree: the
passthrough architecture (ring, RTC pump, tick-credit pacing, frame stepper,
watchdogs); the backends src/hw/sc_es1688.c, sc_vew211.c, sc_scp55.c (forked
from sc_vew211.c), sc_tp755.c, sc_mc8k.c and sc_ibmaud.c, built on the
interfaces and sequences credited above; the codec bring-up recipes worked out
on the bench; the 755C's 8237 DMA ring; the telemetry; the SF2 reader
src/hw/emu_sf2.c, written from the published SoundFont 2.01 specification;
and the Audigy wavetable src/hw/emu_wt.c, which rests on ALSA's register-level
work. Chip register semantics come from the ESS and Crystal datasheets, which
are facts rather than anyone's code.

"(C) 2026 zikolas" means the code written here and nothing more. No claim is
made over anyone else's work, and anything traced to another project is
credited above. Corrections welcome.
