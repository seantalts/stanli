#!/usr/bin/env python3
"""Differential corpus verification against committed CmdStan references.

tools/verify_sample.py runs CmdStan itself and records the exact lp and
gradient values it printed into docs/corpus-refs.json.gz. This script
replays stanli against those recorded values, which needs no CmdStan, no
C++ toolchain, and no 2 GB checkout: just a stanli_check binary and the
posteriordb model + data files. That is what lets the strongest oracle in
the project run in CI on every push, on every platform.

The references were generated on macOS arm64 with Apple's libm. Other
platforms' libm implementations round transcendentals differently, so the
gate here is deliberately looser than the 1e-10 the generating rig holds
itself to: 1e-9 relative. Every bug class that has actually reached the
corpus (silent in-place corruption at 1.7e+05 relative, quadratic
recompute, dropped tape links) sits many orders of magnitude above it,
and honest cross-libm drift sits well below.

Every model is evaluated at all three deterministic points, and every
point carries its own recorded reference; see POINTS below.

Usage: tools/verify_refs.py PDB_DIR [--check BIN] [--max-rel X]
                            [--jobs N] [--timeout S] [model ...]
       tools/verify_refs.py PDB_DIR --wa-report [--filter SUBSTR]
       tools/verify_refs.py PDB_DIR --wa-headers CMDSTAN_DIR [model ...]
Exit nonzero if any referenced model fails to run at any point, changes
shape, or exceeds the gate.
"""
import argparse
import collections
import concurrent.futures
import gzip
import json
import math
import pathlib
import re
import struct
import subprocess
import sys
import tempfile
import zipfile

REPO = pathlib.Path(__file__).resolve().parent.parent
# Corpora carried in the tree, each a directory of model.stan next to
# model.json: language and type constructs lifted from stanc3's own test
# suite (tests/stanc3/README.md), and brms output for the families and
# effect structures posteriordb does not contain (tests/brms/README.md).
# They go through the same oracle as posteriordb, and a reference is keyed
# on the file name either way.
LOCAL_CORPORA = (REPO / "tests" / "stanc3", REPO / "tests" / "brms",
                REPO / "tests" / "rethinking")
N_SAMPLER_COLS = 7
REFS_PATH = REPO / "docs" / "corpus-refs.json.gz"
# The reference file's format. Bumping this is a hard break on purpose:
# load_refs refuses anything else rather than reading what it recognizes
# and silently skipping the rest, which for an oracle is the failure mode
# that matters. Schema 1 held one point per model; see load_refs.
SCHEMA = 2
# The deterministic unconstrained points stanli_check and ref_driver both
# know (eval_point, defined identically in each). Every model carries a
# recorded CmdStan reference at every one of them.
#
# It did not always: schema 1 recorded exactly one point per model,
# because the recorder walked the list and stopped at the first point
# both engines accept and put the model inside its support. That search
# actively selects AWAY from a point that crashes -- which is how
# reductions_allowed segfaulted at point 2 for a month while the corpus
# ran green at point 0, and 25 of the 120 corpus models carry a
# conditional whose other side only some point reaches. Evaluating the
# other two points closed the crash gap; recording them closes the rest,
# since a wrong-but-finite gradient at an unreferenced point has nothing
# to be wrong against. What a point with no reference is still held to,
# for the cases where recording is impossible, is in probe_point.
POINTS = (0, 1, 2)

# (model, point) pairs whose recorded reference is not enforced, because
# stanli is known to disagree with it and the bug is open. The reference
# itself is recorded from CmdStan regardless -- references describe
# CmdStan, not stanli -- so deleting an entry here is all it takes for the
# full three-point parity to bite once the fix lands.
#
# Nothing may be added here on an argument. Every entry names the live
# CmdStan run that settled it: tools/ref_driver.cpp compiled for that
# model and run at that point. Empty since the pow-at-zero derivative that
# held s2_ar_cov's two points was fixed.
QUARANTINED = {}

# Models stanli does not run at all, mapped to what stops them. The
# references are recorded from CmdStan the same way every other model's
# are, so the only thing an entry suppresses is the failure; when the gap
# closes, the model matches its references, the replay reports GAP_CLOSED,
# and the run stays red until the entry is deleted. A crash is never
# excused: a segfault and a refusal are different bugs.
KNOWN_GAPS = {}

# (model, point) pairs excused from probe_point's finite-gradient rule,
# for points that carry no reference at all. Empty while every point is
# referenced -- pair_dev already counts NaN against NaN as agreement, so
# kronecker_gp at point 2 (435 of its 438 gradients nonfinite on BOTH
# engines, at the identical lp -187.85795069042379, because the all-zeros
# point makes its covariance degenerate outright) needs no entry: its
# reference records those nonfinite values and stanli reproduces them.
NONFINITE_GRAD_OK = {}

# Models held at every point to the floor gate_for gives a MISMATCH
# point, because the instruction set moves their values rather than
# stanli. Every entry factors an exponentiated-quadratic covariance that
# brms holds up with a 1e-12 jitter on the diagonal, and the smallest
# Cholesky pivot at the evaluation points is 3.7e-12 for i320_gp_expquad,
# 1.1e-12 for sw_gp and 1.8e-12 for s2_gp_by_gr against a diagonal of 1,
# so the factorization is singular to machine precision. Moving the GP
# covariates by one ulp on arm64 moves the latent-GP gradients at point 2
# by 1.9e-7 and 4.7e-8 relative, the same scale the x86_64 runner
# measured against references recorded on arm64: 1.03e-7 and 6.38e-9 at
# point 2, which arm64 recorded clean (CI run 33938697559). Points 0 and
# 1 of all three are recorded MISMATCH and were already held to that
# floor.
ILL_CONDITIONED = {
    "i320_gp_expquad": "cholesky_decompose of a jittered exp-quad "
                       "covariance whose smallest pivot is 3.7e-12",
    "s2_gp_by_gr": "cholesky_decompose of five jittered exp-quad "
                   "covariances whose smallest pivot is 1.8e-12",
    "sw_gp": "cholesky_decompose of a jittered exp-quad covariance whose "
             "smallest pivot is 1.1e-12",
}


def load_refs(path=REFS_PATH):
    """(models, provenance) from the reference file, or a hard failure.

    The one reader of docs/corpus-refs.json.gz, shared by the replay, the
    recorder, the lite-build cross-check and the corpus scoreboard, so a
    schema change cannot land in one and not the others. `models` maps a
    model name to {"data", "primary", "points": {"0": {...}, ...}};
    `provenance` names the CmdStan/Stan/Math/stanc3/posteriordb revisions
    the values were recorded against.
    """
    blob = json.loads(gzip.decompress(path.read_bytes()))
    schema = blob.get("schema")
    if schema != SCHEMA:
        raise SystemExit(
            f"{path} is schema {schema!r}, and these tools read schema "
            f"{SCHEMA}. Schema 1 recorded one point per model and its "
            f"entries are not a subset of this format, so half-reading it "
            f"would gate on a fraction of the corpus while reporting a "
            f"full pass. Re-record with tools/verify_sample.py "
            f"--from-refs, or check out the commit that matches the file.")
    return blob["models"], blob["recorded"]


def default_check_bin():
    """The stanli_check to use when the caller names none.

    Every build recipe in the project configures build-rel; build/ is the
    older debug tree that only some checkouts keep. Prefer whichever
    exists, so the corpus tools run without a flag either way. Shared by
    every tool that shells out to stanli_check, because a default that
    disagrees between the recorder and the replay is a way to record
    references from one binary and gate on another.
    """
    for name in ("build-rel", "build"):
        p = REPO / name / "stanli_check"
        if p.exists():
            return p
    return REPO / "build-rel" / "stanli_check"


def model_files(model, ref, pdb, tmp):
    """(stan, data) for a reference entry, from whichever corpus has it.

    posteriordb data is a zip per dataset and several models share one, so
    it is unpacked into tmp; the models carried in the tree have their data
    next to them and need no unpacking.
    """
    for directory in LOCAL_CORPORA:
        local = directory / f"{model}.stan"
        if local.exists():
            return local, directory / f"{model}.json"
    stan = pdb / "models" / "stan" / f"{model}.stan"
    dz = pdb / "data" / "data" / f"{ref['data']}.json.zip"
    if not stan.exists() or not dz.exists():
        return stan, dz
    dj = tmp / f"{model}_data.json"
    with zipfile.ZipFile(dz) as z:
        dj.write_bytes(z.read(z.namelist()[0]))
    return stan, dj


def ulp_distance(a, b):
    if a == b:
        return 0
    ia, ib = (struct.unpack("<q", struct.pack("<d", v))[0] for v in (a, b))
    key = lambda i: (-(1 << 63)) - i if i < 0 else i
    return abs(key(ia) - key(ib))


def pair_dev(a, b):
    """(rel, ulp) for one reference/stanli pair, nonfinite-safe.

    Both NaN, or the same infinity, is agreement (0, 0). A nonfinite
    value on one side only is an infinite relative deviation: the old
    arithmetic produced NaN here and Python's max() silently kept the
    running value, which hid dogs_log disagreeing with CmdStan at -inf
    for months.
    """
    if a != a and b != b:
        return (0.0, 0)
    if a == b:
        return (0.0, 0)
    if not (a - a == 0.0 and b - b == 0.0):
        return (float("inf"), ulp_distance(a, b) if a == a and b == b else 0)
    scale = max(abs(a), abs(b), 1.0)
    return (abs(a - b) / scale, ulp_distance(a, b))


def parse_wa(out):
    """(names_csv, value_strings) from WANAMES/WAVALS lines, or None."""
    names, vals = None, None
    for line in out.splitlines():
        if line.startswith("WANAMES "):
            names = line[8:]
        elif line.startswith("WAVALS"):
            vals = line[6:].split()
    if names is None or vals is None or names.startswith("FAIL"):
        return None
    if vals and vals[0] == "FAIL":
        return None
    return (names, vals)


def parse_status(out):
    """Fields from the final machine-readable result line, or [].

    Stan ``print`` statements in transformed data run before evaluation and
    can therefore put ordinary output ahead of stanli_check's result.  Keep
    the three result spellings in one parser so recording and replay do not
    accidentally treat that output as the result line.
    """
    # The driver writes its result after model output.  Walking backwards
    # also prevents a user `print("OK ...")` from shadowing the real status.
    for line in reversed(out.splitlines()):
        fields = line.split()
        if fields and fields[0] in ("OK", "COMPILE_FAIL", "EVAL_FAIL"):
            return fields
    return []


def accepted(fields):
    """Did this engine accept the point, per its own way of saying no?

    The two spell a rejection differently. stanli_check prints EVAL_FAIL
    and no OK line when evaluation threw, and prints the values when it
    did not, so a point it cannot evaluate also arrives as OK with a row
    of nan. CmdStan's log_prob hands back that same row and only throws
    later, out of write_array, so ref_driver prints OK first. A
    unit_vector at the origin is one; s2_invgaussian is another, since
    its inverse-square link takes inv_sqrt of a linear predictor that no
    evaluation point keeps positive. Reading the all-nan row as a value
    would record a reference no engine can be held to.
    """
    if not fields or fields[0] != "OK":
        return False
    values = [float(x) for x in fields[1:]]
    return not (values and all(v != v for v in values))


def corpus_models(pdb, wanted=(), contains="", excluded=()):
    """Unique (model, data-name) pairs, in posteriordb order."""
    wanted, excluded, seen = set(wanted), set(excluded), set()
    for path in sorted((pdb / "posteriors").glob("*.json")):
        meta = json.loads(path.read_text())
        model = meta["model_name"]
        if (model in seen or model in excluded
                or (wanted and model not in wanted)
                or (contains and contains not in model)):
            continue
        seen.add(model)
        yield model, meta["data_name"]


def corpus_input(pdb, tmp, model, data_name):
    stan = pdb / "models" / "stan" / f"{model}.stan"
    zipped = pdb / "data" / "data" / f"{data_name}.json.zip"
    if not stan.exists() or not zipped.exists():
        return None
    data = tmp / f"{data_name}.json"
    if not data.exists():
        with zipfile.ZipFile(zipped) as archive:
            data.write_bytes(archive.read(archive.namelist()[0]))
    return stan, data


def cmdstan_header(cmdstan, work, stan, data):
    """One CmdStan CSV header, less the seven sampler columns."""
    local = work / stan.name
    local.write_bytes(stan.read_bytes())
    exe = work / stan.stem
    built = subprocess.run(["make", str(exe)], cwd=cmdstan,
                           capture_output=True, text=True)
    if built.returncode != 0:
        lines = built.stderr.strip().splitlines()
        return None, "build failed: " + (lines[-1][:120] if lines else "")
    csv = work / f"{stan.stem}.csv"
    run = subprocess.run(
        [str(exe), "sample", "num_warmup=2", "num_samples=1", "data",
         f"file={data}", "output", f"file={csv}"],
        cwd=work, capture_output=True, text=True)
    if not csv.exists():
        return None, "run failed: " + (run.stdout + run.stderr).strip()[-160:]
    for line in csv.read_text().splitlines():
        if line and not line.startswith("#"):
            return line.split(",")[N_SAMPLER_COLS:], ""
    return None, "no header in csv"


def check_wa_headers(pdb, check_bin, cmdstan, models, excluded=()):
    """Compare write_array columns with a live CmdStan, model by model."""
    tmp = pathlib.Path(tempfile.mkdtemp(prefix="stanli_hdr_"))
    ok, bad, skipped = [], [], []
    selected = list(corpus_models(pdb, models, excluded=excluded))
    if not selected:
        print("no corpus models selected", file=sys.stderr)
        return 2
    runnable = 0
    for model, data_name in selected:
        inputs = corpus_input(pdb, tmp, model, data_name)
        if inputs is None:
            continue
        runnable += 1
        stan, data = inputs
        ours = subprocess.run([str(check_bin), str(stan), str(data), "--columns"],
                              capture_output=True, text=True, cwd=REPO)
        if ours.returncode != 0:
            skipped.append((model, ours.stdout.strip()[:110]))
            continue
        theirs, why = cmdstan_header(cmdstan, tmp, stan, data)
        if theirs is None:
            skipped.append((model, why))
            continue
        got = ours.stdout.strip().split(",")
        truncated = ours.stderr.startswith("TRUNCATED")
        ref = theirs[:len(got)] if truncated else theirs
        if got == ref:
            ok.append((model, len(got), truncated))
        else:
            first = next((i for i in range(max(len(got), len(ref)))
                          if i >= len(got) or i >= len(ref) or got[i] != ref[i]),
                         0)
            bad.append((model, len(got), len(ref), first,
                        got[first:first + 4], ref[first:first + 4]))
    print(f"\n== {len(ok)} match, {len(bad)} differ, {len(skipped)} skipped ==")
    for model, n, truncated in ok:
        print(f"  MATCH   {model:44s} {n:6d} cols"
              + ("  (prefix)" if truncated else ""))
    for model, no, nr, first, got, ref in bad:
        print(f"  DIFFER  {model:44s} ours {no} cols, cmdstan {nr}; "
              f"first difference at {first}\n      ours    {got}\n      cmdstan {ref}")
    for model, why in skipped:
        print(f"  skip    {model:44s} {why}")
    if runnable == 0 or not (ok or bad):
        print("no selected model was checked", file=sys.stderr)
        return 2
    return 1 if bad else 0


def fail_detail(proc, got):
    """Why a stanli_check run did not print OK.

    Say HOW it died, not just that it did: a negative returncode is a
    signal (ldaK5's 49 GB compile came back as a bare RUN_FAIL because
    the OOM killer leaves no stdout).
    """
    detail = " ".join(got[:3])
    if not detail:
        detail = (f"killed by signal {-proc.returncode}"
                  if proc.returncode < 0
                  else f"exit {proc.returncode}, no output")
    err = proc.stderr.strip().splitlines()
    if err:
        detail += " | " + err[-1][:120]
    return detail


def probe_point(model, stan, dj, check_bin, point, timeout):
    """Evaluate a point with no recorded reference. (status, detail).

    No CmdStan values means no parity check, so the bar here is only what
    the run can be held to on its own evidence:

      * It must not crash. A signal, or a nonzero exit with none of
        stanli_check's machine-readable lines on stdout, is a hard
        failure. This is the whole reason the extra points are run: a
        SIGSEGV is not a numerical disagreement anyone needs a reference
        to adjudicate.
      * A clean rejection passes. stanli_check prints EVAL_FAIL only when
        evaluation THREW, which is how a model outside its declared
        support at this point reports itself (an ODE solution dipping
        below a lower bound, say). CmdStan's ref_driver throws in the
        same place, and the recorder counts both engines refusing a point
        as agreement rather than a stanli failure -- REJECTED_BOTH in
        verify_sample.py. Only one engine is present here, so the
        rejection is accepted, not confirmed.
      * An lp of -inf passes for the same reason: a zero density is a
        value both engines produce and agree on (bernoulli_lccdf(1|theta)
        is log(0) by definition, and stanli_check deliberately prints
        nonfinite values rather than refusing them so the two drivers
        stay comparable). +inf or NaN does not pass -- no reference in
        the corpus was recorded that way, and a NaN lp is exactly what an
        unwritten register reads as.
      * A finite lp must come with finite gradients. lp finite next to a
        nonfinite gradient is the shape a dropped tape link takes, and
        spotting it needs no reference. NONFINITE_GRAD_OK and QUARANTINED
        carry the (model, point) pairs excused from this, each with the
        CmdStan run that settled it.

    Every point of every model is referenced today, so this runs only for
    a quarantined point or a reference file with a gap in it.

    COMPILE_FAIL is a failure here rather than a rejection: compiling does
    not depend on the evaluation point, so a model that compiled at its
    recorded point must compile at every other one.
    """
    try:
        proc = subprocess.run(
            [str(check_bin), str(stan), str(dj), "--point", str(point)],
            capture_output=True, text=True, cwd=REPO, timeout=timeout)
    except subprocess.TimeoutExpired:
        return ("POINT_TIMEOUT", "")
    got = parse_status(proc.stdout)
    kind = got[0] if got else ""
    if kind == "EVAL_FAIL":
        return ("OK", "")
    if kind != "OK":
        return ("POINT_COMPILE_FAIL" if kind == "COMPILE_FAIL" else "CRASH",
                fail_detail(proc, got))
    lp = float(got[1])
    if math.isnan(lp) or lp == math.inf:
        return ("POINT_NONFINITE_LP", f"lp {got[1]}")
    if lp == -math.inf:
        return ("OK", "")
    bad = sum(1 for x in got[2:] if not math.isfinite(float(x)))
    excused = ((model, point) in NONFINITE_GRAD_OK
               or (model, point) in QUARANTINED)
    if bad and not excused:
        return ("POINT_NONFINITE_GRAD",
                f"lp {got[1]} but {bad}/{len(got) - 2} gradients nonfinite")
    return ("OK", "")


def worst_pair(rv, gv):
    """(worst relative deviation, worst ULP distance) over paired values."""
    worst, worst_ulp = 0.0, 0
    for a, b in zip(rv, gv):
        rel, ulp = pair_dev(a, b)
        worst = max(worst, rel)
        worst_ulp = max(worst_ulp, ulp)
    return worst, worst_ulp


def check_point(model, stan, dj, check_bin, point, pt, timeout, no_wa,
                no_lp):
    """Replay one recorded point. (status, worst, worst_ulp, n, detail).

    A point whose entry has no `values` is one CmdStan itself refuses (it
    threw, or answered with a row of nan, which is how a unit_vector at
    the origin reports itself). The reference records the refusal, and
    stanli has to refuse it too, in either of the two spellings the
    recorder reads as one: EVAL_FAIL, which stanli_check prints when
    evaluation threw, or an OK line whose values are all nan. Accepting a
    point CmdStan rejects is a real disagreement, not a free pass -- it is
    the same asymmetry as one engine throwing, read from the other side.
    """
    cmd = [str(check_bin), str(stan), str(dj), "--point", str(point)]
    want_wa = "values" in pt and not no_wa
    if want_wa:
        cmd.append("--wa-values")
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, cwd=REPO,
                              timeout=timeout)
    except subprocess.TimeoutExpired:
        return ("TIMEOUT", 0.0, 0, 0, f"point {point}")
    got = parse_status(proc.stdout)
    kind = got[0] if got else ""
    if "values" not in pt:
        if kind == "EVAL_FAIL":
            return ("OK", 0.0, 0, 0, "")
        if kind == "OK":
            if not accepted(got):
                return ("OK", 0.0, 0, 0, "")
            return ("POINT_NOT_REJECTED", 0.0, 0, 0,
                    f"point {point}: CmdStan rejects it, stanli returned "
                    f"lp {got[1]}")
        # Refusing for the wrong reason is not agreement: COMPILE_FAIL
        # says the model never ran, and a crash says nothing at all.
        return ("POINT_COMPILE_FAIL" if kind == "COMPILE_FAIL" else "CRASH",
                0.0, 0, 0, f"point {point}: {fail_detail(proc, got)}")
    if kind != "OK":
        # A death with nothing to say (a signal, or a nonzero exit and no
        # machine-readable line) is reported as a crash even here, where a
        # reference exists: "stanli disagrees with CmdStan" and "stanli
        # segfaulted" are different bugs and reading the first one for the
        # second is how reductions_allowed stayed green for a month.
        crashed = proc.returncode < 0 or not got
        return ("CRASH" if crashed else "RUN_FAIL", 0.0, 0, 0,
                f"point {point}: {fail_detail(proc, got)}")
    rv = [float(x) for x in pt["values"]]
    gv = [float(x) for x in got[1:]]
    if len(rv) != len(gv):
        return ("SHAPE_FAIL", 0.0, 0, 0,
                f"point {point}: {len(rv)} vs {len(gv)}")
    if no_lp:
        # Element 0 is lp. A STANLI_LITE_LP build computes the full
        # density where CmdStan's `~` drops constant terms, so its lp sits
        # a per-model constant above the reference while every gradient is
        # unchanged -- which is the part sampling depends on. Dropping the
        # element keeps the corpus oracle usable for that build. Whether
        # the offset is genuinely CONSTANT is a different question, and
        # the one that can actually catch a bug; tools/verify_lite.py
        # answers it by evaluating both builds at several points.
        rv, gv = rv[1:], gv[1:]
    worst, worst_ulp = worst_pair(rv, gv)
    n = len(rv)
    if want_wa:
        # The write_array reference: column names must match exactly, and
        # the values (constrained parameters, transformed parameters and
        # generated quantities at the same point) share the model's gate.
        #
        # A row is demanded even where the reference holds none. The
        # recorder drops the `wa` block whenever the two engines disagreed
        # about it, stanli's included, so a model whose write_array fails
        # outright records no reference and used to be replayed as if it
        # had no section at all. Every model has a row: the constrained
        # parameters are one.
        wa = parse_wa(proc.stdout)
        if wa is None:
            return ("WA_FAIL", worst, worst_ulp, n,
                    f"point {point}: no or failed write_array output")
        if "wa" not in pt:
            return ("OK", worst, worst_ulp, n, "")
        names, vals = wa
        if names != pt["wa"]["names"]:
            return ("WA_NAMES_FAIL", worst, worst_ulp, n,
                    f"point {point}: got {names[:60]} "
                    f"want {pt['wa']['names'][:60]}")
        wref = [float(x) for x in pt["wa"]["values"]]
        wgot = [float(x) for x in vals]
        if len(wref) != len(wgot):
            return ("WA_SHAPE_FAIL", worst, worst_ulp, n,
                    f"point {point}: {len(wref)} vs {len(wgot)}")
        wworst, wulp = worst_pair(wref, wgot)
        worst, worst_ulp = max(worst, wworst), max(worst_ulp, wulp)
        n += len(wref)
    return ("OK", worst, worst_ulp, n, "")


def check_model(model, ref, pdb, check_bin, tmp, timeout, max_rel,
                no_wa=False, no_lp=False):
    """Replay one model, under whatever KNOWN_GAPS says about it."""
    result = replay_model(model, ref, pdb, check_bin, tmp, timeout, max_rel,
                          no_wa, no_lp)
    if model not in KNOWN_GAPS:
        return result
    _, status, rel, ulp, total, _, notes = result
    notes.append(f"KNOWN GAP {model}: {KNOWN_GAPS[model]}")
    if status == "CRASH":
        return result
    if status == "OK":
        return (model, "GAP_CLOSED", rel, ulp, total,
                f"{model} matches its references now "
                f"({KNOWN_GAPS[model]}); delete it from KNOWN_GAPS", notes)
    return (model, "OK", 0.0, 0, total, "", notes)


def replay_model(model, ref, pdb, check_bin, tmp, timeout, max_rel,
                 no_wa=False, no_lp=False):
    """Replay every point of one model.

    Returns (model, status, max_rel, max_ulp, n_values, detail, notes).
    The deviation reported is the worst over the points held to the clean
    gate; a point recorded as MISMATCH is gated against what it was
    recorded at (gate_for) and kept out of the headline number, so one
    documented deviation cannot become the corpus-wide "worst".

    The points run sequentially inside one pool task rather than as three
    tasks, because model_files unpacks the shared posteriordb data zip to
    one path per model and three workers writing it at once would race.
    """
    stan, dj = model_files(model, ref, pdb, tmp)
    if not stan.exists() or not dj.exists():
        return (model, "MISSING_INPUT", 0.0, 0, 0, str(stan), [])
    worst, worst_ulp, total, notes = 0.0, 0, 0, []
    for point in POINTS:
        pt = ref["points"].get(str(point))
        if (model, point) in QUARANTINED:
            # The reference is recorded and is not enforced: stanli is
            # known to disagree here. Say so on every run -- a quarantine
            # nobody sees is an exception that outlives its bug -- and
            # hold the point to what a point with no reference is held to,
            # so a crash here still fails.
            notes.append(f"QUARANTINE {model} point {point}: "
                         f"{QUARANTINED[(model, point)]}")
            status, detail = probe_point(model, stan, dj, check_bin, point,
                                         timeout)
            if status != "OK":
                return (model, status, worst, worst_ulp, total,
                        f"point {point}: {detail}", notes)
            continue
        if pt is None:
            status, detail = probe_point(model, stan, dj, check_bin, point,
                                         timeout)
            if status != "OK":
                return (model, status, worst, worst_ulp, total,
                        f"point {point}: {detail}", notes)
            notes.append(f"UNREFERENCED {model} point {point}: ran clean, "
                         f"but nothing compared it against CmdStan")
            continue
        status, rel, ulp, n, detail = check_point(
            model, stan, dj, check_bin, point, pt, timeout, no_wa, no_lp)
        total += n
        if status != "OK":
            return (model, status, worst, worst_ulp, total, detail, notes)
        gate = gate_for(model, pt, max_rel)
        if rel >= gate:
            return (model, "GATE", rel, ulp, total,
                    f"point {point}: {rel:.2e} ({ulp} ulp) over {n} "
                    f"values, allowed {gate:.1e}", notes)
        if model in ILL_CONDITIONED and rel >= max_rel:
            notes.append(f"ILL-CONDITIONED {model} point {point}: "
                         f"{ILL_CONDITIONED[model]} ({rel:.2e})")
        if pt.get("status") != "MISMATCH" and model not in ILL_CONDITIONED:
            worst, worst_ulp = max(worst, rel), max(worst_ulp, ulp)
    return (model, "OK", worst, worst_ulp, total, "", notes)


def short_wa(msg):
    match = re.search(r"unsupported (?:function |statement function )?([\w]+)",
                      msg)
    return (f"unsupported {match[1]}" if match
            else msg.split("|")[0].strip()[:80])


def check_wa_coverage(pdb, check_bin, models, contains, timeout, excluded=()):
    """Probe write_array at points 0--2 and report corpus coverage."""
    tmp = pathlib.Path(tempfile.mkdtemp(prefix="stanli_wa_"))
    rows, reasons = [], collections.Counter()
    selected = list(corpus_models(pdb, models, contains, excluded))
    if not selected:
        print("no corpus models selected", file=sys.stderr)
        return 2
    runnable = 0
    for model, data_name in selected:
        inputs = corpus_input(pdb, tmp, model, data_name)
        if inputs is None:
            continue
        runnable += 1
        stan, data = inputs
        try:
            for point in ("0", "1", "2"):
                proc = subprocess.run(
                    [str(check_bin), str(stan), str(data), "--point", point],
                    capture_output=True, text=True, timeout=timeout, cwd=REPO)
                if parse_status(proc.stdout)[:1] == ["OK"]:
                    break
        except subprocess.TimeoutExpired:
            rows.append((model, "TIMEOUT", ""))
            continue
        if parse_status(proc.stdout)[:1] != ["OK"]:
            continue
        wa_lines = [line[3:] for line in proc.stderr.splitlines()
                    if line.startswith("WA ")]
        wa = wa_lines[-1] if wa_lines else ""
        if wa.startswith("none"):
            rows.append((model, "NO_GQ_SECTION", ""))
            continue
        if wa.startswith("empty"):
            detail = wa[6:]
            rows.append((model, "EMPTY", detail))
            reasons[short_wa(detail)] += 1
            continue
        match = re.match(r"(\d+) vars (\d+) values (\d+) nonfinite (.*)", wa)
        if not match:
            rows.append((model, "UNPARSED", wa))
            continue
        nvars, nvals, nbad = map(int, match.groups()[:3])
        tail = match[4]
        note = f", {nbad}/{nvals} nonfinite" if nbad else ""
        if tail.startswith("complete"):
            mode = " (interpreted)" if "interpreted" in tail else ""
            rows.append((model, "COMPLETE", f"{nvars} vars{mode}{note}"))
        else:
            rows.append((model, "TRUNCATED", tail + note))
            reasons[short_wa(tail)] += 1
    counts = collections.Counter(row[1] for row in rows)
    print(f"{len(rows)} compiling models")
    for name in ("COMPLETE", "TRUNCATED", "NONFINITE", "EMPTY", "NO_GQ_SECTION",
                 "TIMEOUT", "UNPARSED"):
        if counts[name]:
            print(f"  {counts[name]:4d}  {name}")
    if reasons:
        print("\nwhat stops the rest:")
        for reason, count in reasons.most_common(30):
            print(f"  {count:4d}  {reason}")
    print("\nper model:")
    for model, status, detail in sorted(rows, key=lambda row: (row[1], row[0])):
        print(f"  {status:14s} {model:44s} {detail[:110]}")
    if runnable == 0 or not rows:
        print("no selected model was checked", file=sys.stderr)
        return 2
    return 0


def gate_for(model, pt, default):
    """The threshold this recorded point is held to.

    A point recorded as MISMATCH is one whose disagreement with CmdStan is
    documented and understood (kronecker_gp: two of 438 gradients flow
    through eigenvectors of a nearly degenerate covariance). Gating it at
    the clean threshold would fail every run; ignoring it would let a real
    regression hide behind a known deviation. Gate it above what it was
    recorded at, so it can never get much worse unnoticed.

    A point recorded with no finite deviation at all -- one side nonfinite
    where the other is not, which pair_dev scores as infinite -- gets the
    clean gate and therefore fails. There is no threshold above infinity,
    and deriving one from the recording would turn "stanli was wrong here
    when this was recorded" into "stanli may be wrong here forever". Such
    a point passes only by being fixed, or by an entry in QUARANTINED that
    someone has to write down and delete.

    4x, not 2x: an ill-conditioned eigendecomposition amplifies ISA-level
    differences, and the deviation itself moves across platforms. Measured
    for kronecker_gp point 0: 7.1e-3 on arm64 (where the reference was
    recorded), 1.71e-2 on both x86_64 runners, identical to each other.

    And a floor, because the amplification is not proportional to the
    deviation: point 2 recorded 1.05e-3 on arm64 and measured 8.2e-3 on
    x86_64 (CI run 32637919029) -- 7.8x where point 0 moved 2.4x -- so a
    pure multiplier under-gates precisely the smallest recorded
    deviations. The floor is 4x the worst cross-platform measurement over
    the model's points, and a clean point keeps the clean gate unless its
    model is in ILL_CONDITIONED, where the recording platform decides
    which points come out clean and holding one of them to 1e-9 fails on
    the others. The gate is a tripwire for the regression class this
    corpus has actually caught, which measured 1.7e+5 relative, six
    orders of magnitude above the floor.
    """
    rel = pt.get("max_rel")
    if rel is None:
        return default
    if model in ILL_CONDITIONED or pt.get("status") == "MISMATCH":
        return max(rel * 4.0, 4.0 * 8.2e-3, default)
    return default


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pdb", type=pathlib.Path)
    ap.add_argument("models", nargs="*")
    ap.add_argument("--check", type=pathlib.Path,
                    default=default_check_bin())
    ap.add_argument("--max-rel", type=float, default=1e-9)
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--no-lp", action="store_true",
                    help="compare gradients only, not lp: a STANLI_LITE_LP "
                         "build shifts lp by a constant (docs/lite-lp.md)")
    ap.add_argument("--no-wa", action="store_true",
                    help="replay lp and gradients only (the WASM check "
                         "driver has no write_array entry point yet)")
    ap.add_argument("--wa-report", action="store_true",
                    help="report structural write_array coverage")
    ap.add_argument("--wa-headers", type=pathlib.Path, metavar="CMDSTAN_DIR",
                    help="compare write_array headers with live CmdStan")
    ap.add_argument("--skip", default="",
                    help="comma-separated models to exclude (the wasm32 "
                         "build cannot fit nn_rbm1bJ100's compile in 4GB)")
    ap.add_argument("--timeout", type=float, default=300)
    ap.add_argument("--per-model", action="store_true",
                    help="print each model's worst deviation and ULP")
    ap.add_argument("--filter", default="", metavar="SUBSTR",
                    help="with --wa-report, select model names containing this")
    args = ap.parse_intermixed_args()

    pdb = args.pdb / "posterior_database"
    if args.wa_headers and args.wa_report:
        ap.error("--wa-headers and --wa-report are separate modes")
    if args.filter and not args.wa_report:
        ap.error("--filter requires --wa-report")
    check_bin = args.check
    skip = set(filter(None, args.skip.split(",")))
    if args.models and all(model in skip for model in args.models):
        ap.error("--skip excludes every requested model")
    if args.wa_headers:
        return check_wa_headers(pdb, check_bin, args.wa_headers.resolve(),
                                args.models, skip)
    if args.wa_report:
        return check_wa_coverage(pdb, check_bin, args.models, args.filter,
                                 args.timeout, skip)
    refs, recorded = load_refs()
    # A new teaching fixture without a reference must fail the default push
    # gate. Iterating only the existing references would silently omit it.
    teaching = {p.stem for p in (REPO / "tests" / "rethinking").glob("*.stan")}
    models = args.models or sorted(set(refs) | teaching)
    models = [m for m in models if m not in skip]
    missing = [m for m in models if m not in refs]
    if missing:
        print(f"no reference recorded for: {' '.join(missing)}")
        return 2

    # Which CmdStan these values came from is part of reading the result:
    # the gate is 1e-9 against one specific build of one specific pin.
    print(f"references recorded against CmdStan "
          f"{recorded['cmdstan_version']} ({recorded['cmdstan'][:12]}), "
          f"math {recorded['math'][:12]}, on {recorded['platform']}")
    tmp = pathlib.Path(tempfile.mkdtemp(prefix="stanli_refs_"))
    failures, values = [], 0
    worst_overall = ("", 0.0, 0)
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        futs = [pool.submit(check_model, m, refs[m], pdb, check_bin, tmp,
                            args.timeout, args.max_rel, args.no_wa,
                            args.no_lp)
                for m in models]
        for fut in concurrent.futures.as_completed(futs):
            model, status, rel, ulp, n, detail, notes = fut.result()
            for note in notes:
                print(note)
            values += n
            if status != "OK":
                failures.append((model, status, detail))
                print(f"{status} {model} {detail}")
                continue
            if args.per_model:
                print(f"{model}\t{rel:.2e}\t{ulp}")
            if rel > worst_overall[1]:
                worst_overall = (model, rel, ulp)

    ok = len(models) - len(failures)
    print(f"\n{ok}/{len(models)} models within {args.max_rel:.0e} of the "
          f"CmdStan references at all {len(POINTS)} evaluation points "
          f"({values} values compared)"
          + (" (gradients only)" if args.no_lp else "")
          + (f"; worst {worst_overall[0]} at {worst_overall[1]:.2e} "
             f"({worst_overall[2]} ulp)" if worst_overall[0] else ""))
    if failures:
        print(f"{len(failures)} FAILED: "
              + " ".join(m for m, _, _ in failures))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
