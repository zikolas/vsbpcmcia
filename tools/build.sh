#!/bin/bash
# VSBPCMCIA build: DJGPP + JWasm inside an amd64 Debian container.
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

# Card backend:   ./tools/build.sh              -> ES1688 build   (vsbpcm.exe)
#                 CARD=TP755 ./tools/build.sh   -> CS4248 build   (vsbpcmt.exe)
# CPPFLAGS carries the define too: vopl3.cpp/dbopl.cpp compile under
# $(CPPFLAGS), and without the card define they empty out under NOFM.
if [ "$CARD" = "TP755" ]; then CARDDEF="-DCARD_TP755"; OUTNAME="vsbpcmt"
else CARDDEF=""; OUTNAME="vsbpcm"; fi

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
if [ -n "$OPLDEF" ] && [ "$CARD" != "TP755" ]; then
  echo "build.sh: WARNING -- OPLGEN=$OPLGEN is inert without CARD=TP755 (NOFM strips dbopl)" >&2
fi

rm -f "$REPO"/djgpp/*.o "$REPO"/djgpp/*.ar

docker run --rm --platform linux/amd64 \
  -e CARDDEF="$CARDDEF" -e OPLDEF="$OPLDEF" -e OUTNAME="$OUTNAME" \
  -v "$REPO":/build -v "$DJGPP_DIR":/opt/djgpp -v "$JWASM_BIN":/usr/local/bin/jwasm \
  -w /build debian:stable-slim bash -c '
  apt-get update -qq >/dev/null 2>&1; apt-get install -y -qq make libfl2 >/dev/null 2>&1
  ln -sf /usr/local/bin/jwasm /usr/local/bin/jwasm.exe
  for t in /opt/djgpp/bin/i586-pc-msdosdjgpp-*; do n=$(basename "$t" | sed "s/i586-pc-msdosdjgpp-//"); ln -sf "$t" /usr/local/bin/"$n"; done
  export PATH=/opt/djgpp/bin:/usr/local/bin:$PATH
  echo "=== compiling objects ($OUTNAME) ==="
  make -f djgpp.mak CFLAGS="$CARDDEF" CPPFLAGS="$CARDDEF $OPLDEF" 2>&1 | grep -viE "warning:|note:| \^|~~~|\| |In file included" | tail -8 || true
  cd djgpp
  echo "=== manual archive + link (Linux) ==="
  OBJ=$(ls *.o 2>/dev/null | grep -v "^main.o$")
  ar rc "$OUTNAME".ar $OBJ
  g++ -o "$OUTNAME".exe main.o "$OUTNAME".ar -lm 2>&1 | tail -6
  echo "=== result ==="
  ls -la "$OUTNAME".exe 2>/dev/null && echo "BUILD OK" || echo "BUILD FAILED"
'
