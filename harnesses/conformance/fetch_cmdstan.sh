#!/usr/bin/env bash
# Prepare the exact CmdStan checkout used by the conformance reference.
# Safe to rerun; every revision is verified after submodule initialization.
set -euo pipefail

repo_root=$(cd "$(dirname "$0")/../.." && pwd)
cmdstan_dir=${1:-"$repo_root/deps/cmdstan"}

cmdstan_sha=d3d5df6a22565edbe13edbd4eb40762cc8c5a4d6
stan_sha=a6806ef8477a7b5f65b27449ec33528c162bc024
math_sha=5252d51d47c1d5e78005fc043ad996fad6dd8da8
bridgestan_sha=49e248f351d4dac18d7fd154dbc3a0ab39c5de10

# The conformance stanc is built from source at STANC3_SRC_SHA -- the
# revision the wheels embed -- not downloaded from stanc3's `nightly`
# release tag. That tag is republished in place upstream, so a binary
# pinned against it stops existing the first time no cache holds a
# copy; a git SHA can be fetched and rebuilt forever, and the run then
# tests the frontend the shipped runtime actually uses.
"$repo_root/harnesses/conformance/build_stanc.sh"
stanc_pinned="$repo_root/deps/stanc3/stanc-pinned"
stanc3_src_sha=$(sed -n 's/^STANC3_SRC_SHA=\([0-9a-f]*\).*/\1/p' \
                 "$repo_root/tools/dev_setup.sh")
if [[ "$(cat "$stanc_pinned.src" 2>/dev/null)" != "$stanc3_src_sha" ]]; then
  echo "conformance stanc provenance mismatch: built from" \
       "'$(cat "$stanc_pinned.src" 2>/dev/null)', pin is '$stanc3_src_sha'" >&2
  exit 1
fi

if [[ ! -d "$cmdstan_dir/.git" ]]; then
  git clone --filter=blob:none --no-checkout \
    https://github.com/stan-dev/cmdstan.git "$cmdstan_dir"
fi
git -C "$cmdstan_dir" fetch -q origin "$cmdstan_sha"
git -C "$cmdstan_dir" checkout -q --detach "$cmdstan_sha"
git -C "$cmdstan_dir" submodule update --init --recursive stan

verify_head() {
  local directory=$1 expected=$2 label=$3 actual
  actual=$(git -C "$directory" rev-parse HEAD)
  if [[ "$actual" != "$expected" ]]; then
    echo "$label pin mismatch: expected $expected, got $actual" >&2
    exit 1
  fi
}
verify_head "$cmdstan_dir" "$cmdstan_sha" CmdStan
verify_head "$cmdstan_dir/stan" "$stan_sha" Stan
verify_head "$cmdstan_dir/stan/lib/stan_math" "$math_sha" 'Stan Math'

bridgestan_dir="$repo_root/deps/bridgestan"
if [[ ! -d "$bridgestan_dir/.git" ]]; then
  git clone --filter=blob:none --no-checkout \
    https://github.com/roualdes/bridgestan.git "$bridgestan_dir"
fi
git -C "$bridgestan_dir" fetch -q origin "$bridgestan_sha"
git -C "$bridgestan_dir" checkout -q --detach "$bridgestan_sha"
verify_head "$bridgestan_dir" "$bridgestan_sha" BridgeStan

mkdir -p "$cmdstan_dir/bin"
# Both sides of the differential compile with the identical frontend:
# CmdStan gets the same source-built stanc that stanli lowers through,
# in place of whatever make/stanc would download.
cp "$stanc_pinned" "$cmdstan_dir/bin/stanc"

case "$(uname -s)" in
  Darwin) tbb_target=stan/lib/stan_math/lib/tbb/libtbb.dylib ;;
  Linux) tbb_target=stan/lib/stan_math/lib/tbb/libtbb.so.2 ;;
  *)
    echo "Automatic TBB setup currently supports Linux and macOS." >&2
    exit 1
    ;;
esac
make -C "$cmdstan_dir" "$tbb_target"

echo "CmdStan conformance reference ready at $cmdstan_dir"
echo "  CmdStan $cmdstan_sha"
echo "  Stan    $stan_sha"
echo "  Math    $math_sha"
echo "  BridgeStan $bridgestan_sha"
echo "  stanc   $("$stanc_pinned" --version) from $stanc3_src_sha"
