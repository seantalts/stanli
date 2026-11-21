"""Exercise workflow routing and the actual required-gate shell (needs PyYAML)."""
import ast
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

import yaml


ROOT = Path(__file__).resolve().parents[1]


def workflow(name):
    # BaseLoader keeps the YAML 1.2 event key "on" from becoming a boolean.
    return yaml.load((ROOT / ".github/workflows" / name).read_text(),
                     Loader=yaml.BaseLoader)


def expression(value, context):
    """Evaluate the small boolean/comparison subset used by these CI guards."""
    value = value.strip()
    if value.startswith("${{"):
        value = value[3:-2].strip()
    value = re.sub(r"\b(?:github|needs|matrix|steps)\.[\w.-]+",
                   lambda match: repr(context[match[0]]), value)
    value = value.replace("&&", " and ").replace("||", " or ")
    value = re.sub(r"!(?!=)", " not ", value).strip()

    def visit(node):
        if isinstance(node, ast.Constant):
            return node.value
        if isinstance(node, ast.Name) and node.id in ("true", "false"):
            return node.id == "true"
        if isinstance(node, ast.UnaryOp) and isinstance(node.op, ast.Not):
            return not visit(node.operand)
        if isinstance(node, ast.BoolOp):
            # Like GitHub, && and || return an operand, not a boolean.
            for item in node.values[:-1]:
                value = visit(item)
                if bool(value) != isinstance(node.op, ast.And):
                    return value
            return visit(node.values[-1])
        if isinstance(node, ast.Compare) and len(node.ops) == 1:
            left, right = visit(node.left), visit(node.comparators[0])
            if isinstance(node.ops[0], ast.Eq):
                return left == right
            if isinstance(node.ops[0], ast.NotEq):
                return left != right
        if isinstance(node, ast.Call) and isinstance(node.func, ast.Name):
            if node.func.id == "always" and not node.args and not node.keywords:
                return True
            if node.func.id == "cancelled" and not node.args and not node.keywords:
                return False
            if node.func.id == "startsWith" and len(node.args) == 2:
                return visit(node.args[0]).startswith(visit(node.args[1]))
        raise AssertionError("Unsupported CI guard: " + ast.dump(node))

    return visit(ast.parse(value, mode="eval").body)


def enabled(item, event, heavy="true", ref="refs/heads/main"):
    return expression(item.get("if", "true"), {
        "github.event_name": event,
        "github.ref": ref,
        "needs.changes.outputs.heavy": heavy,
        "needs.changes.outputs.artifact_run": "",
        "matrix.runtime": "linux-x86_64",
        "matrix.plat": "manylinux_2_28_x86_64",
        "steps.bs-models.outputs.cache-hit": "true",
    })


class CiPolicyTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.jobs = workflow("wheels.yml")["jobs"]
        cls.bash = shutil.which("bash")
        if not cls.bash:
            raise RuntimeError("Bash is required to exercise the CI shell")

    def step(self, job, name):
        return next(step for step in self.jobs[job]["steps"]
                    if step.get("name") == name)

    def test_extra_configurations_are_post_submit(self):
        items = [self.jobs[name] for name in (
            "no-stdio", "wasm", "wasm-webr", "r-platform-tests")]
        items.append(workflow("r.yml")["jobs"]["check"])
        items += [self.step("build", name) for name in (
            "Measure portable MIR size and preparation cost",
            "Install reference BridgeStan",
            "Restore BridgeStan sources and shared build objects",
            "Restore compiled BridgeStan conformance models",
            "Advance restored model timestamps past their sources",
            "Conformance against reference BridgeStan")]
        items += [self.step("r-tests", name) for name in (
            "Measure fresh-session time to first posterior",)]
        items += [step for job in ("build", "r-tests")
                  for step in self.jobs[job]["steps"]
                  if step.get("with", {}).get("name") in (
                      "portable-mir-measurements", "r-first-posterior-linux")]
        for item in items:
            for event in ("pull_request", "push", "schedule", "workflow_dispatch"):
                with self.subTest(item=item.get("name", item.get("uses")), event=event):
                    self.assertEqual(enabled(item, event), event != "pull_request")
            self.assertTrue(enabled(item, "push", ref="refs/tags/v1.0.0"))

    def test_nutpie_is_installed_and_required_only_after_merge(self):
        install = next(step for step in self.jobs["r-tests"]["steps"]
                       if "setup-r-dependencies" in step.get("uses", ""))
        packages = install["with"]["extra-packages"]
        required = self.step("r-tests", "Build and check")["env"]["STANLI_REQUIRE_NUTPIER"]
        for event in ("pull_request", "push", "schedule", "workflow_dispatch"):
            with self.subTest(event=event):
                after_merge = event != "pull_request"
                listed = re.sub(r"\$\{\{(.*?)\}\}",
                                lambda m: expression(m[1], {"github.event_name": event}),
                                packages)
                self.assertEqual("any::nutpieR" in listed.split(), after_merge)
                self.assertEqual(expression(required, {"github.event_name": event}),
                                 "1" if after_merge else "")

    def test_windows_wheel_and_prerequisites_follow_source_scope(self):
        for name in ("stanc-windows", "windows-compiler", "windows"):
            job = self.jobs[name]
            self.assertIn("changes", job["needs"])
            for heavy in ("true", "false"):
                with self.subTest(job=name, heavy=heavy):
                    self.assertEqual(enabled(job, "pull_request", heavy),
                                     heavy == "true")
                    for event in ("push", "schedule", "workflow_dispatch"):
                        self.assertTrue(enabled(job, event, heavy))
                    self.assertTrue(enabled(job, "push", heavy,
                                            ref="refs/tags/v1.0.0"))

    def test_windows_wheel_keeps_compiler_and_release_dependencies(self):
        # The required wheel consumes fresh compiler artifacts and waits for
        # native/JavaScript parity. Release publishers must keep that chain.
        for dependency in ("stanc-windows", "windows-compiler"):
            self.assertIn(dependency, self.jobs["windows"]["needs"])
        for dependency in ("stanc-windows", "browser-compiler"):
            self.assertIn(dependency, self.jobs["windows-compiler"]["needs"])
        for publisher in ("publish", "runtime-release", "dev-runtime-release"):
            self.assertIn("windows", self.jobs[publisher]["needs"])

    def test_core_correctness_and_install_checks_still_run_on_prs(self):
        for job in ("build", "browser-compiler"):
            self.assertTrue(enabled(self.jobs[job], "pull_request"))
            self.assertFalse(enabled(self.jobs[job], "pull_request", heavy="false"))
        for job, names in {
            "build": ("Tests", "Corpus verification against CmdStan references",
                      "Wheel", "Install the wheel and run the Python tests",
                      "Typed producer matches exact-source legacy MIR"),
            "r-tests": ("Build and check",
                        "Bundled portable compiler through V8 and the current runtime",
                        "R ecosystem acceptance (no skips)"),
            "browser-compiler": ("Native and JavaScript compilers emit identical bytes",
                                 "R compiler helper in webR"),
            "windows": ("Configure, build, test", "Build the wheel",
                        "Install the wheel and run the Python tests"),
        }.items():
            for name in names:
                with self.subTest(job=job, step=name):
                    self.assertTrue(enabled(self.step(job, name), "pull_request"))
        self.assertNotIn("pull_request", workflow("rethinking.yml")["on"])
        self.assertEqual(workflow("rethinking.yml")["on"]["push"]["branches"], ["main"])
        self.assertNotIn("pull_request", workflow("dev-setup-windows.yml")["on"])

    def scope(self, paths, main_diff=(), gh_fails=False,
              jobs="build (manylinux_2_28_x86_64)|portable browser compiler",
              artifacts="runtime-linux-x86_64 r-oracle-cli-linux-x86_64 browser-compilers"):
        """Run the classifier with git and gh replaced by fixtures.

        The fixture main run 101 at main-sha differs from the PR by main_diff.
        """
        script = self.jobs["changes"]["steps"][1]["run"]
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "git").write_text(
                '#!/usr/bin/env bash\ncase "$1" in\n'
                'fetch) ;;\n'
                'diff) if [ "$4" = fixture-base ]; then printf "%s" "$TEST_CHANGED_PATHS";'
                ' else printf "%s" "$TEST_MAIN_DIFF"; fi ;;\n'
                '*) exit 90 ;;\nesac\n')
            # gh prints what its --jq filter would select.
            (root / "gh").write_text(
                '#!/usr/bin/env bash\n[ -z "$TEST_GH_FAILS" ] || exit 1\ncase "$2" in\n'
                '*/workflows/wheels.yml/runs*) echo "101 main-sha" ;;\n'
                '*/runs/101/jobs*) echo "$TEST_JOBS" ;;\n'
                '*/runs/101/artifacts*) echo "$TEST_ARTIFACTS" ;;\n'
                '*) exit 91 ;;\nesac\n')
            for tool in ("git", "gh"):
                (root / tool).chmod(0o755)
            output = root / "output"
            env = dict(os.environ, PATH=tmp + os.pathsep + os.environ["PATH"],
                       GITHUB_EVENT_NAME="pull_request", BASE_SHA="fixture-base",
                       GITHUB_REPOSITORY="owner/repo",
                       RUNNER_TEMP=tmp, GITHUB_OUTPUT=str(output),
                       TEST_CHANGED_PATHS="".join(path + "\n" for path in paths),
                       TEST_MAIN_DIFF="".join(path + "\n" for path in main_diff),
                       TEST_GH_FAILS="1" if gh_fails else "",
                       TEST_JOBS=jobs, TEST_ARTIFACTS=artifacts)
            result = subprocess.run([self.bash, "-e", "-o", "pipefail", "-c", script],
                                    env=env, cwd=tmp, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            return dict(line.split("=", 1) for line in output.read_text().split())

    def test_changed_files_select_the_correct_validation_path(self):
        cases = [
            ([], "false"),
            (["AGENTS.md", "README.md", "docs/design.md"], "false"),
            (["notes/performance/results.json", "notes/probe.patch"], "false"),
            (["docs/internal/archive/plans/plan.md", "web/index.html"], "false"),
            (["runtime/src/executor.cpp"], "true"),
            (["compiler/ocaml/portable_mir.ml"], "true"),
            (["r/inst/js/stanc.js"], "true"),
            (["r/R/fit.R", "tests/fixtures/example.stan"], "true"),
            (["tests/fixtures/example.stan"], "true"),
            (["docs/internal/artifacts/corpus-refs.json.gz"], "true"),
            ([".github/workflows/wheels.yml"], "true"),
            (["README.md", "unknown/new-file"], "true"),
        ]
        for paths, expected in cases:
            with self.subTest(paths=paths):
                self.assertEqual(self.scope(paths, main_diff=paths),
                                 {"heavy": expected, "artifact_run": ""})

    def test_r_only_changes_reuse_a_matching_main_run(self):
        reuse = {"heavy": "false", "artifact_run": "101"}
        full = {"heavy": "true", "artifact_run": ""}
        for paths in (["r/R/fit.R"], ["r/R/fit.R", "README.md", "r/tests/t.R"]):
            with self.subTest(paths=paths):
                self.assertEqual(self.scope(paths, main_diff=paths + ["docs/x.md"]),
                                 reuse)
        for name, kwargs in {
            "main differs outside r/": {"main_diff": ["r/R/fit.R", "runtime/src/a.cpp"]},
            "main differs in the bundled compiler": {"main_diff": ["r/inst/js/stanc.js"]},
            "lookup fails": {"gh_fails": True},
            "main build failed": {"jobs": "portable browser compiler"},
            "main compiler failed": {"jobs": "build (manylinux_2_28_x86_64)"},
            "artifact expired": {"artifacts": "runtime-linux-x86_64 browser-compilers"},
        }.items():
            with self.subTest(name):
                kwargs.setdefault("main_diff", ["r/R/fit.R"])
                self.assertEqual(self.scope(["r/R/fit.R"], **kwargs), full)

    def test_r_tests_run_on_main_artifacts_or_a_fresh_build(self):
        guard = self.jobs["r-tests"]["if"]
        for artifact_run, build, expected in (
                ("101", "skipped", True), ("", "skipped", False),
                ("", "success", True), ("", "failure", False)):
            with self.subTest(artifact_run=artifact_run, build=build):
                self.assertEqual(expression(guard, {
                    "needs.changes.outputs.artifact_run": artifact_run,
                    "needs.build.result": build,
                    "needs.browser-compiler.result": build}), expected)

    def gate(self, heavy, results, artifact_run=""):
        job = self.jobs["pr-gate"]
        # Every dependency belongs to this required status, including Windows,
        # R, and producer parity; a failed prerequisite still schedules it.
        self.assertEqual(set(job["needs"]), set(results))
        self.assertIn("always()", job["if"])
        step = job["steps"][0]
        context = {"needs.changes.outputs.heavy": heavy,
                   "needs.changes.outputs.artifact_run": artifact_run}
        context.update({"needs." + key + ".result": value
                        for key, value in results.items()})
        env = dict(os.environ)
        env.update({name: expression(value, context) for name, value in step["env"].items()})
        return subprocess.run([self.bash, "-e", "-o", "pipefail", "-c", step["run"]],
                              env=env, capture_output=True).returncode

    def test_required_gate_fails_closed(self):
        for heavy in ("true", "false"):
            expected = {"changes": "success", "static_checks": "success",
                        "build": "success" if heavy == "true" else "skipped",
                        "windows": "success" if heavy == "true" else "skipped",
                        "browser-compiler": "success" if heavy == "true" else "skipped",
                        "r-tests": "success" if heavy == "true" else "skipped",
                        "stanc-windows": "success" if heavy == "true" else "skipped",
                        "windows-compiler": "success" if heavy == "true" else "skipped"}
            self.assertEqual(self.gate(heavy, expected), 0)
            for job in expected:
                for result in ("success", "failure", "cancelled", "skipped", ""):
                    if result == expected[job]:
                        continue
                    with self.subTest(heavy=heavy, job=job, result=result):
                        self.assertNotEqual(self.gate(heavy, dict(expected, **{job: result})), 0)
            for invalid_scope in ("", "unknown"):
                self.assertNotEqual(self.gate(invalid_scope, expected), 0)

    def test_required_gate_for_r_only_needs_the_r_tests(self):
        expected = {"changes": "success", "static_checks": "success",
                    "build": "skipped", "windows": "skipped",
                    "browser-compiler": "skipped", "r-tests": "success",
                    "stanc-windows": "skipped", "windows-compiler": "skipped"}
        self.assertEqual(self.gate("false", expected, "101"), 0)
        self.assertNotEqual(self.gate("true", expected, "101"), 0)
        for job in expected:
            for result in ("success", "failure", "cancelled", "skipped", ""):
                if result == expected[job]:
                    continue
                with self.subTest(job=job, result=result):
                    self.assertNotEqual(
                        self.gate("false", dict(expected, **{job: result}), "101"), 0)


class NetworkRetryTest(unittest.TestCase):
    retry = ROOT / "tools/with_retry.sh"

    def run_retry(self, fail_times, attempts="3"):
        with tempfile.TemporaryDirectory() as tmp:
            counter = Path(tmp) / "count"
            counter.write_text("0")
            script = Path(tmp) / "flaky.sh"
            script.write_text(
                'n=$(( $(cat "$1") + 1 )); echo $n > "$1"\n'
                '[ "$n" -gt "$2" ]\n')
            result = subprocess.run(
                ["bash", str(self.retry), "bash", str(script), str(counter),
                 str(fail_times)],
                env=dict(os.environ, RETRY_ATTEMPTS=attempts, RETRY_DELAY="0"),
                capture_output=True, text=True)
            return result.returncode, int(counter.read_text())

    def test_retries_until_the_command_succeeds(self):
        self.assertEqual(self.run_retry(2), (0, 3))

    def test_gives_up_after_the_last_attempt_with_its_status(self):
        self.assertEqual(self.run_retry(5), (1, 3))

    def test_does_not_rerun_a_command_that_passed(self):
        self.assertEqual(self.run_retry(0), (0, 1))

    def dockerfile_run(self):
        text = (ROOT / ".github/manylinux/Dockerfile").read_text()
        match = re.search(r"^RUN (.*?)(?<!\\)\n", text, re.S | re.M)
        return match[1].replace("\\\n", " ")

    def test_dnf_install_in_the_image_is_retried(self):
        with tempfile.TemporaryDirectory() as tmp:
            fake = Path(tmp) / "dnf"
            fake.write_text(
                '#!/usr/bin/env bash\necho "dnf $*" >> "$DNF_LOG"\n'
                'if [ "$1" = install ]; then\n'
                '  n=$(( $(cat "$DNF_COUNT" 2>/dev/null || echo 0) + 1 ))\n'
                '  echo $n > "$DNF_COUNT"\n'
                '  [ "$n" -gt "$DNF_FAILURES" ]\n'
                'fi\n')
            fake.chmod(0o755)
            for failures, expected in ((2, 0), (99, 1)):
                log, count = Path(tmp) / "log", Path(tmp) / "count"
                log.write_text("")
                count.write_text("0")
                with self.subTest(failures=failures):
                    script = self.dockerfile_run().replace(
                        "sleep $((attempt * 15))", "true").replace(
                        "rm -rf /var/cache/dnf", "true")
                    result = subprocess.run(
                        ["bash", "-c", script],
                        env=dict(os.environ, PATH=tmp + os.pathsep + os.environ["PATH"],
                                 DNF_LOG=str(log), DNF_COUNT=str(count),
                                 DNF_FAILURES=str(failures)),
                        capture_output=True, text=True)
                    self.assertEqual(result.returncode, expected, result.stderr)
                    self.assertIn("install -y epel-release", log.read_text())

    def test_every_git_network_call_is_retried(self):
        files = [ROOT / "deps/fetch.sh", ROOT / "tools/build_web.sh"] + sorted(
            (ROOT / ".github/workflows").glob("*.yml"))
        network = re.compile(r"git (clone|(-C \S+ )?fetch)\b")
        for path in files:
            for number, line in enumerate(path.read_text().splitlines(), 1):
                code = line.split("#")[0]
                if "--no-tags --depth=1 origin" in code:
                    continue
                if network.search(code):
                    with self.subTest(file=path.name, line=number):
                        self.assertIn("with_retry.sh", code)


class RunawayJobTest(unittest.TestCase):
    workflows = sorted((ROOT / ".github/workflows").glob("*.yml"))

    def test_every_job_has_a_timeout(self):
        for path in self.workflows:
            for name, job in workflow(path.name)["jobs"].items():
                if "uses" in job:
                    continue
                with self.subTest(workflow=path.name, job=name):
                    self.assertIn("timeout-minutes", job)

    def test_every_ctest_run_has_a_per_test_timeout(self):
        calls = 0
        for path in self.workflows:
            text = path.read_text().replace("\\\n", " ")
            for line in text.splitlines():
                match = re.search(r"(?:^|[\s;&|])ctest\s.*", line.split("#")[0])
                if match and "--test-dir" in match[0]:
                    calls += 1
                    with self.subTest(workflow=path.name, line=line.strip()):
                        self.assertRegex(match[0], r"--timeout \d+")
        self.assertGreaterEqual(calls, 5)


class SetupRetryTest(unittest.TestCase):
    def test_msys2_setup_runs_a_second_time_after_a_failure(self):
        uses = 0
        for path in sorted((ROOT / ".github/workflows").glob("*.yml")):
            for name, job in workflow(path.name)["jobs"].items():
                steps = job.get("steps", [])
                for index, step in enumerate(steps):
                    if not step.get("uses", "").startswith("msys2/setup-msys2@"):
                        continue
                    uses += 1
                    if step.get("id", "").endswith("-retry"):
                        continue
                    with self.subTest(workflow=path.name, job=name):
                        retry = steps[index + 1]
                        self.assertEqual(step["continue-on-error"], "true")
                        self.assertEqual(retry["uses"], step["uses"])
                        self.assertEqual(retry["with"], step["with"])
                        self.assertEqual(
                            retry["if"], "steps.%s.outcome == 'failure'" % step["id"])
        self.assertGreaterEqual(uses, 4)


class NpmPublishTest(unittest.TestCase):
    def publish(self, view_output, view_status):
        step = next(item for item in workflow("wheels.yml")["jobs"]["npm-publish"]["steps"]
                    if item.get("name") == "Publish")
        version = json.loads((ROOT / "js/package.json").read_text())["version"]
        with tempfile.TemporaryDirectory() as tmp:
            log = Path(tmp) / "log"
            fake = Path(tmp) / "npm"
            fake.write_text(
                '#!/usr/bin/env bash\necho "npm $*" >> "$NPM_LOG"\n'
                'if [ "$1" = view ]; then\n'
                '  printf "%s" "$NPM_VIEW"; exit "$NPM_VIEW_STATUS"\n'
                'fi\n')
            fake.chmod(0o755)
            result = subprocess.run(
                ["bash", "-e", "-o", "pipefail", "-c", step["run"]],
                cwd=ROOT, capture_output=True, text=True,
                env=dict(os.environ, PATH=tmp + os.pathsep + os.environ["PATH"],
                         NPM_LOG=str(log), NPM_VIEW_STATUS=str(view_status),
                         NPM_VIEW=view_output.replace("VERSION", version)))
            return result.returncode, log.read_text() if log.exists() else ""

    def test_rerun_of_a_published_version_skips_publish(self):
        status, log = self.publish("VERSION", 0)
        self.assertEqual(status, 0)
        self.assertIn("npm view", log)
        self.assertNotIn("npm publish", log)

    def test_unpublished_version_is_published(self):
        status, log = self.publish("", 1)
        self.assertEqual(status, 0)
        self.assertIn("npm publish", log)


class DevRuntimeReleaseTest(unittest.TestCase):
    sha = "0123456789abcdef0123456789abcdef01234567"

    @classmethod
    def setUpClass(cls):
        cls.jobs = workflow("wheels.yml")["jobs"]
        cls.job = cls.jobs["dev-runtime-release"]

    def test_runs_only_on_main_after_merge(self):
        for event in ("pull_request", "push", "schedule", "workflow_dispatch"):
            with self.subTest(event=event):
                self.assertEqual(enabled(self.job, event), event != "pull_request")
        self.assertFalse(enabled(self.job, "push", ref="refs/tags/v1.0.0"))
        self.assertFalse(enabled(self.job, "push", ref="refs/heads/other"))

    def test_needs_jobs_that_run_on_a_push_to_main(self):
        # A skipped need would skip the release too.
        needs = self.job["needs"]
        self.assertNotIn("stanr", needs)
        for name in ("static_checks", "build", "windows", "wasm-webr",
                     "r-tests", "r-platform-tests"):
            with self.subTest(need=name):
                self.assertIn(name, needs)
                if name != "r-tests":
                    for event in ("push", "schedule", "workflow_dispatch"):
                        self.assertTrue(enabled(self.jobs[name], event))

    def run_step(self, name, view="", view_status=1, listing="[]", tarballs=6):
        step = next(item for item in self.job["steps"] if item.get("name") == name)
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            (tmp / "runtime").mkdir()
            for i in range(tarballs):
                (tmp / "runtime" / f"stanli-runtime-{i}.tar.gz").write_text("")
            bin_dir = tmp / "bin"
            bin_dir.mkdir()
            log = tmp / "log"
            fake = bin_dir / "gh"
            fake.write_text(
                '#!/usr/bin/env bash\necho "gh $*" >> "$GH_LOG"\n'
                'case "$2" in\n'
                '  view) printf "%s" "$GH_VIEW"; exit "$GH_VIEW_STATUS" ;;\n'
                '  list) printf "%s" "$GH_LIST" ;;\n'
                'esac\n')
            fake.chmod(0o755)
            result = subprocess.run(
                ["bash", "-e", "-o", "pipefail", "-c", step["run"]],
                cwd=tmp, capture_output=True, text=True,
                env=dict(os.environ,
                         PATH=str(bin_dir) + os.pathsep + os.environ["PATH"],
                         GITHUB_SHA=self.sha, GH_LOG=str(log), GH_VIEW=view,
                         GH_VIEW_STATUS=str(view_status), GH_LIST=listing))
            return result.returncode, log.read_text() if log.exists() else ""

    def publish(self, **kwargs):
        return self.run_step("Publish the prerelease", **kwargs)

    def test_a_new_commit_gets_a_draft_prerelease_that_is_then_published(self):
        status, log = self.publish()
        self.assertEqual(status, 0)
        lines = log.splitlines()
        create = next(line for line in lines if line.startswith("gh release create"))
        self.assertIn("dev-0123456789ab", create)
        for flag in ("--draft", "--prerelease", "--latest=false",
                     "--target " + self.sha):
            self.assertIn(flag, create)
        upload = lines.index(next(l for l in lines if l.startswith("gh release upload")))
        edit = lines.index(next(l for l in lines if l.startswith("gh release edit")))
        self.assertLess(lines.index(create), upload)
        self.assertLess(upload, edit)
        self.assertIn("--draft=false", lines[edit])
        self.assertIn("--latest=false", lines[edit])

    def test_a_draft_left_by_a_failed_run_is_finished(self):
        status, log = self.publish(view="true", view_status=0)
        self.assertEqual(status, 0)
        self.assertNotIn("gh release create", log)
        self.assertIn("gh release upload", log)
        self.assertIn("--draft=false", log)

    def test_a_rerun_after_publishing_uploads_nothing(self):
        status, log = self.publish(view="false", view_status=0)
        self.assertEqual(status, 0)
        self.assertNotIn("gh release upload", log)
        self.assertNotIn("gh release edit", log)

    def test_a_missing_platform_publishes_nothing(self):
        status, log = self.publish(tarballs=5)
        self.assertNotEqual(status, 0)
        self.assertNotIn("gh release create", log)

    def test_prune_keeps_the_newest_ten_dev_releases(self):
        releases = [{"tagName": "dev-%012x" % i,
                     "publishedAt": "2026-11-%02dT00:00:00Z" % (i + 1)}
                    for i in range(13)]
        releases.append({"tagName": "v0.30.0",
                         "publishedAt": "2026-01-01T00:00:00Z"})
        step = next(item for item in self.job["steps"]
                    if item.get("name") == "Prune old dev releases")
        self.assertEqual(step.get("continue-on-error"), "true")
        status, log = self.run_step("Prune old dev releases",
                                    listing=json.dumps(releases))
        self.assertEqual(status, 0)
        deleted = sorted(re.findall(r"gh release delete (\S+)", log))
        self.assertEqual(deleted, ["dev-%012x" % i for i in range(3)])
        self.assertEqual(log.count("--cleanup-tag --yes"), 3)
        self.assertIn("--exclude-drafts", log)


class DecodeSpeedGateTest(unittest.TestCase):
    gate = ROOT / "tools/check_mir_decode_speed.sh"

    def run_gate(self, legacy, portable):
        line = ("bytes=1 repetitions=101 min_ns={min} q1_ns={q1} "
                "median_ns={median} p95_ns=9 max_ns=9 checksum=1")
        with tempfile.TemporaryDirectory() as tmp:
            bench = Path(tmp) / "bench"
            bench.write_text(
                "#!/usr/bin/env bash\n"
                '[ "$1" = --paired ] || exit 64\n'
                "cat <<'EOF'\n%s\n%s\nEOF\n"
                % (line.format(**legacy), line.format(**portable)))
            bench.chmod(0o755)
            return subprocess.run(
                ["bash", str(self.gate), str(bench), "legacy.mir", "portable.mir"],
                capture_output=True, text=True)

    def test_passes_on_the_flaked_run_whose_median_was_noisy(self):
        # Run 40324681901: 1.80x by median, 3.06x by minimum.
        result = self.run_gate(
            dict(min=3060, q1=3200, median=1800), dict(min=1000, q1=1400, median=1000))
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_fails_when_the_lower_quartile_is_under_2x(self):
        result = self.run_gate(
            dict(min=2000, q1=2100, median=3000), dict(min=1000, q1=1100, median=1000))
        self.assertEqual(result.returncode, 1)
        self.assertIn("not at least 2x faster", result.stderr)

    def test_boundary_of_exactly_2x_passes(self):
        result = self.run_gate(
            dict(min=2000, q1=2000, median=2000), dict(min=1000, q1=1000, median=1000))
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_rejects_unparseable_benchmark_output(self):
        result = self.run_gate(
            dict(min=1, q1="x", median=1), dict(min=1, q1=1, median=1))
        self.assertNotEqual(result.returncode, 0)


class MainStatusTest(unittest.TestCase):
    """The job that files an issue while main is red."""

    @classmethod
    def setUpClass(cls):
        cls.jobs = workflow("wheels.yml")["jobs"]
        cls.job = cls.jobs["main-status"]
        cls.step = cls.job["steps"][0]

    def test_runs_after_every_merge_to_main_and_nowhere_else(self):
        for event in ("push", "schedule", "workflow_dispatch"):
            with self.subTest(event=event):
                self.assertTrue(enabled(self.job, event))
        self.assertFalse(enabled(self.job, "pull_request"))
        self.assertFalse(enabled(self.job, "push", ref="refs/heads/other"))
        self.assertFalse(enabled(self.job, "push", ref="refs/tags/v1.0.0"))

    def test_waits_for_the_post_merge_jobs_and_only_writes_issues(self):
        for name in ("changes", "static_checks", "no-stdio", "build", "windows",
                     "asan", "tsan", "wasm", "r-tests", "pages",
                     "dev-runtime-release"):
            with self.subTest(need=name):
                self.assertIn(name, self.job["needs"])
        self.assertEqual(self.job["permissions"], {"issues": "write"})
        self.assertTrue(all("uses" not in step for step in self.job["steps"]))

    def run_script(self, results, listing="[]"):
        if not shutil.which("jq"):
            self.skipTest("jq is required to run the job's script")
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            bin_dir = tmp / "bin"
            bin_dir.mkdir()
            log = tmp / "log"
            fake = bin_dir / "gh"
            fake.write_text(
                '#!/usr/bin/env bash\necho "gh $*" >> "$GH_LOG"\n'
                '[ "$2" = list ] && printf "%s" "$GH_LIST"\nexit 0\n')
            fake.chmod(0o755)
            needs = {name: {"result": result} for name, result in results.items()}
            subprocess.run(
                ["bash", "-e", "-o", "pipefail", "-c", self.step["run"]],
                cwd=tmp, capture_output=True, text=True, check=True,
                env=dict(os.environ,
                         PATH=str(bin_dir) + os.pathsep + os.environ["PATH"],
                         GH_LOG=str(log), GH_LIST=listing,
                         GITHUB_WORKFLOW="wheels", GITHUB_SHA="abc123",
                         RUN_URL="https://example.test/run/1",
                         RESULTS=json.dumps(needs)))
            return log.read_text() if log.exists() else ""

    TITLE = "main is red: wheels"
    OPEN = json.dumps([{"number": 7, "title": "main is red: wheels"},
                       {"number": 8, "title": "main is red: other"}])

    def test_a_failure_opens_one_issue(self):
        log = self.run_script({"asan": "failure", "build": "success"})
        self.assertIn("gh issue create --title " + self.TITLE, log)
        self.assertIn("Failed: asan.", log)

    def test_a_failure_while_the_issue_is_open_adds_a_comment(self):
        log = self.run_script({"asan": "failure", "tsan": "failure"},
                              listing=self.OPEN)
        self.assertIn("gh issue comment 7 ", log)
        self.assertIn("Failed: asan,tsan.", log)
        self.assertNotIn("issue create", log)

    def test_a_run_with_nothing_failed_closes_the_open_issue(self):
        log = self.run_script({"asan": "success", "wasm": "skipped"},
                              listing=self.OPEN)
        self.assertIn("gh issue close 7 ", log)
        self.assertNotIn("close 8", log)

    def test_a_green_run_with_no_open_issue_does_nothing(self):
        log = self.run_script({"asan": "success"})
        self.assertNotIn("issue close", log)
        self.assertNotIn("issue create", log)

    def test_a_cancelled_job_neither_opens_nor_closes(self):
        for listing in ("[]", self.OPEN):
            with self.subTest(listing=listing):
                log = self.run_script({"asan": "cancelled", "build": "success"},
                                      listing=listing)
                for verb in ("create", "comment", "close"):
                    self.assertNotIn("issue " + verb, log)


class MpmathRequiredTest(unittest.TestCase):
    def test_the_matrix_tests_install_and_require_mpmath(self):
        steps = workflow("wheels.yml")["jobs"]["build"]["steps"]
        names = [step.get("name") for step in steps]
        self.assertLess(names.index("Install mpmath"), names.index("Tests"))
        self.assertIn("mpmath", steps[names.index("Install mpmath")]["run"])
        self.assertEqual(steps[names.index("Tests")]["env"],
                         {"STANLI_REQUIRE_MPMATH": "1"})


if __name__ == "__main__":
    unittest.main()
