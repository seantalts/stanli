#!/usr/bin/env bash
# Build the embeddable stanc object: copies the shim into a stanc3 checkout,
# builds with dune (-output-complete-obj), and drops stanc_embed.o into
# deps/stanc3/.
# Usage: tools/stanc_embed/build.sh /path/to/stanc3-src [opam-switch]
set -euo pipefail
cd "$(dirname "$0")/../.."
source tools/stanc_embed/provenance.sh
SRC=${1:?stanc3 source dir}
SWITCH=${2:-stanc3-55}
STANC3_SRC_SHA=$(stanc_embed_read_setup STANC3_SRC_SHA)
if [[ ! "$STANC3_SRC_SHA" =~ ^[0-9a-f]{40}$ ]]; then
  echo "could not read an exact STANC3_SRC_SHA from tools/dev_setup.sh" >&2
  exit 1
fi
ACTUAL_SRC_SHA=$(git -C "$SRC" rev-parse HEAD 2>/dev/null || true)
if [ "$ACTUAL_SRC_SHA" != "$STANC3_SRC_SHA" ]; then
  echo "stanc3 source is at ${ACTUAL_SRC_SHA:-an unknown revision}; expected $STANC3_SRC_SHA" >&2
  exit 1
fi
mkdir -p deps/stanc3
mkdir -p "$SRC/src/stanc_embed"
cp tools/stanc_embed/*.ml tools/stanc_embed/*.mli tools/stanc_embed/dune \
  "$SRC/src/stanc_embed/"
eval "$(opam env --switch="$SWITCH")"

# OCaml's complete-object mode calls `ld -r` directly. The manylinux toolchain
# does not give that partial linker the system archive directory that `cc`
# normally supplies, so locate the static librt archive through the compiler
# and pass its directory through dune's link flags.
if [ "$(uname -s)" = Linux ]; then
  LIBRT_PATH=$(cc -print-file-name=librt.a)
  if [ "$LIBRT_PATH" = librt.a ] || [ ! -f "$LIBRT_PATH" ]; then
    echo "could not locate the static librt.a needed by OCaml's partial linker" >&2
    exit 1
  fi
  STANLI_OCAML_SYSTEM_LIBDIR=$(dirname "$LIBRT_PATH")
  export STANLI_OCAML_SYSTEM_LIBDIR
fi

# Release profile: dune's dev profile links the inline-test and expect-test
# runners into every library, which ride into the shipped binary for no
# reason. Worth 364 KB of the final shared library.
(cd "$SRC" && dune build --profile release src/stanc_embed/stanc_embed.exe.o \
   2>&1 | tail -5 ||
 dune build --profile release src/stanc_embed 2>&1 | tail -5)
OBJ=$(find "$SRC/_build" -path '*/src/stanc_embed/stanc_embed*.o' | head -1)
[ -n "$OBJ" ] && [ -f "$OBJ" ] || {
  echo "dune did not produce the embedded stanc object" >&2
  exit 1
}

# Publish the object and its adjacent provenance record together. The record
# is recomputed from the source pin and every local producer input, so callers
# can reject a cache entry left behind by an earlier encoder or build recipe.
OUT=deps/stanc3/stanc_embed.o
TMP_OBJECT="${OUT}.tmp.$$"
TMP_STAMP="${OUT}.stamp.tmp.$$"
cleanup() { rm -f "$TMP_OBJECT" "$TMP_STAMP"; }
trap cleanup EXIT
cp -f "$OBJ" "$TMP_OBJECT"
chmod u+w "$TMP_OBJECT"
stanc_embed_expected_stamp "$STANC3_SRC_SHA" > "$TMP_STAMP"
mv -f "$TMP_OBJECT" "$OUT"
mv -f "$TMP_STAMP" "${OUT}.stamp"
trap - EXIT
echo "embedded stanc object: $OUT ($(du -h "$OUT" | cut -f1))"
