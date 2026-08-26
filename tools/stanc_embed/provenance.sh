#!/usr/bin/env bash
# Shared provenance helpers for the native and browser stanc artifacts.
# This file is sourced by build scripts; it is not intended to be run.

_stanli_embed_repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)

stanc_embed_read_setup() {
  local key=${1:?setup key}
  sed -n "s/^${key}=\\([^ ]*\\).*/\\1/p" \
    "$_stanli_embed_repo_root/tools/dev_setup.sh"
}

_stanc_embed_sha256_stream() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum | awk '{print $1}'
  elif command -v shasum >/dev/null 2>&1; then
    shasum -a 256 | awk '{print $1}'
  else
    python3 -c 'import hashlib, sys; print(hashlib.sha256(sys.stdin.buffer.read()).hexdigest())'
  fi
}

_stanc_embed_sha256_file() {
  local path=${1:?file to hash}
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$path" | awk '{print $1}'
  elif command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$path" | awk '{print $1}'
  else
    python3 -c \
      'import hashlib, sys; print(hashlib.sha256(open(sys.argv[1], "rb").read()).hexdigest())' \
      "$path"
  fi
}

# Hash both the sorted relative names and contents of every producer input.
# A newly added encoder module or a change to the build recipe therefore
# invalidates an old complete object without another manually maintained list.
stanc_embed_inputs_sha256() {
  (
    cd "$_stanli_embed_repo_root"
    while IFS= read -r input; do
      printf '%s\n%s\n' "$input" "$(_stanc_embed_sha256_file "$input")"
    done < <(find tools/stanc_embed -maxdepth 1 -type f -print | LC_ALL=C sort)
  ) | _stanc_embed_sha256_stream
}

stanc_embed_expected_stamp() {
  local src_sha=${1:-$(stanc_embed_read_setup STANC3_SRC_SHA)}
  printf '%s\n' \
    'format=stanli-stanc-embed-v1' \
    "stanc3_src_sha=$src_sha" \
    "producer_inputs_sha256=$(stanc_embed_inputs_sha256)"
}

stanc_embed_artifact_matches() {
  local object=${1:?embedded object}
  local src_sha=${2:-$(stanc_embed_read_setup STANC3_SRC_SHA)}
  local stamp="${object}.stamp"
  [[ -f "$object" && -f "$stamp" ]] || return 1
  [[ "$(cat "$stamp")" == "$(stanc_embed_expected_stamp "$src_sha")" ]]
}

stancjs_expected_stamp() {
  local src_repo=${1:?stanc3 source repository}
  local src_sha=${2:?stanc3 source revision}
  local opam_switch=${3:?opam switch}
  printf '%s\n' \
    'format=stanli-stancjs-v1' \
    "stanc3_src_repo=$src_repo" \
    "stanc3_src_sha=$src_sha" \
    "opam_switch=$opam_switch" \
    'dune_profile=release' \
    'dune_subst=1'
}

stancjs_artifact_matches() {
  local artifact=${1:?stancjs artifact}
  local src_repo=${2:?stanc3 source repository}
  local src_sha=${3:?stanc3 source revision}
  local opam_switch=${4:?opam switch}
  local stamp="${artifact}.stamp"
  [[ -f "$artifact" && -f "$stamp" ]] || return 1
  [[ "$(cat "$stamp")" == \
     "$(stancjs_expected_stamp "$src_repo" "$src_sha" "$opam_switch")" ]]
}
