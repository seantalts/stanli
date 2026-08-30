#!/usr/bin/env bash
# Regenerate MIR fixtures from .stan sources with the pinned stanc3.
set -euo pipefail
cd "$(dirname "$0")/.."
stanc=${STANC:-./deps/stanc3/stanc}
# Most fixtures pin the production O1 pipeline.  These fixtures intentionally
# exercise structured-control constructs that O1 rewrites away, so keep their
# transformed O0 MIR reproducible through the same generator and CI check.
o0_fixtures=' paramcond_intarray runtime_int_array_udf structured_matrix_ops udf_conditional_return udf_local_shape whileloop '

generate_fixture() {
  local f=$1
  local name out
  name=$(basename "$f" .stan)
  out=${f%.stan}.tmir.sexp
  if [[ "$o0_fixtures" == *" $name "* ]]; then
    { "$stanc" --debug-transformed-mir "$f"; echo; } \
      > "$out"
    # O0 pretty-printing can leave spaces before a newline. Canonicalise them
    # so these generated fixtures also satisfy the repository whitespace
    # check.
    sed 's/[[:blank:]]*$//' "$out" > "$out.tmp"
    mv "$out.tmp" "$out"
  else
    { "$stanc" --O1 --debug-optimized-mir "$f"; echo; } \
      > "$out"
  fi
  # stanc also emits C++ next to the model; fixtures only keep the MIR.
  rm -f "${f%.stan}.hpp"
}

# stanc does not terminate its last line, so add the newline here and keep
# every fixture reproducible byte for byte.
for f in tests/fixtures/*.stan; do
  generate_fixture "$f"
done

# Standalone lit cases use the same checked-in MIR contract as the older
# fixture corpus. Only marked sources participate, so helper .stan files can
# live below tests/lit without acquiring an accidental fixture.
while IFS= read -r -d '' f; do
  if grep -q '^// STANLI-LIT: ' "$f"; then
    generate_fixture "$f"
  fi
done < <(find tests/lit -type f -name '*.stan' -print0)
