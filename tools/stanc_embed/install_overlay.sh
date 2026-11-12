#!/usr/bin/env bash
# Copy stanli's OCaml compiler package into an exact checkout of stanli's
# stanc3 fork (STANC3_SRC_REPO at STANC3_SRC_SHA in deps/pins.env).
set -euo pipefail

cd "$(dirname "$0")/../.."
TARGET=${1:?native or js}
SRC=${2:?stanc3 source directory}

case "$TARGET" in
  native)
    LOCAL_DIR=compiler/native
    STANC3_DIR=src/stanc_embed
    ;;
  js)
    LOCAL_DIR=compiler/js
    STANC3_DIR=src/stanli_stancjs
    ;;
  *)
    echo "unknown OCaml compiler target: $TARGET" >&2
    exit 1
    ;;
esac

mkdir -p "$SRC/$STANC3_DIR"

cp compiler/ocaml/*.ml compiler/ocaml/*.mli "$LOCAL_DIR"/*.ml \
  "$LOCAL_DIR"/dune "$SRC/$STANC3_DIR/"
if [ "$TARGET" = js ]; then
  cp "$SRC/src/stancjs/conversion.ml" "$SRC/src/stancjs/conversion.mli" \
    "$SRC/$STANC3_DIR/"
fi
