#!/bin/bash
# VSBPCMCIA build: DJGPP + JWasm inside an amd64 Debian container.
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

rm -f "$REPO"/djgpp/*.o "$REPO"/djgpp/*.ar

docker run --rm --platform linux/amd64 \
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

  echo "=== compiling objects ==="
  make -f djgpp.mak 2>&1 | grep -viE "warning:|note:| \^|~~~|\| |In file included" | tail -8 || true
  cd djgpp
  echo "=== manual archive + link (Linux) ==="
  OBJ=$(ls *.o 2>/dev/null | grep -v "^main.o$")
  ar rc vsbpcm.ar $OBJ
  g++ -o vsbpcm.exe main.o vsbpcm.ar -lm 2>&1 | tail -6
  echo "=== result ==="
  if [ -f vsbpcm.exe ]; then
    mkdir -p /build/djgpp && cp vsbpcm.exe /build/djgpp/vsbpcm.exe
    ls -la /build/djgpp/vsbpcm.exe && echo "BUILD OK"
  else
    echo "BUILD FAILED"
  fi
'
