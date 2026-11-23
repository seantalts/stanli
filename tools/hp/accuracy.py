#!/usr/bin/env python3
"""Measure how far CmdStan and stanli each are from the 80-digit value, for
every value of every corpus model, and record the result.

  tools/hp/accuracy.py HP_DIR [--check BIN] [--only MODEL ...] [--jobs N]
      [--out docs/internal/artifacts/accuracy.json]

HP_DIR holds hp_eval.py results made with --wa (survey.py writes them). For
each model the script runs stanli_check at the three recorded points. Every
log density, gradient entry and write_array value that has an 80-digit value
is scored for both engines: |value - true value| in ULPs of the scale that
the replay gate uses for it (verify_refs.scaled_ulp). The record keeps, for
each model and output kind, the number of values scored, the mean and the
largest distance for each engine, and the values where stanli is more than
BUG_ULP from the true value and farther from it than CmdStan. A value that
both engines miss by more than UNDECIDED_ULP is counted in the means and left
out of that list.

tools/gen_docs.py writes the columns of TESTING.md's table from the record.
"""
import argparse
import concurrent.futures
import datetime
import json
import math
import pathlib
import subprocess
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import metrics  # noqa: E402
from common import REPO, load_case, v  # noqa: E402
from ledger_accuracy import run, truth, wa_ref, wa_truth  # noqa: E402

OUT = REPO / "docs" / "internal" / "artifacts" / "accuracy.json"
SCHEMA = 1
BUG_ULP = 10
# A value that both engines miss by more than this is one that double
# arithmetic cannot compute at that point; which engine is closer says
# nothing about either.
UNDECIDED_ULP = 1e6
WORSE_KEPT = 12
KINDS = ("lp", "grad", "wa")


class Tally:
    def __init__(self):
        self.pairs = []
        self.worse = []

    def add(self, label, point, cmdstan, stanli):
        self.pairs.append((cmdstan, stanli))
        if (stanli > BUG_ULP and stanli > cmdstan
                and min(cmdstan, stanli) <= UNDECIDED_ULP):
            self.worse.append({"value": label, "point": point,
                               "cmdstan": round(cmdstan, 1),
                               "stanli": round(stanli, 1)})

    def record(self):
        if not self.pairs:
            return None
        finite = [(c, s) for c, s in self.pairs
                  if math.isfinite(c) and math.isfinite(s)]
        out = {"n": len(finite), "nonfinite": len(self.pairs) - len(finite)}
        if finite:
            for i, engine in enumerate(("cmdstan", "stanli")):
                errs = [pair[i] for pair in finite]
                out[engine] = {"mean": math.fsum(errs) / len(errs),
                               "max": max(errs)}
            out["stanli_closer"] = sum(s < c for c, s in finite)
            out["cmdstan_closer"] = sum(c < s for c, s in finite)
        if self.worse:
            self.worse.sort(key=lambda w: -w["stanli"])
            out["worse_count"] = len(self.worse)
            out["worse"] = self.worse[:WORSE_KEPT]
        return out


def score_model(model, hp_dir, check, refs, tmp):
    """{kind: record or {"unscored": why}} for one model."""
    path = pathlib.Path(hp_dir) / (model + ".json")
    if not path.exists():
        return {kind: {"unscored": "no 80-digit run"} for kind in KINDS}
    hp = json.loads(path.read_text())
    if "points" not in hp:
        why = "80-digit run: " + str(hp.get("error", "failed"))[:80]
        return {kind: {"unscored": why} for kind in KINDS}
    stan, dj, ref = load_case(model, tmp, refs)
    tally = {kind: Tally() for kind in KINDS}
    why = {}
    for ps, rp in sorted(ref["points"].items()):
        if "values" not in rp:
            continue
        point = int(ps)
        h = hp["points"].get(ps)
        if not h or "lp" not in h:
            why.setdefault("lp", "no 80-digit value")
            continue
        got = run(check, stan, dj, point, {}, False)
        if got is None:
            return {kind: {"unscored": "stanli_check failed"} for kind in KINDS}
        rv = [float(x) for x in rp["values"]]
        gv, gw = got[:-1], got[-1]
        if len(gv) != len(rv):
            return {kind: {"unscored": "shape differs"} for kind in KINDS}
        t = truth(h, 0)
        if t is not None and rv[0] != 0 and math.isfinite(rv[0]):
            tally["lp"].add("lp", point,
                            metrics.ulps_from_truth(rv[0], t, abs(rv[0])),
                            metrics.ulps_from_truth(gv[0], t, abs(rv[0])))
        scale = max([abs(x) for x in rv[1:] if math.isfinite(x)] or [0.0])
        if h.get("method") != "fd" or "grad" not in h:
            why.setdefault("grad", h.get("grad_skipped")
                           or "over 300 parameters: directional derivatives only")
        elif scale:
            for i in range(1, len(rv)):
                t = truth(h, i)
                if t is not None and math.isfinite(rv[i]):
                    tally["grad"].add(f"grad[{i}]", point,
                                      metrics.ulps_from_truth(rv[i], t, scale),
                                      metrics.ulps_from_truth(gv[i], t, scale))
        cw = wa_ref(rp)
        if not cw:
            why.setdefault("wa", "no CmdStan write_array reference")
        elif "wa" not in h:
            why.setdefault("wa", h.get("wa_error") or "no 80-digit write_array")
        elif gw:
            scales = metrics.wa_scales(list(cw), list(cw.values()))
            for column, a in cw.items():
                t = wa_truth(h, column)
                sc = scales.get(column.split(".")[0].split(":")[0], 0.0)
                if t is None or column not in gw or not sc or not math.isfinite(a):
                    continue
                tally["wa"].add("wa " + column, point,
                                metrics.ulps_from_truth(a, t, sc),
                                metrics.ulps_from_truth(gw[column], t, sc))
            if h.get("wa_refused") and not tally["wa"].pairs:
                why.setdefault("wa", "every output depends on an _rng call")
    out = {}
    for kind in KINDS:
        record = tally[kind].record()
        out[kind] = record or {"unscored": why.get(kind, "no value to score")}
    return out


def dumps(record):
    """The record as JSON with one line for each model."""
    line = lambda x: json.dumps(x, sort_keys=True, separators=(",", ":"))
    models = ",\n".join(f"  {json.dumps(m)}: {line(r)}"
                        for m, r in sorted(record["models"].items()))
    return ("{\n" + f' "schema": {record["schema"]},\n'
            + f' "recorded": {line(record["recorded"])},\n'
            + ' "models": {\n' + models + "\n }\n}\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("hp")
    ap.add_argument("--check", default=str(REPO / "build-rel" / "stanli_check"))
    ap.add_argument("--only", nargs="*", default=[])
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--out", type=pathlib.Path, default=OUT)
    a = ap.parse_args()
    refs, recorded = v.replay_refs(v.native_platform(), "clang")
    models = a.only or sorted(refs)
    tmp = tempfile.mkdtemp()

    def one(model):
        try:
            return model, score_model(model, a.hp, a.check, refs, tmp)
        except Exception as e:  # one model's failure is that model's record
            return model, {kind: {"unscored": f"scoring failed: {e}"[:120]}
                           for kind in KINDS}

    with concurrent.futures.ThreadPoolExecutor(a.jobs) as pool:
        scored = dict(pool.map(one, models))
    head = subprocess.run(["git", "rev-parse", "HEAD"], cwd=REPO,
                          capture_output=True, text=True).stdout.strip()
    record = {"schema": SCHEMA,
              "recorded": {"platform": v.native_platform(),
                           "date": datetime.date.today().isoformat(),
                           "stanli_source": head, "digits": 80,
                           "cmdstan": recorded.get("cmdstan_version")},
              "models": dict(sorted(scored.items()))}
    if a.only and a.out.exists():
        old = json.loads(a.out.read_text())
        old["models"].update(record["models"])
        record["models"] = dict(sorted(old["models"].items()))
    a.out.write_text(dumps(record))
    for kind in KINDS:
        done = sum("n" in m[kind] for m in scored.values())
        print(f"{kind}: {done}/{len(scored)} models scored")


if __name__ == "__main__":
    main()
