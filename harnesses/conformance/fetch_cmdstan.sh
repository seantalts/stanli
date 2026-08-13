#!/usr/bin/env bash
# Prepare the exact CmdStan checkout used by the conformance reference.
# Safe to rerun; every revision is verified after submodule initialization.
set -euo pipefail

repo_root=$(cd "$(dirname "$0")/../.." && pwd)
cmdstan_dir=${1:-"$repo_root/deps/cmdstan"}

cmdstan_sha=11cb052d3e1fc8c799e0fec559e2ee5452b38d27
stan_sha=c96d04115d35cb04f42e45c5a69a82f9704798f1
math_sha=8f326d14599d3030c626c46532d8e8534c1cdbec
stanc_build='stanc3 b96c001 (Unix)'
bridgestan_sha=49e248f351d4dac18d7fd154dbc3a0ab39c5de10

case "$(uname -s)-$(uname -m)" in
  Darwin-arm64)
    stanc_sha=d73ab1dfdd7ecbb7c750f978b1aaa09a263718e38c97a74eb83e427a7870761f ;;
  Darwin-x86_64)
    stanc_sha=292508d1dab4d31f0e051d4b44cc20013053c5e98d2ff90e78d67327c34bc805 ;;
  Linux-x86_64)
    stanc_sha=eadfb96fba274d51c0d8e46cfa0cfe9402a35701b94ed219c2e0990c47cf3905 ;;
  Linux-aarch64)
    stanc_sha=fa8e324752ba1dfdf9bb4ff118e79732c5d0851c358252e89c305971658d413e ;;
  *)
    echo "No reviewed stanc binary digest for $(uname -s)-$(uname -m)." >&2
    exit 1
    ;;
esac

if [[ ! -x "$repo_root/deps/stanc3/stanc" ]]; then
  "$repo_root/deps/fetch.sh"
fi

actual_stanc=$("$repo_root/deps/stanc3/stanc" --version)
if [[ "$actual_stanc" != "$stanc_build" ]]; then
  echo "stanc pin mismatch: expected '$stanc_build', got '$actual_stanc'" >&2
  echo "Refusing to build a reference for a moving nightly compiler." >&2
  exit 1
fi
if command -v sha256sum >/dev/null 2>&1; then
  actual_stanc_sha=$(sha256sum "$repo_root/deps/stanc3/stanc" | awk '{print $1}')
else
  actual_stanc_sha=$(shasum -a 256 "$repo_root/deps/stanc3/stanc" | awk '{print $1}')
fi
if [[ "$actual_stanc_sha" != "$stanc_sha" ]]; then
  echo "stanc binary digest mismatch: expected $stanc_sha, got $actual_stanc_sha" >&2
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
cp "$repo_root/deps/stanc3/stanc" "$cmdstan_dir/bin/stanc"

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
echo "  $stanc_build"
