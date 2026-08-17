#!/bin/bash
# VSBPCMCIA build: DJGPP + JWasm inside an amd64 Debian container.
#
# Card backend:   ./tools/build.sh              -> ES1688 build   (vsbpcm.exe)
#                 CARD=VEW211 ./tools/build.sh  -> CS4231A build  (vsbpcmv.exe)
#                 CARD=TP755 ./tools/build.sh   -> CS4248 build   (vsbpcmt.exe)
#                 CARD=AUDIGY ./tools/build.sh  -> Audigy build   (vsbpcma.exe)
#
# CARD=AUDIGY is the odd one out: it keeps the ES1688 PCMCIA backend but ALSO
# links the SB Live/Audigy driver (sc_sbliv + sc_sbl24 + pcibios + ac97mix) for
# the CardBus Audigy 2 ZS Notebook, which is a PCI card rather than PCMCIA and
# so needs CBGO/CBINIT to bring its socket up first.
#
# Host prerequisites (paths overridable via env):
#   DJGPP_DIR - a DJGPP cross toolchain (i586-pc-msdosdjgpp-*), default ~/djgpp
#   JWASM_BIN - a Linux JWasm binary (build: git clone JWasm; make -f GccUnix.mak),
#               default ~/vsbhda-tools/jwasm
#
# CASE HANDLING: upstream tracks src/*.C and src/*.H in UPPERCASE while djgpp.mak
# refers to them in lowercase. That is self-consistent only on a case-INSENSITIVE
# filesystem (DOS, Windows, macOS) -- which is where upstream builds. This container
# is case-SENSITIVE, so we copy the tree to /work and lowercase filenames and the
# #include directives THERE. The repo is never modified: no case-renorm commit, and
# upstream's files keep upstream's contents, so `git merge upstream/main` stays cheap
# (the same reason the unused PCI drivers are still in-tree behind upstream's own
# NOxxx guards). A fresh clone therefore builds with no manual preparation.
#
# The makefile's DOS link stage (copy /b, del) dies under Linux, which is EXPECTED:
# objects are archived + linked manually below. The default DJGPP stub is used (no
# res/stub.bin swap needed -- verified on hardware). Always a CLEAN build: stale
# objects have bitten before (the tsf.o config.h dependency gap).
set -e
REPO="$(cd "$(dirname "$0")/.." && pwd)"
DJGPP_DIR="${DJGPP_DIR:-$HOME/djgpp}"
JWASM_BIN="${JWASM_BIN:-$HOME/vsbhda-tools/jwasm}"

# SAFETY: REPO is derived from $0, so a COPY of this script placed anywhere else
# resolves REPO to that directory's parent -- from /tmp that is "/", and the
# docker run below would mount the ENTIRE FILESYSTEM as /build and tar it.
# (Done exactly once, 2026-08-17, complete with macOS privacy prompts.)
# Refuse to run unless REPO really is this repo.
if [ ! -f "$REPO/djgpp.mak" ] || [ ! -d "$REPO/src" ]; then
  echo "build.sh: refusing to run -- '$REPO' is not the VSBPCMCIA repo" >&2
  echo "  (no djgpp.mak / src). Run tools/build.sh from inside the repo;" >&2
  echo "  do not copy this script elsewhere -- REPO comes from its own path." >&2
  exit 1
fi

if [ "$CARD" = "VEW211" ]; then
  CARDDEF="-DCARD_VEW211"
  OUTNAME="vsbpcmv"
elif [ "$CARD" = "TP755" ]; then
  CARDDEF="-DCARD_TP755"
  OUTNAME="vsbpcmt"
elif [ "$CARD" = "AUDIGY" ]; then
  CARDDEF="-DCARD_AUDIGY"
  OUTNAME="vsbpcma"
else
  CARDDEF=""
  OUTNAME="vsbpcm"
fi

# OPL wave generator (dbopl's DBOPL_WAVE, see src/dbopl.h):
#   OPLGEN unset|TABLEMUL -> upstream default, one 16x16 IMUL per operator per
#                            sample. Tuned for a Pentium's fast multiplier.
#   OPLGEN=TABLELOG       -> multiply-free: log-domain add + ExpTable lookup.
#                            A 486 IMUL is 13-26 clocks vs 1-3 for a shift, so
#                            this is the one to bench on the fleet.
#   OPLGEN=HANDLER        -> smallest tables (SinTable+ExpTable only), per
#                            waveform function pointer. Third bench point:
#                            less table pressure, indirect call per sample.
# Suffix keeps every variant a distinct 8.3 name so they can sit on one box
# side by side:  vsbpcmt.exe / vsbpcmtl.exe / vsbpcmth.exe
case "${OPLGEN:-TABLEMUL}" in
  TABLEMUL) OPLDEF="" ;;
  TABLELOG) OPLDEF="-DDBOPL_WAVE=WAVE_TABLELOG"; OUTNAME="${OUTNAME}l" ;;
  HANDLER)  OPLDEF="-DDBOPL_WAVE=WAVE_HANDLER";  OUTNAME="${OUTNAME}h" ;;
  *) echo "build.sh: unknown OPLGEN='$OPLGEN' (TABLEMUL|TABLELOG|HANDLER)" >&2; exit 1 ;;
esac
# OPLDEF rides CPPFLAGS only -- CFLAGS also feeds jwasm, which has no business
# seeing a C++ enum name. NOFM builds compile no OPL core at all, so the flag
# is inert there; say so rather than shipping a silently identical binary.
# (CARD=AUDIGY also compiles dbopl -- see djgpp.mak -- so it is NOT inert there.)
if [ -n "$OPLDEF" ] && [ "$CARD" != "TP755" ] && [ "$CARD" != "AUDIGY" ]; then
  echo "build.sh: WARNING -- OPLGEN=$OPLGEN is inert without CARD=TP755/AUDIGY (NOFM strips dbopl)" >&2
fi

rm -f "$REPO"/djgpp/*.o "$REPO"/djgpp/*.ar

docker run --rm --platform linux/amd64 \
  -e CARDDEF="$CARDDEF" -e OPLDEF="$OPLDEF" -e OUTNAME="$OUTNAME" \
  -v "$REPO":/build -v "$DJGPP_DIR":/opt/djgpp -v "$JWASM_BIN":/usr/local/bin/jwasm \
  -w /build debian:stable-slim bash -c '
  set -e
  apt-get update -qq >/dev/null 2>&1; apt-get install -y -qq make libfl2 >/dev/null 2>&1
  ln -sf /usr/local/bin/jwasm /usr/local/bin/jwasm.exe
  for t in /opt/djgpp/bin/i586-pc-msdosdjgpp-*; do n=$(basename "$t" | sed "s/i586-pc-msdosdjgpp-//"); ln -sf "$t" /usr/local/bin/"$n"; done
  export PATH=/opt/djgpp/bin:/usr/local/bin:$PATH

  echo "=== case-normalising a throwaway copy (repo untouched) ==="
  rm -rf /work; mkdir -p /work
  tar -C /build --exclude=./.git -cf - . | tar -C /work -xf -
  cd /work
  find . -depth -type d | while read -r d; do
    b=$(basename "$d"); l=$(printf %s "$b" | tr "A-Z" "a-z")
    [ "$b" = "$l" ] || mv "$d" "$(dirname "$d")/$l"
  done
  find . -type f | while read -r f; do
    b=$(basename "$f"); l=$(printf %s "$b" | tr "A-Z" "a-z")
    [ "$b" = "$l" ] || mv "$f" "$(dirname "$f")/$l"
  done
  find . -type f \( -name "*.c" -o -name "*.cpp" -o -name "*.h" -o -name "*.hpp" -o -name "*.asm" -o -name "*.inc" \) -print0 \
    | xargs -0 sed -i -E "s/(#[[:space:]]*include[[:space:]]*\")([^\"]+)(\")/\1\L\2\E\3/"
  rm -f djgpp/*.o djgpp/*.ar

  echo "=== compiling objects ($OUTNAME, ${CARDDEF:-ES1688 default}) ==="
  # CPPFLAGS too, not just CFLAGS: vopl3.cpp/dbopl.cpp use $(CPPFLAGS), and
  # without the card define they compile away under NOFM -> undefined VOPL3_*.
  make -f djgpp.mak CFLAGS="$CARDDEF" CPPFLAGS="$CARDDEF $OPLDEF" 2>&1 | grep -viE "warning:|note:| \^|~~~|\| |In file included" | tail -8 || true
  cd djgpp
  echo "=== manual archive + link (Linux) ==="
  OBJ=$(ls *.o 2>/dev/null | grep -v "^main.o$")
  ar rc "$OUTNAME".ar $OBJ
  g++ -o "$OUTNAME".exe main.o "$OUTNAME".ar -lm 2>&1 | tail -6
  # the DOS link stage in djgpp.mak strips; this manual replacement must too,
  # or ~46% of the shipped exe is non-loadable DWARF (measured on v0.6.1).
  strip -s "$OUTNAME".exe
  echo "=== result ==="
  if [ -f "$OUTNAME".exe ]; then
    mkdir -p /build/djgpp && cp "$OUTNAME".exe /build/djgpp/"$OUTNAME".exe
    ls -la /build/djgpp/"$OUTNAME".exe && echo "BUILD OK"
  else
    echo "BUILD FAILED"
  fi
'
