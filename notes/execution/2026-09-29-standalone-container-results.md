# Standalone nested containers

Status: adopted after correctness and native measurements; not merged.
Baseline: `6d15f815`, after the integer-expression slice.

The standalone API now admits nested numeric arrays and arrays of vectors, row
vectors and matrices to the existing register compiler. Public values retain
Stan's first-index-fast serialization. Input register destinations and output
register order are prepared once with the shared layout helper. Ordinary scalar,
vector and matrix arguments retain their direct copy path.

A prepared function that emits no instructions and returns an input with exactly
the same type and dimensions returns that current input directly. This is a
proved no-op within the existing compiled plan, not another evaluator. It does
not cache values, discard effects, or use function names to select behavior.

The register compiler also recognizes suffix dimensions after a proved constant,
valid array-prefix index. Runtime/invalid selectors still execute through the
existing checked path. In particular, `size(x[1])` must not silently succeed on
an empty outer array merely because the inner dimension is known.

## Evidence

- Native tests require compiled selection, correct integer/real result mirrors,
  repeated calls with changed values, multiple outer dimensions, singleton/empty
  dimensions and zero-width vector/matrix leaves. Nonuniform coordinate-weighted
  arithmetic catches permutations that an identity roundtrip alone would miss.
- An independent pinned CmdStan wrapper produces exactly the same 48 values as
  the coordinate formulas in those tests. The complete wrapper, input data and
  output record are retained with the measurements below.
- Runtime bounds errors survive a shape query; a failed empty-array prefix read
  remains an error. Existing printing/rejection, dynamic-result fallback, integer
  cache churn and recursive-inlining-limit tests continue to pass.
- The complete branch passed 313 CTests and the recorded 329-model CmdStan
  replay (1,020,194 values). The existing 7,040-ULP cancellation case remains
  unchanged; the replay's scaled gate is not a universal 10-ULP claim.
- Installed Python tests pass. Installed R tests pass, including the native CLI
  CSV comparison rerun with `STANLI_RUN`; one platform-inapplicable empty test is
  skipped. Formatting and whitespace checks pass.

## Native performance

[Measurements and oracle](data/2026-09-29-standalone-container-performance.json)
contain six alternating fresh-process pairs per case, binary/source hashes,
first-call and handle-construction costs, peak RSS and rejected iterations.
Release Darwin arm64, Apple Clang 21, full threaded runtime. Warm measurements
include ctypes and a result writer that does no copying.

| Call | Elements | Baseline warm | Adopted warm |
| --- | ---: | ---: | ---: |
| Array of vectors, unchanged return | 32 | 8.69 us | 1.19 us |
| Same, medium | 2,048 | 10.3 us | 1.89 us |
| Same, larger | 8,192 | 12.8 us | 4.56 us |
| Same, stress | 65,536 | 36.6 us | 21.2 us |
| Nested real-array indexed arithmetic | 32 | 82.5 us | 1.71 us |
| Matrix-array indexed arithmetic | 48 | 186 us | 2.05 us |
| Same, larger | 384 | 1,367 us | 6.93 us |

First-call compilation costs more for most newly admitted shapes. For example,
the 48-element matrix case changes from 360 to 404 us, recovered by the next
call. Small unchanged returns change from 62.3 to 70.8 us, recovered after about
two subsequent calls. The largest unchanged return improves its first call too.
These are phase tradeoffs under the accepted normal-use policy.

Scalar/vector affine, branch-return and integer-sized canaries show no clear
regression beyond timing variation. The affine ctypes run varied upward by 5.2%
(paired MAD 4.6%); a separate native C-ABI confirmation without ctypes measured
0.731 to 0.712 us, paired ratio 0.968 (MAD 0.014). Integer-specialization churn
varied by +3.6% (MAD 2.0%); its cache policy and fallback remain unchanged.

The library grows 16,960 bytes (0.043%). Nontrivial adapted real inputs retain
one integer destination per element, accounted within the existing 4 MiB/eight
plan cache budget. Output permutations reuse the existing output-register list.
No-op plans retain neither mappings nor register buffers. These accounting
bounds are not process-memory measurements. Peak RSS is recorded; the largest
unchanged return drops from 19.8 to 19.1 MB, while the 384-element computation
rises from 17.6 to 18.1 MB. Allocator-retained memory was not separately measured.

### Rejected versions

Per-call layout conversion made an 8,192-element unchanged return about three
times slower in a diagnostic pair. Cached permutations fixed that size but a
65,536-element return still paid an unnecessary layout roundtrip (about 147 vs
35.5 us in a diagnostic pair). The adopted no-op proof removes that roundtrip.
Neither rejected implementation remains in production source.

## Remaining limits

Void returns still lack typed-return admission, though the register engine
already represents empty returns. There is no demonstrated hot workload here
yet. Printing/rejection already compile; standalone RNG/solver calls need host
state or hooks that this API does not supply, so MIR is not a universal rescue.

Changing result geometry, non-inlined recursion and integer-specialization
churn remain separate contracts. Complex/tuple types remain unsupported. This
slice closes container adapters, not every standalone-function gap.
