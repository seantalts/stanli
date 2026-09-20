"""Exercise workflow routing and the actual required-gate shell (needs PyYAML)."""
import ast
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

    def visit(node):
        if isinstance(node, ast.Constant):
            return node.value
        if isinstance(node, ast.Name) and node.id in ("true", "false"):
            return node.id == "true"
        if isinstance(node, ast.BoolOp):
            values = (visit(item) for item in node.values)
            return all(values) if isinstance(node.op, ast.And) else any(values)
        if isinstance(node, ast.Compare) and len(node.ops) == 1:
            left, right = visit(node.left), visit(node.comparators[0])
            if isinstance(node.ops[0], ast.Eq):
                return left == right
            if isinstance(node.ops[0], ast.NotEq):
                return left != right
        if isinstance(node, ast.Call) and isinstance(node.func, ast.Name):
            if node.func.id == "always" and not node.args and not node.keywords:
                return True
            if node.func.id == "startsWith" and len(node.args) == 2:
                return visit(node.args[0]).startswith(visit(node.args[1]))
        raise AssertionError("Unsupported CI guard: " + ast.dump(node))

    return visit(ast.parse(value, mode="eval").body)


def enabled(item, event, heavy="true", ref="refs/heads/main"):
    return expression(item.get("if", "true"), {
        "github.event_name": event,
        "github.ref": ref,
        "needs.changes.outputs.heavy": heavy,
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
            "no-stdio", "stanc-windows", "windows-compiler", "windows",
            "wasm", "wasm-webr", "r-platform-tests")]
        items.append(workflow("r.yml")["jobs"]["check"])
        items += [self.step("build", name) for name in (
            "Fetch the main-branch vectorization baseline",
            "MIR vectorization A/B", "Publish MIR vectorization measurements",
            "Measure portable MIR size and preparation cost",
            "Install reference BridgeStan",
            "Restore BridgeStan sources and shared build objects",
            "Restore compiled BridgeStan conformance models",
            "Advance restored model timestamps past their sources",
            "Conformance against reference BridgeStan")]
        items += [self.step("r-tests", name) for name in (
            "Fetch the previous release and the legacy anchor",
            "Both cross-release compiler and runtime directions",
            "Measure fresh-session time to first posterior")]
        items += [step for job in ("build", "r-tests")
                  for step in self.jobs[job]["steps"]
                  if step.get("with", {}).get("name") in (
                      "portable-mir-measurements", "r-first-posterior-linux")]
        for item in items:
            for event in ("pull_request", "push", "schedule", "workflow_dispatch"):
                with self.subTest(item=item.get("name", item.get("uses")), event=event):
                    self.assertEqual(enabled(item, event), event != "pull_request")
            self.assertTrue(enabled(item, "push", ref="refs/tags/v1.0.0"))

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
            "browser-compiler": ("Native and JavaScript compilers emit identical bytes",),
        }.items():
            for name in names:
                with self.subTest(job=job, step=name):
                    self.assertTrue(enabled(self.step(job, name), "pull_request"))
        self.assertNotIn("pull_request", workflow("rethinking.yml")["on"])
        self.assertEqual(workflow("rethinking.yml")["on"]["push"]["branches"], ["main"])

    def test_changed_files_select_the_correct_validation_path(self):
        cases = [
            ([], "false"),
            (["AGENTS.md", "README.md", "docs/design.md"], "false"),
            (["notes/performance/results.json", "notes/probe.patch"], "false"),
            (["docs/superpowers/plans/plan.md", "web/index.html"], "false"),
            (["runtime/src/executor.cpp"], "true"),
            (["compiler/ocaml/portable_mir.ml"], "true"),
            (["r/R/fit.R"], "true"),
            (["tests/fixtures/example.stan"], "true"),
            (["docs/corpus-refs.json.gz"], "true"),
            ([".github/workflows/wheels.yml"], "true"),
            (["README.md", "unknown/new-file"], "true"),
        ]
        script = self.jobs["changes"]["steps"][1]["run"]
        for paths, expected in cases:
            with self.subTest(paths=paths), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                (root / "git").write_text(
                    '#!/usr/bin/env bash\ncase "$1" in\n'
                    'fetch) ;;\ndiff) printf "%s" "$TEST_CHANGED_PATHS" ;;\n'
                    '*) exit 90 ;;\nesac\n')
                (root / "git").chmod(0o755)
                output = root / "output"
                env = dict(os.environ, PATH=tmp + os.pathsep + os.environ["PATH"],
                           GITHUB_EVENT_NAME="pull_request", BASE_SHA="fixture-base",
                           RUNNER_TEMP=tmp, GITHUB_OUTPUT=str(output),
                           TEST_CHANGED_PATHS="".join(path + "\n" for path in paths))
                result = subprocess.run([self.bash, "-e", "-o", "pipefail", "-c", script],
                                        env=env, cwd=tmp, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(output.read_text().strip(), "heavy=" + expected)

    def gate(self, heavy, results):
        job = self.jobs["pr-gate"]
        # Every dependency belongs to this required status, including R and
        # producer parity; a failed prerequisite must still schedule the gate.
        self.assertEqual(set(job["needs"]), set(results))
        self.assertIn("always()", job["if"])
        step = job["steps"][0]
        context = {"needs.changes.outputs.heavy": heavy}
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
                        "browser-compiler": "success" if heavy == "true" else "skipped",
                        "r-tests": "success" if heavy == "true" else "skipped"}
            self.assertEqual(self.gate(heavy, expected), 0)
            for job in expected:
                for result in ("success", "failure", "cancelled", "skipped", ""):
                    if result == expected[job]:
                        continue
                    with self.subTest(heavy=heavy, job=job, result=result):
                        self.assertNotEqual(self.gate(heavy, dict(expected, **{job: result})), 0)
            for invalid_scope in ("", "unknown"):
                self.assertNotEqual(self.gate(invalid_scope, expected), 0)


if __name__ == "__main__":
    unittest.main()
