#!/usr/bin/env python3
"""Run hp_eval.py over many corpus models, one subprocess each.

  tools/hp/survey.py --out DIR [--models-file F] [--jobs 3] [--timeout 300] -- <hp_eval args>
"""
import argparse
import concurrent.futures
import json
import os
import pathlib
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from common import v  # noqa: E402

HERE = pathlib.Path(__file__).resolve().parent


def one(model, out, timeout, extra):
    p = pathlib.Path(out) / (model + ".json")
    if p.exists():
        return model
    try:
        subprocess.run([sys.executable, str(HERE / "hp_eval.py"), model, "--out", str(out),
                        "--timeout", str(timeout)] + extra, capture_output=True, text=True,
                       timeout=timeout + 60)
    except subprocess.TimeoutExpired:
        p.write_text(json.dumps({"model": model, "error": "timeout"}))
    return model


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--models-file")
    ap.add_argument("--jobs", type=int, default=6)
    ap.add_argument("--timeout", type=int, default=1500)
    ap.add_argument("extra", nargs="*")
    a = ap.parse_args()
    refs, _ = v.replay_refs(v.native_platform(), "clang")
    models = sorted(refs)
    if a.models_file:
        models = [m.strip() for m in open(a.models_file) if m.strip()]
    pathlib.Path(a.out).mkdir(parents=True, exist_ok=True)
    with concurrent.futures.ThreadPoolExecutor(a.jobs) as ex:
        for i, m in enumerate(ex.map(lambda m: one(m, a.out, a.timeout, a.extra), models)):
            if i % 25 == 0:
                print(i, m, flush=True)


if __name__ == "__main__":
    main()
