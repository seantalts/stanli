#!/usr/bin/env python3
"""Paired, fresh-process before/after runtime benchmarks on posteriordb.

Both builds consume the same MIR and data. No CmdStan timings are reused or
mixed into this within-Stanli comparison. Raw subprocess output, per-sample
timings, numerical differences and failures are retained in the output folder.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import re
import shlex
import statistics
import struct
import subprocess
import sys
import time
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from verify_refs import parse_status  # noqa: E402


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def shared_driver_command(build, source):
    """Reuse a Ninja build's exact compile flags with a common benchmark source.

    Runtime libraries and their headers remain build-specific. Only the driver
    is shared, so older revisions can use the same deterministic point ladder.
    """
    output = subprocess.check_output(
        ["ninja", "-C", str(build), "-t", "commands", "bench_grad"], text=True)
    for line in output.splitlines():
        argv = shlex.split(line)
        if "-c" not in argv or not argv[argv.index("-c") + 1].endswith("/tools/bench_grad.cpp"):
            continue
        if "-MD" not in argv:
            raise RuntimeError("expected a Unix Ninja benchmark compile command")
        flags = argv[:argv.index("-MD")]
        return [*flags, str(source),
                str(build / "CMakeFiles/stanli_tbb_stub.dir/tests/tbb_stub.cpp.o"),
                str(build / "libstanli.a"), str(build / "libsundials.a"),
                "-o", str(build / "bench_runtime_ab")]
    raise RuntimeError(f"no bench_grad compile command found in {build}")


def clean_environment(overrides=None):
    # A force/escape/profile switch in the launching shell must not silently
    # change what either production binary is being benchmarked against.
    result = {k: v for k, v in os.environ.items()
              if not k.startswith("STANLI_")}
    result.update(OMP_NUM_THREADS="1", OPENBLAS_NUM_THREADS="1",
                  VECLIB_MAXIMUM_THREADS="1", MKL_NUM_THREADS="1")
    result.update(overrides or {})
    return result


def ordered_bits(value):
    bits = struct.unpack(">Q", struct.pack(">d", value))[0]
    return (~bits & ((1 << 64) - 1)) if bits >> 63 else bits | (1 << 63)


def compare_outputs(before, after):
    a, b = parse_status(before), parse_status(after)
    if not a or not b:
        return {"status": "missing_output", "before": a[:1], "after": b[:1]}
    if a[0] != b[0]:
        return {"status": "status_changed", "before": a[0], "after": b[0]}
    if a[0] != "OK":
        return {"status": "both_rejected", "kind": a[0],
                "same_message": a == b, "before": " ".join(a[1:]),
                "after": " ".join(b[1:])}
    av, bv = list(map(float, a[1:])), list(map(float, b[1:]))
    if len(av) != len(bv):
        return {"status": "length_changed", "before": len(av), "after": len(bv)}
    exact = different = max_ulp = zero_sign_changes = nonfinite_changes = 0
    max_abs = max_rel = 0.0
    worst_index = None
    for i, (x, y) in enumerate(zip(av, bv)):
        bits_equal = struct.pack(">d", x) == struct.pack(">d", y)
        exact += bits_equal
        different += not bits_equal
        if not math.isfinite(x) or not math.isfinite(y):
            nonfinite_changes += not ((math.isnan(x) and math.isnan(y)) or x == y)
            continue
        ulp = abs(ordered_bits(x) - ordered_bits(y))
        if ulp > max_ulp:
            max_ulp, worst_index = ulp, i
        zero_sign_changes += x == y == 0.0 and not bits_equal
        max_abs = max(max_abs, abs(x - y))
        max_rel = max(max_rel, abs(x - y) / max(abs(x), abs(y), 1e-300))
    return {"status": "compared", "values": len(av), "exact": exact,
            "different": different, "lp_exact": bool(av) and
            struct.pack(">d", av[0]) == struct.pack(">d", bv[0]),
            "all_finite": all(math.isfinite(v) for v in av + bv),
            "max_ulp": max_ulp, "max_abs": max_abs, "max_rel": max_rel,
            "worst_index": worst_index, "zero_sign_changes": zero_sign_changes,
            "nonfinite_changes": nonfinite_changes}


def distribution(values):
    q1, _, q3 = statistics.quantiles(values, n=4, method="inclusive")
    return {"median": statistics.median(values), "q1": q1, "q3": q3,
            "min": min(values), "max": max(values)}


def parse_bench(output, prep=False):
    # Transformed-data print statements may precede the driver result.
    for line in reversed(output.splitlines()):
        fields = line.split()
        if len(fields) != (2 if prep else 4):
            continue
        try:
            if prep:
                result = {"prep_s": float(fields[0]), "params": int(fields[1])}
            else:
                result = {"gradient_ns": float(fields[0]),
                          "sink": float(fields[1]), "forward_ns": float(fields[2]),
                          "params": int(fields[3])}
            if not all(math.isfinite(v) for v in result.values()):
                continue
            if result["prep_s" if prep else "gradient_ns"] <= 0:
                continue
            return result
        except ValueError:
            pass
    return None


class Runner:
    def __init__(self, output, timeout):
        self.output, self.timeout = output, timeout

    def run(self, command, label, environment=None, measure_rss=False):
        invocation = list(map(str, command))
        if measure_rss:
            invocation = ["/usr/bin/time", "-l" if sys.platform == "darwin" else
                          "-v", *invocation]
        started = time.monotonic()
        try:
            process = subprocess.run(invocation, capture_output=True, text=True,
                                     env=clean_environment(environment), cwd=ROOT,
                                     timeout=self.timeout)
            status = "ok" if process.returncode == 0 else "failed"
            stdout, stderr, code = process.stdout, process.stderr, process.returncode
        except subprocess.TimeoutExpired as error:
            status, code = "timeout", None
            stdout, stderr = error.stdout or b"", error.stderr or b""
            if isinstance(stdout, bytes):
                stdout = stdout.decode(errors="replace")
            if isinstance(stderr, bytes):
                stderr = stderr.decode(errors="replace")
        stem = self.output / "raw" / label
        stem.parent.mkdir(parents=True, exist_ok=True)
        stem.with_suffix(".out").write_text(stdout)
        stem.with_suffix(".err").write_text(stderr)
        rss = None
        if measure_rss:
            pattern = (r"(\d+)\s+maximum resident set size" if sys.platform == "darwin"
                       else r"Maximum resident set size \(kbytes\):\s*(\d+)")
            match = re.search(pattern, stderr)
            if match:
                rss = int(match[1]) / (1048576 if sys.platform == "darwin" else 1024)
        return {"status": status, "returncode": code, "command": invocation,
                "environment": environment or {}, "wall_s": time.monotonic() - started,
                "peak_rss_mib": rss, "stdout": stdout, "stderr": stderr,
                "artifact": str(stem.relative_to(self.output))}


def compact_result(result):
    return {k: v for k, v in result.items() if k not in ("stdout", "stderr")}


def collect_cases(pdb):
    pairs = {}
    for path in sorted((pdb / "posterior_database/posteriors").glob("*.json")):
        meta = json.loads(path.read_text())
        pairs.setdefault(meta["model_name"], meta["data_name"])
    return [{"name": model, "data_name": data, "group": "posteriordb",
             "stan": str(pdb / "posterior_database/models/stan" / f"{model}.stan"),
             "zip": str(pdb / "posterior_database/data/data" / f"{data}.json.zip")}
            for model, data in sorted(pairs.items())]


def prepare_case(case, runner, stanc):
    inputs = runner.output / "inputs"
    inputs.mkdir(exist_ok=True)
    data = inputs / f"{case['name']}.json"
    if "zip" in case:
        with zipfile.ZipFile(case["zip"]) as archive:
            data.write_bytes(archive.read(archive.namelist()[0]))
    else:
        data.write_bytes(Path(case["data"]).read_bytes())
    mir = inputs / f"{case['name']}.mir"
    if "mir" in case:
        mir.write_bytes(Path(case["mir"]).read_bytes())
        stanc_result = None
    else:
        stanc_result = runner.run([stanc, case.get("optimization", "--O1"),
                                  "--debug-optimized-mir", case["stan"]],
                                 f"{case['name']}/stanc")
        if stanc_result["status"] != "ok":
            return None, None, compact_result(stanc_result)
        mir.write_text(stanc_result["stdout"])
    case["input_hashes"] = {"data": digest(data), "mir": digest(mir)}
    return mir, data, compact_result(stanc_result) if stanc_result else None


def measure(case, args, runner):
    row = {"name": case["name"], "group": case.get("group", "extra"),
           "correctness": [], "samples": [], "profiles": {}}
    try:
        mir, data, row["stanc"] = prepare_case(case, runner, args.stanc)
    except (OSError, ValueError, zipfile.BadZipFile) as error:
        row.update(status="input_failure", error=str(error))
        return row
    if mir is None:
        row["status"] = "stanc_failure"
        return row
    row["inputs"] = {"mir": str(mir), "data": str(data),
                     "sha256": case["input_hashes"]}
    envs = {side: case.get(f"{side}_env", {}) for side in ("before", "after")}
    builds = {"before": args.before, "after": args.after}
    for point in args.points:
        outputs, calls = {}, {}
        for side in ("before", "after"):
            call = runner.run([builds[side] / "stanli_check", case["stan"], data,
                               "--mir", mir, "--point", point],
                              f"{case['name']}/check-{point}-{side}", envs[side])
            outputs[side], calls[side] = call["stdout"], compact_result(call)
        comparison = compare_outputs(outputs["before"], outputs["after"])
        comparison.update(point=point, calls=calls)
        row["correctness"].append(comparison)
    valid = [c["point"] for c in row["correctness"]
             if c["status"] == "compared" and c["all_finite"] and
             all(call["status"] == "ok" for call in c["calls"].values())]
    if not valid:
        row["status"] = "no_joint_finite_point"
        return row
    row["benchmark_point"] = valid[0]
    point_args = ["--point", str(valid[0])]
    probes = {}
    for side in ("before", "after"):
        probe = runner.run([builds[side] / args.benchmark, mir, data, "1", *point_args],
                           f"{case['name']}/probe-{side}", envs[side])
        parsed = parse_bench(probe["stdout"]) if probe["status"] == "ok" else None
        probes[side] = {**compact_result(probe), "measurement": parsed}
    row["probes"] = probes
    if not all(p["measurement"] for p in probes.values()):
        row["status"] = "benchmark_unavailable"
        return row
    if probes["before"]["measurement"]["params"] != probes["after"]["measurement"]["params"]:
        row["status"] = "parameter_count_changed"
        return row
    ns = max(p["measurement"]["gradient_ns"] for p in probes.values())
    evaluations = max(3, min(10_000_000, math.ceil(args.seconds * 1e9 / ns)))
    row.update(evaluations=evaluations, params=probes["before"]["measurement"]["params"])
    for sample in range(args.samples):
        order = ("before", "after") if sample % 2 == 0 else ("after", "before")
        pair = {"order": order}
        for side in order:
            pair[side] = {}
            for phase, count in (("prep", "--prep"), ("gradient", str(evaluations))):
                invocation = [builds[side] / args.benchmark, mir, data, count]
                if phase != "prep":
                    invocation += point_args
                result = runner.run(invocation,
                                    f"{case['name']}/{sample}-{phase}-{side}",
                                    envs[side], measure_rss=True)
                parsed = parse_bench(result["stdout"], prep=phase == "prep") if result["status"] == "ok" else None
                pair[side][phase] = {**compact_result(result), "measurement": parsed}
        row["samples"].append(pair)
        if any(pair[side][phase]["measurement"] is None for side in order
               for phase in ("prep", "gradient")):
            row["status"] = "sample_failure"
            return row
    summary = {}
    for side in ("before", "after"):
        summary[side] = {}
        for metric, phase in (("gradient_ns", "gradient"), ("forward_ns", "gradient"),
                              ("prep_s", "prep")):
            summary[side][metric] = distribution([p[side][phase]["measurement"][metric]
                                                  for p in row["samples"]])
        rss = [p[side]["gradient"]["peak_rss_mib"] for p in row["samples"]]
        if all(value is not None for value in rss):
            summary[side]["peak_rss_mib"] = distribution(rss)
    ratios = [p["after"]["gradient"]["measurement"]["gradient_ns"] /
              p["before"]["gradient"]["measurement"]["gradient_ns"]
              for p in row["samples"]]
    summary["paired_after_before"] = distribution(ratios)
    row.update(status="measured", summary=summary)
    # Profiling is separate from all timing samples; it scans graph metadata.
    for side in ("before", "after"):
        result = runner.run([builds[side] / args.benchmark, mir, data, "--prep"],
                            f"{case['name']}/profile-{side}",
                            {**envs[side], "STANLI_PROFILE_PREP": "1"})
        rows = []
        for line in result["stderr"].splitlines():
            if line.startswith("stanli_prep "):
                fields = dict(re.findall(r"(\w+)=([^\s]+)", line))
                rows.append({k: int(v) if re.fullmatch(r"-?\d+", v) else v
                             for k, v in fields.items()})
        row["profiles"][side] = {**compact_result(result), "rows": rows}
    return row


def write_report(report, output):
    (output / "results.json").write_text(json.dumps(report, indent=2, allow_nan=False) + "\n")
    lines = ["# Runtime before/after benchmark", "",
             f"Before: `{report['metadata']['before_ref']}`. After: `{report['metadata']['after_ref']}`.", "",
             "All ratios are after / before; lower is faster. Timings are medians; "
             "raw paired samples and quartiles are in results.json. Numerical ULPs "
             "cover LP and every gradient at all requested points; failures are not dropped.", "",
             "| Model | Before grad (µs) | After grad (µs) | Ratio | Before prep (ms) | After prep (ms) | Before RSS (MiB) | After RSS (MiB) | Max ULP | Status |",
             "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |"]
    for row in report["results"]:
        compared = [c for c in row["correctness"] if c["status"] == "compared"]
        ulp = str(max((c["max_ulp"] for c in compared), default=0)) if compared else "—"
        if row["status"] != "measured":
            lines.append(f"| `{row['name']}` | — | — | — | — | — | — | — | {ulp} | {row['status']} |")
            continue
        summary = row["summary"]
        def cell(side, metric, scale=1):
            value = summary[side].get(metric)
            return f"{value['median'] * scale:.3f}" if value else "—"
        ratio = summary["paired_after_before"]["median"]
        flags = []
        if ratio > 1.05:
            flags.append(">5% slower; confirm")
        if any(c["status"] not in ("compared", "both_rejected") for c in row["correctness"]):
            flags.append("status/shape change")
        if any(c["max_ulp"] > 2 or c["nonfinite_changes"] for c in compared):
            flags.append(">2 ULP; investigate")
        lines.append(f"| `{row['name']}` | {cell('before', 'gradient_ns', .001)} | "
                     f"{cell('after', 'gradient_ns', .001)} | {ratio:.3f} | "
                     f"{cell('before', 'prep_s', 1000)} | {cell('after', 'prep_s', 1000)} | "
                     f"{cell('before', 'peak_rss_mib')} | {cell('after', 'peak_rss_mib')} | {ulp} | "
                     f"{'; '.join(flags) or 'measured'} |")
    (output / "report.md").write_text("\n".join(lines) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--before", type=Path, required=True)
    parser.add_argument("--after", type=Path, required=True)
    parser.add_argument("--before-ref", required=True)
    parser.add_argument("--after-ref", required=True)
    parser.add_argument("--benchmark", default="bench_grad",
                        help="Name of the same benchmark driver linked against both runtimes")
    parser.add_argument("--build-drivers", action="store_true",
                        help="Link this checkout's point-aware driver against both existing Ninja builds")
    parser.add_argument("--stanc", type=Path, required=True)
    parser.add_argument("--pdb", type=Path)
    parser.add_argument("--cases", type=Path, help="Additional case manifest JSON")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--samples", type=int, default=5)
    parser.add_argument("--seconds", type=float, default=.2)
    parser.add_argument("--timeout", type=float, default=300)
    parser.add_argument("--points", nargs="+", type=int, choices=(0, 1, 2), default=[0, 1, 2])
    parser.add_argument("--model", action="append", default=[])
    args = parser.parse_args()
    if (args.samples < 2 or not math.isfinite(args.seconds) or args.seconds <= 0 or
            not math.isfinite(args.timeout) or args.timeout <= 0):
        parser.error("samples >=2 and positive seconds/timeout required")
    args.before, args.after, args.stanc = (p.resolve() for p in (args.before, args.after, args.stanc))
    if args.before == args.after:
        parser.error("before and after builds must be distinct")
    if args.output.exists():
        parser.error("output already exists; use a fresh folder to preserve raw evidence")
    cases = collect_cases(args.pdb.resolve()) if args.pdb else []
    if args.cases:
        cases += json.loads(args.cases.read_text())
    if args.model:
        missing = sorted(set(args.model) - {case["name"] for case in cases})
        if missing:
            parser.error("unknown model(s): " + ", ".join(missing))
        cases = [c for c in cases if c["name"] in args.model]
    if not cases or len({c["name"] for c in cases}) != len(cases):
        parser.error("need nonempty, uniquely named cases")
    driver_commands = []
    if args.build_drivers:
        args.benchmark = "bench_runtime_ab"
        for build in (args.before, args.after):
            command = shared_driver_command(build, ROOT / "tools/bench_grad.cpp")
            subprocess.run(command, cwd=build, check=True)
            driver_commands.append(command)
    binaries = {f"{side}/{name}": digest(build / name)
                for side, build in (("before", args.before), ("after", args.after))
                for name in (args.benchmark, "stanli_check")}
    args.output.mkdir(parents=True)
    metadata = {"before_ref": args.before_ref, "after_ref": args.after_ref,
                "binary_sha256": binaries, "stanc_sha256": digest(args.stanc),
                "benchmark_source_sha256": digest(ROOT / "tools/bench_grad.cpp"),
                "harness_sha256": digest(Path(__file__)),
                "driver_commands": driver_commands,
                "platform": platform.platform(), "samples": args.samples,
                "target_seconds": args.seconds, "timeout_seconds": args.timeout,
                "points": args.points, "started": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
                "load_average_start": os.getloadavg(), "command": sys.argv,
                "environment": {k: v for k, v in clean_environment().items()
                                if k in ("OMP_NUM_THREADS", "OPENBLAS_NUM_THREADS",
                                         "VECLIB_MAXIMUM_THREADS", "MKL_NUM_THREADS")},
                "scrubbed_environment": sorted(k for k in os.environ if k.startswith("STANLI_")),
                "case_count": len(cases)}
    report = {"metadata": metadata, "results": []}
    runner = Runner(args.output, args.timeout)
    for index, case in enumerate(cases, 1):
        print(f"[{index}/{len(cases)}] {case['name']}: starting", flush=True)
        row = measure(case, args, runner)
        report["results"].append(row)
        write_report(report, args.output)
        ratio = row.get("summary", {}).get("paired_after_before", {}).get("median")
        print(f"[{index}/{len(cases)}] {case['name']}: {row['status']} "
              f"after/before={ratio}", flush=True)
    report["metadata"].update(finished=time.strftime("%Y-%m-%dT%H:%M:%S%z"),
                               load_average_end=os.getloadavg())
    write_report(report, args.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
