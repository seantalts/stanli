#!/bin/sh
# Format every tracked C/C++ file with the repo .clang-format (Google
# style). Pass --check to fail on unformatted files instead of writing.
set -e
cd "$(dirname "$0")/.."
mode=-i
[ "$1" = "--check" ] && mode="--dry-run -Werror"
git ls-files '*.cpp' '*.hpp' '*.h' '*.c' ':!deps/*' ':!runtime/third_party/*' |
  xargs clang-format $mode
