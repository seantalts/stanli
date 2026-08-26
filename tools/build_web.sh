#!/usr/bin/env bash
# Assemble the browser demo in web/: build stanli.wasm (emsdk), build
# stancjs (opam switch with js_of_ocaml), copy both next to index.html.
# Serve with: python3 -m http.server -d web
set -euo pipefail
cd "$(dirname "$0")/.."
source tools/stanc_embed/provenance.sh

STANC3_SRC_REPO=$(stanc_embed_read_setup STANC3_SRC_REPO)
STANC3_SRC_SHA=$(stanc_embed_read_setup STANC3_SRC_SHA)
OPAM_SWITCH=$(stanc_embed_read_setup OPAM_SWITCH)
if [[ -z "$STANC3_SRC_REPO" || ! "$STANC3_SRC_SHA" =~ ^[0-9a-f]{40}$ ||
      -z "$OPAM_SWITCH" ]]; then
  echo "could not read the stanc3 repository, revision, and switch from tools/dev_setup.sh" >&2
  exit 1
fi

source deps/emsdk/emsdk_env.sh >/dev/null 2>&1
emcmake cmake -B build-wasm -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build-wasm -j8 --target stanli_wasm

# Keep the browser compiler in a separately stamped artifact. Build it in a
# temporary worktree so dune subst never modifies the source checkout shared
# with the native embedded compiler.
STANC3_SRC=deps/stanc3-src
if ! git -C "$STANC3_SRC" rev-parse --git-dir >/dev/null 2>&1; then
  [ ! -e "$STANC3_SRC" ] || {
    echo "$STANC3_SRC exists but is not a git checkout" >&2
    exit 1
  }
  git clone --no-checkout "$STANC3_SRC_REPO" "$STANC3_SRC"
elif git -C "$STANC3_SRC" remote get-url origin >/dev/null 2>&1; then
  git -C "$STANC3_SRC" remote set-url origin "$STANC3_SRC_REPO"
else
  git -C "$STANC3_SRC" remote add origin "$STANC3_SRC_REPO"
fi
git -C "$STANC3_SRC" fetch -q origin "$STANC3_SRC_SHA"
git -C "$STANC3_SRC" cat-file -e "${STANC3_SRC_SHA}^{commit}"

STANCJS=deps/stanc3/stancjs.bc.js
if stancjs_artifact_matches "$STANCJS" "$STANC3_SRC_REPO" \
     "$STANC3_SRC_SHA" "$OPAM_SWITCH"; then
  echo "$STANCJS matches the configured stanc3 source"
else
  echo "building stancjs from $STANC3_SRC_SHA"
  STANCJS_TMP_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/stanli-stancjs.XXXXXX")
  STANCJS_WORKTREE="$STANCJS_TMP_ROOT/source"
  STANCJS_TMP="${STANCJS}.tmp.$$"
  STANCJS_STAMP_TMP="${STANCJS}.stamp.tmp.$$"
  cleanup_stancjs() {
    git -C "$STANC3_SRC" worktree remove --force "$STANCJS_WORKTREE" \
      >/dev/null 2>&1 || true
    rm -rf "$STANCJS_TMP_ROOT"
    rm -f "$STANCJS_TMP" "$STANCJS_STAMP_TMP"
  }
  trap cleanup_stancjs EXIT
  git -C "$STANC3_SRC" worktree add -q --detach "$STANCJS_WORKTREE" \
    "$STANC3_SRC_SHA"
  [ "$(git -C "$STANCJS_WORKTREE" rev-parse HEAD)" = "$STANC3_SRC_SHA" ]
  (
    cd "$STANCJS_WORKTREE"
    eval "$(opam env --switch="$OPAM_SWITCH")"
    dune subst
    dune build --profile release src/stancjs/stancjs.bc.js
  )
  mkdir -p deps/stanc3
  cp "$STANCJS_WORKTREE/_build/default/src/stancjs/stancjs.bc.js" \
    "$STANCJS_TMP"
  stancjs_expected_stamp "$STANC3_SRC_REPO" "$STANC3_SRC_SHA" \
    "$OPAM_SWITCH" > "$STANCJS_STAMP_TMP"
  mv -f "$STANCJS_TMP" "$STANCJS"
  mv -f "$STANCJS_STAMP_TMP" "${STANCJS}.stamp"
  cleanup_stancjs
  trap - EXIT
fi

# js/ is the npm package: wrapper + worker + the three artifacts.
cp build-wasm/stanli.js build-wasm/stanli.wasm js/
rm -f js/stancjs.bc.js
cp "$STANCJS" js/
chmod +w js/stancjs.bc.js

# web/ is the demo page, assembled as a consumer of the package.
cp js/index.mjs web/stanli.mjs
cp js/worker.js js/stanli.js js/stanli.wasm js/stancjs.bc.js web/

# The model catalog the page searches, written from the posteriordb
# checkout rather than committed.
python3 tools/gen_web_models.py

ls -la web/
