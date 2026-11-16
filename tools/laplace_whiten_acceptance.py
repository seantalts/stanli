#!/usr/bin/env python3
"""Compare ESS per gradient of stanli.laplace_whiten against the default sampler.

Reproduces the M200 column of the #540 follow-up benchmark on six models.
For each model and seed it runs

  A  the default sampler: diag_e, warmup 1000, 4 chains x 1000 draws
  M  laplace_whiten(...) then unit_e with step-size-only adaptation,
     warmup 200, 4 chains x 1000 draws

and scores both as the benchmark does: min bulk ESS over the constrained
parameters per gradient evaluation, warmup included, pooled over the seeds.
M is also charged the builder's gradient evaluations and its Pathfinder wall
time converted to gradients at A's rate (A's gradients per second). A run
counts as zero ESS when a posterior mean is more than 0.3 reference sd from
the reference mean or the maximum R-hat exceeds 1.05.

Everything comes from the repository. The radon and irt_2pl models and data
are read from deps/posteriordb. The other four are generated:
tools/laplace_whiten_stan/ holds their Stan code and tools/laplace_whiten_models.py
simulates their data with fixed seeds (the benchmark drew its data in R, so
the values differ and the ratios agree only approximately). The reference
means come from a long default-sampler run (diag_e, warmup 2000, 4 chains x
5000 draws, seed 9001) made on first use and cached under build-acceptance/,
which git ignores. The checkout of posteriordb that dev_setup.sh makes has no
reference draws, so none are read from there.

Needs numpy, scipy and a built runtime (python/stanli/_bin):

    uv run --no-project --python 3.11 --with numpy --with scipy \\
        python tools/laplace_whiten_acceptance.py
"""
import argparse
import concurrent.futures as cf
import json
import math
import multiprocessing
import os
import pathlib
import sys
import tempfile
import time
import warnings
import zipfile

import numpy as np

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
SEEDS = (1, 2)
REFERENCE_SEED = 9001

# name, kind (pdb: posteriordb, gen: tools/laplace_whiten_models.py), latent
# parameters, benchmark M200 ratio (per_model2.csv)
MODELS = [
    dict(name="radon_all-radon_hierarchical_intercept_centered", kind="pdb",
         model="radon_hierarchical_intercept_centered", data="radon_all",
         latent=["alpha"], bench=76.68),
    dict(name="crossed_c", kind="gen", gen="crossed_c", latent=["r_1_1", "r_2_1"], bench=34.26),
    dict(name="gauss_slopes", kind="gen", gen="gauss_slopes", latent=["z_1"], bench=12.34),
    dict(name="bern_glmm", kind="gen", gen="bern_glmm", latent=["z_1"], bench=1.461),
    dict(name="irt_2pl-irt_2pl", kind="pdb", model="irt_2pl", data="irt_2pl",
         latent=["theta", "a", "b"], bench=0.02975),
    dict(name="spatial_binom_gmrf", kind="gen", gen="spatial_binom_gmrf", latent=["phi"], bench=3.186),
]
MODELS_BY_NAME = {m["name"]: m for m in MODELS}


def _inputs(spec, pdb, tmp):
    if spec["kind"] == "pdb":
        pdb = pathlib.Path(pdb)
        code = (pdb / "models" / "stan" / (spec["model"] + ".stan")).read_text()
        with zipfile.ZipFile(pdb / "data" / "data" / (spec["data"] + ".json.zip")) as z:
            data = pathlib.Path(tmp) / (spec["data"] + ".json")
            data.write_bytes(z.read(z.namelist()[0]))
        return code, data
    import laplace_whiten_models
    return laplace_whiten_models.generate(spec["gen"], tmp)


def _reference_path(cache, name):
    return pathlib.Path(cache) / f"{name}.ref.json"


def make_reference(args):
    """Reference posterior means and sds (name -> [mean, sd]) from a long
    default-sampler run, cached under `cache`."""
    name, pdb, cache = args
    out = _reference_path(cache, name)
    if out.exists():
        return name, "cached"
    os.environ.update(VECLIB_MAXIMUM_THREADS="1", OMP_NUM_THREADS="1",
                      OPENBLAS_NUM_THREADS="1")
    sys.path.insert(0, str(REPO / "python"))
    import stanli
    with tempfile.TemporaryDirectory() as tmp:
        code, data = _inputs(MODELS_BY_NAME[name], pdb, tmp)
        model = stanli.Model(stan_code=code, data=data)
        fit = model.sample(chains=4, warmup=2000, samples=5000,
                           seed=REFERENCE_SEED, parallel_chains=1, refresh=0,
                           metric="diag_e")
    draws = fit.draws().reshape(-1, len(fit.names))
    ref = {nm: [float(draws[:, j].mean()), float(draws[:, j].std(ddof=1))]
           for j, nm in enumerate(fit.names) if nm != "lp__"}
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(ref))
    return name, "made"


def _sample(model, warmup, seed, **kw):
    import stanli
    fit = model.sample(chains=4, warmup=warmup, samples=1000, seed=seed,
                       save_warmup=True, parallel_chains=1, refresh=0, **kw)
    st = fit.sampler_stats
    nl = stanli.SAMPLER_COLUMNS.index("n_leapfrog__")
    dv = stanli.SAMPLER_COLUMNS.index("divergent__")
    grads = float(st[:, :, nl].sum())
    post = stanli.Fit(fit.names, np.ascontiguousarray(fit.draws()[:, warmup:, :]),
                      st[:, warmup:], fit.max_depth, fit.seed)
    return fit, post, grads, int(st[:, warmup:, dv].sum())


def _score(post, reference):
    s = post.summary()
    d = post.draws()
    worst, where, matched = 0.0, "", 0
    for j, nm in enumerate(post.names):
        if nm not in reference:
            continue
        mu, sd = reference[nm]
        matched += 1
        if not (sd > 0 and math.isfinite(sd)):
            continue
        e = abs(float(d[:, :, j].mean()) - mu) / sd
        if not math.isfinite(e):
            e = float("inf")
        if e > worst:
            worst, where = e, nm
    ess = np.asarray(s.ess_bulk)[[nm in reference for nm in post.names]]
    rhat = np.asarray(s.r_hat)[[nm in reference for nm in post.names]]
    ess = float(np.nanmin(ess)) if np.any(np.isfinite(ess)) else float("nan")
    rh = float(np.nanmax(rhat)) if np.any(np.isfinite(rhat)) else float("nan")
    return dict(ess=ess, rhat=rh, err=worst, err_param=where, matched=matched,
                flagged=bool(worst > 0.3 or not rh <= 1.05))


def run_task(args):
    name, seed, mode, pdb, cache, repair = args
    os.environ.update(VECLIB_MAXIMUM_THREADS="1", OMP_NUM_THREADS="1",
                      OPENBLAS_NUM_THREADS="1")
    sys.path.insert(0, str(REPO / "python"))
    import stanli
    spec = MODELS_BY_NAME[name]
    out = dict(model=name, seed=seed, mode=mode)
    try:
        with tempfile.TemporaryDirectory() as tmp:
            code, data = _inputs(spec, pdb, tmp)
            reference = json.loads(_reference_path(cache, name).read_text())
            m = stanli.Model(stan_code=code, data=data)
            out["n"] = m.n_unconstrained
            t = time.time()
            if mode == "A":
                fit, post, grads, div = _sample(m, 1000, seed, metric="diag_e")
                out.update(grads=grads, wall=time.time() - t, builder_grads=0,
                           pf_wall=0.0)
            else:
                with warnings.catch_warnings(record=True) as caught:
                    warnings.simplefilter("always")
                    w, rep = stanli.laplace_whiten(
                        m, spec["latent"], seed=seed, repair=repair)
                out["warnings"] = [str(c.message)[:120] for c in caught]
                build = time.time() - t
                t = time.time()
                fit, post, grads, div = _sample(
                    w, 200, seed, metric="unit_e", adapt_metric=False)
                out.update(grads=grads, wall=time.time() - t, build_wall=build,
                           builder_grads=rep.gradients["total"],
                           pf_wall=rep.pathfinder_seconds,
                           n_clipped=rep.n_clipped, nnz_frac=rep.nnz_L / rep.n ** 2,
                           ordering=rep.ordering,
                           theta_hat=[float(x) for x in rep.theta_hat],
                           gradients=rep.gradients)
            out.update(_score(post, reference), div=div)
            out["status"] = "ok"
    except Exception as e:
        out["status"] = "error: " + str(e)[:300].replace("\n", " ")
    return out


def pooled(runs, charge):
    ess = grads = 0.0
    nflag = 0
    for r in runs:
        if r["status"] != "ok":
            return None, 0
        e = r["ess"]
        if r["flagged"] or not math.isfinite(e):
            nflag += r["flagged"]
            e = 0.0
        ess += e
        grads += charge(r)
    return 1000 * ess / grads, nflag


def fmt(x):
    if x is None or not math.isfinite(x):
        return "n/a"
    if x == 0:
        return "fails"
    return f"{x:.3g}x faster" if x >= 1 else f"{1 / x:.3g}x slower"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--cache", type=pathlib.Path,
                    default=REPO / "build-acceptance",
                    help="where reference means are kept (default "
                    "build-acceptance/, which git ignores); delete a "
                    "file there to recompute it")
    ap.add_argument("--posteriordb", type=pathlib.Path,
                    default=REPO / "deps" / "posteriordb" / "posterior_database")
    ap.add_argument("--models", nargs="*", default=[m["name"] for m in MODELS])
    ap.add_argument("--repair", choices=["clip", "abs", "abs_dense"],
                    default="abs",
                    help="laplace_whiten eigenvalue repair (default abs)")
    ap.add_argument("--jobs", type=int, default=28)
    ap.add_argument("--json", type=pathlib.Path, help="write raw per-run results here")
    a = ap.parse_args()
    tasks = [(nm, s, mode, str(a.posteriordb), str(a.cache), a.repair)
             for nm in a.models for s in SEEDS for mode in ("A", "M")]
    ctx = multiprocessing.get_context("spawn")
    with cf.ProcessPoolExecutor(a.jobs, mp_context=ctx) as ex:
        for nm, how in ex.map(make_reference,
                              [(nm, str(a.posteriordb), str(a.cache))
                               for nm in a.models]):
            print(f"reference {nm}: {how}", file=sys.stderr, flush=True)
        results = list(ex.map(run_task, tasks))
    if a.json:
        a.json.write_text(json.dumps(results, indent=1, default=float))

    by = {}
    for r in results:
        by.setdefault(r["model"], {}).setdefault(r["mode"], {})[r["seed"]] = r
    print("| model | benchmark M200 | ours (pooled) | ours seed 1 | "
          "ours seed 2 | flags | clipped | fill % | ours/benchmark |")
    print("|---|---|---|---|---|---|---|---|---|")
    bad = 0
    for nm in a.models:
        spec = MODELS_BY_NAME[nm]
        A = [by[nm]["A"][s] for s in SEEDS]
        M = [by[nm]["M"][s] for s in SEEDS]
        err = [r["status"] for r in A + M if r["status"] != "ok"]
        if err:
            print(f"| {nm} | {fmt(spec['bench'])} | {err[0]} |||||||")
            bad += 1
            continue
        rate = {r["seed"]: r["grads"] / r["wall"] for r in A}

        def charge(r):
            return r["grads"] + r["builder_grads"] + r["pf_wall"] * rate[r["seed"]]

        va, _ = pooled(A, lambda r: r["grads"])
        vm, nf = pooled(M, charge)
        per = []
        for s in SEEDS:
            a1, _ = pooled([by[nm]["A"][s]], lambda r: r["grads"])
            m1, _ = pooled([by[nm]["M"][s]], charge)
            per.append(fmt(m1 / a1) if a1 else "n/a")
        ratio = vm / va if va else float("nan")
        agree = ratio / spec["bench"] if ratio > 0 else 0.0
        flags = f"{sum(r['flagged'] for r in M)}M/{sum(r['flagged'] for r in A)}A"
        clipped = "/".join(str(r["n_clipped"]) for r in M)
        fill = "/".join(f"{100 * r['nnz_frac']:.1f}" for r in M)
        print(f"| {nm} | {fmt(spec['bench'])} | {fmt(ratio)} | {per[0]} | {per[1]} | "
              f"{flags} | {clipped} | {fill} | {agree:.2f} |")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
