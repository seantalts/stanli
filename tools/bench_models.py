#!/usr/bin/env python3
"""Per-gradient benchmark across a set of posteriordb models: stanli vs
CmdStan, both at the same deterministic unconstrained point, both -O3 and
-ffp-contract=off. Also reports model-preparation time (stanli lowering vs
CmdStan's stanc + clang compile), which is the time-to-first-draw term.

Usage: tools/bench_models.py CMDSTAN_DIR PDB_DIR [model ...]

Requires build-rel/bench_grad (Release). Prints a markdown table.
"""
import json
import pathlib
import subprocess
import sys
import tempfile
import time
import zipfile

from cmdstan_ref import compile_cmd

REPO = pathlib.Path(__file__).resolve().parent.parent
DEFAULT_MODELS = [
    "eight_schools_noncentered",
    "arK",
    "kidscore_momiq",
    "radon_pooled",
    "low_dim_gauss_mix",
    "wells_dist100ars_model",
    "lsat_model",
    "bym2_offset_only",
]


def evals_for(n_params):
    """Fewer repetitions for big models; enough for a stable mean."""
    if n_params > 2000:
        return 300
    if n_params > 200:
        return 3000
    return 50000


def main():
    cs = pathlib.Path(sys.argv[1])
    pdb = pathlib.Path(sys.argv[2]) / "posterior_database"
    models = sys.argv[3:] or DEFAULT_MODELS
    tmp = pathlib.Path(tempfile.mkdtemp(prefix="stanli_bench_"))
    stanc = REPO / "deps/stanc3/stanc"

    datas = {}
    for pj in sorted((pdb / "posteriors").glob("*.json")):
        meta = json.loads(pj.read_text())
        datas.setdefault(meta["model_name"], meta["data_name"])

    rows = []
    for model in models:
        stan = pdb / "models" / "stan" / f"{model}.stan"
        dz = pdb / "data" / "data" / f"{datas[model]}.json.zip"
        dj = tmp / f"{model}_data.json"
        with zipfile.ZipFile(dz) as z:
            dj.write_bytes(z.read(z.namelist()[0]))

        # stanli: stanc to MIR (subprocess here; the library embeds it), then
        # lower + bind, then time the gradient.
        sexp = tmp / f"{model}.sexp"
        t0 = time.perf_counter()
        sexp.write_text(subprocess.run(
            [str(stanc), "--debug-transformed-mir", str(stan)],
            capture_output=True, text=True, check=True).stdout)
        t_stanc = time.perf_counter() - t0

        probe = subprocess.run(
            [str(REPO / "build-rel/bench_grad"), str(sexp), str(dj), "1"],
            capture_output=True, text=True)
        if probe.returncode != 0:
            print(f"SKIP {model}: stanli bench failed")
            continue
        n_params = int(probe.stdout.split()[-1]) if probe.stdout else 0
        n_evals = evals_for(n_params)

        t0 = time.perf_counter()
        out = subprocess.run(
            [str(REPO / "build-rel/bench_grad"), str(sexp), str(dj),
             str(n_evals)], capture_output=True, text=True).stdout.split()
        rt_ns = float(out[0])
        t_rt_total = time.perf_counter() - t0

        # stanli preparation: lowering only (one eval run, minus the eval).
        t0 = time.perf_counter()
        subprocess.run([str(REPO / "build-rel/bench_grad"), str(sexp),
                        str(dj), "1"], capture_output=True, text=True)
        t_lower = time.perf_counter() - t0

        # CmdStan: stanc to C++, clang compile, then time the same loop.
        hpp = tmp / f"{model}.hpp"
        t0 = time.perf_counter()
        subprocess.run([str(stanc), str(stan), f"--o={hpp}"], check=True,
                       capture_output=True)
        t_cs_stanc = time.perf_counter() - t0
        exe = tmp / f"{model}_bench"
        cmd = compile_cmd(cs, hpp, REPO / "tools/bench_cmdstan_grad.cpp",
                          exe, opt="-O3")
        t0 = time.perf_counter()
        r = subprocess.run(cmd, capture_output=True, text=True)
        t_cs_build = time.perf_counter() - t0
        if r.returncode != 0:
            print(f"SKIP {model}: CmdStan build failed: "
                  f"{r.stderr.splitlines()[-1][:100]}")
            continue
        cs_out = subprocess.run([str(exe), str(dj), str(n_evals)],
                                capture_output=True, text=True).stdout.split()
        cs_ns = float(cs_out[0])

        rows.append((model, n_params, rt_ns, cs_ns, cs_ns / rt_ns,
                     t_lower, t_cs_stanc + t_cs_build))
        print(f"{model}: n={n_params} stanli={rt_ns:.0f}ns "
              f"cmdstan={cs_ns:.0f}ns speedup={cs_ns / rt_ns:.2f}x "
              f"prep {t_lower:.3f}s vs {t_cs_stanc + t_cs_build:.1f}s")

    print("\n| model | unconstrained params | stanli ns/grad | "
          "CmdStan ns/grad | speedup | stanli prep | CmdStan build |")
    print("| --- | ---: | ---: | ---: | ---: | ---: | ---: |")
    for m, n, rt, cs_, sp, tl, tb in rows:
        print(f"| `{m}` | {n} | {rt:.0f} | {cs_:.0f} | {sp:.2f}x | "
              f"{tl:.3f} s | {tb:.1f} s |")


if __name__ == "__main__":
    main()
