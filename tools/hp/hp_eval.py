#!/usr/bin/env python3
"""High-precision lp and gradient of a corpus model at the three recorded points.

  tools/hp/hp_eval.py MODEL [MODEL ...] [--dps 80] [--points 0,1,2] [--no-grad]
                      [--timeout S] [--out DIR]

Prints, per point, the scaled deviation of the CmdStan reference, stanli default
and stanli --fast-math from the high-precision values.
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
    p = subprocess.run(cmd, capture_output=True, text=True, cwd=REPO, timeout=600)
    got = v.parse_status(p.stdout)
    if not got or got[0] != "OK":
        return None
    return [float(x) for x in got[1:]]


def hp_float(vals):
    return [float(x) for x in vals]


def run_model(model, args, refs, tmp):
    res = {"model": model}
    stan, dj, ref = load_case(model, tmp, refs)
    t0 = time.time()
    mir = compile_mir(stan)
    data = json.loads(pathlib.Path(dj).read_text())
    mp.dps = args.dps
    it = Interp(mir, data)
    n = it.nparams()
    res["n"] = n
    res["prep_s"] = round(time.time() - t0, 3)
    res["points"] = {}
    for pt in args.points:
        t1 = time.time()
        theta = [mpf(point_value(i, pt)) for i in range(n)]
        lp = it.logp(theta)
        t2 = time.time()
        grad = None if args.no_grad else it.grad_fd(theta)
        out = {"lp_s": round(t2 - t1, 3), "grad_s": round(time.time() - t2, 3),
               "lp": mp.nstr(lp, 55)}
        if grad is not None:
            out["grad"] = [mp.nstr(g, 55) for g in grad]
        hv = [float(lp)] + ([float(g) for g in grad] if grad is not None else [])
        rp = ref["points"][str(pt)]
        if "values" in rp:
            rv = [float(x) for x in rp["values"]]
            out["dev_cmdstan"] = v.fast_dev(hv, rv)[0] if grad is not None else v.pair_dev(hv[0], rv[0])[0]
        for label, fast in (() if args.skip_stanli else (("dev_default", False), ("dev_fast", True))):
            got = stanli(stan, dj, pt, fast)
            if got is not None:
                out[label] = v.fast_dev(hv, got)[0] if grad is not None else v.pair_dev(hv[0], got[0])[0]
        res["points"][str(pt)] = out
    res["total_s"] = round(time.time() - t0, 3)
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("models", nargs="+")
    ap.add_argument("--dps", type=int, default=80)
    ap.add_argument("--points", default="0,1,2")
    ap.add_argument("--no-grad", action="store_true")
    ap.add_argument("--timeout", type=int, default=600)
    ap.add_argument("--out")
    ap.add_argument("-v", action="store_true")
    ap.add_argument("--skip-stanli", action="store_true")
    args = ap.parse_args()
    args.points = [int(x) for x in args.points.split(",")]
    refs, _ = v.replay_refs(v.native_platform(), "clang")
    signal.signal(signal.SIGALRM, alarm)
    tmp = tempfile.mkdtemp()
    for m in args.models:
        signal.alarm(args.timeout)
        try:
            r = run_model(m, args, refs, tmp)
        except Timeout:
            r = {"model": m, "error": "timeout"}
        except Exception as e:
            r = {"model": m, "error": "%s: %s" % (type(e).__name__, str(e)[:200])}
            if "-v" in sys.argv:
                traceback.print_exc()
        signal.alarm(0)
        if args.out:
            p = pathlib.Path(args.out)
            p.mkdir(parents=True, exist_ok=True)
            (p / (m + ".json")).write_text(json.dumps(r))
        if "error" in r:
            print(m, "ERROR", r["error"])
            continue
        for pt, o in r["points"].items():
            print("%-28s n=%-4d pt%s lp_s=%.2f grad_s=%.2f cmdstan=%.2e default=%.2e fast=%.2e" % (
                m, r["n"], pt, o["lp_s"], o["grad_s"], o.get("dev_cmdstan", -1),
                o.get("dev_default", -1), o.get("dev_fast", -1)))
        sys.stdout.flush()


if __name__ == "__main__":
    main()
