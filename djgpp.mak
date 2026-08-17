
# create vsbhdad.exe with DJGPP and JWasm.
# to create a debug version, enter: make -f djgpp.mak DEBUG=1
# note that JWasm v2.17+ is needed ( understands -djgpp option )
#
# Please note: DJGPP uses CWSDPMI as its DPMI host - this works
# fine so long as just DJGPP tools are launched. However, JWasm is no
# such tool - it expects to run under a "full" DPMI host, which CWSDPMI is not.
# This may cause all sorts of errors if JWasm is launched by DJGPP's make.
# The simplest workaround is to run HDPMI32.EXE with option -r before
# DJGPP's make tool is executed.

ifndef DEBUG
DEBUG=0
endif

NAME=vsbpcm

ifeq ($(DEBUG),1)
OUTD=djgppd
C_DEBUG_FLAGS=-D_DEBUG
A_DEBUG_FLAGS=-D_DEBUG -Fl=$(OUTD)/
else
OUTD=djgpp
C_DEBUG_FLAGS=
A_DEBUG_FLAGS=
endif

vpath_src=src mpxplay
vpath %.c $(vpath_src)
vpath %.cpp $(vpath_src)
vpath %.asm $(vpath_src)
vpath_header=src mpxplay
vpath %.h $(vpath_header)
vpath_obj=./$(OUTD)/
vpath %.o $(vpath_obj)

# VSBPCMCIA: PCI card drivers (sc_e1371/ich/inthd/via82/sbliv/sbl24), ac97mix
# and pcibios are excluded -- PCMCIA-only build, see NOxxx defines in config.h.
# The sources stay in the tree for cheap upstream merges.
# dbopl.o dropped too: under NOFM vopl3.cpp never references DBOPL, and the
# OPL engine's tables/objects would still occupy resident RAM (PC110 budget).
OBJFILES=\
	$(OUTD)/main.o		$(OUTD)/sndisr.o	$(OUTD)/ptrap.o		$(OUTD)/linear.o	$(OUTD)/pic.o\
	$(OUTD)/vsb.o		$(OUTD)/vdma.o		$(OUTD)/virq.o		$(OUTD)/vopl3.o		$(OUTD)/vmpu.o		$(OUTD)/tsf.o\
	$(OUTD)/au_cards.o\
	$(OUTD)/dmabuff.o	$(OUTD)/physmem.o	$(OUTD)/timer.o\
	$(OUTD)/sc_es1688.o	$(OUTD)/sc_vew211.o	$(OUTD)/sc_tp755.o	$(OUTD)/fmvol.o	$(OUTD)/fmshim.o\
	$(OUTD)/stackio.o	$(OUTD)/stackisr.o	$(OUTD)/sbisr.o		$(OUTD)/int31.o		$(OUTD)/rmwrap.o	$(OUTD)/mixer.o\
	$(OUTD)/hapi.o		$(OUTD)/dprintf.o	$(OUTD)/vioout.o	$(OUTD)/djdpmi.o	$(OUTD)/uninst.o	$(OUTD)/fileacc.o

# CARD_AUDIGY build: pull the SB Live/Audigy driver and its PCI plumbing back
# in (for the CardBus Audigy 2 ZS Notebook, which is a PCI device, not PCMCIA).
# sc_sbliv.c needs emu_driver_audigyls/live24_funcs from sc_sbl24.c and
# aucards_ac97chan_mixerset from ac97mix.c, so all three are required, plus
# pcibios.c for the PCI enumeration.
# dbopl.o comes back too: CARD_AUDIGY clears NOFM (the Audigy has no hardware
# OPL at 0x388 to fall through to), so vopl3.cpp references DBOPL again.
ifneq (,$(findstring CARD_AUDIGY,$(CFLAGS)))
OBJFILES+= $(OUTD)/pcibios.o	$(OUTD)/ac97mix.o	$(OUTD)/sc_sbliv.o	$(OUTD)/sc_sbl24.o	$(OUTD)/dbopl.o
# SF2 soundfonts played on the EMU10K2's own 64 hardware voices
OBJFILES+= $(OUTD)/emu_sf2.o	$(OUTD)/emu_wt.o
endif

# CARD_TP755: the 755C build keeps the OPL3 emulation (no FM hardware on
# that machine -- see config.h), so dbopl.o comes back for it only.
ifneq (,$(findstring CARD_TP755,$(CFLAGS)))
OBJFILES+= $(OUTD)/dbopl.o
endif

INCLUDE_DIRS=src mpxplay
SRC_DIRS=src mpxplay

C_OPT_FLAGS=-Os -fno-asynchronous-unwind-tables
# ES1688 port: i486, not i586 -- this stack exists FOR the 486 fleet (vdpmi
# covers Pentiums); i586 scheduling is worthless there and risks 586-isms.
C_EXTRA_FLAGS=-march=i486
LD_FLAGS=$(addprefix -Xlinker ,$(LD_EXTRA_FLAGS))
LD_EXTRA_FLAGS=-Map $(OUTD)/$(NAME).map

INCLUDES=$(addprefix -I,$(INCLUDE_DIRS))
LIBS=$(addprefix -l,stdcxx m)

# $(CFLAGS) carries only the card define (-DCARD_AUDIGY etc.), so the ASM side
# can see which card is being built (rmcode1.asm's v86 OPL fast-path is
# CARD_TP755-gated); with CFLAGS empty the command line is unchanged and the
# default build stays byte-identical.
COMPILE.asm.o=jwasm.exe -q -djgpp -Istartup -D?MODEL=small -DDJGPP $(CFLAGS) $(A_DEBUG_FLAGS) -Fo=$@ $<
COMPILE.c.o=gcc $(C_DEBUG_FLAGS) $(C_OPT_FLAGS) $(C_EXTRA_FLAGS) $(CFLAGS) $(INCLUDES) -c $< -o $@
COMPILE.cpp.o=gcc $(C_DEBUG_FLAGS) $(C_OPT_FLAGS) $(C_EXTRA_FLAGS) $(CPPFLAGS) $(INCLUDES) -c $< -o $@

$(OUTD)/%.o: src/%.c
	$(COMPILE.c.o)

$(OUTD)/%.o: src/%.cpp
	$(COMPILE.cpp.o)

$(OUTD)/%.o: src/%.asm
	$(COMPILE.asm.o)

$(OUTD)/%.o: mpxplay/%.c
	$(COMPILE.c.o)

all:: $(OUTD) $(OUTD)/$(NAME)d.exe

$(OUTD):
	@mkdir $(OUTD)

$(OUTD)/$(NAME)d.exe:: $(OUTD)/$(NAME).ar
	gcc -o $@ $(OUTD)/main.o $(OUTD)/$(NAME).ar $(LD_FLAGS) $(LIBS)
	strip -s $@
	exe2coff $@
	copy /b res\stub.bin + $(OUTD)\$(NAME)d $(OUTD)\$(NAME)d.exe

$(OUTD)/$(NAME).ar:: $(OBJFILES)
	ar --target=coff-go32 r $(OUTD)/$(NAME).ar $(OBJFILES)

# to avoid problems with 16-bit relocations, the 16-bit code
# is included in binary format into rmwrap.asm.

$(OUTD)/rmwrap.o:: rmwrap.asm rmcode1.asm rmcode2.asm
	jwasm.exe -q -bin $(CFLAGS) -Fl$(OUTD)/ -Fo$(OUTD)/rmcode1.bin src/rmcode1.asm
	jwasm.exe -q -bin $(CFLAGS) -Fl$(OUTD)/ -Fo$(OUTD)/rmcode2.bin src/rmcode2.asm
	jwasm.exe -q -djgpp -D?MODEL=small -DOUTD=$(OUTD) -Fo$@ src/rmwrap.asm

$(OUTD)/ac97mix.o::  ac97mix.c   mpxplay.h au_cards.h ac97mix.h
$(OUTD)/au_cards.o:: au_cards.c  mpxplay.h au_cards.h dmabuff.h config.h
$(OUTD)/dmabuff.o::  dmabuff.c   mpxplay.h au_cards.h dmabuff.h
$(OUTD)/pcibios.o::  pcibios.c   pcibios.h
$(OUTD)/physmem.o::  physmem.c
$(OUTD)/sc_e1371.o:: sc_e1371.c  mpxplay.h au_cards.h dmabuff.h pcibios.h ac97mix.h
$(OUTD)/sc_ich.o::   sc_ich.c    mpxplay.h au_cards.h dmabuff.h pcibios.h ac97mix.h
$(OUTD)/sc_inthd.o:: sc_inthd.c  mpxplay.h au_cards.h dmabuff.h pcibios.h sc_inthd.h
$(OUTD)/sc_sbl24.o:: sc_sbl24.c  mpxplay.h au_cards.h dmabuff.h pcibios.h ac97mix.h sc_sbl24.h emu10k1.h
$(OUTD)/sc_sbliv.o:: sc_sbliv.c  mpxplay.h au_cards.h dmabuff.h pcibios.h ac97mix.h sc_sbliv.h emu10k1.h
$(OUTD)/sc_via82.o:: sc_via82.c  mpxplay.h au_cards.h dmabuff.h pcibios.h ac97.h
$(OUTD)/sc_es1688.o:: sc_es1688.c au_cards.h config.h ptops.h
$(OUTD)/sc_vew211.o:: sc_vew211.c au_cards.h config.h ptops.h
$(OUTD)/sc_tp755.o:: sc_tp755.c au_cards.h dmabuff.h config.h ptops.h
$(OUTD)/timer.o::    timer.c     mpxplay.h au_cards.h timer.h

$(OUTD)/dbopl.o::    dbopl.cpp   dbopl.h
$(OUTD)/linear.o::   linear.c    linear.h platform.h
$(OUTD)/main.o::     main.c      linear.h platform.h ptrap.h vopl3.h pic.h config.h vsb.h vdma.h virq.h au.h version.h ptops.h
$(OUTD)/pic.o::      pic.c       pic.h platform.h ptrap.h
$(OUTD)/ptrap.o::    ptrap.c     linear.h platform.h ptrap.h config.h fmshim.h ptops.h
$(OUTD)/fmvol.o::    fmvol.c     platform.h ptrap.h fmvol.h
$(OUTD)/fmshim.o::   fmshim.c    platform.h ptrap.h fmshim.h config.h
$(OUTD)/sndisr.o::   sndisr.c    linear.h platform.h vopl3.h pic.h config.h vsb.h vdma.h virq.h ctadpcm.h au.h ptops.h
$(OUTD)/tsf.o::      tsf.c       tsf/tsf.h
$(OUTD)/vdma.o::     vdma.c      linear.h platform.h ptrap.h vdma.h config.h
$(OUTD)/virq.o::     virq.c      linear.h platform.h pic.h ptrap.h virq.h config.h
$(OUTD)/vopl3.o::    vopl3.cpp   dbopl.h vopl3.h config.h
$(OUTD)/vsb.o::      vsb.c       linear.h platform.h vsb.h config.h ptops.h
$(OUTD)/vmpu.o::     vmpu.c      linear.h platform.h vmpu.h config.h

$(OUTD)/djdpmi.o::   djdpmi.asm
$(OUTD)/dprintf.o::  dprintf.asm
$(OUTD)/fileacc.o::  fileacc.asm
$(OUTD)/hapi.o::     hapi.asm
$(OUTD)/int31.o::    int31.asm
$(OUTD)/mixer.o::    mixer.asm
$(OUTD)/sbisr.o::    sbisr.asm
$(OUTD)/stackio.o::  stackio.asm
$(OUTD)/stackisr.o:: stackisr.asm
$(OUTD)/uninst.o::   uninst.asm
$(OUTD)/vioout.o::   vioout.asm

clean::
	del $(OUTD)\$(NAME)d.exe
	del $(OUTD)\$(NAME).ar
	del $(OUTD)\*.o

