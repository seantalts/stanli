#!/usr/bin/env python3
"""Freeze allocator model inputs using the shipping compiler and pinned corpus."""
import argparse
import ctypes
import hashlib
import json
import pathlib
import subprocess
import time
import zipfile

MODELS = ["eight_schools_noncentered", "eight_schools_centered", "kidscore_momiq",
          "radon_pooled", "logistic_regression_rhs", "normal_mixture", "garch11",
          "gp_regr", "hierarchical_gp", "hmm_example", "lotka_volterra", "soil_incubation",
          "one_comp_mm_elim_abs", "GLM_Poisson_model", "dogs", "diamonds"]
PDB_PIN = "28f8d3d6e975315f42aa274a8399f21e07a43b30"
NORMAL = """data { int<lower=0> N; vector[N] y; }
parameters { real mu; real<lower=0> sigma; }
model { mu ~ normal(0, 2); sigma ~ normal(0, 1); y ~ normal(mu, sigma); }
"""
GAMMA = """data { int<lower=0> N; vector<lower=0>[N] y; }
parameters { real<lower=0> alpha; real<lower=0> beta; }
model { alpha ~ normal(0, 2); beta ~ normal(0, 2); y ~ gamma(alpha, beta); }
"""


def prepare(args):
    out, pdb = args.output.resolve(), args.pdb.resolve()
    out.mkdir(parents=True)
    pin = subprocess.check_output(["git", "-C", str(pdb), "rev-parse", "HEAD"], text=True).strip()
    assert pin == PDB_PIN, "Use the recorded posteriordb pin"
    lib = None
    if args.library:
        lib = ctypes.CDLL(str(args.library.resolve()))
        lib.stanli_stan_to_mir.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_size_t]
        lib.stanli_stan_to_mir.restype = ctypes.c_void_p
        lib.stanli_string_free.argtypes = [ctypes.c_void_p]
    db = pdb / "posterior_database"
    metadata = {}
    for path in sorted((db / "posteriors").glob("*.json")):
        item = json.loads(path.read_text())
        metadata.setdefault(item["model_name"], item["data_name"])
    cases = []

    def add(name, source, data, origin):
        case = out / "inputs" / name
        case.mkdir(parents=True)
        (case / "model.stan").write_bytes(source.encode())
        (case / "data.json").write_bytes(data)
        started = time.perf_counter()
        if lib:
            error = ctypes.create_string_buffer(8192)
            result = lib.stanli_stan_to_mir(source.encode(), error, len(error))
            assert result, (name, error.value.decode())
            try:
                mir = ctypes.string_at(result)
            finally:
                lib.stanli_string_free(result)
        else:
            mir = subprocess.check_output([str(args.compiler.resolve()), str(case / "model.stan")])
        assert mir.startswith(b"STANLI2:"), "Expected shipping portable MIR"
        (case / "model.mir").write_bytes(mir)
        cases.append(dict(name=name, origin=origin, input_dir=str(case),
            source_compile_seconds=time.perf_counter() - started,
            sha256={p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(case.iterdir())}))
        print("prepared", name, flush=True)

    for name in MODELS:
        with zipfile.ZipFile(db / "data/data" / (metadata[name] + ".json.zip")) as archive:
            data = archive.read(archive.namelist()[0])
        add(name, (db / "models/stan" / (name + ".stan")).read_text(), data,
            "posteriordb/" + metadata[name])
    for kind, source, sizes in [("normal", NORMAL, [8, 128, 1024, 16384, 262144]),
                                ("gamma", GAMMA, [128, 16384])]:
        for n in sizes:
            add(f"{kind}_{n}", source,
                json.dumps(dict(N=n, y=[0.75 + 0.02 * (i % 31) for i in range(n)])).encode(),
                "generated scaling fixture")
    compiler = args.library or args.compiler
    (out / "models.json").write_text(json.dumps(dict(pdb_head=pin, cases=cases,
        compiler=str(compiler.resolve()),
        compiler_sha256=hashlib.sha256(compiler.read_bytes()).hexdigest()), indent=2) + "\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument("--pdb", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--library", type=pathlib.Path)
    group.add_argument("--compiler", type=pathlib.Path)
    prepare(parser.parse_args())
