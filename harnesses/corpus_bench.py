#!/usr/bin/env python3
"""Corpus-wide head-to-head: stanli vs CmdStan on posteriordb and educational models.

Per model, both engines get one column each for
  - model preparation (stanli: file read + parse + compile + bind;
    CmdStan: stanc + full make)
  - per-gradient latency
  - end to end 1000 warmup + 1000 draws
Results stream to a TSV as they complete, so a partial run is still
useful and a rerun can skip what is already there.

Usage: python3 harnesses/corpus_bench.py deps/cmdstan deps/posteriordb OUT.tsv
                                      [--filter SUBSTR] [--timeout SEC]
                                      [--corpus all|posteriordb|educational]
                                      [--stanli-only] [--no-sample]
                                      [--cmdstan-stanc PATH]
                                      [--stancflags FLAGS]
Needs build-rel/ built. Expect hours: CmdStan builds a binary per model.
Both corpora run by default. --corpus selects one collection; --filter aalto_
selects the 13 checked-in educational models without reading posteriordb data.
The stricter paired educational performance gate remains tools/check_educational.py.

The stanli gradient and prep columns compile with
deps/stanc3/stanli-vectorize-probe and loop vectorization on, which is the
MIR the embedded compiler produces. The sample column runs stanli_run from
source.

--stanli-only re-measures the stanli columns of every EXISTING row in place
and keeps the CmdStan columns as they are. That is the refresh mode for a
stanli-side change (a new graph pass, a sampler fix): the CmdStan numbers
are unaffected and rebuilding 120 model binaries to reproduce them is the
expensive part of a full run.

--cmdstan-stanc PATH installs PATH as deps/cmdstan/bin/stanc for the run
(the original goes back afterwards) and also emits the gradient driver's
header with it. --stancflags FLAGS reaches make as STANCFLAGS and is
appended to the header command. Without them the make build uses
CmdStan's own bin/stanc, the header deps/stanc3/stanc, both with no flags.
OUT.manifest.json next to the TSV records the stanc binaries and flags
behind the CmdStan columns.

--no-sample skips both samplers and leaves stanli_sample_s, stanli_grads,
and cmdstan_sample_s empty. Use it to measure gradient and compile time
without paying for a full 1000 warmup + 1000 draw run per model.
"""
import contextlib
import csv
import hashlib
import json
import os
import pathlib
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import time
import zipfile

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools"))
from cmdstan_ref import compile_cmd  # noqa: E402

BENCH = REPO / "build-rel/bench_grad"
RUN = REPO / "build-rel/stanli_run"
STANC = REPO / "deps/stanc3/stanc"
VECTORIZE_PROBE = REPO / "deps/stanc3/stanli-vectorize-probe"
COLS = ["model", "params", "stanli_prep_s", "stanli_ns_grad",
        "stanli_sample_s", "stanli_grads", "cmdstan_build_s",
        "cmdstan_ns_grad", "cmdstan_sample_s", "note"]

GRAD_COUNT_RE = re.compile(r"stanli_run: (\d+) gradient evaluations")


def parse_grad_count(stderr_text):
    m = GRAD_COUNT_RE.search(stderr_text or "")
    return m.group(1) if m else ""


def row_line(row):
    values = [str(row.get(c, "")) for c in COLS]
    # A literal trailing tab is valid TSV for an empty final field, but it is
    # also trailing whitespace to git. Quoting the empty field preserves the
    # same csv.DictReader value while keeping benchmark diffs checkable.
    if not values[-1]:
        values[-1] = '""'
    return "\t".join(values) + "\n"


def upgrade_header(fieldnames, rows):
    if fieldnames == COLS:
        return None
    return ("\t".join(COLS) + "\n"
            + "".join(row_line(rows[m]) for m in sorted(rows)))


# Returns (result, status): status is "ok", "fail" (non-zero exit) or
# "timeout". Collapsing the last two loses the distinction between "this
# model is too slow" and "this model does not run", which is exactly the
# thing a corpus sweep exists to tell apart.
def run2(cmd, timeout, cwd=None):
    try:
        r = subprocess.run(cmd, capture_output=True, text=True,
                           timeout=timeout, cwd=cwd, env=dict(os.environ))
        return (r, "ok") if r.returncode == 0 else (r, "fail")
    except subprocess.TimeoutExpired:
        return None, "timeout"
    except OSError:
        return None, "fail"


def run(cmd, timeout, cwd=None):
    r, _ = run2(cmd, timeout, cwd)
    return r


def evals_for(n):
    return 300 if n > 2000 else 3000 if n > 200 else 20000


def option(name, default):
    return sys.argv[sys.argv.index(name) + 1] if name in sys.argv else default


def stanc_info(path):
    if not path.exists():
        return {"path": str(path), "sha256": None, "version": ""}
    v = run([str(path), "--version"], 60)
    return {"path": str(path),
            "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
            "version": v.stdout.strip() if v else ""}


def cmdstan_manifest(cs, cmdstan_stanc, stancflags):
    return {"cmdstan": {
        "make_stanc": stanc_info(cmdstan_stanc or cs / "bin" / "stanc"),
        "header_stanc": stanc_info(cmdstan_stanc or STANC),
        "stancflags": stancflags,
    }}


@contextlib.contextmanager
def installed_stanc(cs, stanc):
    """Put `stanc` at cs/bin/stanc for the duration, then restore."""
    if stanc is None:
        yield
        return
    target = cs / "bin" / "stanc"
    aside = cs / "bin" / "stanc.corpus_bench_orig"
    if aside.exists():
        raise SystemExit(f"{aside} exists: an earlier run did not restore "
                         f"{target}; move it back by hand first")
    had_original = target.exists()
    if had_original:
        target.rename(aside)
    try:
        shutil.copy2(stanc, target)
        yield
    finally:
        target.unlink(missing_ok=True)
        if had_original:
            aside.rename(target)


def benchmark_cases(pdb, corpus="all"):
    """Return source/data paths; keep the established first dataset per PDB model."""
    if corpus not in {"all", "posteriordb", "educational"}:
        raise ValueError(f"Unknown benchmark corpus: {corpus}")
    cases = {}
    if corpus != "educational":
        for pj in sorted((pdb / "posteriors").glob("*.json")):
            meta = json.loads(pj.read_text())
            model = meta["model_name"]
            cases.setdefault(model, (
                pdb / "models" / "stan" / f"{model}.stan",
                pdb / "data" / "data" / f"{meta['data_name']}.json.zip"))
    if corpus != "posteriordb":
        from check_educational import files, inventory
        for model in inventory():
            if model in cases:
                raise ValueError(f"Duplicate benchmark model: {model}")
            cases[model] = files(model)
    return cases


def materialize_data(source, destination):
    if source.suffix == ".zip":
        with zipfile.ZipFile(source) as archive:
            destination.write_bytes(archive.read(archive.namelist()[0]))
    else:
        destination.write_bytes(source.read_bytes())


def selected_case(model, filt, done, stanli_only, old_rows):
    return ((not filt or filt in model) and model not in done
            and (not stanli_only or model in old_rows))


def main():
    cs = pathlib.Path(sys.argv[1]).resolve()
    pdb = pathlib.Path(sys.argv[2]) / "posterior_database"
    out_path = pathlib.Path(sys.argv[3])
    filt = option("--filter", "")
    cases = benchmark_cases(pdb, option("--corpus", "all"))
    timeout = int(option("--timeout", 900))
    stanli_only = "--stanli-only" in sys.argv
    no_sample = "--no-sample" in sys.argv
    cmdstan_stanc = option("--cmdstan-stanc", None)
    if cmdstan_stanc is not None:
        cmdstan_stanc = pathlib.Path(cmdstan_stanc).resolve()
        if not cmdstan_stanc.is_file():
            raise SystemExit(f"--cmdstan-stanc: {cmdstan_stanc} is not a file")
    stancflags = option("--stancflags", "")
    header_stanc = cmdstan_stanc or STANC
    tmp = pathlib.Path(tempfile.mkdtemp(prefix="stanli_cb_"))

    done = set()
    old_rows = {}
    if out_path.exists():
        with out_path.open(newline="") as f:
            reader = csv.DictReader(f, delimiter="\t")
            for row in reader:
                done.add(row["model"])
                old_rows[row["model"]] = row
            fieldnames = reader.fieldnames
        text = upgrade_header(fieldnames, old_rows)
        if text is not None:
            out_path.write_text(text)
    else:
        out_path.write_text("\t".join(COLS) + "\n")
    if stanli_only:
        done = set()  # revisit every row; CmdStan columns carry over
    else:
        manifest_path = out_path.with_suffix(".manifest.json")
        manifest = cmdstan_manifest(cs, cmdstan_stanc, stancflags)
        if old_rows:
            # A TSV without a manifest predates the option: defaults.
            recorded = (json.loads(manifest_path.read_text())
                        if manifest_path.exists()
                        else cmdstan_manifest(cs, None, ""))
            if recorded != manifest:
                raise SystemExit(f"{out_path} holds CmdStan columns from "
                                 "another stanc or flags; use a new output "
                                 "TSV")
        manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")

    with installed_stanc(cs, None if stanli_only else cmdstan_stanc):
        for model, (stan, data_source) in sorted(cases.items()):
            if not selected_case(model, filt, done, stanli_only, old_rows):
                continue
            if not stan.exists() or not data_source.exists():
                raise FileNotFoundError(f"Missing benchmark source/data for {model}")
            dj = tmp / f"{model}.json"
            materialize_data(data_source, dj)
            row = {c: "" for c in COLS}
            row["model"] = model
            notes = []

            # ---- stanli ----
            mir = tmp / f"{model}.mir"
            r = run([str(VECTORIZE_PROBE), "--vectorize-loops", "on",
                     "--output", str(mir), str(stan)], timeout)
            if r is None or not mir.exists():
                notes.append("stanc_fail")
            else:
                probe = run([str(BENCH), str(mir), str(dj), "1"], timeout)
                # A rejected model (sir: domain error at the probe point) can
                # exit 0 with nothing on stdout; treat that as eval_fail too.
                if probe is None or not probe.stdout.split():
                    notes.append("stanli_eval_fail")
                else:
                    n_params = int(probe.stdout.split()[-1])
                    row["params"] = n_params
                    # Compile and bind only. The old `1` invocation also ran a
                    # time-capped warmup plus one measured gradient, which made
                    # this column depend on model runtime and mislabeled ~200 ms
                    # as preparation even on small models.
                    prep = run([str(BENCH), str(mir), str(dj), "--prep"],
                               timeout)
                    prep_lines = ([line for line in prep.stdout.splitlines()
                                   if line.strip()]
                                  if prep and prep.returncode == 0 else [])
                    if prep_lines:
                        row["stanli_prep_s"] = (
                            f"{float(prep_lines[-1].split()[0]):.6f}")
                    else:
                        notes.append("stanli_prep_fail")
                    g = run([str(BENCH), str(mir), str(dj),
                             str(evals_for(n_params))], timeout)
                    if g:
                        row["stanli_ns_grad"] = (
                            f"{float(g.stdout.split()[0]):.0f}")
                    if not no_sample:
                        t0 = time.perf_counter()
                        s, st = run2([str(RUN), str(stan), str(dj),
                                      "--warmup", "1000", "--samples", "1000",
                                      "--seed", "1"], timeout)
                        if st == "ok":
                            row["stanli_sample_s"] = (
                                f"{time.perf_counter() - t0:.2f}")
                            row["stanli_grads"] = parse_grad_count(s.stderr)
                        elif st == "timeout":
                            notes.append("stanli_sample_timeout")
                        else:
                            row["stanli_grads"] = parse_grad_count(s.stderr)
                            err = (s.stderr.strip().splitlines()
                                   or [""])[-1][:60]
                            notes.append(f"stanli_sample_fail({err})")

            if stanli_only:
                old = old_rows.get(model, {})
                for c in ("cmdstan_build_s", "cmdstan_ns_grad",
                          "cmdstan_sample_s"):
                    row[c] = old.get(c, "")
                notes += [n for n in old.get("note", "").split(",")
                          if n.startswith("cmdstan")]
                row["note"] = ",".join(n for n in notes if n)
                old_rows[model] = row
                # Rewrite in place so a partial refresh is still a coherent file.
                with out_path.open("w") as f:
                    f.write("\t".join(COLS) + "\n")
                    for m in sorted(old_rows):
                        f.write(row_line(old_rows[m]))
                print(f"{model}: stanli {row['stanli_ns_grad']}ns/"
                      f"{row['stanli_sample_s']}s  {row['note']}", flush=True)
                continue

            # ---- CmdStan: real model binary, built the way users build it ----
            work = tmp / model
            work.mkdir(exist_ok=True)
            (work / f"{model}.stan").write_text(stan.read_text())
            exe = work / model
            make = ["make", str(exe)]
            if stancflags:
                make.append(f"STANCFLAGS={stancflags}")
            t0 = time.perf_counter()
            b = run(make, timeout, cwd=str(cs))
            row["cmdstan_build_s"] = f"{time.perf_counter() - t0:.1f}"
            if b is None:
                notes.append("cmdstan_build_fail")
            else:
                # CmdStan's make compiles the generated header without leaving
                # it behind, so emit our own copy for the gradient driver.
                hpp = work / f"{model}.hpp"
                if run([str(header_stanc), str(work / f"{model}.stan"),
                        f"--o={hpp}", *shlex.split(stancflags)],
                       timeout) and hpp.exists():
                    gexe = work / "gradbench"
                    cmd = compile_cmd(cs, hpp,
                                      REPO / "tools/bench_cmdstan_grad.cpp",
                                      gexe, opt="-O3")
                    if not run(cmd, timeout):
                        notes.append("cmdstan_grad_build_fail")
                    else:
                        n_params = int(row["params"] or 0)
                        g = run([str(gexe), str(dj), str(evals_for(n_params))],
                                timeout)
                        if g and g.stdout.split():
                            row["cmdstan_ns_grad"] = (
                                f"{float(g.stdout.split()[0]):.0f}")
                        else:
                            notes.append("cmdstan_grad_fail")
                if not no_sample:
                    t0 = time.perf_counter()
                    s, st = run2([str(exe), "sample", "num_warmup=1000",
                                  "num_samples=1000", "random", "seed=1",
                                  "data", f"file={dj}",
                                  "output", f"file={work}/out.csv"], timeout)
                    if st == "ok":
                        row["cmdstan_sample_s"] = (
                            f"{time.perf_counter() - t0:.2f}")
                    else:
                        notes.append(f"cmdstan_sample_{st}")

            row["note"] = ",".join(notes)
            with out_path.open("a") as f:
                f.write(row_line(row))
            print(f"{model}: stanli {row['stanli_ns_grad']}ns/"
                  f"{row['stanli_sample_s']}s  "
                  f"cmdstan {row['cmdstan_ns_grad']}ns/"
                  f"{row['cmdstan_sample_s']}s  {row['note']}", flush=True)


if __name__ == "__main__":
    main()
