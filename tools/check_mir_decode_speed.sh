#!/usr/bin/env bash
# Fail unless the compact portable MIR decodes at least 2x faster than the
# legacy format. The two decoders run in alternating order inside each round,
# so a slow interval on a shared runner hits both, and the gate compares
# lower quartiles, so the few rounds a noisy neighbour does slow cannot decide
# it. On run 40324681901 the median ratio was 1.80x and the minimum 3.06x.
#   tools/check_mir_decode_speed.sh BENCH_MIR_DECODE LEGACY_MIR PORTABLE_MIR [ROUNDS]
set -euo pipefail

bench=$1
legacy=$2
portable=$3
rounds=${4:-101}

output=$("$bench" --paired "$legacy" "$portable" "$rounds")
echo "$output"

field() { sed -n "s/.*[ ]$2=\([0-9a-z]*\).*/\1/p" <<<"$1"; }
legacy_q1=$(field "$(sed -n 1p <<<"$output")" q1_ns)
portable_q1=$(field "$(sed -n 2p <<<"$output")" q1_ns)
[[ "$legacy_q1" =~ ^[0-9]+$ && "$portable_q1" =~ ^[0-9]+$ ]] || {
  echo "could not read q1_ns from the benchmark output" >&2
  exit 1
}

echo "lower-quartile decode: legacy ${legacy_q1} ns, portable ${portable_q1} ns"
if ((portable_q1 * 2 > legacy_q1)); then
  echo "compact MIR lower-quartile decode is not at least 2x faster" >&2
  exit 1
fi
