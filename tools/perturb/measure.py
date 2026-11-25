"""Run stanli's tools and CmdStan's gradient driver on one model and data.

The functions here wrap one tool call each and return plain dicts, so the
stages in run.py and ladder.py only decide what to run and how to judge it.
Every source is compiled once and cached by its text, since the MIR does not
depend on the data.
"""
from __future__ import annotations

import hashlib
import json
import math
import os
import pathlib
import re
import shutil
import statistics
import subprocess
import sys
import tempfile
import threading
import time

REPO = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools"))
sys.path.insert(0, str(REPO / "tools" / "fuzz_stanc"))
import benchmark_points  # noqa: E402
import check_status  # noqa: E402
import cmdstan_ref  # noqa: E402
import differential  # noqa: E402
import toolchain  # noqa: E402
from bench_select import NO_WRITE_ARRAY, opcode_counts  # noqa: E402
from lane_declines import first_per_position  # noqa: E402

THREAD_ENV = {k: "1" for k in ("STAN_NUM_THREADS", "OMP_NUM_THREADS",
                               "OPENBLAS_NUM_THREADS", "MKL_NUM_THREADS",
                               "VECLIB_MAXIMUM_THREADS")}
ALL_OPS = "1000000000"
REGION_OPS = ("LOOP", "REGION_MAP", "ISLAND")
# Executor::program_bytes (runtime/src/executor.cpp): per op an Op (80 B),
# a KernelCtx (336 B), a forward entry (8 B) and, unless the graph is
# forward only, a BwdStep (24 B). write_array graphs are forward only.
LOG_PROB_OP_BYTES = 80 + 336 + 8 + 24
WRITE_ARRAY_OP_BYTES = 80 + 336 + 8
# Values and adjoints of every slot element.
SLOT_ELEM_BYTES = 16


def sha_text(text):
    return hashlib.sha256(text.encode()).hexdigest()


def sha_file(path):
    return hashlib.sha256(pathlib.Path(path).read_bytes()).hexdigest()


def clean_env(extra=None):
    """The environment for every tool: one thread, no inherited STANLI_
    switches, which would change what is measured."""
    env = {k: v for k, v in os.environ.items()
           if not k.startswith("STANLI_") and k not in ("STANC",)}
    env.update(THREAD_ENV)
    env.update(extra or {})
    return env


# Each tool process is killed when its resident memory passes this many
# bytes (None: no limit). macOS does not enforce RLIMIT_AS, so run() polls.
MEMORY_LIMIT = None
POLL_S = 0.5


def rss_bytes(pid):
    r = subprocess.run(["ps", "-o", "rss=", "-p", str(pid)],
                       capture_output=True, text=True)
    return int(r.stdout.strip() or 0) * 1024


def run(argv, timeout, env=None, cwd=None):
    """(returncode, stdout, stderr, seconds). returncode is None when the
    process was killed, and stderr then says why: "timeout" or "memory
    limit"."""
    started = time.monotonic()
    proc = subprocess.Popen([str(a) for a in argv], stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, text=True,
                            env=env or clean_env(), cwd=cwd)
    killed = None
    while True:
        try:
            out, err = proc.communicate(timeout=POLL_S)
            break
        except subprocess.TimeoutExpired:
            elapsed = time.monotonic() - started
            if elapsed > timeout:
                killed = "timeout"
            elif MEMORY_LIMIT and rss_bytes(proc.pid) > MEMORY_LIMIT:
                killed = f"memory limit ({MEMORY_LIMIT / 2**30:.1f} GB)"
            if killed:
                proc.kill()
                proc.communicate()
                return None, "", killed, time.monotonic() - started
    return proc.returncode, out, err, time.monotonic() - started


def ratio_text(r):
    """A time ratio new/old as "N.NNx slower" or "N.NNx faster"."""
    if r is None or not math.isfinite(r) or r <= 0:
        return "-"
    return f"{r:.2f}x slower" if r >= 1 else f"{1 / r:.2f}x faster"


def normalized_text(r):
    """stanli's slowdown over CmdStan's, said without a ratio below 1."""
    if r is None or not math.isfinite(r) or r <= 0:
        return "-"
    if r >= 1:
        return f"stanli slows {r:.2f}x more"
    return f"CmdStan slows {1 / r:.2f}x more"


def failure_text(code, err):
    if code is None:
        return err if err.startswith("memory limit") else "timeout"
    lines = [x for x in err.strip().splitlines() if x.strip()]
    tail = lines[-1][:300] if lines else ""
    if "bad_alloc" in err:
        return f"bad_alloc: {tail}"
    if code < 0:
        return f"crash (signal {-code}): {tail}"
    return f"exit {code}: {tail}"


class Tools:
    """Paths of the tools and the caches in OUT/cache."""

    def __init__(self, build, out, timeout=300.0, cmdstan=None, stanc=None):
        self.build = pathlib.Path(build).resolve()
        self.probe = probe_path()
        self.check = self.build / "stanli_check"
        self.bench = self.build / "bench_grad"
        self.dump_ops = self.build / "dump_ops"
        self.dump_islands = self.build / "dump_islands"
        self.cmdstan = pathlib.Path(cmdstan or REPO / "deps" / "cmdstan").resolve()
        self.stanc = pathlib.Path(stanc or toolchain.PINNED_STANC)
        self.cache = pathlib.Path(out).resolve() / "cache"
        self.timeout = timeout
        self.locks = {}
        self.lock = threading.Lock()
        for tool in (self.probe, self.check, self.bench, self.dump_ops,
                     self.dump_islands):
            if not tool.exists():
                raise SystemExit(f"missing {tool}")

    def key_lock(self, key):
        with self.lock:
            return self.locks.setdefault(key, threading.Lock())


def probe_path():
    embed = subprocess.check_output(
        ["bash", "-c", "source tools/stanc_embed/provenance.sh && stanc_embed_dir"],
        cwd=REPO, text=True).strip()
    return pathlib.Path(embed) / "stanli-vectorize-probe"


# ---------------------------------------------------------------------------
# stanli


def compile_source(tools, text):
    """Compile with the shipped configuration (the embedded compiler with
    loop vectorization on, as the corpus benchmark does). The source is
    written as model.stan so variant names do not reach generated names."""
    key = sha_text(text)
    work = tools.cache / "mir" / key[:16]
    with tools.key_lock(("mir", key)):
        done = work / "compile.json"
        if done.exists():
            return json.loads(done.read_text())
        work.mkdir(parents=True, exist_ok=True)
        (work / "model.stan").write_text(text)
        code, _, err, seconds = run(
            [tools.probe, "--vectorize-loops", "on", "--output",
             work / "model.sexp", work / "model.stan"], tools.timeout)
        result = {"ok": code == 0, "seconds": seconds,
                  "stan": str(work / "model.stan"),
                  "mir": str(work / "model.sexp")}
        if code != 0:
            result["error"] = (err.strip() or failure_text(code, err))[-600:]
        done.write_text(json.dumps(result))
        return result


def check(tools, comp, data, points=differential.POINTS, status=None):
    """stanli_check at `points`: differential.run_outcome's (state, payload)
    and the status records."""
    if status is None:
        fd, status = tempfile.mkstemp(suffix=".jsonl",
                                      dir=pathlib.Path(comp["mir"]).parent)
        os.close(fd)
    status = pathlib.Path(status)
    status.unlink(missing_ok=True)
    code, out, err, _ = run(
        [tools.check, comp["stan"], data, "--mir", comp["mir"], "--points",
         ",".join(map(str, points)), "--status-json", status], tools.timeout)
    records = check_status.read(status)
    status.unlink(missing_ok=True)
    raw = None if code is None else (code, out, err)
    return differential.run_outcome(raw, records), records


def finite_values(outcome, points=differential.POINTS):
    """None when every point gave a finite lp and gradient, else why not."""
    state, payload = outcome
    if state != "points":
        return f"{state}: {payload}" if payload else state
    for k in points:
        s = payload["points"][k]["status"]
        if s is None:
            return f"point {k}: no output"
        if s[0] != "OK":
            return f"point {k}: {s[0]} {s[1]}"[:300]
        if not all(math.isfinite(float(v)) for v in s[1]):
            return f"point {k}: non-finite lp or gradient"
    return None


def parse_prep_trace(text):
    """The STANLI_PROFILE_PREP rows we use: per graph, the op count after
    lowering (before the passes), the final op count and slot elements."""
    rows = {}
    for line in text.splitlines():
        if not line.startswith("stanli_prep "):
            continue
        fields = dict(f.split("=", 1) for f in line.split()[1:] if "=" in f)
        graph, stage = fields.get("graph"), fields.get("stage")
        g = rows.setdefault(graph, {})
        if stage == "lower" and "ops" in fields:
            g["lowered_ops"] = int(fields["ops"])
        elif stage == "total" and "ops" in fields:
            g["ops"] = int(fields["ops"])
            g["slot_elems"] = int(fields.get("slot_elems", 0))
            g["fill_elems"] = int(fields.get("fill_elems", 0))
    return rows


def lane_declines(stderr):
    records = [json.loads(line[len("LANES "):]) for line in stderr.splitlines()
               if line.startswith("LANES {")]
    # bounded_log_prob and budget_log_prob are tries of the same graph.
    return sorted({f"{r['pass']}/{re.sub(r'^.*log_prob$', 'log_prob', r['graph'])}: "
                   f"{r['reason']}"
                   for r in first_per_position(records)})


def lowering(ops_text):
    """How the program runs its loops: counts of the region opcodes and of
    scalar ops. No REGION op and many scalar ops means unrolled."""
    counts = opcode_counts(ops_text)
    return {op: counts.get(op, 0) for op in REGION_OPS if counts.get(op)}


def structure(tools, comp, data, prep_reps=3):
    """The prepared program: op counts, executor bytes, islands, declines,
    preparation time and peak RSS. `failure` is set when a tool fails."""
    mir = comp["mir"]
    result = {}
    code, out, err, _ = run([tools.dump_ops, mir, data, ALL_OPS],
                            tools.timeout,
                            env=clean_env({"STANLI_LANE_REPORT": "1"}))
    if code != 0:
        return {"failure": "dump_ops: " + failure_text(code, err)}
    result["ops_text_sha"] = sha_text(out)
    result["lowering"] = lowering(out)
    result["declines"] = lane_declines(err)
    code, wa, err, _ = run([tools.dump_ops, mir, data, ALL_OPS,
                            "--write-array"], tools.timeout)
    if code == 1 and err.strip() == NO_WRITE_ARRAY:
        wa = ""
    elif code != 0:
        return {"failure": "dump_ops --write-array: " + failure_text(code, err)}
    result["wa_text_sha"] = sha_text(wa)
    result["wa_lowering"] = lowering(wa)
    code, islands, err, _ = run([tools.dump_islands, mir, data], tools.timeout)
    if code != 0:
        return {"failure": "dump_islands: " + failure_text(code, err)}
    result["islands_sha"] = sha_text(islands)
    result["islands"] = len(re.findall(r"^== island ", islands, re.M))
    code, out, err, _ = run([tools.bench, mir, data, "--prep"], tools.timeout,
                            env=clean_env({"STANLI_PROFILE_PREP": "1"}))
    if code != 0:
        return {"failure": "bench_grad --prep: " + failure_text(code, err)}
    trace = parse_prep_trace(err)
    # Lowering may try the log density again under another name
    # (bounded_log_prob, budget_log_prob); the last graph is the program.
    tries = [g for name, g in trace.items()
             if name.endswith("log_prob") and "ops" in g]
    lp = dict(tries[-1]) if tries else {}
    lp["lowered_ops"] = max((g.get("lowered_ops") or 0 for name, g in
                             trace.items() if name.endswith("log_prob")),
                            default=0)
    wag = trace.get("write_array", {})
    result.update(
        ops=lp.get("ops"), lowered_ops=lp.get("lowered_ops"),
        wa_ops=wag.get("ops", 0), wa_lowered_ops=wag.get("lowered_ops", 0),
        slot_elems=lp.get("slot_elems"))
    result["program_bytes"] = (LOG_PROB_OP_BYTES * (lp.get("ops") or 0)
                               + WRITE_ARRAY_OP_BYTES * (wag.get("ops") or 0))
    result["executor_bytes"] = result["program_bytes"] + SLOT_ELEM_BYTES * (
        (lp.get("slot_elems") or 0) + (wag.get("slot_elems") or 0))
    best = None
    for _ in range(prep_reps):
        code, out, err, _ = run([tools.bench, mir, data, "--prep"],
                                tools.timeout)
        if code != 0:
            return {**result,
                    "failure": "bench_grad --prep: " + failure_text(code, err)}
        cols = out.split()
        prep = float(cols[0])
        if best is None or prep < best["prep_s"]:
            best = {"prep_s": prep, "params": int(cols[1]),
                    "peak_rss": int(cols[2]),
                    "first_gradient_s": float(cols[3])}
    result.update(best)
    return result


def same_program(a, b):
    return all(a.get(k) == b.get(k) for k in
               ("ops_text_sha", "wa_text_sha", "islands_sha"))


# ---------------------------------------------------------------------------
# Timing


def timing_args(points, warmup_ms=20, measure_ms=40, n_points=10):
    pts = (["--points-file", str(points)] if points
           else ["--points", str(n_points)])
    return ["--timed", "--warmup-ms", str(warmup_ms), "--measure-ms",
            str(measure_ms), *pts]


def parse_timed(stdout):
    """{point: ns per gradient} for the finite points of one process."""
    doc = json.loads(stdout.strip().splitlines()[-1])
    return {p["point"]: p["elapsed_ns"] / p["iterations"]
            for p in doc["points"] if p["finite"]}


def time_once(argv, timeout):
    code, out, err, _ = run(argv, timeout)
    if code != 0:
        raise RuntimeError(failure_text(code, err))
    return parse_timed(out)


def mean_over(times, kept):
    return statistics.fmean(times[k] for k in kept)


def paired_ratio(cmd_a, cmd_b, rounds, timeout):
    """Interleaved fresh processes of a and b, alternating which runs
    first. Each pair's ratio b/a is over the points finite in both. Returns
    the median ratio, the median times and the per-pair ratios."""
    ratios, ta, tb = [], [], []
    for i in range(rounds):
        order = (("a", cmd_a), ("b", cmd_b))
        if i % 2:
            order = order[::-1]
        got = {name: time_once(cmd, timeout) for name, cmd in order}
        kept = sorted(set(got["a"]) & set(got["b"]))
        if not kept:
            raise RuntimeError("no point is finite in both")
        a, b = mean_over(got["a"], kept), mean_over(got["b"], kept)
        ta.append(a)
        tb.append(b)
        ratios.append(b / a)
    return {"ratio": statistics.median(ratios), "a_ns": statistics.median(ta),
            "b_ns": statistics.median(tb), "ratios": ratios,
            "mad": statistics.median(abs(r - statistics.median(ratios))
                                     for r in ratios)}


def stanli_cmd(tools, comp, data, points):
    return [tools.bench, comp["mir"], data, *timing_args(points)]


def concurrent_copies(cmd, copies, timeout):
    """Run `copies` processes of cmd at once; median per-copy mean ns."""
    procs = [subprocess.Popen([str(a) for a in cmd], stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, text=True,
                              env=clean_env()) for _ in range(copies)]
    means = []
    for p in procs:
        out, err = p.communicate(timeout=timeout)
        if p.returncode != 0:
            raise RuntimeError(failure_text(p.returncode, err))
        t = parse_timed(out)
        means.append(statistics.fmean(t.values()))
    return statistics.median(means)


# An executor this large is timed with several concurrent copies too: a
# large program mostly hurts when chains share the caches (issue #657).
BIG_EXECUTOR = 4 << 20
COPIES = 12


def copies_record(a, b, n, timeout):
    """Per-copy ns of n concurrent processes of command a and of b."""
    return {"n": n, "base_ns": concurrent_copies(a, n, timeout),
            "variant_ns": concurrent_copies(b, n, timeout)}


def load_average():
    return os.getloadavg()[0]


# ---------------------------------------------------------------------------
# Committed benchmark points


def committed_points(tools, seed, stan, data):
    """The seed's committed points file, or (None, reason) when the entry is
    missing, fixed, or for a different source or data."""
    try:
        doc = benchmark_points.load(benchmark_points.DEFAULT_PATH)
        entry = benchmark_points.entry_for(doc, seed, sha_file(stan),
                                           sha_file(data))
    except (OSError, ValueError, SystemExit) as e:
        return None, f"fixed points ({e})"
    points = benchmark_points.points_of(entry)
    if not points:
        return None, "fixed points (the committed entry is fixed)"
    path = tools.cache / "points" / f"{seed}.json"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(points))
    return path, "committed reference-run points"


# ---------------------------------------------------------------------------
# CmdStan


def cmdstan_driver(tools, text):
    """The CmdStan gradient driver for a source, built once per source text
    in OUT/cache/cmdstan (stanc, then clang++ -O3 against the header, as
    harnesses/corpus_bench.py does). Returns {"exe"} or {"error"}."""
    key = sha_text(text)
    work = tools.cache / "cmdstan" / key[:16]
    with tools.key_lock(("cmdstan", key)):
        done = work / "build.json"
        if done.exists():
            return json.loads(done.read_text())
        work.mkdir(parents=True, exist_ok=True)
        (work / "model.stan").write_text(text)
        result = {}
        code, _, err, _ = run([tools.stanc, "model.stan", "--o=model.hpp"],
                              tools.timeout, cwd=work)
        if code != 0:
            result["error"] = "stanc: " + failure_text(code, err)
        else:
            argv = cmdstan_ref.compile_cmd(
                tools.cmdstan, work / "model.hpp",
                REPO / "tools" / "bench_cmdstan_grad.cpp", work / "gradbench",
                opt="-O3")
            code, _, err, seconds = run(argv, 1800, cwd=work)
            if code != 0:
                result["error"] = "clang++: " + failure_text(code, err)
            else:
                result = {"exe": str(work / "gradbench"), "build_s": seconds}
        done.write_text(json.dumps(result))
        return result


def cmdstan_cmd(driver, data, points):
    return [driver["exe"], data, *timing_args(points)]


def remove_tree(path):
    shutil.rmtree(path, ignore_errors=True)
