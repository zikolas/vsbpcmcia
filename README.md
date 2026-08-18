# VSBPCMCIA
Sound Blaster emulation for PCMCIA sound cards on DMA-less laptops; a fork of Baron-von-Riedesel's VSBHDA: https://github.com/Baron-von-Riedesel/VSBHDA (itself a fork of crazii's SBEMU: https://github.com/crazii/SBEMU)

Works with unmodified HDPMI binaries (v3.21+), making it compatible with HX.

The target machines are 486-class PCMCIA laptops whose card bridges have no ISA
DMA to the socket. The guest's Sound Blaster audio is intercepted and pushed to
the real card's FIFO by programmed I/O — a passthrough: the chip plays the 
guest's native rate/format, nothing is resampled. FM (AdLib) rides the card's
real OPL directly at 0x388, untrapped. Validated from a Pentium MMX down to a 
386-bus 486SLC/25 (HP OmniBook 425) — see COMPATIBILITY.md for the measured
floor and slow-CPU tuning.

VSBPCM.EXE contains the ES1688, CS4231 and CS4248 backends; `/CARD:` picks 
one at load time. Nothing is probed — you already have to run that card's 
enabler first, so the launcher always knew which card it was talking to.

Supported sound cards:
 * ES1688-based PCMCIA cards: Ratoc REX-5571/5572, Panasonic KXL-C101
   (bring the card up with ES1688GO first,
   https://github.com/zikolas/es1688go) — `/CARD:ES1688`
 * CS4231A based PCMCIA cards: Panasonic CF-VEW211
   (bring the card up with VEW21XGO first,
   https://github.com/zikolas/vew21xgo) — `/CARD:VEW211`
 * BONUS: ThinkPad 755C Crystal CS4248: The planar codec is the sound card
   and the driver wakes it up — `/CARD:TP755`
 
 * Sound Blaster Audigy 2 ZS Notebook (CardBus, SB0530): bring the socket up
   with AUD2GO first (https://github.com/zikolas/aud2go) — VSBPCMA.EXE, a
   separate build (`CARD=AUDIGY`); see "CardBus backend" below

Game compatibility: see COMPATIBILITY.md.

## Launching

Run `VSBPCM /?` for the full option list and a per-card recipe summary.
`/CARD` is required; running without it prints those recipes rather than
guessing. Two addresses are easy to confuse, so the help says it too:

 * `/A` is the **emulated** SB base — the address the guest looks for.
 * `/BASE` is the **real** card's base — match it to the enabler's setting.

`/BASE` defaults per card to that card's own enabler default (220 / 530 /
4E30), so it can be omitted when you left the enabler at its default.

**Emulated SB at 220, real chip elsewhere.** Games that scan for a Sound
Blaster probe 0x220 first and must find the emulation there; if they find the
real chip instead they select a card with no DMA on the socket and go silent.
This is why the ES1688 recipe moves the card to 240 — and why `/BASE` is in
practice mandatory for ES1688, whose default would otherwise collide with
`/A220`.

ES1688 PCMCIA card:

    ES1688GO /SB=240 /FM /W=DC00
    SET BLASTER=A220 I7 D1 T4
    JLOAD QPIEMU.DLL
    HDPMI32I -r -x -v
    VSBPCM /CARD:ES1688 /BASE240 /A220

CF-VEW211 PCMCIA card:

    VEW21XGO /PCIC /IO=530 /VOL=0 /W=DC00
    SET BLASTER=A220 I7 D1 T4
    JLOAD QPIEMU.DLL
    HDPMI32I -r -x -v
    VSBPCM /CARD:VEW211 /BASE530 /DACRATE11025 /CVOL0 /A220

ThinkPad 755C planar codec (no enabler):

    SET BLASTER=A220 I7 D1 T4
    JLOAD QPIEMU.DLL
    HDPMI32I -r -x -v
    VSBPCM /CARD:TP755 /A220 /PS1024 /DACRATE11025

JEMM386 must be loaded before JLOAD — from CONFIG.SYS, or as
`JEMM386.EXE LOAD NOEMS X=DC00-EFFF` at the top of the batch. The `X=`
exclusion covering the card window is MANDATORY: live PCMCIA/planar windows
sit there and Jemm's UMB scan over them hard-wedges the machine.
See deploy/ for working batches.

## Options

`/CARD:name` selects the backend and is required. The rest are optional:

 * `/BASE`    real card's IO base, hex (def per card: 220 / 530 / 4E30)
 * `/DACRATE` codec rate in Hz (def per card). On the ES1688 passthrough this
   only sets the idle/bring-up rate — the guest's own format wins on the first
   feed. On the VEW211 and TP755 it is the codec's actual rate.
 * `/CVOL`    codec DAC attenuation 0-63, ~1.5 dB per step (VEW211/TP755)
 * `/FMVOL`   volume trim on a REAL OPL3, 0-63 TL steps
 * `/FMSHIM`  pretend the card has no FM chip (bench diagnostic; see below)
 * `/A /I /D /T /H` the emulated SB's geometry (base, IRQ, DMA, type, high DMA)

Configuration by environment variable was removed in v1.0: values persisted
between runs, so a base or card left over from one launcher silently
redirected the next. Only transient bench knobs remain in the environment —
`SBERTC` (fixed RTC pump rate-select 3-15), `SBEPTLAT` (passthrough ring
latency target, ms), `SBENORS` (VEW211: disable the frame stepper), `ESNOI8`
(disable the IRQ0 watchdog heartbeat), `ESIRQ5`, `IRQTONE`, `FIFOTEST`.

### FM and the detection shim

Cards with real FM silicon (ES1688's ESFM, the VEW211's discrete YMF262) get
it for free: 0x388 is left untrapped and guest AdLib rides the hardware.

A card with NO FM chip — the 755C — cannot simply ignore those ports. Era
games run an AdLib timer test on the SB's FM aliases BEFORE they will touch
the DSP, so an unanswered 0x388 costs you digital sound as well as music.
The driver therefore answers those ports from a timer-only shim: enough to
pass detection, with no synthesis behind it. FM music is silent on such a
card unless the software OPL is compiled in (see the TP755 build below).
`/FMSHIM` forces that path on a card that does have a chip, to exercise it.

## Builds

`tools/build.sh` runs the whole build in a Linux container (see doc/NOTES.md);
DJGPP v2.05 and JWasm v2.17+ are required.

 * plain — **VSBPCM.EXE**, the unified NOFM binary (ES1688 + VEW211 + TP755)
 * `CARD=TP755` — **VSBPCMT.EXE**: the same three backends PLUS the DOSBox
   OPL3 emulation, i.e. real FM MUSIC on the FM-less 755C instead of the
   detection-only shim. A feature flag, not a card selector.
 * `CARD=AUDIGY` — **VSBPCMA.EXE**, see below.

The 16-bit Open Watcom variant of upstream VSBHDA is not built here; it would
be the way to support 16-bit protected-mode games, which this 32-bit build
cannot serve (DPMI gives 16- and 32-bit clients separate interrupt tables).

## CardBus backend: Audigy 2 ZS Notebook

The odd one out: a CardBus — i.e. PCI — card rather than PCMCIA, driven by
the SB Live/Audigy driver with a real DMA ring instead of the passthrough,
on machines whose BIOS supports CardBus sockets (tested: ThinkPad 235,
Pentium 233MMX). Three sound sources:

 * Sound Blaster digital - the usual emulation, mixed on the card
 * OPL3 (AdLib) - EMULATED here (the Audigy has no hardware OPL); costs real
   CPU on slow machines, disable with /OPL0 when games can use MIDI instead
 * **General MIDI on the EMU10K2's own hardware voices**: a SoundFont 2
   synthesizer inside the driver. The card renders; the host CPU does no
   mixing. Guest MIDI is trapped at port 330h (/P330)

Launch order (see deploy/ and the audigy-wt1 release notes):

    JEMM386 LOAD X=D000-DFFF     (AUD2GO maps socket registers at D000)
    AUD2GO                       (powers the socket, assigns resources)
    JLOAD QPIEMU.DLL
    HDPMI32I -r -x -v
    SET AUDSF2=C:\VSBPCM\TIMGM6MB.SF2
    VSBPCMA /A220 /P330 /OPL0

Configure games: Music = General MIDI port 330, Sound = Sound Blaster
A220 I7 D1. No soundfont ships with this repository: TimGM6mb (GPLv2, the
tested one) comes from Debian's timgm6mb-soundfont package or MuseScore 1.x;
any small/mid GM SoundFont 2 file named by AUDSF2 works.

Wavetable knobs: AUDSF2 (font path; unset = wavetable off), AUDWTGAIN
(level trim in centibels), AUDWTDEMO (play a scale at boot as a smoke test).
Further AUDWT* variables are bisect/diagnostic switches — see mpxplay/emu_wt.c.

Alpha limits: small/mid GM fonts are the stable path — large layered fonts
(GeneralUser GS) can hard-wedge the machine mid-song, a voice-engine
interaction still under investigation on this silicon; some instrument
decays run slightly short; playback only, no MPU MIDI-in.

Chip-level bench tools (cbinit, fxvol, dacvol, the audmix mixer) live in
tools/audigy/.

## The TP755 planar backend

`/CARD:TP755` drives the ThinkPad 755C's internal Crystal CS4248 (AD1848/WSS
class, FRU 84G4289; the 750 family and 360PE carry the same planar codec).
No enabler is needed — the driver wakes the codec itself (ThinkPad control
port 0x15E8, index 0x1C, bit 0x02) at detect. Because that write must happen
before the machine can be identified as a ThinkPad at all, this backend is
opt-in: it does nothing unless `/CARD:TP755` names it.

Unlike the PCMCIA passthrough, this machine HAS ISA DMA to the planar codec,
so audio runs on a real 8237 channel-0 autoinit ring in DOS conventional
memory, and the codec's period interrupt (IRQ10, planar-wired) is the engine
clock. Guest DMA must therefore not be channel 0 — use `/D1` or `/D3`.

For FM music rather than detection-only, build `CARD=TP755` (VSBPCMT.EXE),
which compiles the DOSBox OPL3 emulation back in. FM traffic is then tamed by
a v86 fast path: delay reads answered in-stub, and non-timer register writes
buffered through a 1024-entry ring drained each codec tick — see
`doc/tp755-handoff.md` for the architecture, the IRQ0 guardian (self-healing
engine clock) and the on-box telemetry map.

Notes that matter on this box:
 * `/T4` (SB 2.0), not `/T3`: games with saved SB Pro configs push stereo at
   this mono DSP and play double-speed.
 * No `/CF4` (suspected freeze aggravator here, unresolved).
 * `/DACRATE11025 /PS1024` is the DX4/75 envelope with OPL emulation on.

Status, bench-verified on a 755C (486DX4/75):
 * Digital (SB voice/SFX): daily-driver ready — SBDIAG full pass, DOOM with
   SFX, Epic Pinball, Duke Nukem II.
 * FM music, real-mode games (Monkey Island 1 class): plays with stutter
   during dense passages; the engine self-heals and the game survives.
 * FM music, very dense scores (Monkey Island 2 class): plays, but the
   sustained trap load runs the guest in slow motion and it eventually
   crashes on its own — out of envelope on a DX4/75.
 * DOOM WITH music (protected-mode FM): out of envelope, crashes — run DOOM
   with SFX only. The per-access port-trap cost is the limit, not the
   synthesizer; see doc/tp755-handoff.md ("VSBHDA-as-JLM") for the fix.

### OPL wave generator (OPLGEN)

The dbopl wave generator is selectable at build time:
`OPLGEN=TABLEMUL` (default), `TABLELOG` (multiply-free, fixes an upstream
DOSBox operator-precedence bug in that path), or `HANDLER` (smallest
tables) — outputs VSBPCMT.EXE / VSBPCMTL.EXE / VSBPCMTH.EXE. On the
DX4/75 at 11025 Hz TABLEMUL and TABLELOG measure identical; the switch
exists for smaller-cache machines.

## Emulated modes/cards

8-bit, 16-bit, mono, stereo, high-speed;
Sound Blaster 1.0, 2.0, Pro, Pro2, 16.

## Requirements

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
 * An enabler that powers/configures the card (not needed for the 755C):
   ES1688GO https://github.com/zikolas/es1688go (v1.4+ for game-native ESFM),
   or VEW21XGO https://github.com/zikolas/vew21xgo.

## Credits and licence

VSBPCMCIA is GNU General Public License v2 (see COPYING). It is built on the
projects below; each entry says what came from where, and copyright in those
parts stays with their authors.

 * VSBHDA: https://github.com/Baron-von-Riedesel/VSBHDA - the SB emulation
   core this is a fork of
 * MPXPlay (C) PDSoft (Attila Padar): https://mpxplay.sourceforge.net/ - the
   au_cards sound-card driver interface every backend here implements
 * SBEMU: https://github.com/crazii/SBEMU - the ES1688 passthrough backend
   was originally developed against SBEMU, and the DPMI helper API the
   backends call (DPMI_InstallISR and friends, the pds_* helpers) keeps
   crazii's shape; the DJGPP implementations behind it were written here
 * Linux ALSA, sound/isa/wss/wss_lib.c (GPL v2) - the ThinkPad
   system-control twiddle that wakes the 755C's planar codec (port 0x15E8,
   index 0x1C, bit 0x02) in mpxplay/sc_tp755.c. sc_es1688.c separately cites
   ALSA for one ES1688 reset behaviour (reset bit 1 clears the FIFO): that is
   a documented register effect we cross-checked, not code taken from it --
   noted here for completeness rather than because it is owed
 * Linux ALSA snd-emu10k1 (GPL v2), (C) Jaroslav Kysela and contributors -
   the EMU10K2/CA0108 register definitions (mpxplay/emu10k1.h), the Audigy 2
   ZS Notebook initialisation, the BAR+0x38 wake-up and the WM8768 DAC
   sequences. The bench tools in tools/audigy/ take their chip knowledge from
   the same source (see tools/audigy/README.md)
 * DOSBox's DBOPL (GPL v2) - OPL3 emulation, linked only by the builds that
   need it (CARD=TP755, CARD=AUDIGY)
 * TinySoundFont (MIT, vendored in tsf/) - optional software-synth fallback;
   the hardware wavetable does not use it

Written here and (C) 2026 zikolas, GPL v2 with the rest of the tree: the
passthrough architecture (ring, RTC pump, tick-credit pacing, frame stepper,
watchdogs); the three backends mpxplay/sc_es1688.c, sc_vew211.c and
sc_tp755.c, built on the interfaces and sequences credited above; the codec
bring-up recipes worked out on the bench; the 755C's 8237 DMA ring; the
telemetry; the SF2 reader mpxplay/emu_sf2.c, written from the published
SoundFont 2.01 specification; and the Audigy wavetable mpxplay/emu_wt.c,
which rests on ALSA's register-level work. Chip register semantics come from
the ESS and Crystal datasheets, which are facts rather than anyone's code.

"(C) 2026 zikolas" means the code written here and nothing more. No claim is
made over anyone else's work, and anything traced to another project is
credited above. Corrections welcome.

Released binaries always correspond to the tagged source in this repository.
