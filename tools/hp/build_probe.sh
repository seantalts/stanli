#!/usr/bin/env bash
# usage: build_probe.sh MP_INCLUDE_DIR source.cpp out [extra flags]
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$here/../..
mp=$1; src=$2; out=$3; shift 3
exec clang++ -I"$root/runtime/include" -I"$root/deps/math" -I"$root/deps/stan/src" \
  -I"$root/deps/stan/lib/rapidjson_1.1.0" \
  -I"$root/deps/math/lib/eigen_5.0.1" -I"$root/deps/math/lib/boost_1.87.0" -I"$mp" \
  -I"$root/deps/math/lib/sundials_6.1.1/include" -I"$root/deps/math/lib/tbb_2020.3/include" \
  -DBOOST_DISABLE_ASSERTS -DBOOST_MATH_PROMOTE_DOUBLE_POLICY=false -DSTAN_THREADS=1 -D_REENTRANT \
  -std=gnu++17 -O0 -ffp-contract=off "$@" "$src" -o "$out"
