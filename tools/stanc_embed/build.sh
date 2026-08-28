#!/usr/bin/env bash
# Build the embeddable stanc object: copies the shim into a stanc3 checkout,
# builds with dune (-output-complete-obj), and drops stanc_embed.o into
# deps/stanc3/.
# Usage: tools/stanc_embed/build.sh /path/to/stanc3-src [opam-switch]
set -euo pipefail
cd "$(dirname "$0")/../.."
source tools/stanc_embed/provenance.sh
source tools/build_jobs.sh
BUILD_JOBS=$(stanli_detect_build_jobs)
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
tools/stanc_embed/install_overlay.sh native "$SRC"
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
(cd "$SRC" && dune runtest -j "$BUILD_JOBS" --profile release \
   src/stanc_embed)
if ! (cd "$SRC" && dune build -j "$BUILD_JOBS" --profile release \
      src/stanc_embed/stanc_embed.exe.o 2>&1 | tail -5); then
  (cd "$SRC" && dune build -j "$BUILD_JOBS" --profile release \
    src/stanc_embed 2>&1 | tail -5)
fi
(cd "$SRC" &&
 dune build -j "$BUILD_JOBS" --profile release \
   src/stanc_embed/stanli_vectorize_probe.exe)
OBJ=$(find "$SRC/_build" -path '*/src/stanc_embed/stanc_embed*.o' | head -1)
[ -n "$OBJ" ] && [ -f "$OBJ" ] || {
  echo "dune did not produce the embedded stanc object" >&2
  exit 1
}
PROBE=$(find "$SRC/_build" \
  -path '*/src/stanc_embed/stanli_vectorize_probe.exe' | head -1)
[ -n "$PROBE" ] && [ -f "$PROBE" ] || {
  echo "dune did not produce the vectorization measurement probe" >&2
  exit 1
}

# Publish each artifact with an adjacent provenance record. The record is
# recomputed from the source pin and every local producer input, so callers can
# reject a cache entry left behind by an earlier encoder or build recipe. Each
# stamp moves last and therefore commits the artifact beside it.
OUT=deps/stanc3/stanc_embed.o
PROBE_OUT=deps/stanc3/stanli-vectorize-probe
TMP_OBJECT="${OUT}.tmp.$$"
TMP_STAMP="${OUT}.stamp.tmp.$$"
TMP_PROBE="${PROBE_OUT}.tmp.$$"
TMP_PROBE_STAMP="${PROBE_OUT}.stamp.tmp.$$"
cleanup() {
  rm -f "$TMP_OBJECT" "$TMP_STAMP" "$TMP_PROBE" "$TMP_PROBE_STAMP"
}
trap cleanup EXIT
cp -f "$OBJ" "$TMP_OBJECT"
chmod u+w "$TMP_OBJECT"
cp -f "$PROBE" "$TMP_PROBE"
chmod 755 "$TMP_PROBE"
stanc_embed_expected_stamp "$STANC3_SRC_SHA" "$SWITCH" > "$TMP_STAMP"
cp -f "$TMP_STAMP" "$TMP_PROBE_STAMP"
mv -f "$TMP_OBJECT" "$OUT"
mv -f "$TMP_STAMP" "${OUT}.stamp"
mv -f "$TMP_PROBE" "$PROBE_OUT"
mv -f "$TMP_PROBE_STAMP" "${PROBE_OUT}.stamp"
trap - EXIT
echo "embedded stanc object: $OUT ($(du -h "$OUT" | cut -f1))"
echo "vectorization measurement probe: $PROBE_OUT"
