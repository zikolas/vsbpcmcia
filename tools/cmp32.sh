#!/bin/bash
# Prove the 16-bit port did not disturb the 32-bit binary.
#
#   ./tools/cmp32.sh <pre-port-tree>
#
# Builds this repo and a reference tree with the SAME toolchain and compares
# every object's .text instruction stream with symbol names, literals and
# branch targets masked -- so a difference means the generated CODE changed,
# not that a static was renamed or a function moved.
#
# WHY THIS EXISTS. The 32-bit vsbpcm.exe is bench-proven across the fleet and
# the 16-bit one is not; with the bench down, "the shipping binary still
# generates the same code" is the only safety property that can be checked at
# all. See the invariant note at the top of src/hostsvc.h for the result.
#
# Building a reference tree: copy the repo, restore the pre-port versions of
# the touched files, and delete src/hostsvc.[hc], src/hostisr.h, src/pmisr.asm.
# Confirm it is really the right reference by checking that tools/build.sh in
# it reproduces the shipping sha256 before trusting any comparison.
#
# Objects built from .asm always differ byte-wise between two trees (jwasm
# records the source path), which is why this compares disassembly and not
# bytes.
#
# Reading the attribution: inter-function padding disassembles as junk, so a
# function that merely SHIFTED can be named alongside one that really changed.
# Confirm a hit by extracting that one function from both objects and diffing
# its instructions on their own before believing it.
set -e
NEW="$(cd "$(dirname "$0")/.." && pwd)"
OLD="${1:?usage: tools/cmp32.sh <pre-port-tree>}"
OLD="$(cd "$OLD" && pwd)"
DJGPP_DIR="${DJGPP_DIR:-$HOME/djgpp}"
JWASM_BIN="${JWASM_BIN:-$HOME/vsbhda-tools/jwasm}"

if [ ! -f "$NEW/djgpp.mak" ] || [ ! -f "$OLD/djgpp.mak" ]; then
  echo "cmp32.sh: both trees must be VSBPCMCIA checkouts" >&2; exit 1
fi

docker run --rm --platform linux/amd64 \
  -v "$NEW":/new -v "$OLD":/old \
  -v "$DJGPP_DIR":/opt/djgpp -v "$JWASM_BIN":/usr/local/bin/jwasm \
  debian:stable-slim bash -c '
set -e
apt-get update -qq >/dev/null 2>&1; apt-get install -y -qq make libfl2 >/dev/null 2>&1
ln -sf /usr/local/bin/jwasm /usr/local/bin/jwasm.exe
for t in /opt/djgpp/bin/i586-pc-msdosdjgpp-*; do
  n=$(basename "$t" | sed "s/i586-pc-msdosdjgpp-//"); ln -sf "$t" /usr/local/bin/"$n"
done
export PATH=/opt/djgpp/bin:/usr/local/bin:$PATH

build() {   # $1 = source tree, $2 = work dir  (same case-renorm as build.sh)
  rm -rf $2; mkdir -p $2
  tar -C $1 --exclude=./.git --exclude=./djgpp --exclude=./ow16 -cf - . | tar -C $2 -xf -
  cd $2
  find . -depth -type d | while read -r d; do
    b=$(basename "$d"); l=$(printf %s "$b" | tr "A-Z" "a-z"); [ "$b" = "$l" ] || mv "$d" "$(dirname "$d")/$l"; done
  find . -type f | while read -r f; do
    b=$(basename "$f"); l=$(printf %s "$b" | tr "A-Z" "a-z"); [ "$b" = "$l" ] || mv "$f" "$(dirname "$f")/$l"; done
  find . -type f \( -name "*.c" -o -name "*.cpp" -o -name "*.h" -o -name "*.asm" -o -name "*.inc" \) -print0 \
    | xargs -0 sed -i -E "s/(#[[:space:]]*include[[:space:]]*\")([^\"]+)(\")/\1\L\2\E\3/"
  make -f djgpp.mak >/dev/null 2>&1 || true
  cd /
}
build /old /wold
build /new /wnew

# Two views of the same object.
#  norm_code: what the CPU will execute -- branch targets, symbol references
#             and literals masked, and the "<addr> <name>:" function headers
#             dropped entirely, so a renamed static is NOT reported as a code
#             change (it is not one; the shipped binary is stripped).
#  norm_attr: the same, but keeping the function names, so a real difference
#             can be attributed to the function it is in.
MASK="s/^[[:space:]]*[0-9a-f]+:\t([0-9a-f]{2} )+[[:space:]]*//; s/[0-9a-f]+ <[^>]*>/<T>/g; s/<[^>]*>/<F>/g; s/0x[0-9a-f]+/H/g"
norm_code() { objdump -d --section=.text $1 | grep -vE "^[0-9a-f]{8} <" \
  | sed -E "$MASK" | grep -vE "^$|file format|Disassembly"; }
norm_attr() { objdump -d --section=.text $1 \
  | sed -E "s/^[0-9a-f]{8} <([^>]*)>:/FUNC \1/; $MASK" \
  | grep -vE "^$|file format|Disassembly"; }

# set -e off from here: both helpers end in a grep, and a grep that matches
# nothing exits 1, which would abort the comparison rather than report it.
set +e
echo "=== .text instruction streams, symbols and literals masked ==="
clean=0; dirty=0
for o in /wold/djgpp/*.o; do
  b=$(basename $o)
  [ -f /wnew/djgpp/$b ] || { echo "  ONLY IN OLD: $b"; continue; }
  norm_code $o > /tmp/o.txt; norm_code /wnew/djgpp/$b > /tmp/n.txt
  d=$(diff /tmp/o.txt /tmp/n.txt | grep -c "^[<>]" || true)
  if [ "$d" = "0" ]; then clean=$((clean+1)); else
    dirty=$((dirty+1))
    echo "  CHANGED: $b  ($d lines of $(wc -l < /tmp/o.txt))"
    norm_attr $o > /tmp/oa.txt; norm_attr /wnew/djgpp/$b > /tmp/na.txt
    diff /tmp/oa.txt /tmp/na.txt | grep -E "^[0-9]" | while read h; do
      ln=$(echo "$h" | sed -E "s/^([0-9]+).*/\1/")
      echo "    in $(head -n $ln /tmp/oa.txt | grep "^FUNC" | tail -1)"
    done | sort -u
  fi
done
for o in /wnew/djgpp/*.o; do b=$(basename $o); [ -f /wold/djgpp/$b ] || echo "  ONLY IN NEW: $b"; done
echo "=== unchanged=$clean  changed=$dirty ==="
'
