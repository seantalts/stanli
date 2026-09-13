#!/usr/bin/env python3
"""Bounded Linux rollout gate, run after the normal shipping build.

Builds/tests both variants before any performance run. All evidence stays in
the output directory. Completion means collection succeeded, not permission
to promote: performance, A/A and retained-memory results require review.
"""
import argparse
import json
import os
import pathlib
import platform
import shutil
import subprocess
import sys
import time

from bench import ROOT, save, sha
from sampling import environment

HERE = pathlib.Path(__file__).resolve().parent


def main(args):
    assert sys.platform == "linux", "This rollout only promotes Linux"
    build, out = args.build.resolve(), args.output.resolve()
    out.mkdir(parents=True)
    (out / "logs").mkdir()
    commands = []

    def run(command, label, env=None):
        command = list(map(str, command))
        record = dict(command=command, started=time.time(), log=label)
        commands.append(record)
        save(out / "commands.json", commands)
        print("running", label, flush=True)
        try:
            with (out / "logs" / (label + ".log")).open("w") as log:
                result = subprocess.run(command, cwd=ROOT, env=env,
                                        stdout=log, stderr=subprocess.STDOUT, timeout=14400)
            record["returncode"] = result.returncode
            assert result.returncode == 0, (label, command)
        finally:
            record["finished"] = time.time()
            save(out / "commands.json", commands)

    original = {}
    for line in (build / "CMakeCache.txt").read_text().splitlines():
        for key in ("STANLI_SHARED_ALLOCATOR", "STANLI_BUILD_ALLOCATOR_BENCHMARK"):
            if line.startswith(key + ":"):
                original[key] = line.split("=", 1)[1]
    assert original["STANLI_SHARED_ALLOCATOR"] == "AUTO", "Validate DEFAULT, not an overridden allocator"
    host = dict(platform=platform.platform(), machine=platform.machine(),
        python=sys.executable, cpu_count=os.cpu_count(),
        environment={k: os.environ[k] for k in ("OPENBLAS_NUM_THREADS", "OMP_NUM_THREADS",
            "MKL_NUM_THREADS") if k in os.environ})
    for path in ("/proc/cpuinfo", "/proc/meminfo", "/proc/self/status",
                 "/sys/fs/cgroup/cpu.max", "/sys/fs/cgroup/cpu.stat",
                 "/sys/fs/cgroup/cpuset.cpus.effective"):
        if pathlib.Path(path).exists():
            host[path] = pathlib.Path(path).read_text()
    save(out / "host.json", host)
    run(["c++", "--version"], "compiler")
    run(["git", "-c", "safe.directory=" + str(ROOT), "rev-parse", "HEAD"], "source")
    variants, packages = {}, {}
    try:
        for slot in ("system", "candidate"):
            mode = ["-DSTANLI_SHARED_ALLOCATOR=SYSTEM"] if slot == "system" else ["-USTANLI_SHARED_ALLOCATOR"]
            run(["cmake", "-S", ROOT, "-B", build, "-DSTANLI_BUILD_ALLOCATOR_BENCHMARK=ON", *mode],
                "configure-" + slot)
            run(["cmake", "--build", build, "--parallel", args.jobs], "build-" + slot)
            run(["ctest", "--test-dir", build, "--parallel", os.cpu_count() or 2,
                 "--output-on-failure"], "ctest-" + slot)
            directory = out / "libraries" / slot
            directory.mkdir(parents=True)
            for name in ("libstanli.so", "libstanli_allocator_benchmark.so"):
                shutil.copy2(build / name, directory / name)
            shutil.copy2(build / "CMakeCache.txt", directory / "CMakeCache.txt")
            variants[slot] = directory
            package = out / "packages" / slot
            shutil.copytree(ROOT / "python/stanli", package / "stanli",
                            ignore=shutil.ignore_patterns("_bin", "__pycache__", "*.pyc"))
            (package / "stanli/_bin").mkdir()
            shutil.copy2(directory / "libstanli.so", package / "stanli/_bin/libstanli.so")
            packages[slot] = package
            run([sys.executable, ROOT / "tests/test_python.py"], "python-" + slot,
                environment(package))
    finally:
        run(["cmake", "-S", ROOT, "-B", build,
             *["-D" + k + "=" + v for k, v in original.items()]], "restore-configuration")
    save(out / "binary-identities.json", {s: {p.name: sha(p) for p in path.iterdir()
                                             if p.suffix == ".so"} for s, path in variants.items()})
    run([sys.executable, HERE / "prepare.py", "--pdb", args.pdb, "--library",
         variants["system"] / "libstanli.so", "--output", out / "models"], "prepare")
    inputs = out / "models/models.json"
    gradient = ["--inputs", inputs, "--system", variants["system"] / "libstanli_allocator_benchmark.so",
                "--candidate", variants["candidate"] / "libstanli_allocator_benchmark.so",
                "--output", out / "gradients", "--eight"]
    sampling = ["--inputs", inputs, "--system", packages["system"],
                "--candidate", packages["candidate"], "--output", out / "sampling"]
    # Refuse a numerical or sampling failure before spending the broad timing budget.
    run([sys.executable, HERE / "bench.py", "verify", *gradient], "gradient-verify")
    run([sys.executable, HERE / "sampling.py", "verify", *sampling], "sampling-verify")
    retention = {}
    for slot in ("system", "candidate"):
        run([sys.executable, HERE / "retention.py", "--inputs", inputs, "--library",
             variants[slot] / "libstanli_allocator_benchmark.so", "--mode",
             "system" if slot == "system" else "private", "--output", out / ("retention-" + slot)],
            "retention-" + slot, environment(packages[slot]))
        retention[slot] = json.loads((out / ("retention-" + slot) / "result.json").read_text())
    assert retention["system"]["snapshots"] == retention["candidate"]["snapshots"]
    run([sys.executable, HERE / "bench.py", "bench", *gradient], "gradient-bench")
    run([sys.executable, HERE / "confirm.py", "--screen", out / "gradients",
         "--inputs", inputs, "--output", out / "gradient-confirmation"], "gradient-confirm")
    run([sys.executable, HERE / "sampling.py", "bench", *sampling], "sampling-bench")
    run([sys.executable, HERE / "sampling.py", "confirm", *sampling], "sampling-confirm")
    save(out / "completion.json", dict(finished=time.time(),
        collection_complete=True, decision="pending performance, A/A and RSS review"))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument("--build", type=pathlib.Path, required=True)
    parser.add_argument("--pdb", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--jobs", type=int, default=2)
    main(parser.parse_args())
