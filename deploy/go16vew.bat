@echo off
rem Panasonic CF-VEW211 (CS4231A), 16-BIT PROTECTED-MODE build.
rem
rem The ONLY differences from GOVEW211.BAT are HDPMI16I instead of HDPMI32I
rem and VSBPCM16 instead of VSBPCM.
rem
rem DO NOT LOAD BOTH. Run UNINST.EXE before switching between VSBPCM and
rem VSBPCM16: neither reliably detects the other (vsbhda.txt:76).
rem
rem /BASE must match VEW21XGO's /IO=. /VOL=0 on the ENABLER and no /FMVOL --
rem see GOVEW211.BAT for why.
rem Drop the JEMM386 line if Jemm already loads from CONFIG.SYS.
C:\VSBPCM\JEMM386.EXE LOAD NOEMS X=DC00-DFFF
C:\VSBPCM\VEW21XGO.COM /PCIC /IO=530 /VOL=0 /W=DC00
cd \VSBPCM
set BLASTER=A220 I7 D1 T4
set SBEPTLAT=60
JLOAD QPIEMU.DLL
HDPMI16I -r -x -v
VSBPCM16 /CARD:VEW211 /BASE530 /DACRATE11025 /CVOL0 /A220 %1 %2 %3 %4 %5
cd \
