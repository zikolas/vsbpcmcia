@echo off
rem Audigy 2 ZS Notebook (CardBus) -> VSBPCMA / SBEMU on the ThinkPad 235
rem AUD2GO powers the socket and assigns PCI resources; VSBPCMA wakes the
rem audio chip itself at start-up.
rem D000-DFFF must stay excluded: AUD2GO maps the socket registers there.
rem Extra args are passed through, e.g.  GOAUD /VOL4   or   GOAUD /PS2048
C:\VSBPCM\JEMM386.EXE LOAD X=D000-DFFF
C:\VSBPCM\AUD2GO.EXE
cd \VSBPCM
set BLASTER=A220 I7 D1 T4
set SBEBASE=220
JLOAD QPIEMU.DLL
HDPMI32I -r -x -v
VSBPCMA /A220 %1 %2 %3
cd \
