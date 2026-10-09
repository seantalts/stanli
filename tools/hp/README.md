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
reference values, `defa` `build-rel/stanli_check`, `fast` the same with
`--fast-math`), the log-density and gradient error. Build `stanli_check`
first. `--dps-check` repeats point 0 at twice the precision to show that the
reference has converged. `--no-grad` skips gradients, `--skip-stanli` skips
the two stanli arms.

Gradients: up to `--full-grad-max` (300) unconstrained parameters the
reference is a central finite difference per coordinate at step
`10^-(dps/3)`. Above that it is `--dirs` (4) directional derivatives along
unit vectors drawn from `random.Random("MODEL:POINT:K")`, each by a central
difference with two evaluations. The seeds are in the output JSON.

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
