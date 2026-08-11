#!/usr/bin/env python3
"""Differential conformance: stanli's BridgeStan ABI against BridgeStan's.

stanli's own tests can only say that the facade agrees with stanli. They
cannot catch the case that matters most here -- the facade and its tests
sharing a wrong idea of what a BridgeStan call means -- because both sides
of that comparison come from the same head. The only thing that can is a
second implementation, so this drives the REFERENCE BridgeStan and the
stanli runtime through the same client and compares what they say.

It needs a C++ toolchain, because compiling the reference model is the
whole point. CI has one; stanli's users never do, which is the reason
stanli exists.

    tools/bs_conformance.py MODEL.stan [--data DATA.json] [--tol 0]

Exits non-zero on the first disagreement that is not a documented
difference (see EXPECTED_DIFFERENCES).
"""
import argparse
import ctypes
import json
import pathlib
import sys

import numpy as np

# Where the two implementations are known to differ, with the reason. Any
# other difference is a bug in the facade. Keep this list short and
# justified: it is the list of things stanli does not promise.
EXPECTED_DIFFERENCES = {
    "model_info": "free text; stanli names its own build, not Stan's",
    "name": "stanli takes the name from the embedded manifest, "
            "BridgeStan from the compiled model class",
}


class Reference:
    """The reference BridgeStan library, through its own Python client."""

    def __init__(self, stan_file, data):
        import bridgestan
        self.model = bridgestan.StanModel.from_stan_file(
            str(stan_file), model_data=data, capture_stan_prints=False)

    def unc_num(self):
        return self.model.param_unc_num()

    def num(self, tp, gq):
        return self.model.param_num(include_tp=tp, include_gq=gq)

    def names(self, tp, gq):
        return self.model.param_names(include_tp=tp, include_gq=gq)

    def unc_names(self):
        return self.model.param_unc_names()

    def log_density(self, q):
        return self.model.log_density(np.asarray(q), propto=True,
                                      jacobian=True)

    def log_density_gradient(self, q):
        return self.model.log_density_gradient(np.asarray(q), propto=True,
                                               jacobian=True)

    def constrain(self, q, tp, gq, seed):
        rng = self.model.new_rng(seed=seed) if gq else None
        return self.model.param_constrain(np.asarray(q), include_tp=tp,
                                          include_gq=gq, rng=rng)


class Stanli:
    """The stanli runtime, through the same client, so the client is not
    a variable in the comparison."""

    def __init__(self, stan_file, data):
        import stanli
        self.model = stanli.bridgestan_model(stan_file=stan_file, data=data,
                                             capture_stan_prints=False)

    unc_num = Reference.unc_num
    num = Reference.num
    names = Reference.names
    unc_names = Reference.unc_names
    log_density = Reference.log_density
    log_density_gradient = Reference.log_density_gradient
    constrain = Reference.constrain


# Every numeric comparison made, as (worst relative deviation, worst ulp,
# what). Reported whether or not anything failed: a run that passes at a
# tolerance is only reassuring if you can see how much room it had.
WORST = []


def _deviation(got, want):
    """Relative deviation and ulp distance, elementwise, nonfinite-safe.

    Nonfinite values compare equal only to themselves, and a -inf against
    a finite number is an infinite deviation rather than the NaN that
    abs(-inf - x)/inf gives -- the bug that once let a density with no
    support pass as verified.
    """
    got, want = np.asarray(got, float), np.asarray(want, float)
    both_finite = np.isfinite(got) & np.isfinite(want)
    same_nonfinite = (~both_finite) & (got == want) | (np.isnan(got)
                                                       & np.isnan(want))
    denom = np.where(np.abs(want) > 0, np.abs(want), 1.0)
    # Only the finite entries go through the subtraction: inf - inf is a
    # NaN and a warning, and neither is the answer here.
    rel = np.full(got.shape, np.inf)
    diff = np.zeros(got.shape)
    np.subtract(got, want, out=diff, where=both_finite)
    np.divide(np.abs(diff), denom, out=rel, where=both_finite)
    rel = np.where(same_nonfinite, 0.0, rel)
    # ulp distance via the monotone integer ordering of IEEE doubles.
    gi = got.view(np.int64) if got.dtype == np.float64 else \
        got.astype(np.float64).view(np.int64)
    wi = want.view(np.int64) if want.dtype == np.float64 else \
        want.astype(np.float64).view(np.int64)
    gi = np.where(gi < 0, np.int64(-(2**63)) - gi, gi)
    wi = np.where(wi < 0, np.int64(-(2**63)) - wi, wi)
    ulp = np.where(both_finite, np.abs(gi - wi), 0)
    return rel, ulp


def compare(name, got, want, failures, tol=0.0):
    if isinstance(want, (list, tuple)) and all(isinstance(x, str)
                                               for x in want):
        got, want = list(got), list(want)
        if got != want:
            extra = [x for x in got if x not in want]
            missing = [x for x in want if x not in got]
            failures.append(
                f"{name}: {len(got)} vs {len(want)} names"
                + (f"; only ours: {extra[:4]}" if extra else "")
                + (f"; only theirs: {missing[:4]}" if missing else "")
                + ("; same set, different order" if not extra and not missing
                   else ""))
        return
    if isinstance(want, (float, list, tuple, np.ndarray)):
        got_a = np.atleast_1d(np.asarray(got, float))
        want_a = np.atleast_1d(np.asarray(want, float))
        if got_a.shape != want_a.shape:
            failures.append(f"{name}: shape {got_a.shape} vs {want_a.shape}")
            return
        rel, ulp = _deviation(got_a, want_a)
        WORST.append((float(rel.max()), int(ulp.max()), name))
        bad = rel > tol
        if bad.any():
            i = int(np.argmax(rel))
            failures.append(
                f"{name}: [{i}] {got_a.flat[i]!r} vs {want_a.flat[i]!r}, "
                f"rel {rel.flat[i]:.2e} ({ulp.flat[i]} ulp); "
                f"{int(bad.sum())} of {bad.size} over tolerance")
        return
    if got != want:
        failures.append(f"{name}: {got!r} vs {want!r}")


def run(stan_file, data, tol, point_seed=7):
    ref, ours = Reference(stan_file, data), Stanli(stan_file, data)
    failures = []

    n = ref.unc_num()
    compare("param_unc_num", ours.unc_num(), n, failures)
    if ours.unc_num() != n:
        return failures  # nothing below is comparable

    for tp in (False, True):
        for gq in (False, True):
            tag = f"(include_tp={tp}, include_gq={gq})"
            compare(f"param_num {tag}", ours.num(tp, gq), ref.num(tp, gq),
                    failures)
            compare(f"param_names {tag}", ours.names(tp, gq),
                    ref.names(tp, gq), failures)
    compare("param_unc_names", ours.unc_names(), ref.unc_names(), failures)

    rng = np.random.default_rng(point_seed)
    for k in range(3):
        q = rng.normal(size=n) * 0.5
        tag = f"at point {k}"
        compare(f"log_density {tag}", ours.log_density(q), ref.log_density(q),
                failures, tol)
        o_lp, o_g = ours.log_density_gradient(q)
        r_lp, r_g = ref.log_density_gradient(q)
        compare(f"log_density_gradient value {tag}", o_lp, r_lp, failures, tol)
        compare(f"log_density_gradient {tag}", o_g, r_g, failures, tol)
        # Constrained output without generated quantities is deterministic
        # on both sides; with them the streams are each implementation's
        # own, so only the SHAPE is comparable.
        for tp in (False, True):
            compare(f"param_constrain(tp={tp}, gq=False) {tag}",
                    ours.constrain(q, tp, False, 1),
                    ref.constrain(q, tp, False, 1), failures, tol)
        compare(f"param_constrain gq width {tag}",
                len(ours.constrain(q, True, True, 1)),
                len(ref.constrain(q, True, True, 1)), failures)
    return failures


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("model", type=pathlib.Path)
    ap.add_argument("--data", default=None)
    ap.add_argument("--tol", type=float, default=1e-9,
                    help="relative deviation allowed, matching the corpus "
                         "oracle's gate. Not bitwise: the two libraries "
                         "are separate compilations of stan-math, so a "
                         "last-ulp disagreement is the toolchain, not a "
                         "bug. The bug class this catches is orders of "
                         "magnitude wide.")
    args = ap.parse_args()
    data = args.data
    if data is not None and pathlib.Path(data).exists():
        data = pathlib.Path(data).read_text()

    failures = run(args.model, data, args.tol)
    for f in failures:
        print(f"DIFFER {f}")
    if WORST:
        rel, ulp, what = max(WORST)
        print(f"{args.model.name}: worst {rel:.2e} relative ({ulp} ulp) "
              f"at {what}, over {len(WORST)} comparisons")
    print(f"{args.model.name}: "
          + ("CONFORMS" if not failures
             else f"{len(failures)} differences over {args.tol:g}"))
    if EXPECTED_DIFFERENCES:
        print("not compared: " + ", ".join(sorted(EXPECTED_DIFFERENCES)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
