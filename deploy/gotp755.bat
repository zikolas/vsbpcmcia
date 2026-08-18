@echo off
rem ThinkPad 755C internal Crystal CS4248. No PC Card and no enabler -- the
rem backend wakes the planar codec itself. It is OPT-IN and never probed
rem (waking it means writing ThinkPad port 0x15E8 before we can know the
rem machine is a ThinkPad), so /CARD:TP755 is REQUIRED.
rem
rem X=DC00-EFFF is MANDATORY: live planar/PCMCIA windows sit there and Jemm
rem wedges the machine if it scans them. Guest DMA must not be channel 0
rem (/D1 or /D3) -- ch0 belongs to the codec.
rem
rem This build has no OPL, so 388h is answered by a timer-only shim: SB
rem detection and DIGITAL audio work, FM MUSIC IS SILENT. Build CARD=TP755
rem (VSBPCMT.EXE) if you want real FM music on this machine.
C:\VSBPCM\JEMM386.EXE LOAD NOEMS X=DC00-EFFF
cd \VSBPCM
set BLASTER=A220 I7 D1 T4
JLOAD QPIEMU.DLL
HDPMI32I -r -x -v
VSBPCM /CARD:TP755 /A220 /PS1024 /DACRATE11025 %1 %2 %3 %4 %5
cd \
