#!/usr/bin/env bash
# OCaml's compiler-libs use zstd when linking build-time PPX drivers. Build
# that host dependency at the wheel's deployment target: Homebrew bottles
# can require a newer macOS than the target inherited by the PPX link.
set -euo pipefail

prefix=${1:?installation directory}
target=${MACOSX_DEPLOYMENT_TARGET:?macOS deployment target}
[ "$(uname -s)" = Darwin ] || { echo "macOS is required" >&2; exit 1; }
mkdir -p "$prefix"
prefix=$(cd "$prefix" && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/stanli-zstd.XXXXXX")
trap 'rm -rf "$work"' EXIT

version=1.5.7
sha256=eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3
archive="$work/zstd.tar.gz"
curl --fail --location --retry 3 \
  "https://github.com/facebook/zstd/releases/download/v$version/zstd-$version.tar.gz" \
  -o "$archive"
printf '%s  %s\n' "$sha256" "$archive" | shasum -a 256 --check
tar -xzf "$archive" -C "$work"

# Static linkage keeps PPX drivers independent of the temporary build prefix.
# This library is used only by the OCaml build tools, not the shipped runtime.
cmake -S "$work/zstd-$version/build/cmake" -B "$work/build" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_DEPLOYMENT_TARGET="$target" \
  -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_INSTALL_LIBDIR=lib \
  -DZSTD_BUILD_SHARED=OFF -DZSTD_BUILD_STATIC=ON \
  -DZSTD_BUILD_PROGRAMS=OFF -DZSTD_BUILD_TESTS=OFF
cmake --build "$work/build" --parallel 4
cmake --install "$work/build"

# Exercise the same target/library combination as the PPX link, and fail on
# linker warnings so a newer deployment target cannot quietly return.
cat > "$work/probe.c" <<'C'
#include <zstd.h>
int main(void) { return ZSTD_versionNumber() != ZSTD_VERSION_NUMBER; }
C
cc -mmacosx-version-min="$target" -Wl,-fatal_warnings \
  -I"$prefix/include" "$work/probe.c" "$prefix/lib/libzstd.a" \
  -o "$work/probe"
"$work/probe"
otool -L "$work/probe"

# Inspect every object's actual target, including Intel's older load-command
# spelling, rather than treating a CMake cache entry as evidence.
python3 - "$prefix/lib/libzstd.a" "$target" <<'PY'
import re
import subprocess
import sys

archive, target = sys.argv[1:]
text = subprocess.check_output(["otool", "-l", archive], text=True)
versions = re.findall(r"^\s*(?:minos|version)\s+(\d+(?:\.\d+)+)\s*$", text, re.M)

def normalized(value):
    parts = tuple(map(int, value.split(".")))
    return parts + (0,) * (3 - len(parts))

if not versions or any(normalized(v) != normalized(target) for v in versions):
    raise SystemExit(f"zstd object deployment targets {set(versions)} != {target}")
print(f"Verified {len(versions)} zstd object deployment targets: {target}")
PY
