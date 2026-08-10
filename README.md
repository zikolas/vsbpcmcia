# VSBPCMCIA
Sound Blaster emulation for PCMCIA sound cards on DMA-less laptops; a fork of Baron-von-Riedesel's VSBHDA: https://github.com/Baron-von-Riedesel/VSBHDA (itself a fork of crazii's SBEMU: https://github.com/crazii/SBEMU)

Works with unmodified HDPMI binaries (v3.21+), making it compatible with HX.

The target machines are 486-class PCMCIA laptops whose card bridges have no ISA
DMA to the socket (IBM PC110, ThinkPad 235, Toshiba T2130CT, HP OmniBook, ...).
The guest's Sound Blaster audio is intercepted and pushed to the real card's
FIFO by programmed I/O — a passthrough: the chip plays the guest's native
rate/format, nothing is resampled. FM (AdLib) rides the card's real ESFM
directly at 0x388, untrapped.

Supported Sound cards:
 * ES1688-class PCMCIA cards: Ratoc REX-5571/5572, Panasonic KXL-C101
   (bring the card up with ES1688GO first) - VSBPCM.EXE
 * CS4231A PCMCIA cards: Panasonic CF-VEW211/212, NEC PC-9801N-J04
   (bring the card up with VEW21XGO first,
   https://github.com/zikolas/vew21xgo) - VSBPCMV.EXE, built with
   CARD=VEW211
 * Sound Blaster Audigy 2 ZS Notebook (CardBus, SB0530): bring the socket up
   with AUD2GO first (https://github.com/zikolas/aud2go) - VSBPCMA.EXE, built
   with CARD=AUDIGY. This branch only; see "CardBus backend" below

Sibling forks: VSBCMI (https://github.com/drivelling-spinel/VSBCMI) supports
PCI cards based on CMI 8338/8738; upstream VSBHDA covers HDA/AC97/SB Live.
The PCI card drivers are present in this tree but excluded from the PCMCIA
builds; this branch's CARD=AUDIGY build pulls the SB Live/Audigy driver back
in for the CardBus card below.

Game compatibility: see COMPATIBILITY.md.

## CardBus backend: Audigy 2 ZS Notebook (this branch)

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
 * Linux ALSA snd-emu10k1 (GPL v2) - the Audigy 2 ZS Notebook initialisation
   and WM8768 DAC sequences ported for the CARD=AUDIGY build
 * TinySoundFont (MIT, vendored in tsf/) - optional software-synth fallback;
   the hardware wavetable itself (mpxplay/emu_sf2.c, emu_wt.c) is original

To create the binary, DJGPP v2.05 and JWasm (v2.17 or better) are needed;
tools/build.sh runs the whole build in a Linux container (see doc/NOTES.md):
plain for VSBPCM.EXE, CARD=VEW211 for VSBPCMV.EXE, CARD=AUDIGY for
VSBPCMA.EXE.
The 16-bit Open Watcom variant of upstream VSBHDA is not built here.

License: GNU General Public License v2 (see COPYING). VSBPCMCIA is a
derivative of VSBHDA, SBEMU, MPXPlay (C) PDSoft (Attila Padar) and DOSBox's
OPL emulation, all GPL v2; the ES1688 passthrough backend (C) 2026 zikolas.
The Audigy hardware wavetable and SF2 parser (C) 2026 zikolas, GPL v2 with
the rest of the tree.
Released binaries always correspond to the tagged source in this repository.
