# Compiler policy and reference corrections in main CI

Baseline: `36753e0b06c6b59e550310f548c015d49bbd2d66`; the repair was also
validated after integrating `3ac7ef8880def79d78cdb72d5c85675987366e9b`.
The failing [main workflow](https://github.com/seantalts/stanli/actions/runs/37132787599)
retains the vectorization measurements. The implementation and validation
are in [PR #427](https://github.com/seantalts/stanli/pull/427).

## Vectorization probe policy

Adding the ctsem source to the common corpus exposed a mismatch between the
measurement probe and the shipped compiler. The probe unconditionally disabled
the structural O1 budget. Both vectorization modes then failed all three
recorded ctsem points with `runtime-control region: break outside a loop`,
and their four graph-measurement configurations could not compile either.
The shipped compiler's transformed-O0 fallback passed the recorded oracle.

The probe now obtains the same default pass selection as production and
changes only `vectorize_loops`. It reports the fallback diagnostic. Eligibility
depends on the existing structural cost, with no source or model exceptions;
numerical, error, shape and timing gates are unchanged. An explicit
`STANLI_NO_O1_FALLBACK=1` still enables unguarded O1 experiments.

At stanc3 `d58446e631b02cacc5355e373defc6092a684554`, the ctsem source has
structural cost 57,398 against the 20,000 budget (7,116 statements, maximum
control depth 16). On Darwin ARM64, the repaired probe passed the existing
vectorization harness for ctsem and `eight_schools_centered`: six parameter
points, four graph configurations per model, two preparation samples per
configuration, and no semantic or measurement failures. The command was:

```sh
python3 harnesses/vectorize_ab.py deps/posteriordb \
  ctsem_ctsm eight_schools_centered --prep-samples 2 \
  --output-dir build/vectorize-policy
```

The pinned PosteriorDB revision was `28f8d3d6e975315f42aa274a8399f21e07a43b30`.
For `tests/compiler/portable_vectorize_loop.stan`, each mode's output was
byte-identical to the old probe and the off/on outputs still differed. Forced
O1 reproduced the old ctsem portable MIR byte for byte. Rebuilding the bundled
JavaScript compiler also produced identical bytes; its provenance was refreshed.

Forced-O1 ctsem lowering remains unresolved. A trial that checked for runtime
loop exits before unrolling a `while` still produced the same error and was
removed. The next investigation needs a reduced example locating the inlined
`break` and the loop that should own it. Passing the default compiler policy
does not establish support for that unguarded pipeline.

## ARM64 numerical reference

The zero-probe test constructed `a * x[i]` separately inside its reference
loop, while the Stan source constructs `vector[N] mu = a * x` first. Their
pullbacks reduce in different orders; Linux ARM64 exceeded the unchanged
10-ULP gate by reporting 14 ULP. The reference now uses Stan Math's vector
multiply and the FMA emitted in the fixture's optimized Stan code.

Using the pinned stanc3-generated C++ and Stan/Math sources, the corrected
reference and runtime agreed bit for bit on Darwin ARM64 for log density and
both gradients at `(0.3, -0.4)` and `(-1.5, 0.9)`, with `A=0`, `-0.0`, `0.5`
and NaN. The original scalar reference differed even on Darwin. The repaired
Linux ARM64 build and tests passed in
[the portability run](https://github.com/seantalts/stanli/actions/runs/37136107978).
These are focused reference checks, not a universal 10-ULP claim.
