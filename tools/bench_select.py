#!/usr/bin/env python3
"""Choose the corpus models that a compiler-pipeline change must time.

python3 tools/bench_select.py PDB OUT_DIR --base BUILD --candidate BUILD [--opcodes OP_X,...]

Each BUILD is a shipped-configuration build directory with bench_grad, dump_ops
and dump_islands. Every corpus model is compiled with each arm's own compiler
and prepared by each arm's runtime. A model is selected when its prepared
program differs, when it runs a named opcode, when its preparation gets slower,
when any step fails in either arm, or when it is a control. OUT_DIR/selection.json
keeps the evidence, and its "models" field is the --models list for
harnesses/corpus_bench.py. See docs/benchmarks.md#selected-models.
"""
import argparse
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import math
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools"))
from corpus_inventory import corpus_cases, materialize_data

CONTROLS = ("i319_pois_re", "sw_ar", "eight_schools_noncentered", "hmm_example",
            "one_comp_mm_elim_abs", "gpcm_latent_reg_irt", "cm_lba2",
            "mlumr_survival_mspline_relaxed", "hierarchical_gp", "kronecker_gp",
            "oncobayes2_blrm_exnex_combo3")
ARMS = ("base", "candidate")
ALL_OPS = "1000000000"
THREAD_ENV = {k: "1" for k in ("STAN_NUM_THREADS", "OMP_NUM_THREADS", "OPENBLAS_NUM_THREADS",
                               "MKL_NUM_THREADS", "VECLIB_MAXIMUM_THREADS")}


def opcode_counts(ops_text):
    return Counter(re.findall(r"^ *\d+ (\w+) +v=", ops_text, re.M))


def short_opcode(name):
    return name[3:] if name.startswith("OP_") else name


def used_opcodes(ops_text, wanted, islands_text=""):
    present = set(opcode_counts(ops_text))
    present.update(re.findall(r"\bCALL +OP_(\w+)", islands_text))
    return [name for name in map(short_opcode, wanted) if name in present]


def table_opcodes(optable_text):
    return set(re.findall(r"X\(OP_(\w+)\)", optable_text))


def program_change(base, candidate):
    """One line on how the candidate's prepared program differs, or None."""
    if base["ops"] != candidate["ops"]:
        old, new = opcode_counts(base["ops"]), opcode_counts(candidate["ops"])
        deltas = [(f"-{name}", old[name] - new[name]) for name in old if old[name] > new[name]]
        deltas += [(f"+{name}", new[name] - old[name]) for name in new if new[name] > old[name]]
        text = ", ".join(sign + (f" x{n}" if n > 1 else "") for sign, n in deltas)
        return (f"ops {sum(old.values())} -> {sum(new.values())}: "
                + (text or "same opcodes, different operands"))
    if base["islands"] != candidate["islands"]:
        return "islands differ"
    if base["mir"] != candidate["mir"]:
        return "MIR differs; programs identical"
    return None


def prep_regressed(base_s, candidate_s):
    return candidate_s - base_s > max(0.02 * base_s, 0.005)


def selection(models, changes, opcode_users, prep_flags, failures, controls):
    unknown = sorted(set(controls) - set(models))
    if unknown:
        raise ValueError("control models are not in the corpus: " + ", ".join(unknown))
    chosen = {}
    for name in models:
        reasons = []
        if name in changes:
            reasons.append("program: " + changes[name])
        reasons += ["uses " + opcode for opcode in opcode_users.get(name, [])]
        if name in prep_flags:
            reasons.append(prep_flags[name])
        if name in failures:
            reasons.append("unresolved: " + failures[name])
        if name in controls:
            reasons.append("control")
        if reasons:
            chosen[name] = reasons
    return chosen


def sha(path):
    return hashlib.sha256(pathlib.Path(path).read_bytes()).hexdigest()


def run(argv, timeout):
    try:
        done = subprocess.run([str(a) for a in argv], capture_output=True, text=True,
                              timeout=timeout, env={**os.environ, **THREAD_ENV})
    except subprocess.TimeoutExpired:
        return None, "timed out"
    if done.returncode:
        return None, f"exit {done.returncode}"
    return done.stdout, None


def arm_tools(build):
    build = build.absolute()
    cache = (build / "CMakeCache.txt").read_text()
    source = pathlib.Path(re.search(r"^CMAKE_HOME_DIRECTORY:INTERNAL=(.*)$", cache, re.M).group(1))
    embed = subprocess.check_output(
        ["bash", "-c", "source tools/stanc_embed/provenance.sh && stanc_embed_dir"],
        cwd=source, text=True).strip()
    tools = dict(build=build, source=source, probe=pathlib.Path(embed) / "stanli-vectorize-probe",
                 bench=build / "bench_grad", dump_ops=build / "dump_ops",
                 dump_islands=build / "dump_islands")
    for key in ("probe", "bench", "dump_ops", "dump_islands"):
        if not tools[key].exists():
            raise SystemExit(f"missing {tools[key]}")
    return tools


def identity(tools):
    head = subprocess.run(["git", "-C", tools["source"], "rev-parse", "HEAD"],
                          capture_output=True, text=True).stdout.strip()
    dirty = subprocess.run(["git", "-C", tools["source"], "status", "--porcelain",
                            "--untracked-files=no"], capture_output=True, text=True).stdout
    return dict(source=str(tools["source"]), head=head, dirty=bool(dirty.strip()),
                **{k + "_sha256": sha(tools[k]) for k in ("probe", "bench", "dump_ops",
                                                           "dump_islands")})


def prepare(arm, tools, name, stan, data, out):
    """Compile and dump one model with one arm; returns (program, failure)."""
    work = out / arm / name
    work.mkdir(parents=True, exist_ok=True)
    source, mir = work / f"{name}.stan", work / "model.sexp"
    shutil.copyfile(stan, source)
    steps = (("compile", [tools["probe"], "--vectorize-loops", "on", "--output", mir, source], 900),
             ("dump_ops", [tools["dump_ops"], mir, data, ALL_OPS], 300),
             ("dump_islands", [tools["dump_islands"], mir, data], 300))
    texts = {}
    for step, argv, timeout in steps:
        text, error = run(argv, timeout)
        if error:
            return None, f"{arm} {step} failed ({error})"
        texts[step] = text
    (work / "ops.txt").write_text(texts["dump_ops"])
    (work / "islands.txt").write_text(texts["dump_islands"])
    return dict(mir=sha(mir), ops=texts["dump_ops"], islands=texts["dump_islands"]), None


def prep_seconds(tools, mir, data):
    text, error = run([tools["bench"], mir, data, "--prep"], 300)
    if error:
        return None, error
    try:
        value = float(text.splitlines()[-1].split()[0])
    except (IndexError, ValueError):
        return None, "unreadable output"
    return (value, None) if math.isfinite(value) and value >= 0 else (None, "bad duration")


def time_preparation(names, arms, data, out, reps):
    """Best of reps per arm, one process at a time, arms alternating."""
    times, failures = {}, {}
    for count, name in enumerate(names, 1):
        best = {arm: math.inf for arm in ARMS}
        for rep in range(reps):
            for arm in ARMS if rep % 2 == 0 else ARMS[::-1]:
                value, error = prep_seconds(arms[arm], out / arm / name / "model.sexp", data[name])
                if error:
                    failures[name] = f"{arm} preparation failed ({error})"
                    break
                best[arm] = min(best[arm], value)
            if name in failures:
                break
        if name not in failures:
            times[name] = {arm + "_s": best[arm] for arm in ARMS}
        if count % 50 == 0:
            print(f"timed preparation for {count}/{len(names)} models", flush=True)
    return times, failures


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("pdb", type=pathlib.Path)
    parser.add_argument("out", type=pathlib.Path)
    parser.add_argument("--base", type=pathlib.Path, required=True)
    parser.add_argument("--candidate", type=pathlib.Path, required=True)
    parser.add_argument("--opcodes", default="",
                        help="comma-separated opcodes whose kernels the change edits")
    parser.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) - 4))
    parser.add_argument("--prep-reps", type=int, default=3)
    args = parser.parse_args(argv)
    arms = {"base": arm_tools(args.base), "candidate": arm_tools(args.candidate)}
    opcodes = [short_opcode(o.strip()) for o in args.opcodes.split(",") if o.strip()]
    known = table_opcodes((arms["candidate"]["source"]
                           / "runtime/include/stanli/optable.hpp").read_text())
    if set(opcodes) - known:
        parser.error("unknown opcodes: " + ", ".join(sorted(set(opcodes) - known)))
    out = args.out.absolute()
    (out / "inputs").mkdir(parents=True, exist_ok=True)

    cases = corpus_cases(args.pdb, "all")
    names = sorted(cases)
    data = {name: out / "inputs" / f"{name}.json" for name in names}
    with tempfile.TemporaryDirectory() as unzipped:
        for name in names:
            shutil.copyfile(materialize_data(cases[name], unzipped), data[name])

    with ThreadPoolExecutor(args.jobs) as pool:
        jobs = {(arm, name): pool.submit(prepare, arm, arms[arm], name, cases[name].source,
                                         data[name], out)
                for name in names for arm in ARMS}
    programs, failures = {}, {}
    for (arm, name), job in jobs.items():
        program, failure = job.result()
        if failure:
            failures[name] = "; ".join(filter(None, [failures.get(name), failure]))
        programs[arm, name] = program
    prepared = [n for n in names if n not in failures]
    changes = {n: c for n in prepared
               if (c := program_change(programs["base", n], programs["candidate", n]))}
    opcode_users = {n: used for n in prepared
                    if (used := sorted({u for arm in ARMS
                                     for u in used_opcodes(programs[arm, n]["ops"], opcodes,
                                                           programs[arm, n]["islands"])}))}

    times, prep_failures = time_preparation(prepared, arms, data, out, args.prep_reps)
    failures.update(prep_failures)
    prep_flags = {n: f"preparation {t['base_s'] * 1e3:.1f} -> {t['candidate_s'] * 1e3:.1f} ms"
                  for n, t in times.items() if prep_regressed(t["base_s"], t["candidate_s"])}
    chosen = selection(names, changes, opcode_users, prep_flags, failures, CONTROLS)

    ratios = [t["base_s"] / t["candidate_s"] for t in times.values()
              if t["base_s"] > 0 and t["candidate_s"] > 0]
    geomean = math.exp(sum(map(math.log, ratios)) / len(ratios)) if ratios else None
    report = dict(arms={arm: identity(arms[arm]) for arm in ARMS}, opcodes=opcodes,
                  controls=list(CONTROLS), prep_reps=args.prep_reps,
                  counts=dict(models=len(names), changed=len(changes),
                              opcode_users=len(opcode_users), prep_timed=len(times),
                              prep_regressed=len(prep_flags), failures=len(failures),
                              selected=len(chosen)),
                  prep_geomean_speedup=geomean, changed=changes, opcode_users=opcode_users,
                  prep_regressed=prep_flags, failures=failures, preparation=times,
                  selected=chosen, models=",".join(sorted(chosen)))
    (out / "selection.json").write_text(json.dumps(report, indent=1, sort_keys=True) + "\n")

    print(f"{len(names)} models; {len(changes)} changed programs, {len(opcode_users)} use "
          f"{','.join(opcodes) or 'no named opcodes'}, {len(prep_flags)} slower preparations, "
          f"{len(failures)} failures; {len(chosen)} selected")
    if geomean:
        print(f"preparation over {len(times)} models: geomean {geomean:.3f}x faster")
    for name, reasons in sorted(chosen.items()):
        print(f"  {name}: {'; '.join(reasons)}")
    print("--models " + report["models"])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
