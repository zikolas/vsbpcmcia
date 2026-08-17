#!/bin/sh
# Host-side test of the timer-only FM shim (src/fmshim.c). Pure logic, no DOS
# or hardware: replays the AdLib timer probe that gates SB detection.
#   sh tools/fmshimtest/run.sh
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$DIR/../.." && pwd)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
# ptrap.h is DOS-only; the shim needs exactly one define out of it
printf '#define TRAPF_OUT 4
' > "$TMP/PTRAP.H"
: > "$TMP/CONFIG.H"
: > "$TMP/PLATFORM.H"
cp "$REPO/src/FMSHIM.H" "$TMP/"
cp "$REPO/src/FMSHIM.C" "$TMP/fmshim.c"
cc -Wall -Wextra -o "$TMP/fmshimtest" "$DIR/fmshimtest.c" "$TMP/fmshim.c" -I "$TMP"
"$TMP/fmshimtest"
