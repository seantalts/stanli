# Testing

How this project convinces itself the numbers are right, and how it knows
what is covered. Two different questions, answered by different machinery;
this page maps all of it and says plainly what each layer cannot see.

The standing bar, everywhere: a supported density's gradients match CmdStan
**bitwise** at the tested points. Where bitwise is impossible the budget is
2 ULP, and a tolerance may only be chosen after the actual error has been
measured and its mechanism named in a comment. A gradient that "merely"
agrees to 1e-12 is a bug report, not a feature.

## The layers

| layer | scope | runs | command |
| --- | --- | --- | --- |
| unit and kernel tests | one kernel or pass, pinned against stan-math | every PR | `(cd build-rel && ctest -j4)` |
| corpus oracle | 129 real models, lp + full gradient vs recorded CmdStan | every PR | `python3 tools/verify_refs.py deps/posteriordb --check build-rel/stanli_check --jobs 4` |
| conformance sweep | all ~24k signatures + 31 constructs vs live BridgeStan | nightly, and locally on demand | see below |
| coverage ratchet | classifications vs a checked-in baseline, directional | inside the nightly | `--baseline docs/conformance-baseline.json.gz` |
| doc consistency | headline numbers vs their artifacts | every PR | `python3 tools/gen_docs.py --check` |
| formatting | clang-format over tracked C/C++ | every PR | `./tools/format.sh --check` |

## Unit and kernel tests

The ctest suite pins kernels against stan-math itself: the test constructs
the same call CmdStan's generated C++ would make, on `stan::math::var`, and
compares value plus every gradient, bitwise by default (`expect_eq`).

Two practices are house style rather than suggestion:

- **RED first.** A fix's test is verified to fail against the pre-fix
  source -- stash the runtime change, rebuild, watch it fail for the right
  reason -- before the fix counts. A test that was never red proves nothing
  about the bug it claims to pin.
- **Reference activity is chosen on purpose.** A same-activity var
  reference gives a bitwise pin; CmdStan's mixed data/var instantiation
  reassociates and lands tens of ULP away. Tests say which they use and
  why, and never split the difference with a loose tolerance.

Fixture MIR is generated from checked-in .stan files with the pinned
compiler: `./deps/stanc3/stanc --O1 --debug-optimized-mir tests/fixtures/X.stan
> tests/fixtures/X.tmir.sexp` (then delete the `.hpp` stanc drops next to
it).

What this layer cannot see: a whole execution path computing something
else. A kernel test exercises the kernel you thought to write it for.

## The corpus oracle

`tools/verify_refs.py` replays every referenced posteriordb model -- plus
the language-construct models in `tests/stanc3/` -- against CmdStan's
recorded log density and full unconstrained gradient, gated at 1e-9
relative with per-model deviations tracked. This is the strongest oracle in
the repo and has caught what unit tests missed: models silently wrong by
large factors have reached main with every test green, and only this diff
flagged them.

References live in `docs/corpus-refs.json.gz`, recorded by the local
CmdStan rig (`tools/verify_sample.py`; needs `--corpus` from dev_setup),
and the file names the CmdStan, Stan, Math, stanc3 and posteriordb
revisions its values came out of. Every model carries a reference at
every one of the three points, including the points CmdStan itself
refuses -- those record the refusal, which stanli then has to reproduce.
Recording is a reviewed act, like advancing the baseline: regenerating
references to make a diff go away defeats the oracle.

What this layer cannot see: language surface no real posterior uses,
which is most of it. The conformance sweep exists for that. It used to
be blind to any evaluation point other than the one recorded, which is
two gaps -- a crash at an unreferenced point, and a wrong-but-finite
gradient there with nothing to be wrong against. The three-point replay
closes the first and the three-point references close the second.

## The conformance sweep

`harnesses/stan_conformance.py` inventories every signature the pinned
stanc knows (`--dump-stan-math-signatures`) plus a catalog of named
language constructs, generates a type-directed model for each case, and
evaluates both sides -- stanli, and reference BridgeStan built from the
identical pinned CmdStan/Stan/Math source with the identical source-built
stanc, compiled from the same `--O1` MIR stanli reads. Comparison is lp
plus the complete gradient at deterministic probe points, default gate
10 ULP, with reviewed exceptions in `harnesses/conformance/policy.toml` --
each rule carries a mandatory reason naming its mechanism, and stale rules
flag the run.

Run it locally (macOS works; only the nightly *workflow* is Linux):

```sh
tools/dev_setup.sh --conformance          # one-time setup, idempotent
.venv-conformance/bin/python harnesses/stan_conformance.py \
  --stanc deps/stanc3/stanc-pinned --cmdstan deps/cmdstan \
  --build build-rel --stanli-pythonpath python \
  --filter beta_binomial --output /tmp/conf
```

The driver must run under the venv interpreter (TOML support), a filtered
run reporting `partial_run` is expected, and the staged library in
`python/stanli/_bin/` must be re-copied after every rebuild or you are
testing a stale runtime. `harnesses/conformance/README.md` has the rest.

**What blocks a run** is disagreement, not absence: `mismatch` (stanli
answered, differently), `crashed` (the runtime process died -- a segfault
is not a coverage gap), and `harness_error` (the rig itself failed). A
function stanli has not implemented yet is reported, counted, and given a
reproducer, and does not fail CI. That framing is deliberate: this runtime
is built out from the ground up, and a gate that is red for "there is
still work to do" is a gate nobody reads.

What this layer cannot see: behavior reached through operators rather than
function calls (`B / A` silently meaning elementwise division was never a
red row, before or after its fix), anything the generator cannot build a
case for (`generator_gap`), and cost -- conformance asks "same answer",
never "at what price".

## The coverage ratchet

Relaxing the gate to ignore coverage gaps opened a hole: a regression that
turns a verified function into a compile refusal lands in the non-blocking
bucket, and a green run would not mention the loss. The nightly therefore
compares every classification against `docs/conformance-baseline.json.gz`,
**directionally**: a case that verified and no longer does blocks
(`coverage_regressed:N`, IDs listed), a vanished case blocks, a moved
toolchain pin blocks (it invalidates the comparison). Improvements are
recorded and stay green -- wiring a function up must not turn CI red, or
the baseline gets re-stamped on reflex until nobody reads it.

Advancing the baseline is a reviewed act: rerun with `--update-snapshot`
and commit the result in the same PR as the work that earned it.

## Numeric standards, in one place

- Bitwise wherever the two sides share arithmetic. The default.
- 2 ULP is the outer budget for a var-path comparison, used only with the
  divergence mechanism named in a comment (e.g. adjoint contributions
  summed on a fresh tape per op associate differently than on one tape).
- Policy gates in `policy.toml` are for cross-implementation differences
  with an identified cause -- probe sums that cancel toward zero take an
  `abs_tol`, because ULP and relative error describe the cancellation
  rather than the disagreement.
- A tolerance chosen before the error was measured is a defect. One was
  once written as 1e-13 with a plausible rationale; the measured error was
  exactly zero.

## Coverage: what is supported, and how we know

Support is never inferred from stanli's own dispatch tables -- a function
listed in a table no path can reach has twice been "supported" that way.
A name counts as covered when the conformance sweep reports no
`unexpected_unsupported` row for it anywhere in the signature space.

Every case carries exactly one status: `verified`, `mismatch`, `crashed`,
`harness_error`, `unexpected_unsupported` (the to-do list),
`expected_unsupported` (a reviewed policy boundary -- complex types, tuple
results -- never created by observing a refusal), `generator_gap` (the
harness cannot build the case), or `inapplicable` (nothing real to test).
Every red row has a copy-pasteable repro command, and `unsupported.md`
in each run's output groups the backlog by function.

The human-facing table in `docs/coverage.md` regenerates from a nightly
aggregate with the snippet embedded in that file; drift between the table
and the sweep is a bug in the table. A separate nightly job
(`signature-watch`) diffs the pinned signature dump against stanc3
upstream, so new language surface is reported the day it appears; adopting
it is a pin advance that re-inventories everything.

## Practices that exist because something went wrong

- Verify a merge landed the commits you think it did; a follow-up commit
  orphaned during a merge left a README pointing at an artifact nothing
  produced.
- A green nightly covers the commit it was dispatched against, not the
  merges since; check the run's head SHA before citing it.
- A crash at one evaluation point can hide behind success at another; the
  corpus recorder once selected the working point automatically, which is
  the opposite of what an oracle is for.
- Numbers nothing recomputes are numbers that drift. The binary-size
  table and the coverage table both rotted this way; both are generated
  now, and `gen_docs.py --check` fails CI when the stamped numbers
  disagree with their artifacts.

## In progress

Two additions are landing as of late August 2026: corpus replay at all
three deterministic points with crash-at-any-point a hard failure, and an
ASan job over the ctest suite. A third is designed and building: a
cross-path agreement harness comparing the op graph, the MIR interpreter,
and the register program against each other on the same models -- the
stanli-against-itself axis that four this-month bugs (one path taught, the
other not) showed was missing. Until it lands, transformed data has a
single engine and no stanli-internal cross-check; that class is caught
only on the CmdStan axis.
