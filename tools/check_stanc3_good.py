#!/usr/bin/env python3
"""Replay every model in stanc3's test/integration/good against CmdStan.

Each model runs once through stanli_check at five points, and its log
density, gradient and write_array row are compared with CmdStan's answers
recorded in tests/stanc3_good/references.json.xz. A model where stanli
differs from CmdStan must have a matching entry in
tests/stanc3_good/ledger.json, and an entry that no longer matches fails
too, in either direction. Agreeing, including throwing the same error, needs
no entry.

Ordinary runs never touch CmdStan. `--record` rebuilds the references from
the pinned CmdStan. See tests/stanc3_good/README.md.
"""
from __future__ import annotations

import argparse
import concurrent.futures
import gzip
import hashlib
import json
import lzma
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
REFERENCES = SUITE / "references.json.xz"
LEDGER = SUITE / "ledger.json"
OVERRIDES = SUITE / "overrides.json"
POINTS = (0, 1, 2, 3, 4)
MAX_REL = 1e-9
TIMEOUT = 120.0
LEDGERED_TIMEOUT = 5.0
SCHEMA = 1

# Statuses where stanli does what CmdStan does, including throwing the same
# error; where there is no CmdStan answer to compare with; and the rest, which
# need a ledger entry. "points" is a model that ran on both sides with some
# points disagreeing.
AGREE_VALUES = ("verified",)
AGREE_ERRORS = ("rejected_both", "data_rejected")
NOT_CHECKABLE = ("cmdstan_timeout", "cmdstan_crashed", "data_unavailable",
                 "stanc_rejected")
DIVERGES = ("points", "data_message_mismatch", "unsupported",
            "one_side_threw", "timeout", "crashed")
POINT_VERDICTS = ("ok", "both_threw", "mismatch", "one_side_threw",
                  "message_mismatch", "wa_unsupported")
_RANK = {"points": 6, "data_message_mismatch": 6, "unsupported": 5,
         "one_side_threw": 2, "timeout": 1, "crashed": 0}
_AGREES_RANK = 9

# A Stan Math validation error. stanli reporting one of these at compile
# time, on data CmdStan accepted, is a disagreement and not a missing feature.
_UNSUPPORTED = re.compile(r"unsupported|not supported|requires the MIR "
                          r"interpreter|^mir: ")
_DOMAIN_ERROR = re.compile(r", but must be|is not a valid|is not positive "
                           r"definite|is not symmetric|must match in size")


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def source_digest(path):
    return sha256(pathlib.Path(path).read_bytes().replace(b"\r\n", b"\n"))


def load_gz(path):
    """A .json.gz or .json.xz file."""
    raw = pathlib.Path(path).read_bytes()
    unpack = lzma.decompress if str(path).endswith(".xz") else gzip.decompress
    return json.loads(unpack(raw))


def write_xz(path, value):
    """xz: the references are 3x smaller than with gzip."""
    text = json.dumps(value, separators=(",", ":"), sort_keys=True,
                      allow_nan=False) + "\n"
    pathlib.Path(path).write_bytes(
        lzma.compress(text.encode(), preset=9 | lzma.PRESET_EXTREME))


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
    # An entry CmdStan never accepted keeps its last rejected candidate. It
    # is still run: CmdStan's rejection is recorded and stanli has to reject
    # it the same way.
    return source, (data["models"].get(path) or {}).get("data")


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
    and may nest it. The leading function name is dropped too: stanli may
    report the variant it called (dae_tol for dae).
    """
    text = message.replace("\n", " ")
    text = text.split("Exception: ")[-1]
    text = re.split(r" \(in '", text)[0]
    text = re.sub(r"^[\w:]+: ", "", text)
    return normalize(text)


def normalize(text):
    """Numbers and single element indices removed: the engines format
    numbers differently, and one side may name the element of a vectorized
    argument ("Scale parameter[2]") where the other's scalar overload does
    not. A multi-dimensional index ("[1, 1]") is kept. Boost names the type
    it evaluated in, which differs by platform, so the template argument of a
    boost::math function name and its parameter types are made uniform."""
    text = re.sub(
        r"(boost::math::\w+)<(long double|double|__float128)>\(([^)]*)\)",
        lambda m: f"{m[1]}<T>({re.sub(re.escape(m[2]), 'T', m[3])})",
        text or "")
    text = re.sub(r"-?\d+(\.\d+)?([eE][+-]?\d+)?", "N", text)
    text = text.replace("[N]", "")
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
    if "threw" not in want and not accepted(("OK", want["lp_grad"])):
        # An all-NaN row is how log_prob refuses without throwing.
        want = {"threw": ""}
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


def classify(ref, run, unstable=()):
    """(status, points, details) for one model.

    A value mismatch at a point in `unstable` counts as agreement: the
    overrides' `platform_dependent` points, where CmdStan's own answer
    depends on the platform's libm and long double.

    status is one of the status groups above; points maps each non-ok point
    to its verdict when the status is "points". `run` is (returncode, stdout,
    stderr) or None for a timeout.
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
            if not _UNSUPPORTED.search(msg):
                return "data_message_mismatch", {}, [
                    f"CmdStan: {ref['message']}", f"stanli: {msg}"]
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
        want = ref["points"][str(k)]
        if "values" in want.get("wa", {}) and "names" not in want["wa"]:
            want = dict(want, wa=dict(want["wa"], names=ref["wa_names"]))
        verdict, why = point_verdict(want,
                                     parsed["points"].get(k))
        if verdict == "mismatch" and str(k) in unstable:
            verdict, why = "ok", []
        verdicts[k] = verdict
        details += [f"point {k}: {w}" for w in why]
    bad = {str(k): v for k, v in verdicts.items()
           if v not in ("ok", "both_threw")}
    if bad:
        return "points", bad, details
    if all(v == "both_threw" for v in verdicts.values()):
        return "rejected_both", {}, [ref["points"]["0"].get(
            "threw", "log_prob returns NaN")]
    return "verified", {}, []


def ledger_verdict(status, points, entry):
    """None when the computed result matches the ledger, else a message."""
    want = (("points", entry["points"]) if entry and "points" in entry
            else (entry["status"], {}) if entry and "status" in entry
            else None)
    got = (status, points) if status in DIVERGES else None
    if want == got:
        if entry and entry.get("cause", "").startswith("UNTRIAGED"):
            return "ledger entry has no named cause"
        return None
    describe = (lambda w: "agrees" if w is None
                else f"points {w[1]}" if w[0] == "points" else w[0])
    rank = (lambda w: _AGREES_RANK if w is None else _RANK.get(w[0], -1))
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
    if ref["outcome"] in ("ok", "data_fail", "stanc_fail"):
        timeout = (LEDGERED_TIMEOUT if entry and entry.get("status") in
                   ("timeout",) else ctx["timeout"])
        with tempfile.TemporaryDirectory(prefix="stanc3_good_") as work:
            data_file = pathlib.Path(work) / "data.json"
            data_file.write_bytes(blob)
            run = run_process([ctx["check"], source, data_file, "--points",
                               ",".join(map(str, POINTS)), "--wa-values"],
                              timeout)
    status, points, details = classify(ref, run, set(
        ctx["overrides"].get(path, {}).get("platform_dependent", ())))
    problem = ledger_verdict(status, points, entry)
    ctx["seconds"][path] = time.monotonic() - started
    return path, status, points, details, problem


def replay(models, ctx, jobs, verbose=False):
    failures, counts, groups = [], {}, dict.fromkeys(
        ("values", "errors", "not checkable", "ledgered"), 0)
    with concurrent.futures.ThreadPoolExecutor(jobs) as pool:
        for path, status, points, details, problem in pool.map(
                lambda p: replay_one(p, ctx), models):
            counts[status] = counts.get(status, 0) + 1
            ctx["results"][path] = (status, points, details)
            if problem:
                failures.append((path, problem, details))
            else:
                groups["values" if status in AGREE_VALUES
                       else "errors" if status in AGREE_ERRORS
                       else "not checkable" if status in NOT_CHECKABLE
                       else "ledgered"] += 1
    for path, problem, details in failures:
        print(f"FAIL {path}: {problem}")
        for line in details[:12]:
            print(f"    {line}")
        if len(details) > 12:
            print(f"    ... {len(details) - 12} more")
    if verbose:
        print(", ".join(f"{k}={v}" for k, v in sorted(
            counts.items(), key=lambda kv: str(kv[0]))))
    print(f"{len(models)} models: {groups['values']} agree on values, "
          f"{groups['errors']} agree on errors, {groups['not checkable']} "
          f"not checkable, {groups['ledgered']} ledgered divergences, "
          f"{len(failures)} failures")
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
        if "cause" not in entry or ("status" in entry) + ("points" in entry) != 1:
            problems.append(f"ledger entry {path} needs a cause and one of "
                            "status or points")
        elif "status" in entry and entry["status"] not in DIVERGES[1:]:
            problems.append(f"ledger entry {path}: unknown status")
        elif "points" in entry and not set(entry["points"].values()) <= set(
                POINT_VERDICTS[2:]):
            problems.append(f"ledger entry {path}: unknown point verdict")
    return problems


def update_ledger(ledger, results):
    """Rewrite the ledger from this run, keeping causes that still apply."""
    out = {}
    for path, (status, points, details) in sorted(results.items()):
        if status not in DIVERGES:
            continue
        old = ledger.get(path, {})
        entry = ({"points": points} if status == "points"
                 else {"status": status})
        if {k: old.get(k) for k in entry} == entry and "cause" in old:
            entry["cause"] = old["cause"]
        elif status == "data_message_mismatch":
            entry["cause"] = "UNTRIAGED: " + " / ".join(details)[:400]
        elif status == "points" and set(points.values()) == {"wa_unsupported"}:
            entry["cause"] = details[0].split(": ", 1)[1][:300]
        elif status != "unsupported":
            entry["cause"] = "UNTRIAGED: " + "; ".join(details[:3])[:300]
        else:
            entry["cause"] = (details[0] if details else status)[:300]
        out[path] = entry
    return out


# ---------------------------------------------------------------------------
# Recording. Only this path touches CmdStan.


def record(models, ctx, args):
    toolchain.require_pinned_toolchain(args.cmdstan, args.stanc)
    flags_file = args.ref_objs / "FLAGS"
    if flags_file.exists():
        flags = json.loads(flags_file.read_text())
        compile_flags = flags["compile"]
        link_tail = flags["link_tail"][:-2]  # drop the "-o ref" placeholder
    else:
        import cmdstan_ref  # only --record touches CmdStan
        whole = cmdstan_ref.compile_cmd(args.cmdstan, "M", "D", "E")
        compile_flags = whole[:whole.index("-include")]
        link_tail = whole[whole.index("D") + 1:whole.index("-o")]
        flags = {}
    stanc_args = flags.get("stanc_args", [])
    default_opt = next((f for f in compile_flags if f.startswith("-O")), "")
    work = args.work.resolve()
    work.mkdir(parents=True, exist_ok=True)
    driver = REPO / "tools" / "ref_driver.cpp"
    driver_sha = source_digest(driver)[:16]
    driver_o = work / f"ref_driver-{driver_sha}.o"
    if not driver_o.exists():
        must([*compile_flags, "-c", driver, "-o", str(driver_o) + ".part"])
        os.replace(str(driver_o) + ".part", driver_o)

    def model_object(path, source):
        sha = sha256(source.read_bytes())
        for root in (args.ref_objs, work / "objs"):
            status = root / sha / "status"
            if status.exists():
                return root / sha, status.read_text().strip()
        if not args.build_objects and (
                "source" not in ctx["overrides"].get(path, {})):
            # The large function-signature models need several GB each to
            # compile; building them here beside the prebuild swaps the
            # machine. Only adapted sources, which are small, build here.
            raise RuntimeError(f"{path}: no CmdStan object in {args.ref_objs}")
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
        opt = next((f for f in compile_flags if f.startswith("-O")), "")
        (obj / "opt").write_text(opt + "\n")
        (obj / "status").write_text(status + "\n")
        return obj, status

    def record_one(path):
        source, data = model_input(path, ctx["overrides"], ctx["data"])
        entry = {"source_sha256": source_digest(source)}
        if data is None:
            given = ctx["data"]["models"].get(path, {})
            return path, dict(entry, outcome="no_data", message=strip_paths(
                first_line(given.get("note", "")) or "no data"))
        blob = data_bytes(data)
        entry["data_sha256"] = sha256(blob)
        obj, status = model_object(path, source)
        # Objects without an opt file were built with FLAGS as given.
        opt_file = obj / "opt"
        entry["cmdstan_opt"] = (opt_file.read_text().strip()
                                if opt_file.exists() else default_opt)
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
        named = [p["wa"] for p in points.values() if "names" in p.get("wa", {})]
        if named and all(w["names"] == named[0]["names"] for w in named):
            # One copy of the column names; they are most of the file.
            entry["wa_names"] = named[0]["names"]
            for w in named:
                del w["names"]
        return path, dict(entry, outcome="ok", points=points)

    done = 0
    known = set(corpus_models())
    models_out = {p: e for p, e in ctx["refs"]["models"].items() if p in known}
    same_driver = (ctx["refs"].get("toolchain", {}).get("ref_driver_sha256")
                   == source_digest(driver))
    if not same_driver and args.filter and ctx["refs"]["models"]:
        raise SystemExit("ref_driver.cpp changed: re-record the whole corpus")
    if same_driver and not args.all:
        models = [m for m in models if stale(m, ctx)]
    print(f"recording {len(models)} models", flush=True)

    def guarded(path):
        try:
            return record_one(path)
        except RuntimeError as error:
            return path, error

    errors = []
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        for path, entry in pool.map(guarded, models):
            if isinstance(entry, RuntimeError):
                errors.append(str(entry))
                print(f"NOT RECORDED {entry}", flush=True)
            else:
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
        "flags": [f for f in compile_flags[1:]
                  if not f.startswith(("-I", "-O"))],
        "ref_driver_sha256": source_digest(driver),
        "platform": f"{os.uname().sysname} {os.uname().machine}",
    }
    for key, sub in (("cmdstan", ""), ("stan", "stan"),
                     ("math", "stan/lib/stan_math")):
        provenance[key + "_commit"] = toolchain.git_head(args.cmdstan / sub)
    write_xz(REFERENCES, {"schema": SCHEMA, "toolchain": provenance,
                          "models": dict(sorted(models_out.items()))})
    ctx["refs"] = load_gz(REFERENCES)
    return errors


def strip_paths(message):
    """A message without the recording machine's directories."""
    return re.sub(r"(/[^\s'\"]+/)([^/\s'\"]+\.(stan|hpp))", r"\2", message)


def stale(path, ctx):
    """Whether a model's recorded inputs differ from the current ones."""
    ref = ctx["refs"]["models"].get(path)
    if ref is None:
        return True
    source, data = model_input(path, ctx["overrides"], ctx["data"])
    data_sha = None if data is None else sha256(data_bytes(data))
    return (ref["source_sha256"] != source_digest(source)
            or ref.get("data_sha256") != data_sha)


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
    ap.add_argument("--build-objects", action="store_true",
                    help="with --record, compile missing CmdStan model "
                         "objects (several GB each for the largest models; "
                         "use a small --jobs)")
    ap.add_argument("--all", action="store_true",
                    help="with --record, re-record models whose inputs and "
                         "driver are unchanged too")
    ap.add_argument("--ref-timeout", type=float, default=600.0)
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
           "check": next((p.resolve() for p in (
               args.build / "stanli_check", args.build / "stanli_check.exe")
               if p.exists()), args.build / "stanli_check"),
           "timeout": args.timeout, "seconds": {}, "results": {},
           "refs": (load_gz(REFERENCES) if REFERENCES.exists()
                    else {"models": {}})}
    selected = [m for m in models if args.filter in m]
    if args.shard:
        index, count = (int(x) for x in args.shard.split("/"))
        selected = selected[index::count]

    if args.record:
        args.cmdstan, args.stanc = args.cmdstan.resolve(), args.stanc.resolve()
        if record(selected, ctx, args):
            print("some models were not recorded; see NOT RECORDED above")
            return 1

    problems = []
    if ctx["refs"].get("schema", SCHEMA) != SCHEMA:
        problems.append("unknown reference schema")
    problems += inventory_problems(models, ctx["refs"], ledger, overrides)
    for p in problems:
        print("FAIL", p)
    if not ctx["check"].exists():
        print(f"FAIL no stanli_check at {ctx['check']}")
        return 1
    failures = replay(selected, ctx, args.jobs, args.times)
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
