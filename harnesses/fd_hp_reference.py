#!/usr/bin/env python3
"""70-digit log density and gradient of long-vector density models, against stanli arms.

python3 harnesses/fd_hp_reference.py truth OUT.json [--models a,b]
python3 harnesses/fd_hp_reference.py compare TRUTH.json --arm NAME=BIN[,VAR=VAL...] ... [--exact A]

truth writes the synthetic models (scratch-fd/models/NAME/model.stan, data.json), then
evaluates every model in mpmath at the three corpus evaluation points (the
point tools/stanli_check calls --point 0, 1, 2) and stores the values as
70-digit strings. Gradients are mpmath central differences of the log density
with respect to the unconstrained parameters, with Jacobians, as stanli_check
prints them. Terms Stan's `~` drops (those with no parameter) are dropped.
compare runs each arm's stanli_check at those points and prints the error
against the truth in ULP of the truth, per model and per arm.
"""
import argparse
import json
import math
import pathlib
import random
import statistics
import subprocess
import sys
import zipfile

from mpmath import diff, exp, log, log1p, loggamma, mp, mpf

mp.dps = 70
REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools"))
import verify_refs as V  # noqa: E402
from corpus_inventory import corpus_cases  # noqa: E402

MODELS_DIR = REPO / "scratch-fd" / "models"
PDB = REPO / "deps" / "posteriordb" / "posterior_database"


def eval_point(i, variant):
    if variant == 1:
        return 0.02 * ((i % 5) - 2)
    if variant == 2:
        return 0.0
    return 0.1 + 0.05 * (i % 7) - 0.15 * (i % 3)


def corpus_data(name):
    case = corpus_cases(PDB, include_language=True)[name]
    if case.data.suffix == ".zip":
        with zipfile.ZipFile(case.data) as z:
            return json.loads(z.read(z.namelist()[0])), case.source, case.data
    return json.loads(case.data.read_text()), case.source, case.data


def cauchy_prior(sigma, scale):
    return -log1p((sigma / mpf(scale)) ** 2)


def normal_prior(x, sd):
    return -(x / mpf(sd)) ** 2 / 2


def linear_normal(y, cols, prior, full=False):
    n = len(y)
    k = len(cols)
    yv = [mpf(v) for v in y]
    cv = [[mpf(v) for v in c] for c in cols]

    def lp(u):
        beta, ls = u[:k], u[k]
        sigma = exp(ls)
        acc = mpf(0)
        for i in range(n):
            mu = sum(beta[j] * cv[j][i] for j in range(k))
            acc += ((yv[i] - mu) / sigma) ** 2
        const = -n * log(2 * mp.pi) / 2 if full else 0
        return -acc / 2 - n * ls + const + prior(beta, sigma) + ls
    return lp, k + 1


def corpus_models():
    out = {}

    def kid(extra):
        def build(d):
            cols = [[1.0] * d["N"], d["mom_iq"]] if extra == "iq" else None
            if extra == "hsiq":
                cols = [[1.0] * d["N"], d["mom_hs"], d["mom_iq"]]
            if extra == "inter":
                cols = [[1.0] * d["N"], d["mom_hs"], d["mom_iq"],
                        [a * b for a, b in zip(d["mom_hs"], d["mom_iq"])]]
            return linear_normal(d["kid_score"], cols, lambda b, s: cauchy_prior(s, "2.5"))
        return build

    out["kidscore_momiq"] = kid("iq")
    out["kidscore_momhsiq"] = kid("hsiq")
    out["kidscore_interaction"] = kid("inter")
    out["logearn_height"] = lambda d: linear_normal(
        [math.log(e) for e in d["earn"]], [[1.0] * d["N"], d["height"]], lambda b, s: 0)
    out["logearn_interaction"] = lambda d: linear_normal(
        [math.log(e) for e in d["earn"]],
        [[1.0] * d["N"], d["height"], d["male"], [a * b for a, b in zip(d["height"], d["male"])]],
        lambda b, s: 0)
    out["earn_height"] = lambda d: linear_normal(
        d["earn"], [[1.0] * d["N"], d["height"]], lambda b, s: 0)

    def nes(d):
        age = d["age_discrete"]
        cols = [[1.0] * d["N"], d["real_ideo"], d["race_adj"],
                [float(a == 2) for a in age], [float(a == 3) for a in age],
                [float(a == 4) for a in age], d["educ1"], d["gender"], d["income"]]
        return linear_normal(d["partyid7"], cols, lambda b, s: 0)
    out["nes"] = nes

    def radon_pooled(d):
        return linear_normal(
            d["log_radon"], [[1.0] * d["N"], d["floor_measure"]],
            lambda b, s: normal_prior(s, 1) + normal_prior(b[0], 10) + normal_prior(b[1], 10),
            full=True)
    out["radon_pooled"] = radon_pooled
    return out


def ch16_m16_1(d):
    h = [mpf(v) for v in d["h"]]
    w = [mpf(v) for v in d["w"]]
    lw = [log(v) for v in w]
    n = len(w)

    def lp(u):
        up, uk, us = u
        p = 1 / (1 + exp(-up))
        k, s = exp(uk), exp(us)
        t = -s
        t += -k / 2
        t += (2 - 1) * log(p) + (18 - 1) * log(1 - p)
        acc = mpf(0)
        for i in range(n):
            mu = log(mpf(3.141593) * k * p ** 2 * h[i] ** 3)
            acc += ((lw[i] - mu) / s) ** 2
        t += -acc / 2 - n * us - n * log(2 * mp.pi) / 2
        t += log(p) + log(1 - p) + uk + us
        return t
    return lp, 3


def student_t_full(y, nu, m, s):
    nu, m, s = mpf(nu), mpf(m), mpf(s)
    z = (y - m) / s
    return (loggamma((nu + 1) / 2) - loggamma(nu / 2) - log(nu) / 2 - log(mp.pi) / 2
            - log(s) - (nu + 1) / 2 * log1p(z * z / nu))


def s2_s_by(d):
    n = d["N"]
    Y = [mpf(v) for v in d["Y"]]
    Xs = [[mpf(v) for v in row] for row in d["Xs"]]
    Z = [[[mpf(v) for v in row] for row in d[f"Zs_{k}_1"]] for k in range(1, 6)]
    kn = [d[f"knots_{k}"][0] for k in range(1, 6)]
    ks = d["Ks"]

    def lp(u):
        pos = 0
        ic = u[pos]
        pos += 1
        bs = u[pos:pos + ks]
        pos += ks
        zs, sds = [], []
        for k in range(5):
            zs.append(u[pos:pos + kn[k]])
            pos += kn[k]
            sds.append(exp(u[pos]))
            pos += 1
        ls = u[pos]
        sigma = exp(ls)
        s = [[sds[k] * z for z in zs[k]] for k in range(5)]
        t = student_t_full(ic, 3, "-0.3", "2.5")
        for k in range(5):
            t += student_t_full(sds[k], 3, 0, "2.5") + log(2)
        t += student_t_full(sigma, 3, 0, "2.5") + log(2)
        for k in range(5):
            t += -sum(z * z for z in zs[k]) / 2 - len(zs[k]) * log(2 * mp.pi) / 2
        acc = mpf(0)
        for i in range(n):
            mu = ic + sum(Xs[i][j] * bs[j] for j in range(ks))
            for k in range(5):
                mu += sum(Z[k][i][j] * s[k][j] for j in range(kn[k]))
            acc += ((Y[i] - mu) / sigma) ** 2
        t += -acc / 2 - n * ls - n * log(2 * mp.pi) / 2
        t += sum(log(x) for x in sds) + ls
        return t
    return lp, 1 + ks + sum(kn) + 5 + 1


SYNTH = {}


def synth(name, stan, make_data, build):
    SYNTH[name] = (stan, make_data, build)


def rng_data(seed):
    return random.Random(seed)


def student_data(seed, n):
    r = rng_data(seed)
    x = [r.uniform(-2, 2) for _ in range(n)]
    y = [0.3 + 0.5 * xi + 0.8 * r.gauss(0, 1) / math.sqrt(r.gammavariate(2.5, 1 / 2.5)) for xi in x]
    return dict(N=n, x=x, y=y)


def lognormal_data(seed, n):
    r = rng_data(seed)
    x = [r.uniform(-2, 2) for _ in range(n)]
    y = [math.exp(0.3 + 0.5 * xi + 0.4 * r.gauss(0, 1)) for xi in x]
    return dict(N=n, x=x, y=y)


def gamma_data(seed, n):
    r = rng_data(seed)
    x = [r.uniform(-2, 2) for _ in range(n)]
    y = [r.gammavariate(3.0, math.exp(0.2 + 0.4 * xi) / 3.0) for xi in x]
    return dict(N=n, x=x, y=y)


def beta_data(seed, n):
    r = rng_data(seed)
    x = [r.uniform(-2, 2) for _ in range(n)]
    y = []
    for xi in x:
        m = 1 / (1 + math.exp(-(0.2 + 0.6 * xi)))
        y.append(min(max(r.betavariate(m * 8, (1 - m) * 8), 1e-9), 1 - 1e-9))
    return dict(N=n, x=x, y=y)


def cauchy_data(seed, n):
    r = rng_data(seed)
    x = [r.uniform(-2, 2) for _ in range(n)]
    y = [0.3 + 0.5 * xi + 0.7 * math.tan(math.pi * (r.random() - 0.5)) for xi in x]
    return dict(N=n, x=x, y=y)


def normal_data(seed, n, offset=0.0):
    r = rng_data(seed)
    x = [r.uniform(-2, 2) for _ in range(n)]
    y = [offset + 0.3 + 0.5 * xi + r.gauss(0, 1) for xi in x]
    return dict(N=n, x=x, y=y)


def cancel_data(seed, n):
    r = rng_data(seed)
    x = [r.uniform(-2, 2) for _ in range(n)]
    e = [mpf(r.gauss(0, 1)) for _ in range(n)]
    mx = sum(mpf(v) for v in x) / n
    sxx = sum((mpf(v) - mx) ** 2 for v in x)
    me = sum(e) / n
    slope = sum((mpf(xv) - mx) * (ev - me) for xv, ev in zip(x, e)) / sxx
    e = [ev - me - slope * (mpf(xv) - mx) for xv, ev in zip(x, e)]
    y = [0.1 + float(ev) for ev in e]
    return dict(N=n, x=x, y=y)


STAN_STUDENT = """data { int<lower=0> N; vector[N] x; vector[N] y; }
parameters { real a; real b; real<lower=0> sigma; real<lower=1> nu; }
model {
  a ~ normal(0, 10);
  b ~ normal(0, 10);
  sigma ~ cauchy(0, 2.5);
  nu ~ gamma(2, 0.1);
  y ~ student_t(nu, a + b * x, sigma);
}
"""
STAN_LOGNORMAL = """data { int<lower=0> N; vector[N] x; vector<lower=0>[N] y; }
parameters { real a; real b; real<lower=0> sigma; }
model {
  a ~ normal(0, 5);
  b ~ normal(0, 5);
  sigma ~ exponential(1);
  y ~ lognormal(a + b * x, sigma);
}
"""
STAN_GAMMA = """data { int<lower=0> N; vector[N] x; vector<lower=0>[N] y; }
parameters { real a; real b; real<lower=0> shape; }
model {
  a ~ normal(0, 5);
  b ~ normal(0, 2);
  shape ~ gamma(0.01, 0.01);
  y ~ gamma(shape, shape ./ exp(a + b * x));
}
"""
STAN_BETA = """data { int<lower=0> N; vector[N] x; vector<lower=0, upper=1>[N] y; }
parameters { real a; real b; real<lower=0> phi; }
model {
  vector[N] mu = inv_logit(a + b * x);
  a ~ normal(0, 2.5);
  b ~ normal(0, 2.5);
  phi ~ gamma(0.01, 0.01);
  y ~ beta(mu * phi, (1 - mu) * phi);
}
"""
STAN_CAUCHY = """data { int<lower=0> N; vector[N] x; vector[N] y; }
parameters { real a; real b; real<lower=0> sigma; }
model {
  a ~ normal(0, 10);
  b ~ normal(0, 10);
  sigma ~ normal(0, 5);
  y ~ cauchy(a + b * x, sigma);
}
"""
STAN_NORMAL = """data { int<lower=0> N; vector[N] x; vector[N] y; }
parameters { real a; real b; real<lower=0> sigma; }
model {
  a ~ normal(0, 10);
  b ~ normal(0, 10);
  sigma ~ normal(0, 5);
  y ~ normal(a + b * x, sigma);
}
"""


def build_student(d):
    x = [mpf(v) for v in d["x"]]
    y = [mpf(v) for v in d["y"]]
    n = len(y)

    def lp(u):
        a, b, ls, un = u
        sigma, nu = exp(ls), 1 + exp(un)
        t = normal_prior(a, 10) + normal_prior(b, 10) + cauchy_prior(sigma, "2.5")
        t += (2 - 1) * log(nu) - mpf("0.1") * nu
        c = loggamma((nu + 1) / 2) - loggamma(nu / 2) - log(nu) / 2
        acc = mpf(0)
        for i in range(n):
            z = (y[i] - (a + b * x[i])) / sigma
            acc += (nu + 1) / 2 * log1p(z * z / nu)
        t += n * (c - ls) - acc
        return t + ls + un
    return lp, 4


def build_lognormal(d):
    x = [mpf(v) for v in d["x"]]
    ly = [log(mpf(v)) for v in d["y"]]
    n = len(ly)

    def lp(u):
        a, b, ls = u
        sigma = exp(ls)
        t = normal_prior(a, 5) + normal_prior(b, 5) - sigma
        acc = mpf(0)
        for i in range(n):
            acc += ((ly[i] - (a + b * x[i])) / sigma) ** 2
        t += -acc / 2 - n * ls - n * log(2 * mp.pi) / 2
        return t + ls
    return lp, 3


def build_gamma(d):
    x = [mpf(v) for v in d["x"]]
    y = [mpf(v) for v in d["y"]]
    ly = [log(v) for v in y]
    n = len(y)

    def lp(u):
        a, b, ls = u
        shape = exp(ls)
        t = normal_prior(a, 5) + normal_prior(b, 2)
        t += (mpf("0.01") - 1) * ls - mpf("0.01") * shape
        acc = mpf(0)
        for i in range(n):
            eta = a + b * x[i]
            beta = shape / exp(eta)
            acc += shape * log(beta) + (shape - 1) * ly[i] - beta * y[i]
        t += acc - n * loggamma(shape)
        return t + ls
    return lp, 3


def build_beta(d):
    x = [mpf(v) for v in d["x"]]
    y = [mpf(v) for v in d["y"]]
    ly = [log(v) for v in y]
    l1y = [log(1 - v) for v in y]
    n = len(y)

    def lp(u):
        a, b, ls = u
        phi = exp(ls)
        t = normal_prior(a, 2.5) + normal_prior(b, 2.5)
        t += (mpf("0.01") - 1) * ls - mpf("0.01") * phi
        acc = mpf(0)
        for i in range(n):
            mu = 1 / (1 + exp(-(a + b * x[i])))
            al, be = mu * phi, (1 - mu) * phi
            acc += (-loggamma(al) - loggamma(be) + (al - 1) * ly[i]
                    + (be - 1) * l1y[i] + loggamma(al + be))
        return t + acc + ls
    return lp, 3


def build_cauchy(d):
    x = [mpf(v) for v in d["x"]]
    y = [mpf(v) for v in d["y"]]
    n = len(y)

    def lp(u):
        a, b, ls = u
        sigma = exp(ls)
        t = normal_prior(a, 10) + normal_prior(b, 10) + normal_prior(sigma, 5)
        acc = mpf(0)
        for i in range(n):
            acc += log1p(((y[i] - (a + b * x[i])) / sigma) ** 2)
        return t - acc - n * ls + ls
    return lp, 3


def build_normal_params(d):
    x = [mpf(v) for v in d["x"]]
    y = [mpf(v) for v in d["y"]]
    n = len(y)

    def lp(u):
        a, b, ls = u
        sigma = exp(ls)
        t = normal_prior(a, 10) + normal_prior(b, 10) + normal_prior(sigma, 5)
        acc = mpf(0)
        for i in range(n):
            acc += ((y[i] - (a + b * x[i])) / sigma) ** 2
        return t - acc / 2 - n * ls + ls
    return lp, 3


for nm, st, dg, bd in (
        ("syn_student_t_1000", STAN_STUDENT, lambda: student_data(11, 1000), build_student),
        ("syn_student_t_300", STAN_STUDENT, lambda: student_data(12, 300), build_student),
        ("syn_lognormal_1000", STAN_LOGNORMAL, lambda: lognormal_data(21, 1000), build_lognormal),
        ("syn_lognormal_300", STAN_LOGNORMAL, lambda: lognormal_data(22, 300), build_lognormal),
        ("syn_gamma_800", STAN_GAMMA, lambda: gamma_data(31, 800), build_gamma),
        ("syn_beta_800", STAN_BETA, lambda: beta_data(41, 800), build_beta),
        ("syn_cauchy_1000", STAN_CAUCHY, lambda: cauchy_data(51, 1000), build_cauchy),
        ("syn_normal_100000", STAN_NORMAL, lambda: normal_data(61, 100000), build_normal_params),
        ("syn_normal_offset_5000", STAN_NORMAL, lambda: normal_data(62, 5000, 1e4), build_normal_params),
        ("syn_normal_cancel_2000", STAN_NORMAL, lambda: cancel_data(71, 2000), build_normal_params)):
    synth(nm, st, dg, bd)


def model_inputs(name):
    if name in SYNTH:
        d = MODELS_DIR / name
        return d / "model.stan", d / "data.json"
    data, source, data_path = corpus_data(name)
    d = MODELS_DIR / name
    d.mkdir(parents=True, exist_ok=True)
    (d / "data.json").write_text(json.dumps(data))
    return source, d / "data.json"


def load_model(name):
    if name in SYNTH:
        stan, make, build = SYNTH[name]
        d = MODELS_DIR / name
        d.mkdir(parents=True, exist_ok=True)
        if not (d / "data.json").exists():
            data = make()
            (d / "data.json").write_text(json.dumps(data))
            (d / "model.stan").write_text(stan)
        return build(json.loads((d / "data.json").read_text()))
    data, _, _ = corpus_data(name)
    if name == "ch16_m16_1":
        return ch16_m16_1(data)
    if name == "s2_s_by":
        return s2_s_by(json.loads((REPO / "tests" / "brms" / "s2_s_by.json").read_text()))
    return corpus_models()[name](data)


def all_names():
    return (list(corpus_models()) + ["ch16_m16_1", "s2_s_by"] + list(SYNTH))


def hp(lp, n, point):
    u = [mpf(eval_point(i, point)) for i in range(n)]
    val = lp(u)
    grad = []
    for i in range(n):
        order = [0] * n
        order[i] = 1
        grad.append(diff(lambda *a: lp(list(a)), u, tuple(order)))
    return [val] + grad


def cmd_truth(args):
    names = args.models.split(",") if args.models else all_names()
    out = json.loads(args.out.read_text()) if args.out.exists() else {}
    for name in names:
        if name in out:
            continue
        lp, n = load_model(name)
        model_inputs(name)
        out[name] = {str(p): [mp.nstr(v, 70) for v in hp(lp, n, p)] for p in V.POINTS}
        args.out.write_text(json.dumps(out, indent=1))
        print("truth", name, flush=True)


def ulp_of(f):
    return abs(math.nextafter(f, math.inf) - f)


def run_arm(binary, env, stan, data, point):
    cmd = ["/usr/bin/env"] + env + [str(binary), str(stan), str(data), "--point", str(point)]
    proc = subprocess.run(cmd, capture_output=True, text=True, cwd=REPO, timeout=600)
    fields = V.parse_status(proc.stdout)
    if not fields or fields[0] != "OK":
        raise RuntimeError(proc.stdout[-300:] + proc.stderr[-300:])
    return [float(x) for x in fields[1:]]


def cmd_compare(args):
    truth = json.loads(pathlib.Path(args.truth).read_text())
    arms = {}
    for spec in args.arm:
        name, rest = spec.split("=", 1)
        parts = rest.split(",")
        arms[name] = (parts[0], parts[1:])
    refs, _ = V.load_refs()
    rows = []
    for model, pts in truth.items():
        stan, data = model_inputs(model)
        for point, hs in pts.items():
            h = [mpf(s) for s in hs]
            hf = [float(v) for v in h]
            scale = max(abs(v) for v in hf[1:]) or 1.0
            row = dict(model=model, point=int(point), truth=hs, arms={})
            for name, (binary, env) in arms.items():
                vals = run_arm(binary, env, stan, data, point)
                errs = [float(abs(mpf(v) - hh) / ulp_of(f)) if ulp_of(f) else 0.0
                        for v, hh, f in zip(vals, h, hf)]
                big = [e for e, f in zip(errs[1:], hf[1:]) if abs(f) >= 1e-6 * scale]
                abserr = [float(abs(mpf(v) - hh)) for v, hh in zip(vals, h)]
                row["arms"][name] = dict(values=vals, ulp=errs, lp_ulp=errs[0],
                                         grad_ulp=max(big),
                                         scaled=max(abserr[1:]) / scale,
                                         lp_rel=abserr[0] / max(abs(hf[0]), 1e-300))
            ref = refs.get(model)
            if ref and str(point) in ref["points"] and "values" in ref["points"][str(point)]:
                rv = [float(x) for x in ref["points"][str(point)]["values"]]
                row["cmdstan_ulp"] = max(float(abs(mpf(v) - hh) / ulp_of(f)) if ulp_of(f) else 0.0
                                         for v, hh, f in zip(rv, h, hf))
            rows.append(row)
    pathlib.Path(args.out).write_text(json.dumps(rows, indent=1))
    names = list(arms)
    print("model\tpt\tcmdstan\t" + "\t".join(names))
    for r in rows:
        print(f"{r['model']}\t{r['point']}\t{r.get('cmdstan_ulp', float('nan')):.1f}\t"
              + "\t".join(f"{r['arms'][n]['lp_ulp']:.1f}/{r['arms'][n]['grad_ulp']:.1f}" for n in names))
    print("\nper arm over all model-points: max grad ULP (components above 1e-6 of the largest), "
          "median grad ULP, max lp ULP, max scaled grad error, geomean of scaled grad error "
          "ratio to exact, geomean of lp error ratio, model-points with scaled grad error "
          "below 0.8x / above 1.25x of exact")
    ex = args.exact
    for n in names:
        g = [r["arms"][n]["grad_ulp"] for r in rows]
        l = [r["arms"][n]["lp_ulp"] for r in rows]
        s = [r["arms"][n]["scaled"] for r in rows]

        def gmean(key):
            ratios = [(r["arms"][n][key] + 1e-300) / (r["arms"][ex][key] + 1e-300)
                      for r in rows if r["arms"][ex][key] > 0 and r["arms"][n][key] > 0]
            return (math.exp(statistics.mean(math.log(x) for x in ratios))
                    if ratios else float("nan"))
        better = sum(1 for r in rows if r["arms"][n]["scaled"] < 0.8 * r["arms"][ex]["scaled"])
        worse = sum(1 for r in rows if r["arms"][n]["scaled"] > 1.25 * r["arms"][ex]["scaled"])
        print(f"{n}\t{max(g):.1f}\t{statistics.median(g):.2f}\t{max(l):.1f}\t{max(s):.2e}\t"
              f"{gmean('scaled'):.3f}\t{gmean('lp_rel'):.3f}\t{better}/{worse}")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    sub = ap.add_subparsers(dest="cmd", required=True)
    t = sub.add_parser("truth")
    t.add_argument("out", type=pathlib.Path)
    t.add_argument("--models", default="")
    c = sub.add_parser("compare")
    c.add_argument("truth")
    c.add_argument("--arm", action="append", required=True)
    c.add_argument("--exact", default="A")
    c.add_argument("--out", default="hp_compare.json")
    args = ap.parse_args()
    (cmd_truth if args.cmd == "truth" else cmd_compare)(args)


if __name__ == "__main__":
    main()
