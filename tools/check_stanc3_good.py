#!/usr/bin/env python3
"""Replay every model in stanc3's test/integration/good against CmdStan.

Each model runs once through stanli_check at four points, and its log
density, gradient and write_array row are compared with CmdStan's answers
recorded in tests/stanc3_good/references.json.gz. A model that is not fully
verified must have a matching entry in tests/stanc3_good/ledger.json, and an
entry that no longer matches fails too, in either direction.

Ordinary runs never touch CmdStan. `--record` rebuilds the references from
the pinned CmdStan. See tests/stanc3_good/README.md.
"""
from __future__ import annotations

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

REPO = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "tools"))
import toolchain  # noqa: E402
from verify_refs import pair_dev  # noqa: E402

SUITE = REPO / "tests" / "stanc3_good"
CORPUS = REPO / "deps" / "stanc3-src" / "test" / "integration" / "good"
DATA = SUITE / "data.json.gz"
REFERENCES = SUITE / "references.json.gz"
LEDGER = SUITE / "ledger.json"
OVERRIDES = SUITE / "overrides.json"
POINTS = (0, 1, 2, 3)
MAX_REL = 1e-9
TIMEOUT = 120.0
LEDGERED_TIMEOUT = 5.0
SCHEMA = 1

# Model-level statuses, best first. A point-level result (some points
# disagree) sits between "rejected_both" and "unsupported" when the two
# directions are named in a failure message; equality is what gates.
MODEL_STATUSES = (
    "verified",
    "rejected_both",     # every point threw on both sides, messages agree
    "data_rejected",     # both reject the data, messages agree
    "unsupported",       # stanli refuses to compile it; cause is its reason
    "stanc_rejected",    # stanc refuses the program
    "data_unavailable",  # no data: stanc could not generate any
    "cmdstan_timeout",   # CmdStan does not finish; stanli is not run
    "cmdstan_crashed",   # CmdStan dies (a stack overflow, say); likewise
    "one_side_threw",    # model-level: data or compile accepted by one side
    "timeout",
    "crashed",
)
POINT_VERDICTS = ("ok", "both_threw", "mismatch", "one_side_threw",
                  "message_mismatch", "wa_unsupported")
_RANK = {"verified": 9, "rejected_both": 8, "data_rejected": 7,
         "points": 6, "unsupported": 5, "stanc_rejected": 5,
         "data_unavailable": 5, "cmdstan_timeout": 5, "cmdstan_crashed": 5,
         "one_side_threw": 2, "timeout": 1, "crashed": 0}

# A Stan Math validation error. stanli reporting one of these at compile
# time, on data CmdStan accepted, is a disagreement and not a missing feature.
_UNSUPPORTED = re.compile(r"unsupported|not supported|requires the MIR "
                          r"interpreter")
_DOMAIN_ERROR = re.compile(r", but must be|is not a valid|is not positive "
                           r"definite|is not symmetric|must match in size")


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def source_digest(path):
    return sha256(pathlib.Path(path).read_bytes().replace(b"\r\n", b"\n"))


def load_gz(path):
    return json.loads(gzip.decompress(pathlib.Path(path).read_bytes()))


def write_gz(path, value):
    text = json.dumps(value, indent=1, sort_keys=True, allow_nan=False) + "\n"
    pathlib.Path(path).write_bytes(gzip.compress(text.encode(), 9, mtime=0))


def check_corpus(corpus=CORPUS):
    """The pinned stanc3 checkout, or an error. Never a skip."""
    if not corpus.is_dir():
        raise SystemExit(f"{corpus} is missing; run tools/dev_setup.sh")
    want = toolchain.pins()["STANC3_SRC_SHA"]
    try:
        head = toolchain.git_head(corpus)
    except (OSError, subprocess.CalledProcessError):
        head = "unknown"
    if head != want:
        raise SystemExit(f"{corpus} is at {head}, deps/pins.env wants "
                         f"STANC3_SRC_SHA={want}")


def corpus_models(corpus=CORPUS):
    return sorted(p.relative_to(corpus).as_posix()
                  for p in corpus.rglob("*.stan"))


def model_input(path, overrides, data, corpus=CORPUS):
    """(source path, data object or None) for one model."""
    override = overrides.get(path, {})
    source = (SUITE / override["source"] if "source" in override
              else corpus / path)
    if "data" in override:
        return source, json.loads((SUITE / override["data"]).read_text())
    entry = data["models"].get(path)
    return source, (entry or {}).get("data")


def data_bytes(obj):
    return json.dumps(obj).encode()


# ---------------------------------------------------------------------------
# Parsing driver output.

_STATUS = ("OK", "EVAL_FAIL", "COMPILE_FAIL", "DATA_FAIL")


def parse_output(text):
    """{'pre': status, 'points': {k: {'status', 'wa'}}} from --points output.

    A status is (kind, payload). For OK the payload is the value strings,
    otherwise the message. 'wa' is ('ok', names, values), ('threw', msg) or
    None. The last status line of a section wins, so a Stan print earlier
    in the section cannot shadow it.
    """
    sections = {"pre": {"status": None, "wa": None}}
    order, current = [], "pre"
    names = None
    for line in text.splitlines():
        head, _, tail = line.partition(" ")
        if head == "POINT" and tail.strip().isdigit():
            current = int(tail)
            sections[current] = {"status": None, "wa": None}
            order.append(current)
            names = None
        elif head in _STATUS:
            sections[current]["status"] = (
                head, tail.split() if head == "OK" else tail.strip())
        elif head == "WANAMES":
            if tail == "FAIL no write_array":
                # stanli_check's spelling of a model with no columns.
                sections[current]["wa"] = ("ok", [], [])
            elif tail.startswith("FAIL"):
                sections[current]["wa"] = ("threw", tail[4:].strip())
                names = None
            else:
                names = tail.split(",") if tail else []
        elif head == "WAVALS" and names is not None:
            values = tail.split()
            sections[current]["wa"] = ("ok", names, values)
        elif line == "WANAMES":
            names = []
        elif line == "WAVALS" and names is not None:
            sections[current]["wa"] = ("ok", names, [])
    return {"pre": sections["pre"]["status"],
            "points": {k: sections[k] for k in order}}


def run_process(argv, timeout):
    """(returncode, stdout, stderr) or None on timeout."""
    try:
        r = subprocess.run([str(a) for a in argv], capture_output=True,
                           text=True, timeout=timeout, cwd=REPO)
    except subprocess.TimeoutExpired:
        return None
    return r.returncode, r.stdout, r.stderr


# ---------------------------------------------------------------------------
# Comparison.


def message_core(message):
    """What stanli's message has to contain of CmdStan's.

    CmdStan wraps a message as "Exception: ... (in 'file', line N, ...)"
    and may nest it; numbers are formatted differently by the two engines.
    """
    text = message.replace("\n", " ")
    text = text.split("Exception: ")[-1]
    text = re.split(r" \(in '", text)[0]
    return normalize(text)


def normalize(text):
    text = re.sub(r"-?\d+(\.\d+)?([eE][+-]?\d+)?", "N", text or "")
    text = re.sub(r"\s+", " ", text).strip()
    return text.rstrip("!. ")


def messages_agree(cmdstan, stanli):
    core = message_core(cmdstan or "")
    return not core or core in normalize(stanli or "")


def accepted(status):
    """Did this side return values? An all-NaN row counts as a refusal."""
    if not status or status[0] != "OK":
        return False
    values = [float(x) for x in status[1]]
    return not (values and all(v != v for v in values))


def compare_values(want, got, field, labels=None):
    """Mismatch descriptions for two lists of value strings."""
    if len(want) != len(got):
        return [f"{field} width CmdStan {len(want)}, stanli {len(got)}"]
    out = []
    for i, (a, b) in enumerate(zip(want, got)):
        rel, _ = pair_dev(float(a), float(b))
        if rel > MAX_REL:
            label = labels[i] if labels else str(i)
            out.append(f"{field}[{label}] stanli={b} CmdStan={a} "
                       f"rel={rel:.3g}")
    return out


def point_verdict(want, got):
    """(verdict, details) for one point. want is the reference entry."""
    status = got["status"] if got else None
    if "threw" in want:
        if status and (status[0] == "EVAL_FAIL" or not accepted(status)):
            stanli_msg = status[1] if status[0] == "EVAL_FAIL" else ""
            if status[0] == "OK" or messages_agree(want["threw"], stanli_msg):
                return "both_threw", []
            return "message_mismatch", [f"CmdStan: {want['threw']}",
                                        f"stanli: {stanli_msg}"]
        return "one_side_threw", [f"CmdStan threw: {want['threw']}",
                                  "stanli returned values"]
    if not status or status[0] != "OK":
        msg = status[1] if status else "no output"
        return "one_side_threw", [f"stanli threw: {msg}",
                                  "CmdStan returned values"]
    if not accepted(status):
        return "one_side_threw", ["stanli returned all-NaN",
                                  "CmdStan returned values"]
    details = compare_values(want["lp_grad"], status[1], "lp_grad")
    wa_want, wa_got = want.get("wa"), got.get("wa")
    if wa_want is not None:
        if "threw" in wa_want:
            if wa_got is None or wa_got[0] != "threw":
                return "one_side_threw", [
                    f"CmdStan write_array threw: {wa_want['threw']}",
                    "stanli write_array did not"] + details
            if not messages_agree(wa_want["threw"], wa_got[1]):
                details.append(f"write_array messages: CmdStan "
                               f"{wa_want['threw']!r}, stanli {wa_got[1]!r}")
                return ("mismatch" if len(details) > 1
                        else "message_mismatch"), details
        elif wa_got is None or wa_got[0] != "ok":
            why = wa_got[1] if wa_got else "no write_array output"
            if _UNSUPPORTED.search(why):
                return ("mismatch" if details else "wa_unsupported"), [
                    f"stanli write_array unsupported: {why}"] + details
            return "one_side_threw", [f"stanli write_array threw: {why}",
                                      "CmdStan write_array returned values"
                                      ] + details
        elif wa_want["names"] != wa_got[1]:
            details.append("write_array names differ: CmdStan "
                           f"{','.join(wa_want['names'])[:120]} stanli "
                           f"{','.join(wa_got[1])[:120]}")
        else:
            details += compare_values(wa_want["values"], wa_got[2],
                                      "write_array", wa_want["names"])
    return ("mismatch" if details else "ok"), details


def classify(ref, run):
    """(status, points, details) for one model.

    status is a MODEL_STATUSES entry or "points" when the model ran on both
    sides and some points disagree; points maps each non-ok point to its
    verdict. `run` is (returncode, stdout, stderr) or None for a timeout.
    """
    outcome = ref["outcome"]
    if outcome == "no_data":
        return "data_unavailable", {}, [ref.get("message", "")]
    if outcome == "timeout":
        return "cmdstan_timeout", {}, ["CmdStan did not finish"]
    if outcome == "crashed":
        return "cmdstan_crashed", {}, [ref.get("message", "")]
    if run is None:
        return "timeout", {}, ["stanli did not finish"]
    code, out, err = run
    parsed = parse_output(out)
    pre = parsed["pre"]
    if outcome == "stanc_fail":
        if pre and pre[0] == "COMPILE_FAIL":
            return "stanc_rejected", {}, [ref["message"]]
        return "one_side_threw", {}, ["stanc rejects it, stanli compiled it"]
    if pre and pre[0] == "COMPILE_FAIL":
        msg = pre[1]
        if outcome == "data_fail":
            if messages_agree(ref["message"], msg):
                return "data_rejected", {}, [ref["message"]]
            return "unsupported", {}, [msg]
        if _DOMAIN_ERROR.search(msg):
            return "one_side_threw", {}, [f"stanli rejects the data: {msg}"]
        return "unsupported", {}, [msg]
    if code not in (0, 1) or not parsed["points"]:
        tail = (err or out).strip().splitlines()[-1:] or [f"exit {code}"]
        return "crashed", {}, [f"exit {code}: {tail[0][:200]}"]
    if outcome == "data_fail":
        return "one_side_threw", {}, [
            f"CmdStan rejects the data: {ref['message']}",
            "stanli accepted it"]
    verdicts, details = {}, []
    for k in POINTS:
        verdict, why = point_verdict(ref["points"][str(k)],
                                     parsed["points"].get(k))
        verdicts[k] = verdict
        details += [f"point {k}: {w}" for w in why]
    bad = {str(k): v for k, v in verdicts.items()
           if v not in ("ok", "both_threw")}
    if bad:
        return "points", bad, details
    if all(v == "both_threw" for v in verdicts.values()):
        return "rejected_both", {}, [ref["points"]["0"]["threw"]]
    return "verified", {}, []


def ledger_verdict(status, points, entry):
    """None when the computed result matches the ledger, else a message."""
    want = (None if entry is None
            else ("points", entry["points"]) if "points" in entry
            else (entry["status"], {}))
    got = None if status == "verified" else (status, points)
    if want == got:
        if entry and entry.get("cause", "").startswith("UNTRIAGED"):
            return "ledger entry has no named cause"
        return None
    describe = (lambda w: "verified" if w is None
                else f"points {w[1]}" if w[0] == "points" else w[0])
    rank = (lambda w: _RANK["verified"] if w is None else _RANK[w[0]])
    if rank(got) > rank(want):
        kind = "improved; update the ledger entry"
    elif rank(got) < rank(want):
        kind = "REGRESSED"
    else:
        kind = "changed"
    return f"{kind}: ledger says {describe(want)}, now {describe(got)}"


# ---------------------------------------------------------------------------
# Replay.


def replay_one(path, ctx):
    source, data = model_input(path, ctx["overrides"], ctx["data"])
    ref = ctx["refs"]["models"].get(path)
    entry = ctx["ledger"].get(path)
    if ref is None:
        return path, None, {}, [], "no reference; run --record"
    if ref["source_sha256"] != source_digest(source):
        return path, None, {}, [], "source changed; re-record"
    blob = None if data is None else data_bytes(data)
    if (sha256(blob) if blob else None) != ref.get("data_sha256"):
        return path, None, {}, [], "data changed; re-record"
    started = time.monotonic()
    run = None
    if ref["outcome"] not in ("no_data", "timeout", "crashed"):
        timeout = (LEDGERED_TIMEOUT if entry and entry.get("status") in
                   ("timeout",) else ctx["timeout"])
        with tempfile.TemporaryDirectory(prefix="stanc3_good_") as work:
            data_file = pathlib.Path(work) / "data.json"
            data_file.write_bytes(blob)
            run = run_process([ctx["check"], source, data_file, "--points",
                               ",".join(map(str, POINTS)), "--wa-values"],
                              timeout)
    status, points, details = classify(ref, run)
    problem = ledger_verdict(status, points, entry)
    ctx["seconds"][path] = time.monotonic() - started
    return path, status, points, details, problem


def replay(models, ctx, jobs):
    failures, counts = [], {}
    with concurrent.futures.ThreadPoolExecutor(jobs) as pool:
        for path, status, points, details, problem in pool.map(
                lambda p: replay_one(p, ctx), models):
            counts[status] = counts.get(status, 0) + 1
            ctx["results"][path] = (status, points, details)
            if problem:
                failures.append((path, problem, details))
    for path, problem, details in failures:
        print(f"FAIL {path}: {problem}")
        for line in details[:12]:
            print(f"    {line}")
        if len(details) > 12:
            print(f"    ... {len(details) - 12} more")
    tally = ", ".join(f"{k}={v}" for k, v in sorted(
        counts.items(), key=lambda kv: str(kv[0])))
    print(f"{len(models)} models: {tally}; {len(failures)} failures")
    return failures


def inventory_problems(models, refs, ledger, overrides):
    known = set(models)
    problems = [f"reference for unknown model {p}"
                for p in sorted(set(refs["models"]) - known)]
    problems += [f"no reference for {p}" for p in sorted(known - set(refs["models"]))]
    problems += [f"ledger entry for unknown model {p}"
                 for p in sorted(set(ledger) - known)]
    problems += [f"override for unknown model {p}"
                 for p in sorted(set(overrides) - known)]
    for path, entry in overrides.items():
        copy = entry.get("copy")
        if copy and path in known and (
                source_digest(SUITE / copy) != source_digest(CORPUS / path)):
            problems.append(f"override copy {copy} differs from upstream "
                            f"{path}; a changed source is a 'source' override")
    for path, entry in ledger.items():
        if "cause" not in entry or ("status" in entry) == ("points" in entry):
            problems.append(f"ledger entry {path} needs a cause and exactly "
                            "one of status/points")
        elif "status" in entry and entry["status"] not in MODEL_STATUSES[1:]:
            problems.append(f"ledger entry {path}: unknown status")
        elif "points" in entry and not set(entry["points"].values()) <= set(
                POINT_VERDICTS[2:]):
            problems.append(f"ledger entry {path}: unknown point verdict")
    return problems


def update_ledger(ledger, results):
    """Rewrite the ledger from this run, keeping causes that still apply."""
    out = {}
    for path, (status, points, details) in sorted(results.items()):
        if status in ("verified", None):
            continue
        old = ledger.get(path, {})
        entry = ({"points": points} if status == "points"
                 else {"status": status})
        same = ({k: old.get(k) for k in entry} == entry)
        if same and "cause" in old:
            entry["cause"] = old["cause"]
        elif status == "points" and set(points.values()) == {"wa_unsupported"}:
            entry["cause"] = details[0].split(": ", 1)[1][:300]
        elif status in ("points", "one_side_threw", "crashed", "timeout",
                        "cmdstan_crashed", "cmdstan_timeout"):
            entry["cause"] = "UNTRIAGED: " + "; ".join(details[:3])[:300]
        else:
            entry["cause"] = (details[0] if details else status)[:300]
        out[path] = entry
    return out


# ---------------------------------------------------------------------------
# Recording. Only this path touches CmdStan.


def record(models, ctx, args):
    toolchain.require_pinned_toolchain(args.cmdstan, args.stanc)
    flags = json.loads((args.ref_objs / "FLAGS").read_text())
    compile_flags = flags["compile"]
    link_tail = flags["link_tail"][:-2]  # drop the "-o ref" placeholder
    stanc_args = flags.get("stanc_args", [])
    work = args.work.resolve()
    work.mkdir(parents=True, exist_ok=True)
    driver = REPO / "tools" / "ref_driver.cpp"
    driver_sha = source_digest(driver)[:16]
    driver_o = work / f"ref_driver-{driver_sha}.o"
    if not driver_o.exists():
        must([*compile_flags, "-c", driver, "-o", str(driver_o) + ".part"])
        os.replace(str(driver_o) + ".part", driver_o)

    def model_object(source):
        sha = sha256(source.read_bytes())
        for root in (args.ref_objs, work / "objs"):
            status = root / sha / "status"
            if status.exists():
                return root / sha, status.read_text().strip()
        obj = work / "objs" / sha
        obj.mkdir(parents=True, exist_ok=True)
        r = run_process([args.stanc, *stanc_args, source,
                         f"--o={obj / 'model.hpp'}"], 600)
        if r is None or r[0]:
            status = "stanc_fail: " + (r[2] or r[1] if r else "timeout").strip()
        else:
            r = run_process([*compile_flags, "-x", "c++", "-c",
                             obj / "model.hpp", "-o", obj / "model.o"], 1800)
            status = ("ok" if r and not r[0] else
                      "cc_fail: " + ((r[2] if r else "timeout")[-2000:]))
        (obj / "status").write_text(status + "\n")
        return obj, status

    def record_one(path):
        source, data = model_input(path, ctx["overrides"], ctx["data"])
        entry = {"source_sha256": source_digest(source)}
        if data is None:
            note = ctx["data"]["models"].get(path, {}).get("note", "")
            return path, dict(entry, outcome="no_data",
                              message=note or "no data")
        blob = data_bytes(data)
        entry["data_sha256"] = sha256(blob)
        obj, status = model_object(source)
        if status.startswith("stanc_fail"):
            return path, dict(entry, outcome="stanc_fail",
                              message=strip_paths(first_line(
                                  status[len("stanc_fail: "):])))
        if status != "ok":
            raise RuntimeError(f"{path}: CmdStan build failed: {status}")
        exe = work / "bin" / f"{obj.name[:24]}-{driver_sha}"
        if not exe.exists():
            exe.parent.mkdir(exist_ok=True)
            must(["clang++", obj / "model.o", driver_o, *link_tail,
                  "-o", str(exe) + ".part"])
            os.replace(str(exe) + ".part", exe)
        with tempfile.TemporaryDirectory(prefix="stanc3_good_ref_") as tmp:
            data_file = pathlib.Path(tmp) / "data.json"
            data_file.write_bytes(blob)
            r = run_process([exe, data_file, "--points",
                             ",".join(map(str, POINTS))], args.ref_timeout)
        if r is None:
            return path, dict(entry, outcome="timeout")
        parsed = parse_output(r[1])
        if parsed["pre"] and parsed["pre"][0] in ("DATA_FAIL", "EVAL_FAIL"):
            return path, dict(entry, outcome="data_fail",
                              message=strip_paths(parsed["pre"][1]))
        if set(parsed["points"]) != set(POINTS) or r[0] not in (0, 1):
            tail = (r[2] or r[1]).strip().splitlines()[-1:] or [f"exit {r[0]}"]
            return path, dict(entry, outcome="crashed",
                              message=strip_paths(
                                  f"exit {r[0]}: {tail[0][:300]}"))
        points = {}
        for k in POINTS:
            sec = parsed["points"][k]
            st = sec["status"]
            if st is None or st[0] != "OK":
                points[str(k)] = {"threw": strip_paths(st[1]) if st
                                  else "no output"}
                continue
            pt = {"lp_grad": st[1]}
            wa = sec["wa"]
            if wa is None:
                raise RuntimeError(f"{path}: no write_array output at {k}")
            pt["wa"] = ({"threw": strip_paths(wa[1])} if wa[0] == "threw"
                        else {"names": wa[1], "values": wa[2]})
            points[str(k)] = pt
        return path, dict(entry, outcome="ok", points=points)

    done = 0
    known = set(corpus_models())
    models_out = {p: e for p, e in ctx["refs"]["models"].items() if p in known}
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        for path, entry in pool.map(record_one, models):
            models_out[path] = entry
            done += 1
            if done % 50 == 0:
                print(f"recorded {done}/{len(models)}", flush=True)
    stanc = pathlib.Path(args.stanc)
    provenance = {
        "stanc_sha256": toolchain.sha256_file(stanc),
        "stanc_version": run_process([stanc, "--version"], 30)[1].strip(),
        "stanc_args": stanc_args,
        "stanc3_src": toolchain.pins()["STANC3_SRC_SHA"],
        "compiler": run_process(["clang++", "--version"], 30)[1].splitlines()[0],
        "flags": [f for f in compile_flags[1:] if not f.startswith("-I")],
        "ref_driver_sha256": source_digest(driver),
        "platform": f"{os.uname().sysname} {os.uname().machine}",
    }
    for key, sub in (("cmdstan", ""), ("stan", "stan"),
                     ("math", "stan/lib/stan_math")):
        provenance[key + "_commit"] = toolchain.git_head(args.cmdstan / sub)
    write_gz(REFERENCES, {"schema": SCHEMA, "toolchain": provenance,
                          "models": dict(sorted(models_out.items()))})
    ctx["refs"] = load_gz(REFERENCES)


def strip_paths(message):
    """A message without the recording machine's directories."""
    return re.sub(r"(/[^\s'\"]+/)([^/\s'\"]+\.(stan|hpp))", r"\2", message)


def first_line(text):
    lines = (text or "").strip().splitlines()
    return lines[0][:300] if lines else ""


def must(argv):
    r = subprocess.run([str(a) for a in argv], capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError(" ".join(map(str, argv[:3])) + " failed:\n"
                           + (r.stderr or r.stdout)[-3000:])


# ---------------------------------------------------------------------------


def main(argv=None):
    global REFERENCES, LEDGER
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--build", type=pathlib.Path, default=REPO / "build-rel")
    ap.add_argument("--shard", default="",
                    help="I/N: replay the models whose sorted index is I mod N")
    ap.add_argument("--filter", default="",
                    help="substring of the model path")
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 8) - 4))
    ap.add_argument("--timeout", type=float, default=TIMEOUT)
    ap.add_argument("--update-ledger", action="store_true",
                    help="rewrite the ledger from this run (whole corpus only)")
    ap.add_argument("--record", action="store_true",
                    help="record CmdStan references, then replay")
    ap.add_argument("--cmdstan", type=pathlib.Path, default=REPO / "deps/cmdstan")
    ap.add_argument("--stanc", type=pathlib.Path,
                    default=REPO / "deps/stanc3/stanc")
    ap.add_argument("--ref-objs", type=pathlib.Path,
                    default=REPO.parent / "stanc3-ref-objs",
                    help="prebuilt CmdStan model objects, <sha>/model.o")
    ap.add_argument("--work", type=pathlib.Path,
                    default=REPO / "build-stanc3-ref",
                    help="recorder cache: objects it builds, driver, binaries")
    ap.add_argument("--ref-timeout", type=float, default=60.0)
    ap.add_argument("--data", type=pathlib.Path, default=DATA)
    ap.add_argument("--references", type=pathlib.Path, default=REFERENCES)
    ap.add_argument("--ledger", type=pathlib.Path, default=LEDGER)
    ap.add_argument("--times", action="store_true",
                    help="print the slowest models")
    args = ap.parse_args(argv)
    REFERENCES, LEDGER = args.references, args.ledger

    check_corpus()
    models = corpus_models()
    data = load_gz(args.data)
    ledger = json.loads(LEDGER.read_text()) if LEDGER.exists() else {}
    overrides = json.loads(OVERRIDES.read_text())
    ctx = {"overrides": overrides, "data": data, "ledger": ledger,
           "check": (args.build / "stanli_check").resolve(),
           "timeout": args.timeout, "seconds": {}, "results": {},
           "refs": (load_gz(REFERENCES) if REFERENCES.exists()
                    else {"models": {}})}
    selected = [m for m in models if args.filter in m]
    if args.shard:
        index, count = (int(x) for x in args.shard.split("/"))
        selected = selected[index::count]

    if args.record:
        args.cmdstan, args.stanc = args.cmdstan.resolve(), args.stanc.resolve()
        record(selected, ctx, args)

    problems = []
    if ctx["refs"].get("schema", SCHEMA) != SCHEMA:
        problems.append("unknown reference schema")
    problems += inventory_problems(models, ctx["refs"], ledger, overrides)
    for p in problems:
        print("FAIL", p)
    if not ctx["check"].exists():
        print(f"FAIL no stanli_check at {ctx['check']}")
        return 1
    failures = replay(selected, ctx, args.jobs)
    if args.times:
        for path, sec in sorted(ctx["seconds"].items(),
                                key=lambda kv: -kv[1])[:15]:
            print(f"  {sec:6.2f}s {path}")
    if args.update_ledger:
        if args.filter or args.shard:
            print("--update-ledger needs the whole corpus")
            return 2
        LEDGER.write_text(json.dumps(update_ledger(ledger, ctx["results"]),
                                     indent=1, sort_keys=True) + "\n")
        print(f"wrote {LEDGER.relative_to(REPO)}")
        return 0
    return 1 if failures or problems else 0


if __name__ == "__main__":
    sys.exit(main())
