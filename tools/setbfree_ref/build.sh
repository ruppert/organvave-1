#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Builds sbf_ref (OrganVave-1's floating-point reference) against setBfree at the commit ORGAN.md was measured on.
#   sh tools/setbfree_ref/build.sh [OUTDIR]     (default build/sbf_ref; clones setBfree into OUTDIR/setBfree if missing)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-build/sbf_ref}
REV=2f41983
mkdir -p "$OUT"
if [ ! -d "$OUT/setBfree/src" ]; then
	git clone -q https://github.com/pantherb/setBfree "$OUT/setBfree"
	git -C "$OUT/setBfree" checkout -q $REV
fi
S="$OUT/setBfree/src"
cc -O2 -std=gnu99 -w -I"$S" -I"$OUT/setBfree/b_whirl" -I"$OUT/setBfree/b_overdrive" -I"$OUT/setBfree/b_reverb" \
	-DHAVE_ASEQ=0 -o "$OUT/sbf_ref" "$HERE/sbf_ref.c" "$HERE/stubs.c" \
	"$S/vibrato.c" "$S/midi.c" "$S/cfgParser.c" "$S/state.c" "$S/pgmParser.c" "$S/program.c" -lm
echo "built $OUT/sbf_ref"

# the rotary's reference: setBfree's whirl.c as it is (the tables, and the render the rotary port is measured against)
B="$OUT/setBfree/b_whirl"
cc -O2 -std=gnu99 -w -I"$S" -I"$B" -DHAVE_ASEQ=0 -o "$OUT/whirl_tables" "$HERE/whirl_tables.c" "$B/eqcomp.c" -lm
cc -O2 -std=gnu99 -w -I"$S" -I"$B" -DHAVE_ASEQ=0 -o "$OUT/whirl_render" "$HERE/whirl_render.c" "$B/eqcomp.c" -lm
echo "built $OUT/whirl_tables and $OUT/whirl_render"
