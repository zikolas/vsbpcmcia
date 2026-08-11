@echo off
rem Audigy 2 ZS Notebook (CardBus) stack on the ThinkPad 235 -- base loader
rem and the entry point for FM-MUSIC (AdLib/Sound Blaster era) games:
rem OPL3 is EMULATED (this card has no hardware OPL) and costs real CPU.
rem
rem   GOAUD              SB digital + emulated OPL3 music
rem   GOWT  [font.sf2]   SB digital + General MIDI on the hardware wavetable
rem                      (OPL off) -- use for any game with a GM music option
rem
rem AUD2GO powers the socket and assigns PCI resources; VSBPCMA wakes the
rem audio chip itself at start-up.
rem D000-DFFF must stay excluded: AUD2GO maps the socket registers there.
rem Extra args pass through to VSBPCMA. One stack per boot.
C:\VSBPCM\JEMM386.EXE LOAD X=D000-DFFF
C:\VSBPCM\AUD2GO.EXE
cd \VSBPCM
set BLASTER=A220 I7 D1 T4
set SBEBASE=220
JLOAD QPIEMU.DLL
HDPMI32I -r -x -v
VSBPCMA /A220 %1 %2 %3 %4 %5
cd \
