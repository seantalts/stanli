#!/usr/bin/env python3
"""Paired, reproducible corpus benchmarks. See docs/benchmark-protocol.md.

python3 harnesses/corpus_bench.py CMDSTAN PDB fresh.tsv [--corpus rethinking]
Defaults to gradient measurements. Add --sampling for full inference runs.
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
import statistics
import subprocess
import sys
import time

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools"))
from cmdstan_ref import compile_cmd
from verify_refs import model_files

PROTOCOL = "stanli-corpus-v2"
BENCH = REPO / "build-rel/bench_grad"
RUN = REPO / "build-rel/stanli_run"
STANC = REPO / "deps/stanc3/stanc"
COLS = ["model", "params", "stanli_prep_s", "stanli_ns_grad",
        "stanli_sample_s", "stanli_grads", "cmdstan_build_s",
        "cmdstan_ns_grad", "cmdstan_sample_s", "stanli_ns_grad_mad",
        "cmdstan_ns_grad_mad", "paired_speedup", "paired_speedup_mad",
        "paired_rounds", "run_id", "note"]
THREAD_ENV = {k: "1" for k in ("STAN_NUM_THREADS", "OMP_NUM_THREADS",
                               "OPENBLAS_NUM_THREADS", "MKL_NUM_THREADS",
                               "VECLIB_MAXIMUM_THREADS")}


def parse_grad_count(stderr_text):
    import re
    found = re.search(r"stanli_run: (\d+) gradient evaluations", stderr_text or "")
    return found.group(1) if found else ""


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
                try:
                    record["returncode"] = process.wait(timeout=timeout)
                    record["status"] = "ok" if process.returncode == 0 else "failed"
                except subprocess.TimeoutExpired:
                    if os.name == "posix":
                        os.killpg(process.pid, signal.SIGKILL)
                    else:
                        process.kill()
                    record["returncode"] = process.wait()
                    record["status"] = "timeout"
                except BaseException:
                    if os.name == "posix":
                        os.killpg(process.pid, signal.SIGKILL)
                    else:
                        process.kill()
                    process.wait()
                    record["status"] = "interrupted"
                    raise
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


def validate_draws(path, expected_rows):
    """A zero exit status alone does not establish a complete sampling run."""
    try:
        with path.open() as stream:
            rows = csv.reader(line for line in stream if line.strip() and not line.startswith("#"))
            header = next(rows)
            if "lp__" not in header or len(set(header)) != len(header):
                raise ValueError("missing sampler density or duplicate columns")
            count = 0
            for row in rows:
                if len(row) != len(header) or not all(math.isfinite(float(v)) for v in row):
                    raise ValueError("invalid or non-finite draw")
                count += 1
            if count != expected_rows:
                raise ValueError(f"expected {expected_rows} draws, found {count}")
    except (OSError, StopIteration, ValueError) as exc:
        raise PhaseFailure(f"invalid sampling CSV: {exc}") from exc


def summarize_sampling(events, seeds):
    result = {}
    for engine in ("stanli", "cmdstan"):
        selected = [e for e in events if e["engine"] == engine]
        if (len(selected) == len(seeds) and {e["seed"] for e in selected} == set(seeds)
                and all(e["status"] == "ok" for e in selected)):
            result[engine + "_sample_s"] = statistics.median(e["elapsed_s"] for e in selected)
    return result


def sampling_order(round_index, cmdstan_multiple):
    return ("cmdstan", "stanli") if cmdstan_multiple is not None else paired_order(round_index)


def sampling_limit(engine, absolute_limit, cmdstan_multiple, reference):
    if engine == "cmdstan" or cmdstan_multiple is None:
        return absolute_limit
    if reference is None or reference["status"] != "ok":
        return None
    return min(absolute_limit, cmdstan_multiple * reference["elapsed_s"])


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


def model_pairs(pdb, corpus):
    pairs = {}
    if corpus != "rethinking":
        for source in sorted((pdb / "posteriors").glob("*.json")):
            meta = json.loads(source.read_text())
            pairs.setdefault(meta["model_name"], meta["data_name"])
    if corpus != "posteriordb":
        pairs.update({p.stem: None for p in (REPO / "tests/rethinking").glob("*.stan")})
    return pairs


def build_model(name, stan, data, work, runner, args):
    work.mkdir(exist_ok=True)
    source, hpp, mir = work / f"{name}.stan", work / f"{name}.hpp", work / "model.sexp"
    source.write_bytes(stan.read_bytes())
    result = runner.require(f"{name}/stanc-mir",
        [args.stanc, "--O1", "--debug-optimized-mir", source, f"--o={hpp}"], args.build_timeout)
    mir.write_text(runner.text(result))
    runner.require(f"{name}/stanc-cpp", [args.stanc, source, f"--o={hpp}"], args.build_timeout)
    gradient = work / "gradbench"
    runner.require(f"{name}/gradient-driver-build",
        compile_cmd(args.cmdstan, hpp, REPO / "tools/bench_cmdstan_grad.cpp", gradient, opt="-O3"),
        args.build_timeout)
    executables = {"stanli": [args.bench, mir, data], "cmdstan": [gradient, data]}
    return executables, source


def measure_model(name, stan, data, directory, manifest, runner, args):
    row = dict(model=name, run_id=manifest["run_id"])
    record = dict(model=name, inputs={"stan": sha(stan), "data": sha(data)}, row=row,
                  gradients=[], sampling=[], status="failed")
    notes = []
    try:
        commands, source = build_model(name, stan, data, directory / name, runner, args)
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
        # File-to-bound-executor preparation is a distinct metric; it excludes stanc.
        preparations = []
        for i in range(args.rounds):
            event = runner.require(f"{name}/prepare/{i}", commands["stanli"] + ["--prep"],
                                   args.gradient_timeout)
            try:
                duration = float(runner.text(event).splitlines()[-1].split()[0])
                if not math.isfinite(duration) or duration < 0:
                    raise ValueError("non-finite or negative duration")
                preparations.append(duration)
            except (IndexError, ValueError) as exc:
                raise PhaseFailure(f"invalid preparation output: {exc}") from exc
        row["stanli_prep_s"] = statistics.median(preparations)
        record["preparation_s"] = preparations
        if args.sampling:
            exe = source.with_suffix("")
            event = runner.require(f"{name}/cmdstan-build", ["make", exe], args.build_timeout,
                                   args.cmdstan)
            row["cmdstan_build_s"] = event["elapsed_s"]
            record["sampling_executables"] = {"stanli": sha(args.run), "cmdstan": sha(exe)}
            for i, seed in enumerate(args.seeds):
                reference = None
                for engine in sampling_order(i, args.cmdstan_runtime_multiple):
                    timeout = sampling_limit(engine, args.sample_timeout,
                                             args.cmdstan_runtime_multiple, reference)
                    if timeout is None:
                        record["sampling"].append(dict(engine=engine, seed=seed,
                            status="not_run", reason="matching CmdStan run did not finish successfully"))
                        notes.append(f"stanli_sample_not_run(seed={seed}; reference unavailable)")
                        continue
                    command = ([args.run, stan, data, "--warmup", str(args.iter_warmup),
                                "--samples", str(args.iter_sampling), "--seed", str(seed),
                                "--sampler-stats", "--stanc", args.stanc]
                               if engine == "stanli" else
                               [exe, "sample", f"num_warmup={args.iter_warmup}",
                                f"num_samples={args.iter_sampling}", "random", f"seed={seed}",
                                "data", f"file={data}", "output", f"file={directory/name}/sample-{seed}.csv"])
                    event = runner.run(f"{name}/sample/{seed}/{engine}", command, timeout)
                    if event["status"] == "ok":
                        csv_path = (directory / event["stdout"] if engine == "stanli" else
                                    directory / name / f"sample-{seed}.csv")
                        try:
                            validate_draws(csv_path, args.iter_sampling)
                        except PhaseFailure as exc:
                            event = dict(event, status="failed", validation_error=str(exc))
                    record["sampling"].append(dict(engine=engine, seed=seed, **event))
                    if engine == "cmdstan":
                        reference = event
                    if event["status"] != "ok":
                        notes.append(f"{engine}_sample_{event['status']}({timeout:.6g}s, seed={seed})")
            # No median over surviving seeds: a timeout censors the whole summary.
            row.update(summarize_sampling(record["sampling"], args.seeds))
        else:
            notes.append("sampling_not_requested")
        record["status"] = ("failed" if any("failed" in n for n in notes) else
                            "censored" if any("timeout" in n for n in notes) else "ok")
    except (PhaseFailure, ValueError) as exc:
        notes.append(str(exc))
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
    parser.add_argument("--corpus", choices=("all", "posteriordb", "rethinking"), default="all")
    parser.add_argument("--filter", default="")
    parser.add_argument("--rounds", type=int, default=6)
    parser.add_argument("--warmup-ms", type=int, default=200)
    parser.add_argument("--measure-ms", type=int, default=250)
    parser.add_argument("--gradient-timeout", type=float, default=60)
    parser.add_argument("--build-timeout", type=float, default=900)
    parser.add_argument("--sampling", action="store_true")
    parser.add_argument("--sample-timeout", type=float, default=900)
    parser.add_argument("--cmdstan-runtime-multiple", type=float,
                        help="run CmdStan first per seed; cap stanli at this multiple of its runtime, "
                             "also bounded by --sample-timeout")
    parser.add_argument("--seeds", type=int, nargs="+", default=[1, 2, 3, 4])
    parser.add_argument("--iter-warmup", type=int, default=1000)
    parser.add_argument("--iter-sampling", type=int, default=1000)
    parser.add_argument("--resume", action="store_true")
    parser.add_argument("--bench", type=pathlib.Path, default=BENCH)
    parser.add_argument("--run", type=pathlib.Path, default=RUN)
    parser.add_argument("--stanc", type=pathlib.Path, default=STANC)
    args = parser.parse_args(argv)
    if args.cmdstan_runtime_multiple is not None:
        if not args.sampling:
            parser.error("--cmdstan-runtime-multiple requires --sampling")
        if not math.isfinite(args.cmdstan_runtime_multiple) or args.cmdstan_runtime_multiple <= 0:
            parser.error("--cmdstan-runtime-multiple must be finite and positive")
    if args.rounds < 2 or args.rounds % 2:
        parser.error("--rounds must be even and at least two for balanced engine order")
    if not all(1 <= n <= 60000 for n in (args.warmup_ms, args.measure_ms)):
        parser.error("timing windows must be 1..60000 milliseconds")
    if not all(math.isfinite(n) and n > 0 for n in
               (args.gradient_timeout, args.build_timeout, args.sample_timeout)):
        parser.error("phase timeouts must be finite and positive")
    if args.gradient_timeout <= (args.warmup_ms + args.measure_ms) / 1000:
        parser.error("gradient timeout must exceed warmup plus measurement")
    if args.iter_warmup < 0 or args.iter_sampling < 1 or any(s < 1 for s in args.seeds):
        parser.error("invalid sampling iterations or seeds")
    if args.sampling and (len(args.seeds) < 2 or len(args.seeds) % 2 or
                          len(set(args.seeds)) != len(args.seeds)):
        parser.error("sampling requires an even number of distinct seeds for balanced order")
    for key in ("cmdstan", "pdb", "output", "bench", "run", "stanc"):
        setattr(args, key, getattr(args, key).absolute())
    if any(os.getenv(k, "0") != "0" for k in ("STANLI_PROFILE", "STANLI_PROFILE_PREP")):
        parser.error("disable profiling for comparative measurements")
    # Resolve inputs in a separate temporary cache before freezing their identities.
    import tempfile
    with tempfile.TemporaryDirectory(prefix="stanli-bench-inputs-") as cache:
        inputs = {}
        for name, dname in sorted(model_pairs(args.pdb / "posterior_database", args.corpus).items()):
            if args.filter and args.filter not in name:
                continue
            stan, data = model_files(name, {"data": dname}, args.pdb / "posterior_database", pathlib.Path(cache))
            inputs[name] = (stan, data)
        if not inputs:
            parser.error("no models selected")
        config = {k: str(v) if isinstance(v, pathlib.Path) else v for k, v in vars(args).items()
                  if k not in ("resume", "output")}
        identities = {str(p.relative_to(REPO)): sha(p) for p in (
            REPO / "harnesses/corpus_bench.py", REPO / "tools/benchmark_timer.hpp",
            REPO / "tools/bench_grad.cpp", REPO / "tools/bench_cmdstan_grad.cpp",
            REPO / "tools/cmdstan_ref.py", REPO / "tools/verify_refs.py")}
        expected = dict(protocol=PROTOCOL, config=config,
                        machine=dict(platform=platform.platform(), host=platform.node(),
                                     processor=platform.processor(), logical_cpus=os.cpu_count()),
                        threads=THREAD_ENV, sources=identities,
                        executables={k: sha(getattr(args, k)) for k in
                                     (("bench", "stanc", "run") if args.sampling else ("bench", "stanc"))},
                        toolchains={"cmdstan": checkout_identity(args.cmdstan),
                                    "stan": checkout_identity(args.cmdstan / "stan"),
                                    "math": checkout_identity(args.cmdstan / "stan/lib/stan_math"),
                                    "libraries": library_identity(args.cmdstan),
                                    "clang_sha256": sha(shutil.which("clang++")),
                                    "build_environment": {k: os.getenv(k) for k in
                                        ("CC", "CXX", "CFLAGS", "CXXFLAGS", "CPPFLAGS", "LDFLAGS", "MAKEFLAGS")}},
                        inputs={m: {"stan": sha(s), "data": sha(d)} for m, (s, d) in inputs.items()})
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
        return int(any(r["status"] == "failed" for r in records.values()))


if __name__ == "__main__":
    raise SystemExit(main())
