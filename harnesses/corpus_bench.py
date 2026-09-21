#!/usr/bin/env python3
"""Paired, reproducible corpus benchmarks. See docs/benchmark-protocol.md.

python3 harnesses/corpus_bench.py CMDSTAN PDB fresh.tsv [--corpus rethinking]
Measures source/setup costs and warm gradients. Setup plus 20,000 gradients
is a fixed-work proxy, not measured HMC or sampling time.
A sibling fresh.tsv.run directory holds the frozen manifest and raw evidence.
--resume requires identical inputs, binaries and settings. Historical TSVs are
never appended to, and candidate-only refreshes cannot create paired results.
"""
import argparse
import csv
import hashlib
import io
import json
import math
import os
import pathlib
import platform
import signal
import shutil
import shlex
import zipfile
import statistics
import subprocess
import sys
import time
import threading

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools"))
from cmdstan_ref import compile_cmd
from corpus_inventory import corpus_cases

PROTOCOL = "stanli-corpus-v4"
GRADIENT_BUDGET = 20000
BENCH = REPO / "build-rel/bench_grad"
STANC = REPO / "deps/stanc3/stanc"
VECTORIZE_PROBE = REPO / "deps/stanc3/stanli-vectorize-probe"
COLS = ["model", "params", "stanli_compile_s", "stanli_prep_s",
        "cmdstan_stanc_s", "cmdstan_build_s", "stanli_ns_grad", "cmdstan_ns_grad",
        "stanli_ns_grad_mad", "cmdstan_ns_grad_mad", "paired_speedup",
        "paired_speedup_mad", "paired_rounds", "gradient_budget",
        "stanli_estimated_s", "cmdstan_estimated_s", "run_id", "note"]
THREAD_ENV = {k: "1" for k in ("STAN_NUM_THREADS", "OMP_NUM_THREADS",
                               "OPENBLAS_NUM_THREADS", "MKL_NUM_THREADS",
                               "VECLIB_MAXIMUM_THREADS")}


def row_line(row):
    # A quoted empty last cell is valid TSV without trailing whitespace.
    values = [str(row.get(c, "")) for c in COLS]
    output = io.StringIO()
    csv.writer(output, delimiter="\t", lineterminator="\n").writerow(values)
    line = output.getvalue()
    return line[:-2] + '\t""\n' if not values[-1] else line


def upgrade_header(fieldnames, rows):
    """Legacy serializer helper; new runs never upgrade historical artifacts."""
    if fieldnames == COLS:
        return None
    return "\t".join(COLS) + "\n" + "".join(row_line(rows[m]) for m in sorted(rows))


def sha(path):
    digest = hashlib.sha256()
    with pathlib.Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def checkout_identity(path):
    def git(*args):
        return subprocess.check_output(["git", "-C", str(path), *args])
    return dict(head=git("rev-parse", "HEAD").decode().strip(),
                tracked_diff_sha256=hashlib.sha256(git("diff", "HEAD", "--binary")).hexdigest())


def library_identity(cmdstan):
    # These ignored build products can change without changing a git diff.
    lib = cmdstan / "stan/lib/stan_math/lib"
    paths = set()
    for pattern in ("tbb/*.dylib", "tbb/*.so*", "sundials_*/lib/*.a"):
        paths.update(p for p in lib.glob(pattern) if p.is_file())
    for root in (cmdstan, cmdstan / "stan/lib/stan_math"):
        local = root / "make/local"
        if local.is_file():
            paths.add(local)
    return {str(p.relative_to(cmdstan)): sha(p) for p in sorted(paths)}


def atomic_json(path, value):
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True, allow_nan=False) + "\n")
    temporary.replace(path)


def median_mad(values):
    median = statistics.median(values)
    return median, statistics.median(abs(value - median) for value in values)


def paired_order(round_index):
    return ("stanli", "cmdstan") if round_index % 2 == 0 else ("cmdstan", "stanli")


class PhaseFailure(RuntimeError):
    pass


class Runner:
    """Persist all commands and logs, including failures and censored timeouts."""
    def __init__(self, directory):
        self.directory = directory
        (directory / "logs").mkdir(exist_ok=True)
        self.events = directory / "events.jsonl"
        self.next_id = sum(1 for _ in self.events.open()) if self.events.exists() else 0

    def run(self, phase, argv, timeout, cwd=None):
        event_id = self.next_id
        self.next_id += 1
        stdout = self.directory / "logs" / f"{event_id:06d}.stdout"
        stderr = self.directory / "logs" / f"{event_id:06d}.stderr"
        record = dict(id=event_id, phase=phase, argv=list(map(str, argv)),
                      cwd=str(cwd or REPO), timeout_s=timeout,
                      stdout=str(stdout.relative_to(self.directory)),
                      stderr=str(stderr.relative_to(self.directory)))
        start = time.perf_counter()
        record["load_average"] = list(os.getloadavg()) if hasattr(os, "getloadavg") else None
        with stdout.open("wb") as out, stderr.open("wb") as err:
            try:
                process = subprocess.Popen(record["argv"], cwd=record["cwd"],
                    stdout=out, stderr=err, env={**os.environ, **THREAD_ENV},
                    start_new_session=(os.name == "posix"))
                # wait(timeout=...) polls with sleeps up to 50 ms on POSIX.
                # A separate deadline lets waitpid observe short runs promptly.
                expired = threading.Event()
                def stop_at_deadline():
                    if process.poll() is None:
                        expired.set()
                        try:
                            if os.name == "posix":
                                os.killpg(process.pid, signal.SIGKILL)
                            else:
                                process.kill()
                        except ProcessLookupError:
                            pass
                timer = threading.Timer(timeout, stop_at_deadline)
                timer.daemon = True
                timer.start()
                try:
                    record["returncode"] = process.wait()
                    record["status"] = ("timeout" if expired.is_set() else
                        "ok" if process.returncode == 0 else "failed")
                except BaseException:
                    if os.name == "posix":
                        os.killpg(process.pid, signal.SIGKILL)
                    else:
                        process.kill()
                    process.wait()
                    record["status"] = "interrupted"
                    raise
                finally:
                    timer.cancel()
                    timer.join()
            except OSError as exc:
                record.update(status="failed", returncode=None, error=str(exc))
            finally:
                record["elapsed_s"] = time.perf_counter() - start
                with self.events.open("a") as events:
                    events.write(json.dumps(record, allow_nan=False) + "\n")
        return record

    def text(self, result, stream="stdout"):
        return (self.directory / result[stream]).read_text(errors="replace")

    def require(self, phase, argv, timeout, cwd=None):
        result = self.run(phase, argv, timeout, cwd)
        if result["status"] != "ok":
            raise PhaseFailure(f"{phase}: {result['status']} (event {result['id']})")
        return result


def parse_timing(text, warmup_ms, measure_ms):
    # Stan transformed-data print statements can precede the driver's JSON.
    lines = [line for line in text.splitlines() if line.strip()]
    try:
        result = json.loads(lines[-1])
        if result["protocol"] != "stanli-gradient-v2":
            raise ValueError("wrong timing protocol")
        for key in ("iterations", "elapsed_ns", "batch", "warmup_iterations", "warmup_elapsed_ns"):
            if type(result[key]) is not int or result[key] <= 0:
                raise ValueError(f"invalid {key}")
        if result["warmup_elapsed_ns"] < warmup_ms * 1e6:
            raise ValueError("warmup ended early")
        if result["elapsed_ns"] < measure_ms * 1e6:
            raise ValueError("measurement ended early")
        values = result["values"]
        if not isinstance(values, list) or not values:
            raise ValueError("missing density and gradient")
        if not all(type(v) in (int, float) and math.isfinite(v) for v in values):
            raise ValueError("non-finite density or gradient")
    except (ValueError, KeyError, IndexError, TypeError) as exc:
        raise PhaseFailure(f"invalid timing output: {exc}") from exc
    return result


def check_pair(pair):
    a, b = pair["stanli"]["values"], pair["cmdstan"]["values"]
    if len(a) != len(b):
        raise PhaseFailure("density/gradient widths differ")
    worst = max(abs(x - y) / max(abs(x), abs(y), 1) for x, y in zip(a, b))
    if worst > 1e-9:
        raise PhaseFailure(f"density/gradient mismatch: scaled error {worst:.3g}")
    return worst


def summarize_pairs(pairs):
    times = {engine: [p[engine]["elapsed_ns"] / p[engine]["iterations"] for p in pairs]
             for engine in ("stanli", "cmdstan")}
    result = {}
    for engine, values in times.items():
        result[engine + "_ns_grad"], result[engine + "_ns_grad_mad"] = median_mad(values)
    result["paired_speedup"], result["paired_speedup_mad"] = median_mad(
        [c / s for s, c in zip(times["stanli"], times["cmdstan"])])
    result["paired_rounds"] = len(pairs)
    return result


def open_run(output, expected, resume):
    directory = output.with_suffix(output.suffix + ".run")
    manifest_path = directory / "manifest.json"
    if resume:
        if not manifest_path.exists():
            raise ValueError("--resume requires a versioned run manifest")
        manifest = json.loads(manifest_path.read_text())
        if manifest["identity"] != expected:
            raise ValueError("run identity changed: inputs, binaries or protocol settings differ")
    else:
        if output.exists() or directory.exists():
            raise ValueError("choose a fresh output path; historical or existing runs cannot be appended to")
        directory.mkdir(parents=True)
        manifest = {"identity": expected, "started_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())}
        manifest["run_id"] = hashlib.sha256(json.dumps(manifest, sort_keys=True).encode()).hexdigest()[:16]
        atomic_json(manifest_path, manifest)
    return directory, manifest


def benchmark_cases(pdb, corpus="all"):
    """Benchmark views of the shared numerical/benchmark fixture inventory."""
    return {name: (case.source, case.data)
            for name, case in corpus_cases(pdb, corpus).items()}


def materialize_data(source, destination):
    if source.suffix == ".zip":
        with zipfile.ZipFile(source) as archive:
            destination.write_bytes(archive.read(archive.namelist()[0]))
    else:
        destination.write_bytes(source.read_bytes())


def setup_phase(runner, setup, key, phase, argv, timeout, cwd=None):
    """Keep attempted setup events, including failures, separate from gradients."""
    event = runner.run(phase, argv, timeout, cwd)
    setup[key] = event
    if event["status"] != "ok":
        raise PhaseFailure(f"{phase}: {event['status']} (event {event['id']})")
    return event


def build_model(name, stan, data, work, runner, args, setup=None):
    setup = {} if setup is None else setup
    work.mkdir(exist_ok=True)
    source, hpp, mir = work / f"{name}.stan", work / f"{name}.hpp", work / "model.sexp"
    source.write_bytes(stan.read_bytes())
    setup_phase(runner, setup, "stanli_compile", f"{name}/stanli-mir",
        [args.vectorize_probe, "--vectorize-loops", "on", "--output", mir, source],
        args.build_timeout)
    setup_phase(runner, setup, "cmdstan_stanc", f"{name}/stanc-cpp",
        [args.stanc, source, f"--o={hpp}", *shlex.split(args.stancflags)], args.build_timeout)
    gradient = work / "gradbench"
    runner.require(f"{name}/gradient-driver-build",
        compile_cmd(args.cmdstan, hpp, REPO / "tools/bench_cmdstan_grad.cpp", gradient, opt="-O3"),
        args.build_timeout)
    executables = {"stanli": [args.bench, mir, data], "cmdstan": [gradient, data]}
    return executables, source


def setup_estimates(row, setup):
    """Populate measured setup and fixed-work estimates only with all components."""
    for key in ("stanli_compile", "cmdstan_stanc", "cmdstan_build"):
        event = setup.get(key, {})
        if event.get("status") == "ok":
            elapsed = event["elapsed_s"]
            if not math.isfinite(elapsed) or elapsed < 0:
                raise ValueError(f"invalid setup duration: {key}")
            row[key + "_s"] = elapsed
    for engine, fields in (("stanli", ("stanli_compile_s", "stanli_prep_s")),
                           ("cmdstan", ("cmdstan_stanc_s", "cmdstan_build_s"))):
        grad = row.get(engine + "_ns_grad")
        if grad is not None and all(field in row for field in fields):
            row[engine + "_estimated_s"] = (sum(row[field] for field in fields)
                                            + GRADIENT_BUDGET * grad / 1e9)


def measure_model(name, stan, data, directory, manifest, runner, args):
    row = dict(model=name, run_id=manifest["run_id"], gradient_budget=GRADIENT_BUDGET)
    record = dict(model=name, inputs={"stan": sha(stan), "data": sha(data)}, row=row,
                  gradients=[], setup={}, preparation_s=[], status="failed")
    notes = []
    source = directory / name / f"{name}.stan"
    try:
        commands, source = build_model(name, stan, data, directory / name, runner, args,
                                       record["setup"])
        record["executables"] = {engine: sha(command[0]) for engine, command in commands.items()}
        for round_index in range(args.rounds):
            pair = {}
            order = paired_order(round_index)
            for engine in order:
                event = runner.require(f"{name}/gradient/{round_index}/{engine}",
                    commands[engine] + ["--timed", "--warmup-ms", str(args.warmup_ms),
                                        "--measure-ms", str(args.measure_ms)], args.gradient_timeout)
                pair[engine] = parse_timing(runner.text(event), args.warmup_ms, args.measure_ms)
                pair[engine]["event"] = event["id"]
            deviation = check_pair(pair)
            record["gradients"].append(dict(pair, order=list(order), max_scaled_error=deviation))
        row.update(summarize_pairs(record["gradients"]))
        row["params"] = len(record["gradients"][0]["stanli"]["values"]) - 1
        # Binding existing MIR excludes the separately measured source compiler.
        for i in range(args.rounds):
            event = runner.require(f"{name}/prepare/{i}", commands["stanli"] + ["--prep"],
                                   args.gradient_timeout)
            try:
                duration = float(runner.text(event).splitlines()[-1].split()[0])
                if not math.isfinite(duration) or duration < 0:
                    raise ValueError("non-finite or negative duration")
                record["preparation_s"].append(duration)
            except (IndexError, ValueError) as exc:
                raise PhaseFailure(f"invalid preparation output: {exc}") from exc
        row["stanli_prep_s"] = statistics.median(record["preparation_s"])
    except (PhaseFailure, ValueError) as exc:
        notes.append(str(exc))
    # An ordinary CmdStan executable is setup for the fixed-work proxy. Build
    # it independently of gradient/preparation success, without executing it.
    # Its failure must not discard already accepted gradient measurements.
    if record["setup"].get("cmdstan_stanc", {}).get("status") == "ok":
        try:
            exe = source.with_suffix("")
            setup_phase(runner, record["setup"], "cmdstan_build", f"{name}/cmdstan-build",
                        ["make", exe, f"STANC={args.stanc}", f"STANCFLAGS={args.stancflags}"],
                        args.build_timeout, args.cmdstan)
            record["cmdstan_executable_sha256"] = sha(exe)
        except (PhaseFailure, ValueError) as exc:
            notes.append(str(exc))
    try:
        setup_estimates(row, record["setup"])
    except ValueError as exc:
        notes.append(str(exc))
    record["status"] = ("ok" if not notes else
                        "censored" if all(": timeout (event " in note for note in notes) else "failed")
    row["note"] = "; ".join(notes)
    atomic_json(directory / f"{name}.result.json", record)
    return record


def write_summary(output, records):
    temporary = output.with_suffix(output.suffix + ".tmp")
    temporary.write_text("\t".join(COLS) + "\n" +
                         "".join(row_line(records[m]["row"]) for m in sorted(records)))
    temporary.replace(output)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("cmdstan", type=pathlib.Path)
    parser.add_argument("pdb", type=pathlib.Path)
    parser.add_argument("output", type=pathlib.Path)
    parser.add_argument("--corpus", choices=("all", "posteriordb", "educational", "rethinking", "brms", "teaching"), default="all")
    parser.add_argument("--filter", default="")
    parser.add_argument("--rounds", type=int, default=6)
    parser.add_argument("--warmup-ms", type=int, default=200)
    parser.add_argument("--measure-ms", type=int, default=250)
    parser.add_argument("--gradient-timeout", type=float, default=60)
    parser.add_argument("--build-timeout", type=float, default=900)
    parser.add_argument("--resume", action="store_true")
    parser.add_argument("--bench", type=pathlib.Path, default=BENCH)
    parser.add_argument("--stanc", "--cmdstan-stanc", type=pathlib.Path, default=STANC,
                        help="compiler for the CmdStan model header")
    parser.add_argument("--stancflags", default="", help="flags for CmdStan header generation")
    parser.add_argument("--vectorize-probe", type=pathlib.Path, default=VECTORIZE_PROBE)
    args = parser.parse_args(argv)
    if args.rounds < 2 or args.rounds % 2:
        parser.error("--rounds must be even and at least two for balanced engine order")
    if not all(1 <= n <= 60000 for n in (args.warmup_ms, args.measure_ms)):
        parser.error("timing windows must be 1..60000 milliseconds")
    if not all(math.isfinite(n) and n > 0 for n in
               (args.gradient_timeout, args.build_timeout)):
        parser.error("phase timeouts must be finite and positive")
    if args.gradient_timeout <= (args.warmup_ms + args.measure_ms) / 1000:
        parser.error("gradient timeout must exceed warmup plus measurement")
    for key in ("cmdstan", "pdb", "output", "bench", "stanc", "vectorize_probe"):
        setattr(args, key, getattr(args, key).absolute())
    if any(os.getenv(k, "0") != "0" for k in ("STANLI_PROFILE", "STANLI_PROFILE_PREP")):
        parser.error("disable profiling for comparative measurements")
    # Compiler and runtime overrides change which code is measured. Require
    # explicit tool arguments rather than inherit a shell's experiment state.
    overrides = [k for k, v in os.environ.items() if v and
                 (k in ("STANC", "STANLI_COMPILE") or
                  k.startswith(("STANLI_NO_", "STANLI_PROFILE", "STANLI_LITE_",
                                "STANLI_BOUNDED_", "STANLI_STRUCTURED_",
                                "STANLI_SYMBOLIC_", "STANLI_ISLAND_",
                                "STANLI_WA_", "STANLI_PACKET_",
                                "STANLI_DEBUG_", "STANLI_DUMP_")))]
    if overrides:
        parser.error("unset benchmark environment overrides: " + ", ".join(sorted(overrides)))
    # Resolve inputs in a separate temporary cache before freezing their identities.
    import tempfile
    with tempfile.TemporaryDirectory(prefix="stanli-bench-inputs-") as cache:
        inputs = {}
        cases = corpus_cases(args.pdb, args.corpus)
        for name, case in sorted(cases.items()):
            stan, data_source = case.source, case.data
            if args.filter and args.filter not in name:
                continue
            data = pathlib.Path(cache) / f"{name}.json"
            materialize_data(data_source, data)
            inputs[name] = (stan, data)
        if not inputs:
            parser.error("no models selected")
        config = {k: str(v) if isinstance(v, pathlib.Path) else v for k, v in vars(args).items()
                  if k not in ("resume", "output")}
        config["gradient_budget"] = GRADIENT_BUDGET
        identities = {str(p.relative_to(REPO)): sha(p) for p in (
            REPO / "harnesses/corpus_bench.py", REPO / "tools/benchmark_timer.hpp",
            REPO / "tools/bench_grad.cpp", REPO / "tools/bench_cmdstan_grad.cpp",
            REPO / "tools/cmdstan_ref.py", REPO / "tools/verify_refs.py",
            REPO / "tools/corpus_inventory.py")}
        expected = dict(protocol=PROTOCOL, config=config,
                        machine=dict(platform=platform.platform(), host=platform.node(),
                                     processor=platform.processor(), logical_cpus=os.cpu_count()),
                        threads=THREAD_ENV, sources=identities,
                        stanli_source=checkout_identity(REPO),
                        executables={k: sha(getattr(args, k)) for k in
                                     ("bench", "stanc", "vectorize_probe")},
                        toolchains={"cmdstan": checkout_identity(args.cmdstan),
                                    "stan": checkout_identity(args.cmdstan / "stan"),
                                    "math": checkout_identity(args.cmdstan / "stan/lib/stan_math"),
                                    "stanli_math": checkout_identity(REPO / "deps/math"),
                                    "stanli_stan": checkout_identity(REPO / "deps/stan"),
                                    "stanli_fmt": checkout_identity(REPO / "deps/fmt"),
                                    "libraries": library_identity(args.cmdstan),
                                    "clang_sha256": sha(shutil.which("clang++")),
                                    "build_environment": {k: os.getenv(k) for k in
                                        ("CC", "CXX", "CFLAGS", "CXXFLAGS", "CPPFLAGS", "LDFLAGS", "MAKEFLAGS")}},
                        inputs={m: {"stan": sha(s), "data": sha(d),
                                    "collection": cases[m].collection}
                                for m, (s, d) in inputs.items()})
        try:
            directory, manifest = open_run(args.output, expected, args.resume)
        except ValueError as exc:
            parser.error(str(exc))
        # Paths in raw commands remain valid after this process exits.
        (directory / "inputs").mkdir(exist_ok=True)
        for name, (stan, data) in inputs.items():
            copies = []
            for source, extension in ((stan, ".stan"), (data, ".json")):
                copied = directory / "inputs" / (name + extension)
                if copied.exists() and sha(copied) != sha(source):
                    parser.error(f"retained input was modified: {copied}")
                copied.write_bytes(source.read_bytes())
                copies.append(copied)
            inputs[name] = tuple(copies)
        runner, records = Runner(directory), {}
        for name, (stan, data) in inputs.items():
            saved = directory / f"{name}.result.json"
            if args.resume and saved.exists():
                records[name] = json.loads(saved.read_text())
            else:
                print(f"{name}: paired gradient measurements", flush=True)
                records[name] = measure_model(name, stan, data, directory, manifest, runner, args)
            write_summary(args.output, records)
            print(f"{name}: {records[name]['status']} {records[name]['row']['note']}", flush=True)
        return int(any(r["status"] != "ok" for r in records.values()))


if __name__ == "__main__":
    raise SystemExit(main())
