# ctsem/stanc3 follow-up for stanli issue #248

**Date:** 2026-08-29

**Status:** stanli mitigation implemented and tested; the stanc3 optimization
is open upstream as draft PR
[#1682](https://github.com/stan-dev/stanc3/pull/1682)

## Reproducer and pinned tools

The reproducer is ctsem's model source alone. No JSON/R dump is needed for the
source-to-MIR problem. Because ctsem is GPL-3, stanrt does not copy the model;
[`tools/repro_ctsem_stanc3.sh`](../../../tools/repro_ctsem_stanc3.sh) downloads
and verifies this immutable source:

- ctsem revision: `a0f1e69b7282c1dcaed820919ae0d8b280d076c4`
- path: `inst/stan/ctsm.stan`
- SHA-256: `b148f5b3f129f981e7a3bfbd966e825c28fbc7314d14e2896d18c7c6bc1b01d2`
- stanrt stanc3 pin: `5b824ee48c590fa229dcebf6b57457b2fd212aa8`
- compiler: `stanc3 v2.39.0-142-g5b824ee (Unix)`
- stanc executable SHA-256:
  `5c1e1c10bdd8eab806fce67376cd3caaeb55d6f71839151c0a3511a2f86b95d3`

Exact invocation:

```sh
tools/repro_ctsem_stanc3.sh deps/stanc3/stanc /tmp/ctsem-stanc3
```

The underlying compiler commands are:

```sh
curl -L --fail \
  https://raw.githubusercontent.com/cdriveraus/ctsem/a0f1e69b7282c1dcaed820919ae0d8b280d076c4/inst/stan/ctsm.stan \
  -o /tmp/stanli-ctsm.stan
/usr/bin/time -l deps/stanc3/stanc \
  --debug-transformed-mir /tmp/stanli-ctsm.stan \
  >/tmp/ctsem-stanc3/transformed.mir
/usr/bin/time -l deps/stanc3/stanc \
  --O1 --debug-optimized-mir /tmp/stanli-ctsm.stan \
  >/tmp/ctsem-stanc3/optimized.mir
```

## Refreshed measurements

Host: Apple M3 Ultra, 96 GiB, Darwin arm64. `/usr/bin/time -l` reports maximum
resident set size in bytes.

| pinned stanc3 mode | real | user | max RSS | MIR bytes | `While` nodes |
| --- | ---: | ---: | ---: | ---: | ---: |
| transformed O0 | 0.11 s | 0.10 s | 53,133,312 | 4,525,185 | 11 |
| O1 optimized | 75.89 s | 75.43 s | 731,889,664 | 32,030,940 | 95 |

MIR includes `prog_path`, so byte counts vary with the source pathname. A
second end-to-end run through the wrapper used its longer output-directory
path and measured 0.12 s / 52,101,120 bytes / 4,525,196 MIR bytes for O0 and
75.25 s / 741,031,936 bytes / 32,030,951 MIR bytes for O1; the structural
counts remained 11 and 95.

The outer `rowx` loop is a sequential Kalman recurrence: the next row reads
state written by the previous row. stanc3 PRs
[#1678](https://github.com/stan-dev/stanc3/pull/1678) and
[#1681](https://github.com/stan-dev/stanc3/pull/1681) remain useful general
`--Oexperimental` loop-vectorization work, but cannot remove this recurrence.
This compiler mitigation is independent of stanli's proposed `OP_SCAN` runtime
and lowering work.

## Stanli mitigation

The shared OCaml producer already obtains backend-transformed O0 MIR before it
runs stanli's O1-plus-vectorization policy. The mitigation now performs
function inlining in isolation, before dataflow, and measures each procedure:

```text
cost(procedure) = sum over statements (1 + enclosing if/loop depth)
```

If the maximum procedure cost exceeds 20,000, the compiler returns the
untouched transformed O0 MIR. It never waits for O1, kills a compiler thread,
or accepts partial/malformed output. The native CLI and embedded compiler emit
a stderr diagnostic; the JavaScript API adds the same text to its warnings.
`STANLI_NO_O1_FALLBACK=1` forces the old unconditional O1 policy for a bisect.

For ctsem the decision is 54,115 cost, 6,708 statements, and maximum control
depth 16. The budgeted portable producer completed in 0.28 s at 33,128,448
bytes maximum RSS and emitted 579,496 bytes of compact portable MIR. The size
is not directly comparable to the legacy s-expression sizes above.

The calibration set contained 321 successfully parsed models from stanrt's
Stan fixtures and the pinned PosteriorDB source corpus. Its maximum cost was
1,288 (`tests/stanc3/mother.stan`), so the 20,000 cutoff is 15.5 times that
observed ordinary-model maximum. Feature-on below-budget output is checked
byte-for-byte against the pre-mitigation O1-plus-vectorization pipeline;
feature-off output is checked against it too. A forced fallback is checked
byte-for-byte against upstream O0, and malformed source remains a frontend
error with no fallback diagnostic.

The runtime comparison used `tests/fixtures/udf.stan`, which has a transformed-
data UDF, a loop, range slices, and data-dependent model control flow. Forced
O0 and normal O1 portable MIR differed as expected (8,400 versus 8,352 bytes),
but both lowered to the same six-op graph. At all three deterministic points,
their log densities and every gradient component were bitwise identical; the
write-array path reported the same three finite values as well. The final
embedded C API produced the same 579,496 ctsem bytes and SHA-256
`4d30220be1d268f8e6e9c4b4d3e4db67b6d4d140cb96f2fc4ed3b13f690305ae`
as the native portable CLI, including the fallback diagnostic.

## stanc3 draft PR

The patch was developed in the separate worktree
`/Users/xitrium/.codex/worktrees/09c4/stanc3-issue248-perf`, branch
`codex/issue-248-dataflow`, at the stanrt pin, and is now draft PR
[#1682](https://github.com/stan-dev/stanc3/pull/1682). It changes only:

1. `Dataflow_utils.build_statement_map` inserts its unique singleton with
   `LabelMap.add`. Predecessor-graph traversal carries the graph accumulator
   through mutually exclusive branches while starting each branch from the
   same control-flow state. This avoids repeatedly unioning persistent maps
   that share their complete prefix.
2. `dual_partial_function_lattice.leq` checks the bindings present in its
   right-hand partial function directly. The old equivalent definition scanned
   every name in the procedure-wide `Dom.total` set and did two string-map
   lookups for each name.

All variants emitted byte-identical optimized MIR, SHA-256
`9095704f4dead18ec68330b1d5763b28884c16831772cd1b0329b7e613adec1d`.

| stanc3 build | real | max RSS | retired instructions | speedup |
| --- | ---: | ---: | ---: | ---: |
| pinned baseline | 75.89 s | 731,889,664 | 1,096,932,192,045 | 1.00x |
| predecessor-map change only | 48.34 s | 695,943,168 | 668,814,667,792 | 1.57x |
| partial-function `leq` only | 50.45 s | 734,789,632 | 694,418,168,062 | 1.50x |
| combined, final patch | 17.54 s | 693,829,632 | 266,531,558,372 | 4.33x |

The changes interact because several O1 phases repeatedly rebuild flow graphs
and run the lattice fixpoint. A prior combined run took 19.24 s, 694,091,776
bytes, and 266,661,291,935 retired instructions, so elapsed-time noise is larger
than the instruction-count change. Against the first pinned baseline, the final
run removes 76.9% of wall time and 75.7% of retired instructions, but only 5.2%
of maximum RSS. Even 17.5 seconds is pathological relative to transformed MIR,
so this patch complements rather than replaces stanli's structural fallback.

## Draft upstream report

> stanc3 O1 takes 75.9 seconds and 732 MB on ctsem's model-only source at
> `a0f1e69…/inst/stan/ctsm.stan`; transformed MIR takes 0.11 seconds and 53 MB.
> O1 expands the MIR from 4.5 MB/11 `While`s to 32.0 MB/95 `While`s. A process
> sample attributes most CPU to persistent map unions in predecessor-graph
> construction, partial-function lattice comparisons in
> `Monotone_framework.mfp`, and the resulting GC. At stanc3 `5b824ee…`, carrying
> the unique-label graph map sequentially across branches and comparing only
> bindings present in the right-hand partial function reduces O1 to 17.5--19.2
> seconds (about 4x) with byte-identical MIR and passing unit tests. Individual
> changes take 48.3 and 50.5 seconds. Reproducer, exact hashes, patch, and full
> timings are available for review.

## Validation run

- stanli native OCaml pipeline tests: pass
- native embedded object, portable CLI, and js_of_ocaml compiler builds: pass
- stanrt RelWithDebInfo build with embedded stanc3: pass
- stanrt CTest: 67/67 pass
- stanc3 `test/unit` and `test/integration/good/compiler-optimizations`: pass
- reproducer wrapper: syntax check and full O0/O1 run pass
- `git diff --check` in both worktrees: pass

The Node executable is not installed on this host, so the separate CommonJS
native/JavaScript byte-parity script was not run locally. The js_of_ocaml
artifact itself built successfully, and its changed API path shares the tested
OCaml pipeline.

## Remaining blockers

- The O0 MIR reaches stanli quickly but the runtime still lacks several ctsem
  functions and general `while` lowering. Those are runtime/region issues, not
  a reason to re-enable pathological O1.
- The sequential `rowx` recurrence still needs the separate loop-bearing region
  / `OP_SCAN` track for bounded-memory execution.
- stanc3 draft PR #1682 needs upstream review and broader integration testing.
