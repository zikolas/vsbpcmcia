@echo off
rem Panasonic CF-VEW211 (CS4231A). /BASE must match VEW21XGO's /IO=.
rem
rem /VOL=0 on the ENABLER, and no /FMVOL: the codec DAC attenuator defaults to
rem -36 dB, which buried digital SFX under the FM music. Raising PCM to full
rem scale fixes the balance with one register write, where /FMVOL cost a trap
rem and a filter pass on EVERY FM register write. Do not use both.
rem Drop the JEMM386 line if Jemm already loads from CONFIG.SYS.
C:\VSBPCM\JEMM386.EXE LOAD NOEMS X=DC00-DFFF
C:\VSBPCM\VEW21XGO.COM /PCIC /IO=530 /VOL=0 /W=DC00
cd \VSBPCM
set BLASTER=A220 I7 D1 T4
set SBEPTLAT=60
JLOAD QPIEMU.DLL
HDPMI32I -r -x -v
VSBPCM /CARD:VEW211 /BASE530 /DACRATE11025 /CVOL0 /A220 %1 %2 %3 %4 %5
cd \
