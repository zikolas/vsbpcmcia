#!/bin/bash
# VSBPCM16 build: Open Watcom (wcc386/wpp386/wlib/wlink) + JWasm in a container.
#
# This is the 16-BIT PROTECTED-MODE half of the project, and it exists for one
# reason only (vsbhda.txt:51-57): DPMI 0.9 gives 16-bit and 32-bit clients
# SEPARATE protected-mode interrupt tables, so the 32-bit vsbpcm.exe cannot
# provide services to a 16-bit PM game -- Tyrian, and every other Borland RTM /
# Phar Lap 286 / DOS16M title. Both binaries serve real-mode games; only this
# one serves 16-bit PM ones. It is NOT a size optimisation.
#
#   ./tools/build16.sh              -> ow16/vsbpcm16.exe   (ONE module)
#   DIAG=1 ./tools/build16.sh       -> the same + STKDIAG: survivable
#                                      stack-check trip, PMISR depth probes
#
# ABOUT THE TARGET. Despite the name, the generated code is 32-bit (.386,
# USE32 segments -- see startup/cstrt16x.asm, "DOS 32-bit startup code for
# 16-bit client"). What is 16-bit is the DPMI *client type*: the host is
# entered through its 16:16 API entry, so interrupt and callback frames are
# 16-bit and the host files us in its 16-bit interrupt table. Note that DS
# still has a full 4GB limit (init1632.asm sets it -- "for linear access, we
# need full 4GB descriptor"), so NearPtr reaches low memory exactly as under
# DJGPP; the 64K figures in vsbhda.txt are about the MZ module format and
# the upstream exe+drv layout, not the runtime descriptors. See doc/16bit.md.
#
# Host prerequisites (paths overridable via env):
#   OW_DIR    - Open Watcom v2 snapshot (needs binl64/ and h/), default
#               ~/vsbhda-tools/ow20; fetch ow-snapshot.tar.xz from the
#               open-watcom-v2 "Current-build" release.
#   JWASM_BIN - a Linux JWasm binary, default ~/vsbhda-tools/jwasm (same one
#               tools/build.sh uses).
#
# CASE HANDLING and the throwaway /work copy: identical to tools/build.sh, and
# for the same reason -- upstream tracks src/*.C in uppercase, the makefiles
# say lowercase, and that is self-consistent only on a case-insensitive host.
# The repo is never modified.
#
# wmake is NOT used: ow16.mak spells its tool paths and its output paths the
# DOS way ($(WATCOM)\binnt\wcc386.exe, $(OUTD)\name.exe), which no amount of
# macro overriding survives on Linux. The object list and every flag below are
# transcribed from ow16.mak; keep them in step with it.
set -e
REPO="$(cd "$(dirname "$0")/.." && pwd)"
OW_DIR="${OW_DIR:-$HOME/vsbhda-tools/ow20}"
JWASM_BIN="${JWASM_BIN:-$HOME/vsbhda-tools/jwasm}"

# SAFETY: REPO comes from $0, so a copy of this script placed elsewhere would
# resolve REPO to that directory's parent -- from /tmp that is "/", and the
# docker run below would mount the entire filesystem. (Done exactly once,
# 2026-08-17.) Refuse unless REPO really is this repo. See tools/build.sh.
# djgpp.mak, not OW16.mak: upstream tracks that one in uppercase, so a clone
# on a case-sensitive filesystem would fail this test for the wrong reason.
if [ ! -f "$REPO/djgpp.mak" ] || [ ! -d "$REPO/src" ]; then
  echo "build16.sh: refusing to run -- '$REPO' is not the VSBPCMCIA repo" >&2
  echo "  (no djgpp.mak / src). Run tools/build16.sh from inside the repo;" >&2
  echo "  do not copy this script elsewhere -- REPO comes from its own path." >&2
  exit 1
fi
for d in "$OW_DIR/binl64" "$OW_DIR/h"; do
  [ -d "$d" ] || { echo "build16.sh: missing $d -- set OW_DIR to an Open Watcom v2 snapshot" >&2; exit 1; }
done
[ -f "$JWASM_BIN" ] || { echo "build16.sh: missing $JWASM_BIN -- set JWASM_BIN" >&2; exit 1; }

docker run --rm --platform linux/amd64 \
  -e DIAG="$DIAG" \
  -v "$REPO":/build -v "$OW_DIR":/ow -v "$JWASM_BIN":/usr/local/bin/jwasm \
  -w /build debian:stable-slim bash -c '
set -e
export PATH=/ow/binl64:$PATH
export WATCOM=/ow

echo "=== case-normalising a throwaway copy (repo untouched) ==="
rm -rf /work; mkdir -p /work
tar -C /build --exclude=./.git --exclude=./djgpp --exclude=./ow16 -cf - . | tar -C /work -xf -
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

OUTD=ow16
rm -rf $OUTD; mkdir -p $OUTD

# --- flags, transcribed from ow16.mak ------------------------------------
DBG="-D_LOG"
COPT="-q -oxa -ms -ecc -5s -fp5 -fpi87 -wcd=111 -za99"
CPPOPT="-q -oxa -ms -bc -5s -fp5 -fpi87"   # no -za99: that is a C-only switch, wpp386 reads the 99 as a filename
CEXTRA="-DNOTFLAT -DONEMODULE ${DIAG:+-DSTKDIAG}"   # ONEMODULE: no sndcard.drv boundary, so the
                                # AU_* entry points are NEAR (see au_cards.h)
INC="-I/ow/h"
AFLAGS="-q -DNOTFLAT -DONEMODULE ${DIAG:+-DSTKDIAG} -Istartup -D?MODEL=small"

cc()  { wcc386 $DBG $COPT -os $CEXTRA $CARDDEF -Isrc $INC -fo=$OUTD/$2 src/$1; }
ccx() { wcc386 $DBG $COPT     $CEXTRA $CARDDEF -Impxplay -Isrc $INC -fo=$OUTD/$2 mpxplay/$1; }
cpp() { wpp386 $DBG $CPPOPT -os $CEXTRA $CARDDEF -Isrc $INC -fo=$OUTD/$2 src/$1; }
asm() { jwasm $AFLAGS $CARDDEF -Fo=$OUTD/$2 src/$1; }
sasm(){ jwasm -q -zcw -DNOTFLAT -D?MODEL=small -Fo=$OUTD/$2 startup/$1; }

fail=0
try() { local out; if out=$("$@" 2>&1); then :; else fail=1; fi; [ -z "$out" ] || echo "$out"; }

# ONE MODULE, not the upstream vsbhda16.exe + sndcard.drv pair. Upstream
# splits because its driver half is six PCI drivers plus ac97mix and
# pcibios, and because that half only ever answers the 14-entry AUEXP
# table (src/auexp16.asm) -- a strictly one-way interface with a stack
# switch and pointer translation on every call. Our backends are not
# shaped like that: they reach BACK into the engine for PTOPS_Register,
# PTOPS_CardIs, seven SNDISR_* entries, PTRAP_SetOplRing and the FOpts
# option block -- and SNDISR_HasTsc, SNDISR_ReviveSquelch and FOpts are
# DATA, which no call thunk can bridge. Splitting would mean inventing a
# reverse import table with pointer translation for engine globals.
# Nothing forces the split: startup/init1632.asm already sets CSGT64K=1
# ("_TEXT may exceed 64k") and gives DGROUP a 4GB limit, so the 64K figure in
# vsbhda.txt describes the upstream module layout, not a hard ceiling. One
# module also keeps the object set the same as the djgpp.mak one, which is
# what makes the two binaries comparable when the bench comes back.
echo "=== C objects ==="
for f in main sndisr ptrap linear pic vsb vdma virq vmpu tsf fmvol fmshim hostsvc; do try cc $f.c $f.obj; done
try cpp vopl3.cpp vopl3.obj
echo "=== card objects ==="
for f in au_cards dmabuff physmem timer sc_es1688 sc_vew211 sc_scp55 sc_tp755; do try ccx $f.c $f.obj; done
echo "=== asm objects ==="
for f in stackio stackisr sbisr int31 mixer hapi dprintf vioout djdpmi uninst fileacc pmisr rte200 logfile cv1to2; do
  try asm $f.asm $f.obj
done
for f in malloc sbrk; do try sasm $f.asm $f.obj; done
try sasm cstrt16x.asm cstrt16x.obj
try sasm init1632.asm init1632.obj

# the 16-bit real-mode blobs are included in binary form by rmwrap.asm
echo "=== rmwrap (binary blobs) ==="
try jwasm -q -bin $CARDDEF -Fl$OUTD/ -Fo$OUTD/rmcode1.bin src/rmcode1.asm
try jwasm -q -bin $CARDDEF -Fl$OUTD/ -Fo$OUTD/rmcode2.bin src/rmcode2.asm
try jwasm -q -DNOTFLAT -D?MODEL=small -DOUTD=$OUTD -Fo=$OUTD/rmwrap.obj src/rmwrap.asm

if [ $fail -ne 0 ]; then echo "=== COMPILE FAILED ==="; exit 1; fi

echo "=== link ==="
OBJ="main sndisr ptrap linear pic vsb vdma virq vmpu tsf fmvol fmshim hostsvc vopl3 \
  au_cards dmabuff physmem timer sc_es1688 sc_vew211 sc_scp55 sc_tp755 \
  stackio stackisr sbisr int31 mixer hapi dprintf vioout djdpmi uninst fileacc \
  pmisr rte200 logfile cv1to2 rmwrap malloc sbrk"

cd $OUTD
wlib -q -b -n vsbpcm16.lib $(for o in $OBJ; do printf "%s.obj " $o; done)
# ow16.mak feeds these directives to wlink as an inline file (@<<); that is a
# wmake facility, so here they go through a real directive file instead.
cat > vsbpcm16.lnk <<EOF
format dos
file cstrt16x.obj, main.obj, init1632.obj name vsbpcm16.exe   # .obj spelled out: wlink on Linux defaults to .o
libpath /ow/lib386/dos;/ow/lib386
lib vsbpcm16.lib
op q,statics,m=vsbpcm16.map
disable 80
EOF
wlink @vsbpcm16.lnk

echo "=== result ==="
if [ -f vsbpcm16.exe ]; then
  mkdir -p /build/ow16 && cp vsbpcm16.exe vsbpcm16.map /build/ow16/
  ls -la /build/ow16/vsbpcm16.exe && echo "BUILD OK"
else
  echo "BUILD FAILED"; exit 1
fi
'
