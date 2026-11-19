#!/usr/bin/env python3
"""Steps of the nightly fuzz workflow (.github/workflows/fuzz-nightly.yml).

    nightly.py seeds [--date YYYY-MM-DD]     the seed list for one night
    nightly.py summary RUN                   Markdown for the job summary
    nightly.py reduce RUN [binary options]   reduce one seed of each new bucket
    nightly.py file RUN --repo OWNER/NAME    open or update one issue per bucket
    nightly.py gate RUN                      exit 1 when the run needs a look

RUN is a fuzz.py --out directory. Only correctness buckets open issues and
decide the result. The performance buckets (fuzz.PERF_KINDS) time things on
a shared runner or compare sizes against fixed ratios, so they appear in
the summary only.
"""
from __future__ import annotations

import argparse
import datetime
import json
import os
import pathlib
import re
import signal
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import fuzz  # noqa: E402
import gen  # noqa: E402

REGRESSION = "0:200"
# About 20 minutes on a 4-core runner. A program costs 0.10 CPU seconds on
# an M-series Mac; runner cores were 1.6 to 2 times slower on verify_refs.
BUDGET = 20_000
# Night N runs BASE + N * STRIDE onward. The stride is larger than any
# budget we expect, so changing the budget never runs a seed twice.
BASE, STRIDE = 1_000_000, 50_000
EPOCH = datetime.date(2026, 11, 1)
MIN_AGREE = 0.5
LABEL = "fuzzer"
BODY_LIMIT = 60_000  # GitHub rejects bodies over 65,536 characters
SECTION_LIMIT = 12_000
PERF = {f"o1:{k}" for k in fuzz.PERF_KINDS}


def night_seeds(day):
    n = (day - EPOCH).days
    if n < 0:
        raise ValueError(f"{day} is before {EPOCH}")
    start = BASE + n * STRIDE
    return f"{REGRESSION},{start}:{start + BUDGET}"


def load(run):
    return json.loads((run / "results.json").read_text())


def buckets(results):
    """{bucket: {"new": seeds, "known": seeds}} over all programs."""
    out = {}
    for r in results["programs"]:
        for b in r.get("buckets", []):
            side = "known" if b in r.get("known", {}) else "new"
            out.setdefault(b, {"new": [], "known": []})[side].append(r["seed"])
    return out


def new_correctness(results):
    return {b: v["new"] for b, v in sorted(buckets(results).items())
            if v["new"] and b not in PERF}


def slug(bucket):
    return bucket.replace(":", "__")


def by_seed(results):
    return {r["seed"]: r for r in results["programs"]}


def outcome_counts(results):
    count = {}
    for r in results["programs"]:
        count[r["outcome"]] = count.get(r["outcome"], 0) + 1
    return count


def problems(results):
    """Reasons for the run to fail."""
    out = [f"new bucket {b} ({len(s)} programs)"
           for b, s in new_correctness(results).items()]
    count = outcome_counts(results)
    n = len(results["programs"])
    if count.get("harness_error"):
        out.append(f"{count['harness_error']} harness errors")
    if n and count.get("agree", 0) < MIN_AGREE * n:
        out.append(f"only {count.get('agree', 0)} of {n} programs agree; "
                   f"the tools may be broken")
    return out


# ---------------------------------------------------------------------------
# summary


def cmd_summary(args):
    results = load(args.run)
    n = len(results["programs"])
    p = print
    p("# Nightly stanc3 fuzz")
    p()
    wall = args.run / "wall_seconds"
    line = f"Seeds `{results['seeds']}`: {n} programs"
    if wall.exists():
        secs = float(wall.read_text())
        line += f" in {secs / 60:.1f} min ({n / secs:.2f} per second)"
    p(line + ".")
    p()
    p("| outcome | programs |")
    p("| --- | ---: |")
    for k, v in sorted(outcome_counts(results).items()):
        p(f"| {k} | {v} |")
    table = buckets(results)
    sections = (
        ("New correctness buckets",
         lambda b, v: b not in PERF and v["new"], "new"),
        ("Known correctness buckets",
         lambda b, v: b not in PERF and v["known"], "known"),
        ("Performance buckets (reported only, never filed)",
         lambda b, v: b in PERF, None))
    for title, keep, side in sections:
        rows = [(b, v[side] if side else v["new"] + v["known"])
                for b, v in sorted(table.items()) if keep(b, v)]
        p()
        p(f"## {title}")
        p()
        if not rows:
            p("None.")
            continue
        p("| bucket | programs | first seeds |")
        p("| --- | ---: | --- |")
        for b, seeds in rows:
            p(f"| `{b}` | {len(seeds)} | {', '.join(map(str, seeds[:5]))} |")
    errors = [r for r in results["programs"]
              if r["outcome"] == "harness_error"]
    if errors:
        p()
        p("## Harness errors")
        p()
        for r in errors[:10]:
            p(f"- seed {r['seed']}: `{r.get('error', '')[:200]}`")
    reasons = problems(results)
    p()
    p("**Result:** " + ("needs a look: " + "; ".join(reasons) if reasons
                        else "no new correctness buckets."))
    return 0


# ---------------------------------------------------------------------------
# reduce


def run_limited(argv, seconds):
    """Run argv in its own process group and kill the group after
    `seconds`. True when it finished in time and exited 0."""
    proc = subprocess.Popen(argv, start_new_session=True,
                            stdout=subprocess.DEVNULL,
                            stderr=subprocess.PIPE, text=True)
    try:
        _, err = proc.communicate(timeout=seconds)
    except subprocess.TimeoutExpired:
        os.killpg(proc.pid, signal.SIGKILL)
        proc.communicate()
        return False
    if proc.returncode:
        sys.stderr.write(err[-2000:])
    return proc.returncode == 0


def cmd_reduce(args):
    results = load(args.run)
    todo = list(new_correctness(results).items())[:args.max_buckets]
    cfg_file = args.run / "config.json"
    cfg_file.write_text(json.dumps(results["config"]) + "\n")
    for b, seeds in todo:
        out = args.run / "reduced" / slug(b)
        argv = [sys.executable, str(HERE / "reduce.py"), "--seed",
                str(seeds[0]), "--bucket", b, "--out", str(out),
                "--config", str(cfg_file), "--jobs", str(args.jobs)]
        for name in ("compiler", "check", "bench"):
            if getattr(args, name):
                argv += [f"--{name}", str(getattr(args, name))]
        ok = run_limited(argv, args.time_limit)
        print(f"{b}: seed {seeds[0]} "
              f"{'reduced' if ok else 'not reduced in time'}")
        if not ok:
            for f in ("reduced.stan", "data.json", "README.txt"):
                (out / f).unlink(missing_ok=True)
    return 0


# ---------------------------------------------------------------------------
# file


def clip(text, limit=SECTION_LIMIT):
    if len(text) <= limit:
        return text
    return text[:limit] + f"\n... ({len(text) - limit} more characters)"


def fence(text, lang=""):
    ticks = "```"
    while ticks in text:
        ticks += "`"
    return f"{ticks}{lang}\n{text.rstrip()}\n{ticks}"


def repro(seed, bucket):
    return fence(
        "tools/dev_setup.sh --no-embed --no-build\n"
        "cmake -B build-rel -DCMAKE_BUILD_TYPE=Release\n"
        "cmake --build build-rel --target stanli_check bench_grad\n"
        f"python3 tools/fuzz_stanc/fuzz.py --seeds {seed},{seed} --jobs 1 "
        f"--out fuzz-{seed}\n"
        f"python3 tools/fuzz_stanc/reduce.py --seed {seed} "
        f"--bucket {bucket} --out reduce-{seed}", "sh")


def issue_body(run, results, bucket, seeds, context):
    seed = seeds[0]
    record = by_seed(results)[seed]
    program = gen.generate(seed, results["config"])
    parts = [
        f"The nightly fuzzer found programs in bucket `{bucket}` that "
        f"`known.json` does not cover. tools/fuzz_stanc/README.md explains "
        f"buckets.",
        "",
        f"- Seed: {seed}",
        f"- Programs in this bucket in this run: {len(seeds)} "
        f"(seeds {', '.join(map(str, seeds[:20]))}"
        f"{', ...' if len(seeds) > 20 else ''})",
    ]
    if context.get("commit"):
        parts.append(f"- Commit: {context['commit']}")
    if context.get("run_url"):
        parts.append(f"- Run: {context['run_url']} (the artifact has the "
                     f"run directory)")
    parts += ["", "## Reproduce", "",
              "From a checkout of that commit:", "", repro(seed, bucket),
              "", "## Differences", "",
              fence(clip(json.dumps(record.get("diffs", []), indent=1)),
                    "json")]
    if record.get("bisect"):
        parts += ["", "Bisection over passes:", "",
                  fence(clip(json.dumps(record["bisect"], indent=1), 4000),
                        "json")]
    reduced = run / "reduced" / slug(bucket) / "reduced.stan"
    if reduced.exists():
        parts += ["", "## Reduced program", "",
                  fence(clip(reduced.read_text()), "stan")]
        data = reduced.with_name("data.json")
        if data.exists():
            parts += ["", fence(clip(data.read_text(), 4000), "json")]
    else:
        parts += ["", "The reducer did not finish within the time limit."]
    parts += ["", "## Program", "", fence(clip(program.source()), "stan"),
              "", fence(clip(json.dumps(program.data()), 4000), "json")]
    body = "\n".join(parts) + "\n"
    return body if len(body) <= BODY_LIMIT else clip(body, BODY_LIMIT)


def comment_body(bucket, seeds, context):
    lines = [f"The nightly fuzzer found {len(seeds)} more programs in "
             f"`{bucket}`: seeds {', '.join(map(str, seeds[:20]))}"
             f"{', ...' if len(seeds) > 20 else ''}."]
    if context.get("run_url"):
        lines.append(f"Run: {context['run_url']}")
    return "\n\n".join(lines) + "\n"


def gh(args, *argv, check=True):
    r = subprocess.run([args.gh, *argv, "--repo", args.repo],
                       capture_output=True, text=True)
    if check and r.returncode:
        raise RuntimeError(f"gh {' '.join(argv[:2])} failed: {r.stderr}")
    return r


def cmd_file(args):
    results = load(args.run)
    todo = new_correctness(results)
    if not todo:
        print("no new correctness buckets")
        return 0
    regression = set(fuzz.parse_seeds(args.regression)) \
        if args.regression else set()
    context = {"commit": args.commit, "run_url": args.run_url}
    issues = json.loads(gh(args, "issue", "list", "--state", "open",
                           "--limit", "1000", "--json",
                           "number,title").stdout)
    open_by_title = {i["title"]: i["number"] for i in issues}
    label = gh(args, "label", "create", LABEL, "--color", "d93f0b",
               "--description", "Found by the nightly stanc3 fuzzer",
               check=False)
    have_label = label.returncode == 0 or "already exists" in label.stderr
    with tempfile.TemporaryDirectory() as tmp:
        body_file = pathlib.Path(tmp) / "body.md"
        for bucket, seeds in todo.items():
            if bucket in open_by_title:
                fresh = [s for s in seeds if s not in regression]
                if not fresh:
                    print(f"{bucket}: #{open_by_title[bucket]} is open; "
                          f"only regression seeds this run")
                    continue
                body_file.write_text(comment_body(bucket, fresh, context))
                gh(args, "issue", "comment", str(open_by_title[bucket]),
                   "--body-file", str(body_file))
                print(f"{bucket}: commented on #{open_by_title[bucket]}")
                continue
            body_file.write_text(issue_body(args.run, results, bucket, seeds,
                                            context))
            argv = ["issue", "create", "--title", bucket,
                    "--body-file", str(body_file)]
            if have_label:
                argv += ["--label", LABEL]
            r = gh(args, *argv)
            print(f"{bucket}: opened {r.stdout.strip()}")
    return 0


# ---------------------------------------------------------------------------


def cmd_seeds(args):
    if args.override:
        if not re.fullmatch(r"[0-9:,]+", args.override):
            raise SystemExit(f"bad seed list {args.override!r}")
        fuzz.parse_seeds(args.override)
        print(args.override)
    else:
        print(night_seeds(datetime.date.fromisoformat(args.date)))
    return 0


def cmd_gate(args):
    reasons = problems(load(args.run))
    for r in reasons:
        print(r)
    return 1 if reasons else 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("seeds")
    s.add_argument("--date", default=datetime.date.today().isoformat())
    s.add_argument("--override", default="",
                   help="a fuzz.py --seeds list to use instead")
    for name in ("summary", "reduce", "file", "gate"):
        s = sub.add_parser(name)
        s.add_argument("run", type=pathlib.Path)
        if name == "reduce":
            s.add_argument("--compiler", type=pathlib.Path)
            s.add_argument("--check", type=pathlib.Path)
            s.add_argument("--bench", type=pathlib.Path)
            s.add_argument("--jobs", type=int, default=4)
            s.add_argument("--time-limit", type=float, default=600.0,
                           help="seconds per bucket")
            s.add_argument("--max-buckets", type=int, default=3)
        if name == "file":
            s.add_argument("--repo", required=True)
            s.add_argument("--gh", default="gh")
            s.add_argument("--regression", default=REGRESSION,
                           help="seeds that do not count as new for an "
                                "open issue")
            s.add_argument("--commit", default="")
            s.add_argument("--run-url", default="")
    args = ap.parse_args(argv)
    return {"seeds": cmd_seeds, "summary": cmd_summary,
            "reduce": cmd_reduce, "file": cmd_file,
            "gate": cmd_gate}[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())
