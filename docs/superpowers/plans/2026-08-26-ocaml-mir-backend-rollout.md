# OCaml MIR backend rollout

This is the execution plan for moving stanli's source-sensitive MIR work into
OCaml while keeping data specialization, graph construction, kernels, and the
runtime in C++. It incorporates the Fable 5 Max review from 2026-08-26.

## Ownership boundary

```text
Stan source
  -> stanc3 parse/typecheck/Stan-Math transform/O1       OCaml, stan-dev/stanc3
  -> selected passes + Portable MIR encoder              OCaml, stanli
  -> compact Portable MIR v2 or legacy debug MIR         C++, stanli
  -> data binding + transformed data + Graph lowering    C++, stanli
  -> Graph passes + executor + kernels + samplers        C++, stanli
```

The upstream interface is `Driver.Entry.stan2mir`. Upstream owns the typed MIR
and its standard passes. Stanli owns pass selection, any stanli-specific MIR
transform, the portable schema, every producer entry point, and both decoders.

The OCaml package exists only while compiling Stan source. No OCaml value
crosses the language boundary, OCaml does not run while sampling, and OCaml
does not emit the final data-specialized Graph.

The legacy decoder remains available for cached MIR, checked-in fixtures, and
users compiling with stock stanc3.

## Requirements carried from review

1. The encoder is total over `Middle.Program.Typed.t`. Unsupported forms are
   represented explicitly and fail at the normal unsupported boundary; they
   are never dropped from the document.
2. Portable MIR v2 is a faithful active-field encoding. No source idiom is
   normalized until a separate change proves that normalization against the
   corpus.
3. Native OCaml, js_of_ocaml, and Windows producers must emit byte-identical
   canonical UTF-8 for the same model name and source.
4. Producer artifacts are tied to the exact stanc3 revision, OCaml toolchain,
   js_of_ocaml version where applicable, and every stanli encoder input.
5. Source-sensitive optimization cannot turn on until every shipping channel
   uses the same stanli pipeline.

## Completed foundation

PR #208 completed the first tranche:

- upstream `stan2mir` merged and stanli pinned its merge commit;
- exact-source compiler builds replaced the moving Windows artifact;
- the initial portable schema and canonical OCaml encoder landed;
- the strict portable decoder and legacy decoder both produce `mir::Program`;
- the embedded macOS/Linux producer emits the stanli-owned format;
- semantic comparison covered 147 fixture programs and executable models;
- malformed portable inputs are structurally validated before lowering;
- native, Windows-stock, browser-stock, and R compiler provenance is checked.

## Phase 1: shared producer and browser/npm

Status: complete. All gates below run in CI.

- Move the encoder and O1 policy into one stanli-owned OCaml package under
  `compiler/`.
- Keep thin native, js_of_ocaml, and later Windows entry points around that
  package.
- Build `stanli-compiler.js` for the browser and npm package.
- Preserve the ordinary `stanc()` JavaScript API in that artifact.
- Have the worker prefer `stanli_compile()` and retain stock `stancjs.bc.js`
  as the rollback path for one release cycle.
- Leave the R package's bundled `r/inst/js/stanc.js` on the legacy format in
  this phase.
- Make every R legacy path request O1 optimized MIR rather than transformed
  but unoptimized MIR.
- Serialize embedded compiler calls and register foreign C/Python threads
  with the OCaml runtime.

Gates:

- Native and js_of_ocaml bytes agree for an ordinary model, a nested UDF, the
  mother model, an expression where O1 folds `0.1 + 0.2` to the exact binary64
  payload `3fd3333333333334`, checked int32 overflow/no-fold behavior, Unicode
  text, and an in-memory include.
- Repeated JS compilation is byte-identical.
- A malformed model produces an error-only result, never a partial document.
- Frontend diagnostics retain source excerpts, compiler warnings agree with
  stock stancjs, and JavaScript include maps reach the shared pipeline.
- The custom artifact's `stanc()` behavior agrees with stock stancjs on
  ordinary inputs; the checked overflow fixture deliberately differs from
  pristine stanc3's host-width-dependent fold.
- The custom compiler feeds the WASM runtime through gradient, NUTS, WALNUTS,
  Pathfinder, and generated-quantity paths.
- Compiler cache keys include all shared OCaml and entry-point sources.

Current v2 measurements on Eight Schools across 51 fresh processes on an Apple
arm64 release build:

| Item | Legacy | Portable |
|---|---:|---:|
| MIR bytes | 33,320 | 6,932 |
| MIR gzip bytes | 2,000 | 1,793 |
| decoder parse time, median | 0.293 ms | 0.074 ms |
| complete preparation, median | 0.682 ms | 0.278 ms |

An exact-source census of all 153 programs under `tests/fixtures` produced
identical decoded C++ fields for every model; the separate `mother` producer
case matched as well. Across the current fixture census compact v2 used
823,104 raw bytes versus 4,652,169 for legacy MIR. Before the retired JSON
encoder was removed, the preceding 146-program census measured 11,686,102
JSON bytes, 807,032 compact-v2 bytes, and 4,453,653 legacy bytes. In a
31-repetition direct-decoder run over that preceding census, the per-model
v2/legacy time ratio was 0.192 at p50, 0.319 at p95, and 0.501 at the worst
model; there were no decode or equivalence failures.

The current local compact-v2 JS compiler is 2,992,413 raw bytes after removing
the pre-release v1 encoder, about 20 KB larger than the 2,971,695-byte exact-pin
stock build.
CI records raw and gzip sizes for both artifacts on each build. Shipping both
for the rollback cycle doubles the compiler portion of the browser payload;
removing the stock producer after the rollback window recovers that temporary
cost.

The manylinux pull-request gate requires compact v2 to use no more than half
the legacy raw bytes and half the legacy median direct-decode time across 51
repetitions on Eight Schools. Gzip and complete-preparation timings remain
descriptive. Source compilation still dominates this one-time preparation
path. The compact reader constructs the C++ MIR directly and no longer
allocates a JSON DOM.

## Phase 2: Windows portable producer

Status: implemented. The compiler-only gate runs on pull requests; the full
Windows C++ build runs after merge, nightly, and on release tags.

- Cross-build pristine `stanc.exe` before applying the stanli overlay, then
  cross-build the shared CLI entry point as `stanli-compile.exe` with stanc3's
  Linux-to-Windows recipe.
- Ship both beside `stanli.dll` for one release cycle. The portable producer is
  preferred; pristine stanc is the rollback selected only when it is absent.
- Python searches its packaged `_bin` directory for `stanli-compile.exe`, then
  `stanc.exe`. Native R first honors the explicit `STANLI_STANC` stock override,
  then checks beside the runtime for the portable producer and stock stanc,
  then checks `PATH` for stock stanc, and finally uses its JavaScript path.
- Treat a selected compiler's launch error, nonzero exit, or empty output as
  an error. If the runtime decoder later rejects its document, surface that
  error too; do not hide either failure by retrying stock.
- Keep the Windows compiler in short-lived subprocesses. No OCaml compiler DLL
  is introduced.

Pull-request gates:

- Validate the exact stanc3 revision, the portable producer's source/toolchain
  stamp, LF checkout bytes for every stamped input, and that both artifacts are
  x86-64 PE executables.
- Execute pristine stanc on the checked overflow fixture to prove it was copied
  before the overlay changed the fold policy.
- Execute `stanli-compile.exe` on Windows and compare its output byte for byte
  with the same JavaScript producer already compared with native OCaml. The
  shared eight-model suite covers ordinary models, nested UDFs, loop control,
  the mother model, folded binary64, checked int32 overflow, Unicode, and
  includes. It also checks final-newline behavior. The surrounding JavaScript
  gate covers diagnostics, warnings, stock API compatibility, and repeat
  determinism.
- Run both executables through native R from paths containing spaces and
  Unicode, with CRLF source staged as exact UTF-8 bytes, and distinguish the
  portable and legacy envelopes.

Post-merge full-platform gates:

- Build the Windows C++ runtime and run CTest.
- Require the wheel and R runtime tarball to contain `stanli.dll`,
  `stanli-compile.exe`, and pristine `stanc.exe`.
- Install the wheel into python.org CPython and exercise source compilation,
  errors, lowering, gradients, sampling, and generated quantities.

This split keeps the bounded compiler contract merge-blocking while the
multi-hour stan-math build remains a post-merge platform check. Publishing
still waits for the full Windows wheel.

## Phase 3: R and webR compiler

No package registry is an architectural gate. The real constraint is direct
cross-release compatibility because an R package can carry compiler JavaScript
from one release while loading a runtime from another.

Status: shipped and validated in v0.9.4. The v0.9.3 compatibility anchor is
published with a dual-reader runtime and the legacy R compiler. The v0.9.4
package carries the exact shared compiler artifact. CI loads it through the
real V8 and webR helpers, sends its output through the public
`stanli_model(mir=...)` API, and exercises UTF-8, deterministic bytes,
warnings, malformed input, gradients, sampling, and generated quantities.

Shipping implementation:

- The tracked R compiler JavaScript is byte-identical to a fresh exact-source
  build from the shared OCaml package, with complete producer provenance.
- The real V8 and webR helpers select callable `stanli_compile()` by
  export presence, using stock `stanc()` only when that export is absent.
- They never retry stock after a selected portable compiler reports an error.
- The new compiler runs against both the current runtime and the previous
  released dual-reader runtime.
- The old package's legacy compiler runs against the new runtime.
- V8, native subprocess, and webR source-compilation paths are separate gates.

This compatibility matrix replaces the earlier assumed release-cycle gate and
is both stricter and directly testable.

## Phase 4: upstream `vectorize_loops`

The pinned stanc3 contains the pass, but O1 leaves it disabled. Enable O1 plus
only `vectorize_loops` in the shared stanli pipeline. Do not enable the entire
experimental suite.

Status: production-enabled, with release validation pending. The shared OCaml
default selects exactly upstream O1 plus `vectorize_loops` in every native,
Windows, browser, Python, and R producer. A test-only native OCaml probe emits
pass-off/pass-on portable MIR, and the bounded CI report checks semantic and
reference parity while publishing graph, preparation, reroll, compiler, and
interleaved gradient evidence. The no-model harness command covers all 130
committed reference models and labels any additional posteriordb census model
as A/B-only; CI uses seven named models, and the timing ratios are descriptive
rather than acceptance thresholds. See
[`TESTING.md`](../../../TESTING.md#mir-loop-vectorization-measurement) for the
operator command and artifact contract.

The release matrix advances its previous portable pair to v0.9.4: the new
vectorizing compiler runs against that runtime, and the v0.9.4 pass-off
compiler runs against the new runtime. The v0.9.3 legacy compiler remains a
separate permanent decoder oracle.

### 2026-08-26 measurement baseline

The complete Release run at implementation revision `d3b7d26` used macOS
26.4 arm64 and Apple clang 21. It covered 131 models and 393 evaluation
points. Enabling the pass changed MIR for `covid19imperial_v2`,
`covid19imperial_v3`, `normal_mixture`, `radon_pooled`, and
`soil_incubation`. The only finite arithmetic-order changes were two
one-ULP log-density results in `soil_incubation`; the run reported zero
semantic failures and zero infrastructure failures and took 229.898 seconds.
Timing ratios remain descriptive.

The pass removed only 4 of the 23 `log_prob` term-density reroll hits; 19
models still reported that rewrite. No C++ reroll case can be retired from
this evidence, so production enables the upstream pass while retaining the
complete C++ reroll pass.

Keep C++ reroll enabled initially and measure with the OCaml pass both on and
off:

- whole-corpus log density, gradient, write-array, and error parity;
- graph op counts and which C++ reroll patterns still fire;
- compile time and preparation time;
- interleaved gradient timings for `radon_pooled`, `arK`, and models whose
  loops should remain untouched;
- arithmetic-order changes, reported separately from semantic mismatches.

Only remove a C++ reroll case after the pass-off comparison shows that no
supported model still benefits from it.

## Phase 5: stanli-specific OCaml transformations

Add one structural, data-independent transformation per PR. Each transform
gets:

- a typed-MIR unit test;
- exact producer-byte tests on every toolchain;
- pass-on/pass-off corpus and conformance runs;
- op-count and performance measurements;
- a capability check for the runtime vocabulary it can synthesize.

Data-dependent transformations remain in C++: transformed-data evaluation,
concrete dimensions, data-known branches and loop bounds, physical layouts,
kernel selection, in-place rewrites, CSE, partitioning, tape islands, arena
packing, and executor scheduling.

## Phase 6: producer retirement

- Remove stock stancjs from browser/npm after the rollback cycle.
- Remove the duplicate stock Windows compiler after the Windows portable
  producer has shipped and downgrade behavior is documented.
- Deprecate legacy-producing convenience paths only after all maintained
  channels emit portable MIR.
- Retain the legacy decoder indefinitely.
- Consider explicit schema fields that replace old MIR idioms one at a time;
  each is a new gated schema evolution, not an implicit v2 normalization.
