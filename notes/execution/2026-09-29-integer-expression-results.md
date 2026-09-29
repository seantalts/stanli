# Generated-quantity integer expressions

Status: focused correctness and performance checks pass; not merged.
Baseline: `177ecdd0` (the accepted container-RNG slice).

Straight-line generated quantities can now send integer division and remainder
to the existing register instructions. This includes an integer result assigned
to a real variable: `real y = divide(7, 2)` must produce 3, not 3.5. Negative
operands truncate toward zero, and zero divisors retain Stan's domain error.
No execution engine or instruction was added.

Integer array literals now preserve their children's initialization and range
proofs. This admits direct sums and extrema of proved values, including both
Bernoulli RNG spellings. Sums still require every possible partial sum to fit
int32. Unknown ranges, partial initialization and possible overflow retain the
fallback. Signed integer overflow has no portable upstream C++ numerical
contract; this change does not invent one. General dynamic geometry, promoted
integer sums, and wider integer-range inference remain separate work.

Validation:

- The new `gq_integer_expressions` fixture has 51 values independently recorded
  from pinned CmdStan 2.40.0 at three points: maximum difference 1 ULP.
- Native tests check positive/negative operands and divisors, int32 boundary
  divisors, promoted results, zero-divisor errors and complete RNG state against
  MIR and direct Stan Math. Errors consume the preceding draw and no later draw.
- Direct-literal unknown-range/overflow guards and the existing partial-array,
  empty-input and reduction tests pass. The remainder lit tests pass too.
- These are focused checks; the combined branch must also pass the full CTest
  and recorded CmdStan replay before submission.

## Native measurements

[Raw measurements](data/2026-09-29-integer-expression-performance.json) retain
binary hashes, source hashes and six alternating fresh-process pairs. The
existing phase benchmark measures preparation, first/warm evaluations and
100 warmup + 100 posterior draws with output generation. ctypes overhead is
included. Release, Darwin arm64, Apple Clang 21, full runtime with threads.

| Integer fixture phase | Baseline | Candidate |
| --- | ---: | ---: |
| Preparation | 219 us | 471 us |
| Warm output row | 14.22 us | 0.66 us |
| Short inference plus output | 1,739 us | 338 us |
| Preparation plus that inference | 1,958 us | 809 us |
| Whole-process peak RSS | 27.89 MB | 28.33 MB |

The extra preparation recovers after about 19 output rows. Normal posterior
output therefore improves substantially; a one-row call pays more setup. This
fits the accepted normal-use policy. The source-compile phase is recorded
separately. The library size is unchanged at 39,813,984 bytes. Retained memory
was not independently measured.

AR1, scalar RNG and mixed-container RNG canaries show no clear complete-run
slowdown beyond paired dispersion. Scalar-RNG preparation varied upward by
3.5% (paired MAD 2.4%); short inference varied by 5.9% (MAD 7.6%). These noisy
microbenchmarks do not establish a universal no-regression guarantee. All
paired gradient checksums match exactly.

The existing execution benchmark manifest now includes this fixture so the
new compiled path remains measurable.
