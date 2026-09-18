#!/usr/bin/env python3
"""Run the bounded reduce_sum probe in fresh processes; retain raw evidence."""
import argparse
import hashlib
import itertools
import json
import os
from pathlib import Path
import platform
import statistics
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", default="build-reduce-sum/bench_reduce_sum")
    parser.add_argument("--output", default="build-reduce-sum/evidence")
    parser.add_argument("--checks-only", action="store_true")
    parser.add_argument("--imports", action="store_true")
    parser.add_argument("--native", action="store_true")
    parser.add_argument("--owner-merge", action="store_true")
    args = parser.parse_args()
    if args.native and args.imports:
        parser.error("--native and --imports select different experiments")
    out = Path(args.output)
    out.mkdir(parents=True, exist_ok=True)
    binary = Path(args.binary).resolve()
    metadata = {
        "platform": platform.platform(),
        "head": subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip(),
        "binary": str(binary),
        "allocation_scope": "ordinary C++ new/new[] only; excludes aligned new and malloc",
        "rss_scope": "fresh-process high water including compilation, whole, full and (if enabled) compact executors",
        "native": args.native, "imports": args.imports, "worker_publish": not args.owner_merge,
        "logical_cpus": os.cpu_count(),
    }
    inputs = [binary, Path("tools/bench_reduce_sum.cpp"),
              Path("tools/bench_reduce_sum.py"), Path("tools/reduce_sum_imports.hpp"),
              Path("runtime/include/stanli/detail/input_ranges.hpp"),
              Path("runtime/src/reduce_sum.cpp"), Path("runtime/src/lower_higher_order.cpp"),
              Path("runtime/src/executor.cpp"),
              Path("tests/fixtures/reduce_sum_parallel_probe.stan"),
              Path("tests/fixtures/reduce_sum_parallel_probe.tmir.sexp")]
    metadata["sha256"] = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
    cache = binary.parent / "CMakeCache.txt"
    if cache.exists():
        names = ("CMAKE_BUILD_TYPE:", "CMAKE_CXX_COMPILER:", "CMAKE_CXX_FLAGS_RELEASE:",
                 "STANLI_THREADS:", "STANLI_SANITIZE:")
        metadata["build_configuration"] = [line for line in cache.read_text().splitlines()
                                           if line.startswith(names)]
    if platform.system() == "Darwin":
        metadata["cpu"] = subprocess.check_output(["sysctl", "-n", "machdep.cpu.brand_string"], text=True).strip()
    metadata["dependencies"] = {
        name: subprocess.check_output(["git", "-C", "deps/" + name, "rev-parse", "HEAD"], text=True).strip()
        for name in ["math", "stan"]}
    metadata["stanc_source"] = Path("deps/stanc3/stanc.src").read_text().strip()
    metadata["dependency_patches"] = {
        name: subprocess.check_output(["git", "-C", "deps/" + name, "diff", "HEAD", "--"], text=True)
        for name in ["math", "stan"]}
    (out / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")

    def run(options, check=False):
        command = [str(binary)]
        if args.native:
            command.append("--native")
        if args.imports:
            command.append("--imports")
        if args.owner_merge:
            command.append("--owner-merge")
        for name, value in options.items():
            command.extend(["--" + name, str(value)])
        if check:
            command.append("--check-only")
        proc = subprocess.run(command, text=True, capture_output=True)
        if proc.returncode:
            raise RuntimeError(f"{command}: {proc.stderr}")
        record = json.loads(proc.stdout)
        if args.imports and record["compact_accepted_chunks"] != record["chunks"]:
            raise RuntimeError(f"{command}: compact path unexpectedly refused")
        record["command"] = command
        return record

    checks = []
    with (out / "checks.jsonl").open("w") as f:
        for n, kind, active, threads in itertools.product(
                ([0, 1, 8192, 16385] if args.native else [0, 1, 7, 31]), [0, 1], [0, 1], [1, 2, 4]):
            result = run(dict(n=n, kind=kind, active=active, threads=threads), check=True)
            checks.append(result)
            f.write(json.dumps(result) + "\n")
        result = run(dict(n=17, chunks=8, threads=8, active=1), check=True)
        checks.append(result)
        f.write(json.dumps(result) + "\n")
    print(f"{len(checks)} correctness configurations passed", flush=True)
    if args.checks_only:
        return

    cases = [dict(n=n, kind=kind) for n in [1000, 10000, 100000] for kind in [0, 1]]
    cases += [dict(n=100000, kind=1, threads=t) for t in [1, 2]]
    cases += [dict(n=n, kind=1, active=1) for n in [1000, 10000, 100000]]
    cases += [dict(n=10000, kind=1, p=p) for p in [256, 4096]]
    lines = ["| N | P | Kind | Active slice | Threads | Whole µs | Chunks serial µs | Parallel µs (IQR) | Speedup vs whole | Peak MiB |",
             "|--:|--:|:--|:--|--:|--:|--:|--:|--:|--:|"]
    if args.imports:
        lines = ["| N | P | Kind | Active | Threads | Whole µs | Full parallel µs | Compact parallel µs (IQR) | Gain vs full | Arena MiB full → compact |",
                 "|--:|--:|:--|:--|--:|--:|--:|--:|--:|--:|"]
    serial_key = "native_serial_ns" if args.native else "serial_chunks_ns"
    parallel_key = "native_parallel_ns" if args.native else "parallel_chunks_ns"
    rss_key = "peak_rss_bytes" if args.native else "peak_process_rss_bytes"
    with (out / "timings.jsonl").open("w") as f:
        for case in cases:
            result = run(case)
            f.write(json.dumps(result) + "\n")
            f.flush()
            whole, serial, parallel = [statistics.median(result[key]) / 1000
                                       for key in ["whole_ns", serial_key, parallel_key]]
            q1, _, q3 = statistics.quantiles(result[parallel_key], n=4)
            line = (f"| {result['n']} | {result['p']} | {'normal' if result['kind'] == 0 else 'student_t'} | "
                    f"{bool(result['active'])} | {result['threads']} | {whole:.2f} | {serial:.2f} | "
                    f"{parallel:.2f} ({(q3-q1)/1000:.2f}) | {whole/parallel:.2f}x | "
                    f"{result[rss_key]/2**20:.1f} |")
            if args.imports:
                compact = statistics.median(result["compact_parallel_ns"]) / 1000
                q1, _, q3 = statistics.quantiles(result["compact_parallel_ns"], n=4)
                full_bytes = sum(result["chunk_" + name + "_bytes"] for name in ["value", "adjoint", "scratch"])
                compact_bytes = sum(result["compact_" + name + "_bytes"] for name in ["value", "adjoint", "scratch"])
                line = (f"| {result['n']} | {result['p']} | {'normal' if result['kind'] == 0 else 'student_t'} | "
                        f"{bool(result['active'])} | {result['threads']} | {whole:.2f} | {parallel:.2f} | "
                        f"{compact:.2f} ({(q3-q1)/1000:.2f}) | {parallel/compact:.2f}x | "
                        f"{full_bytes/2**20:.3f} → {compact_bytes/2**20:.3f} |")
            lines.append(line)
            print(line, flush=True)
    (out / "summary.md").write_text("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
