#!/usr/bin/env python3
"""Freeze data for every model in stanc3's test/integration/good.

The models carry no data. For each one this builds CmdStan's reference
driver once, then asks `stanc --debug-generate-data` for candidates until
the driver loads one without a DATA_FAIL. Among loadable candidates it
keeps the one finite at the most points and stops once every point is
finite. stanc respects declared constraints but not a distribution's
support, which is why retries help.

Hand-written data in tests/stanc3_good/hand_data.json replaces generation
for the models listed there. Entries already in the output whose source is
unchanged and which were accepted are kept, because stanc has no seed and
regenerating would invalidate everything recorded against them.

  tools/gen_stanc3_good_data.py [--jobs N] [--filter SUBSTR] [--regenerate]
  tools/gen_stanc3_good_data.py --check

See tests/stanc3_good/README.md for the file and the binary cache layout.
"""
import argparse
import collections
import concurrent.futures
import gzip
import hashlib
import json
import math
import os
import pathlib
import re
import subprocess
import sys
import tempfile
import time

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools"))
from cmdstan_ref import compile_cmd, point_results  # noqa: E402
from verify_refs import parse_status  # noqa: E402
import toolchain  # noqa: E402

CORPUS = REPO / "deps" / "stanc3-src" / "test" / "integration" / "good"
OUT = REPO / "tests" / "stanc3_good" / "data.json.gz"
HAND = REPO / "tests" / "stanc3_good" / "hand_data.json"
CACHE = REPO / "build-stanc3-ref"
STANC = toolchain.PINNED_STANC
CMDSTAN = REPO / "deps" / "cmdstan"
DRIVER = REPO / "tools" / "ref_driver.cpp"
POINTS = (0, 1, 2, 3, 4)
STANC_FLAGS = []
OPT = "-O1"
BUILD_TIMEOUT = 900
RUN_TIMEOUT = 60


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def pinned_sha():
    return toolchain.pins()["STANC3_SRC_SHA"]


def corpus_models():
    return {p.relative_to(CORPUS).as_posix(): sha256(p.read_bytes())
            for p in sorted(CORPUS.rglob("*.stan"))}


def serialize(doc):
    text = json.dumps(doc, sort_keys=True, separators=(",", ":"))
    return gzip.compress(text.encode() + b"\n", mtime=0)


def run(cmd, timeout):
    try:
        return subprocess.run([str(c) for c in cmd], capture_output=True,
                              text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return None


def last_line(text, limit=200):
    lines = [ln for ln in (text or "").strip().splitlines() if ln.strip()]
    return lines[-1][:limit] if lines else ""


def stamp():
    head = run(["git", "-C", CMDSTAN, "rev-parse", "HEAD"], 30)
    return {"driver_sha256": sha256(DRIVER.read_bytes()),
            "stanc_sha256": sha256(STANC.read_bytes()),
            "cmdstan_head": head.stdout.strip() if head else "unknown",
            "stanc_flags": STANC_FLAGS, "opt": OPT}


def build(model, model_sha, want, objs):
    """Path to the cached reference binary, or (None, reason)."""
    d = CACHE / model_sha
    exe, stamp_file = d / "ref", d / "stamp.json"
    if objs:
        status = objs / model_sha / "status"
        while not status.exists():
            time.sleep(10)
        state = status.read_text().strip()
        if state != "ok":
            return None, state.replace("cc_fail", "build", 1)[:200]
        o = (objs / model_sha / "model.o").stat()
        want = {**want, "model_o": f"{o.st_size}-{o.st_mtime_ns}"}
    if exe.exists() and stamp_file.exists() and \
            json.loads(stamp_file.read_text()) == want:
        return exe, None
    d.mkdir(parents=True, exist_ok=True)
    stamp_file.unlink(missing_ok=True)
    exe.unlink(missing_ok=True)
    if objs:
        r = link_prebuilt(objs, model_sha, exe)
    else:
        hpp = d / "model.hpp"
        r = run([STANC, *STANC_FLAGS, model, f"--o={hpp}"], BUILD_TIMEOUT)
        if r is None or r.returncode:
            return None, "stanc: " + (last_line(r.stderr or r.stdout) if r
                                      else "timed out")
        r = run(compile_cmd(CMDSTAN, hpp, DRIVER, exe, OPT), BUILD_TIMEOUT)
    if r is None or r.returncode:
        return None, "build: " + (last_line(r.stderr) if r else "timed out")
    stamp_file.write_text(json.dumps(want, sort_keys=True, indent=1) + "\n")
    return exe, None


def driver_object(objs):
    """ref_driver.cpp compiled once with the prebuilt objects' flags."""
    flags = json.loads((objs / "FLAGS").read_text())
    out = CACHE / f"driver-{sha256(DRIVER.read_bytes())[:16]}.o"
    if not out.exists():
        CACHE.mkdir(exist_ok=True)
        tmp = out.with_suffix(f".{os.getpid()}.tmp")
        subprocess.run([*flags["compile"], "-c", str(DRIVER), "-o", str(tmp)],
                       check=True)
        tmp.rename(out)
    return out


def link_prebuilt(objs, model_sha, exe):
    flags = json.loads((objs / "FLAGS").read_text())
    tail = flags["link_tail"]
    tail = tail[:tail.index("-o")]
    return run(["clang++", objs / model_sha / "model.o", driver_object(objs),
                *tail, "-o", exe], BUILD_TIMEOUT)


def evaluate(exe, data_text, work):
    """(data_fail_message or None, number of finite points, messages of the
    points that threw)."""
    path = work / "data.json"
    path.write_text(data_text)
    r = run([exe, path, "--points", ",".join(map(str, POINTS))], RUN_TIMEOUT)
    if r is None:
        return "driver timed out", 0, []
    head = parse_status(r.stdout)
    if head[:1] == ["DATA_FAIL"]:
        msg = " ".join(head[1:])
        return re.sub(r"\(in '[^']*/good/", "(in '", msg)[:200], 0, [msg]
    results = {k: f for k, f in point_results(r.stdout).items()
               if f[:1] in (["OK"], ["EVAL_FAIL"])}
    if len(results) != len(POINTS):
        return f"driver exited {r.returncode}: {last_line(r.stderr)}", 0, []
    finite = sum(f[:1] == ["OK"] and all(math.isfinite(float(v))
                                         for v in f[1:])
                 for f in results.values())
    thrown = [" ".join(f[1:]) for f in results.values()
              if f[:1] == ["EVAL_FAIL"]]
    return None, finite, thrown


def remap(value, ints=None, reals=None, top=True):
    """Apply ints/reals to the leaves of a JSON value; scalars at the top
    level are left alone when top is False."""
    if isinstance(value, dict):
        return {k: remap(v, ints, reals, top) for k, v in value.items()}
    if isinstance(value, list):
        return [remap(v, ints, reals, True) for v in value]
    if isinstance(value, bool) or not top:
        return value
    if isinstance(value, int):
        return ints(value) if ints else value
    if isinstance(value, float):
        return reals(value) if reals else value
    return value


def first(value):
    """Every list cut to its first element, every top-level int set to 1:
    for models that use one int as both a size and a bounded argument."""
    if isinstance(value, list):
        return [first(value[0])] if value else value
    if isinstance(value, dict):
        return {k: first(v) for k, v in value.items()}
    return value


def is_real_vector(v):
    return isinstance(v, list) and v and all(type(x) is float for x in v)


def structured(value, vector):
    """Square real matrices become the identity and real vectors become
    vector(v): symmetric, triangular, correlation and Cholesky arguments,
    simplexes and ordered vectors."""
    if isinstance(value, dict):
        return {k: structured(v, vector) for k, v in value.items()}
    if not isinstance(value, list):
        return value
    if value and all(is_real_vector(r) and len(r) == len(value)
                     for r in value):
        n = len(value)
        return [[1.0 if i == j else 0.0 for j in range(n)] for i in range(n)]
    if is_real_vector(value):
        return vector(value)
    return [structured(v, vector) for v in value]


def simplex(v):
    return [1.0 / len(v)] * len(v)


def ascending(v):
    return [float(i + 1) for i in range(len(v))]


def ones(d):
    return {k: 1 if type(v) is int else first(v) for k, v in d.items()}


def binary(v):
    return v % 2


def unit(v):
    return abs(v) % 1.0 or 0.5


# stanc draws ints and reals without knowing a distribution's support, so
# the outcome of a bernoulli or the probability of a binomial is usually
# out of range. Later attempts cycle through these repairs.
REPAIRS = (
    lambda d: d,
    lambda d: {k: remap(v, ints=binary, top=False) for k, v in d.items()},
    lambda d: remap(d, ints=binary),
    lambda d: remap(d, reals=unit),
    lambda d: {k: remap(v, ints=binary, reals=unit, top=False)
               for k, v in d.items()},
    ones,
    lambda d: remap(ones(d), reals=unit),
    lambda d: structured(d, simplex),
    lambda d: structured(d, ascending),
)


# Stan Math names the offending value and the bound it broke, so a failed
# candidate can be steered: every data leaf printed as that value is moved
# inside the bound.
NUM = r"(-?(?:\d+\.?\d*(?:e[-+]?\d+)?|inf|nan))"
BOUND = re.compile(
    r"(?: is|  ?=) " + NUM + r", but must be (?:"
    r"in the interval \[" + NUM + r", " + NUM + r"\]"
    r"|(less|greater) than( or equal to)?[^=\d-]*=? ?" + NUM +
    r"|(positive|nonnegative|non-negative|finite))")
UNSORTED = re.compile(r"not a valid sorted vector\. The element at \d+ is "
                      + NUM)


def near(leaf, v):
    if type(leaf) not in (int, float) or isinstance(leaf, bool):
        return False
    if math.isnan(v):
        return math.isnan(leaf)
    return leaf == v or abs(leaf - v) <= 1e-5 * max(1.0, abs(v))


def inside(v, m, is_int, exact=None):
    """A value inside the bound m names; exact is the unrounded bound when
    a data leaf holds it, so that "or equal to" can be met with equality."""
    lo, hi, side, eq, bound, kind = (m.group(i) for i in range(2, 8))
    if kind:
        if kind == "finite" or math.isnan(v) or math.isinf(v):
            return 1
        return abs(v) or 1
    if lo is not None:
        lo, hi = float(lo), float(hi)
        if is_int:
            return math.ceil(lo) if v < lo else math.floor(hi)
        return (lo + hi) / 2 if math.isfinite(lo + hi) else (
            lo + 1 if math.isfinite(lo) else hi - 1)
    b = float(bound)
    if eq and exact is not None and not is_int:
        return exact
    if is_int:
        return (math.ceil(b) if eq else math.floor(b) + 1) if side == \
            "greater" else (math.floor(b) if eq else math.ceil(b) - 1)
    if side == "greater":
        return b + 1 if b >= 0 else b / 2
    return b / 2 if b > 0 else b - 1


def leaf_paths(value, path=()):
    if isinstance(value, dict):
        for k, v in value.items():
            yield from leaf_paths(v, path + (k,))
    elif isinstance(value, list):
        yield path
        for i, v in enumerate(value):
            yield from leaf_paths(v, path + (i,))
    else:
        yield path


def get(value, path):
    for p in path:
        value = value[p]
    return value


def put(value, path, new):
    if not path:
        return new
    value = json.loads(json.dumps(value))
    get(value, path[:-1])[path[-1]] = new
    return value


SIZES = re.compile(r"\((\d+)\) and right hand side(?: rows| columns)? "
                   r"\((\d+)\)")


def variants(data, new):
    """new applied to all its leaves, to its real or int leaves alone, then
    to each leaf alone: stanc reuses its draws, and an int may be a size."""
    paths = list(new)
    groups = [paths] + [[p for p in paths if type(get(data, p)) is t]
                        for t in (float, int)] + [[p] for p in paths[:6]]
    out = []
    for group in groups:
        if not group or any(group == g for g in groups[:groups.index(group)]):
            continue
        d = data
        for p in group:
            d = put(d, p, new[p])
        out.append(d)
    return out


def matching(data, v, kind=None):
    return [p for p in leaf_paths(data) if near(get(data, p), v)
            and (kind is None or type(get(data, p)) is kind)]


def fixes(data, message):
    """Candidates that move the data leaves message names inside the bound
    it broke. When the value has no leaf (a parameter), the bound's leaves
    move to the other side of it instead."""
    m = BOUND.search(message)
    if m and m.group(1) != "nan":
        v = float(m.group(1))
        at = m.group(6) and [get(data, p)
                             for p in matching(data, float(m.group(6)))]
        exact = max(at) if at else None
        kind = {p: type(get(data, p)) for p in matching(data, v)}
        out = variants(data, {p: t(inside(v, m, t is int, exact))
                              for p, t in kind.items()})
        if out or m.group(6) is None:
            return out
        side = m.group(4)
        far = (v / 2 if v > 0 else v - 1) if side == "greater" else (
            2 * v if v > 0 else v + 1)
        return variants(data, {p: far for p in
                               matching(data, float(m.group(6)), float)})
    m = UNSORTED.search(message)
    if m:
        v = float(m.group(1))
        return variants(data, {p: sorted(get(data, p))
                               for p in leaf_paths(data)
                               if isinstance(get(data, p), list)
                               and any(near(x, v) for x in get(data, p))})
    m = SIZES.search(message)
    if m:
        a, b = int(m.group(1)), int(m.group(2))
        return (variants(data, {p: b for p in matching(data, a, int)})
                + variants(data, {p: a for p in matching(data, b, int)}))
    return []


def score(fail, finite, thrown):
    """Loading first, then points that evaluate, then finite ones."""
    return (fail is None, 0 if fail else len(POINTS) - len(thrown), finite)


def steer(exe, data, work, rounds=40):
    """Every (evaluate result, data) seen while repeatedly fixing the
    failure messages of data."""
    text = json.dumps(data, sort_keys=True)
    cur = evaluate(exe, text, work)
    seen, out = {text}, [(cur, data)]
    for _ in range(rounds):
        if cur[0] is None and cur[1] == len(POINTS):
            break
        step = None
        for m in dict.fromkeys(cur[2]):
            for cand in fixes(data, m):
                t = json.dumps(cand, sort_keys=True)
                if t in seen:
                    continue
                seen.add(t)
                r = evaluate(exe, t, work)
                out.append((r, cand))
                if step is None or score(*r) > score(*step[0]):
                    step = (r, cand)
        if step is None or score(*step[0]) < score(*cur):
            break
        cur, data = step
    return out


def generate(model, model_sha, want, max_attempts, objs, prev=None):
    """prev, an accepted entry being improved, is kept unless a new
    candidate evaluates at more points, or as many and finite at more."""
    entry = {"source_sha256": model_sha, "data": None, "origin": "generated",
             "attempts": 0, "accepted": False, "note": ""}
    exe, why = build(model, model_sha, want, objs)
    if exe is None:
        entry["note"] = why
        return entry
    fails = collections.Counter()
    best = None
    with tempfile.TemporaryDirectory(prefix="stanc3_good_data_") as work:
        work = pathlib.Path(work)
        if prev:
            entry["attempts"] = prev["attempts"]
            for r, data in steer(exe, prev["data"], work):
                if not r[0] and (best is None or score(*r) > best[0]):
                    best = (score(*r), data)
        for attempt in range(1, max_attempts + 1):
            if best and best[0][2] == len(POINTS):
                break
            entry["attempts"] += 1
            g = run([STANC, "--debug-generate-data", model], RUN_TIMEOUT)
            if g is None or g.returncode:
                entry["note"] = "stanc --debug-generate-data: " + (
                    last_line(g.stderr or g.stdout) if g else "timed out")
                return entry
            drawn = REPAIRS[(attempt - 1) % len(REPAIRS)](json.loads(g.stdout))
            if not drawn:
                entry["origin"] = "none_needed"
            for r, data in steer(exe, drawn, work):
                if r[0]:
                    fails[r[0]] += 1
                    entry["data"] = data
                elif best is None or score(*r) > best[0]:
                    best = (score(*r), data)
            if not drawn:
                break
    if best is None:
        entry["note"] = fails.most_common(1)[0][0]
        return entry
    entry.update(data=best[1], accepted=True)
    _, ok, finite = best[0]
    if finite < len(POINTS):
        entry["note"] = f"finite at {finite} of {len(POINTS)} points"
    if ok > finite:
        entry["note"] += f", evaluates at {ok}"
    return entry


def hand_entry(model, model_sha, want, spec, objs):
    entry = {"source_sha256": model_sha, "data": spec["data"],
             "origin": "hand", "attempts": 0, "accepted": False,
             "note": spec["note"]}
    exe, why = build(model, model_sha, want, objs)
    if exe is None:
        entry["note"] += f" ({why})"
        return entry
    with tempfile.TemporaryDirectory(prefix="stanc3_good_data_") as work:
        fail, _, _ = evaluate(exe, json.dumps(spec["data"], sort_keys=True),
                           pathlib.Path(work))
    entry["accepted"] = fail is None
    if fail:
        entry["note"] += f" (rejected: {fail})"
    return entry


def load(path):
    return json.loads(gzip.decompress(path.read_bytes()))


def check():
    errors = []
    doc = load(OUT)
    if doc.get("schema") != 1:
        errors.append("schema is not 1")
    if doc.get("stanc3_sha") != pinned_sha():
        errors.append(f"stanc3_sha {doc.get('stanc3_sha')} is not the pinned "
                      f"{pinned_sha()}")
    models, recorded = corpus_models(), doc.get("models", {})
    for rel in sorted(models.keys() - recorded.keys()):
        errors.append(f"missing: {rel}")
    for rel in sorted(recorded.keys() - models.keys()):
        errors.append(f"not in the corpus: {rel}")
    for rel in sorted(models.keys() & recorded.keys()):
        e = recorded[rel]
        if e.get("source_sha256") != models[rel]:
            errors.append(f"source changed: {rel}")
        if e.get("origin") not in ("generated", "hand", "none_needed"):
            errors.append(f"bad origin: {rel}")
    # Compare the JSON, not the gzip bytes, which differ between zlib builds.
    if gzip.decompress(serialize(doc)) != gzip.decompress(OUT.read_bytes()):
        errors.append("file is not in canonical form")
    for e in errors:
        print(e, file=sys.stderr)
    if not errors:
        n = sum(e["accepted"] for e in recorded.values())
        print(f"ok: {len(recorded)} models, {n} with accepted data")
    return 1 if errors else 0


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 8) - 4))
    ap.add_argument("--filter", action="append", default=[],
                    help="substring of the model path; repeatable")
    ap.add_argument("--max-attempts", type=int, default=20)
    ap.add_argument("--regenerate", action="store_true",
                    help="redo accepted generated entries too")
    ap.add_argument("--improve", action="store_true",
                    help="retry accepted entries that are not finite at "
                         "every point, keeping the old data unless beaten")
    ap.add_argument("--objs", type=pathlib.Path,
                    help="link prebuilt <sha256>/model.o objects from DIR "
                         "(with its FLAGS file) instead of compiling")
    ap.add_argument("--check", action="store_true",
                    help="verify the file against the pinned corpus; "
                         "needs no CmdStan")
    args = ap.parse_args()
    if args.check:
        return check()

    models = corpus_models()
    old = load(OUT)["models"] if OUT.exists() else {}
    hand = json.loads(HAND.read_text()) if HAND.exists() else {}
    result = {rel: e for rel, e in old.items() if rel in models}
    todo = collections.defaultdict(list)
    for rel, model_sha in models.items():
        prev = old.get(rel)
        keep = (prev and prev["source_sha256"] == model_sha
                and prev["accepted"] and prev["origin"] != "hand"
                and rel not in hand and not args.regenerate
                and not (args.improve and prev["note"]))
        if any(f in rel for f in args.filter or [""]) and not keep:
            todo[(model_sha, rel in hand and rel)].append(rel)
    want = stamp()
    if args.objs:
        want = {"driver_sha256": want["driver_sha256"],
                "objs": str(args.objs.resolve())}
        driver_object(args.objs)

    def work(key):
        model_sha, hand_rel = key
        rel = todo[key][0]
        if hand_rel:
            return hand_entry(CORPUS / rel, model_sha, want, hand[hand_rel], args.objs)
        prev = old.get(rel)
        improve = (args.improve and not args.regenerate and prev
                   and prev["accepted"] and prev["origin"] == "generated"
                   and prev["source_sha256"] == model_sha)
        return generate(CORPUS / rel, model_sha, want, args.max_attempts,
                        args.objs, prev if improve else None)

    def save():
        OUT.parent.mkdir(parents=True, exist_ok=True)
        OUT.write_bytes(serialize({"schema": 1, "stanc3_sha": pinned_sha(),
                                   "models": result}))

    print(f"{sum(map(len, todo.values()))} models to generate, "
          f"{args.jobs} jobs", file=sys.stderr)
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        ready = sorted(todo, key=lambda k: not (
            args.objs and (args.objs / k[0] / "status").exists()))
        futures = {pool.submit(work, k): k for k in ready}
        for i, fut in enumerate(concurrent.futures.as_completed(futures), 1):
            entry = fut.result()
            for rel in todo[futures[fut]]:
                result[rel] = entry
                print(f"[{i}/{len(todo)}] {rel}: {entry['origin']} "
                      f"accepted={entry['accepted']} "
                      f"attempts={entry['attempts']} {entry['note']}",
                      file=sys.stderr)
            if i % 50 == 0:
                save()
    save()
    counts = collections.Counter((e["origin"], e["accepted"])
                                 for e in result.values())
    for (origin, accepted), n in sorted(counts.items()):
        print(f"{origin:12} accepted={accepted!s:5} {n}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
