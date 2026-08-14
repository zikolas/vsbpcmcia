# VSBPCMCIA
Sound Blaster emulation for PCMCIA sound cards on DMA-less laptops; a fork of Baron-von-Riedesel's VSBHDA: https://github.com/Baron-von-Riedesel/VSBHDA (itself a fork of crazii's SBEMU: https://github.com/crazii/SBEMU)

Works with unmodified HDPMI binaries (v3.21+), making it compatible with HX.

The target machines are 486-class PCMCIA laptops whose card bridges have no ISA
DMA to the socket (IBM PC110, ThinkPad 235, Toshiba T2130CT, HP OmniBook, ...).
The guest's Sound Blaster audio is intercepted and pushed to the real card's
FIFO by programmed I/O — a passthrough: the chip plays the guest's native
rate/format, nothing is resampled. FM (AdLib) rides the card's real ESFM
directly at 0x388, untrapped.
Validated from a Pentium MMX down to a 386-bus 486SLC/25 (HP OmniBook 425) —
see COMPATIBILITY.md for the measured floor and slow-CPU tuning.

Supported Sound cards:
 * ES1688-class PCMCIA cards: Ratoc REX-5571/5572, Panasonic KXL-C101
   (bring the card up with ES1688GO first)
 * THIS BRANCH: ThinkPad 755C internal Crystal CS4248 (WSS) -- see below

Sibling forks: VSBCMI (https://github.com/drivelling-spinel/VSBCMI) supports
PCI cards based on CMI 8338/8738; upstream VSBHDA covers HDA/AC97/SB Live.
The PCI card drivers are present in this tree but excluded from the build.

Game compatibility: see COMPATIBILITY.md.

Emulated modes/cards:
8-bit, 16-bit, mono, stereo, high-speed;
Sound blaster 1.0, 2.0, Pro, Pro2, 16.

Requirements:
 * HDPMI32i v3.21+ - DPMI host with port trapping; 32-bit protected-mode.
   Get it from https://github.com/Baron-von-Riedesel/HX (BIN\HDPMI32i.EXE
   inside the HXRT release zip, e.g. HXRT223.zip). Note it must be the "i"
   variant, and stock v3.20 or older fails with "Failed installing IO port
   trap for protected-mode".
 * JEMM386/JEMMEX + JLOAD QPIEMU.DLL - V86 monitor with port trapping;
   v86-mode. Get Jemm v5.84+ from https://github.com/Baron-von-Riedesel/Jemm -
   JEMM386.EXE, JLOAD.EXE and QPIEMU.DLL all ship in that one zip and MUST
   come from the same release (mixed generations refuse to load, lose
   real-mode support, or hang).
 * An enabler that powers/configures the card - ES1688GO,
   https://github.com/zikolas/es1688go (v1.4+ for game-native ESFM) -
   real chip at 0x220,
   emulation at 0x240 (see deploy/GO.BAT for the proven launch order)

Environment knobs (all optional):
 * SBEBASE  - real chip base (hex, default 220)
 * DACRATE  - idle/baseline DAC rate (default 22050)
 * SBERTC   - force a fixed RTC pump rate-select 3-15 (default: self-pacing)
 * SBEPTLAT - passthrough ring latency target in ms (default 250)
 * ESNOI8   - set 1 to disable the IRQ0 watchdog heartbeat (diagnosis only)
 * ESIRQ5   - set 1 to service the card's TC IRQ (normally unneeded)

VSBPCMCIA uses some source codes from:
 * VSBHDA: https://github.com/Baron-von-Riedesel/VSBHDA - the SB emulation core
 * MPXPlay: https://mpxplay.sourceforge.net/ - sound card driver interface
 * SBEMU: https://github.com/crazii/SBEMU - the ES1688 passthrough backend
   was originally developed against SBEMU

To create the binary, DJGPP v2.05 and JWasm (v2.17 or better) are needed;
tools/build.sh runs the whole build in a Linux container (see doc/NOTES.md).
The 16-bit Open Watcom variant of upstream VSBHDA is not built here.

License: GNU General Public License v2 (see COPYING). VSBPCMCIA is a
derivative of VSBHDA, SBEMU, MPXPlay (C) PDSoft (Attila Padar) and DOSBox's
OPL emulation, all GPL v2; the ES1688 passthrough backend (C) 2026 zikolas.
Released binaries always correspond to the tagged source in this repository.

---

# This branch: TP755 backend (VSBPCMT.EXE)

`CARD=TP755 ./tools/build.sh` builds **VSBPCMT.EXE**, a second backend that
drives the ThinkPad 755C's internal Crystal CS4248 (AD1848/WSS class, FRU
84G4289; the 750 family and 360PE carry the same planar codec) instead of a
PCMCIA card. No enabler is needed -- the driver wakes the codec itself
(ThinkPad control port 0x15E8, index 0x1C, bit 0x02) at detect. Unlike the
ES1688 passthrough, this machine HAS ISA DMA to the planar codec, so audio
runs on a real 8237 channel-0 autoinit ring in DOS conventional memory, and
the codec's period interrupt (IRQ10, planar-wired) is the engine clock.

Because the 755C has no FM chip anywhere, this build compiles the DOSBox
OPL3 emulation back in (every other build leaves 0x388 to real hardware).
FM traffic is tamed by a v86 fast path: delay reads answered in-stub, and
non-timer register writes buffered through a 1024-entry ring drained each
codec tick -- see `doc/tp755-handoff.md` for the full architecture, the
IRQ0 guardian (self-healing engine clock), and the on-box telemetry map.

## Launch recipe (proven)

```
JEMM386.EXE LOAD NOEMS X=DC00-EFFF
SET BLASTER=A220 I7 D1 T4
SET DACRATE=11025
JLOAD QPIEMU.DLL
HDPMI32I -r -x -v
VSBPCMT /A220 /PS1024
```

Notes that matter:
 * The `X=DC00-EFFF` exclusion is MANDATORY -- live PCMCIA/planar windows
   sit there and Jemm's UMB scan over them hard-wedges the machine.
 * `/T4` (SB 2.0), not `/T3`: games with saved SB Pro configs push stereo
   at this mono DSP and play double-speed.
 * No `/CF4` (suspected freeze aggravator on this box, unresolved).
 * DACRATE=11025 + /PS1024 is the DX4/75 envelope with OPL emulation on.
 * Guest DMA must not be channel 0 (`/D1` or `/D3`) -- ch0 is the codec's.

## Status (bench-verified on a 755C, 486DX4/75)

 * Digital (SB voice/SFX): daily-driver ready -- SBDIAG full pass, DOOM
   with SFX, Epic Pinball.
 * FM music, real-mode games (Monkey Island 1 class): plays with stutter
   during dense passages; the engine self-heals and the game survives.
 * FM music, very dense scores (Monkey Island 2 class): plays, but the
   sustained trap load runs the guest in slow motion and it eventually
   crashes on its own -- out of envelope on a DX4/75.
 * DOOM WITH music (protected-mode FM): out of envelope, crashes -- run
   DOOM with SFX only. The per-access port-trap cost is the limit, not
   the synthesizer; see doc/tp755-handoff.md ("VSBHDA-as-JLM") for the
   identified fix.

## OPL wave generator (OPLGEN)

The dbopl wave generator is selectable at build time:
`OPLGEN=TABLEMUL` (default), `TABLELOG` (multiply-free, fixes an upstream
DOSBox operator-precedence bug in that path), or `HANDLER` (smallest
tables) -- outputs VSBPCMT.EXE / VSBPCMTL.EXE / VSBPCMTH.EXE. On the
DX4/75 at 11025 Hz TABLEMUL and TABLELOG measure identical; the switch
exists for smaller-cache machines.
