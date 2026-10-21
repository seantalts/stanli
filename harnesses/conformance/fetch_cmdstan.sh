#!/usr/bin/env bash
# Prepare the exact CmdStan checkout used by the conformance reference.
# Safe to rerun; every revision is verified after submodule initialization.
set -euo pipefail

repo_root=$(cd "$(dirname "$0")/../.." && pwd)
cmdstan_dir=${1:-"$repo_root/deps/cmdstan"}

source "$repo_root/deps/pins.env"
cmdstan_sha=$CMDSTAN_SHA
stan_sha=$STAN_SHA
math_sha=$MATH_SHA
bridgestan_sha=$BRIDGESTAN_SHA

# The conformance stanc is built from source at STANC3_SRC_SHA -- the
# revision the wheels embed -- not downloaded from stanc3's `nightly`
# release tag. That tag is republished in place upstream, so a binary
# pinned against it stops existing the first time no cache holds a
# copy; a git SHA can be fetched and rebuilt forever, and the run then
# tests the frontend the shipped runtime actually uses.
"$repo_root/harnesses/conformance/build_stanc.sh"
stanc_pinned="$repo_root/deps/stanc3/stanc-pinned"
stanc3_src_sha=$STANC3_SRC_SHA
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
stanc_suffix=""
case "$(uname -s)" in
  Darwin) tbb_target=stan/lib/stan_math/lib/tbb/libtbb.dylib ;;
  Linux) tbb_target=stan/lib/stan_math/lib/tbb/libtbb.so.2 ;;
  MINGW*|MSYS*|CYGWIN*)
    stanc_suffix=.exe
    tbb_target=stan/lib/stan_math/lib/tbb/tbb.dll
    ;;
  *)
    echo "Automatic TBB setup supports Linux, macOS and Windows." >&2
    exit 1
    ;;
esac
cp "$stanc_pinned" "$cmdstan_dir/bin/stanc$stanc_suffix"
make -C "$cmdstan_dir" "$tbb_target"

echo "CmdStan conformance reference ready at $cmdstan_dir"
echo "  CmdStan $cmdstan_sha"
echo "  Stan    $stan_sha"
echo "  Math    $math_sha"
echo "  BridgeStan $bridgestan_sha"
echo "  stanc   $("$stanc_pinned" --version) from $stanc3_src_sha"
