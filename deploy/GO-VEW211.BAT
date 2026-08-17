@echo off
C:\VSBPCM\JEMM386.EXE LOAD NOEMS X=D000-D3FF
C:\VSBPCM\VEW21XGO.COM /PCIC /IO=530 /VOL=24 /W=D000
cd \VSBPCM
set BLASTER=A240 I7 D1 T4
set SBEBASE=530
set DACRATE=11025
set SBEPTLAT=60
JLOAD QPIEMU.DLL
HDPMI32I -r -x -v
rem /FMVOL = hardware-FM volume: 0-63 TL steps (0.75 dB each) added to every
rem carrier on the REAL OPL3, filtered through the ordinary port traps -- so
rem it works for real-mode AND protected-mode (DOS/4GW) games alike.
rem Do NOT also load the FMVOL.DLL JLM: that would attenuate twice.
VSBPCMV /A240 /FMVOL32
cd \
