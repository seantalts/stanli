#!/usr/bin/env bash
# Checks that the cross-built Windows compilers in a directory come from the
# pinned stanc3 revision and this checkout's producer inputs, and that those
# inputs are checked out with LF endings.
#
#   tools/check_windows_compiler_provenance.sh DIR
#
# DIR holds stanc.src and stanli-compile (with its .stamp).
set -euo pipefail
cd "$(dirname "$0")/.."
source tools/stanc_embed/provenance.sh

dir=${1:?directory holding the Windows compilers}
src_repo=$(stanc_embed_read_setup STANC3_SRC_REPO)
src_sha=$(stanc_embed_read_setup STANC3_SRC_SHA)

while IFS= read -r input; do
  eol=$(git ls-files --eol -- "$input")
  printf '%s\n' "$eol"
  printf '%s\n' "$eol" | grep -Fq 'w/lf'
done < <(stanli_windows_cli_inputs)
test "$(cat "$dir/stanc.src")" = "$src_sha"
stanli_windows_cli_artifact_matches "$dir/stanli-compile" "$src_repo" "$src_sha"
