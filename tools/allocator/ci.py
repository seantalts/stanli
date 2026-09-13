#!/usr/bin/env python3
"""Paired allocator measurements after a native shipping build, outside its tests.

Where alignment is unchanged, reuse runtime objects and relink the non-installed
benchmark target. Apple ARM also rebuilds those objects with alignment OFF for
the baseline, to check the combined default on a second Apple machine.
"""
import argparse
import hashlib
import json
import pathlib
import platform
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
HERE = pathlib.Path(__file__).resolve().parent
NAMES = ["eight_schools_noncentered", "hierarchical_gp", "normal_8",
         "normal_1024", "normal_262144", "gamma_16384"]


def main(args):
    apple_arm = sys.platform == "darwin" and platform.machine() == "arm64"
    build, out = args.build.resolve(), args.output.resolve()
    out.mkdir(parents=True)
    commands = []

    def run(command):
        command = list(map(str, command))
        commands.append(command)
        (out / "commands.json").write_text(json.dumps(commands, indent=2))
        subprocess.run(command, cwd=ROOT, check=True)

    cache = (build / "CMakeCache.txt").read_text()
    original = {}
    for line in cache.splitlines():
        for key in ["STANLI_SHARED_ALLOCATOR", "STANLI_ALIGN_LOOPS", "STANLI_BUILD_ALLOCATOR_BENCHMARK"]:
            if line.startswith(key + ":"):
                original[key] = line.split("=", 1)[1]
    assert original["STANLI_SHARED_ALLOCATOR"] in ("MIMALLOC", "AUTO")
    library_name = {"darwin": "libstanli_allocator_benchmark.dylib",
                    "win32": "stanli_allocator_benchmark.dll"}.get(sys.platform, "libstanli_allocator_benchmark.so")
    variants = {}
    try:
        for slot, mode in [("candidate", "MIMALLOC"), ("system", "SYSTEM")]:
            alignment = ["-DSTANLI_ALIGN_LOOPS=" + ("ON" if slot == "candidate" else "OFF")] if apple_arm else []
            run(["cmake", "-S", ROOT, "-B", build, "-DSTANLI_BUILD_ALLOCATOR_BENCHMARK=ON",
                 "-DSTANLI_SHARED_ALLOCATOR=" + mode, *alignment])
            run(["cmake", "--build", build, "--target", "stanli_allocator_benchmark", "--parallel", args.jobs])
            directory = out / slot
            directory.mkdir()
            variants[slot] = directory / library_name
            shutil.copy2(build / library_name, variants[slot])
    finally:
        # Shipping binaries are untouched: only the measurement target was built.
        run(["cmake", "-S", ROOT, "-B", build,
             *["-D" + key + "=" + value for key, value in original.items()]])
    (out / "build-cache.txt").write_text(cache)
    (out / "binary-identities.json").write_text(json.dumps({slot: dict(path=str(path),
        sha256=hashlib.sha256(path.read_bytes()).hexdigest()) for slot, path in variants.items()}, indent=2))
    prep = [sys.executable, HERE / "prepare.py", "--pdb", args.pdb,
            "--output", out / "models"]
    if args.compiler:
        prep += ["--compiler", args.compiler]
    else:
        prep += ["--library", variants["system"]]
    run(prep)
    cells = out / "timing-cells.json"
    cells.write_text(json.dumps([[name, w] for name in NAMES for w in (1, 4)]))
    common = ["--inputs", out / "models/models.json", "--system", variants["system"],
              "--candidate", variants["candidate"], "--output", out / "measurements"]
    # All 46 one/four-worker cells get complete-byte checks; six predeclared
    # sentinels get the five-round, both-A/A performance screen (480 processes).
    run([sys.executable, HERE / "bench.py", "verify", *common])
    run([sys.executable, HERE / "bench.py", "bench", *common, "--cells", cells])


if __name__ == "__main__":
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument("--build", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--pdb", type=pathlib.Path, required=True)
    parser.add_argument("--compiler", type=pathlib.Path)
    parser.add_argument("--jobs", type=int, default=2)
    main(parser.parse_args())
