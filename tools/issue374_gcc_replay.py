import concurrent.futures
import json
import pathlib
import os
import subprocess

from verify_refs import ULP_LIMITS, check_model, replay_refs, runtime_compiler

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT = ROOT / "gcc-results"
OUT.mkdir(exist_ok=True)
CHECK = ROOT / "build-rel/stanli_check"
compiler = runtime_compiler(CHECK)
refs, rig = replay_refs("Linux x86_64", compiler)
refs = {name: refs[name] for name in ULP_LIMITS}


def one(name):
    result = check_model(name, refs[name], ROOT / "unused-pdb", CHECK, OUT, 600, 1e-9)
    # Keep raw output too, including points after an initial disagreement.
    for point in (0, 1, 2):
        got = subprocess.run([str(CHECK), str(ROOT / f"tests/brms/{name}.stan"),
                              str(ROOT / f"tests/brms/{name}.json"),
                              "--point", str(point), "--wa-values"],
                             capture_output=True, text=True, timeout=600)
        (OUT / f"{name}.{point}.stdout").write_text(got.stdout)
        (OUT / f"{name}.{point}.stderr").write_text(got.stderr)
    print(result, flush=True)
    return result


with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
    results = list(pool.map(one, sorted(refs)))
report = {"compiler": subprocess.check_output([os.environ.get("CXX", "c++"), "--version"], text=True),
          "cmdstan_rig": rig, "results": results}
(OUT / "report.json").write_text(json.dumps(report, indent=2) + "\n")
failures = [r for r in results if r[1] != "OK"]
print(f"{len(results) - len(failures)}/{len(results)} pass")
raise SystemExit(bool(failures))
