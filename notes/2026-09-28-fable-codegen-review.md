# Fable review of code-generation decision

Requested through Claude CLI 2.1.283 with model `fable`, effort `high`. Every
assistant response resolved to `claude-fable-5-1`. The review was read-only and
ran no builds or experiments. Raw response, reviewed decision snapshot, prompt
and provenance are retained in `.cache/fable-codegen-review/`.

## Disposition

1. Accepted: present both native timing runs together and link the saved evidence
   JSON. The initial/final variability is explicit. The JSON was created while
   review was in progress; the revised note no longer relies on an ambiguous
   reference.
2. Accepted: withdraw the unsupported ranking of native versus native-host Wasm.
   Wasm-to-Wasm imports in Node do not measure native-host kernel imports.
3. Accepted: distinguish browser Wasm (common to both routes) from the incremental
   choice of native code generation versus an embedded engine. ISA/OS coverage
   and dependency costs need measurement for both.
4. Accepted: document that the Wasm prototype fuses binding and frame, borrowing
   caller buffers whose lifetimes must cover the handle. Add output-adjoint
   consume-and-clear behavior and a repeated-reverse regression to the probe.
   Separate frames, concurrency and stateful kernels remain unproven.
5. Accepted: code-generation value for shipped models remains unquantified. The
   next gate must profile/select actual model regions, not extrapolate synthetic
   loop/branch timing into inference performance.
6. Accepted: add illustrative break-even arithmetic, with the shared four-case
   compilation cost explicitly distinguished from a per-model compile cost.
7. Accepted: report absolute saved time; common copy/zero/seed work is included.
8. Accepted: share browser linker options, record Release for the VM, and note
   that compile-time reporting was added between the initial and final Wasm
   scripts. The post-review probe also tests adjoint consumption.

The updated draft and small probe fixes were checked locally; Fable has not
reviewed the revisions a second time. Backend selection remains open.

## Original review (verbatim)

Review complete. Findings below, ranked by severity, with the verdict at the end.

## Findings

**1. High. The timing table reports only the initial run, and the final run changes the headline row.** The table at `notes/2026-09-28-codegen-decision.md:52-57` matches medians I computed from `.cache/native-probe/results.log`. But the final verification run in `final-results.log` gives a different picture for the 256-instruction case, because the initial run had three VM outliers near 900 to 1070 ns.

| Case | Initial VM / generated (ns) | Final VM / generated (ns) |
| --- | ---: | ---: |
| 16 instructions | 47.6 / 20.9 | 47.3 / 20.8 |
| 256 instructions | 768.6 / 551.0 | 631.6 / 554.0 |
| Branch | 34.0 / 10.0 | 34.8 / 9.9 |
| Cholesky 2x2 | 154.0 / 151.8 | 162.1 / 155.9 |

The 256-instruction advantage is about 14 percent in the final run, not about 40 percent. Line 66 also says both runs are in "the accompanying evidence JSON", but the native evidence is two log files. Fix before presenting: show the final run or both, and correct the file reference.

**2. High. The recommendation's rationale is unmeasured, and the Wasm probe does not stand in for the alternative.** Line 25 says the recommended route "prioritizes direct native performance". No native-versus-Wasm comparison exists in any log. Worse, the browser probe measured a Wasm-to-Wasm import inside one V8 instance (`tools/probe_generated_kernel.cjs:25-28`). The alternative at lines 18-20 is native-host Wasm with host kernel imports, which crosses a Wasm-to-native boundary this probe never exercised. The few-nanosecond import delta therefore says nothing for or against native-host Wasm. Reword the recommendation as a default for designing the discriminating experiment, not a performance-based preference.

**3. Medium. The "two targets versus one" framing understates the real choice.** A Wasm emitter is required under both routes, since browsers have no toolchain. The actual delta is native emitter versus embedded engine. "Native" is itself x86-64 plus arm64 plus per-OS executable-memory regimes, and the probe is Unix-only (line 140). If a packaged backend handles the ISAs, that backend is a dependency of comparable weight to a Wasm engine. The note's refusal to commit to a backend is correct, but the dependency-size axis may not discriminate between routes. Say so.

**4. Medium. The ownership description does not match the Wasm bridge.** Lines 72-73 say the handle owns metadata and the caller owns buffers. `tools/probe_kernel_bridge.cpp:16-19` captures raw buffer addresses into the handle at create time, so binding and frame are fused, contrary to proposal item 2 at lines 99-100. Also the Wasm bridge never clears the output adjoint after backward, unlike `runtime/src/adjoint.cpp:1006` and the native bridge at `tools/probe_native_program.cpp:217`. The JS reseeds before every reverse (`probe_generated_kernel.cjs:43,49`), so consume-and-clear semantics are untested on the Wasm side. The prototype demonstrates the contract only for a stateless kernel: the Cholesky kernel reads none of scratch, udata or eval_state (`runtime/kernels/matrix_fns.cpp:218-241`). Lines 111-114 say this; keep it prominent.

**5. Medium. The probes are synthetic, so the "does codegen matter for real models" question stays open.** The programs at `probe_native_program.cpp:22-68` are hand-built chains, not census-selected regions as the roadmap and my earlier review asked. The census note reports selected structure, not runtime shares. Nothing here says what fraction of a real gradient sits in VM dispatch. Line 59 admits this. The decision text should state plainly that the value of any codegen route is still unquantified for shipped models.

**6. Low. Break-even arithmetic is required by the roadmap but absent.** The inputs are present: 256 ms compile, 371 µs load, and per-evaluation savings of roughly 25 to 80 ns from the logs. Derived from those numbers, break-even is in the millions of evaluations, far beyond both the 2,000 and 20,000 gradient budgets. That supports the note's own conclusion that toolchain spawning is not a shipping option. State it.

**7. Low. Harness overhead is common-mode, so ratios are conservative but absolute deltas are the usable quantity.** Both sides of `evaluate` at `probe_native_program.cpp:223-237` copy inputs, zero adjoints and seed outputs. For the 16-instruction case that overhead is a large share of the 20.9 ns generated figure. Report deltas, not speedup factors.

**8. Low. Provenance and build hygiene.** The initial `wasm-kernel-probe.json` lacks the compile timing that the final JSON has, so the script changed between runs; note that. The `probe_codegen_wasm` target at `CMakeLists.txt:601-615` duplicates every link option of the shipped target instead of sharing them, so it can silently drift from "the current runtime" the note claims to test. The generated C++ was compiled at -O3 while the runtime defaults to RelWithDebInfo (`CMakeLists.txt:33-34`); the note should record the VM build type used.

## What I verified as correct

The native emitter matches VM semantics at every point I checked: JZ compares the double to zero (`runtime/include/stanli/program.hpp:718` versus emitter line 101), segment guards (`adjoint.cpp:1272` versus line 181), MUL reverse using checkpointed value indices (`adjoint.cpp:1052` versus line 152), and the backward CALL binding (`adjoint.cpp:986-1006` versus bridge lines 200-217). Forward CALL uses the same runtime binding path (`runtime/src/executor.cpp:80-94,252`). The bitwise comparison includes signed zero. The shipped Wasm target and webR exports are untouched. The scalar RNG note describes completed, independently validated production work and is correctly separated from the probes.

## Evidence limits

I ran nothing. I could not confirm whether the runtime export passed as an import was a raw Wasm function or an Emscripten wrapper, which affects what the import measurement isolates. I could not check the 0.5 KB module size claim. No webR, browser, CSP or hardened-runtime evidence exists.

## Verdict

The evidence supports three decisions now: adopt a shared lowered representation and kernel contract as the design axis, continue codegen research, and defer backend choice. It does not support ranking separate native and Wasm targets over native-host Wasm. Nothing here is shipping-ready, and nothing in the probes is meant to be. Present options only after fixing the table, the "direct native performance" rationale, the ownership sentence, and the missing break-even arithmetic. None of those changes the architectural conclusion, but the table fix changes a number the user will read.
