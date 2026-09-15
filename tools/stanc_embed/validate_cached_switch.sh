#!/usr/bin/env bash
# Only for restored caches on disposable CI runners, never local setup.
set -euo pipefail
cd "$(dirname "$0")/../.."
source tools/stanc_embed/provenance.sh
switch=$(stanc_embed_read_setup OPAM_SWITCH)
expected=$(stanc_embed_read_setup OCAML_VERSION)
if ! _stanc_embed_switch_exists "$switch"; then
  exit 0
fi
if actual=$(_stanc_embed_ocaml_version "$switch"); then
  if [[ -z "$actual" ]]; then
    echo "Cached OCaml returned an empty version; preserving $switch. Check its external dependencies." >&2
    exit 1
  fi
else
  status=$?
  echo "Cached OCaml could not run (exit $status); preserving $switch. Check its external dependencies." >&2
  exit "$status"
fi
if [[ "$actual" != "$expected" ]]; then
  # Legacy cache fallback keys did not encode the OCaml version.
  echo "Cached OCaml $actual does not match $expected; recreating $switch"
  opam switch remove "$switch" --yes
fi
