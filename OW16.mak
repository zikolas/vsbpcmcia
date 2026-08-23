# Create vsbpcm16.exe with Open Watcom and JWasm -- the 16-BIT PROTECTED-MODE
# build of VSBPCMCIA.
#
# To create the binary, enter
#   wmake -f ow16.mak
# Optionally, for a debug version, enter
#   wmake -f ow16.mak debug=1
#
# The build process is supposed to be run in either Windows or DOS.
# For DOS, however, it's necessary to install the HX runtime package
# and setup HX for full Win32 emulation:
#  C:\>HDPMI32 -a -r
#  C:\>SET DPMILDR=8
#  C:\>HXLDR32
# On a Linux/macOS host use tools/build16.sh instead, which drives the same
# tools from a container; keep the two in step.
#
# ---------------------------------------------------------------------------
# WHY THIS BUILD EXISTS. DPMI 0.9 gives 16-bit and 32-bit clients SEPARATE
# protected-mode interrupt tables (vsbhda.txt:51-57), so the 32-bit vsbpcm.exe
# cannot provide services to a 16-bit PM game -- Tyrian, and the rest of the
# Borland RTM / Phar Lap 286 / DOS16M catalogue. Both binaries serve real-mode
# games; only this one serves 16-bit PM ones. It is NOT a size optimisation.
# On the box: HDPMI16I -x instead of HDPMI32I -x, same Jemm + QPIEMU.DLL.
#
# WHAT CHANGED FROM THE UPSTREAM ow16.mak:
#  * ONE MODULE, not vsbhda16.exe + sndcard.drv. Upstream splits because its
#    driver half is six PCI drivers plus ac97mix and pcibios, and because that
#    half only ever answers the 14-entry AUEXP table (src/auexp16.asm) -- a
#    one-way interface with a stack switch and pointer translation per call.
#    Our backends reach BACK into the engine (PTOPS_Register, PTOPS_CardIs,
#    seven SNDISR_* entries, PTRAP_SetOplRing, FOpts), and three of those are
#    DATA, which no call thunk can bridge. Nothing forces the split:
#    startup/init1632.asm already sets CSGT64K=1 and gives DGROUP a 4GB limit,
#    and the linked result is ~48K of code and ~41K of data -- both inside 64K
#    anyway. One module also keeps the object list the same as djgpp.mak's,
#    which is what makes the two binaries comparable.
#    ldmod16 / libmain / dstrt16x / auimp16 / auexp16 are therefore unused.
#  * the PCMCIA backends (sc_es1688, sc_vew211, sc_tp755) replace the upstream
#    PCI drivers, exactly as in djgpp.mak; ac97mix/pcibios/sc_* stay in the
#    tree behind the upstream NOxxx guards so `git merge upstream/main` stays
#    cheap.
#  * hostsvc.c + pmisr.asm are new: the toolchain compatibility layer and the
#    chained PM interrupt trampolines that replace the go32 chain wrapper.
#    See src/hostsvc.h.
#  * -za99 on the C compiler. main.c declares after statements (C99); the
#    alternative was moving declarations around in an upstream file.
# ---------------------------------------------------------------------------

!ifndef DEBUG
DEBUG=0
!endif

!ifndef WATCOM
WATCOM=\ow20
!endif
# use OW v2 (0) or OW v1.9 (1)
!ifndef USE19
USE19=0
!endif
!ifndef USEJWL
USEJWL=1
!endif

CC=$(WATCOM)\binnt\wcc386.exe
CPP=$(WATCOM)\binnt\wpp386.exe
!if $(USEJWL)
LINK=jwlink.exe
!else
LINK=$(WATCOM)\binnt\wlink.exe
!endif
LIB=$(WATCOM)\binnt\wlib.exe
ASM=jwasm.exe

NAME=vsbpcm16

!if $(DEBUG)
OUTD=ow16d
C_DEBUG_FLAGS=-D_DEBUG -DSNDISRLOG
A_DEBUG_FLAGS=-D_DEBUG -Fl=$*
!else
OUTD=ow16
C_DEBUG_FLAGS=-D_LOG
A_DEBUG_FLAGS=
!endif

!if $(USE19)
OW19=-DOW19
!endif

# The card set and the NOxxx exclusions are config.h's business, exactly as in
# djgpp.mak; dbopl is dropped because NOFM leaves the OPL to the hardware and
# its tables would still sit in resident RAM (the PC110 budget).
OBJFILES = &
	$(OUTD)/main.obj		$(OUTD)/sndisr.obj		$(OUTD)/ptrap.obj		$(OUTD)/linear.obj		$(OUTD)/pic.obj &
	$(OUTD)/vsb.obj			$(OUTD)/vdma.obj		$(OUTD)/virq.obj		$(OUTD)/vopl3.obj		$(OUTD)/vmpu.obj		$(OUTD)/tsf.obj &
	$(OUTD)/fmvol.obj		$(OUTD)/fmshim.obj		$(OUTD)/hostsvc.obj &
	$(OUTD)/au_cards.obj	$(OUTD)/dmabuff.obj		$(OUTD)/physmem.obj		$(OUTD)/timer.obj &
	$(OUTD)/sc_es1688.obj	$(OUTD)/sc_vew211.obj	$(OUTD)/sc_tp755.obj &
	$(OUTD)/stackio.obj		$(OUTD)/stackisr.obj	$(OUTD)/sbisr.obj		$(OUTD)/int31.obj		$(OUTD)/rmwrap.obj		$(OUTD)/mixer.obj &
	$(OUTD)/hapi.obj		$(OUTD)/dprintf.obj		$(OUTD)/vioout.obj		$(OUTD)/djdpmi.obj		$(OUTD)/uninst.obj		$(OUTD)/fileacc.obj &
	$(OUTD)/pmisr.obj		$(OUTD)/rte200.obj		$(OUTD)/logfile.obj		$(OUTD)/cv1to2.obj &
	$(OUTD)/sbrk.obj		$(OUTD)/malloc.obj

C_OPT_FLAGS=-q -oxa -ms -ecc -5s -fp5 -fpi87 -wcd=111 -za99
# OW's wpp386 doesn't like the -ecc option ("function modifier cannot be used ...")
# nor -za99 (a C-only switch: it reads the 99 as a filename).
CPP_OPT_FLAGS=-q -oxa -ms -bc -5s -fp5 -fpi87
# ONEMODULE: there is no sndcard.drv boundary in this build, so the AU_*
# entry points must be NEAR, not the far exports upstream needs. See
# mpxplay/au_cards.h and doc/16bit.md.
C_EXTRA_FLAGS=-DNOTFLAT -DONEMODULE

INCLUDES=-I$(WATCOM)\h
LIBS=

{src}.asm{$(OUTD)}.obj
	@$(ASM) -q -DNOTFLAT -DONEMODULE -Istartup -D?MODEL=small $(A_DEBUG_FLAGS) -Fo$@ $<

{src}.c{$(OUTD)}.obj
	@$(CC) $(C_DEBUG_FLAGS) $(C_OPT_FLAGS) -os $(C_EXTRA_FLAGS) $(CFLAGS) -Isrc $(INCLUDES) -fo=$@ $<

{src}.cpp{$(OUTD)}.obj
	@$(CPP) $(C_DEBUG_FLAGS) $(CPP_OPT_FLAGS) -os $(C_EXTRA_FLAGS) $(CPPFLAGS) -Isrc $(INCLUDES) -fo=$@ $<

{mpxplay}.c{$(OUTD)}.obj
	@$(CC) $(C_DEBUG_FLAGS) $(C_OPT_FLAGS) $(C_EXTRA_FLAGS) $(CFLAGS) -Impxplay -Isrc $(INCLUDES) -fo=$@ $<

{startup}.asm{$(OUTD)}.obj
	@$(ASM) -q -zcw -DNOTFLAT -DONEMODULE -D?MODEL=small $(OW19) $(A_DEBUG_FLAGS) -Fo$@ $<

all: $(OUTD) $(OUTD)\$(NAME).exe

$(OUTD):
	@mkdir $(OUTD)

$(OUTD)\$(NAME).exe: $(OUTD)\$(NAME).lib $(OUTD)\cstrt16x.obj $(OUTD)\init1632.obj
	@$(LINK) @<<
format dos
file $(OUTD)\cstrt16x, $(OUTD)\main, $(OUTD)\init1632 name $@
libpath $(WATCOM)\lib386\dos;$(WATCOM)\lib386
lib $*.lib
op q,statics,m=$*.map
disable 80
<<

$(OUTD)\$(NAME).lib: $(OBJFILES)
	@$(LIB) -q -b -n $(OUTD)\$(NAME).lib $(OBJFILES)

$(OUTD)/au_cards.obj:  mpxplay\au_cards.c
$(OUTD)/dmabuff.obj:   mpxplay\dmabuff.c
$(OUTD)/physmem.obj:   mpxplay\physmem.c
$(OUTD)/timer.obj:     mpxplay\timer.c
$(OUTD)/sc_es1688.obj: mpxplay\sc_es1688.c
$(OUTD)/sc_vew211.obj: mpxplay\sc_vew211.c
$(OUTD)/sc_tp755.obj:  mpxplay\sc_tp755.c

$(OUTD)/cv1to2.obj:    src\cv1to2.asm
$(OUTD)/djdpmi.obj:    src\djdpmi.asm
$(OUTD)/dprintf.obj:   src\dprintf.asm
$(OUTD)/fileacc.obj:   src\fileacc.asm
$(OUTD)/fmshim.obj:    src\fmshim.c
$(OUTD)/fmvol.obj:     src\fmvol.c
$(OUTD)/hapi.obj:      src\hapi.asm
$(OUTD)/hostsvc.obj:   src\hostsvc.c
$(OUTD)/int31.obj:     src\int31.asm
$(OUTD)/linear.obj:    src\linear.c
$(OUTD)/logfile.obj:   src\logfile.asm
$(OUTD)/main.obj:      src\main.c
$(OUTD)/mixer.obj:     src\mixer.asm
$(OUTD)/pic.obj:       src\pic.c
$(OUTD)/pmisr.obj:     src\pmisr.asm
$(OUTD)/ptrap.obj:     src\ptrap.c
$(OUTD)/rte200.obj:    src\rte200.asm
$(OUTD)/sbisr.obj:     src\sbisr.asm
$(OUTD)/sndisr.obj:    src\sndisr.c
$(OUTD)/stackio.obj:   src\stackio.asm
$(OUTD)/stackisr.obj:  src\stackisr.asm
$(OUTD)/tsf.obj:       src\tsf.c
$(OUTD)/uninst.obj:    src\uninst.asm
$(OUTD)/vdma.obj:      src\vdma.c
$(OUTD)/vioout.obj:    src\vioout.asm
$(OUTD)/virq.obj:      src\virq.c
$(OUTD)/vmpu.obj:      src\vmpu.c
$(OUTD)/vopl3.obj:     src\vopl3.cpp
$(OUTD)/vsb.obj:       src\vsb.c

$(OUTD)/cstrt16x.obj:  startup\cstrt16x.asm
$(OUTD)/init1632.obj:  startup\init1632.asm
$(OUTD)/malloc.obj:    startup\malloc.asm
$(OUTD)/sbrk.obj:      startup\sbrk.asm

# the 16-bit code is included in binary format into rmwrap.asm.

$(OUTD)/rmwrap.obj:    src\rmwrap.asm src\rmcode1.asm src\rmcode2.asm
	@$(ASM) -q -bin -Fl$(OUTD)\ -Fo$(OUTD)\rmcode1.bin src\rmcode1.asm
	@$(ASM) -q -bin -Fl$(OUTD)\ -Fo$(OUTD)\rmcode2.bin src\rmcode2.asm
	@$(ASM) -q -DNOTFLAT -D?MODEL=small $(OW19) -Fl$(OUTD)\ -Fo$@ -DOUTD=$(OUTD) src\rmwrap.asm

clean: .SYMBOLIC
	@del $(OUTD)\$(NAME).lib
	@del $(OUTD)\$(NAME).exe
	@del $(OUTD)\*.obj
	@del $(OUTD)\*.map
	@del $(OUTD)\*.lst
	@del $(OUTD)\rmcode?.bin
