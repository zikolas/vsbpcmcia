#!/bin/bash
# VSBPCMCIA build: DJGPP + JWasm inside an amd64 Debian container.
#
# Card backend:   ./tools/build.sh              -> ES1688 build   (vsbpcm.exe)
#                 CARD=VEW211 ./tools/build.sh  -> CS4231A build  (vsbpcmv.exe)
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
# The tree must be case-normalized (lowercase filenames + #include directives) --
# already done in this repo. The makefile's DOS link stage (copy /b, del) dies
# under Linux, which is EXPECTED: objects are archived + linked manually below.
# The default DJGPP stub is used (no res/stub.bin swap needed -- verified on
# hardware). Always a CLEAN build: stale objects have bitten before (the tsf.o
# config.h dependency gap).
set -e
REPO="$(cd "$(dirname "$0")/.." && pwd)"
DJGPP_DIR="${DJGPP_DIR:-$HOME/djgpp}"
JWASM_BIN="${JWASM_BIN:-$HOME/vsbhda-tools/jwasm}"

if [ "$CARD" = "VEW211" ]; then
  CARDDEF="-DCARD_VEW211"
  OUTNAME="vsbpcmv"
elif [ "$CARD" = "AUDIGY" ]; then
  CARDDEF="-DCARD_AUDIGY"
  OUTNAME="vsbpcma"
else
  CARDDEF=""
  OUTNAME="vsbpcm"
fi

rm -f "$REPO"/djgpp/*.o "$REPO"/djgpp/*.ar

docker run --rm --platform linux/amd64 \
  -e CARDDEF="$CARDDEF" -e OUTNAME="$OUTNAME" \
  -v "$REPO":/build -v "$DJGPP_DIR":/opt/djgpp -v "$JWASM_BIN":/usr/local/bin/jwasm \
  -w /build debian:stable-slim bash -c '
  apt-get update -qq >/dev/null 2>&1; apt-get install -y -qq make libfl2 >/dev/null 2>&1
  ln -sf /usr/local/bin/jwasm /usr/local/bin/jwasm.exe
  for t in /opt/djgpp/bin/i586-pc-msdosdjgpp-*; do n=$(basename "$t" | sed "s/i586-pc-msdosdjgpp-//"); ln -sf "$t" /usr/local/bin/"$n"; done
  export PATH=/opt/djgpp/bin:/usr/local/bin:$PATH
  echo "=== compiling objects (${CARDDEF:-ES1688 default}) ==="
  # CPPFLAGS too, not just CFLAGS: vopl3.cpp/dbopl.cpp use $(CPPFLAGS), and
  # without the card define they compile away under NOFM -> undefined VOPL3_*.
  make -f djgpp.mak CFLAGS="$CARDDEF" CPPFLAGS="$CARDDEF" 2>&1 | grep -viE "warning:|note:| \^|~~~|\| |In file included" | tail -8 || true
  cd djgpp
  echo "=== manual archive + link (Linux) ==="
  OBJ=$(ls *.o 2>/dev/null | grep -v "^main.o$")
  ar rc "$OUTNAME".ar $OBJ
  g++ -o "$OUTNAME".exe main.o "$OUTNAME".ar -lm 2>&1 | tail -6
  echo "=== result ==="
  ls -la "$OUTNAME".exe 2>/dev/null && echo "BUILD OK" || echo "BUILD FAILED"
'
