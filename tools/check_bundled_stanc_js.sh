#!/usr/bin/env bash
# Checks the provenance record of the stanc.js bundled in the R package
# against the pinned toolchain and this checkout's producer inputs. With a
# freshly built stamp it also requires the same producer and toolchain
# identity as that build.
#
#   tools/check_bundled_stanc_js.sh [fresh-build.stamp]
set -euo pipefail
cd "$(dirname "$0")/.."
source tools/stanc_embed/provenance.sh

bundle=r/inst/js/stanc.js
record=r/inst/js/stanc.js.src
built=${1:-}
recorded() { _stanc_embed_stamp_value "$record" "$1"; }

expected_repo=$(stanc_embed_read_setup STANC3_SRC_REPO)
expected_revision=$(stanc_embed_read_setup STANC3_SRC_SHA)
actual_sha=$(_stanc_embed_sha256_file "$bundle")

[ -n "$expected_repo" ]
[ -n "$expected_revision" ]
[ "$(recorded format)" = stanli-portable-stancjs-v3 ]
[ "$(recorded stanc3_src_repo)" = "$expected_repo" ]
[ "$(recorded stanc3_src_sha)" = "$expected_revision" ]
for key in opam_switch ocaml_target dune_version; do
  [ -n "$(recorded "$key")" ]
done
[ "$(recorded ocaml_version)" = "$(stanc_embed_read_setup OCAML_VERSION)" ]
[ "$(recorded js_of_ocaml_version)" = 6.4.1 ]
[ "$(recorded producer_inputs_sha256)" = "$(stanli_stancjs_inputs_sha256)" ]
[ "$(recorded dune_profile)" = release ]
[ "$(recorded dune_subst)" = 1 ]
[ "$(recorded artifact_sha256)" = "$actual_sha" ]
if grep -a -F -q -e '%%NAME%%' -e '%%VERSION%%' "$bundle"; then
  echo "$bundle has unsubstituted version metadata" >&2
  exit 1
fi

if [ -n "$built" ]; then
  for key in format stanc3_src_repo stanc3_src_sha ocaml_version \
             js_of_ocaml_version producer_inputs_sha256 \
             dune_profile dune_subst; do
    [ -n "$(recorded "$key")" ]
    [ "$(recorded "$key")" = "$(_stanc_embed_stamp_value "$built" "$key")" ]
  done
fi
