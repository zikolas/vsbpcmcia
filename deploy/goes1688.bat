@echo off
rem ES1688 PC Card: Ratoc REX-5571/5572, Panasonic KXL-C101 and friends.
rem
rem EMULATED SB AT 220, REAL CHIP AT 240. Games that scan for a Sound Blaster
rem probe 220 first and must find the emulation there; if they find the real
rem chip they select a card with no DMA on the socket and go silent. So
rem ES1688GO puts the card at 240 and /BASE240 tells the driver where it is.
rem Drop the JEMM386 line if Jemm already loads from CONFIG.SYS.
C:\VSBPCM\JEMM386.EXE LOAD NOEMS X=DC00-DFFF
C:\VSBPCM\ES1688GO.COM /SB=240 /FM /W=DC00
cd \VSBPCM
set BLASTER=A220 I7 D1 T4
JLOAD QPIEMU.DLL
HDPMI32I -r -x -v
VSBPCM /CARD:ES1688 /BASE240 /A220 %1 %2 %3 %4 %5
cd \
