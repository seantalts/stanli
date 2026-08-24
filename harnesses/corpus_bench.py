#!/usr/bin/env python3
"""Corpus-wide head-to-head: stanli vs CmdStan on every posteriordb model.

Per model, both engines get one column each for
  - model preparation (stanli: file read + parse + compile + bind;
    CmdStan: stanc + full make)
  - per-gradient latency
  - end to end 1000 warmup + 1000 draws
Results stream to a TSV as they complete, so a partial run is still
useful and a rerun can skip what is already there.

Usage: python3 harnesses/corpus_bench.py deps/cmdstan deps/posteriordb OUT.tsv
                                      [--filter SUBSTR] [--timeout SEC]
                                      [--stanli-only]
Needs build-rel/ built. Expect hours: CmdStan builds a binary per model.

--stanli-only re-measures the stanli columns of every EXISTING row in place
and keeps the CmdStan columns as they are. That is the refresh mode for a
stanli-side change (a new graph pass, a sampler fix): the CmdStan numbers
are unaffected and rebuilding 120 model binaries to reproduce them is the
expensive part of a full run.
"""
import json
import os
import pathlib
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
COLS = ["model", "params", "stanli_prep_s", "stanli_ns_grad",
        "stanli_sample_s", "cmdstan_build_s", "cmdstan_ns_grad",
        "cmdstan_sample_s", "note"]


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


def main():
    cs = pathlib.Path(sys.argv[1]).resolve()
    pdb = pathlib.Path(sys.argv[2]) / "posterior_database"
    out_path = pathlib.Path(sys.argv[3])
    filt = (sys.argv[sys.argv.index("--filter") + 1]
            if "--filter" in sys.argv else "")
    timeout = int(sys.argv[sys.argv.index("--timeout") + 1]
                  if "--timeout" in sys.argv else 900)
    stanli_only = "--stanli-only" in sys.argv
    tmp = pathlib.Path(tempfile.mkdtemp(prefix="stanli_cb_"))

    done = set()
    old_rows = {}
    if out_path.exists():
        for line in out_path.read_text().splitlines()[1:]:
            parts = line.split("\t")
            done.add(parts[0])
            old_rows[parts[0]] = dict(zip(COLS, parts))
    else:
        out_path.write_text("\t".join(COLS) + "\n")
    if stanli_only:
        done = set()  # revisit every row; CmdStan columns carry over

    pairs = {}
    for pj in sorted((pdb / "posteriors").glob("*.json")):
        meta = json.loads(pj.read_text())
        pairs.setdefault(meta["model_name"], meta["data_name"])

    for model, dname in sorted(pairs.items()):
        if (filt and filt not in model) or model in done:
            continue
        stan = pdb / "models" / "stan" / f"{model}.stan"
        dz = pdb / "data" / "data" / f"{dname}.json.zip"
        if not stan.exists() or not dz.exists():
            continue
        dj = tmp / f"{model}.json"
        with zipfile.ZipFile(dz) as z:
            dj.write_bytes(z.read(z.namelist()[0]))
        row = {c: "" for c in COLS}
        row["model"] = model
        notes = []

        # ---- stanli ----
        sexp = tmp / f"{model}.sexp"
        r = run([str(STANC), "--O1", "--debug-optimized-mir", str(stan)], timeout)
        if r is None:
            notes.append("stanc_fail")
        else:
            sexp.write_text(r.stdout)
            probe = run([str(BENCH), str(sexp), str(dj), "1"], timeout)
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
                prep = run([str(BENCH), str(sexp), str(dj), "--prep"], timeout)
                prep_lines = ([line for line in prep.stdout.splitlines()
                               if line.strip()]
                              if prep and prep.returncode == 0 else [])
                if prep_lines:
                    row["stanli_prep_s"] = (
                        f"{float(prep_lines[-1].split()[0]):.3f}")
                else:
                    notes.append("stanli_prep_fail")
                g = run([str(BENCH), str(sexp), str(dj),
                         str(evals_for(n_params))], timeout)
                if g:
                    row["stanli_ns_grad"] = f"{float(g.stdout.split()[0]):.0f}"
                t0 = time.perf_counter()
                s, st = run2([str(RUN), str(stan), str(dj), "--warmup",
                              "1000", "--samples", "1000", "--seed", "1"],
                             timeout)
                if st == "ok":
                    row["stanli_sample_s"] = f"{time.perf_counter() - t0:.2f}"
                elif st == "timeout":
                    notes.append("stanli_sample_timeout")
                else:
                    err = (s.stderr.strip().splitlines() or [""])[-1][:60]
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
                    f.write("\t".join(str(old_rows[m].get(c, ""))
                                      for c in COLS) + "\n")
            print(f"{model}: stanli {row['stanli_ns_grad']}ns/"
                  f"{row['stanli_sample_s']}s  {row['note']}", flush=True)
            continue

        # ---- CmdStan: real model binary, built the way users build it ----
        work = tmp / model
        work.mkdir(exist_ok=True)
        (work / f"{model}.stan").write_text(stan.read_text())
        exe = work / model
        t0 = time.perf_counter()
        b = run(["make", str(exe)], timeout, cwd=str(cs))
        row["cmdstan_build_s"] = f"{time.perf_counter() - t0:.1f}"
        if b is None:
            notes.append("cmdstan_build_fail")
        else:
            # CmdStan's make compiles the generated header without leaving
            # it behind, so emit our own copy for the gradient driver.
            hpp = work / f"{model}.hpp"
            if run([str(STANC), str(work / f"{model}.stan"), f"--o={hpp}"],
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
                    if g:
                        row["cmdstan_ns_grad"] = f"{float(g.stdout.split()[0]):.0f}"
                    else:
                        notes.append("cmdstan_grad_fail")
            t0 = time.perf_counter()
            s, st = run2([str(exe), "sample", "num_warmup=1000",
                          "num_samples=1000", "random", "seed=1",
                          "data", f"file={dj}",
                          "output", f"file={work}/out.csv"], timeout)
            if st == "ok":
                row["cmdstan_sample_s"] = f"{time.perf_counter() - t0:.2f}"
            else:
                notes.append(f"cmdstan_sample_{st}")

        row["note"] = ",".join(notes)
        with out_path.open("a") as f:
            f.write("\t".join(str(row[c]) for c in COLS) + "\n")
        print(f"{model}: stanli {row['stanli_ns_grad']}ns/"
              f"{row['stanli_sample_s']}s  cmdstan {row['cmdstan_ns_grad']}ns/"
              f"{row['cmdstan_sample_s']}s  {row['note']}", flush=True)


if __name__ == "__main__":
    main()
