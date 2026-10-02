# cogmod families: gradient gaps and their causes

Scope: [issue #422](https://github.com/seantalts/stanli/issues/422) reported
that cogmod's brms custom families are slow under stanli when their densities
branch on parameters. This note records the 21 families measured against
CmdStan, the causes found, the fixes, and what remains open.

## Corpus and method

The programs are `brms::make_stancode()` output with cogmod's stanvars, from
cogmod dev at `732be30bdae2a4cc7f38cf78b5ca7cfeb85dc732`, about 2000 simulated
observations each. `lnr_bench` reproduces the issue's LNR setup (speed_acc
participants 1-3, N=4620, `sigmabias = 0`). The models are not checked in;
cogmod is MIT-licensed if they are added later.

Gradients were timed with `bench_grad --timed` and CmdStan's equivalent from
`tools/bench_cmdstan_grad.cpp` at `-O3`, on an idle Apple M-series laptop, at
the fixed point `bench_grad` uses. Gradients agree with CmdStan to within
8e-15 relative on every family that runs in both.

The MIR must come from the embedded compiler (`stanli_check
--dump-passes=mir`), which is what Python and R users get. Since #403 that
pipeline runs O1 without stanc's partial evaluation, so branches on constants
reach stanli unfolded. Plain `stanc --O1` MIR hid two of the problems below
and overstated others.

## Results

Per gradient in microseconds. "main" is `d4a88756`, which includes #423.
"branch" adds the two commits described below.

| family | main | branch | CmdStan | branch / CmdStan |
| --- | ---: | ---: | ---: | ---: |
| lnr | 2507 | 2502 | 308 | 8.12 |
| lnr_bench | 7908 | 2285 | 657 | 3.48 |
| betagate | 764 | 766 | 537 | 1.43 |
| geg | 866 | 863 | 627 | 1.38 |
| invgaussian | fails | 2055 | 1515 | 1.36 |
| exgaussian | 479 | 481 | 363 | 1.33 |
| exwald | 332 | 329 | 250 | 1.32 |
| choco | 956 | 953 | 726 | 1.31 |
| lognormal | 366 | 368 | 286 | 1.29 |
| gamma | 252 | 253 | 198 | 1.28 |
| invgamma | 257 | 257 | 202 | 1.27 |
| rdm | 338 | 342 | 271 | 1.26 |
| weibull | 250 | 252 | 200 | 1.26 |
| loggamma | 254 | 255 | 204 | 1.25 |
| logstudent | 255 | 255 | 205 | 1.24 |
| bisa | 249 | 249 | 205 | 1.21 |
| logweibull | 208 | 210 | 176 | 1.19 |
| invweibull | 256 | 253 | 220 | 1.15 |
| lba1 | 330 | 325 | 301 | 1.08 |
| lba2 | 361 | 359 | 331 | 1.08 |
| betadiscrete | 19052 | 18893 | 19064 | 0.99 |
| ddm | fails | fails | 400246 | |

Preparation is about 0.01 s for every family except LNR: `lnr_bench` takes
5.2 s on main and 2.0 s on the branch, and `lnr` 2.4 s.

## Causes found and fixed

Merged in #423:

- **Branchy regions skipped register compaction.** Any runtime-control region
  containing a jump kept registers for the whole data vector it indexed, so a
  per-observation branch cost O(N) per region and O(N^2) per gradient.
- **Packed region inputs copied whole vectors.** A region with more than six
  live-ins concatenated entire slots. LNR concatenated two 4620-element vectors
  per observation.
- **`log_mix` and comparisons used as values** did not compile inside regions.
- **Bounded specialization unrolled loops into one region per iteration**
  where the structured loop compiles the body once. The trial now refuses in
  that case.

On this branch:

- **Constants did not fold inside regions.** A transformed parameter fixed at
  a constant reached a region as an ordinary input, so every arm it selected
  between was compiled. In `lnr_bench`, `sigmabias = 0` left 1913 instructions
  and 93 kernel calls per observation where 181 and 5 are reachable. The region
  compiler is now seeded with known scalar constants: 3.5x faster on
  `lnr_bench`.
- **Guard-clause returns failed to compile in functions stanc did not
  inline.** invgaussian exceeds the O1 structural budget, so the embedded
  compiler falls back to O0 and stanli inlines the functions itself. A guard
  such as `if (mu <= 0) return negative_infinity();` followed by more code
  failed with "runtime-control region: return inside runtime control without a
  function scope". Such bodies are now rewritten into stanc's single-exit form.

## Open

- **Structured-loop overhead on the 1.1 to 1.4x families.** A sampled gamma
  gradient spends about half its time in `structured_loop_forward` and
  `structured_loop_backward` themselves, and about a sixth in the math
  functions CmdStan also calls. The ceiling is therefore near 2x if the
  executor's own work were removed. The next measurement is a line-level
  profile of those two functions.
- **LNR does not use the structured loop.** It refuses with "structured
  assignment changes logical shape" on the inlined return variable of the
  vector-returning `cogmod_lognormal_acc_ltails`, declared unsized and
  assigned in several arms. LNR therefore lowers to one region per
  observation, which is the 2 s preparation. Whether the loop path is also
  faster per gradient is unmeasured until that refusal is fixed.
- **`lnr` with a free `sigmabias`** is 8x slower than CmdStan. Its per-observation
  region interprets every start-point-variability path, about 500 ns per
  observation against CmdStan's 67. Closing this needs either the structured
  loop above or a region that executes all observations in one pass.
- **`ddm` does not compile.** Its density calls `wiener_lpdf` with four, five
  and seven parameters plus a precision argument. The reader accepts only five
  or seven arguments ("malformed wiener_lpdf call: expected 5 or 7
  argument(s), got 6"), and only the four-parameter form has a kernel. Stan
  Math's `wiener5_lpdf.hpp` and `wiener_full_lpdf.hpp` provide the others;
  adding them means new kernels and a binary-size measurement. CmdStan spends
  400 ms per gradient on this model.
