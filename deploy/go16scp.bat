@echo off
rem Roland SCP-55 (CS4231A codec plus an onboard GS Sound Canvas).
rem
rem This card has NO DMA and NO FM chip. For music use the card's own MPU-401
rem Sound Canvas rather than the OPL detection shim: set the game's music to
rem General MIDI on port 330 and its digital sound to Sound Blaster on 220.
rem Do NOT add /P, and keep P= out of BLASTER -- either one traps 330 and
rem takes the Sound Canvas away.
rem
rem /BASE330 is the card's I/O window base, which is also SCP55GO's default.
rem /I=0 because the codec cannot raise the card's IREQ, so no IRQ is needed.
rem SCP55GO auto-detects the host backend; add /PCIC or /CS to force one.
rem Drop the JEMM386 line if Jemm already loads from CONFIG.SYS.
C:\VSBPCM\JEMM386.EXE LOAD NOEMS X=DC00-DFFF
C:\VSBPCM\SCP55GO.COM /I=0 /W=DC00
cd \VSBPCM
set BLASTER=A220 I7 D1 T4
set SBEPTLAT=60
JLOAD QPIEMU.DLL
rem
rem 16-BIT protected-mode build (Tyrian and the rest). DO NOT load both:
rem run UNINST.EXE before switching between VSBPCM and VSBPCM16.
HDPMI16I -r -x -v
VSBPCM16 /CARD:SCP55 /BASE330 /CVOL0 /A220 %1 %2 %3 %4 %5
cd \
