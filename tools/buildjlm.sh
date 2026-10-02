#!/bin/bash
# VSBPCMJ.DLL, the ring-0 companion JLM (jlm/VSBPCMJ.ASM): JWasm -coff, then
# wlink, then pe2px.py, which checks the DLL against what Jemm's JLOAD needs
# and marks it PX. Runs on the host with native tools, no container.
#
#   ./tools/buildjlm.sh          -> jlm/out/VSBPCMJ.DLL
#
# Host prerequisites (paths overridable via env):
#   JWASM    - a JWasm built for the host (its -coff writer; -pe is not used)
#   WLINK    - Open Watcom v2 wlink for the host
#   JLM_INC  - directory holding Jemm's JLM.INC. It is not in this
#              repository; jlm/jemm/ is git-ignored for it. Fetch it with
#                mkdir -p jlm/jemm && curl -L -o jlm/jemm/JLM.INC \
#                  https://raw.githubusercontent.com/Baron-von-Riedesel/Jemm/master/Include/JLM.INC
#
# The DLL loads under Jemm386/JemmEx 5.84 and later. Only the JLM itself has
# to be built with these tools; the box needs nothing but JEMM386 + JLOAD.
set -e
REPO="$(cd "$(dirname "$0")/.." && pwd)"
JWASM="${JWASM:-$HOME/tools/ow2/armo64/jwasm}"
WLINK="${WLINK:-$HOME/tools/ow2/armo64/wlink}"
JLM_INC="${JLM_INC:-$REPO/jlm/jemm}"

if [ ! -f "$REPO/jlm/VSBPCMJ.ASM" ]; then
  echo "buildjlm.sh: no jlm/VSBPCMJ.ASM under '$REPO'" >&2
  exit 1
fi
[ -x "$JWASM" ] || { echo "buildjlm.sh: missing $JWASM -- set JWASM" >&2; exit 1; }
[ -x "$WLINK" ] || { echo "buildjlm.sh: missing $WLINK -- set WLINK" >&2; exit 1; }
[ -f "$JLM_INC/JLM.INC" ] || {
  echo "buildjlm.sh: missing $JLM_INC/JLM.INC -- see the header of this script" >&2
  exit 1; }

OUT="$REPO/jlm/out"
mkdir -p "$OUT"
cd "$OUT"
rm -f VSBPCMJ.OBJ VSBPCMJ.DLL VSBPCMJ.MAP
"$JWASM" -c -coff -nologo -I"$JLM_INC" -Fl=VSBPCMJ.LST -Fo=VSBPCMJ.OBJ "$REPO/jlm/VSBPCMJ.ASM"
"$WLINK" @"$REPO/jlm/VSBPCMJ.LNK"
python3 "$REPO/tools/pe2px.py" VSBPCMJ.DLL
ls -l VSBPCMJ.DLL
