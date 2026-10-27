#!/usr/bin/env bash
# The embedded stanc object lives under a directory named by a content key,
# and configuring CMake refuses an object that does not belong to the tree.
set -euo pipefail
cd "$(dirname "$0")/.."
repo=$PWD

work=$(mktemp -d "${TMPDIR:-/tmp}/stanli-embed-key.XXXXXX")
work=$(cd "$work" && pwd -P)
trap 'rm -rf "$work"' EXIT
# No opam switch, as on a CI cache hit: the stamp is checked field by field.
mkdir "$work/bin"
printf '#!/usr/bin/env bash\nexit 1\n' > "$work/bin/opam"
chmod +x "$work/bin/opam"
export PATH="$work/bin:$PATH"

tree=$work/tree
mkdir -p "$tree/deps/stanc3" "$tree/tools" "$tree/compiler" "$tree/cmake"
cp -R compiler/native compiler/ocaml "$tree/compiler/"
cp -R tools/stanc_embed "$tree/tools/"
cp deps/pins.env "$tree/deps/"
cp cmake/StanliEmbed.cmake "$tree/cmake/"
cat > "$tree/CMakeLists.txt" <<'CMAKE'
cmake_minimum_required(VERSION 3.16)
project(embed_check NONE)
option(STANLI_EMBED_STANC "" OFF)
set(STANLI_STANC_EXECUTABLE "${CMAKE_CURRENT_SOURCE_DIR}/deps/stanc3/stanc"
    CACHE FILEPATH "")
include(cmake/StanliEmbed.cmake)
stanli_resolve_stanc_embed()
stanli_check_default_stanc("${CMAKE_CURRENT_SOURCE_DIR}/deps/stanc3/stanc")
message(STATUS "embed object: [${STANLI_EMBED_OBJECT}]")
message(STATUS "embed include: [${STANLI_EMBED_INCLUDE}]")
CMAKE

fail() { echo "FAIL: $*" >&2; exit 1; }
in_tree() { (cd "$tree" && source tools/stanc_embed/provenance.sh && "$@"); }
pin() { sed -n "s/^$1=//p" "$tree/deps/pins.env"; }

key=$(in_tree stanc_embed_key)
[[ $key =~ ^[0-9a-f]{16}$ ]] || fail "key is not 16 hex digits: $key"
[[ $(in_tree stanc_embed_key) == "$key" ]] || fail "key is not deterministic"
[[ $(in_tree stanc_embed_dir) == "$tree/deps/stanc3/embed/$key" ]] ||
  fail "unexpected directory $(in_tree stanc_embed_dir), wanted $tree/deps/stanc3/embed/$key"

echo '(* edit *)' >> "$tree/compiler/ocaml/portable_mir.ml"
[[ $(in_tree stanc_embed_key) != "$key" ]] || fail "compiler/ocaml edit kept the key"
cp compiler/ocaml/portable_mir.ml "$tree/compiler/ocaml/portable_mir.ml"
[[ $(in_tree stanc_embed_key) == "$key" ]] || fail "restoring the file did not restore the key"

sed -i.bak 's/^STANC3_SRC_SHA=.*/STANC3_SRC_SHA=0000000000000000000000000000000000000000/' \
  "$tree/deps/pins.env"
[[ $(in_tree stanc_embed_key) != "$key" ]] || fail "source pin did not change the key"
cp deps/pins.env "$tree/deps/pins.env"
sed -i.bak 's/^OCAML_VERSION=.*/OCAML_VERSION=0.0.0/' "$tree/deps/pins.env"
[[ $(in_tree stanc_embed_key) != "$key" ]] || fail "OCaml version did not change the key"
cp deps/pins.env "$tree/deps/pins.env"

publish() {
  local dir=$1
  mkdir -p "$dir"
  : > "$dir/stanc_embed.o"
  mkdir -p "$dir/include/caml"
  printf '%s\n' \
    format=stanli-stanc-embed-v3 \
    "stanc3_src_sha=$(pin STANC3_SRC_SHA)" \
    "producer_inputs_sha256=$(in_tree stanc_embed_inputs_sha256)" \
    "ocaml_version=$(pin OCAML_VERSION)" \
    ocaml_target=test dune_version=test > "$dir/stanc_embed.o.stamp"
}
configure() {
  rm -rf "$work/build"
  cmake -S "$tree" -B "$work/build" "$@" > "$work/out" 2>&1
}
expect_configures() {
  configure "$@" || { cat "$work/out" >&2; fail "configure failed: $*"; }
}
expect_refused() {
  if configure "${@:2}"; then fail "configure accepted: ${*:2}"; fi
  tr -s ' \n' ' ' < "$work/out" | grep -Fq -- "$1" || { cat "$work/out" >&2; fail "no '$1' in the message"; }
}

printf '#!/bin/sh\n' > "$tree/deps/stanc3/stanc"
pin STANC3_SRC_SHA > "$tree/deps/stanc3/stanc.src"

expect_configures -DSTANLI_EMBED_STANC=OFF
grep -Fq 'embed object: []' "$work/out" || fail "OFF embedded something"

expect_refused 'tools/dev_setup.sh --embed --no-build' -DSTANLI_EMBED_STANC=ON
grep -Fq "embed/$key" "$work/out" || fail "message does not name the keyed directory"

publish "$tree/deps/stanc3/embed/$key"
expect_configures -DSTANLI_EMBED_STANC=ON
grep -Fq "embed object: [$tree/deps/stanc3/embed/$key/stanc_embed.o]" "$work/out" ||
  fail "ON did not resolve the keyed object"
grep -Fq "embed include: [$tree/deps/stanc3/embed/$key/include]" "$work/out" ||
  fail "ON did not resolve the keyed headers"

echo '(* edit *)' >> "$tree/compiler/ocaml/portable_mir.ml"
expect_refused 'tools/dev_setup.sh --embed --no-build' -DSTANLI_EMBED_STANC=ON
cp compiler/ocaml/portable_mir.ml "$tree/compiler/ocaml/portable_mir.ml"
expect_configures -DSTANLI_EMBED_STANC=ON

stamp=$tree/deps/stanc3/embed/$key/stanc_embed.o.stamp
cp "$stamp" "$work/stamp"
sed -i.bak 's/^producer_inputs_sha256=.*/producer_inputs_sha256=stale/' "$stamp"
expect_refused 'does not match' -DSTANLI_EMBED_STANC=ON
cp "$work/stamp" "$stamp"
rm -r "$tree/deps/stanc3/embed/$key/include"
expect_refused 'does not match' -DSTANLI_EMBED_STANC=ON

echo 0000000000000000000000000000000000000000 > "$tree/deps/stanc3/stanc.src"
expect_refused 'tools/dev_setup.sh --no-build' -DSTANLI_EMBED_STANC=OFF
rm "$tree/deps/stanc3/stanc.src"
expect_refused 'tools/dev_setup.sh --no-build' -DSTANLI_EMBED_STANC=OFF
expect_configures -DSTANLI_EMBED_STANC=OFF -DSTANLI_STANC_EXECUTABLE=/bin/true

echo 'stanc embed key tests passed'
