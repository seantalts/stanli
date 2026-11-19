#!/usr/bin/env python3
"""Tests for tools/fuzz_stanc/nightly.py on a fake run directory, with a
fake gh on PATH. Nothing here runs a compiler or uses the network."""
from __future__ import annotations

import contextlib
import datetime
import io
import json
import os
import pathlib
import sys
import tempfile
import time
import unittest

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools" / "fuzz_stanc"))

import gen  # noqa: E402
import nightly  # noqa: E402

FAKE_GH = """\
#!/usr/bin/env bash
# Log each call, with the body file's text, and answer like gh would.
{
  printf 'CALL'; printf ' %q' "$@"; printf '\\n'
  prev=
  for a in "$@"; do
    if [ "$prev" = --body-file ]; then printf 'BODY\\n'; cat "$a"; printf 'END\\n'; fi
    prev=$a
  done
} >> "$GH_LOG"
case "$1 $2" in
  "issue list") cat "$GH_ISSUES" ;;
  "issue view") cat "$GH_COMMENTS" ;;
  "label create") [ -n "$GH_LABEL_FAILS" ] && { echo 'HTTP 403' >&2; exit 1; }; true ;;
  "issue create") echo "https://github.com/o/r/issues/99" ;;
  "issue comment") echo "https://github.com/o/r/issues/7#issuecomment-1" ;;
esac
"""


def program(seed, outcome="agree", buckets=(), known=None, diffs=None):
    r = {"seed": seed, "outcome": outcome, "buckets": list(buckets),
         "features": []}
    if known:
        r["known"] = known
    if diffs is not None:
        r["diffs"] = diffs
        r["bisect"] = [{"passes": "function_inlining", "differs": True}]
    return r


def results(programs, seeds="0:10"):
    return {"seeds": seeds, "config": gen.config(), "seconds": 1.0,
            "programs": programs}


NOW = "2026-12-20T03:40:00Z"
OLD = "2026-12-01T03:40:00Z"
RECENT = "2026-12-16T03:40:00Z"
LP = [{"kind": "lp", "point": 0, "none": -1.5, "o1": -2.5}]


def standard_run():
    agree = [program(s) for s in range(20, 40)]
    return results(agree + [
        program(3, "differ", ["lcm:lp"], diffs=LP),
        program(1_000_005, "differ", ["lcm:lp"], diffs=LP),
        program(4, "differ", ["function_inlining:output"],
                known={"function_inlining:output": "tracked"},
                diffs=[{"kind": "output"}]),
        program(5, "differ", ["o1:slow_gradient", "o1:mir_growth"]),
        program(6, "differ", ["copy_propagation:write_array"],
                diffs=[{"kind": "write_array"}]),
    ])


# The fake gh is a bash script and run_limited uses process groups; the
# nightly job runs on Linux only.
POSIX_ONLY = unittest.skipIf(os.name == "nt", "needs a POSIX shell")


@POSIX_ONLY
class Fixture(unittest.TestCase):
    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp())
        self.run_dir = self.tmp / "run"
        self.run_dir.mkdir()
        bin_dir = self.tmp / "bin"
        bin_dir.mkdir()
        self.gh = bin_dir / "gh"
        self.gh.write_text(FAKE_GH)
        self.gh.chmod(0o755)
        self.log = self.tmp / "gh.log"
        self.issues = self.tmp / "issues.json"
        self.issues.write_text("[]")
        self.comments = self.tmp / "comments.json"
        self.comments.write_text('{"comments": []}')
        self.env = {"GH_LOG": str(self.log), "GH_ISSUES": str(self.issues),
                    "GH_COMMENTS": str(self.comments)}
        self.old_env = {k: os.environ.get(k) for k in
                        ("GH_LOG", "GH_ISSUES", "GH_COMMENTS",
                         "GH_LABEL_FAILS")}
        os.environ.update(self.env)
        os.environ.pop("GH_LABEL_FAILS", None)

    def tearDown(self):
        for k, v in self.old_env.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v

    def write(self, res):
        (self.run_dir / "results.json").write_text(json.dumps(res))

    def main(self, *argv):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = nightly.main(list(argv))
        return code, out.getvalue()

    def file(self, *extra):
        return self.main("file", str(self.run_dir), "--repo", "o/r",
                         "--gh", str(self.gh), "--commit", "abc123",
                         "--run-url", "https://example.invalid/run/1",
                         "--now", NOW, *extra)

    def calls(self):
        text = self.log.read_text() if self.log.exists() else ""
        return [c for c in text.splitlines() if c.startswith("CALL")]

    def bodies(self):
        text = self.log.read_text()
        return [b.split("\nEND\n")[0] for b in text.split("BODY\n")[1:]]


class FileTest(Fixture):
    def test_one_issue_per_new_correctness_bucket(self):
        self.write(standard_run())
        code, _ = self.file()
        self.assertEqual(code, 0)
        creates = [c for c in self.calls() if "issue create" in c]
        self.assertEqual(len(creates), 2)
        self.assertIn("--title copy_propagation:write_array", creates[0])
        self.assertIn("--title lcm:lp", creates[1])
        for c in creates:
            self.assertIn("--label fuzzer", c)
            self.assertIn("--repo o/r", c)
        joined = "\n".join(self.calls())
        self.assertNotIn("function_inlining:output", joined, "known")
        self.assertNotIn("o1:", joined, "performance buckets are not filed")

    def test_body_has_seed_repro_diffs_and_program(self):
        self.write(standard_run())
        reduced = self.run_dir / "reduced" / "lcm__lp"
        reduced.mkdir(parents=True)
        (reduced / "reduced.stan").write_text("model { target += 1; }\n")
        (reduced / "data.json").write_text("{}\n")
        self.file()
        body = self.bodies()[1]
        self.assertIn("- Seed: 3", body)
        self.assertIn("seeds 3, 1000005", body)
        self.assertIn("--seeds 3,3 --jobs 1", body)
        self.assertIn("reduce.py --seed 3 --bucket lcm:lp", body)
        self.assertIn('"o1": -2.5', body)
        self.assertIn("## Reduced program", body)
        self.assertIn("model { target += 1; }", body)
        self.assertIn(gen.generate(3, gen.config()).source().strip(), body)
        self.assertIn("abc123", body)
        self.assertIn("https://example.invalid/run/1", body)
        self.assertIn("did not finish", self.bodies()[0],
                      "copy_propagation has no reduced program")

    def test_open_issue_gets_a_comment_with_the_new_seeds(self):
        self.write(standard_run())
        self.issues.write_text(json.dumps(
            [{"number": 7, "title": "lcm:lp", "createdAt": OLD},
             {"number": 8, "title": "lcm:lp extra words",
              "createdAt": OLD}]))
        self.file()
        calls = self.calls()
        self.assertEqual(sum("issue create" in c for c in calls), 1)
        comments = [c for c in calls if "issue comment" in c]
        self.assertEqual(len(comments), 1)
        self.assertIn("issue comment 7", comments[0])
        comment = [b for b in self.bodies() if "more programs" in b][0]
        self.assertIn("seeds 1000005", comment)
        self.assertNotIn(" 3,", comment, "regression seeds are not new")

    def test_only_regression_seeds_leave_an_open_issue_alone(self):
        res = standard_run()
        res["programs"] = [r for r in res["programs"]
                           if r["seed"] != 1_000_005]
        self.write(res)
        self.issues.write_text(json.dumps(
            [{"number": 7, "title": "lcm:lp", "createdAt": OLD}]))
        self.file()
        self.assertFalse([c for c in self.calls() if "issue comment" in c])

    def open_lcm(self, created, comments):
        self.issues.write_text(json.dumps(
            [{"number": 7, "title": "lcm:lp", "createdAt": created}]))
        self.comments.write_text(json.dumps({"comments": comments}))

    def comment_calls(self):
        return [c for c in self.calls() if "issue comment" in c]

    def test_at_most_one_comment_a_week(self):
        self.write(standard_run())
        self.open_lcm(OLD, [{"body": "The nightly fuzzer found 3 more",
                             "createdAt": RECENT}])
        self.file()
        self.assertEqual(self.comment_calls(), [])
        self.log.unlink()
        self.open_lcm(RECENT, [])
        self.file()
        self.assertEqual(self.comment_calls(), [], "opened this week")
        self.log.unlink()
        self.open_lcm(OLD, [{"body": "I am looking at this",
                             "createdAt": RECENT},
                            {"body": "The nightly fuzzer found 3 more",
                             "createdAt": OLD}])
        self.file()
        self.assertEqual(len(self.comment_calls()), 1,
                         "a person's comment does not count")

    def test_label_failure_still_opens_the_issue(self):
        os.environ["GH_LABEL_FAILS"] = "1"
        self.write(standard_run())
        self.file()
        creates = [c for c in self.calls() if "issue create" in c]
        self.assertEqual(len(creates), 2)
        self.assertNotIn("--label", creates[0])

    def test_nothing_new_calls_nothing(self):
        res = standard_run()
        res["programs"] = [r for r in res["programs"]
                           if r["seed"] not in (3, 6, 1_000_005)]
        self.write(res)
        code, out = self.file()
        self.assertEqual(code, 0)
        self.assertEqual(self.calls(), [])
        self.assertIn("no new correctness buckets", out)

    def test_huge_differences_stay_under_the_body_limit(self):
        res = standard_run()
        big = [{"kind": "lp", "none": "x" * 100_000, "o1": "y"}] * 5
        for r in res["programs"]:
            if r["seed"] == 3:
                r["diffs"] = big
                r["bisect"] = big
        self.write(res)
        self.file()
        for body in self.bodies():
            self.assertLess(len(body), 65_536)


class GateTest(Fixture):
    def filed(self):
        return json.loads((self.run_dir / "filed.json").read_text())

    def test_only_newly_opened_issues_fail_after_filing(self):
        self.write(standard_run())
        self.issues.write_text(json.dumps(
            [{"number": 7, "title": "lcm:lp", "createdAt": RECENT}]))
        self.file()
        self.assertEqual(self.filed()["opened"],
                         ["copy_propagation:write_array"])
        self.assertEqual(self.filed()["already_open"], ["lcm:lp"])
        code, out = self.main("gate", str(self.run_dir))
        self.assertEqual(code, 1)
        self.assertIn("copy_propagation:write_array", out)
        self.assertNotIn("lcm:lp", out)

    def test_every_bucket_already_open_passes(self):
        self.write(standard_run())
        self.issues.write_text(json.dumps(
            [{"number": 7, "title": "lcm:lp", "createdAt": OLD},
             {"number": 9, "title": "copy_propagation:write_array",
              "createdAt": RECENT}]))
        self.file()
        self.assertEqual(self.filed()["opened"], [])
        self.assertEqual(self.filed()["commented"], ["lcm:lp"])
        self.assertEqual(self.main("gate", str(self.run_dir))[0], 0)

    def test_harness_errors_fail_after_filing(self):
        res = standard_run()
        res["programs"].append(program(11, "harness_error"))
        self.write(res)
        self.issues.write_text(json.dumps(
            [{"number": 7, "title": "lcm:lp", "createdAt": OLD},
             {"number": 9, "title": "copy_propagation:write_array",
              "createdAt": OLD}]))
        self.file()
        code, out = self.main("gate", str(self.run_dir))
        self.assertEqual(code, 1)
        self.assertEqual(out.strip(), "1 harness errors")

    def test_new_correctness_bucket_fails(self):
        self.write(standard_run())
        code, out = self.main("gate", str(self.run_dir))
        self.assertEqual(code, 1)
        self.assertIn("new bucket lcm:lp (2 programs)", out)
        self.assertNotIn("o1:", out)

    def test_known_and_performance_buckets_pass(self):
        res = standard_run()
        res["programs"] = [r for r in res["programs"]
                           if r["seed"] not in (3, 6, 1_000_005)]
        self.write(res)
        self.assertEqual(self.main("gate", str(self.run_dir))[0], 0)

    def test_harness_errors_and_a_broken_toolchain_fail(self):
        self.write(results([program(s) for s in range(10)] +
                           [program(10, "harness_error")]))
        code, out = self.main("gate", str(self.run_dir))
        self.assertEqual(code, 1)
        self.assertIn("1 harness errors", out)
        self.write(results([program(s, "reject") for s in range(10)]))
        code, out = self.main("gate", str(self.run_dir))
        self.assertEqual(code, 1)
        self.assertIn("only 0 of 10 programs agree", out)


class SummaryTest(Fixture):
    def test_sections(self):
        self.write(standard_run())
        (self.run_dir / "wall_seconds").write_text("120\n")
        _, out = self.main("summary", str(self.run_dir))
        new = out.split("## New correctness buckets")[1].split("##")[0]
        known = out.split("## Known correctness buckets")[1].split("##")[0]
        perf = out.split("## Performance buckets")[1].split("**Result")[0]
        self.assertIn("`lcm:lp` | 2 |", new)
        self.assertIn("function_inlining:output", known)
        self.assertNotIn("o1:", new + known)
        self.assertIn("o1:slow_gradient", perf)
        self.assertIn("o1:mir_growth", perf)
        self.assertIn("in 2.0 min", out)
        self.assertIn("needs a look", out)


class SeedsTest(unittest.TestCase):
    def test_nights_do_not_overlap(self):
        a = nightly.night_seeds(datetime.date(2026, 11, 19))
        b = nightly.night_seeds(datetime.date(2026, 11, 20))
        sa, sb = (set(nightly.fuzz.parse_seeds(x)) for x in (a, b))
        regression = set(nightly.fuzz.parse_seeds(nightly.REGRESSION))
        self.assertEqual(sa & sb, regression)
        self.assertEqual(len(sa), len(regression) + nightly.BUDGET)
        self.assertGreaterEqual(min(sa - regression), 1_000_000)
        self.assertGreaterEqual(nightly.STRIDE, nightly.BUDGET)

    def test_override_is_validated(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            nightly.main(["seeds", "--override", "5:9,12"])
        self.assertEqual(out.getvalue().strip(), "5:9,12")
        for bad in ("5:9; rm -rf /", "9:5", "a"):
            with self.subTest(bad=bad), self.assertRaises(
                    (SystemExit, ValueError)):
                nightly.main(["seeds", "--override", bad])


@POSIX_ONLY
class ReduceLimitTest(unittest.TestCase):
    def test_kills_the_process_group_at_the_limit(self):
        started = time.monotonic()
        ok = nightly.run_limited(
            ["sh", "-c", "sleep 30 & sleep 30; wait"], 0.5)
        self.assertFalse(ok)
        self.assertLess(time.monotonic() - started, 5)
        self.assertTrue(nightly.run_limited(["true"], 5))


if __name__ == "__main__":
    unittest.main()
