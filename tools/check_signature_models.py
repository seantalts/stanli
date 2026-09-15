#!/usr/bin/env python3
"""Compare the generated signature models with recorded CmdStan answers.

The builtin and density signature models are build products generated from
the pinned stanc inventory and the runtime registry; the committed manifests
name every model and its source SHA-256. This gate replays each generated
model against a recorded CmdStan reference at the same three deterministic
points as the corpus replay: log density, the full gradient, the output
column schema, and every output value. Case k of a partition reads its
probe from parameter k and writes output k, so gradient element k and
generated_quantities_result.k compare that one overload on its own.

--record compiles every reference from the pinned CmdStan checkout
(tools/dev_setup.sh --corpus provisions it) and is incremental: a model
whose manifest source hash already matches its recorded reference is kept.
A changed toolchain invalidates the whole artifact so one file never mixes
reference provenances. Replay refuses stale inputs in either direction: a
generated model that disagrees with its manifest means the generators must
be rerun, and a manifest that disagrees with the reference artifact means
coverage moved and the reference must be re-recorded with the change.
Recording is independent of stanli's answers and a mismatch still fails;
never re-record to hide a stanli regression.

A failure names the model and the case whose gradient element or output
column diverged. Replay a subset of densities with
tools/generate_density_signature_model.py --filter/--start/--count into a
scratch --output-dir/--manifest, then point this script's --manifest and
--reference at those scratch files and --record.
"""
from __future__ import annotations

import argparse
import concurrent.futures
import gzip
import hashlib
import json
import os
import pathlib
import platform
import re
import subprocess
import sys

from check_function_models import digest, parse, source_digest
from cmdstan_ref import compile_cmd

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "harnesses"))
from function_signature_common import CONTEXT_DATA  # noqa: E402

REPO = pathlib.Path(__file__).resolve().parents[1]
MANIFESTS = (REPO / "tests/function_coverage/builtin_signatures_manifest.json",
             REPO / "tests/function_coverage/density_signatures_manifest.json")
REFERENCE = REPO / "tests/function_coverage/signature_references.json.gz"
LEDGER = REPO / "tests/function_coverage/signature_ledger.json"
POINTS = 3
# The corpus replay's gate (check_function_models.py).
GATE = 1e-9
# Case comments in the generated sources, one per case block in order.
CASE = re.compile(r"// (\S+\(\S*=>\S+)$", re.MULTILINE)


def run(argv, timeout=600, env=None):
    result = subprocess.run([str(x) for x in argv], cwd=REPO, env=env,
                            text=True, capture_output=True, timeout=timeout)
    if result.returncode:
        raise RuntimeError(f"{argv[0]} failed ({result.returncode}):\n"
                           + result.stdout[-3000:] + result.stderr[-3000:])
    return result.stdout


def manifest_models(manifest_paths):
    """name -> (source, manifest hash, function names) per manifest."""
    models = {}
    for manifest_path in manifest_paths:
        manifest = json.loads(manifest_path.read_text())
        for model in manifest["models"]:
            source = REPO / model["file"]
            models[source.stem] = (source, model["source_sha256"],
                                   set(model["functions"]))
    return models


def load_ledger(models):
    """Known-divergence entries, keyed by the function name they excuse.

    Mirrors the cross-path ledger: a comparison beyond the gate passes only
    when the model contains a ledgered function and the error stays within
    that entry's recorded bound, so a new divergence is still reported.
    A ledgered name absent from every manifest is stale and fails loudly.
    """
    if not LEDGER.exists():
        return {}
    ledger = json.loads(LEDGER.read_text())
    if ledger.get("schema") != 1:
        raise ValueError("Unknown signature ledger schema")
    entries = {}
    for entry in ledger["entries"]:
        if not entry.get("reason"):
            raise ValueError(f"ledger entry {entry['name']} needs a reason")
        if not GATE < entry["bound"] < 1:
            raise ValueError(f"ledger bound for {entry['name']} must sit "
                             f"between the {GATE:g} gate and 1")
        entries[entry["name"]] = float(entry["bound"])
    known = set().union(*(functions for _, _, functions in
                          models.values()))
    stale = sorted(set(entries) - known)
    if stale:
        raise ValueError("signature ledger names no generated signature: "
                         + ", ".join(stale))
    return entries


def checked_source(name, source, manifest_sha):
    if not source.exists():
        raise ValueError(f"{name}: {source} is not generated yet; build the "
                         "stanli_signature_models target first")
    if source_digest(source) != manifest_sha:
        raise ValueError(f"{name}: generated source disagrees with its "
                         "manifest; rerun the signature generators")
    return source


def toolchain_block(args):
    block = {
        "stanc_sha256": digest(args.stanc),
        "stanc_version": run([args.stanc, "--version"]).strip(),
        "compiler": run(["clang++", "--version"]).splitlines()[0],
        "platform": platform.system() + " " + platform.machine(),
        "flags": "-std=c++17 -O1 -ffp-contract=off -D_REENTRANT "
                 "-DBOOST_DISABLE_ASSERTS",
    }
    for key, path in {"cmdstan": args.cmdstan,
                      "stan": args.cmdstan / "stan",
                      "math": args.cmdstan / "stan/lib/stan_math"}.items():
        block[key + "_commit"] = run(
            ["git", "-C", path, "rev-parse", "HEAD"]).strip()
    return block


def record_one(name, source, args):
    identity = hashlib.sha256(
        (source_digest(source) + source_digest(REPO / "tools/ref_driver.cpp")
         + json.dumps(args.toolchain, sort_keys=True)).encode()
    ).hexdigest()[:16]
    cache = args.build / "signature-refs" / (name + "-" + identity)
    cache.mkdir(parents=True, exist_ok=True)
    hpp, exe = cache / "model.hpp", cache / "reference"
    if not exe.exists():
        run([args.stanc, source, f"--o={hpp}"])
        # A partition renders a couple hundred overloads into one translation
        # unit, so the reference compile can far outlast an ordinary model's.
        run(compile_cmd(args.cmdstan, hpp, REPO / "tools/ref_driver.cpp", exe),
            timeout=7200)
    points = [parse(run([exe, args.data, p])) for p in range(POINTS)]
    print(f"recorded {name}", flush=True)
    return name, {"source_sha256": source_digest(source), "points": points}


def record(models, complete, args):
    args.toolchain = toolchain_block(args)
    kept = {}
    if args.reference.exists():
        previous = json.loads(gzip.decompress(args.reference.read_bytes()))
        if previous.get("toolchain") == args.toolchain:
            kept = {name: reference
                    for name, reference in previous["models"].items()
                    if (name not in models
                        and not complete)  # a partial run prunes nothing else
                    or (name in models
                        and reference["source_sha256"] == models[name][1])}
    todo = sorted(set(models) - set(kept))
    for name in todo:
        source, manifest_sha = models[name][0], models[name][1]
        checked_source(name, source, manifest_sha)

    def attempt(name):
        try:
            return record_one(name, models[name][0], args)
        except (RuntimeError, ValueError,
                subprocess.TimeoutExpired) as error:
            print(f"RECORD_FAIL {name}: {error}", flush=True)
            return name, None

    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        results = dict(pool.map(attempt, todo))
    recorded = {name: reference for name, reference in results.items()
                if reference is not None}
    # Keep every success even when a sibling fails, so a rerun after the
    # fix only records the models that still need it.
    payload = json.dumps(
        {"schema": 1, "toolchain": args.toolchain,
         "models": dict(sorted((kept | recorded).items()))},
        indent=2, sort_keys=True, allow_nan=False) + "\n"
    args.reference.write_bytes(gzip.compress(payload.encode(), mtime=0))
    print(f"recorded {len(recorded)} models, kept {len(kept)}", flush=True)
    failed = sorted(name for name, value in results.items() if value is None)
    if failed:
        raise RuntimeError("reference recording failed for: "
                           + ", ".join(failed))


def case_of(cases, field, index, label):
    """The case behind lp_grad element index (0 is lp itself) or output
    column label, or None for anything that is not one case's own."""
    if field == "lp_grad":
        position = index
    elif label.startswith("generated_quantities_result."):
        position = int(label.rsplit(".", 1)[1])
    else:
        return None
    if not 1 <= position <= len(cases):
        return None
    return cases[position - 1]


def compare(name, source, manifest_sha, functions, reference, ledger,
            args):
    checked_source(name, source, manifest_sha)
    if reference["source_sha256"] != manifest_sha:
        raise ValueError(name + ": coverage changed since its CmdStan "
                         "reference was recorded; re-record with --record")
    if len(reference["points"]) != POINTS:
        raise ValueError(f"{name}: expected exactly {POINTS} reference points")
    excused = {fn: bound for fn, bound in ledger.items() if fn in functions}
    used = set()
    worst = 0.0

    cases = CASE.findall(source.read_text())

    def evaluate(point):
        # The models exist to exercise the compiled paths; a section the
        # MIR interpreter took over would replay correctly and prove
        # nothing, so make that a compile error instead of a warning.
        return parse(run([args.build / "stanli_check", source, args.data,
                          "--point", point, "--wa-values"],
                         env={**os.environ, "STANLI_NO_INTERPRETER": "1"}))

    with concurrent.futures.ThreadPoolExecutor(max_workers=POINTS) as pool:
        evaluations = list(pool.map(evaluate, range(POINTS)))
    for point, want in enumerate(reference["points"]):
        got = evaluations[point]
        if got["names"] != want["names"]:
            raise ValueError(name + ": output column names/order differ")
        for field in ("lp_grad", "values"):
            if len(got[field]) != len(want[field]):
                raise ValueError(name + ": " + field + " width differs")
            for i, (a, b) in enumerate(zip(got[field], want[field])):
                error = abs(a - b) / max(1.0, abs(a), abs(b))
                worst = max(worst, error)
                if error <= GATE:
                    continue
                covering = {fn for fn, bound in excused.items()
                            if error <= bound}
                label = want["names"][i] if field == "values" else str(i)
                case = case_of(cases, field, i, label)
                if case:
                    label += " " + case
                if not covering:
                    raise ValueError(
                        f"{name} point {point} {field}[{label}]: "
                        f"stanli={a:.17g}, CmdStan={b:.17g}, scaled "
                        f"error={error:.3g}; the manifest records this "
                        "model's coverage")
                used.update(covering)
                print(f"LEDGER {name} point {point} {field}[{label}] "
                      f"error={error:.3g} within {', '.join(sorted(covering))}",
                      flush=True)
    print(f"PASS {name}: {POINTS} points, lp/gradient/all outputs; "
          f"worst={worst:.3g}", flush=True)
    return used


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=pathlib.Path, default=REPO / "build-rel")
    parser.add_argument("--stanc", type=pathlib.Path,
                        default=REPO / "deps/stanc3/stanc")
    parser.add_argument("--cmdstan", type=pathlib.Path,
                        default=REPO / "deps/cmdstan")
    parser.add_argument("--manifest", action="append", type=pathlib.Path,
                        help="signature manifest(s); default both committed")
    parser.add_argument("--reference", type=pathlib.Path, default=REFERENCE,
                        help="reference artifact; default the committed one")
    parser.add_argument("--model", action="append",
                        help="restrict to these model stems")
    parser.add_argument("--record", action="store_true")
    parser.add_argument("--jobs", type=int, default=32)
    args = parser.parse_args()
    args.build, args.stanc, args.cmdstan, args.reference = (
        p.resolve() for p in (args.build, args.stanc, args.cmdstan,
                              args.reference))
    args.data = args.build / "context_seed.json"
    text = json.dumps(CONTEXT_DATA) + "\n"
    if not args.data.exists() or args.data.read_text() != text:
        # Concurrent ctest replays share this file: land it with one rename
        # so a reader never sees it half written.
        staged = args.data.with_name(f"context_seed.{os.getpid()}.json")
        staged.write_text(text)
        os.replace(staged, args.data)
    models = manifest_models(args.manifest or MANIFESTS)
    ledger = load_ledger(models)
    if args.model:
        missing = set(args.model) - set(models)
        if missing:
            raise ValueError("not in any signature manifest: "
                             + ", ".join(sorted(missing)))
        models = {name: models[name] for name in args.model}
    if args.record:
        record(models, complete=not args.model and not args.manifest,
               args=args)
    if not args.reference.exists():
        raise ValueError(f"{args.reference} is missing; record it against "
                         "the pinned CmdStan checkout with --record")
    references = json.loads(gzip.decompress(args.reference.read_bytes()))
    if references.get("schema") != 1:
        raise ValueError("Unknown signature reference schema")
    absent = sorted(set(models) - set(references["models"]))
    if absent:
        raise ValueError("models without a recorded CmdStan reference "
                         "(re-record with --record): " + ", ".join(absent))
    failures = []
    used = set()

    def replay(item):
        name, (source, manifest_sha, functions) = item
        try:
            return compare(name, source, manifest_sha, functions,
                           references["models"][name], ledger, args), None
        except (RuntimeError, ValueError) as error:
            print("FAIL", error, flush=True)
            return set(), str(error)

    # Each model spends most of its wall time compiling and lowering, so
    # replay models in parallel too (each also runs its points in parallel).
    with concurrent.futures.ThreadPoolExecutor(
            max_workers=max(1, args.jobs // POINTS)) as pool:
        for entry_names, failure in pool.map(replay,
                                             sorted(models.items())):
            used |= entry_names
            if failure is not None:
                failures.append(failure)
    # A complete replay that needed none of an entry's allowance shows the
    # divergence is gone; the obsolete entry must be removed, mirroring the
    # conformance baseline's policy-improvement ratchet.
    if not args.model and not args.manifest and not failures:
        obsolete = sorted(set(ledger) - used)
        if obsolete:
            failures.append("obsolete signature ledger entries: "
                            + ", ".join(obsolete))
            print("FAIL", failures[-1], flush=True)
    if failures:
        return 1
    print(f"All {len(models)} signature models passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
