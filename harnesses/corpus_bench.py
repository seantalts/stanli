#!/usr/bin/env python3
"""Paired, reproducible corpus benchmarks. See docs/benchmarks.md.

python3 harnesses/corpus_bench.py CMDSTAN PDB fresh.tsv [--corpus rethinking]
Measures source/setup costs and warm gradients. Setup plus 20,000 gradients
is a fixed-work proxy, not measured HMC or sampling time.
A sibling fresh.tsv.run directory holds the frozen manifest and raw evidence.
--resume requires identical inputs, binaries and settings. Historical TSVs are
never appended to, and candidate-only refreshes cannot create paired results.
CmdStan builds finish before any timing: stanc and make run one at a time, gradient
drivers in parallel (--build-jobs). Builds are reused across runs from --build-cache;
--no-build-cache builds inside the run directory.
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
from concurrent.futures import ThreadPoolExecutor, as_completed
import statistics
import subprocess
import sys
import time
import threading

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools"))
import benchmark_points
from bench_build_cache import BuildCache, cache_key
from cmdstan_ref import compile_cmd
from corpus_inventory import corpus_cases

PROTOCOL = "stanli-corpus-v6"
GRADIENT_BUDGET = 20000
CACHE_STEM = "model"
DEFAULT_CACHE = pathlib.Path("~/.cache/stanli/corpus-bench")
BENCH = REPO / "build-rel/bench_grad"
STANC = REPO / "deps/stanc3/stanc"
VECTORIZE_PROBE = REPO / "deps/stanc3/stanli-vectorize-probe"
COLS = ["model", "params", "stanli_compile_s", "stanli_prep_s",
        "cmdstan_stanc_s", "cmdstan_build_s", "stanli_ns_grad", "cmdstan_ns_grad",
        "stanli_ns_grad_mad", "cmdstan_ns_grad_mad", "paired_speedup",
        "paired_speedup_mad", "paired_rounds", "points_kept", "points_dropped",
        "point_source", "gradient_budget",
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
        self.lock = threading.Lock()
        self.active = set()
        recorded = ([json.loads(line)["id"] for line in self.events.read_text().splitlines()]
                    if self.events.exists() else [])
        self.next_id = max(recorded, default=-1) + 1

    def kill_active(self):
        with self.lock:
            processes = list(self.active)
        for process in processes:
            try:
                if os.name == "posix":
                    os.killpg(process.pid, signal.SIGKILL)
                else:
                    process.kill()
            except (ProcessLookupError, PermissionError):
                pass

    def run(self, phase, argv, timeout, cwd=None):
        with self.lock:
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
                with self.lock:
                    self.active.add(process)
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
                    with self.lock:
                        self.active.discard(process)
                    timer.cancel()
                    timer.join()
            except OSError as exc:
                record.update(status="failed", returncode=None, error=str(exc))
            finally:
                record["elapsed_s"] = time.perf_counter() - start
                with self.lock, self.events.open("a") as events:
                    events.write(json.dumps(record, allow_nan=False) + "\n")
        return record

    def text(self, result, stream="stdout"):
        return (self.directory / result[stream]).read_text(errors="replace")

    def require(self, phase, argv, timeout, cwd=None):
        result = self.run(phase, argv, timeout, cwd)
        if result["status"] != "ok":
            raise PhaseFailure(f"{phase}: {result['status']} (event {result['id']})")
        return result


def parse_timing(text, warmup_ms, measure_ms, points):
    # Stan transformed-data print statements can precede the driver's JSON.
    lines = [line for line in text.splitlines() if line.strip()]
    try:
        result = json.loads(lines[-1])
        if result["protocol"] != "stanli-gradient-v3":
            raise ValueError("wrong timing protocol")
        for key in ("warmup_iterations", "warmup_elapsed_ns"):
            if type(result[key]) is not int or result[key] <= 0:
                raise ValueError(f"invalid {key}")
        if result["warmup_elapsed_ns"] < warmup_ms * 1e6:
            raise ValueError("warmup ended early")
        entries = result["points"]
        if not isinstance(entries, list) or len(entries) != points:
            raise ValueError(f"expected {points} points")
        for index, entry in enumerate(entries):
            if entry["point"] != index or type(entry["finite"]) is not bool:
                raise ValueError(f"invalid point {index}")
            if not entry["finite"]:
                if set(entry) != {"point", "finite"}:
                    raise ValueError(f"point {index} has measurements but is not finite")
                continue
            for key in ("iterations", "elapsed_ns", "batch"):
                if type(entry[key]) is not int or entry[key] <= 0:
                    raise ValueError(f"point {index}: invalid {key}")
            if entry["elapsed_ns"] < measure_ms * 1e6:
                raise ValueError(f"point {index}: measurement ended early")
            values = entry["values"]
            if not isinstance(values, list) or not values:
                raise ValueError(f"point {index}: missing density and gradient")
            if not all(type(v) in (int, float) and math.isfinite(v) for v in values):
                raise ValueError(f"point {index}: non-finite density or gradient")
        if not any(entry["finite"] for entry in entries):
            raise ValueError("no finite point")
    except (ValueError, KeyError, IndexError, TypeError) as exc:
        raise PhaseFailure(f"invalid timing output: {exc}") from exc
    return result


def kept_points(pair):
    return [a["point"] for a, b in zip(pair["stanli"]["points"], pair["cmdstan"]["points"])
            if a["finite"] and b["finite"]]


def check_pair(pair):
    kept, dropped, errors, width = [], [], [], None
    for a, b in zip(pair["stanli"]["points"], pair["cmdstan"]["points"]):
        k = a["point"]
        if not a["finite"] and not b["finite"]:
            dropped.append(k)
            continue
        if a["finite"] != b["finite"]:
            bad, good = ("stanli", "cmdstan") if b["finite"] else ("cmdstan", "stanli")
            raise PhaseFailure(f"point {k} is non-finite in {bad} but finite in {good}")
        if len(a["values"]) != len(b["values"]):
            raise PhaseFailure(f"point {k}: density/gradient widths differ")
        error = max(abs(x - y) / max(abs(x), abs(y), 1) for x, y in zip(a["values"], b["values"]))
        if error > 1e-9:
            raise PhaseFailure(f"point {k}: density/gradient mismatch: scaled error {error:.3g}")
        kept.append(k)
        errors.append(error)
        width = len(a["values"])
    if not kept:
        raise PhaseFailure("no point is finite in both engines")
    return dict(kept_points=kept, dropped_points=dropped, scaled_errors=errors,
                max_scaled_error=max(errors), width=width)


def strip_values(pair):
    for engine in ("stanli", "cmdstan"):
        for point in pair[engine]["points"]:
            point.pop("values", None)


def mean_ns(result, kept):
    entries = {entry["point"]: entry for entry in result["points"]}
    return statistics.fmean(entries[k]["elapsed_ns"] / entries[k]["iterations"] for k in kept)


def summarize_pairs(pairs):
    times = {engine: [mean_ns(p[engine], kept_points(p)) for p in pairs]
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
        raise PhaseFailure(failure_note(phase, event))
    return event


def failure_note(phase, event):
    return f"{phase}: {event['status']} (event {event['id']})"


def stanli_compile(name, stan, work, runner, args, setup):
    work.mkdir(exist_ok=True)
    source, mir = work / f"{name}.stan", work / "model.sexp"
    source.write_bytes(stan.read_bytes())
    fast = ["--fast-math"] if getattr(args, "fast_math", False) else []
    setup_phase(runner, setup, "stanli_compile", f"{name}/stanli-mir",
        [args.vectorize_probe, "--vectorize-loops", "on", *fast, "--output", mir, source],
        args.build_timeout)
    return mir, source


class CmdstanCache:
    def __init__(self, store, context, run_id):
        self.store, self.context, self.run_id = store, context, run_id

    def key(self, stan):
        return cache_key(self.context, sha(stan))


def cached_build(name, key, meta, cache):
    reused = dict(status="ok", returncode=0, cached=True, cache_key=key, cache_run_id=meta["run_id"])
    entry = cache.store.entry(key)
    return dict(
        setup={"cmdstan_stanc": dict(reused, phase=f"{name}/stanc-cpp", elapsed_s=meta["stanc_s"]),
               "cmdstan_build": dict(reused, phase=f"{name}/cmdstan-build", elapsed_s=meta["build_s"])},
        gradient=entry / "gradbench", executable=entry / CACHE_STEM,
        gradient_failure=None, build_failure=None,
        cache=dict(directory=str(cache.store.root), key=key, hit=True, run_id=meta["run_id"]))


class CmdstanJob:
    """One model's CmdStan build in stages, so a caller can serialize the timed ones."""
    def __init__(self, name, stan, work, cache=None):
        self.name, self.stan, self.work, self.cache = name, stan, work, cache
        self.scratch = None
        work.mkdir(parents=True, exist_ok=True)
        if cache is None:
            self.directory, self.stem = work, name
            self.build = dict(setup={}, gradient=None, executable=None, gradient_failure=None,
                              build_failure=None, cache=dict(directory=None, hit=False))
            (work / f"{name}.stan").write_bytes(stan.read_bytes())
            return
        self.key = cache.key(stan)
        meta = cache.store.lookup(self.key)
        if meta is not None:
            self.build = cached_build(name, self.key, meta, cache)
            return
        self.scratch = self.directory = cache.store.scratch(self.key)
        self.stem = CACHE_STEM
        self.build = dict(setup={}, gradient=None, executable=None, gradient_failure=None,
                          build_failure=None, cache=dict(directory=str(cache.store.root),
                                                         key=self.key, hit=False))
        (self.directory / f"{CACHE_STEM}.stan").write_bytes(stan.read_bytes())

    @property
    def hit(self):
        return self.build["cache"]["hit"]

    def stanc(self, runner, args):
        phase = f"{self.name}/stanc-cpp"
        event = self.build["setup"]["cmdstan_stanc"] = runner.run(
            phase, [args.stanc, f"{self.stem}.stan", f"--o={self.stem}.hpp", *shlex.split(args.stancflags)],
            args.build_timeout, self.directory)
        if event["status"] != "ok":
            self.build["gradient_failure"] = failure_note(phase, event)

    def header_ready(self):
        return self.build["setup"].get("cmdstan_stanc", {}).get("status") == "ok"

    def driver(self, runner, args):
        phase = f"{self.name}/gradient-driver-build"
        event = runner.run(phase, driver_command(args.cmdstan.resolve(), f"{self.stem}.hpp",
                                                 REPO / "tools/bench_cmdstan_grad.cpp", "gradbench"),
                           args.build_timeout, self.directory)
        self.build["driver_build_s"] = event["elapsed_s"]
        if event["status"] == "ok":
            self.build["gradient"] = self.directory / "gradbench"
        else:
            self.build["gradient_failure"] = failure_note(phase, event)

    def make(self, runner, args):
        phase = f"{self.name}/cmdstan-build"
        executable = self.directory / self.stem
        event = self.build["setup"]["cmdstan_build"] = runner.run(
            phase, make_command(executable, args.stanc, args.stancflags), args.build_timeout,
            args.cmdstan.resolve())
        if event["status"] == "ok":
            self.build["executable"] = executable
        else:
            self.build["build_failure"] = failure_note(phase, event)

    def finish(self):
        """Publish a complete build into the cache; keep an incomplete one in the run."""
        build = self.build
        if self.scratch is None:
            return build
        store, scratch = self.cache.store, self.scratch
        try:
            if build["gradient_failure"] or build["build_failure"]:
                kept = self.work / "cmdstan-build"
                shutil.rmtree(kept, ignore_errors=True)
                shutil.move(str(scratch), kept)
                for field in ("gradient", "executable"):
                    if build[field]:
                        build[field] = kept / build[field].name
                return build
            setup = build["setup"]
            files = [f"{CACHE_STEM}.stan", f"{CACHE_STEM}.hpp", "gradbench", CACHE_STEM]
            meta = dict(files=files, run_id=self.cache.run_id, key=self.key,
                        created_utc=time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                        stanc_s=setup["cmdstan_stanc"]["elapsed_s"], build_s=setup["cmdstan_build"]["elapsed_s"],
                        driver_build_s=build["driver_build_s"],
                        driver_sha256=sha(scratch / "gradbench"), executable_sha256=sha(scratch / CACHE_STEM))
            if not store.commit(scratch, self.key, meta):
                build["cache"]["raced"] = True
            entry = store.entry(self.key)
            build["gradient"], build["executable"] = entry / "gradbench", entry / CACHE_STEM
            return build
        finally:
            store.discard(scratch)

    def abort(self):
        if self.scratch is not None:
            self.cache.store.discard(self.scratch)


def cmdstan_build(name, stan, work, runner, args, cache=None):
    """Build the CmdStan binaries for one model, or reuse the cached ones."""
    job = CmdstanJob(name, stan, work, cache)
    try:
        if not job.hit:
            job.stanc(runner, args)
            if job.header_ready():
                job.driver(runner, args)
                job.make(runner, args)
        return job.finish()
    except BaseException:
        job.abort()
        raise


def prebuild(inputs, directory, runner, args, cache=None):
    """Build every CmdStan binary before any timing starts; a failure stays with its model.

    The stanc and make durations are published, so those commands run one at a
    time with nothing else building. Only the unpublished gradient drivers run in parallel.
    """
    leaders, twins, seen = [], [], set()
    for name, (stan, _) in inputs.items():
        try:
            key = cache.key(stan) if cache else name
        except OSError:
            key = name
        (twins if key in seen else leaders).append(name)
        seen.add(key)

    builds, jobs = {}, {}

    def broken(name, exc):
        builds[name] = dict(setup={}, gradient=None, executable=None, build_failure=None,
                            gradient_failure=f"{name}/cmdstan-build: error ({exc})",
                            cache=dict(directory=str(cache.store.root) if cache else None, hit=False))

    def guarded(name, step):
        try:
            step(jobs[name])
        except Exception as exc:
            jobs.pop(name).abort()
            broken(name, exc)

    def report(name):
        build = builds[name]
        failed = build["gradient_failure"] or build["build_failure"]
        print(f"{name}: cmdstan build {'failed' if failed else 'cached' if build['cache']['hit'] else 'built'}",
              flush=True)

    pool = ThreadPoolExecutor(max_workers=args.build_jobs)
    try:
        for wave in (leaders, twins):
            for name in wave:
                try:
                    jobs[name] = CmdstanJob(name, inputs[name][0], directory / name, cache)
                except Exception as exc:
                    broken(name, exc)
            misses = [n for n in wave if n in jobs and not jobs[n].hit]
            for name in misses:
                guarded(name, lambda job: job.stanc(runner, args))
            ready = [n for n in misses if n in jobs and jobs[n].header_ready()]
            for future in as_completed([pool.submit(guarded, n, lambda job: job.driver(runner, args))
                                        for n in ready]):
                future.result()
            for name in ready:
                if name in jobs:
                    guarded(name, lambda job: job.make(runner, args))
            for name in wave:
                if name in jobs:
                    guarded(name, lambda job: builds.__setitem__(name, job.finish()))
                report(name)
    except BaseException:
        pool.shutdown(wait=False, cancel_futures=True)
        runner.kill_active()
        for job in jobs.values():
            job.abort()
        raise
    finally:
        pool.shutdown(wait=True)
    return {name: builds[name] for name in inputs}


def build_model(name, stan, data, work, runner, args, setup=None, build=None):
    setup = {} if setup is None else setup
    build = {} if build is None else build
    mir, source = stanli_compile(name, stan, work, runner, args, setup)
    if not build:
        build.update(cmdstan_build(name, stan, work, runner, args))
    if "cmdstan_stanc" in build["setup"]:
        setup["cmdstan_stanc"] = build["setup"]["cmdstan_stanc"]
    if build["gradient_failure"]:
        raise PhaseFailure(build["gradient_failure"])
    executables = {"stanli": [args.bench, mir, data], "cmdstan": [build["gradient"], data]}
    return executables, source


def driver_command(cmdstan, hpp, driver, gradient):
    return compile_cmd(cmdstan, hpp, driver, gradient, opt="-O3")


def make_command(exe, stanc, stancflags):
    return ["make", exe, f"STANC={stanc}", f"STANCFLAGS={stancflags}"]


def cache_context(args, expected):
    """Everything besides the .stan bytes that determines the CmdStan binaries."""
    cmdstan = args.cmdstan.resolve()
    sources = expected["sources"]
    return dict(
        stanc_sha256=expected["executables"]["stanc"], stancflags=args.stancflags,
        driver_sha256=sources["tools/bench_cmdstan_grad.cpp"],
        timer_sha256=sources["tools/benchmark_timer.hpp"],
        toolchains={k: v for k, v in expected["toolchains"].items() if not k.startswith("stanli_")},
        cmdstan=str(cmdstan),
        driver_command=list(map(str, driver_command(cmdstan, f"{CACHE_STEM}.hpp", "{driver}", "gradbench"))),
        make_command=list(map(str, make_command("{exe}", "{stanc}", args.stancflags))))


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


def prior_point_files(inputs, document, points, directory):
    """Write each prior-point model's vectors to directory; other models keep the fixed points."""
    found = {}
    if document is None:
        return found
    for name, (stan, data) in sorted(inputs.items()):
        vectors = benchmark_points.points_for(document, name, sha(stan), sha(data),
                                              pathlib.Path(stan).read_text())
        if vectors is None:
            continue
        if len(vectors) != points:
            raise ValueError(f"{name}: the points file has {len(vectors)} points but --points is {points}")
        pathlib.Path(directory).mkdir(parents=True, exist_ok=True)
        found[name] = pathlib.Path(directory) / f"{name}.points.json"
        found[name].write_text(json.dumps(vectors, allow_nan=False))
    return found


def run_config(args):
    config = {k: str(v) if isinstance(v, pathlib.Path) else v for k, v in vars(args).items()
              if k not in ("resume", "output", "build_cache", "no_build_cache", "build_jobs",
                           "prior_points", "no_prior_points")}
    if not config.get("fast_math"):
        config.pop("fast_math", None)
    return config


def measure_model(name, stan, data, directory, manifest, runner, args, build=None, points=None):
    source = "prior" if points else "fixed"
    row = dict(model=name, run_id=manifest["run_id"], gradient_budget=GRADIENT_BUDGET,
               point_source=source)
    inputs = {"stan": sha(stan), "data": sha(data), "point_source": source}
    if points:
        inputs["points_sha256"] = sha(points)
    record = dict(model=name, inputs=inputs, row=row,
                  gradients=[], setup={}, preparation_s=[], status="failed")
    notes, build = [], {} if build is None else build
    fast = ["--fast-math"] if getattr(args, "fast_math", False) else []
    source = directory / name / f"{name}.stan"
    try:
        commands, source = build_model(name, stan, data, directory / name, runner, args,
                                       record["setup"], build)
        record["executables"] = {engine: sha(command[0]) for engine, command in commands.items()}
        for round_index in range(args.rounds):
            pair = {}
            order = paired_order(round_index)
            for engine in order:
                event = runner.require(f"{name}/gradient/{round_index}/{engine}",
                    commands[engine] + ["--timed", "--warmup-ms", str(args.warmup_ms),
                                        "--measure-ms", str(args.measure_ms),
                                        *(["--points-file", str(points)] if points
                                          else ["--points", str(args.points)])]
                    + (fast if engine == "stanli" else []), args.gradient_timeout)
                pair[engine] = parse_timing(runner.text(event), args.warmup_ms, args.measure_ms,
                                             args.points)
                pair[engine]["event"] = event["id"]
            checked = check_pair(pair)
            strip_values(pair)
            record["gradients"].append(dict(pair, order=list(order), **checked))
        if any(g["kept_points"] != record["gradients"][0]["kept_points"] for g in record["gradients"]):
            raise PhaseFailure("points differ between rounds")
        first = record["gradients"][0]
        row.update(summarize_pairs(record["gradients"]))
        row["points_kept"] = len(first["kept_points"])
        row["points_dropped"] = len(first["dropped_points"])
        row["params"] = first["width"] - 1
        # Binding existing MIR excludes the separately measured source compiler.
        for i in range(args.rounds):
            event = runner.require(f"{name}/prepare/{i}",
                                   commands["stanli"] + ["--prep"] + fast,
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
    if record["setup"].get("cmdstan_stanc", {}).get("status") == "ok" and "cmdstan_build" in build["setup"]:
        record["setup"]["cmdstan_build"] = build["setup"]["cmdstan_build"]
        if build["build_failure"]:
            notes.append(build["build_failure"])
        else:
            try:
                record["cmdstan_executable_sha256"] = sha(build["executable"])
            except (OSError, ValueError) as exc:
                notes.append(str(exc))
    record["build_cache"] = build.get("cache")
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
    parser.add_argument("--corpus", choices=("all", "posteriordb", "educational", "rethinking", "brms", "cogmod", "ctsem", "teaching"), default="all")
    parser.add_argument("--filter", default="")
    parser.add_argument("--rounds", type=int, default=6)
    parser.add_argument("--warmup-ms", type=int, default=20)
    parser.add_argument("--measure-ms", type=int, default=40,
                        help="measured window per evaluation point")
    parser.add_argument("--points", type=int, default=10,
                        help="evaluation points; gradient time is their mean")
    parser.add_argument("--prior-points", type=pathlib.Path, default=benchmark_points.DEFAULT_PATH,
                        help="evaluation points drawn from the prior of brms-style models; "
                             "other models use the fixed points")
    parser.add_argument("--no-prior-points", action="store_true",
                        help="use the fixed points for every model")
    parser.add_argument("--gradient-timeout", type=float, default=60)
    parser.add_argument("--build-timeout", type=float, default=900)
    parser.add_argument("--resume", action="store_true")
    parser.add_argument("--bench", type=pathlib.Path, default=BENCH)
    parser.add_argument("--stanc", "--cmdstan-stanc", type=pathlib.Path, default=STANC,
                        help="compiler for the CmdStan model header")
    parser.add_argument("--stancflags", default="", help="flags for CmdStan header generation")
    parser.add_argument("--vectorize-probe", type=pathlib.Path, default=VECTORIZE_PROBE)
    parser.add_argument("--fast-math", action="store_true",
                        help="build stanli MIR in fast mode and run bench_grad --fast-math")
    parser.add_argument("--build-cache", type=pathlib.Path,
                        help="directory of reusable CmdStan builds "
                             f"(default: $STANLI_BENCH_CACHE or {DEFAULT_CACHE})")
    parser.add_argument("--no-build-cache", action="store_true",
                        help="build every CmdStan binary inside the run directory")
    parser.add_argument("--build-jobs", type=int, default=8,
                        help="gradient driver builds to run at once before timing starts")
    args = parser.parse_args(argv)
    if args.build_cache and args.no_build_cache:
        parser.error("--build-cache and --no-build-cache are exclusive")
    if args.build_jobs < 1:
        parser.error("--build-jobs must be at least one")
    if args.rounds < 2 or args.rounds % 2:
        parser.error("--rounds must be even and at least two for balanced engine order")
    if not all(1 <= n <= 60000 for n in (args.warmup_ms, args.measure_ms)):
        parser.error("timing windows must be 1..60000 milliseconds")
    if not all(math.isfinite(n) and n > 0 for n in
               (args.gradient_timeout, args.build_timeout)):
        parser.error("phase timeouts must be finite and positive")
    if not 1 <= args.points <= 100:
        parser.error("--points must be 1..100")
    if args.gradient_timeout <= (args.warmup_ms + args.points * (args.measure_ms + 2)) / 1000:
        parser.error("gradient timeout must exceed warmup plus measurement")
    for key in ("cmdstan", "pdb", "output", "bench", "stanc", "vectorize_probe", "prior_points"):
        setattr(args, key, getattr(args, key).absolute())
    if any(os.getenv(k, "0") != "0" for k in ("STANLI_PROFILE", "STANLI_PROFILE_PREP")):
        parser.error("disable profiling for comparative measurements")
    # Compiler and runtime overrides change which code is measured. Require
    # explicit tool arguments rather than inherit a shell's experiment state.
    overrides = [k for k, v in os.environ.items() if v and
                 (k in ("STANC", "STANLI_COMPILE") or
                  k.startswith(("STANLI_NO_", "STANLI_PROFILE", "STANLI_LITE_",
                                "STANLI_BOUNDED_", "STANLI_STRUCTURED_",
                                "STANLI_ISLAND_",
                                "STANLI_WA_", "STANLI_PACKET_",
                                "STANLI_DEBUG_", "STANLI_DUMP_")))]
    if overrides:
        parser.error("unset benchmark environment overrides: " + ", ".join(sorted(overrides)))
    if args.no_build_cache:
        args.build_cache = None
    else:
        args.build_cache = pathlib.Path(args.build_cache or os.getenv("STANLI_BENCH_CACHE")
                                        or DEFAULT_CACHE).expanduser().absolute()
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
        try:
            prior_document = None if args.no_prior_points else benchmark_points.load(args.prior_points)
            staged_points = prior_point_files(inputs, prior_document, args.points,
                                              pathlib.Path(cache) / "points")
        except (OSError, ValueError) as exc:
            parser.error(str(exc))
        config = run_config(args)
        config["gradient_budget"] = GRADIENT_BUDGET
        identities = {p.relative_to(REPO).as_posix(): sha(p) for p in (
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
                                    "collection": cases[m].collection,
                                    **({"point_source": "prior", "points_sha256": sha(staged_points[m])}
                                       if m in staged_points else {"point_source": "fixed"})}
                                for m, (s, d) in inputs.items()},
                        prior_points_sha256=None if prior_document is None else sha(args.prior_points))
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
        point_files = {}
        for name, staged in staged_points.items():
            copied = directory / "inputs" / f"{name}.points.json"
            if copied.exists() and sha(copied) != sha(staged):
                parser.error(f"retained input was modified: {copied}")
            copied.write_bytes(staged.read_bytes())
            point_files[name] = copied
        runner, records = Runner(directory), {}
        for name in inputs:
            saved = directory / f"{name}.result.json"
            if args.resume and saved.exists():
                records[name] = json.loads(saved.read_text())
        pending = {name: copies for name, copies in inputs.items() if name not in records}
        cache = (CmdstanCache(BuildCache(args.build_cache), cache_context(args, expected),
                              manifest["run_id"]) if args.build_cache else None)
        started = time.perf_counter()
        builds = prebuild(pending, directory, runner, args, cache)
        with (directory / "build-prepass.jsonl").open("a") as stream:
            stream.write(json.dumps(dict(
                started_utc=time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), jobs=args.build_jobs,
                cache_directory=str(args.build_cache) if cache else None,
                wall_s=time.perf_counter() - started,
                models={name: dict(hit=build["cache"]["hit"], key=build["cache"].get("key"))
                        for name, build in builds.items()}), allow_nan=False) + "\n")
        for name, (stan, data) in inputs.items():
            if name not in records:
                print(f"{name}: paired gradient measurements", flush=True)
                records[name] = measure_model(name, stan, data, directory, manifest, runner, args,
                                              builds[name], point_files.get(name))
            write_summary(args.output, records)
            print(f"{name}: {records[name]['status']} {records[name]['row']['note']}", flush=True)
        return int(any(r["status"] != "ok" for r in records.values()))


if __name__ == "__main__":
    raise SystemExit(main())
