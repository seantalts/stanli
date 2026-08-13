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

Usage: tools/verify_refs.py PDB_DIR [--check BIN] [--max-rel X]
                            [--jobs N] [--timeout S] [model ...]
       tools/verify_refs.py PDB_DIR --wa-report [--filter SUBSTR]
       tools/verify_refs.py PDB_DIR --wa-headers CMDSTAN_DIR [model ...]
Exit nonzero if any referenced model fails to run, changes shape, or
exceeds the gate.
"""
import argparse
import collections
import concurrent.futures
import gzip
import json
import pathlib
import re
import struct
import subprocess
import sys
import tempfile
import zipfile

REPO = pathlib.Path(__file__).resolve().parent.parent
# Models from stanc3's own test suite, with data generated for them; see
# tests/stanc3/README.md. They cover language and type constructs no real
# posterior happens to use, so they live beside the corpus rather than in
# it, and a reference is keyed on the file name either way.
LANG = REPO / "tests" / "stanc3"
N_SAMPLER_COLS = 7


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
    it is unpacked into tmp; the language models carry their data next to
    them and need no unpacking.
    """
    local = LANG / f"{model}.stan"
    if local.exists():
        return local, LANG / f"{model}.json"
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


def check_model(model, ref, pdb, check_bin, tmp, timeout, no_wa=False,
                no_lp=False):
    """Returns (model, status, max_rel, max_ulp, n_values, detail)."""
    stan, dj = model_files(model, ref, pdb, tmp)
    if not stan.exists() or not dj.exists():
        return (model, "MISSING_INPUT", 0.0, 0, 0, str(stan))
    cmd = [str(check_bin), str(stan), str(dj), "--point", str(ref["point"])]
    if "wa" in ref and not no_wa:
        cmd.append("--wa-values")
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, cwd=REPO,
                              timeout=timeout)
    except subprocess.TimeoutExpired:
        return (model, "TIMEOUT", 0.0, 0, 0, "")
    lines = proc.stdout.splitlines()
    got = lines[0].split() if lines else []
    if not got or got[0] != "OK":
        # Say HOW it died, not just that it did: a negative returncode is
        # a signal (ldaK5's 49 GB compile came back as a bare RUN_FAIL
        # because the OOM killer leaves no stdout).
        detail = " ".join(got[:3])
        if not detail:
            detail = (f"killed by signal {-proc.returncode}"
                      if proc.returncode < 0
                      else f"exit {proc.returncode}, no output")
        err = proc.stderr.strip().splitlines()
        if err:
            detail += " | " + err[-1][:120]
        return (model, "RUN_FAIL", 0.0, 0, 0, detail)
    rv = [float(x) for x in ref["values"]]
    gv = [float(x) for x in got[1:]]
    if len(rv) != len(gv):
        return (model, "SHAPE_FAIL", 0.0, 0, 0, f"{len(rv)} vs {len(gv)}")
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
    worst, worst_ulp = 0.0, 0
    for a, b in zip(rv, gv):
        rel, ulp = pair_dev(a, b)
        worst = max(worst, rel)
        worst_ulp = max(worst_ulp, ulp)
    n = len(rv)
    if "wa" in ref and not no_wa:
        # The write_array reference: column names must match exactly, and
        # the values (transformed parameters + generated quantities at the
        # same point) share the model's gate.
        wa = parse_wa(proc.stdout)
        if wa is None:
            return (model, "WA_FAIL", worst, worst_ulp, n,
                    "no or failed write_array output")
        names, vals = wa
        if names != ref["wa"]["names"]:
            return (model, "WA_NAMES_FAIL", worst, worst_ulp, n,
                    f"got {names[:60]} want {ref['wa']['names'][:60]}")
        wref = [float(x) for x in ref["wa"]["values"]]
        wgot = [float(x) for x in vals]
        if len(wref) != len(wgot):
            return (model, "WA_SHAPE_FAIL", worst, worst_ulp, n,
                    f"{len(wref)} vs {len(wgot)}")
        for a, b in zip(wref, wgot):
            rel, ulp = pair_dev(a, b)
            worst = max(worst, rel)
            worst_ulp = max(worst_ulp, ulp)
        n += len(wref)
    return (model, "OK", worst, worst_ulp, n, "")


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
                if proc.stdout.startswith("OK"):
                    break
        except subprocess.TimeoutExpired:
            rows.append((model, "TIMEOUT", ""))
            continue
        if not proc.stdout.startswith("OK"):
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


def gate_for(ref, default):
    """The threshold this model is held to.

    A model recorded as MISMATCH is one whose disagreement with CmdStan is
    documented and understood (kronecker_gp: two of 438 gradients flow
    through eigenvectors of a nearly degenerate covariance). Gating it at
    the clean threshold would fail every run; ignoring it would let a real
    regression hide behind a known deviation. Gate it above what it was
    recorded at, so it can never get much worse unnoticed.

    4x, not 2x: an ill-conditioned eigendecomposition amplifies ISA-level
    differences, and the deviation itself moves across platforms. Measured
    for kronecker_gp: 7.1e-3 on arm64 (where the reference was recorded),
    1.71e-2 on both x86_64 runners, identical to each other. The gate is a
    tripwire for the regression class this corpus has actually caught,
    which measured 1.7e+5 relative, seven orders of magnitude above it.
    """
    if ref.get("status") == "MISMATCH":
        return max(ref.get("max_rel", 0.0) * 4.0, default)
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
    refs = json.loads(gzip.decompress(
        (REPO / "docs" / "corpus-refs.json.gz").read_bytes()))
    models = args.models or sorted(refs)
    models = [m for m in models if m not in skip]
    missing = [m for m in models if m not in refs]
    if missing:
        print(f"no reference recorded for: {' '.join(missing)}")
        return 2

    tmp = pathlib.Path(tempfile.mkdtemp(prefix="stanli_refs_"))
    failures = []
    worst_overall = ("", 0.0, 0)
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        futs = [pool.submit(check_model, m, refs[m], pdb, check_bin, tmp,
                            args.timeout, args.no_wa, args.no_lp)
                for m in models]
        for fut in concurrent.futures.as_completed(futs):
            model, status, rel, ulp, n, detail = fut.result()
            if status != "OK":
                failures.append((model, status, detail))
                print(f"{status} {model} {detail}")
                continue
            if (rel > worst_overall[1]
                    and refs[model].get("status") != "MISMATCH"):
                worst_overall = (model, rel, ulp)
            gate = gate_for(refs[model], args.max_rel)
            if rel >= gate:
                failures.append((model, f"rel {rel:.2e}", f"{ulp} ulp"))
                print(f"GATE {model}: {rel:.2e} ({ulp} ulp) over {n} "
                      f"values, allowed {gate:.1e}")

    ok = len(models) - len(failures)
    print(f"\n{ok}/{len(models)} models within {args.max_rel:.0e} of the "
          f"CmdStan references"
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
