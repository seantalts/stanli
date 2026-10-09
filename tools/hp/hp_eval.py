#!/usr/bin/env python3
"""High-precision lp and gradient of corpus models, and the error of three arms.

  tools/hp/hp_eval.py MODEL [MODEL ...] [--dps 80] [--points 0,1,2] [--out DIR]
                      [--full-grad-max 300] [--dirs 4] [--timeout S] [--no-grad]
                      [--skip-stanli] [--dps-check] [-v]

For each recorded point the model is evaluated with mpmath at --dps digits.
The arms are the recorded CmdStan values, stanli default and stanli
--fast-math (build-rel/stanli_check). Up to --full-grad-max unconstrained
parameters the reference gradient is by central finite differences; above that
it is --dirs directional derivatives along seeded unit vectors, compared with
the arm's gradient projected on the same vectors.

Errors: log density |a-b| / max(|a|,|b|,1); full gradient max|g-r| / max(1,
max|g|, max|r|); directional |g.v - D| / max(1, |g_cmdstan|_2) with |v| = 1.
Per-model JSON goes to --out.
"""
import argparse
import json
import pathlib
import signal
import subprocess
import sys
import tempfile
import time
import traceback

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import metrics  # noqa: E402
from common import REPO, compile_mir, load_case, v  # noqa: E402
from mpmath import mp, mpf  # noqa: E402
from mpinterp import Interp  # noqa: E402


def point_value(i, variant):
    if variant == 1:
        return 0.02 * float((i % 5) - 2)
    if variant == 2:
        return 0.0
    return 0.1 + 0.05 * float(i % 7) - 0.15 * float(i % 3)


class Timeout(Exception):
    pass


def alarm(_s, _f):
    raise Timeout()


def stanli(stan, dj, point, fast):
    cmd = [str(REPO / "build-rel" / "stanli_check"), str(stan), str(dj), "--point", str(point)]
    if fast:
        cmd.append("--fast-math")
    p = subprocess.run(cmd, capture_output=True, text=True, cwd=REPO, timeout=900)
    got = v.parse_status(p.stdout)
    if not got or got[0] != "OK":
        return None
    vals = [float(x) for x in got[1:]]
    return vals if v.accepted(got) else None


def arm_errors(vals, hp_lp, hp_grad, dirs, scale):
    out = {}
    if vals is None:
        return {"rejected": True}
    out["lp"] = metrics.lp_error(vals[0], hp_lp)
    g = vals[1:]
    if hp_grad is not None:
        out["grad"] = metrics.grad_error(g, hp_grad)
    elif dirs is not None:
        out["grad"] = max(metrics.directional_error(g, d["v"], d["D"], scale) for d in dirs)
    return out


def hp_gradient(it, theta, h, model, pt, args, n, lp_s):
    full = n <= args.full_grad_max and lp_s * (2 * n + 1) * 3 < 0.5 * args.timeout
    if full:
        return "fd", it.grad_fd(theta), None
    dirs = []
    for k in range(args.dirs):
        vec = metrics.direction("%s:%d:%d" % (model, pt, k), n)
        tp = [t + h * mpf(c) for t, c in zip(theta, vec)]
        tm = [t - h * mpf(c) for t, c in zip(theta, vec)]
        D = (it.logp(tp) - it.logp(tm)) / (2 * h)
        dirs.append({"seed": "%s:%d:%d" % (model, pt, k), "v": vec, "D": float(D), "D_hp": mp.nstr(D, 55)})
    return "directional", None, dirs


def run_model(model, args, refs, tmp, res):
    stan, dj, ref = load_case(model, tmp, refs)
    t0 = time.time()
    mir = compile_mir(stan)
    data = json.loads(pathlib.Path(dj).read_text())
    mp.dps = args.dps
    it = Interp(mir, data)
    n = it.nparams()
    res["n"] = n
    res["dps"] = args.dps
    res["prep_s"] = round(time.time() - t0, 3)
    res["points"] = {}
    h = mpf(10) ** (-(args.dps // 3))
    for pt in args.points:
        rp = ref["points"][str(pt)]
        entry = {"status": rp["status"]}
        res["points"][str(pt)] = entry
        if "values" not in rp or rp["status"] == "REJECTED_BOTH":
            entry["skipped"] = "rejected by CmdStan"
            continue
        theta = [mpf(point_value(i, pt)) for i in range(n)]
        t1 = time.time()
        lp = it.logp(theta)
        lp_s = time.time() - t1
        entry["lp_s"] = round(lp_s, 3)
        entry["lp"] = mp.nstr(lp, 55)
        if args.dps_check and pt == args.points[0]:
            mp.dps = 2 * args.dps
            lp2 = Interp(mir, data).logp([mpf(point_value(i, pt)) for i in range(n)])
            mp.dps = args.dps
            entry["lp_dps_diff"] = float(abs(lp2 - lp) / max(abs(lp2), 1))
        hp_grad, dirs, method = None, None, None
        if not mp.isfinite(lp):
            entry["grad_skipped"] = "reference log density is not finite"
        elif not args.no_grad:
            t2 = time.time()
            method, g, dirs = hp_gradient(it, theta, h, model, pt, args, n, lp_s)
            entry["grad_s"] = round(time.time() - t2, 3)
            entry["method"] = method
            if g is not None:
                hp_grad = [float(x) for x in g]
                entry["grad"] = [mp.nstr(x, 55) for x in g]
            else:
                entry["dirs"] = [{k: d[k] for k in ("seed", "D_hp")} for d in dirs]
        hp_lp = float(lp)
        rv = [float(x) for x in rp["values"]]
        scale = metrics.l2(rv[1:])
        entry["grad_scale"] = scale
        entry["arms"] = {"cmdstan": arm_errors(rv, hp_lp, hp_grad, dirs, scale)}
        if not args.skip_stanli:
            for label, fast in (("default", False), ("fast", True)):
                entry["arms"][label] = arm_errors(stanli(stan, dj, pt, fast), hp_lp, hp_grad, dirs, scale)
    res["total_s"] = round(time.time() - t0, 3)


def write(res, out, m):
    if out:
        p = pathlib.Path(out)
        p.mkdir(parents=True, exist_ok=True)
        (p / (m + ".json")).write_text(json.dumps(res))


def fmt(x):
    return "%.2e" % x if isinstance(x, float) else "   -   "


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("models", nargs="+")
    ap.add_argument("--dps", type=int, default=80)
    ap.add_argument("--points", default="0,1,2")
    ap.add_argument("--no-grad", action="store_true")
    ap.add_argument("--full-grad-max", type=int, default=300)
    ap.add_argument("--dirs", type=int, default=4)
    ap.add_argument("--timeout", type=int, default=1500)
    ap.add_argument("--out")
    ap.add_argument("-v", action="store_true")
    ap.add_argument("--skip-stanli", action="store_true")
    ap.add_argument("--dps-check", action="store_true")
    args = ap.parse_args()
    args.points = [int(x) for x in args.points.split(",")]
    refs, _ = v.replay_refs(v.native_platform(), "clang")
    signal.signal(signal.SIGALRM, alarm)
    tmp = tempfile.mkdtemp()
    for m in args.models:
        res = {"model": m}
        signal.alarm(args.timeout)
        try:
            run_model(m, args, refs, tmp, res)
        except Timeout:
            res["error"] = "timeout"
        except Exception as e:
            res["error"] = "%s: %s" % (type(e).__name__, str(e)[:200])
            if args.v:
                traceback.print_exc()
        signal.alarm(0)
        write(res, args.out, m)
        if "error" in res:
            print(m, "ERROR", res["error"])
        for pt, o in res.get("points", {}).items():
            if "arms" not in o:
                print("%-28s pt%s %s" % (m, pt, o.get("skipped", "")))
                continue
            row = "%-28s n=%-5d pt%s %-11s" % (m, res["n"], pt, o.get("method", "-"))
            for a in ("cmdstan", "default", "fast"):
                e = o["arms"].get(a, {})
                row += "  %s lp %s grad %s" % (a[:4], fmt(e.get("lp")), fmt(e.get("grad")))
            print(row)
        sys.stdout.flush()


if __name__ == "__main__":
    main()
