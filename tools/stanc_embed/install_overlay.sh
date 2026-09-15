#!/usr/bin/env bash
# Copy stanli's OCaml compiler package into an exact stanc3 source checkout.
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

# stanc3's partial evaluator uses the host OCaml [int]. Native OCaml therefore
# folds with 63-bit arithmetic while js_of_ocaml folds with 32-bit arithmetic.
# Stan integers are int32, so apply stanli's exact-pin correction before either
# stanli producer target is built. The browser build copies its pristine stock
# fallback before installing this overlay. Accept an already-applied patch
# because native and JS can share one source checkout during local development.
INT32_PATCH=$(pwd)/compiler/ocaml/stanc3-int32-fold.patch
if git -C "$SRC" apply --check "$INT32_PATCH" >/dev/null 2>&1; then
  git -C "$SRC" apply "$INT32_PATCH"
elif git -C "$SRC" apply --reverse --check "$INT32_PATCH" >/dev/null 2>&1; then
  :
else
  echo "stanc3 int32-fold correction does not apply to this source tree" >&2
  exit 1
fi

# Partial evaluation repeatedly resolves the same immutable built-in
# signatures. The portable producer scopes this bounded cache to one compile;
# stock stanc leaves it disabled. Include the patch in producer provenance.
CACHE_PATCH=$(pwd)/compiler/ocaml/stanc3-signature-cache.patch
if git -C "$SRC" apply --check "$CACHE_PATCH" >/dev/null 2>&1; then
  git -C "$SRC" apply "$CACHE_PATCH"
elif git -C "$SRC" apply --reverse --check "$CACHE_PATCH" >/dev/null 2>&1; then
  :
else
  echo "stanc3 signature cache does not apply to this source tree" >&2
  exit 1
fi

# Deserialize immutable overload sets only when a source uses their names.
LAZY_PATCH=$(pwd)/compiler/ocaml/stanc3-lazy-signatures.patch
if git -C "$SRC" apply --check "$LAZY_PATCH" >/dev/null 2>&1; then
  git -C "$SRC" apply "$LAZY_PATCH"
elif git -C "$SRC" apply --reverse --check "$LAZY_PATCH" >/dev/null 2>&1; then
  :
else
  echo "stanc3 lazy signatures do not apply to this source tree" >&2
  exit 1
fi

cp compiler/ocaml/*.ml compiler/ocaml/*.mli "$LOCAL_DIR"/*.ml \
  "$LOCAL_DIR"/dune "$SRC/$STANC3_DIR/"
if [ "$TARGET" = js ]; then
  cp "$SRC/src/stancjs/conversion.ml" "$SRC/src/stancjs/conversion.mli" \
    "$SRC/$STANC3_DIR/"
fi
