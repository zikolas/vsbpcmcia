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

VSBPCM.EXE contains the ES1688, CS4231/CS4231A, CS4248, EMU8200 (TDK) and IBM
Audio Adapter backends; `/CARD:`
picks one at load time. Nothing is probed — you already have to run that card's 
enabler first, so the launcher always knew which card it was talking to.

Supported sound cards:
 * ES1688-based PCMCIA cards:
   - Ratoc REX-5571/5572, Panasonic KXL-C101
   (bring the card up with ES1688GO first,
   https://github.com/zikolas/es1688go) — `/CARD:ES1688`
 * CS4231A based PCMCIA cards:
   - Panasonic CF-VEW211 and CF-VEW212 "Sound Card PRO"
   (bring the card up with VEW21XGO first — 2.5+ for the 212,
   https://github.com/zikolas/vew21xgo) — `/CARD:VEW211` for both. 
   - Roland SCP-55 (bring the card up with SCP55GO first,
   https://github.com/zikolas/scp55-enabler) — `/CARD:SCP55`
 * IBM PCMCIA Audio Adapter (P/N 0933967, CIS "IBM NON-DSP AUDIO"): bring the
   card up with IBMAUDGO first (https://github.com/zikolas/ibmaudgo) —
   `/CARD:IBMAUD`
 * TDK MusicCard MC-8000 and DMC-9000 (EMU8200): bring the card up with MC8KGO
   first (https://github.com/zikolas/mc8kgo) — `/CARD:MC8K`
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

`/BASE` defaults per card to that card's own enabler default (220 / 250 / 330
/ 530 / 4E30; MC8K looks at 240 and then 260), so it can be omitted when you
left the enabler at its default.

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

CF-VEW212 PCMCIA card — same backend, same command line:

    VEW21XGO /PCIC /VOL=0 /W=DC00
    SET BLASTER=A220 I7 D1 T4
    JLOAD QPIEMU.DLL
    HDPMI32I -r -x -v
    VSBPCM /CARD:VEW211 /BASE530 /DACRATE11025 /CVOL0 /A220

The 212 carries the same CS4231A behind a different ASIC, on a config index
its own CIS never declares; VEW21XGO 2.5 sets that index itself, and there is
only one codec base on this card, so `/IO=` is overridden to 530 (2.3 and 2.4
recognise a 212 and decline it, leaving the card unconfigured).
FM comes from the OPL4's OPL3-compatible half at 0x388 and rides it untrapped
exactly like the 211's YMF262.

Roland SCP-55 PCMCIA card:

    SCP55GO /PCIC /I=0 /W=DC00
    SET BLASTER=A220 I7 D1 T4
    JLOAD QPIEMU.DLL
    HDPMI32I -r -x -v
    VSBPCM /CARD:SCP55 /BASE330 /CVOL0 /A220

IBM PCMCIA Audio Adapter:

    IBMAUDGO /W=DC00
    SET BLASTER=A220 I7 D1 T4
    JLOAD QPIEMU.DLL
    HDPMI32I -r -x -v
    VSBPCM /CARD:IBMAUD /A220

TDK MC-8000 / DMC-9000:

    MC8KGO /W=DC00
    SET BLASTER=A220 I7 D1 T4
    SET SBEMAXHZ=11025
    JLOAD QPIEMU.DLL
    HDPMI32I -r -x -v
    VSBPCM /CARD:MC8K /A220

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

 * `/BASE`    real card's IO base, hex (def per card: 220 / 250 / 330 / 530 /
   4E30)
 * `/DACRATE` codec rate in Hz (def per card). On the ES1688 passthrough this
   only sets the idle/bring-up rate — the guest's own format wins on the first
   feed. On the VEW211, SCP55, TP755 and IBMAUD it is the codec's actual
   rate.
 * `/MAXHZ`   codec rate ceiling in Hz (VEW211/SCP55; def 22050). The CPU knob
   for a slow host: the 16-frame FIFO makes the pump interrupt count scale with
   the codec rate, and the frame stepper folds a faster guest down onto the
   cap. `/MAXHZ11025` halves the ticks of a 22 kHz stream.
 * `/CVOL`    codec DAC attenuation 0-63, ~1.5 dB per step (VEW211/SCP55/TP755)

 * `/FMVOL`   volume trim on a REAL OPL3, 0-63 TL steps
 * `/FMSHIM`  pretend the card has no FM chip (bench diagnostic; see below)
 * `/LPT`     FM from an OPL3LPT on the parallel port: `/LPT` (378),
   `/LPT278` or `/LPT3BC` (see below)
 * `/A /I /D /T /H` the emulated SB's geometry (base, IRQ, DMA, type, high DMA)

Configuration by environment variable was removed in v1.0: values persisted
between runs, so a base or card left over from one launcher silently
redirected the next. Only transient bench knobs remain in the environment —
`SBERTC` (fixed RTC pump rate-select 3-15), `SBEPTLAT` (passthrough ring
latency target, ms), `SBENORS` (VEW211/SCP55: disable the frame stepper),
`SBEMAXHZ` (SCP55: cap the codec rate, `/MAXHZ` being the switch form for both
CS4231A cards; MC8K: cap the guest rate, above which the feed decimates),
`SBENOSTUB` (leave the V86 stub's fast paths — the FM alias forward, the DSP
write-status answer and the `/LPT` forward — disarmed, for an A/B),
`SBELPTDLY` and `SBELPTCLI` (`/LPT` timing: control-port reads after each
strobe, default 6; `SBELPTCLI=0` leaves interrupts on in the stub),
`SBEFMPATCH=1` (VSBPCM.EXE, with the FM shim: rewrite a protected-mode
game's FM delay reads, the dummy `IN AL,DX` after each register write, to
`NOP` in its code so they stop trapping; count in IAC 0x4F1), `ESNOI8`
(disable the IRQ0 watchdog heartbeat), `ESIRQ5`, `IRQTONE`, `FIFOTEST`, and
`SBEIBMST` / `SBEIBM16` (IBMAUD output format, see its section below).


### FM and the detection shim

Cards with real FM silicon (ES1688's ESFM, the VEW211's discrete YMF262, the
VEW212's OPL4) get it for free: 0x388 is left untrapped and guest AdLib
rides the hardware.

A card with NO FM chip — the 755C and the SCP-55 — cannot simply ignore those
ports. Era
games run an AdLib timer test on the SB's FM aliases BEFORE they will touch
the DSP, so an unanswered 0x388 costs you digital sound as well as music.
The driver therefore answers those ports from a timer-only shim: enough to
pass detection, with no synthesis behind it. FM music is silent on such a
card unless the software OPL is compiled in (see the TP755 build below). The
SCP-55 has a better option than either — see its section below.
`/FMSHIM` forces that path on a card that does have a chip, to exercise it.

`/LPT` takes the same path and also sends every FM register write, from the
0x388-0x38B ports and the SB-base aliases alike, to an OPL3LPT (a YMF262 on
the printer port). The OPL3LPT is write-only, so detection is answered by the
shim, and the card's own FM chip, if it has one, goes unused. The chip is
silenced at load and at unload. It works in both trap worlds on any `/CARD`.

Each FM access is a port trap, and era drivers pad every register write with
40 or so status reads. For real-mode games VSBPCM.EXE installs a V86 stub
that answers those reads and drives the LPT itself, with no switch to
protected mode (it lives in a small DOS block of its own; `SBENOSTUB=1`
disarms it). Protected-mode games still pay a full trap per access. Before
the stub, Monkey Island played cleanly on a DX4 and slowed on a 486SLC; on a
DX4/75 DOOM itself ran normally but its music (protected mode, DMX) lost
tempo after the opening. FastDoom has its own OPL3LPT driver, which needs no
trapping. `/LPT` is not available in VSBPCMT (its V86 stub keeps 388h writes
for dbopl), has no V86 stub in VSBPCM16, and does not combine with `/FMVOL`.

## Builds

`tools/build.sh` runs the whole build in a Linux container (see doc/NOTES.md);
DJGPP v2.05 and JWasm v2.17+ are required.
The engine is VSBHDA 2.0 (upstream merged 2026-09-05: in-place ADPCM decoder,
central ring write pointer, `/B` `/BP` buffer options, `src/hw` layout).

 * plain — **VSBPCM.EXE**, the unified NOFM binary (ES1688 + VEW211 + SCP55
   + MC8K + TP755 + IBMAUD)
 * `CARD=TP755` — **VSBPCMT.EXE**: the same backends PLUS the DOSBox
   OPL3 emulation, i.e. real FM MUSIC on the FM-less 755C instead of the
   detection-only shim. A feature flag, not a card selector.
 * `CARD=AUDIGY` — **VSBPCMA.EXE**, see below.

### 16-bit protected-mode games — VSBPCM16.EXE

DPMI 0.9 gives 16-bit and 32-bit clients separate protected-mode interrupt
tables, so the 32-bit binaries above cannot serve a 16-bit PM game — Tyrian
and the rest of the Borland RTM / Phar Lap 286 / DOS16M catalogue. Those need
a driver that is itself a 16-bit client:

    ./tools/build16.sh        ->  ow16/VSBPCM16.EXE

built with Open Watcom (`ow16.mak` is the on-box equivalent) rather than
DJGPP. On the box it differs from the 32-bit stack in exactly two words —
`HDPMI16I -x` instead of `HDPMI32I -x`, and `VSBPCM16` instead of `VSBPCM`;
see `deploy/go16es.bat` and `deploy/go16vew.bat`. Run `UNINST.EXE` before
switching between the two: neither reliably detects the other. Both serve
real-mode games.

Bench-verified on the ThinkPad 235 + KXL-C101 (2026-08-23): **Tyrian**
(Borland RTM, 16-bit PM) with digital SFX and AdLib FM both working, and
**Jazz Jackrabbit** clean throughout. Known wart: Tyrian's own menu shell
runs slightly slow (jukebox and gameplay are full speed) — see
`doc/16bit.md` for the analysis, the staged experiments, and the three port
bugs the bench shook out. The new protected-mode interrupt trampolines
(`src/pmisr.asm`, replacing a DJGPP libc facility Open Watcom has no
equivalent for) are proven on both hosts; `PMISR=1 ./tools/build.sh` builds
the 32-bit A/B binary that runs them where known-good results exist —
re-run it after any trampoline change.

`doc/vdpmi.md` covers the related question of whether crazii's VDPMI could
replace this: it cannot — it is Pentium-only, it is at its worst on 16-bit
clients, and VSBPCM's synchronous IRQ delivery is not the design VDPMI's
virtual PIC serves.

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
Further AUDWT* variables are bisect/diagnostic switches — see src/hw/emu_wt.c.

Alpha limits: small/mid GM fonts are the stable path — large layered fonts
(GeneralUser GS) can hard-wedge the machine mid-song, a voice-engine
interaction still under investigation on this silicon; some instrument
decays run slightly short; playback only, no MPU MIDI-in.

Chip-level bench tools (cbinit, fxvol, dacvol, the audmix mixer) live in
tools/audigy/.

## The Roland SCP-55

`/CARD:SCP55`, after bringing the card up with SCP55GO. `/BASE` is 330, which
is the enabler's own default, so you can leave it off.

The card has no FM chip, but it does carry a real MPU-401 Sound Canvas. So set
the game's music to **General MIDI on port 330** rather than AdLib, and its
digital sound to Sound Blaster on 220 — both play at once, and the music is a
GS synth instead of emulated OPL. Never pass `/P`, and keep `P=` out of
BLASTER: either traps 330 and takes the Sound Canvas away.

Verified on a Pentium MMX. On 486 machines digital audio plays but can crackle.
`SBEMAXHZ` caps the codec rate and the pump rate follows it; `SBERTC` pins the
pump directly. Lower is safer, at the cost of bandwidth.

Why the card needs its own backend, and why it cannot pace audio from a card
interrupt, is written up in the enabler repository.

## The IBM PCMCIA Audio Adapter

`/CARD:IBMAUD`, after bringing the card up with IBMAUDGO. The card needs no
IRQ. `/BASE` is 250, IBMAUDGO's default, so you can leave it off.

This card is a WAV player. It has no Sound Blaster logic, no DMA and no FM
chip: an IBM ASIC in front of a serial codec keeps a 16K-word sample ring on
the card, the host appends samples to it, and the card reports how far it has
played. The backend tops the ring up from the RTC pump and paces on that
position, so the card needs no interrupt. The ring must never run dry while
playing (the codec link dies until the card is set up again), so the pump
pads silence when the guest falls behind, and closes the card after a second
with nothing to play.

The codec runs one fixed format and the guest's audio is converted to it:
8-bit stereo at 11025 Hz by default, one card word per frame, which is 11025
port writes a second. With `/RESAMP` the engine does the mixing, software FM
included, and the default is 16-bit stereo (22050 writes a second): cut to 8
bits, FM note tails and fades come through grainy.
 * `/DACRATE` picks another rate from the codec's table (22050 for 22 kHz
   titles, at twice the writes).
 * `SBEIBMST=0` selects mono, two samples per word, for the slowest hosts.
   Open issue: in Epic Pinball the mono downmix was heard as one of the two
   channels.
 * `SBEIBM16=1` or `SBEIBM16=0` forces 16-bit or 8-bit output either way.

FM music takes the software OPL build, VSBPCMT.EXE, with `/RESAMP`: the
passthrough path never runs the FM mixer (`deploy/goibmf.bat`). That build
needs an FPU for its table setup. With the plain build the detection shim
answers 388h instead, so games still find an AdLib and go on to use the
digital, with FM silent.

Verified on an IBM PC110 (486SX/33) at T4: DOOM (mono) and Epic Pinball
(stereo). On a Toshiba T2130CT (486DX4) with VSBPCMT `/RESAMP`: Monkey Island's
AdLib music, slowing a little in its densest passages. The playback interface
was recovered by I/O trace of IBM's own DOS WAV player.

## The TDK MC-8000 and DMC-9000

`/CARD:MC8K`, after bringing the card up with MC8KGO. `/BASE` can be left off:
the backend looks for the card at 240h (MC-8000) and then 260h (DMC-9000).

These cards carry an EMU8200 wavetable chip and its sample DRAM, with no Sound
Blaster logic, no DMA and no FM chip. The backend streams the guest's digital
audio into a ring in that DRAM and loops a voice over it, so the chip plays
the guest's own rate at exact pitch. `SBEMAXHZ` caps that rate (22050 by
default; above it the feed decimates), and 486-class hosts want
`SBEMAXHZ=11025`.

The detection shim answers 388h, so AdLib music is silent. For music, TDKSYN
in the MC8KGO repository plays General MIDI on the card's EMU8200.

Verified on a ThinkPad 235, and on a Toshiba T2130CT (486DX4) with DOOM and
Epic Pinball. Known issue: Epic Pinball has a rare stutter that sounds like
part of an earlier sample replaying.

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
   VEW21XGO https://github.com/zikolas/vew21xgo (2.5+ for the CF-VEW212),
   or SCP55GO
   https://github.com/zikolas/scp55-enabler.

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
   index 0x1C, bit 0x02) in src/hw/sc_tp755.c. sc_es1688.c separately cites
   ALSA for one ES1688 reset behaviour (reset bit 1 clears the FIFO): that is
   a documented register effect we cross-checked, not code taken from it --
   noted here for completeness rather than because it is owed
 * Linux ALSA snd-emu10k1 (GPL v2), (C) Jaroslav Kysela and contributors -
   the EMU10K2/CA0108 register definitions (src/hw/EMU10K1.H), the Audigy 2
   ZS Notebook initialisation, the BAR+0x38 wake-up and the WM8768 DAC
   sequences. The bench tools in tools/audigy/ take their chip knowledge from
   the same source (see tools/audigy/README.md)
 * Linux ALSA, sound/isa/sb/emu8000.c (GPL v2 or later), (C) Jaroslav Kysela,
   Steve Ratcliffe and Takashi Iwai - the EMU8000 initialisation arrays in
   src/hw/emu8kini.h, carried verbatim (emu8000.c notes they come from
   Creative's ADIP), which sc_mc8k.c loads into the TDK cards' EMU8200
 * DOSBox's DBOPL (GPL v2) - OPL3 emulation, linked only by the builds that
   need it (CARD=TP755, CARD=AUDIGY)
 * TinySoundFont (MIT, vendored in tsf/) - optional software-synth fallback;
   the hardware wavetable does not use it

Written here and (C) 2026 zikolas, GPL v2 with the rest of the tree: the
passthrough architecture (ring, RTC pump, tick-credit pacing, frame stepper,
watchdogs); the backends src/hw/sc_es1688.c, sc_vew211.c, sc_scp55.c (forked
from sc_vew211.c), sc_tp755.c, sc_mc8k.c and sc_ibmaud.c, built on the
interfaces and sequences credited above; the codec
bring-up recipes worked out on the bench; the 755C's 8237 DMA ring; the
telemetry; the SF2 reader src/hw/emu_sf2.c, written from the published
SoundFont 2.01 specification; and the Audigy wavetable src/hw/emu_wt.c,
which rests on ALSA's register-level work. Chip register semantics come from
the ESS and Crystal datasheets, which are facts rather than anyone's code.

"(C) 2026 zikolas" means the code written here and nothing more. No claim is
made over anyone else's work, and anything traced to another project is
credited above. Corrections welcome.

Released binaries always correspond to the tagged source in this repository.
