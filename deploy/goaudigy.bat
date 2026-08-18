@echo off
rem Audigy 2 ZS Notebook (CardBus, SB0530) -- a PCI card, not PCMCIA, so it
rem needs VSBPCMA.EXE (built with CARD=AUDIGY) rather than VSBPCM.EXE.
rem AUD2GO powers the socket and assigns PCI resources first; the driver wakes
rem the audio chip itself at start-up.
rem
rem D000-DFFF must stay excluded: AUD2GO maps the socket registers there.
rem OPL3 is EMULATED on this card (it has no hardware OPL) and costs real CPU
rem -- add /OPL0 on a slow machine, or use gowt.bat for General MIDI on the
rem EMU10K2's own hardware voices instead. One stack per boot.
C:\VSBPCM\JEMM386.EXE LOAD X=D000-DFFF
C:\VSBPCM\AUD2GO.EXE
cd \VSBPCM
set BLASTER=A220 I7 D1 T4
JLOAD QPIEMU.DLL
HDPMI32I -r -x -v
VSBPCMA /CARD:AUDIGY /A220 %1 %2 %3 %4 %5
cd \
