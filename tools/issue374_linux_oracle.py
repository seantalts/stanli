#!/usr/bin/env python3
"""One-off independent Linux CmdStan comparison for PR #403.

Records CmdStan output unconditionally, including mismatches and refusals.
The candidate never decides which oracle values to retain.
"""
import concurrent.futures
import gzip
import hashlib
import json
import pathlib
import platform
import subprocess
import sys
import threading

from cmdstan_ref import compile_cmd
from corpus_inventory import source_digest
from verify_refs import accepted, load_refs, parse_status, parse_wa, worst_pair

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT = ROOT / "linux-oracle-results"
WORK = ROOT / ".cache/linux-oracle-build"
CS = ROOT / "deps/cmdstan"
STANC = ROOT / "deps/stanc3/stanc"
CHECK = ROOT / "build-rel/stanli_check"
OUT.mkdir(exist_ok=True)
WORK.mkdir(parents=True, exist_ok=True)
LOCK = threading.Lock()
OLD, _ = load_refs()


def command(args):
    return subprocess.check_output(args, text=True).strip()


def sha(path):
    return command(["git", "-C", str(path), "rev-parse", "HEAD"])


RIG = {
    "cmdstan": sha(CS),
    "cmdstan_version": "2.40.0",
    "stan": sha(CS / "stan"),
    "math": sha(CS / "stan/lib/stan_math"),
    "stanc3": (STANC.with_suffix(".src")).read_text().strip(),
    "stanc_sha256": hashlib.sha256(STANC.read_bytes()).hexdigest(),
    "compiler": command(["clang++", "--version"]),
    "reference_flags": "-O1 -ffp-contract=off; stanc default optimization",
    "platform": f"{platform.system()} {platform.machine()}",
    "libc": " ".join(platform.libc_ver()),
    "driver_sha256": source_digest(ROOT / "tools/ref_driver.cpp"),
}
REFS = {"schema": 2, "recorded": RIG, "models": {}}
REPORT = {"candidate": sha(ROOT), "recorded": RIG, "models": {}}


def compare(left, right):
    if len(left) != len(right):
        raise ValueError(f"shape mismatch {len(left)} != {len(right)}")
    return worst_pair(list(map(float, left)), list(map(float, right)))


def one(stan):
    name = stan.stem
    data = stan.with_suffix(".json")
    hpp, exe = WORK / f"{name}.hpp", WORK / f"{name}_ref"
    commands = [
        [str(STANC), str(stan), f"--o={hpp}"],
        compile_cmd(CS, hpp, ROOT / "tools/ref_driver.cpp", exe, sundials=False),
    ]
    for args in commands:
        result = subprocess.run(args, capture_output=True, text=True, timeout=600)
        if result.returncode:
            (OUT / f"{name}.build.log").write_text(result.stdout + result.stderr)
            raise RuntimeError(f"{name}: build failed; see log")
    entry = {"source_sha256": source_digest(stan),
             "data_sha256": source_digest(data), "points": {}}
    if "primary" in OLD[name]:
        entry["primary"] = OLD[name]["primary"]
    report = {}
    for point in (0, 1, 2):
        ref = subprocess.run([str(exe), str(data), str(point)],
                             capture_output=True, text=True, timeout=600)
        got = (subprocess.run([str(CHECK), str(stan), str(data),
                               "--point", str(point), "--wa-values"],
                              capture_output=True, text=True, timeout=600)
               if CHECK.exists() else subprocess.CompletedProcess(
                   [], 0, stdout="", stderr="reference-only recording\n"))
        for engine, result in (("cmdstan", ref), ("stanli", got)):
            (OUT / f"{name}.{point}.{engine}.txt").write_text(result.stdout)
            (OUT / f"{name}.{point}.{engine}.stderr").write_text(result.stderr)
        rf, gf = parse_status(ref.stdout), parse_status(got.stdout)
        pt = {"status": "REJECTED_BOTH"}
        result = {"cmdstan_status": rf[:1], "stanli_status": gf[:1],
                  "cmdstan_returncode": ref.returncode,
                  "stanli_returncode": got.returncode}
        if accepted(rf):
            pt.update(status="OK", values=rf[1:])
            wa = parse_wa(ref.stdout)
            if wa:
                pt["wa"] = {"names": wa[0], "values": wa[1]}
            if accepted(gf):
                result["lp"] = compare(rf[1:2], gf[1:2])
                result["grad"] = compare(rf[2:], gf[2:])
                gwa = parse_wa(got.stdout)
                if wa and gwa and wa[0] == gwa[0]:
                    result["wa"] = compare(wa[1], gwa[1])
                else:
                    result["wa_error"] = "missing or different output names"
            old = OLD[name]["points"][str(point)]
            if "values" in old:
                result["cmdstan_linux_vs_mac"] = compare(old["values"], rf[1:])
        entry["points"][str(point)] = pt
        report[str(point)] = result
    with LOCK:
        REFS["models"][name] = entry
        REPORT["models"][name] = report
        (OUT / "corpus-refs-linux-x86_64.json.gz").write_bytes(
            gzip.compress(json.dumps(REFS, sort_keys=True, indent=0).encode(), mtime=0))
        (OUT / "report.json").write_text(json.dumps(REPORT, indent=2, sort_keys=True) + "\n")
        maxima = {key: max((p.get(key, [0, 0])[1] for p in report.values()), default=0)
                  for key in ("lp", "grad", "wa", "cmdstan_linux_vs_mac")}
        print(f"{len(REFS['models']):3d}/124 {name}: {maxima}", flush=True)


with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
    models = sorted((ROOT / "tests/brms").glob("*.stan"))
    if sys.argv[1:]:
        models = [stan for stan in models if stan.stem in sys.argv[1:]]
    futures = [pool.submit(one, stan) for stan in models]
    errors = []
    for future in concurrent.futures.as_completed(futures):
        try:
            future.result()
        except Exception as exc:
            errors.append(str(exc))
            print(f"ERROR {exc}", flush=True)
    (OUT / "errors.json").write_text(json.dumps(errors))
    if errors:
        raise SystemExit(1)
