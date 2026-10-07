#!/usr/bin/env python3
"""Replay the recorded corpus points through several stanli_check arms.

python3 harnesses/fd_replay.py PDB OUT.jsonl --arm A=BIN --arm C4=BIN,STANLI_FAST_REDUCE=1 ...
    [--exact A] [--jobs 8] [--models FILE]

An arm is NAME=BIN, optionally followed by ,VAR=VALUE environment entries. Every
model is run at all recorded points with --wa-values by every arm. One record
per (model, point) is appended to OUT.jsonl with, per arm, the status, the
density and gradient, a hash of the whole stdout, and the deviation from the
CmdStan reference (scaled error and ULP) and from the exact arm.
"""
import argparse
import concurrent.futures
import hashlib
import json
import pathlib
import subprocess
import sys
import tempfile

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools"))
import verify_refs as V  # noqa: E402
from corpus_inventory import corpus_cases  # noqa: E402


def run_arm(binary, env, stan, data, point, timeout):
    cmd = ["/usr/bin/env"] + env + [str(binary), str(stan), str(data), "--point",
                                    str(point), "--wa-values"]
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, cwd=REPO, timeout=timeout)
    except subprocess.TimeoutExpired:
        return dict(status="TIMEOUT", values=[], hash="", wa=[])
    fields = V.parse_status(proc.stdout)
    kind = fields[0] if fields else "NONE"
    values = [float(x) for x in fields[1:]] if kind == "OK" else []
    wa = V.parse_wa(proc.stdout)
    digest = hashlib.sha256(proc.stdout.encode()).hexdigest()[:16]
    return dict(status=kind, values=values, hash=digest,
                wa=[float(x) for x in wa[1]] if wa else [])


def dev(a, b):
    if len(a) != len(b):
        return None
    rel, ulp = 0.0, 0
    for x, y in zip(a, b):
        r, u = V.pair_dev(x, y)
        rel, ulp = max(rel, r), max(ulp, u)
    return rel, ulp


def one(model, ref, pdb, tmp, arms, exact, timeout, target):
    stan, data = V.model_files(model, ref, pdb, tmp)
    records = []
    ulp_limit = V.ulp_limit_for(model, ref, target)
    for point in V.POINTS:
        pt = ref["points"].get(str(point))
        rec = dict(model=model, point=point, arms={},
                   ref_status=None if pt is None else pt.get("status"),
                   ulp_limit=ulp_limit,
                   gate=None if pt is None else V.gate_for(model, pt, 1e-9),
                   ill=model in V.ILL_CONDITIONED)
        for name, (binary, env) in arms.items():
            rec["arms"][name] = run_arm(binary, env, stan, data, point, timeout)
        ex = rec["arms"][exact]
        refvals = [float(x) for x in pt["values"]] if pt and "values" in pt else None
        for name, a in rec["arms"].items():
            a["same_bytes"] = a["hash"] == ex["hash"]
            a["vs_exact"] = dev(a["values"], ex["values"]) if a["status"] == "OK" and ex["status"] == "OK" else None
            a["vs_exact_wa"] = dev(a["wa"], ex["wa"]) if a["wa"] and ex["wa"] else None
            a["vs_cmdstan"] = (dev(a["values"], refvals)
                               if refvals is not None and a["status"] == "OK" else None)
            a["vs_cmdstan_wa"] = None
            if pt and "wa" in pt and a["wa"]:
                a["vs_cmdstan_wa"] = dev(a["wa"], [float(x) for x in pt["wa"]["values"]])
        records.append(rec)
    return records


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("pdb", type=pathlib.Path)
    ap.add_argument("out", type=pathlib.Path)
    ap.add_argument("--arm", action="append", required=True)
    ap.add_argument("--exact", default="A")
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--timeout", type=float, default=300)
    ap.add_argument("--models", default="")
    a = ap.parse_args()
    arms = {}
    for spec in a.arm:
        name, rest = spec.split("=", 1)
        parts = rest.split(",")
        arms[name] = (pathlib.Path(parts[0]).resolve(), parts[1:])
    pdb = a.pdb / "posterior_database"
    target = V.native_platform()
    compiler = V.runtime_compiler(arms[a.exact][0])
    refs, _ = V.replay_refs(target, compiler)
    cases = corpus_cases(pdb, include_language=True)
    local = {n for n, c in cases.items() if c.collection != "posteriordb"}
    models = sorted(set(refs) | local)
    if a.models:
        want = set(pathlib.Path(a.models).read_text().split())
        models = [m for m in models if m in want]
    done = set()
    if a.out.exists():
        done = {json.loads(line)["model"] for line in a.out.open()}
    models = [m for m in models if m not in done and m in refs]
    tmp = pathlib.Path(tempfile.mkdtemp(prefix="fd_replay_"))
    with concurrent.futures.ThreadPoolExecutor(a.jobs) as pool, a.out.open("a") as out:
        futs = {pool.submit(one, m, refs[m], pdb, tmp, arms, a.exact, a.timeout, target): m
                for m in models}
        for n, fut in enumerate(concurrent.futures.as_completed(futs), 1):
            for rec in fut.result():
                out.write(json.dumps(rec) + "\n")
            out.flush()
            if n % 25 == 0:
                print(f"{n}/{len(models)}", flush=True)


if __name__ == "__main__":
    main()
