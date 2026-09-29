# Python Function call overhead

## Evaluator and hypotheses

Baseline: `54786bc` (the Python adapter merged in #291). Compare identical
Release builds on one machine, excluding compilation from call latency.

- Semantic gate: exact scalar/array outputs, integer promotion and bounds,
  nonfinite values and signed zero, unchanged exceptions, overload selection
  on every call, independent result ownership, and concurrent/reentrant calls.
- Performance gate (provisional): at least 20% lower median scalar-affine
  latency, with no material large-array regression. Use 11 warmed samples,
  rotate baseline/candidate order, retain medians, quartiles, and raw samples.
- Generality gate: dispatch only on exact built-in scalar types and immutable
  MIR function definitions, never on source/model/argument names. NumPy
  scalars, subclasses, arrays, and other array-like inputs retain conversion
  through the established NumPy path. Include unrelated scalar math, overloads,
  unused definitions, empty/strided arrays, and invalid inputs.
- Delivery gate: separately ablatable changes, full native/Python suites,
  reproducible benchmark commands, documentation, and a green merged PR.

H1: NumPy packing is avoidable for exact Python `float` and `int` objects.
Box those directly into call-local ctypes storage; check integer bounds before
conversion. Booleans and subclasses must not accidentally enter this path.
No mutable scratch buffers may be shared across calls. Keep only if this
reduces small-call latency while preserving the generic fallback.

H2: Function definitions and name-matched candidate lists are immutable for
the lifetime of a native handle. Cache these at construction, but do not cache
an argument-dependent winning overload. Cached pointers must remain owned by
the handle's immutable Program. Compare with H1 independently, including a
program with many unrelated definitions; reject if lookup caching changes
resolution, nested calls, exceptions, or concurrency behavior.

Larger array-copy reductions and a scalar-result ABI without callbacks are
deferred: neither is necessary to test these two hypotheses, and each requires
separate ownership/lifetime analysis.

## Results

Both hypotheses survive. The production changes are isolated in `2fe0396`
(packing) and `dc2a835` (lookup caching). Neither changes a public API or ABI,
the interpreter's arithmetic, or array ownership.

Final four-way comparison: Apple M3 Ultra, macOS 26.6.2, Apple Clang 21.0.0,
Release build, Python 3.11.15, NumPy 2.4.6; 11 warmed, rotating-order samples,
at least 50 ms per calibrated sample. Compilation is excluded. All four
variants return bitwise-identical values, dtypes, and shapes for every case.
The runtime identifies itself as `abi1-dc2a835c7b8f-Darwin-arm64-threads`.
[Raw samples, quartiles, and binary hashes](../../python-function-overhead.json)
are retained; values below are median microseconds per call.

| Case | Baseline | Packing only | Lookups only | Both |
| --- | ---: | ---: | ---: | ---: |
| Scalar affine, 3 Python floats | 18.315 | 13.684 | 18.351 | 13.408 |
| Same arguments as NumPy scalars (fallback) | 18.443 | 18.110 | 18.152 | 17.709 |
| Integer increment | 15.752 | 11.583 | 15.625 | 11.457 |
| Nonlinear `log1p_exp` canary | 13.887 | 11.790 | 13.560 | 11.610 |
| 100-element vector | 20.806 | 17.921 | 20.550 | 17.475 |
| 100,000-element vector | 129.309 | 126.349 | 129.567 | 126.383 |
| Scalar affine + 128 unrelated definitions | 28.132 | 22.760 | 18.242 | 13.313 |

The scalar reduction is 26.8%: baseline IQR 18.258–18.690 µs, combined
13.269–13.622 µs. With 128 unrelated definitions it is 52.7%: baseline IQR
27.853–28.268 µs, combined 13.205–13.631 µs. The large vector improves by
2.3% (baseline IQR 128.903–129.882 µs, combined 124.892–126.517 µs).
The empty, length-one, and strided-vector cases also improve; no measured
fallback/canary regression. An earlier stable 15-sample repeat found the same
pattern: 19.065 → 13.915 µs scalar, 28.794 → 13.717 µs with 128 definitions,
and 134.857 → 130.780 µs for the large vector. An initial run disrupted by
other CPU-saturating jobs was retained locally but not used for conclusions.

Construction/free from cached MIR is measured separately. With two definitions
it is effectively unchanged (9.576 → 9.558 µs). With 130 definitions, building
the cache adds about 10 µs once (234.084 → 244.349 µs), while saving about
10 µs per subsequent call from lookup caching alone. This is not a
compilation-speed improvement.

Memory tradeoff: the native cache retains O(F + C) table entries per handle
(F total definitions, C name-matched candidates), including copied map keys.
The Program itself is not duplicated. These tables replace per-call temporary
tables; scalar packing removes transient NumPy arrays for exact built-ins.
Input, output, and interpreter array-copy behavior is unchanged. No claim of
reduced whole-process peak RSS is made.

The ordinary public-API benchmark on the final build measured 13.236 µs for
scalar affine and 125.467 µs for 100,000 elements (IQR 125.054–126.516 µs),
versus 2,267.401 µs for the Python list loop and 24.196 µs for NumPy on that
vector. Pure Python scalar arithmetic remains much faster (0.059 µs).
One-time source compilation was 5.230 ms; constructing two handles from MIR
took 0.057 ms. These are workload-specific observations, not a parity claim.

## Proof and validation

Exact Python floats already have binary64 representation; `ctypes.c_double`
copies that value without arithmetic. Exact Python integers are bounded to
Stan's int range before `ctypes.c_int` conversion. Only exact types qualify:
booleans, subclasses, NumPy scalars, and arrays keep the established path.
Bitwise differential tests include signed zero, subnormals, infinities, and
a NaN payload. Integer limits, rejected types, and post-error recovery pass.

The native Program is immutable and owns all cached FunDef pointers until
handle destruction. The full table and candidate order are identical to the
previous per-call construction. Actual overload selection, ambiguity checks,
argument validation, interpreter state, and return copying remain per-call.
Tests alternate integer/real and scalar/vector overloads, exercise resolved
signatures and vector/row-vector ambiguity, invoke a recursive UDF, trip the
recursion guard, and successfully reuse the handle afterward. Shared-handle
thread tests and a reentrant Python result callback both pass. Inspection
confirms there are no model/source-specific eligibility checks.

Final validation: 113/113 CTest tests (including lowering, ODE, adjoint,
conformance, and unrelated runtime suites), 38/38 Python tests, clang-format,
Python byte-compilation, generated-doc checks, and `git diff --check` pass.
Pinned Math, Stan, and stanc revisions are respectively `8f326d14599d`,
`c96d04115d35`, and `5b824ee48c59`. The local embedded OCaml object targets
macOS 26.0, so local results do not establish older-macOS wheel compatibility;
release-platform packaging is checked by CI.

## Reproduction

Build baseline `54786bc` in a separate checkout with the same Release settings
and preserve its shared library. Build the candidate and stage its library in
`python/stanli/_bin/`. Then, from the candidate checkout:

```sh
PYTHONPATH=python python tools/bench_python_function_ab.py \
  --baseline-ref 54786bc --baseline-library /path/to/baseline/libstanli.dylib \
  --samples 11 --target-seconds .05 --output /tmp/function-ab.json
PYTHONPATH=python python tools/bench_python_function.py \
  --samples 11 --target-seconds .05 --output /tmp/function-public.json
ctest --test-dir build-rel --parallel 16 --output-on-failure
PYTHONPATH=python python tests/test_python.py
```

The four-way tool independently crosses old/new Python adapter source with
old/new native libraries. Use the appropriate shared-library extension on
other platforms. Reuse the handle in application code; tiny scalar calls still
pay ctypes/callback and interpreter overhead. Larger array-copy changes and
callback-free scalar results remain separate future hypotheses.
