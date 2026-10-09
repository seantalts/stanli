#!/usr/bin/env python3
"""THROWAWAY (spike/vector-libm). Build fast-math MIR + data for every corpus
model into OUT/<model>/{model.stan,model.sexp,data.json}; then optionally
profile each with the sprof LD_PRELOAD sampler.

prep_corpus.py REPO OUT prep
prep_corpus.py REPO OUT profile BENCH SPROF_SO [measure_ms]
"""
import concurrent.futures as cf
import json
import os
import pathlib
import subprocess
import sys

repo = pathlib.Path(sys.argv[1]).resolve()
out = pathlib.Path(sys.argv[2]).resolve()
mode = sys.argv[3]
sys.path.insert(0, str(repo / "harnesses"))
from corpus_bench import benchmark_cases, materialize_data  # noqa: E402

PROBE = repo / "deps/stanc3/stanli-vectorize-probe"
ENV = dict(os.environ, STAN_NUM_THREADS="1", OMP_NUM_THREADS="1")
PCORES = [0, 2, 4, 6, 8, 10, 12, 14]


def prep(item):
    name, (stan, data) = item
    d = out / name
    d.mkdir(parents=True, exist_ok=True)
    src, mir, dj = d / "model.stan", d / "model.sexp", d / "data.json"
    src.write_bytes(stan.read_bytes())
    materialize_data(data, dj)
    r = subprocess.run([PROBE, "--vectorize-loops", "on", "--fast-math",
                        "--output", mir, src], capture_output=True, text=True,
                       timeout=300)
    return name, r.returncode, r.stderr[-300:]


def profile(args):
    name, cpu, bench, so, ms = args
    d = out / name
    if not (d / "model.sexp").exists():
        return name, "no-mir", ""
    env = dict(ENV, LD_PRELOAD=so, SPROF_OUT=str(d / "sprof.bin"), STANLI_SPIKE_WIDTH="1")
    try:
        r = subprocess.run(["taskset", "-c", str(cpu), bench, d / "model.sexp",
                            d / "data.json", "--timed", "--fast-math",
                            "--warmup-ms", "200", "--measure-ms", ms],
                           capture_output=True, text=True, timeout=300, env=env)
    except subprocess.TimeoutExpired:
        return name, "timeout", ""
    (d / "profile_run.txt").write_text(r.stdout + "\n---\n" + r.stderr)
    return name, "ok" if r.returncode == 0 else "fail", r.stderr[-200:]


cases = benchmark_cases(repo / "deps/posteriordb", "all")
if mode == "prep":
    status = {}
    with cf.ThreadPoolExecutor(12) as ex:
        for name, rc, err in ex.map(prep, sorted(cases.items())):
            status[name] = dict(rc=rc, err=err)
    (out / "prep_status.json").write_text(json.dumps(status, indent=1))
    print(len(status), "models;", sum(1 for s in status.values() if s["rc"]), "failed")
else:
    bench, so = sys.argv[4], sys.argv[5]
    ms = sys.argv[6] if len(sys.argv) > 6 else "2000"
    names = sorted(cases)
    status = {}

    def worker(k):
        res = []
        for name in names[k::len(PCORES)]:
            res.append(profile((name, PCORES[k], bench, so, ms)))
        return res
    with cf.ThreadPoolExecutor(len(PCORES)) as ex:
        for res in ex.map(worker, range(len(PCORES))):
            for name, st, err in res:
                status[name] = dict(status=st, err=err)
    (out / "profile_status.json").write_text(json.dumps(status, indent=1))
    from collections import Counter
    print(Counter(s["status"] for s in status.values()))
