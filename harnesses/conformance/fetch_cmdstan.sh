#!/usr/bin/env bash
# Install the CmdStan checkout, stanc and build that deps/pins.env names.
# Safe to rerun: when the heads, the stanc bytes and the compiler match the
# stamp left by the last build, nothing is rebuilt.
#
# Usage: harnesses/conformance/fetch_cmdstan.sh [--bridgestan] [CMDSTAN_DIR]
set -euo pipefail

repo_root=$(cd "$(dirname "$0")/../.." && pwd)
source "$repo_root/deps/pins.env"
source "$repo_root/tools/build_jobs.sh"
BUILD_JOBS=$(stanli_detect_build_jobs)
PYTHON=${PYTHON:-$(command -v python3 || command -v python)}

want_bridgestan=0
cmdstan_dir=
for arg in "$@"; do
  case "$arg" in
    --bridgestan) want_bridgestan=1 ;;
    *) cmdstan_dir=$arg ;;
  esac
done
cmdstan_dir=${cmdstan_dir:-"$repo_root/deps/cmdstan"}

if [[ -L "$cmdstan_dir" ]]; then
  echo "$cmdstan_dir is a symlink into another checkout; run this script" \
       "in the checkout that owns it so every worktree sharing it sees the" \
       "update." >&2
  exit 1
fi

stanc_pinned="$repo_root/deps/stanc3/stanc-pinned"
if [[ ! -x "$stanc_pinned" ||
      "$(cat "$stanc_pinned.src" 2>/dev/null)" != "$STANC3_SRC_SHA" ]]; then
  "$repo_root/harnesses/conformance/build_stanc.sh"
fi
if [[ "$(cat "$stanc_pinned.src" 2>/dev/null)" != "$STANC3_SRC_SHA" ]]; then
  echo "conformance stanc provenance mismatch: built from" \
       "'$(cat "$stanc_pinned.src" 2>/dev/null)', pin is '$STANC3_SRC_SHA'" >&2
  exit 1
fi

fetch_pinned() {
  local directory=$1 url=$2 sha=$3
  if [[ ! -d "$directory/.git" ]]; then
    git clone --filter=blob:none --no-checkout "$url" "$directory"
  fi
  if ! git -C "$directory" cat-file -e "$sha^{commit}" 2>/dev/null; then
    git -C "$directory" fetch -q origin "$sha"
  fi
  git -C "$directory" checkout -q -f --detach "$sha"
}

verify_head() {
  local directory=$1 expected=$2 label=$3 actual
  actual=$(git -C "$directory" rev-parse HEAD)
  if [[ "$actual" != "$expected" ]]; then
    echo "$label pin mismatch: expected $expected, got $actual" >&2
    exit 1
  fi
}

fetch_pinned "$cmdstan_dir" https://github.com/stan-dev/cmdstan.git "$CMDSTAN_SHA"
git -C "$cmdstan_dir" submodule update --init --recursive --force -q
verify_head "$cmdstan_dir" "$CMDSTAN_SHA" CmdStan
verify_head "$cmdstan_dir/stan" "$STAN_SHA" Stan
verify_head "$cmdstan_dir/stan/lib/stan_math" "$MATH_SHA" 'Stan Math'

if [[ "$want_bridgestan" = 1 ]]; then
  bridgestan_dir="$repo_root/deps/bridgestan"
  fetch_pinned "$bridgestan_dir" https://github.com/roualdes/bridgestan.git "$BRIDGESTAN_SHA"
  verify_head "$bridgestan_dir" "$BRIDGESTAN_SHA" BridgeStan
fi

stanc_suffix=""
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) stanc_suffix=.exe ;;
esac
install_stanc() {
  mkdir -p "$cmdstan_dir/bin"
  cmp -s "$stanc_pinned" "$cmdstan_dir/bin/stanc$stanc_suffix" ||
    cp "$stanc_pinned" "$cmdstan_dir/bin/stanc$stanc_suffix"
}

install_stanc
if "$PYTHON" "$repo_root/tools/toolchain.py" stamp-current "$cmdstan_dir"; then
  echo "CmdStan build is current"
else
  make -C "$cmdstan_dir" clean-all
  install_stanc
  make -C "$cmdstan_dir" -j"$BUILD_JOBS" build
  "$PYTHON" "$repo_root/tools/toolchain.py" require "$cmdstan_dir"
  "$PYTHON" "$repo_root/tools/toolchain.py" write-stamp "$cmdstan_dir"
fi

echo "CmdStan reference ready at $cmdstan_dir"
echo "  CmdStan $CMDSTAN_SHA"
echo "  Stan    $STAN_SHA"
echo "  Math    $MATH_SHA"
if [[ "$want_bridgestan" = 1 ]]; then
  echo "  BridgeStan $BRIDGESTAN_SHA"
fi
echo "  stanc   $("$stanc_pinned" --version) from $STANC3_SRC_SHA"
