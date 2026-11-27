# tools/hp

A high-precision reference for stanli's log density and gradient, used to
measure how far CmdStan, stanli and stanli `--fast-math` each are from the
true value. The results of the 2026-10-09 run are in
[`notes/performance/2026-10-09-fast-mode-hp-accuracy.md`](../../notes/performance/2026-10-09-fast-mode-hp-accuracy.md).

The reference is an mpmath interpreter over stanli's MIR (`stanc --O0
--debug-optimized-mir`). It evaluates `log_prob_propto_jacobian` at 80 digits
with the same propto term dropping as Stan Math. It needs `mpmath` and, for
speed, `gmpy2`; point `PYTHONPATH` at a directory holding them.

## One model

    PYTHONPATH=... tools/hp/hp_eval.py radon_pooled --out /tmp/hp
    PYTHONPATH=... tools/hp/hp_eval.py gp_pois_regr --dps-check --timeout 600

For each of the three recorded points it prints, per arm (`cmds` CmdStan
reference values, `defa` `build-rel/stanli_check` or the binary that
`--check` names, `fast` the same with `--fast-math`), the log-density and
gradient error. Build `stanli_check` first. `--dps-check` repeats point 0
at twice the precision to show that the reference has converged.
`--no-grad` skips gradients, `--skip-stanli` skips the two stanli arms.

Gradients: up to `--full-grad-max` (300) unconstrained parameters the
reference is a central finite difference per coordinate at step
`10^-(dps/3)`. Above that it is `--dirs` (4) directional derivatives along
unit vectors drawn from `random.Random("MODEL:POINT:K")`, each by a central
difference with two evaluations. The seeds are in the output JSON.
`--grad-index 1,387` adds, above that size, a central difference along each
listed coordinate (1-based, as the ULP ledger names gradient entries), stored
as `grad_coord` in the point's entry.

Transformed parameters and generated quantities:

    PYTHONPATH=... tools/hp/hp_eval.py epinow2_estimate_infections --wa infections.56
    PYTHONPATH=... tools/hp/hp_eval.py MODEL --wa mu sigma.2

`--wa` also runs the model's `generate_quantities` MIR at each point and
stores the whole write_array row, with CmdStan's column names, under `wa` in
the point's entry. A variable whose value depends on an `_rng` call is left
out and listed under `wa_refused` with the function. For each named column,
or every column of a named variable, it prints the 80-digit value and each
arm's distance from it in ULPs of the variable's largest CmdStan value, the
scale that the replay gate uses for write_array. With no names it only
stores the row, which `ledger_accuracy.py` reads.

Errors, in `metrics.py`: log density `|a-b| / max(|a|,|b|,1)`; full gradient
`max|g-r| / max(1, max|g|, max|r|)` (the fast gate in
[TESTING.md](../../TESTING.md#fast-mode)); directional `|g.v - D| / max(1,
|g_cmdstan|_2)` with `|v| = 1`, where `g` is the arm's gradient and `D` the
reference derivative.

## The corpus

    PYTHONPATH=... tools/hp/survey.py --out /tmp/hp --jobs 6 -- --dps-check
    PYTHONPATH=... tools/hp/report.py /tmp/hp

`survey.py` runs `hp_eval.py` once per model and skips models that already
have a JSON file, so an interrupted run resumes. A model that exceeds
`--timeout` is recorded as a timeout, with the points it finished. `report.py` prints coverage, the validity gate, error quantiles and
the models where fast mode differs from default.

Validity: a model is used for conclusions only if every CmdStan and stanli
default error is below 1e-12. Otherwise it is `ill_conditioned` (a member of
`verify_refs.ILL_CONDITIONED`) or `gate_fail`. Fast is `fast_worse` when its
error exceeds twice the default error and 1e-14, `fast_better` for the
mirror image.

## The accuracy columns of TESTING.md

    PYTHONPATH=... tools/hp/survey.py --out /tmp/hp --jobs 26 --timeout 2400 -- --skip-stanli --wa
    PYTHONPATH=... tools/hp/accuracy.py /tmp/hp --check build-rel/stanli_check
    python3 tools/gen_docs.py

`accuracy.py` writes
[`docs/internal/artifacts/accuracy.json`](../../docs/internal/artifacts/accuracy.json).
It scores every log density, gradient entry and write_array value that has an
80-digit value. It scores CmdStan's recorded value and the value that
`stanli_check` gives now. A score is the distance from the 80-digit value in
ULP, with the same scale as the replay gate.

For each model and kind of value, the record has:

- the number of values scored
- the mean and the largest distance for each engine
- `worse`, the values where stanli is more than 10 ULP from the true value and
  farther from it than CmdStan. A value that both engines miss by more than
  1e6 ULP is not listed there.

`--only MODEL ...` scores the named models and merges them into the existing
record. The survey takes about 40 minutes on 26 cores, and the scoring takes
about 3 minutes. Run both on macOS arm64, where the CmdStan references were
recorded, after a change that moves results.

## The ULP ledger

    PYTHONPATH=... tools/hp/ledger_accuracy.py HP_DIR --check build-rel/stanli_check
    PYTHONPATH=... tools/hp/ledger_accuracy.py HP_DIR --only ldaK5 --switch NO_REROLL NO_REROLL+NO_PARTITION
    PYTHONPATH=... tools/hp/ledger_accuracy.py HP_DIR --census --only ch12_m12_5 --switch NO_REROLL

Scores each `main` entry of `tests/ulp_ledger.json` against the 80-digit
values in `HP_DIR` (write_array entries need a run with `--wa`): at the
point and index where stanli is farthest from CmdStan, it prints how many
ULPs of the scale each engine is from the true value, and whether stanli is
`FARTHER` than CmdStan. `--switch` repeats the stanli run with `STANLI_<NAME>` set and scores it at the same point and
index. `--census` scores every value of the listed models instead of the
ledger's entries, and counts how often stanli is the closer one.

    PYTHONPATH=... tools/hp/ledger_accuracy.py HP_DIR --write-true-values

Writes the true values of the `main` entries whose `closer` is `stanli` into
the ledger's `true_values`, which the corpus replay checks stanli against
([TESTING.md](../../TESTING.md#true-values)). An entry gets them when every
point that CmdStan accepts has an 80-digit value for every value that the
entry covers. For a gradient that has to be a full finite-difference
gradient. The distances that the entry's reason states must also agree with
this tool's measure, within one unit of their last digit. The bound is the
largest distance of stanli from the true value over the points. The run
lists every other entry with the reason that it has none: a stale
reference, a stanli value that has moved since the reason was written,
directional derivatives only, or no 80-digit run. `--only` limits the run to
some models and keeps the true values of the others.

## Attribution

    PYTHONPATH=... tools/hp/attribute.py /tmp/hp MODEL...

Reruns `--fast-math` on the stored reference with one pass disabled at a time
through the `STANLI_NO_*` switches (collapse, CSE, partition, re-roll, fused
density, GP diagonal fusion, islands).

## Other files

`census.py`, `census_report.py` and the `probe_*.cpp` files record why
multiprecision stanli (Stan Math with a wider scalar type) was not used.
`mpinterp.py` is the interpreter; `mpdens.py`, `mpextra.py`, `mpfun.py`,
`mptrans.py` and `mpvals.py` hold densities, transforms, functions and value
types. Tests are in `tests/test_hp_reference.py`.
