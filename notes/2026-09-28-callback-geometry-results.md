# Matrix geometry at retained callback boundaries

Base: f21e8e65, on fetched origin/HEAD 5b913866768f351f86eed4d5a7246414530f3b2d.
Native Release, AppleClang 21, Darwin arm64. Instruction-generation/JIT research
remains tabled.

The original non-square matrix callback could not run: register compilation
refused its view and positional interpretation failed with `dims=1
[IndexBetween] [IndexSingle]`. The matching flattened-vector model ran and is
the supported performance comparator. Independent CmdStan 2.40.0 references
were recorded before the implementation for all three initial fixtures.

The change carries plain-matrix rows/columns in retained argument metadata,
admits validated Matrix views in the shared callback compiler, and reconstructs
typed matrix values on genuine compiler refusals. ODE, DAE, variadic algebra,
quadrature and adjoint ODE adapters share the fallback helper. Scalar/vector
callbacks keep their existing positional invocation and compiled seeding is
unchanged. No direct-RK opcode was admitted by this patch.

## Validation

A clean rebuild completed because RhsArg changed the internal runtime contract.
Final validation passes 285/285 CTests, all329 recorded CmdStan models at three
points (1,020,194 values, all124 platform ULP gates), and installed Python/R
checks. The corpus maximum remains9.38e-13 scaled error /7040 ULP; that is not
a universal ten-ULP guarantee. New independent fixture references pass:

- ordinary matrix, flattened vector and dynamic-local fallback:39 values each,
  max2 ULP;
- five-family context fixtures, compiled and fallback:36 values each,max10 ULP;
- empty JSON matrices/transposes/product/interleaved arguments:42 values,max0 ULP.

The C++ test checks bitwise callback values and weighted state/active-matrix
gradients across changed values, early returns and nested UDFs; matrix shape
queries at2x3,3x2,0x3,3x0,0x0; malformed/missing/overflowed/integer geometry;
array-of-matrix refusal; and interpreted print effects exactly once. The
public diagnostic test confirms two compiled sites per solver family (ordinary
and runtime branch), versus genuine interpreter selection in the adversarial
fixture. Matrix/flat complete log-density, gradient and output bits match with
direct RK enabled and disabled at all three reference points.

Generated quantities still have an existing whole-block fallback when runtime
real values carry data-only annotations. The context fixture exercises this
value-only route rather than hiding it. Nested arrays remain excluded.
Fable reviewed the plan, implementation and then packing excerpts after a tool
size limit blocked the full lowering-file read. Findings and dispositions are
retained in the adjacent reviews. Two pinned upstream expression-argument
limitations are documented separately in the matrix-callback upstream note;
no tolerance was widened and no dependency was modified.

## Native measurements

Six alternating fresh-process repeats, 200ms warmup and250ms measurement
windows. Baseline library/executable are saved from f21e8e65. The old matrix
path does not execute, so the valid before/after control is the mathematically
equivalent flattened-vector callback. All compared benchmark-point density and
gradient values agree bitwise. Median(MAD) native warm gradients:

| Case | Before | After |
| --- | ---: | ---: |
| Flattened-vector ODE |2.520(0.074) us|2.479(0.070) us|
| Matrix ODE |failed shape binding|2.613(0.074) us|
| Deliberately interpreted matrix ODE |failed shape binding|213.009(5.223) us|
| Ordinary branch-callback canary |1.892(0.067) us|1.892(0.031) us|
| Lotka–Volterra corpus canary |24.044(0.388) us|23.907(0.940) us|

The compiled matrix fixture is81.5x faster than its deliberately interpreted
variant for this warm gradient, and5.4% slower than the same-build vector
formulation. The fallback variant additionally creates a tiny runtime-sized
local array, so this is not a pure instruction-dispatch measurement.

Public C API phases include Python call overhead. Matrix preparation median
176.1us versus vector baseline181.9us; first gradient34.6us versus35.1us; warm
gradient3.383us versus3.147us. Six complete100-warmup+100-draw+output runs take
3.080s for matrix versus2.976s vector baseline and2.935s vector candidate.
The slower new matrix form versus vector is reported, not treated as a generic
speedup. The forced interpreted complete inference was stopped after170s
without completion; no inferred end-to-end speedup is claimed. Its later phase
measurements explicitly omit inference. The ordinary branch canary's complete
inference is3.520ms before/3.377ms after; the small public-API vector warm
increase(+2.5%) is not reproduced by direct native measurements.

Peak process RSS includes embedded compiler work and allocator variation:
vector28.40→28.53MB, matrix28.54MB, forced fallback28.79MB; canary28.07→28.00MB.
Each retained argument adds16 inline bytes, with no new per-argument allocation.
The library grows20,768 bytes uncompressed and6,466 bytes gzip. There is no
extra dependency or change to compiled hot register seeding. Binary size
measurement overlapped an initial last phase repeat; that repeat was replaced
in full after contention ended, and the original samples remain locally.

Raw samples, medians/MAD, manifest hashes and size evidence are in
`2026-09-28-callback-geometry-performance.json`. These are bounded fixture and
two-canary measurements, not an exhaustive callback corpus or portability claim.
