#!/usr/bin/env python3
"""Check that the R and Python laplace_whiten builders agree.

Runs both on the same model, data and starting point (zeros, so Pathfinder
does not enter) and compares what they return:

  q_hat       the marginal mode, relative to 1 + |q_hat|
  theta_hat   the same for the hyperparameters; Python's outer optimizer is
              L-BFGS-B with analytic gradients of the marginal, R's is BFGS
              with forward differences (R's L-BFGS-B aborts on these
              objectives), so this is where the two differ most
  perm, nnz_L, n_clipped, rank   equal when the factored matrices have the
              same pattern, which they do unless an entry sits on the
              threshold
  L           max |L_R - L_py| relative to max |L_py|; the two sides
              difference the gradient at slightly different modes

Models: radon centred (posteriordb), bern_glmm (brms, non-centred intercepts),
crossed_c (brms, crossed centred intercepts), and the saddle model of
tests/test_laplace_whiten.py with both parameters latent, so no outer
optimizer moves off the saddle and the repair is needed.

The tolerances are TOL_MODE, 1e-3 for q_hat, theta_hat and L. The two
optimizers stop on a relative decrease of the objective of 1e-10, which on a
flat marginal leaves theta uncertain to about 1e-4; the largest differences
seen are 2.2e-4 (q_hat and theta_hat, crossed_c) and 1.1e-4 (L, bern_glmm), so
the tolerance is a factor of about 5 above them. perm, nnz_L, n_clipped and
rank are combinatorial and must match exactly.

Needs numpy, scipy, Rscript with the Matrix package and stanli installed
(R CMD INSTALL r), and a built runtime:

    STANLI_RUNTIME=build/libstanli.dylib uv run --no-project --python 3.11 \
        --with numpy --with scipy python tools/laplace_whiten_r_vs_python.py

Exits 0 with a message when Rscript or the R package is unavailable.
"""
import argparse
import pathlib
import shutil
import subprocess
import sys
import tempfile
import warnings

import numpy as np

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

TOL_MODE = {"q_hat": 1e-3, "theta_hat": 1e-3, "L": 1e-3}

SADDLE = """
parameters {
  real x;
  real y;
}
model {
  target += x^2 / 2 - x^4 / 4;
  y ~ normal(0.5 * x, 1);
}
"""

R_SCRIPT = r"""
args <- commandArgs(TRUE)
suppressMessages({library(stanli); library(Matrix)})
code <- paste(readLines(args[1], warn = FALSE), collapse = "\n")
data <- if (args[2] == "-") NULL else args[2]
latent <- strsplit(args[3], ",")[[1]]
out <- args[4]
m <- stanli_model(code = code, data = data)
init <- numeric(m$n_unconstrained)
repair <- args[5]
res <- withCallingHandlers(
  laplace_whiten(m, latent = latent, init = init, seed = 1, repair = repair),
  warning = function(w) invokeRestart("muffleWarning"))
r <- res$report
L <- methods::as(r$L, "TsparseMatrix")
w <- function(x, name) write(format(as.numeric(x), digits = 17), file.path(out, name), ncolumns = 1)
w(r$q_hat, "q_hat"); w(r$theta_hat, "theta_hat"); w(r$perm - 1, "perm")
w(cbind(L@i, L@j, L@x), "L")
w(c(r$n_clipped, r$rank, r$nnz_L, r$gradients$pattern_hessian,
    r$gradients$marginal_mode, r$gradients$final_hessian), "counts")
"""


def models(pdb, tmp):
    import laplace_whiten_acceptance as acc
    out = []
    spec = acc.MODELS_BY_NAME["radon_all-radon_hierarchical_intercept_centered"]
    code, data = acc._inputs(spec, pdb, tmp)
    out.append(("radon_centred", code, data, ["alpha"], "abs"))
    for name, latent in (("bern_glmm", ["z_1"]), ("crossed_c", ["r_1_1", "r_2_1"])):
        spec = acc.MODELS_BY_NAME[name]
        code, data = acc._inputs(spec, pdb, tmp)
        out.append((name, code, data, latent, "abs"))
    out.append(("saddle", SADDLE, None, ["x", "y"], "abs"))
    return out


def run_python(code, data, latent, repair):
    import stanli
    m = stanli.Model(stan_code=code, data=data)
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        _, r = stanli.laplace_whiten(m, latent=latent,
                                     init=np.zeros(m.n_unconstrained),
                                     seed=1, repair=repair)
    L = r.L.toarray()
    return dict(q_hat=r.q_hat, theta_hat=r.theta_hat, perm=np.asarray(r.perm),
                L=L, n_clipped=r.n_clipped, rank=r.rank, nnz_L=r.nnz_L,
                grads=r.gradients)


def run_r(rscript, script, code_path, data, latent, repair, work):
    out = pathlib.Path(work) / "r_out"
    out.mkdir(exist_ok=True)
    cmd = [rscript, str(script), str(code_path),
           "-" if data is None else str(data), ",".join(latent), str(out),
           repair]
    done = subprocess.run(cmd, capture_output=True, text=True)
    if done.returncode:
        raise RuntimeError("R failed:\n" + done.stderr[-2000:])
    def read(n):
        if not (out / n).read_text().strip():
            return np.zeros(0)
        return np.loadtxt(out / n, ndmin=1)
    triplets = read("L").reshape(3, -1).T
    n = len(read("perm"))
    L = np.zeros((n, n))
    L[triplets[:, 0].astype(int), triplets[:, 1].astype(int)] = triplets[:, 2]
    counts = read("counts").astype(int)
    return dict(q_hat=read("q_hat"), theta_hat=read("theta_hat"),
                perm=read("perm").astype(int), L=L, n_clipped=counts[0],
                rank=counts[1], nnz_L=counts[2],
                grads=dict(pattern_hessian=counts[3], marginal_mode=counts[4],
                           final_hessian=counts[5]))


def rel(a, b):
    return float(np.max(np.abs(a - b) / (1 + np.abs(b)))) if len(a) else 0.0


def compare(name, p, r):
    rows = []
    rows.append(("q_hat", rel(r["q_hat"], p["q_hat"]), TOL_MODE["q_hat"]))
    rows.append(("theta_hat", rel(r["theta_hat"], p["theta_hat"]),
                 TOL_MODE["theta_hat"]))
    scale = float(np.max(np.abs(p["L"])))
    rows.append(("L", float(np.max(np.abs(r["L"] - p["L"]))) / scale,
                 TOL_MODE["L"]))
    bad = [f"{k} {v:.2e} > {t:.0e}" for k, v, t in rows if not v <= t]
    for key in ("perm",):
        if not np.array_equal(r[key], p[key]):
            bad.append(f"{key} differs")
    for key in ("nnz_L", "n_clipped", "rank"):
        if r[key] != p[key]:
            bad.append(f"{key} R {r[key]} Python {p[key]}")
    print(f"{name:14s} " + "  ".join(f"{k} {v:.1e}" for k, v, _ in rows)
          + f"  nnz_L {p['nnz_L']}  n_clipped {p['n_clipped']}"
          + f"  rank {p['rank']}"
          + f"  grads R {sum(r['grads'].values())} py "
            f"{p['grads']['pattern_hessian'] + p['grads']['marginal_mode'] + p['grads']['final_hessian']}")
    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--posteriordb", type=pathlib.Path,
                    default=REPO / "deps" / "posteriordb" / "posterior_database")
    ap.add_argument("--rscript", default=shutil.which("Rscript"))
    a = ap.parse_args()
    if not a.rscript:
        print("Rscript not found; skipping")
        return 0
    probe = subprocess.run([a.rscript, "-e",
                            "stopifnot(requireNamespace('stanli'), "
                            "requireNamespace('Matrix'))"],
                           capture_output=True, text=True)
    if probe.returncode:
        print("the R package stanli (or Matrix) is not installed; skipping")
        return 0
    failures = []
    with tempfile.TemporaryDirectory() as tmp:
        script = pathlib.Path(tmp) / "run.R"
        script.write_text(R_SCRIPT)
        for name, code, data, latent, repair in models(a.posteriordb, tmp):
            code_path = pathlib.Path(tmp) / f"{name}.stan"
            code_path.write_text(code)
            p = run_python(code, data, latent, repair)
            try:
                r = run_r(a.rscript, script, code_path, data, latent, repair,
                          tmp)
            except RuntimeError as e:
                failures.append(f"{name}: {e}")
                continue
            failures += [f"{name}: {m}" for m in compare(name, p, r)]
    for f in failures:
        print("FAIL", f)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
