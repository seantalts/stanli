#!/usr/bin/env python3
"""Census of stanli over stanc3's own integration model library.

harnesses/stan_conformance.py is signature-directed: it enumerates the
functions stan-math declares and generates one tiny model per signature.
This is model-directed. It takes the 1,231 .stan files stanc3 keeps in
test/integration/good -- files written to be *compiled* by stanc3's
expect-test framework and never to be run -- and asks a different
question: given a whole model somebody wrote to stress the language, does
stanli lower it and evaluate a gradient?

The two do not overlap. A signature sweep cannot see a tuple unpacked in
a loop, a user `_lupdf` called from a `transformed parameters` block, or
an `array[4,5] matrix[2,3]` parameter, because no signature mentions
them. This can, and the product is the ranked bucket list at the bottom
of the report: the language surface stanli refuses, ordered by how many
real models it costs.

harnesses/fn_sweep.py explains why it generates its models instead of
borrowing these: "those files carry no data, and this lowering evaluates
transformed data eagerly and unrolls loops against known bounds, so a
model without data cannot be lowered at all." That is still true. The
answer here is `stanc --debug-generate-data`, which invents data
respecting the declared constraints. It is not free of consequences:

  - The values are random per invocation, so they are cached
    content-addressed by the model's sha256 (`model-census-cache/data/`,
    gitignored). A fresh clone with the cache reruns identically; without
    it, data changes and some rows move. Every row records the data
    file's own sha256 so that drift is visible rather than mysterious.
  - It does not know a distribution's support. A generated `binomial`
    outcome can exceed its own trial count, which both engines then
    reject. See `data_rejected` below.

Phase B (`--differential`) settles what the census can only guess: it
compiles the model through CmdStan the way tools/verify_sample.py does
and compares lp and the full gradient at the same deterministic points.
A model that lowered and then disagrees with CmdStan is a wrong number
rather than a missing feature, and outranks every other row in the
report.

Cost, measured on a 32-core arm64 Mac against a built CmdStan:

  census only, warm cache   1,231 models   2m 04s at --jobs 8
  census only, cold cache   1,231 models   7m 58s at --jobs 8
  census + --differential     392 refs     7m 45s at --jobs 10
                                           (9.2s per reference on
                                            average, 50s at the worst,
                                            56 CPU-minutes in total)

Cold is nearly all stanc: the function-signatures models take ten to
twenty-five seconds each at --O1, which is why the MIR is cached beside
the data. The whole cache is 22 MB. Every reference is cached too, so
the differential above costs about a second on a second run:

  harnesses/model_census.py --differential --cmdstan deps/cmdstan \
      --jobs 10 --out model-census.json

Usage:
  harnesses/model_census.py [--filter SUBSTR] [--jobs N]
                            [--corpus DIR] [--cache DIR] [--out FILE]
                            [--check BIN] [--differential --cmdstan DIR]

  --filter SUBSTR   substring of the path relative to the corpus root, so
                    `--filter tuples/` takes one directory
  --differential    also compare lp + gradient against CmdStan for every
                    model that lowered; needs --cmdstan
"""
import argparse
import concurrent.futures
import gzip
import hashlib
import json
import os
import pathlib
import re
import subprocess
import sys
import tempfile
import time

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools"))
from cmdstan_ref import compile_cmd  # noqa: E402
# The deviation arithmetic is shared with the CI replay gate on purpose;
# see the note at its definition. A census that scored agreement its own
# way would be a second opinion nobody asked for.
from verify_refs import default_check_bin, pair_dev  # noqa: E402

CORPUS = REPO / "deps" / "stanc3-src" / "test" / "integration" / "good"
CACHE = REPO / "model-census-cache"
STANC = REPO / "deps" / "stanc3" / "stanc"
DRIVER = REPO / "tools" / "ref_driver.cpp"

# stanli_check walks these; a model can be undefined at one point and fine
# at the next (an ODE solution dipping below a declared bound, say), which
# is why both drivers agree on the list. Same three, same order.
POINTS = (0, 1, 2)

# One status per model, and the set is closed. Anything a row could be
# that is not here is a bug in this file, not a new kind of outcome.
STATUSES = (
    "lowered",         # stanli compiled it and evaluated lp + gradient
    "unsupported",     # clean COMPILE_FAIL: stanli said what it cannot do
    "eval_failed",     # compiled, then threw at every evaluation point
    "data_rejected",   # the refusal is about the invented data, not the
                       # model -- see _DATA_REFUSAL below; heuristic
    "stanc_rejected",  # stanc would not produce MIR; not stanli's problem
    "data_unavailable",  # stanc could not invent data for it
    "not_a_model",     # a functions-only fragment, not a standalone model
    "crashed",         # died, or printed nothing recognizable
    "timed_out",       # still running when the budget ran out
    "harness_error",   # this script broke
)

# A refusal whose subject is the data rather than the language. stanc's
# data generator respects declared constraints but not a distribution's
# support, and stanli evaluates transformed data eagerly at compile time,
# so a bad invented value surfaces as a COMPILE_FAIL that reads exactly
# like a missing feature and is not one. CmdStan throws on the same input
# at validate_params/transformed data, so both engines reject and that is
# agreement -- fn_sweep's precedent.
#
# This is a heuristic over an error string and it is the softest thing in
# the file. It is a separate status precisely so it can be audited: every
# such row keeps the message it matched on, and `--differential` is what
# actually proves the reference agrees. Treat the count as a ceiling on
# "not stanli's fault", never as a pass.
# The first alternative carries most of the weight. Stan Math spells
# every domain complaint the same way -- "<what> is <value>, but must be
# <constraint>" -- and nothing stanli says about a missing feature has
# that shape. The rest are the validators that phrase it differently.
# Deliberately NOT matched: "type must be number, but is array", which is
# nlohmann's complaint that stanli's JSON reader cannot express a shape.
# That is a stanli gap and belongs in `unsupported`.
_DATA_REFUSAL = re.compile(
    r"is .{0,80}?, but must be"
    r"|is not a valid|is not positive definite|is not symmetric"
    r"|is not lower triangular|does not sum to"
    r"|not found in data|missing data|no data (variable|value)"
    r"|size mismatch|must match in size|sizes? .{0,40}do not match"
    r"|mismatch in dimension declared|argument outside range"
    r"|out[ _]of[ _]range",
    re.I)

# What a bucket key drops from a message so that one refusal is one row.
#
# stanli appends the offending expression to some refusals after " | in: "
# -- a whole s-expression, different for every model, which splits
# "unsupported sized type SComplexMatrix" into as many buckets as there
# are matrix dimensions anybody wrote. And a validator's message embeds
# the value it rejected, so "atanh: x is 3" and "atanh: x is 4.63241" are
# two entries for one problem. Neither distinction survives into the
# to-do list this report exists to rank; both stay in the row's `detail`.
# Not preceded by a word character or an `=`, so a rejected *value* is
# normalised while an identifier keeps its digits: pareto_type_2_lpdf and
# `dims=2` name what failed, `x is 4.63241` does not.
_NUMERAL = re.compile(r"(?<![\w=])-?\d+(\.\d+)?([eE][+-]?\d+)?")


def bucket_key(message):
    return _NUMERAL.sub("N", message.split(" | in: ")[0])

# Blocks a standalone model can declare. A file naming none of them but
# `functions` is a fragment stanc3 compiles with --standalone-functions;
# it has no log density and lowering it says nothing. Two files in the
# pinned tree (code-gen/standalone_functions/) are this.
_BLOCK = re.compile(
    r"^\s*(functions|data|transformed\s+data|parameters"
    r"|transformed\s+parameters|model|generated\s+quantities)\s*\{", re.M)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def repo_rel(path):
    """A path as a repro command should spell it: relative to the repo.

    deps/ is a symlink farm in every worktree, so a resolved corpus path
    points outside the checkout and relative_to() raises. Fall back to
    the absolute path rather than losing the command.
    """
    path = pathlib.Path(path)
    try:
        return str(path.relative_to(REPO))
    except ValueError:
        return str(path)


def write_once(path, data):
    """Publish `data` at `path` atomically, tolerating a racing writer.

    Two corpus files with identical bytes share a cache key, and at
    --jobs 8 both workers reach the empty cache at once. os.replace makes
    the loser's write a no-op rather than a torn file the next reader
    parses halfway through.
    """
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=path.parent, suffix=".part")
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(data)
        os.replace(tmp, path)
    except BaseException:
        pathlib.Path(tmp).unlink(missing_ok=True)
        raise


def run(cmd, timeout, **kw):
    """subprocess.run that reports a timeout as a result, not a raise.

    macOS ships no `timeout(1)`, and a corpus written to be compiled and
    never run contains at least one infinite loop (`infinite_loop_return`,
    `for_early_return`). A hang has to become a row.
    """
    try:
        return subprocess.run([str(c) for c in cmd], capture_output=True,
                              text=True, timeout=timeout, **kw)
    except subprocess.TimeoutExpired:
        return None


def first_line(text, limit=300):
    """One line of a refusal, with absolute paths stripped.

    Paths differ per checkout and per worktree, so leaving them in splits
    one bucket into as many buckets as there are machines. This is the
    message a row keeps; `bucket_key` decides what a bucket keys on.
    """
    line = (text or "").strip().splitlines()
    line = line[0].strip() if line else ""
    line = re.sub(r"(/[\w.@+-]+)+/([\w.@+-]+)", r"\2", line)
    return line[:limit] or "(no message)"


def status_line(stdout):
    """(kind, detail) from stanli_check's machine-readable stdout.

    Its contract is documented at the top of tools/stanli_check.cpp: one
    of OK / COMPILE_FAIL / EVAL_FAIL on the first stdout line. Anything
    else means the process did not get far enough to make a statement,
    which is what `crashed` is for.
    """
    for line in (stdout or "").splitlines():
        for kind in ("OK", "COMPILE_FAIL", "EVAL_FAIL"):
            if line.startswith(kind + " ") or line == kind:
                return kind, line[len(kind):].strip()
    return None, ""


def row(path, **fields):
    r = {"path": path, "status": None, "reason": "", "detail": "",
         "sha256": "", "data_sha256": None, "point": None,
         "n_gradients": None, "lp_finite": None, "repro": "",
         "seconds": 0.0, "differential": None}
    r.update(fields)
    return r


# stanli_check shells out to stanc on every invocation and there is no
# flag to hand it MIR it already has (tools/ is another agent's to change;
# the wish is recorded in this harness's report instead). Three points per
# model therefore means three more stanc runs on top of the one this
# harness makes for classification -- and stanc --O1 takes 22 seconds on a
# wiener model, of which there are 108. Standing in for stanc with a
# script that prints the MIR from the cache turns 4 compilations per model
# into 0 on a warm cache. stanli_check reads $STANC ahead of --stanc, so
# no flag has to change; the argv it passes is ignored.
#
# What stanli lowers is byte-for-byte what `stanc --O1
# --debug-optimized-mir` printed, so this is a cache, not a substitution.
# Repro commands in the report name the real stanc.
_SHIM = """#!/bin/sh
# Written by harnesses/model_census.py. Prints the cached MIR for the
# model named by $STANLI_CENSUS_MIR, ignoring stanc's arguments.
exec gzip -dc "$STANLI_CENSUS_MIR"
"""


def install_shim(cache):
    path = cache / "stanc-shim.sh"
    write_once(path, _SHIM.encode("utf-8"))
    path.chmod(0o755)
    return path


def cached_mir(model, model_sha, cache, stanc, timeout):
    """The --O1 MIR for one model, gzipped, compiled at most once ever.

    Gzipped because it is not small: a wiener signature model expands to
    3.9 MB of s-expression, and the corpus has 108 of them.
    """
    out = cache / "mir" / model_sha[:2] / f"{model_sha}.sexp.gz"
    if out.exists():
        return out, None
    result = run([stanc, "--O1", "--debug-optimized-mir", model], timeout)
    if result is None:
        return None, f"stanc timed out after {timeout}s"
    if result.returncode != 0 or not result.stdout.strip():
        return None, (result.stderr or result.stdout
                      or "stanc produced no MIR")
    write_once(out, gzip.compress(result.stdout.encode("utf-8"), mtime=0))
    return out, None


def cached_data(model, model_sha, cache, stanc, timeout):
    """The invented data for one model, generated at most once ever.

    Keyed on the model's own bytes: the generator is a pure function of
    the declarations, so a model that has not changed does not need new
    data, and a model that has changed must not keep the old data.
    """
    out = cache / "data" / model_sha[:2] / f"{model_sha}.json"
    if out.exists():
        return out, sha256(out.read_bytes()), None
    result = run([stanc, "--debug-generate-data", model], timeout)
    if result is None:
        return None, None, "timed out generating data"
    if result.returncode != 0 or not result.stdout.strip():
        return None, None, first_line(result.stderr or result.stdout
                                      or "stanc produced no data")
    blob = result.stdout.encode("utf-8")
    write_once(out, blob)
    return out, sha256(blob), None


def census_one(model, corpus, cache, check, stanc, shim, timeout):
    """Classify one model. Returns exactly one row with exactly one status."""
    relpath = str(model.relative_to(corpus))
    started = time.monotonic()
    try:
        source = model.read_bytes()
    except OSError as exc:
        return row(relpath, status="harness_error",
                   reason=f"unreadable: {exc}")
    model_sha = sha256(source)

    text = source.decode("utf-8", "replace")
    blocks = set(re.sub(r"\s+", " ", m.group(1))
                 for m in _BLOCK.finditer(text))
    if blocks == {"functions"}:
        return row(relpath, status="not_a_model", sha256=model_sha,
                   reason="functions-only fragment",
                   detail="declares no model, data or parameters block")

    # Pipeline order decides precedence, so a row is attributed to the
    # first stage that refused it. stanli_check runs stanc itself and
    # discards its stderr, so the reason a model never reached stanli
    # would otherwise be lost.
    mir, why = cached_mir(model, model_sha, cache, stanc, timeout)
    if mir is None:
        return row(relpath, status="stanc_rejected", sha256=model_sha,
                   reason=bucket_key(first_line(why)),
                   detail=first_line(why),
                   repro=f"{repo_rel(stanc)} --O1 "
                         f"--debug-optimized-mir {repo_rel(model)}")

    data, data_sha, why = cached_data(model, model_sha, cache, stanc, timeout)
    if data is None:
        return row(relpath, status="data_unavailable", sha256=model_sha,
                   reason=bucket_key(why), detail=why,
                   repro=f"{repo_rel(stanc)} --debug-generate-data "
                         f"{repo_rel(model)}")

    def repro(point):
        return (f"{repo_rel(check)} {repo_rel(model)} "
                f"{repo_rel(data)} --stanc {repo_rel(stanc)} "
                f"--point {point}")

    # Walk the points. A COMPILE_FAIL does not depend on the evaluation
    # point, so it settles the row immediately; an EVAL_FAIL might, so it
    # is only fatal once every point has produced one.
    env = dict(os.environ, STANC=str(shim), STANLI_CENSUS_MIR=str(mir))
    last_eval = ""
    for point in POINTS:
        result = run([check, model, data, "--stanc", stanc,
                      "--point", point], timeout, cwd=REPO, env=env)
        if result is None:
            # Separate from `crashed` because the two point at different
            # things. A dead process made no statement; a live one is
            # still making it, and the corpus contains at least one model
            # that does not terminate by construction, which no engine
            # could answer. stanli has a "while loop did not terminate"
            # guard -- see function-signatures/math/while.stan -- so a row
            # here is either a model nobody can run or a path that guard
            # does not cover. Both are worth naming, neither is a crash.
            return row(relpath, status="timed_out", sha256=model_sha,
                       data_sha256=data_sha, point=point,
                       reason=f"still running after {timeout}s",
                       repro=repro(point),
                       seconds=time.monotonic() - started)
        kind, detail = status_line(result.stdout)
        if result.returncode < 0 or result.returncode not in (0, 1) \
                or kind is None:
            signal = (f"killed by signal {-result.returncode}"
                      if result.returncode < 0
                      else f"exit {result.returncode} with no status line")
            return row(relpath, status="crashed", sha256=model_sha,
                       data_sha256=data_sha, point=point, reason=signal,
                       detail=(result.stderr or result.stdout).strip()[-400:],
                       repro=repro(point),
                       seconds=time.monotonic() - started)
        if kind == "OK":
            values = detail.split()
            lp = float(values[0]) if values else float("nan")
            return row(relpath, status="lowered", sha256=model_sha,
                       data_sha256=data_sha, point=point,
                       n_gradients=len(values) - 1,
                       lp_finite=(lp == lp and abs(lp) != float("inf")),
                       reason="lowered and evaluated", repro=repro(point),
                       seconds=time.monotonic() - started)
        if kind == "COMPILE_FAIL":
            status = ("data_rejected" if _DATA_REFUSAL.search(detail)
                      else "unsupported")
            return row(relpath, status=status, sha256=model_sha,
                       data_sha256=data_sha, point=point,
                       reason=bucket_key(first_line(detail)),
                       detail=first_line(detail), repro=repro(point),
                       seconds=time.monotonic() - started)
        last_eval = detail

    status = ("data_rejected" if _DATA_REFUSAL.search(last_eval)
              else "eval_failed")
    return row(relpath, status=status, sha256=model_sha, data_sha256=data_sha,
               point=None, reason=bucket_key(first_line(last_eval)),
               detail=first_line(last_eval), repro=repro(POINTS[-1]),
               seconds=time.monotonic() - started)


# ---------------------------------------------------------------------------
# Phase B: differential against CmdStan.


def reference_key(model_sha, data_sha, cmdstan, stanc, opt):
    """Everything that can change what the reference binary computes.

    Same shape as harnesses/conformance/oracle.py's cache key and for the
    same reason: a reference is expensive enough that it should be built
    once per model ever, and cheap to invalidate wrongly if the key omits
    a toolchain the answer depends on.
    """
    head = run(["git", "-C", cmdstan, "rev-parse", "HEAD"], 30)
    payload = {
        "schema": 1,
        "model_sha256": model_sha,
        "data_sha256": data_sha,
        "cmdstan_head": (head.stdout.strip() if head and not head.returncode
                         else "unknown"),
        "stanc_sha256": sha256(pathlib.Path(stanc).read_bytes()),
        "driver_sha256": sha256(DRIVER.read_bytes()),
        "opt": opt,
    }
    return sha256(json.dumps(payload, sort_keys=True).encode("utf-8")), payload


def reference_points(model, model_sha, data, data_sha, cache, cmdstan, stanc,
                     opt, timeout):
    """CmdStan's `OK lp g...` line at each point, built at most once ever.

    Only the output is cached, not the binary: the reference driver is
    deterministic at a fixed point and the recorded line is a few hundred
    bytes against sixty megabytes of executable. That is also what lets a
    later run compare without CmdStan present at all, which is the same
    trick tools/verify_refs.py plays on the corpus.
    """
    key, payload = reference_key(model_sha, data_sha, cmdstan, stanc, opt)
    out = cache / "ref" / key[:2] / f"{key}.json"
    if out.exists():
        try:
            record = json.loads(out.read_text(encoding="utf-8"))
            record["cache_hit"] = True
            return record
        except (OSError, json.JSONDecodeError):
            pass  # a torn cache entry is rebuilt, not trusted

    started = time.monotonic()
    record = {"key": key, "inputs": payload, "cache_hit": False}
    with tempfile.TemporaryDirectory(prefix="stanli_census_ref_") as work:
        work = pathlib.Path(work)
        hpp = work / "model.hpp"
        # --O1, matching what stanli consumes. Without it stanc emits
        # different arithmetic (it forms fma at O1) and the two sides
        # differ by a rounding per accumulation, which reads as a
        # semantic mismatch and is not one. oracle.py carries the same
        # note; tools/cmdstan_ref.build_reference does NOT pass it, which
        # is why it is not used here.
        translated = run([stanc, "--O1", model, f"--o={hpp}"], timeout)
        if translated is None or translated.returncode != 0:
            record["error"] = "ref_stanc_fail: " + first_line(
                "" if translated is None
                else (translated.stderr or translated.stdout))
            return _remember(out, record)
        exe = work / "ref"
        built = run(compile_cmd(cmdstan, hpp, DRIVER, exe, opt=opt), 1800.0)
        if built is None or built.returncode != 0:
            # Cached like a success. The key carries the CmdStan head, the
            # stanc binary and the driver, so anything that could make this
            # model build is also something that invalidates the entry; the
            # alternative is re-running a doomed minute-long compile on
            # every invocation.
            record["error"] = "ref_build_fail: " + first_line(
                "" if built is None
                else (built.stderr or built.stdout)[-2000:])
            return _remember(out, record)
        lines = {}
        for point in POINTS:
            result = run([exe, data, point], timeout)
            lines[str(point)] = (result.stdout.splitlines() or [""])[0] \
                if result is not None else ""
        record["points"] = lines
    record["build_seconds"] = time.monotonic() - started
    return _remember(out, record)


def _remember(path, record):
    write_once(path, (json.dumps(record, indent=1, sort_keys=True)
                      + "\n").encode("utf-8"))
    return record


def _ok_values(line):
    """The floats on an `OK ...` line, or None if the side refused.

    An all-nan reference row counts as a refusal: CmdStan's log_prob
    returns nan where stanli_check reports EVAL_FAIL and prints nothing,
    and reading that asymmetry as disagreement would be wrong. The rule
    is cmdstan_ref.compare_points'; it is restated here because that
    function re-runs the reference binary on every call, which is exactly
    the cost the cache above exists to pay once.
    """
    if not line or not line.startswith("OK"):
        return None
    try:
        values = [float(v) for v in line.split()[1:]]
    except ValueError:
        return None
    if not values or all(v != v for v in values):
        return None
    return values


def differential_one(entry, corpus, cache, check, stanc, shim, cmdstan, opt,
                     max_rel, timeout):
    """lp + gradient against CmdStan for one model that already lowered."""
    model = corpus / entry["path"]
    data = (cache / "data" / entry["sha256"][:2] / f"{entry['sha256']}.json")
    record = reference_points(model, entry["sha256"], data,
                              entry["data_sha256"], cache, cmdstan, stanc,
                              opt, timeout)
    # Carried into every row so the report can price a full-library run
    # from measurement rather than guesswork: this is the number that says
    # what --differential over 1,231 models would actually cost.
    cost = {"cache_hit": record.get("cache_hit", False),
            "build_seconds": record.get("build_seconds", 0.0)}
    if "error" in record:
        kind, _, note = record["error"].partition(": ")
        return dict(cost, status=kind, detail=note)

    mir = cache / "mir" / entry["sha256"][:2] / f"{entry['sha256']}.sexp.gz"
    env = dict(os.environ, STANC=str(shim), STANLI_CENSUS_MIR=str(mir))
    worst_rel, worst_ulp, compared, agreed_refusals = 0.0, 0, 0, 0
    for point in POINTS:
        reference = _ok_values(record["points"].get(str(point), ""))
        result = run([check, model, data, "--stanc", stanc,
                      "--point", point], timeout, cwd=REPO, env=env)
        kind, detail = status_line(result.stdout if result else "")
        # A timeout lands here as `ours = None`, indistinguishable from a
        # refusal. Every model reaching this function already evaluated
        # inside the same timeout during the census, so it is an edge the
        # census would have caught as `crashed` first.
        ours = [float(v) for v in detail.split()] if kind == "OK" else None
        if reference is None or ours is None:
            if (reference is None) != (ours is None):
                return dict(
                    cost, status="one_side_threw",
                    detail=f"point {point}: cmdstan="
                           f"{'threw' if reference is None else 'ok'} "
                           f"stanli={'threw' if ours is None else 'ok'}")
            agreed_refusals += 1
            continue
        if len(reference) != len(ours):
            return dict(cost, status="shape_mismatch",
                        detail=f"cmdstan {len(reference)}, "
                               f"stanli {len(ours)}")
        for a, b in zip(reference, ours):
            rel, ulp = pair_dev(a, b)
            worst_rel, worst_ulp = max(worst_rel, rel), max(worst_ulp, ulp)
            compared += 1

    if compared == 0:
        # Every shared point is outside the model's support and both
        # engines said so. fn_sweep counts that as agreement.
        return dict(cost, status="rejected_both",
                    detail=f"{agreed_refusals} points refused by both")
    return dict(cost,
                status="verified" if worst_rel < max_rel else "mismatch",
                max_rel=worst_rel, max_ulp=worst_ulp, n_values=compared,
                detail="")


# ---------------------------------------------------------------------------
# Reporting.


def buckets(rows, statuses):
    """Rejection reasons by model count -- the to-do list, ranked.

    This is the product. A bucket is the first line of the refusal with
    absolute paths stripped, so "unsupported function foo" collapses
    across every model that hits it and the number beside it is how many
    models the fix is worth.
    """
    counts = {}
    for r in rows:
        if r["status"] in statuses:
            key = (r["status"], r["reason"])
            counts.setdefault(key, []).append(r["path"])
    return sorted(((s, why, sorted(paths)) for (s, why), paths
                   in counts.items()), key=lambda t: (-len(t[2]), t[0], t[1]))


def summarize(report, limit):
    rows = report["rows"]
    print(f"\n{len(rows)} models from {report['corpus']}")
    print(f"stanc {report['stanc_version']}  "
          f"stanli_check {report['check_sha256'][:12]}\n")
    width = max(len(s) for s in STATUSES)
    for status in STATUSES:
        n = sum(1 for r in rows if r["status"] == status)
        if n:
            print(f"  {status:<{width}}  {n:>5}  "
                  f"{100.0 * n / max(len(rows), 1):5.1f}%")

    # A lowering that returns -inf is still a lowering -- both engines
    # agree at log(0) and verify_refs' arithmetic scores it as agreement --
    # but it exercised no density, so it is weaker evidence than the count
    # alone suggests and the summary says how much of the count it is.
    flat = sum(1 for r in rows
               if r["status"] == "lowered" and not r["lp_finite"])
    if flat:
        print(f"\n  of the lowered, {flat} returned a nonfinite lp at the "
              "point that evaluated")

    for status, heading in (("crashed", "crashes"),
                            ("timed_out", "timeouts")):
        listed = [r for r in rows if r["status"] == status]
        print(f"\n{heading}: {len(listed)}")
        for r in listed:
            print(f"  {r['path']}: {r['reason']}")
            print(f"    {r['repro']}")

    print("\nrejection buckets, by model count "
          "(unsupported + eval_failed + data_rejected):")
    ranked = buckets(rows, ("unsupported", "eval_failed", "data_rejected"))
    for status, why, paths in ranked[:limit]:
        print(f"  {len(paths):>5}  [{status}] {why}")
        print(f"           e.g. {paths[0]}")
    if len(ranked) > limit:
        print(f"  ... {len(ranked) - limit} more buckets, all in the JSON")

    diffs = [r for r in rows if r.get("differential")]
    if diffs:
        print(f"\ndifferential against CmdStan: {len(diffs)} models")
        kinds = {}
        for r in diffs:
            kinds.setdefault(r["differential"]["status"], []).append(r)
        for kind in sorted(kinds):
            print(f"  {kind:<16} {len(kinds[kind]):>5}")
        built = [r["differential"]["build_seconds"] for r in diffs
                 if not r["differential"]["cache_hit"]]
        if built:
            print(f"  {len(built)} references built, "
                  f"{sum(built) / len(built):.1f}s each on average, "
                  f"{sum(built) / 60.0:.1f} CPU-minutes total")
        for r in kinds.get("mismatch", []) + kinds.get("shape_mismatch", []) \
                + kinds.get("one_side_threw", []):
            d = r["differential"]
            print(f"  ! {r['path']}: {d['status']} "
                  f"{d.get('detail') or ''} "
                  f"max_rel={d.get('max_rel', 'n/a')}")
            print(f"    {r['repro']}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--corpus", type=pathlib.Path, default=CORPUS)
    ap.add_argument("--cache", type=pathlib.Path, default=CACHE)
    ap.add_argument("--check", type=pathlib.Path, default=None,
                    help="stanli_check binary (default: build-rel then build)")
    ap.add_argument("--stanc", type=pathlib.Path, default=STANC)
    ap.add_argument("--filter", default="",
                    help="substring of the path relative to the corpus root")
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--timeout", type=float, default=120.0)
    ap.add_argument("--out", type=pathlib.Path,
                    default=REPO / "model-census.json")
    ap.add_argument("--buckets", type=int, default=40,
                    help="rejection buckets printed (all stay in the JSON)")
    ap.add_argument("--differential", action="store_true",
                    help="also compare lp + gradient against CmdStan")
    ap.add_argument("--cmdstan", type=pathlib.Path,
                    help="CmdStan checkout, required by --differential")
    ap.add_argument("--max-rel", type=float, default=1e-10,
                    help="relative deviation gate for --differential")
    ap.add_argument("--ref-opt", default="-O1",
                    help="optimization level for the reference binary")
    args = ap.parse_args()

    check = (args.check or default_check_bin()).resolve()
    if not check.exists():
        print(f"no stanli_check at {check}", file=sys.stderr)
        return 2
    if args.differential and not args.cmdstan:
        print("--differential needs --cmdstan DIR", file=sys.stderr)
        return 2

    # abspath, not resolve: deps/ is a symlink in every worktree and
    # resolving it puts the corpus outside the checkout, which turns every
    # repro command in the report into a machine-specific absolute path.
    corpus = pathlib.Path(os.path.abspath(args.corpus))
    models = sorted(p for p in corpus.rglob("*.stan")
                    if args.filter in str(p.relative_to(corpus)))
    if not models:
        print(f"no models under {corpus} matching {args.filter!r}",
              file=sys.stderr)
        return 2

    shim = install_shim(args.cache)
    version = run([args.stanc, "--version"], 30)
    started = time.monotonic()
    rows = []
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        for r in pool.map(lambda m: census_one(m, corpus, args.cache, check,
                                               args.stanc, shim,
                                               args.timeout),
                          models):
            rows.append(r)
            print(".", end="", file=sys.stderr, flush=True)
    print(file=sys.stderr)

    if args.differential:
        lowered = [r for r in rows if r["status"] == "lowered"]
        print(f"differential over {len(lowered)} lowered models",
              file=sys.stderr)
        with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
            for entry, result in zip(lowered, pool.map(
                    lambda e: differential_one(
                        e, corpus, args.cache, check, args.stanc, shim,
                        args.cmdstan.resolve(), args.ref_opt, args.max_rel,
                        args.timeout), lowered)):
                entry["differential"] = result
                print(".", end="", file=sys.stderr, flush=True)
        print(file=sys.stderr)

    unknown = [r for r in rows if r["status"] not in STATUSES]
    report = {
        "schema": 1,
        "corpus": repo_rel(corpus),
        "filter": args.filter,
        "stanc_version": ((version.stdout or version.stderr).strip()
                          if version else "unknown"),
        "check": str(check),
        "check_sha256": sha256(check.read_bytes()),
        "max_rel": args.max_rel if args.differential else None,
        "wall_seconds": time.monotonic() - started,
        "counts": {s: sum(1 for r in rows if r["status"] == s)
                   for s in STATUSES},
        "buckets": [{"status": s, "reason": why, "models": paths}
                    for s, why, paths in buckets(rows, STATUSES)],
        "rows": sorted(rows, key=lambda r: r["path"]),
    }
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(report, indent=1, sort_keys=True) + "\n")
    summarize(report, args.buckets)
    print(f"\nwrote {args.out}  ({report['wall_seconds']:.1f}s)")

    if unknown:
        print(f"\n{len(unknown)} rows carry no status; that is a bug in "
              "this harness", file=sys.stderr)
        return 1
    # A process that died or hung is the headline, not a footnote, so it
    # is the only census outcome that fails the run. Everything else --
    # including every unsupported bucket -- is the backlog this report
    # exists to rank, and a red exit that means "there is work to do" is
    # one nobody reads.
    return 1 if (report["counts"]["crashed"]
                 or report["counts"]["timed_out"]) else 0


if __name__ == "__main__":
    sys.exit(main())
