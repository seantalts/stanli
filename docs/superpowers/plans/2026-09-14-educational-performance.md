# Educational performance investigation

Objective: preserve the 13-model correctness oracle and reach CmdStan/Stanli
CLI wall time >= 0.5 for every model, including source preparation, NUTS,
generated quantities and full CSV output. Base: `7d4f25ec`; its binaries are
retained at `/tmp/stanli-educational-perf-baseline`. No model-name recognition,
source rewrites, tolerance increases, RNG resequencing or benchmark exclusions.

Baseline: Pareto 0.440x (32.6 ms prep, 15.0 ms NUTS, 13.1 ms output);
hurdle-Poisson 0.271x (3.135 s output); simple Poisson 0.272x (0.819 s output).
All fixed-point comparisons and sampled-mean checks pass. Three paired runs,
1000 warmup/1000 draws, full settings and raw timing data are in
`tests/educational/benchmark-results.json`.

Hypotheses and first experiments:

* Interpreter overhead: macOS `sample` on 5000 hurdle draws places almost all
  samples in WaInterp, with allocation/free and string/function dispatch
  dominating. Counterproposal to caching interpreter calls: avoid interpreting
  the section by using existing register control flow and OP_RNG. Add scalar
  Poisson/Student-t families, then permit integer draws to occupy runtime
  registers. Draw-dependent compile-time shape demands must still refuse;
  array reads must keep checked indexing. Stop first at same-seed graph vs
  interpreter parity, then measure the two Poisson examples.
* Pareto preparation: STANLI_PROFILE_PREP reports ~24 ms before runtime MIR
  decoding/lowering and ~6 ms lowering generated quantities. Investigate
  producer work on unused procedures before tuning runtime graph passes.
  Candidate alternatives: prune unconsumed MIR sections, or retain UDF calls
  instead of repeated inlining. Verify emitted consumed sections and existing
  compiler producer parity before accepting a change.

Acceptance: targeted adversarial fixtures; same-seed fixed-point and repeated
draw parity; 13-model CmdStan oracle and full performance gate; unrelated
corpus/CTest checks; bitwise safe fallback; compiler producer tests if touched.
Keep negative experiments and phase-level measurements here.

## Accepted implementation boundaries

1. The existing register machine now admits scalar integer RNG results in
   runtime control. Stan's 32-bit integer draws are exactly representable in
   its double registers. RNGs stay effectful; lexical integer discovery must
   never execute a draw. Runtime writes reify integers, checked indexing
   remains checked, and a draw-dependent extent still refuses to WaInterp.
   Shared draw kernels add Poisson, Student-t, and Bernoulli-logit, with the
   same Stan Math functions and engine as the interpreter. Existing RNG
   variant numbers, including container families, are preserved.
2. The producer inlines the original MIR first, makes the unchanged structural
   budget decision, and then drops unused backend procedures before the
   remaining O1 passes. Explicit model-only entry points additionally prune
   unreachable UDFs. Roots include model preparation, log density, transform
   initialization and generated quantities; the transitive name closure keeps
   every overload, higher-order callbacks, and their callees. General source
   compilation preserves all exported functions for the public function API.
   Native embedding, the standalone producer and JavaScript expose the same
   policy. The O0 budget fallback is unchanged.
3. CSV formatting uses a bounded buffer and pinned {fmt} 11.2.0 with `:.17g`.
   This is a CLI-only dependency, fetched by the existing pinned-dependency
   script. It avoids libc's per-field formatting/locking overhead without
   reducing significant digits or omitting columns. NaNs retain the host
   printf spelling. No sampler, graph arithmetic, RNG order or JSON values
   change. The existing macOS deployment targets remain unchanged.

## Rejected experiments and regression discoveries

- Pruning before inlining changed the upstream shared fresh-name supply and
  therefore the encoded consumed procedures. Pruning after inlining preserves
  those bytes; a producer test compares them against unpruned upstream O1.
- Unconditionally pruning UDFs broke `test_function` (`affine` disappeared).
  An explicit model-only compilation mode fixes this; ordinary compilation
  still exports every function. Tests exercise both native and JS modes.
- Enabling integer RNG instructions alone left lexical integer initialization
  able to consume a draw during compilation. Effectful integer assignments
  now request runtime control; a fixture covers a draw before its first loop.
- Upstream rewrites Bernoulli of inverse-logit into Bernoulli-logit. The shared
  family registry handles both, rather than undoing that generic rewrite.
- The RNG/compiler candidate passed 12/13 performance rows; Pareto remained
  at 0.487x. Poisson hurdle/simple reached 1.120x/1.080x and Student-t 0.868x.
  Intermediate evidence: `build-rel/educational-optimized/results.json`.
- A 100,000-draw Pareto profile placed 632/1,448 main-thread samples under
  printf (mostly libc dtoa), motivating numeric formatting. The profile run
  took 0.730 s in NUTS and 1.273 s in output; it is diagnostic, not a gate run.
- Floating `std::to_chars` is unavailable at the macOS 11 deployment target,
  even inside an availability guard. Buffered snprintf alone did not provide
  a useful improvement (five-pair Pareto ratio 0.476x); neither candidate is
  shipped. {fmt} provides the fast algorithm across the supported targets.
  Its negative-NaN spelling initially differed from Apple printf, caught by
  the byte oracle and handled explicitly before accepting the formatter.

## Correctness evidence

The integer-RNG fixture compares compiled and interpreted outputs bitwise for
30 consecutive draws at N=0,1,9, cycling parameters and copying the executor.
It exercises rejection sampling, branches, dynamic indices and Student-t
outputs, and compares the next RNG word. Separate fixtures check dynamic-size
fallback and invalid indices throwing after identical stream consumption.
AddressSanitizer + UndefinedBehaviorSanitizer pass these write-array tests and
program conformance. The existing 246-test Release suite and 254-model corpus
(976,394 compared values) pass; native/JS producer parity passes in both
compilation modes. The new CSV oracle compares legacy printf bytes for
100,000 random double bit patterns, extreme/subnormal values, signed zeros,
NaNs/infinities, notation boundaries, and flushing within a row.

## Final acceptance

Implementation commits: `ea970b26` (RNG), `3770ee3f` (compiler), `d28b13ae`
(CSV). The final Release binary SHA256 is
`392095a0378cdb001b7ee556bd57d5a3a49342e8e176cb447907f6730663be3f`;
it is unchanged after the final rebuild. All 247 CTests pass. The added CSV
oracle also passes ASan+UBSan, alongside the earlier write-array/program tests.

The complete live gate passes 13/13 rows. Pareto is 0.551x vectorized CmdStan,
hurdle-Poisson 1.300x and simple Poisson 2.477x. Pareto preparation fell from
32.60 to 24.54 ms and output from 13.10 to 4.44 ms. Hurdle output fell from
3134.75 to 587.58 ms, with preparation increasing from 19.34 to 39.58 ms;
the extra preparation pays back within roughly eight rows for this fixture.
Simple Poisson output fell from 818.54 to 70.29 ms. Each model retains its
three-point CmdStan oracle and posterior mean checks. The baseline evidence
is preserved, not overwritten; complete tables and raw observations are in
`tests/educational/RESULTS.md` and its linked JSON files.

A second, matched revision comparison alternated baseline/candidate for one
warmup pair and three measured pairs, seeds 1 through 4, using the same
`stanli_command` helper and actual CSV files. All 52 complete output files
were byte-identical. It measured 1.35x Pareto, 4.75x hurdle-Poisson, 8.51x
simple Poisson and 1.96x Student-t improvement. Raw CSVs, stderr and the exact
experiment script remain in `build-rel/educational-revision-ab/`; the checked
in `revision-ab-results.json` records timings, phase data and CSV hashes.

Primary reproduction commands, from the isolated worktree:

```sh
cmake --build build-rel -j8
ctest --test-dir build-rel -j8 --output-on-failure
python3 tools/check_educational.py --benchmark \
  --output build-rel/educational-final-optimized/results.json
ctest --test-dir build-sanitize \
  -R '^(test_write_array|test_mir_program_conformance|test_csv_writer)$' \
  --output-on-failure
```

The wider corpus validation uses the existing external reference policy; no
new numerical tolerance, model exemption or benchmark setting was introduced.
Results describe the supplied small fixtures, not full-size course datasets.
Pareto has the narrowest margin: future preparation changes must retain the
complete wall-time gate. Other supported operating systems are covered by the
portable implementation and CI tests, but their performance was not measured
in this experiment.
