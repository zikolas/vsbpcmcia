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
   (bring the card up with ES1688GO first)

Sibling forks: VSBCMI (https://github.com/drivelling-spinel/VSBCMI) supports
PCI cards based on CMI 8338/8738; upstream VSBHDA covers HDA/AC97/SB Live.
The PCI card drivers are present in this tree but excluded from the build.

Emulated modes/cards:
8-bit, 16-bit, mono, stereo, high-speed;
Sound blaster 1.0, 2.0, Pro, Pro2, 16.

Requirements:
 * HDPMI32i v3.21+ - DPMI host with port trapping; 32-bit protected-mode
 * JEMM386/JEMMEX + JLOAD QPIEMU.DLL - V86 monitor with port trapping; v86-mode
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
