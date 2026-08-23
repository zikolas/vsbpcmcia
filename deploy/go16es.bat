@echo off
rem ES1688 PC Card, 16-BIT PROTECTED-MODE build (Tyrian and the rest of the
rem Borland RTM / Phar Lap 286 / DOS16M catalogue).
rem
rem The ONLY differences from GOES1688.BAT are HDPMI16I instead of HDPMI32I
rem and VSBPCM16 instead of VSBPCM. Everything else -- enabler, BLASTER,
rem QPIEMU for the real-mode trap -- is identical, because both binaries
rem serve real-mode games and only the protected-mode half differs.
rem
rem DO NOT LOAD BOTH. Run UNINST.EXE before switching between VSBPCM and
rem VSBPCM16: neither reliably detects the other (vsbhda.txt:76).
rem
rem EMULATED SB AT 220, REAL CHIP AT 240 -- see GOES1688.BAT for why.
rem Drop the JEMM386 line if Jemm already loads from CONFIG.SYS.
C:\VSBPCM\JEMM386.EXE LOAD NOEMS X=DC00-DFFF
C:\VSBPCM\ES1688GO.COM /SB=240 /FM /W=DC00
cd \VSBPCM
set BLASTER=A220 I7 D1 T4
rem 60ms passthrough cap: the engine default is 250ms, which put a
rem clearly audible gap between keypress and SFX (Tyrian, 235 bench
rem 2026-08-23). 60 is the same value GOVEW211.BAT landed on.
set SBEPTLAT=60
JLOAD QPIEMU.DLL
HDPMI16I -r -x -v
VSBPCM16 /CARD:ES1688 /BASE240 /A220 %1 %2 %3 %4 %5
cd \
