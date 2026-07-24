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

rm -f "$REPO"/djgpp/*.o "$REPO"/djgpp/*.ar

docker run --rm --platform linux/amd64 \
  -v "$REPO":/build -v "$DJGPP_DIR":/opt/djgpp -v "$JWASM_BIN":/usr/local/bin/jwasm \
  -w /build debian:stable-slim bash -c '
  apt-get update -qq >/dev/null 2>&1; apt-get install -y -qq make libfl2 >/dev/null 2>&1
  ln -sf /usr/local/bin/jwasm /usr/local/bin/jwasm.exe
  for t in /opt/djgpp/bin/i586-pc-msdosdjgpp-*; do n=$(basename "$t" | sed "s/i586-pc-msdosdjgpp-//"); ln -sf "$t" /usr/local/bin/"$n"; done
  export PATH=/opt/djgpp/bin:/usr/local/bin:$PATH
  echo "=== compiling objects ==="
  make -f djgpp.mak 2>&1 | grep -viE "warning:|note:| \^|~~~|\| |In file included" | tail -8 || true
  cd djgpp
  echo "=== manual archive + link (Linux) ==="
  OBJ=$(ls *.o 2>/dev/null | grep -v "^main.o$")
  ar rc vsbpcm.ar $OBJ
  g++ -o vsbpcm.exe main.o vsbpcm.ar -lm 2>&1 | tail -6
  echo "=== result ==="
  ls -la vsbpcm.exe 2>/dev/null && echo "BUILD OK" || echo "BUILD FAILED"
'
