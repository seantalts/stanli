#!/usr/bin/env python3
"""The recorder: CmdStan's own answers, written down for the corpus oracle.

For each model: stanc emits C++, clang++ compiles it with tools/ref_driver.cpp
against the CmdStan tree (with -ffp-contract=off, matching CmdStan's own
build flags), both sides evaluate at the same deterministic unconstrained
points, and gradients are compared (relative 1e-10 pass threshold; measured
diffs are typically at or near bitwise).
lp must match exactly in policy terms: both sides use propto=false +
jacobian, so no propto constant is expected either.

CmdStan's values at EVERY point in verify_refs.POINTS are written to
docs/corpus-refs.json.gz, including the points CmdStan refuses (recorded
as a refusal, which stanli then has to reproduce) and the points where
stanli disagrees with them. What gets recorded is a property of CmdStan
alone; whether stanli matches it is a separate field, and never a reason
to leave a point out. The one exception is the write_array row, which is
recorded only where stanli already reproduces it -- stanli's write_array
is still incomplete for much of the corpus, and a reference nothing can
replay is a permanent red gate rather than an oracle.

Recording is a reviewed act: regenerating references to make a diff go
away defeats the whole oracle. See TESTING.md.

Usage: tools/verify_sample.py CMDSTAN_DIR PDB_DIR model1 model2 ...
       tools/verify_sample.py CMDSTAN_DIR PDB_DIR --from-refs [--jobs N]
"""
import argparse
import concurrent.futures
import gzip
import json
import math
import pathlib
import platform
import subprocess
import tempfile
import threading

from cmdstan_ref import compile_cmd
# The deviation arithmetic and the reference format live in the replay
# script, not here, so a change to either cannot land in the recorder
# without landing in the CI gate.
from verify_refs import (POINTS, REFS_PATH, SCHEMA, default_check_bin,
                         load_refs, model_files, pair_dev, parse_wa)

REPO = pathlib.Path(__file__).resolve().parent.parent
# Everything the recorded numbers depend on, by revision. Read from the
# checkouts themselves rather than from dev_setup.sh's pins: the pins say
# what should be there, and a reference has to say what was.
PINNED = {"cmdstan": REPO / "deps" / "cmdstan",
          "stan": REPO / "deps" / "stan",
          "math": REPO / "deps" / "math",
          "stanc3": REPO / "deps" / "stanc3-src",
          "posteriordb": REPO / "deps" / "posteriordb"}
WRITE_LOCK = threading.Lock()


def head_sha(repo):
    out = subprocess.run(["git", "-C", str(repo), "rev-parse", "HEAD"],
                         capture_output=True, text=True)
    return out.stdout.strip() or "unknown"


def cmdstan_version(cs):
    for line in (cs / "makefile").read_text().splitlines():
        if line.startswith("CMDSTAN_VERSION"):
            return line.split(":=")[1].strip()
    return "unknown"


def provenance(cs):
    """The revisions this run's numbers came out of."""
    p = {name: head_sha(path) for name, path in PINNED.items()}
    p["cmdstan_version"] = cmdstan_version(cs)
    # The libm is part of the answer: transcendentals round differently
    # per platform, which is why the replay gate is 1e-9 and not 1e-10.
    p["platform"] = f"{platform.system()} {platform.machine()}"
    return p


def inside_support(fields):
    """Is this `OK lp grad...` line from a point the model is defined at?

    A point outside a declared support prints a finite gradient next to an
    lp of -inf. Both engines agree on it and the density is never
    exercised, so it is a poor choice for the ONE point the scoreboard in
    docs/verification.json summarizes -- dogs_log's priors are uniform,
    and only its third point is in range. Every point is recorded either
    way; this only picks the primary one.
    """
    try:
        return math.isfinite(float(fields[1]))
    except (IndexError, ValueError):
        return False


def accepted(fields):
    """Did this engine accept the point, per its own way of saying no?

    The two spell a rejection differently. stanli_check prints EVAL_FAIL
    and no OK line; CmdStan's log_prob hands back a row of nan and only
    throws later, out of write_array, so ref_driver prints OK first. A
    unit_vector at the origin is exactly this. Reading the all-nan row as
    a value would record a reference no engine can be held to.
    """
    if not fields or fields[0] != "OK":
        return False
    values = [float(x) for x in fields[1:]]
    return not (values and all(v != v for v in values))


def write_results(results):
    """Merge into the machine-readable record the corpus scoreboard reads.

    One entry per model, describing the PRIMARY point only -- the point
    the old single-point recorder would have chosen. docs/corpus-status.md
    and the README counts derived from it have always meant "at the shared
    deterministic point", and the three-point references do not change
    what that sentence says. The per-point detail lives in the reference
    file, and the replay is what reports it.
    """
    out = REPO / "docs" / "verification.json"
    prev = json.loads(out.read_text()) if out.exists() else {}
    prev.update(results)
    out.write_text(json.dumps(prev, indent=1, sort_keys=True) + "\n")


def write_refs(refs, recorded):
    """Merge the raw CmdStan values into the committed reference file.

    tools/verify_refs.py replays these without CmdStan installed, which is
    what lets CI run the differential corpus check on every push. Values
    are stored as the exact %.17g strings ref_driver printed, so they
    round-trip bitwise and diffs stay readable.

    A merge into a file recorded against different revisions is refused:
    the provenance is one block for the whole file, and stamping this
    run's revisions onto values another CmdStan produced would make the
    file say something false about the models this run did not touch.
    """
    blob = {"schema": SCHEMA, "recorded": recorded, "models": {}}
    if REFS_PATH.exists():
        prev = json.loads(gzip.decompress(REFS_PATH.read_bytes()))
        if prev.get("schema") == SCHEMA:
            drift = [k for k, v in recorded.items()
                     if prev["recorded"].get(k) != v]
            if drift and set(prev["models"]) - set(refs):
                raise SystemExit(
                    f"docs/corpus-refs.json.gz was recorded against a "
                    f"different {', '.join(drift)}; re-record every model "
                    f"(--from-refs) rather than mixing two rigs in one "
                    f"file.")
            blob["models"] = prev["models"]
    blob["models"].update(refs)
    text = json.dumps(blob, indent=0, sort_keys=True).encode()
    REFS_PATH.write_bytes(gzip.compress(text, mtime=0))


def build_ref(cs, work, model, stan):
    """Compile ref_driver against this model. (exe, error).

    Cached on the path: recording the whole corpus is ~129 clang++ runs
    over stan-math, which is nearly all of the wall time, and a re-record
    after a recorder change should not pay it twice.
    """
    exe = work / f"{model}_ref"
    if exe.exists():
        return exe, None
    hpp = work / f"{model}.hpp"
    stanc = subprocess.run([str(REPO / "deps/stanc3/stanc"), str(stan),
                            f"--o={hpp}"], capture_output=True, text=True)
    if stanc.returncode != 0:
        return None, f"stanc: {stanc.stderr.strip().splitlines()[-1][:120]}"
    # ODE models pull in CVODES; CmdStan ships it prebuilt.
    tmp_exe = work / f"{model}_ref.part"
    r = subprocess.run(compile_cmd(cs, hpp, REPO / "tools/ref_driver.cpp",
                                   tmp_exe), capture_output=True, text=True)
    if r.returncode != 0:
        return None, r.stderr.strip().splitlines()[-1][:120]
    tmp_exe.rename(exe)  # only a complete binary lands in the cache
    return exe, None


def evaluate(exe, check_bin, stan, dj, point, timeout=600):
    """Both engines at one point. (ref fields, ref stdout, got, got stdout)."""
    ref_out = subprocess.run([str(exe), str(dj), str(point)],
                             capture_output=True, text=True,
                             timeout=timeout).stdout
    ref = (ref_out.splitlines() or [""])[0].split()
    got_out = subprocess.run(
        [str(check_bin), str(stan), str(dj), "--point", str(point),
         "--wa-values"], capture_output=True, text=True, cwd=REPO,
        timeout=timeout).stdout
    got = (got_out.splitlines() or [""])[0].split()
    return ref, ref_out, got, got_out


def compare(ref, got):
    """(status, max_rel, max_ulp, n) for one point's two OK lines."""
    rv = [float(x) for x in ref[1:]]
    gv = [float(x) for x in got[1:]]
    if len(rv) != len(gv):
        return ("SHAPE_FAIL", None, 0, len(rv))
    worst, worst_ulp = 0.0, 0
    for a, b in zip(rv, gv):
        rel, ulp = pair_dev(a, b)
        worst = max(worst, rel)
        worst_ulp = max(worst_ulp, ulp)
    if not math.isfinite(worst):
        # One side nonfinite where the other is not. There is no gate
        # above infinity, so the point is recorded with no deviation at
        # all and verify_refs holds it to the clean threshold, where it
        # fails until it is fixed or written into QUARANTINED by hand.
        return ("MISMATCH", None, worst_ulp, len(rv))
    return ("VERIFIED" if worst < 1e-10 else "MISMATCH", worst, worst_ulp,
            len(rv))


def record_wa(stan, point_entry, ref_out, got_out):
    """CmdStan's write_array row, when stanli reproduces it. Returns a note.

    Recorded whenever the row is deterministic (no _rng anywhere), so the
    values are a property of the draw and not of anyone's RNG stream. A
    generated quantities block is not required -- the parameter columns
    are the row too, and the order they come out in is exactly what a
    transposed array of matrices got wrong while every gradient stayed
    right.
    """
    wa, got_wa = parse_wa(ref_out), parse_wa(got_out)
    if "_rng" in stan.read_text() or not wa:
        return ""
    if not got_wa:
        return "; WA not recorded (stanli produced none)"
    if wa[0] != got_wa[0]:
        return "; WA not recorded (column names differ)"
    if len(wa[1]) != len(got_wa[1]):
        return "; WA not recorded (widths differ)"
    worst = 0.0
    for a, b in zip(map(float, wa[1]), map(float, got_wa[1])):
        worst = max(worst, pair_dev(a, b)[0])
    if not worst < 1e-9:
        return f"; WA not recorded (rel {worst:.2e})"
    point_entry["wa"] = {"names": wa[0], "values": wa[1]}
    return f"; WA {len(wa[1])} values recorded (max rel {worst:.2e})"


def record_model(model, stan, dj, exe, check_bin):
    """Every point of one model. (entry, result, lines).

    `entry` is the reference file's record for the model, `result` the
    scoreboard row for its primary point, `lines` what to print.
    """
    points, lines, primary, fallback = {}, [], None, None
    for point in POINTS:
        try:
            ref, ref_out, got, got_out = evaluate(exe, check_bin, stan, dj,
                                                  point)
        except subprocess.TimeoutExpired:
            lines.append(f"  point {point}: TIMEOUT, not recorded")
            continue
        ref_ok, got_ok = accepted(ref), accepted(got)
        if not ref_ok:
            # CmdStan refuses the point. Recorded as a refusal rather than
            # skipped: both engines refusing is agreement, and the replay
            # can hold stanli to it. Only stanli accepting it is news.
            status = "REJECTED_BOTH" if not got_ok else "STANLI_ONLY"
            points[str(point)] = {"status": status}
            lines.append(f"  point {point}: {status}"
                         + ("  (CmdStan rejects it and stanli does not)"
                            if got_ok else ""))
            continue
        entry = {"values": ref[1:]}
        points[str(point)] = entry
        if not got_ok:
            entry["status"] = "CMDSTAN_ONLY"
            entry["max_rel"] = None
            lines.append(f"  point {point}: CMDSTAN_ONLY, {len(ref) - 1} "
                         f"values recorded; stanli refuses this point")
            continue
        status, worst, worst_ulp, n = compare(ref, got)
        entry["status"] = status
        entry["max_rel"] = worst
        entry["max_ulp"] = worst_ulp
        note = record_wa(stan, entry, ref_out, got_out)
        shown = "no finite deviation" if worst is None else f"{worst:.2e}"
        lines.append(f"  point {point}: {status}, max rel diff {shown} "
                     f"({worst_ulp} ulp) over lp + {n - 1} grads{note}")
        if status == "SHAPE_FAIL":
            continue
        # The primary point: the first one both engines accept and put
        # inside the support, exactly the point the single-point recorder
        # would have stopped at. It is what docs/verification.json
        # summarizes, so the scoreboard means what it always meant.
        if primary is None and inside_support(ref) and inside_support(got):
            primary = point
        if fallback is None:
            fallback = point
    if primary is None:
        primary = fallback
    if primary is None:
        result = {"status": "REJECTED_BOTH", "max_rel": 0.0, "max_ulp": 0,
                  "n_values": 0, "point": POINTS[0]}
        return {"points": points}, result, lines
    pt = points[str(primary)]
    result = {"status": pt["status"], "max_rel": pt["max_rel"],
              "max_ulp": pt["max_ulp"], "n_values": len(pt["values"]),
              "point": primary}
    return {"primary": primary, "points": points}, result, lines


def one(model, cs, pdb, work, datas, check_bin):
    """(model, entry, result, lines) for one model, or entry None if it
    could not be built."""
    stan, dj = model_files(model, {"data": datas.get(model)}, pdb, work)
    exe, why = build_ref(cs, work, model, stan)
    if exe is None:
        return (model, None, None, [f"BUILD_FAIL {model}: {why}"])
    entry, result, lines = record_model(model, stan, dj, exe, check_bin)
    if model in datas:  # the posteriordb dataset; the language models
        entry["data"] = datas[model]  # carry their own data file
    return (model, entry, result, lines)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmdstan", type=pathlib.Path)
    ap.add_argument("pdb", type=pathlib.Path)
    ap.add_argument("models", nargs="*")
    ap.add_argument("--from-refs", action="store_true",
                    help="record every model already in the reference file")
    ap.add_argument("--jobs", type=int, default=1,
                    help="models recorded in parallel; the CmdStan "
                         "compiles are the wall time")
    ap.add_argument("--work", type=pathlib.Path,
                    help="reusable directory for the compiled reference "
                         "binaries (default: a fresh temporary one)")
    args = ap.parse_intermixed_args()

    cs = args.cmdstan
    pdb = args.pdb / "posterior_database"
    models = list(args.models)
    if args.from_refs:
        models += sorted(load_refs()[0])
    if not models:
        ap.error("name at least one model, or pass --from-refs")
    work = args.work or pathlib.Path(
        tempfile.mkdtemp(prefix="stanli_verify_"))
    work.mkdir(parents=True, exist_ok=True)
    check_bin = default_check_bin()
    recorded = provenance(cs)
    print(f"recording against CmdStan {recorded['cmdstan_version']} "
          f"({recorded['cmdstan'][:12]}), math {recorded['math'][:12]}, "
          f"on {recorded['platform']}")

    datas = {}
    for pj in sorted((pdb / "posteriors").glob("*.json")):
        meta = json.loads(pj.read_text())
        datas.setdefault(meta["model_name"], meta["data_name"])

    n_pass = 0
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        futs = [pool.submit(one, m, cs, pdb, work, datas, check_bin)
                for m in models]
        for fut in concurrent.futures.as_completed(futs):
            model, entry, result, lines = fut.result()
            if entry is None:
                print(lines[0])
                continue
            statuses = {p.get("status") for p in entry["points"].values()}
            print(f"{model}: " + " ".join(sorted(s for s in statuses if s)))
            print("\n".join(lines))
            n_pass += result["status"] == "VERIFIED"
            # Incremental, under a lock: a later hang keeps the rest, and
            # two workers must not read-modify-write the same file.
            with WRITE_LOCK:
                write_refs({model: entry}, recorded)
                write_results({model: result})

    print(f"\n{n_pass}/{len(models)} models verified against CmdStan at "
          f"their primary point, all {len(POINTS)} points recorded")


if __name__ == "__main__":
    main()
